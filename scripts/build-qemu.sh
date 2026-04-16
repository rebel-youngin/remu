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
if [ ! -d "${QEMU_SRC}" ]; then
    echo "ERROR: QEMU source not found at ${QEMU_SRC}"
    echo "Run: git submodule add https://gitlab.com/qemu-project/qemu.git external/qemu"
    echo "     cd external/qemu && git checkout v9.2.0"
    exit 1
fi

# Clean build if requested
if [ "$1" = "clean" ]; then
    echo "Cleaning build directory..."
    rm -rf "${QEMU_BUILD}"
    rm -rf "${QEMU_SRC}/hw/arm/r100"
    echo "Done."
    exit 0
fi

# Symlink R100 device models into QEMU source tree
R100_QEMU_DIR="${QEMU_SRC}/hw/arm/r100"
if [ ! -L "${R100_QEMU_DIR}" ] && [ ! -d "${R100_QEMU_DIR}" ]; then
    echo "Symlinking R100 device models into QEMU source tree..."
    ln -sf "${REMU_MACHINE_SRC}" "${R100_QEMU_DIR}"
fi

# Ensure remu_addrmap.h is accessible
R100_INCLUDE_LINK="${QEMU_SRC}/include/hw/arm/remu_addrmap.h"
if [ ! -L "${R100_INCLUDE_LINK}" ]; then
    echo "Symlinking remu headers..."
    ln -sf "${REMU_INCLUDE_SRC}/remu_addrmap.h" "${R100_INCLUDE_LINK}"
fi

# Check if subdir('r100') is already in hw/arm/meson.build
if ! grep -q "subdir('r100')" "${QEMU_SRC}/hw/arm/meson.build" 2>/dev/null; then
    echo "NOTE: You need to add the following line to ${QEMU_SRC}/hw/arm/meson.build:"
    echo "  subdir('r100')"
    echo ""
    echo "Also update the r100_soc.h include path in source files."
fi

# Configure QEMU (aarch64 target only, minimal features)
mkdir -p "${QEMU_BUILD}"
if [ ! -f "${QEMU_BUILD}/build.ninja" ]; then
    echo "Configuring QEMU..."
    cd "${QEMU_SRC}"
    ./configure \
        --target-list=aarch64-softmmu \
        --prefix="${REMU_ROOT}/install" \
        --enable-debug \
        --disable-werror \
        --disable-docs \
        --disable-guest-agent \
        --extra-cflags="-I${REMU_INCLUDE_SRC}" \
        "${QEMU_BUILD}"
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
