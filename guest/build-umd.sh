#!/usr/bin/env bash
# Build the umd integration-test binary `command_submission-static` against
# the in-tree umd (external/ssw-bundle/products/common/umd) and stage it
# alongside its runtime shared libraries under `guest/` so the x86 guest
# can run it over the virtio-9p share. This is the P10 smoke target —
# `rblnCreateContext` → record `rblnCmdCopy` (host→shm→host) → submit →
# wait — i.e., the silicon-accurate path through kmd → q-cp → r100-rbdma
# OTO byte-mover → cb_complete → MSI-X → fence release.
#
# Usage:
#   ./guest/build-umd.sh                  # uses host gcc/cmake + libcmocka
#   CMAKE_BUILD_TYPE=Debug ./guest/build-umd.sh
#
# Submodules `external/unity` + `src/common/headers` are init'd on demand.
# Heavy umd sub-systems are off:
#   - RBLN_USE_RUST=OFF   (no Rust toolchain)
#   - RBLN_BUILD_CCL=OFF  (no NCCL-style collectives)
#   - RBLN_BUILD_ML=OFF   (no rbln-ml)
#   - RBLN_BUILD_TOOLS=OFF (no replayer / cdb / perf_tools)
#   - DRM_ON=OFF          (kmd exposes the legacy /dev/rbln%d char device)
# What survives is exactly: rbln-thunk + cmdgen + ssgen + rblnthunk-tests
# + the per-suite cmocka executables under src/tests/integration_tests/.
#
# Output:
#   guest/bin/command_submission   stripped binary (the smoke test)
#   guest/lib/*.so.*               its required shared libraries
#
# Both gitignored. setup.sh picks them up post-FW_BOOT_DONE when present.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
UMD_SRC="$REPO/external/ssw-bundle/products/common/umd"
BUILD_DIR="${BUILD_DIR:-$REPO/build/umd}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"

if [[ ! -d "$UMD_SRC" ]]; then
    echo "ERROR: umd source not found at $UMD_SRC" >&2
    echo "       Run: git submodule update --init --recursive" >&2
    exit 1
fi

for tool in cmake make gcc ldd; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: $tool not found in PATH" >&2
        exit 1
    }
done

# umd has two nested submodules of its own.
echo "[build-umd] init umd submodules (idempotent)"
(cd "$UMD_SRC" && git submodule update --init external/unity src/common/headers >/dev/null)

echo "[build-umd] cmake configure ($BUILD_DIR, type=$BUILD_TYPE)"
mkdir -p "$BUILD_DIR"
(cd "$BUILD_DIR" && cmake \
    -DRBLN_BUILD_ML=OFF \
    -DRBLN_BUILD_CCL=OFF \
    -DRBLN_BUILD_TOOLS=OFF \
    -DRBLN_USE_RUST=OFF \
    -DDRM_ON=OFF \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    "$UMD_SRC" >/dev/null)

# `command_submission-static` links the umd in statically so we don't need
# to drag librbln-thunk.so onto the 9p share alongside it.
echo "[build-umd] make command_submission-static (parallel: $(nproc))"
(cd "$BUILD_DIR" && make -j"$(nproc)" command_submission-static)

BIN_SRC="$BUILD_DIR/bin/command_submission-static"
if [[ ! -x "$BIN_SRC" ]]; then
    echo "ERROR: build did not produce $BIN_SRC" >&2
    exit 1
fi

mkdir -p "$HERE/bin" "$HERE/lib"
cp "$BIN_SRC" "$HERE/bin/command_submission"
strip --strip-debug "$HERE/bin/command_submission"

# Stage the runtime .so files the binary actually needs (resolved by the
# loader against the host's fresh build). These are bundled in `guest/lib`
# rather than the initramfs because they are guest-runtime concerns, not
# guest-bringup concerns: the initramfs only ships what's needed to mount
# the 9p share + insmod the kmd; everything userspace lives over 9p.
# libc / ld-linux / libgcc_s / libpthread / libgomp DO go into the
# initramfs (build-guest-image.sh) because the dynamic loader has to be
# resolvable before LD_LIBRARY_PATH even applies.
echo "[build-umd] stage shared libraries under $HERE/lib"
: > "$HERE/lib/.deps.txt"
ldd "$BIN_SRC" | awk '/=>/ && $3 ~ /^\// {print $3}' | sort -u | while read -r so; do
    base="$(basename "$so")"
    case "$base" in
        # Glibc + GCC runtime go in the initramfs (see build-guest-image.sh)
        # so the loader finds them before LD_LIBRARY_PATH is set up.
        libc.so.6|ld-linux-x86-64.so.2|libpthread.so.0|libdl.so.2|\
        libm.so.6|librt.so.1|libgcc_s.so.1|libgomp.so.1|libresolv.so.2)
            printf '  skip-initramfs %s -> %s\n' "$base" "$so"
            echo "$base initramfs $so" >> "$HERE/lib/.deps.txt"
            continue
            ;;
    esac
    printf '  + %s -> %s\n' "$base" "$so"
    cp -L "$so" "$HERE/lib/$base"
    echo "$base 9p $so" >> "$HERE/lib/.deps.txt"
done

echo
echo "[build-umd] done. Binary + libs staged under $HERE/{bin,lib}:"
ls -lh "$HERE/bin/command_submission"
ls -lh "$HERE/lib/" | tail -n +2
echo
echo "Next: ./guest/build-guest-image.sh (must include glibc + libgcc_s"
echo "      + libgomp in the initramfs so the dynamic loader is reachable),"
echo "      then ./remucli run --host or ./remucli test p10."
