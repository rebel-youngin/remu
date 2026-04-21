#!/bin/bash
# REMU - Build QEMU with R100 device models
#
# Usage: ./scripts/build-qemu.sh [clean]
#
# Prerequisites:
#   - QEMU build dependencies (libglib2.0-dev, libpixman-1-dev, ninja-build, etc.)
#   - AArch64 cross-compiler (for target firmware, not for QEMU itself)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMU_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
QEMU_SRC="${REMU_ROOT}/external/qemu"
QEMU_BUILD="${REMU_ROOT}/build/qemu"
REMU_MACHINE_SRC="${REMU_ROOT}/src/machine"
REMU_INCLUDE_SRC="${REMU_ROOT}/src/include"

# Check QEMU source exists
if [ ! -d "${QEMU_SRC}" ] || [ ! -f "${QEMU_SRC}/configure" ]; then
    echo "ERROR: QEMU source not found at ${QEMU_SRC}"
    echo "Initialize the submodule first:"
    echo "  git submodule update --init external/qemu"
    exit 1
fi

# Clean build if requested
if [ "$1" = "clean" ]; then
    echo "Cleaning build directory..."
    rm -rf "${QEMU_BUILD}"
    rm -f "${QEMU_SRC}/hw/arm/r100"
    rm -f "${QEMU_SRC}/include/hw/arm/r100_soc.h"
    rm -f "${QEMU_SRC}/include/hw/arm/remu_addrmap.h"
    # Revert meson.build injection (subdir('r100') line) if present
    if grep -q "^subdir('r100')$" "${QEMU_SRC}/hw/arm/meson.build" 2>/dev/null; then
        sed -i "/^subdir('r100')$/d" "${QEMU_SRC}/hw/arm/meson.build"
    fi
    echo "Done."
    exit 0
fi

# Symlink R100 device models into QEMU source tree
R100_QEMU_DIR="${QEMU_SRC}/hw/arm/r100"
if [ ! -L "${R100_QEMU_DIR}" ] && [ ! -d "${R100_QEMU_DIR}" ]; then
    echo "Symlinking R100 device models into QEMU source tree..."
    ln -sf "${REMU_MACHINE_SRC}" "${R100_QEMU_DIR}"
fi

# Symlink R100 headers into QEMU's include/hw/arm/ so in-tree device files
# can #include "hw/arm/r100_soc.h" / "hw/arm/remu_addrmap.h" directly.
for hdr_src_path in "${REMU_MACHINE_SRC}/r100_soc.h" "${REMU_INCLUDE_SRC}/remu_addrmap.h"; do
    hdr_basename="$(basename "${hdr_src_path}")"
    hdr_link="${QEMU_SRC}/include/hw/arm/${hdr_basename}"
    if [ ! -L "${hdr_link}" ]; then
        echo "Symlinking ${hdr_basename} → include/hw/arm/..."
        ln -sf "${hdr_src_path}" "${hdr_link}"
    fi
done

# Inject subdir('r100') into hw/arm/meson.build so meson picks up the R100
# device models via the symlinked r100/ directory. Idempotent.
if ! grep -q "^subdir('r100')$" "${QEMU_SRC}/hw/arm/meson.build" 2>/dev/null; then
    echo "Injecting subdir('r100') into hw/arm/meson.build..."
    echo "subdir('r100')" >> "${QEMU_SRC}/hw/arm/meson.build"
fi

# Configure QEMU (aarch64 target only, minimal features)
# Out-of-tree build: invoke configure FROM the build dir so meson writes
# build.ninja at ${QEMU_BUILD} rather than the submodule's own working tree.
mkdir -p "${QEMU_BUILD}"
if [ ! -f "${QEMU_BUILD}/build.ninja" ]; then
    echo "Configuring QEMU..."
    cd "${QEMU_BUILD}"
    "${QEMU_SRC}/configure" \
        --target-list=aarch64-softmmu \
        --prefix="${REMU_ROOT}/install" \
        --enable-debug \
        --disable-werror \
        --disable-docs \
        --disable-guest-agent \
        --extra-cflags="-I${REMU_INCLUDE_SRC}"
fi

# Build
echo "Building QEMU..."
cd "${QEMU_BUILD}"
ninja -j$(nproc)

echo ""
echo "Build complete. Binary at:"
echo "  ${QEMU_BUILD}/qemu-system-aarch64"
echo ""
echo "Run with:"
echo "  ${QEMU_BUILD}/qemu-system-aarch64 -M r100-soc -nographic -serial stdio"
