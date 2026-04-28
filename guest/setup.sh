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
if [ -r /proc/interrupts ]; then
    cat /proc/interrupts > "$OUT/interrupts.txt"
fi

if ! grep -q 'FW_BOOT_DONE' "$DMESG_AFTER"; then
    log "FAIL: FW_BOOT_DONE never showed up in dmesg"
    echo "--- last 40 kmd-related dmesg lines ---"
    grep -E 'rebellions|rebel' "$DMESG_AFTER" | tail -40 || true
    exit 1
fi

log "PASS: dmesg observed FW_BOOT_DONE from kmd"
grep -E 'rebellions|rebel|FW_BOOT_DONE' "$DMESG_AFTER" | tail -30

# P10 smoke target — opt-in via kernel cmdline `remu.run_p10=1`. The
# `tests/p10_umd_smoke_test.py` orchestrator passes that flag through
# `./remucli run --guest-cmdline-extra`; a plain `./remucli run --host`
# stays in the M8b "boot to FW_BOOT_DONE and idle" shape so day-to-day
# interactive runs aren't held to the in-progress P10 handshake.
#
# When enabled, runs the umd `test_command_submission` integration test
# (built by guest/build-umd.sh, staged at $HERE/bin + $HERE/lib via the
# 9p share). It exercises the full silicon-accurate path:
#   rblnCreateContext → rblnCmdCopy(host→shm) → rblnCmdInvokeCs (RBDMA
#   simple_copy, OTO via cmdgen) → rblnCmdCopy(shm→host) → rblnSubmitJob
#   → rblnWaitJob → host buffer ↔ device buffer mem-compare
# i.e., kmd submit ioctl → q-cp `cb_parse_*` walk on CA73 CP0 → r100-rbdma
# OTO byte-mover → FNSH FIFO + GIC SPI 978 → q-cp `cb_complete` → BD.DONE
# writeback via P1b cfg-mirror trap → MSI-X via r100-imsix → kmd fence
# release → rblnWaitJob returns RBLN_RETURN_STATUS_COMPLETED.
#
# Currently blocked at `rebel_queue_init`'s init_done poll — the kmd's
# `readl_poll_timeout_atomic` busy-loops while q-cp on CP0 is parked in
# `r100-pcie-outbound`'s qemu_cond_wait_bql() waiting for the host's
# OP_READ_RESP that never arrives. See docs/roadmap.md → P10 for the
# open chardev / TCG-scheduling investigation.
P10_BIN="$HERE/bin/command_submission"
P10_LIB="$HERE/lib"
P10_REQUESTED=0
if [ -r /proc/cmdline ] && grep -qw 'remu.run_p10=1' /proc/cmdline; then
    P10_REQUESTED=1
fi

if [ "$P10_REQUESTED" -ne 1 ]; then
    log "P10 not requested (no remu.run_p10=1 on /proc/cmdline) — done."
    exit 0
fi

if [ ! -x "$P10_BIN" ] || [ ! -d "$P10_LIB" ]; then
    log "P10 binary not staged ($P10_BIN missing) — skipping smoke test."
    log "Run guest/build-umd.sh on the host to stage it, then re-run."
    exit 0
fi

log "P10 smoke: running command_submission with LD_LIBRARY_PATH=$P10_LIB"
P10_LOG="$OUT/p10_command_submission.log"

# `-y 5` selects RBLN_DEVICE_NAME_REBEL_QUAD (matches our 1eff:2030).
# The integration-test harness queries `/dev/rbln0` and skips with
# exit 0 if no device is present — we want the device case, where
# cmocka prints `[  PASSED  ] 1 test(s).` on success.
set +e
LD_LIBRARY_PATH="$P10_LIB" "$P10_BIN" -y 5 > "$P10_LOG" 2>&1
P10_RC=$?
set -e

cat "$P10_LOG"

# `[  PASSED  ]` is cmocka's success line. `[  SKIPPED ]` means the
# binary couldn't see a device (kmd not loaded, /dev/rbln0 missing,
# etc.) — that's a P10 fail for us even if cmocka returns 0 on
# skip.
if [ "$P10_RC" -eq 0 ] && grep -q '\[  PASSED  \] 1 test(s)' "$P10_LOG"; then
    log "P10 PASS: test_command_submission completed (rblnWaitJob -> COMPLETED, mem-compare ok)"
    exit 0
fi

log "P10 FAIL: rc=$P10_RC; log saved at $P10_LOG"
exit 2
