#!/bin/sh
# Runs INSIDE the x86 guest once this directory is mounted at the
# tag-free location you picked (examples assume /mnt/remu).  Drives
# the Rebellions kmd insmod and prints / saves the evidence that the
# q-sys firmware's FW_BOOT_DONE signal made it out of the NPU QEMU,
# across the shared BAR4 MMIO mirror, and into the host driver.
#
# Usage inside the guest:
#   sh /mnt/remu/setup.sh

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/output"
mkdir -p "$OUT"

log() { printf '[setup] %s\n' "$*"; }

log "kernel: $(uname -r)"
log "mount point: $HERE"

if [ ! -r "$HERE/rebellions.ko" ]; then
    echo "ERROR: rebellions.ko missing from $HERE" >&2
    echo "       Run ./guest/build-kmd.sh on the host first." >&2
    exit 1
fi

log "PCI scan — expecting 1eff:2030 (CR03 quad, our r100-npu-pci)"
PCI_SYS=/sys/bus/pci/devices
FOUND_NPU=""
if [ -d "$PCI_SYS" ]; then
    : > "$OUT/lspci.txt"
    for dev in "$PCI_SYS"/*; do
        [ -r "$dev/vendor" ] || continue
        v=$(cat "$dev/vendor")
        d=$(cat "$dev/device")
        printf '%s %s:%s\n' "$(basename "$dev")" "$v" "$d" >> "$OUT/lspci.txt"
        if [ "$v" = "0x1eff" ] && [ "$d" = "0x2030" ]; then
            FOUND_NPU=$dev
        fi
    done
    if command -v lspci >/dev/null 2>&1; then
        lspci -nn > "$OUT/lspci.txt"
        lspci -vvv > "$OUT/lspci-vvv.txt" 2>&1 || true
    fi
    if [ -z "$FOUND_NPU" ]; then
        echo "ERROR: 1eff:2030 not visible to the guest." >&2
        echo "       Is r100-npu-pci attached to the x86 QEMU cmdline?" >&2
        cat "$OUT/lspci.txt" >&2
        exit 1
    fi
    log "found CR03 quad at $(basename "$FOUND_NPU")"
else
    log "(no /sys/bus/pci — skipping PCI sanity check)"
fi

DMESG_BEFORE="$OUT/dmesg.before.txt"
DMESG_AFTER="$OUT/dmesg.after.txt"
dmesg > "$DMESG_BEFORE" 2>/dev/null || true

log "insmod rebellions.ko"
insmod "$HERE/rebellions.ko" || {
    echo "ERROR: insmod rebellions.ko failed" >&2
    dmesg | tail -40
    exit 1
}

# rblnfs depends on rebellions — load it too, but don't block the
# verify path if it fails (FW_BOOT_DONE is the real signal we want).
if [ -r "$HERE/rblnfs.ko" ]; then
    insmod "$HERE/rblnfs.ko" || log "rblnfs.ko insmod failed (non-fatal)"
fi

log "waiting up to 10s for rebel_reset_done() to print 'FW_BOOT_DONE'"
i=0
while [ $i -lt 20 ]; do
    if dmesg | grep -q 'FW_BOOT_DONE'; then
        break
    fi
    sleep 0.5
    i=$((i + 1))
done

dmesg > "$DMESG_AFTER" 2>/dev/null || true
cp "$DMESG_AFTER" "$OUT/dmesg.log"
[ -r /proc/interrupts ] && cp /proc/interrupts "$OUT/interrupts.txt"

if grep -q 'FW_BOOT_DONE' "$DMESG_AFTER"; then
    log "PASS: dmesg observed FW_BOOT_DONE from kmd"
    grep -E 'rebellions|rebel|FW_BOOT_DONE' "$DMESG_AFTER" | tail -30
    exit 0
fi

log "FAIL: FW_BOOT_DONE never showed up in dmesg"
echo "--- last 40 kmd-related dmesg lines ---"
grep -E 'rebellions|rebel' "$DMESG_AFTER" | tail -40 || true
exit 1
