#!/usr/bin/env bash
# Build the x86 guest boot artifacts for `./remucli run --host`:
#
#   images/x86_guest/bzImage            — distro kernel (Ubuntu HWE)
#   images/x86_guest/initramfs.cpio.gz  — busybox + 9p modules + /init
#
# The idea: `apt download` (no sudo needed) grabs the linux-image and
# busybox-static .debs into a temp dir; `dpkg-deb -x` extracts them;
# we then cherry-pick bzImage + the 4 modules needed to mount our
# virtio-9p share (netfs, 9pnet, 9pnet_virtio, 9p) and stitch them
# into a minimal cpio that boots straight into /mnt/remu/setup.sh.
#
# Usage:
#   ./guest/build-guest-image.sh                  # for $(uname -r)
#   KVERSION=6.8.0-110-generic ./guest/build-guest-image.sh
#
# Requires: apt, dpkg-deb, cpio, gzip (all standard on Debian/Ubuntu).

set -euo pipefail

KVERSION="${KVERSION:-$(uname -r)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OUT_DIR="${OUT_DIR:-$REPO/images/x86_guest}"

mkdir -p "$OUT_DIR"

log() { printf '[build-guest] %s\n' "$*"; }

for tool in apt-get dpkg-deb cpio gzip; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: $tool not found in PATH" >&2
        exit 1
    }
done

STAGE="$(mktemp -d -t remu-guest-XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

log "stage dir: $STAGE"
log "target kernel: $KVERSION"

DEBS="$STAGE/debs"
FS="$STAGE/fs"
IRFS="$STAGE/initramfs"
mkdir -p "$DEBS" "$FS" "$IRFS"

log "downloading linux-{image,modules}-$KVERSION + busybox-static + glibc/gcc runtime debs (no sudo needed)"
# linux-image-*.deb ships only the bzImage; the .ko tree ships in
# linux-modules-*.deb (the `image` deb Depends: on it at install
# time). We need both because we have to cherry-pick 4 modules for
# the initramfs.
#
# libc6 + libgcc-s1 + libgomp1 are pulled in for P10's umd smoke test
# binary (`command_submission`, built by build-umd.sh). The initramfs
# only needs the dynamic loader + glibc + gcc runtime so the binary
# resolves at exec(); the rest of its deps (libcmocka / libbz2 / libz
# / liburing / libzstd) live on the 9p share under guest/lib and are
# resolved via LD_LIBRARY_PATH inside setup.sh. If P10 isn't being
# exercised these debs cost ~6 MB extra in the cpio — small enough
# that staging them unconditionally beats a config knob.
(cd "$DEBS" && apt-get download \
    "linux-image-unsigned-$KVERSION" \
    "linux-modules-$KVERSION" \
    "busybox-static" \
    "libc6" \
    "libgcc-s1" \
    "libgomp1" 2>/dev/null) || \
(cd "$DEBS" && apt-get download \
    "linux-image-$KVERSION" \
    "linux-modules-$KVERSION" \
    "busybox-static" \
    "libc6" \
    "libgcc-s1" \
    "libgomp1")

