#!/usr/bin/env bash
# Rebuild the Rebellions kmd (rebellions.ko + rblnfs.ko) against a
# specific kernel's headers and drop the stripped results next to this
# script so the x86 guest can insmod them over the virtio-9p share.
#
# Usage:
#   ./guest/build-kmd.sh                  # builds for $(uname -r)
#   KVERSION=6.8.0-110-generic ./guest/build-kmd.sh
#
# The module's vermagic MUST match the kernel the guest boots, so
# prefer running this on (or targeting) the same distro build that the
# guest will use.  Ubuntu: `sudo apt install linux-headers-$KVERSION`.

set -euo pipefail

KVERSION="${KVERSION:-$(uname -r)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
SRC="$REPO/external/ssw-bundle/products/common/kmd/rebellions"

if [[ ! -d "/lib/modules/$KVERSION/build" ]]; then
    echo "ERROR: /lib/modules/$KVERSION/build is missing." >&2
    echo "       Install linux-headers-$KVERSION (or set KVERSION=...)." >&2
    exit 1
fi

if [[ ! -d "$SRC" ]]; then
    echo "ERROR: kmd source not found at $SRC" >&2
    echo "       Run: git submodule update --init --recursive" >&2
    exit 1
fi

echo "[build-kmd] building kmd for kernel $KVERSION"
(cd "$SRC" && make KVERSION="$KVERSION")

echo "[build-kmd] staging stripped modules under $HERE"
for mod in rebellions.ko rblnfs.ko; do
    cp "$SRC/$mod" "$HERE/$mod"
    strip --strip-debug "$HERE/$mod"
done

echo "[build-kmd] done:"
ls -lh "$HERE"/*.ko
echo
echo "vermagic check:"
for mod in "$HERE"/*.ko; do
    printf '  %-20s %s\n' "$(basename "$mod")" "$(modinfo -F vermagic "$mod")"
done
