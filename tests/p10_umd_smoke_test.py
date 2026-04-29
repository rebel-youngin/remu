#!/usr/bin/env python3
"""P10 — umd smoke test on the silicon-accurate path.

End-to-end shape: boot `./remucli run --host --name p10-umd`, let the x86
guest's `setup.sh` insmod `rebellions.ko`, observe `FW_BOOT_DONE`, then
launch the umd integration-test binary `command_submission` (built by
`guest/build-umd.sh`) which exercises:

  rblnCreateContext
    → rblnCreateCommandBuffer
    → rblnCmdCopy(host→shm, in_dma)         (HDMA LL chain on chiplet 0)
    → rblnCmdInvokeCs(simple_copy)          (RBDMA OTO via cmdgen_rebel)
    → rblnCmdCopy(shm→host, out_dma)        (HDMA LL chain again)
    → rblnSubmitJob → rblnWaitJob
    → host buffer ↔ device buffer mem-compare

This is the first test that drives every silicon-side component end-to-end
in one round trip:

  kmd  rebel_ioctl_invoke_cs    (RBLN_IOCTL_INVOKE_CS via /dev/rbln0)
   ↓  via PCIe BAR4 INTGR1[qid] doorbell
  r100-cm7  forward as SPI 185 (queue-doorbell on chiplet-0 GIC)
   ↓
  q-cp/CP0  hq_task → cb_task → cb_parse_*
   ↓  walks BD via P1a r100-pcie-outbound (chiplet-0 4 GB AXI window
      tunneled over `hdma` chardev → host pci_dma_read)
   ↓  programs HDMA channel registers (P5 r100-hdma LL walker —
      OP_WRITE / OP_READ_REQ for host-leg LLIs)
   ↓  programs RBDMA descriptor (P4A reg block + P4B OTO byte mover —
      address_space_read → buf → address_space_write on chiplet 0)
   ↓  pushes DNC tasks via mtq (P3 four mailboxes) if the CB carries any
   ↓
  RBDMA FNSH FIFO + GIC SPI 978 → q-cp's rbdma_done_handler
  HDMA  GIC SPI 186 → q-cp's hdma_done_handler
   ↓
  q-cp/CP0  cb_complete
   ↓  writes BD.DONE + advances queue_desc.ci through P1b cfg-mirror
   ↓  pcie_msix_trigger → r100-imsix MMIO
   ↓
  r100-imsix → host msix.sock → r100-npu-pci → MSI-X to kmd
   ↓
  kmd fence → wakes rblnWaitJob → COMPLETED
   ↓
  user buffer == output buffer (memcmp green)

If anything in that chain regresses, the umd test is the canonical
reproducer.

The host-side harness does ~nothing tricky: boot the simulator, watch
`output/p10-umd/host/serial.log` for the `setup.sh` exit marker the guest
prints, and key off the `P10 PASS` / `P10 FAIL` line setup.sh emits.

Skipped-with-PASS cases:
  - guest/bin/command_submission missing (smoke not built yet) → setup.sh
    prints `P10 binary not staged ...` and exits 0. The harness reports
    SKIPPED (exit 77 — `automake`-style skip) so `./remucli test p10`
    doesn't pretend success when the binary isn't there.

Exits:
  0  PASS  — `command_submission` returned 0 and cmocka reported `[  PASSED  ] 1`
  1  infra failure (timeout, missing serial.log, simulator died, etc.)
  2  P10 FAIL line observed (kmd didn't expose device, ioctl failed,
     mem-compare mismatch, …)
  77 SKIPPED — guest/bin/command_submission missing; run guest/build-umd.sh
"""
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RUN_NAME = "p10-umd"
RUN_DIR = REPO / "output" / RUN_NAME

# Where the x86 guest's serial console (and hence /init + setup.sh stdout)
# lands. `--host` always wires this up.
HOST_SERIAL_LOG = RUN_DIR / "host" / "serial.log"

# Existence of the host monitor socket means QEMU got past
# `chardev_open()` for that path AND truncated the serial.log alongside
# (`-serial file:PATH` opens with O_TRUNC). Only after that point is it
# safe to scan serial.log content — otherwise a stale log from a prior
# run would short-circuit the harness with cached PASS/FAIL markers.
HOST_MON_SOCK = RUN_DIR / "host" / "monitor.sock"
NPU_MON_SOCK = RUN_DIR / "npu" / "monitor.sock"

# Total budget end-to-end. Cold-boot (FW + kernel + insmod) takes ~30 s on
# our build host; the integration test's two iterations of the full
# round-trip add maybe 5 s. 120 s gives generous headroom on slower hosts
# and on the first run after a long idle (page-fault warmups).
DEADLINE = 180.0

# setup.sh exit markers we care about — see guest/setup.sh:
#   "P10 PASS" — full silicon-accurate round trip green
#   "P10 FAIL" — binary launched but something failed (rc != 0, or
#                cmocka reported SKIPPED, etc.)
#   "P10 binary not staged" — guest/bin/command_submission absent
PASS_RE = re.compile(r"P10 PASS:")
FAIL_RE = re.compile(r"P10 FAIL:")
SKIP_RE = re.compile(r"P10 binary not staged")
INIT_DONE_RE = re.compile(r"\[init\] setup\.sh exit (\d+)")