ls "$DEBS"/*.deb | while read -r d; do
    log "extract $(basename "$d")"
    dpkg-deb -x "$d" "$FS"
done

VMLINUZ="$FS/boot/vmlinuz-$KVERSION"
if [[ ! -f "$VMLINUZ" ]]; then
    VMLINUZ="$(ls "$FS"/boot/vmlinuz-* 2>/dev/null | head -1 || true)"
fi
if [[ ! -f "$VMLINUZ" ]]; then
    echo "ERROR: no vmlinuz found in extracted linux-image deb" >&2
    exit 1
fi

log "staging bzImage -> $OUT_DIR/bzImage"
cp "$VMLINUZ" "$OUT_DIR/bzImage"
chmod 0644 "$OUT_DIR/bzImage"

BUSYBOX="$FS/bin/busybox"
if [[ ! -x "$BUSYBOX" ]]; then
    echo "ERROR: busybox-static .deb did not drop /bin/busybox" >&2
    exit 1
fi

log "building initramfs tree under $IRFS"
mkdir -p "$IRFS"/{bin,sbin,proc,sys,dev,tmp,mnt/remu,etc,lib/modules/$KVERSION,lib64,lib/x86_64-linux-gnu,usr/lib/x86_64-linux-gnu,usr/lib64}

cp "$BUSYBOX" "$IRFS/bin/busybox"
chmod 0755 "$IRFS/bin/busybox"

# glibc + libgcc-s1 + libgomp1 — the runtime + loader for P10's umd
# smoke test binary. Two layout subtleties on Ubuntu jammy/noble:
#   - The dynamic loader lives at /usr/lib64/ld-linux-x86-64.so.2 in
#     the deb, but /lib64/ld-linux-x86-64.so.2 is the path baked into
#     `ldd`'s output and into every dynamic ELF's INTERP. We mirror
#     /usr/lib64 → /lib64 so the loader resolves either way.
#   - libc itself is in /usr/lib/x86_64-linux-gnu in the deb; we also
#     mirror that to /lib/x86_64-linux-gnu (the older "merged-/usr"
#     layout some tooling still expects).
# Falls back silently if any path is missing — not all Ubuntu releases
# ship every multi-arch combination.
log "staging glibc + libgcc-s1 + libgomp1 into initramfs"
for src_dir in "$FS/usr/lib64" "$FS/lib64"; do
    if [[ -d "$src_dir" ]]; then
        cp -a "$src_dir/." "$IRFS/lib64/" 2>/dev/null || true
    fi
done
for src_dir in "$FS/usr/lib/x86_64-linux-gnu" "$FS/lib/x86_64-linux-gnu"; do
    if [[ -d "$src_dir" ]]; then
        cp -a "$src_dir/." "$IRFS/lib/x86_64-linux-gnu/" 2>/dev/null || true
        cp -a "$src_dir/." "$IRFS/usr/lib/x86_64-linux-gnu/" 2>/dev/null || true
    fi
done
# `man ld.so` says the loader searches /lib + /lib64 + /usr/lib + the
# trusted dirs from /etc/ld.so.cache before LD_LIBRARY_PATH. Without an
# ld.so.cache (we don't run ldconfig in the initramfs) it falls back to
# the hard-coded list, which includes /lib/x86_64-linux-gnu via DT_RPATH
# on most distro libcs. Still, dropping a one-line ld.so.conf helps
# diagnostic builds where someone strace's the loader.
cat > "$IRFS/etc/ld.so.conf" <<'LDCONF'
/lib/x86_64-linux-gnu
/usr/lib/x86_64-linux-gnu
/lib64
LDCONF
if [[ ! -e "$IRFS/lib64/ld-linux-x86-64.so.2" ]]; then
    log "WARNING: no ld-linux-x86-64.so.2 staged — dynamic ELFs (umd binary) will NOT run"
fi

# Install busybox applets as symlinks. We query busybox itself for the
# canonical list so we don't hardcode anything host-specific.
log "installing busybox applets as symlinks"
"$BUSYBOX" --list | while read -r applet; do
    [[ -z "$applet" || "$applet" == "busybox" ]] && continue
    ln -sf busybox "$IRFS/bin/$applet"
done
for applet in $("$BUSYBOX" --list-full 2>/dev/null | grep '^sbin/' || true); do
    ln -sf ../bin/busybox "$IRFS/$applet"
done

# 9p module dependency chain: netfs, 9pnet, 9pnet_virtio, 9p.
# virtio + virtio_ring + virtio_pci are built into Ubuntu HWE kernels.
MOD_SRC="$FS/lib/modules/$KVERSION/kernel"
MOD_DST="$IRFS/lib/modules/$KVERSION"
for m in \
    fs/netfs/netfs.ko \
    net/9p/9pnet.ko \
    net/9p/9pnet_virtio.ko \
    fs/9p/9p.ko
do
    src="$MOD_SRC/$m"
    [[ -f "$src" ]] || {
        echo "ERROR: module missing in linux-image: $m" >&2
        echo "       (tried $src)" >&2
        exit 1
    }
    dst="$MOD_DST/$(basename "$m")"
    log "  + $m"
    cp "$src" "$dst"
done

# /init — mounts the pseudo-fs, loads 9p modules, mounts the share,
# runs setup.sh, then drops to a shell for further poking.
cat > "$IRFS/init" <<'INIT'
#!/bin/sh
export PATH=/bin:/sbin

/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null || \
    /bin/busybox mdev -s

echo "[init] kernel $(uname -r) up — loading 9p modules"
cd /lib/modules/"$(uname -r)" 2>/dev/null && \
    for ko in netfs.ko 9pnet.ko 9pnet_virtio.ko 9p.ko; do
        insmod "$ko" 2>&1 | sed 's/^/[init]   /' || true
    done
cd /

echo "[init] mounting 9p share 'remu' at /mnt/remu"
mkdir -p /mnt/remu
if mount -t 9p -o trans=virtio,version=9p2000.L remu /mnt/remu; then
    echo "[init] 9p mount OK"
    if [ -x /mnt/remu/setup.sh ] || [ -r /mnt/remu/setup.sh ]; then
        echo "[init] running /mnt/remu/setup.sh"
        sh /mnt/remu/setup.sh
        rc=$?
        echo "[init] setup.sh exit $rc"
    else
        echo "[init] no setup.sh in /mnt/remu (nothing to do)"
    fi
else
    echo "[init] 9p mount FAILED — share tag 'remu' not wired?"
fi

echo "[init] dropping to shell (Ctrl-A X in QEMU to quit)"
exec sh
INIT
chmod 0755 "$IRFS/init"

# Make sure /dev has a minimal set even if devtmpfs isn't auto-mounted.
# The devtmpfs mount above handles the common case; these are backups.
for node_spec in \
    "null c 1 3" "zero c 1 5" "tty c 5 0" \
    "console c 5 1" "random c 1 8" "urandom c 1 9"
do
    set -- $node_spec
    name=$1; type=$2; maj=$3; min=$4
    [[ -e "$IRFS/dev/$name" ]] && continue
    # Can't mknod without CAP_MKNOD; skip silently if not permitted.
    /bin/busybox mknod "$IRFS/dev/$name" "$type" "$maj" "$min" 2>/dev/null || true
done

log "cpio | gzip -> $OUT_DIR/initramfs.cpio.gz"
(cd "$IRFS" && find . -print0 | cpio --null --quiet -o -H newc) | \
    gzip -9 > "$OUT_DIR/initramfs.cpio.gz"

log "done — artifacts under $OUT_DIR:"
ls -lh "$OUT_DIR"
