#!/bin/bash
# REMU - Launch FW QEMU instance (Phase 1)
#
# Boots R100 firmware (BL1→BL31→FreeRTOS) on the emulated SoC.
#
# Usage:
#   ./scripts/run-fw.sh                    # Normal run
#   ./scripts/run-fw.sh --gdb              # Wait for GDB attach
#   ./scripts/run-fw.sh --trace            # Enable MMIO trace logging
#
# FW binaries must be placed in images/ directory:
#   images/bl1.bin      - TF-A BL1 (loaded at 0x1E00010000)
#   images/bl2.bin      - TF-A BL2 (loaded at BL2_BASE)
#   images/bl31_cp0.bin - TF-A BL31 for CP0 (loaded at 0x00000000)
#   images/bl31_cp1.bin - TF-A BL31 for CP1 (loaded at 0x14100000)
#   images/freertos_cp0.bin - FreeRTOS for CP0 (loaded at 0x00200000)
#   images/freertos_cp1.bin - FreeRTOS for CP1 (loaded at 0x14200000)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMU_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
QEMU="${REMU_ROOT}/build/qemu/qemu-system-aarch64"
IMAGES="${REMU_ROOT}/images"

if [ ! -x "${QEMU}" ]; then
    echo "ERROR: QEMU binary not found. Run ./scripts/build-qemu.sh first."
    exit 1
fi

# Parse arguments
GDB_OPTS=""
TRACE_OPTS=""
for arg in "$@"; do
    case "$arg" in
        --gdb)
            GDB_OPTS="-s -S"
            echo "Waiting for GDB connection on localhost:1234..."
            ;;
        --trace)
            TRACE_OPTS="-d guest_errors,unimp"
            ;;
    esac
done

# Build loader arguments for FW images
LOADER_ARGS=""
if [ -f "${IMAGES}/bl1.bin" ]; then
    LOADER_ARGS="${LOADER_ARGS} -device loader,file=${IMAGES}/bl1.bin,addr=0x1E00010000"
fi
if [ -f "${IMAGES}/bl31_cp0.bin" ]; then
    LOADER_ARGS="${LOADER_ARGS} -device loader,file=${IMAGES}/bl31_cp0.bin,addr=0x00000000"
fi
if [ -f "${IMAGES}/freertos_cp0.bin" ]; then
    LOADER_ARGS="${LOADER_ARGS} -device loader,file=${IMAGES}/freertos_cp0.bin,addr=0x00200000"
fi
if [ -f "${IMAGES}/bl31_cp1.bin" ]; then
    LOADER_ARGS="${LOADER_ARGS} -device loader,file=${IMAGES}/bl31_cp1.bin,addr=0x14100000"
fi
if [ -f "${IMAGES}/freertos_cp1.bin" ]; then
    LOADER_ARGS="${LOADER_ARGS} -device loader,file=${IMAGES}/freertos_cp1.bin,addr=0x14200000"
fi

if [ -z "${LOADER_ARGS}" ]; then
    echo "WARNING: No FW images found in ${IMAGES}/"
    echo "Place bl1.bin, bl31_cp0.bin, freertos_cp0.bin, etc. in the images/ directory."
    echo "Running with empty memory (will likely hang at reset vector)."
fi

echo "Starting R100 emulator..."
echo "  Machine: r100-soc (4 chiplets, 32 CA73 cores)"
echo "  UART: serial stdio"
echo ""

exec "${QEMU}" \
    -M r100-soc \
    -nographic \
    -serial stdio \
    ${GDB_OPTS} \
    ${TRACE_OPTS} \
    ${LOADER_ARGS}