def boot_remu(log_path):
    # `remu.run_p10=1` is the opt-in marker `guest/setup.sh` greps
    # /proc/cmdline for. Without it `setup.sh` stops after FW_BOOT_DONE
    # and a plain `./remucli run --host` keeps the M8b interactive boot
    # shape regardless of whether `guest/bin/command_submission` is
    # staged from a prior `guest/build-umd.sh`.
    #
    # `--host-mem 4G` is required for `pci_p2pdma_add_resource` against
    # the 64 GB BAR0: the kernel allocates ~1 GB of `struct page`
    # vmemmap to back the BAR's worth of pages, which OOMs the default
    # 512 MB guest. The kmd handles `-ENOMEM` gracefully (just a
    # `dev_warn`), but the OOM-killer takes out `sh` while reclaiming,
    # killing setup.sh and the test along with it. With 4 GB the
    # allocation succeeds and probe continues past P2PDMA. See
    # `docs/debugging.md` → "P10 — cb[1] HDMA LL chain reads all zeros".
    return subprocess.Popen(
        [str(REPO / "remucli"), "run", "--host", "--name", RUN_NAME,
         "--host-mem", "4G",
         "--guest-cmdline-extra", "remu.run_p10=1"],
        stdout=open(log_path, "wb"),
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        start_new_session=True,
    )


def teardown(proc):
    try:
        os.killpg(os.getpgid(proc.pid), 15)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except ProcessLookupError:
            pass


def tail(path: Path, n: int = 80) -> str:
    if not path.exists():
        return "(%s does not exist)" % path
    try:
        text = path.read_text(errors="replace")
    except OSError as e:
        return "(read error on %s: %s)" % (path, e)
    return "\n".join(text.splitlines()[-n:])


def main():
    os.environ["PYTHONUNBUFFERED"] = "1"

    if not (REPO / "guest" / "bin" / "command_submission").exists():
        print("SKIP: guest/bin/command_submission missing — run "
              "guest/build-umd.sh on the host first.")
        return 77

    log_path = RUN_DIR.with_suffix(".log")
    log_path.parent.mkdir(parents=True, exist_ok=True)

    # Belt-and-suspenders against stale state: drop the prior serial.log
    # AND the monitor socket so a half-cleaned `output/p10-umd/host/`
    # never makes our wait loop latch onto last-run markers. ./remucli
    # run also clears process / shm / socket leakage via its
    # auto-`clean --name` step, but the per-name output dir is
    # deliberately retained across runs (post-mortem evidence) and
    # contains the file we're about to scan.
    for stale in (HOST_SERIAL_LOG, HOST_MON_SOCK, NPU_MON_SOCK):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    proc = boot_remu(log_path)
    deadline = time.time() + DEADLINE
    verdict = None
    init_rc = None
    last_seen_size = -1

    try:
        # Step 1: wait for QEMU to come up. The monitor socket appears
        # right after r100-soc instantiation (NPU side) / chardev_open
        # (host side); both predate the guest serial log being touched
        # so by the time both sockets are present the serial.log we
        # scan is guaranteed to be the new boot's.
        while time.time() < deadline:
            if proc.poll() is not None:
                print("FAIL: ./remucli run exited prematurely "
                      "with rc=%d before QEMU monitors came up"
                      % proc.returncode)
                print("--- last 80 lines of remucli log ---")
                print(tail(log_path))
                return 1
            if HOST_MON_SOCK.exists() and NPU_MON_SOCK.exists():
                break
            time.sleep(0.2)
        else:
            print("FAIL: QEMU monitor sockets never appeared "
                  "(both NPU + host) within %.0fs" % DEADLINE)
            print("--- last 80 lines of remucli log ---")
            print(tail(log_path))
            return 1

        # Step 2: poll the freshly-truncated serial log for setup.sh
        # markers.
        while time.time() < deadline:
            if proc.poll() is not None:
                print("FAIL: ./remucli run exited prematurely "
                      "with rc=%d during guest boot" % proc.returncode)
                break

            if not HOST_SERIAL_LOG.exists():
                time.sleep(0.5)
                continue

            try:
                size = HOST_SERIAL_LOG.stat().st_size
            except OSError:
                time.sleep(0.2)
                continue

            if size == last_seen_size:
                time.sleep(0.5)
                continue
            last_seen_size = size

            # Re-read incrementally on every change; the log is at most
            # a few hundred KB.
            text = HOST_SERIAL_LOG.read_text(errors="replace")

            if SKIP_RE.search(text):
                # setup.sh found no binary — this is an infra error in
                # the harness (we already gated on its presence above)
                # but be defensive.
                verdict = "skip"
                break

            if PASS_RE.search(text):
                verdict = "pass"
                break

            if FAIL_RE.search(text):
                verdict = "fail"
                break

            m = INIT_DONE_RE.search(text)
            if m and not (PASS_RE.search(text) or FAIL_RE.search(text)):
                # setup.sh ran to completion but we didn't see a P10
                # marker — treat as failure (e.g. setup.sh exited
                # before reaching the P10 block).
                init_rc = int(m.group(1))
                verdict = "fail"
                break

        if verdict is None:
            print("FAIL: timed out after %.0fs without seeing a P10 "
                  "verdict in %s" % (DEADLINE, HOST_SERIAL_LOG))
            print("--- last 80 lines of host serial ---")
            print(tail(HOST_SERIAL_LOG))
            return 1

        if verdict == "skip":
            print("SKIP: guest reports binary not staged "
                  "(harness gating slipped — should not happen)")
            return 77

        if verdict == "fail":
            extra = ("init rc=%d; " % init_rc) if init_rc is not None else ""
            print("FAIL: %sP10 marker reported failure in guest" % extra)
            print("--- last 80 lines of host serial ---")
            print(tail(HOST_SERIAL_LOG))
            return 2

        print("PASS: command_submission completed end-to-end "
              "(kmd → q-cp → r100-rbdma OTO → cb_complete → MSI-X)")
        return 0

    finally:
        teardown(proc)


if __name__ == "__main__":
    sys.exit(main())
