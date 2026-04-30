#!/usr/bin/env python3
"""P10 q-cp GDB probe — diagnostic snapshot of CA73 state at the cb[1] hang.

This is **not** a regression test (it carries no pass/fail assertion) — it
is the live-debug recipe for the open P10 issue documented in
`docs/debugging.md` → "P10 — cb[1] HDMA LL chain reads all zeros" and
`docs/roadmap.md` → P10. Run it manually when investigating the hang.
The companion regression test is `tests/p10_umd_smoke_test.py` (which
just times out today; this probe explains why).

What it does
============

Boots `./remucli run --host --name p10-debug` with `remu.run_p10=1`,
watches `output/p10-debug/doorbell.log` until the kmd has emitted
*two* `INTGR1=0x1` doorbells (= cb[0] + cb[1] CQ kicks), waits a
configurable settle time so q-cp's hq_task / cb_task is fully wedged
on cb[1], then starts QEMU's gdbstub on demand via the NPU monitor
(`gdbserver tcp::PORT`, `wait=off` so the VM keeps running). Attaches
the upstream `aarch64-none-elf-gdb` and dumps:

  - hq_mgr / cb_mgr globals (cb_run_cnt, cq[0].ci/pi, ready_list/wait_list
    head sentinels — the headline state for "did cb_task pull cb[1]
    off the ready list?")
  - hdma_mgr + per-channel state (hdma_queue[0/1], htask handle —
    answers "did hdma_task get notified?")
  - cfg-shadow region @ 0x10200000 (DDH + FUNC_SCRATCH)
  - HDMA RD ch0 register block @ 0x1D80380300/0x400 (LLP / CTRL1
    after the kick — confirms vacuous completion)
  - LL cursor neighborhood @ 0x42B86700..0x42B867BF (192 B window
    centred on the empty chain)
  - `info threads` + `bt 40` per CA73 thread (32 vCPUs across 4 chiplets)

Output
======

  output/p10-debug/qcp-bt-<TAG>.txt           — set-logging redirected:
                                                gdb commands + their output
  output/p10-debug/qcp-bt-<TAG>.gdb-stdout.txt — gdb stderr/stdout (symbol
                                                load warnings, etc)
  output/p10-debug/qcp-bt-<TAG>.gdb           — the generated gdb script

`<TAG>` defaults to `cb1`, override with `REMU_P10_DEBUG_TAG=<name>`.

Tunables
========

  REMU_P10_DEBUG_TAG     artifact tag (default: cb1)
  REMU_P10_DEBUG_DELAY_S sleep after observing 2 CQ doorbells before
                         snapshotting (default: 1.5)
  REMU_QCP_GDB           explicit path to aarch64-none-elf-gdb;
                         otherwise the script tries (in order):
                           - $PATH
                           - $COMPILER_PATH_ARM64/aarch64-none-elf-gdb
                           - the 13.3 toolchain default

Two interesting settle points to compare:

  ~1.5 s  cb[1] is mid-flight. cb_task (CP0.cpu0) should be parked in
          `xTaskNotifyWait` waiting for an engine done IRQ that never
          comes; ready_list / wait_list both empty; cb_run_cnt = 1.
  ~8.0 s  TDR has fired (kmd timeout = 3 s). q-cp is now mid
          `handle_unload_event` doing the 700-line register dump with
          `mdelay(500000)` busy-waits — useful for the unload-cascade
          analysis but hides the cb[1] root cause. See "P10 — cb[1]
          HDMA LL chain reads all zeros → Side note — TDR
          `URG_EVENT_UNLOAD` register-dump cascade" in
          `docs/debugging.md`.

Exit codes
==========

  0 — backtraces captured (read the artifact for the answer; this
      script makes no claim about pass/fail)
  1 — infrastructure failure (boot, doorbell never reached count=2,
      gdbstub didn't come up, gdb couldn't attach, missing FW ELFs)
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RUN_NAME = os.environ.get("REMU_RUN_NAME", "p10-debug")
RUN_DIR = REPO / "output" / RUN_NAME
NPU_MON_SOCK = RUN_DIR / "npu" / "monitor.sock"
HOST_MON_SOCK = RUN_DIR / "host" / "monitor.sock"
HOST_SERIAL = RUN_DIR / "host" / "serial.log"
DOORBELL_LOG = RUN_DIR / "doorbell.log"
HILS_LOG = RUN_DIR / "hils.log"
ARTIFACT_NAME = os.environ.get("REMU_P10_DEBUG_TAG", "cb1")
ARTIFACT = RUN_DIR / ("qcp-bt-%s.txt" % ARTIFACT_NAME)

GDB_PORT = 4571
BOOT_DEADLINE = 180.0
TARGET_CQ_DOORBELLS = 2
POST_DOORBELL_SETTLE_S = float(os.environ.get("REMU_P10_DEBUG_DELAY_S", "1.5"))

GDB_FALLBACK = ("/mnt/data/tools/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf"
                "/bin/aarch64-none-elf-gdb")
FW_BUNDLE = REPO / "external" / "ssw-bundle" / "products" / "rebel" / "q" / "sys"
# FreeRTOS_CP is the FW image that hosts q-cp's hq_task / cb_task / hdma_mgr
# globals (verified via `nm`). FreeRTOS_CP1 is the second-cluster image
# (also FreeRTOS, but no hq_mgr / cb_mgr symbols). q-cp runs on CP0.
ELF_FREERTOS_QCP = FW_BUNDLE / "binaries" / "FreeRTOS_CP" / "freertos_kernel.elf"
ELF_FREERTOS_CP1 = FW_BUNDLE / "binaries" / "FreeRTOS_CP1" / "freertos_kernel.elf"
ELF_BL31_CP0 = FW_BUNDLE / "binaries" / "BootLoader_CP" / "bl31.elf"
ELF_BL31_CP1 = FW_BUNDLE / "binaries" / "FreeRTOS_CP1" / "bl31.elf"


def find_gdb():
    """Locate aarch64-none-elf-gdb. Honours REMU_QCP_GDB, then $PATH,
    then $COMPILER_PATH_ARM64, then the 13.3 toolchain fallback."""
    explicit = os.environ.get("REMU_QCP_GDB")
    if explicit and Path(explicit).exists():
        return explicit
    on_path = shutil.which("aarch64-none-elf-gdb")
    if on_path:
        return on_path
    cpath = os.environ.get("COMPILER_PATH_ARM64", "")
    if cpath:
        cand = Path(cpath) / "aarch64-none-elf-gdb"
        if cand.exists():
            return str(cand)
    if Path(GDB_FALLBACK).exists():
        return GDB_FALLBACK
    return None


# --------------------------------------------------------------------------
# HMP helper (lifted from p4b — same prompt-sniffing wire pattern)
# --------------------------------------------------------------------------

def hmp(sock_path, cmd, timeout=10.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    for _ in range(50):
        try:
            s.connect(str(sock_path))
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.1)
    else:
        raise RuntimeError("socket %s didn't come up" % sock_path)

    def drain_until_prompt():
        buf = b""
        while b"(qemu) " not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.decode("utf-8", errors="replace")

    drain_until_prompt()
    s.sendall((cmd + "\n").encode())
    resp = drain_until_prompt()
    s.close()
    pat = re.escape(cmd) + r"\r?\n"
    last = None
    for m in re.finditer(pat, resp):
        last = m
    body = resp[last.end():] if last else resp
    body = body.replace("\r", "")
    if body.rstrip().endswith("(qemu)"):
        body = body.rstrip()[:-len("(qemu)")].rstrip()
    return body.strip("\n")


# --------------------------------------------------------------------------
# Boot / teardown
# --------------------------------------------------------------------------

def boot_remu(log_path):
    # `--host-mem 4G`: the kmd's `pci_p2pdma_add_resource` call against
    # the 64 GB BAR0 needs ~1 GB of `struct page` vmemmap; the default
    # 512 MB guest OOM-kills `sh` while reclaiming, which takes
    # `setup.sh` and the test along with it. Same rationale as
    # `tests/p10_umd_smoke_test.py`.
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


def wait_for_monitors(proc, timeout=BOOT_DEADLINE):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if NPU_MON_SOCK.exists() and HOST_MON_SOCK.exists():
            return True
        if proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


# --------------------------------------------------------------------------
# Doorbell trace polling — count INTGR1=0x1 occurrences. Each line is
#   "doorbell off=0x1c val=0x1 count=N"
# --------------------------------------------------------------------------

CQ_RE = re.compile(r"^doorbell off=0x1c val=0x1 count=", re.MULTILINE)


def count_cq_doorbells():
    if not DOORBELL_LOG.exists():
        return 0
    try:
        return len(CQ_RE.findall(DOORBELL_LOG.read_text(errors="replace")))
    except OSError:
        return 0


def wait_for_cq_count(proc, target, timeout):
    deadline = time.time() + timeout
    last_n = -1
    while time.time() < deadline:
        if proc.poll() is not None:
            return -1
        n = count_cq_doorbells()
        if n != last_n:
            print("  cq doorbells observed: %d (target=%d)" % (n, target))
            last_n = n
        if n >= target:
            return n
        time.sleep(0.5)
    return last_n


# --------------------------------------------------------------------------
# GDB driver. We invoke aarch64-none-elf-gdb in batch mode with a one-shot
# command file. The script:
#   - connects to the on-demand gdbstub over TCP
#   - loads the FreeRTOS_CP0 + BL31_CP0 ELF symbol files at their link
#     addresses (the FW MMU maps virt = phys + 0x10000000000; QEMU's
#     gdbstub speaks virtual post-MMU, so symbols line up directly)
#   - prints `info threads` (one entry per vCPU; CA73 cores 0..31 across
#     four chiplets) and `bt 40` per thread
#
# `set print pretty` etc are best-effort — gdb in batch mode keeps going
# past `print` errors (e.g. when a symbol isn't in the symtab), so we
# can ask for both rbdma_mgr and hdma_mgr without guarding each one.
# --------------------------------------------------------------------------

GDB_CMDS_TEMPLATE = r"""
set pagination off
set confirm off
set print pretty on
set print elements 256
set logging file {artifact}
set logging overwrite on
set logging redirect on
set logging enabled on
target remote :{port}
echo \n========== Symbol files ==========\n
file {elf_freertos_qcp}
add-symbol-file {elf_bl31_cp0}
add-symbol-file {elf_freertos_cp1}
add-symbol-file {elf_bl31_cp1}

echo \n========== hq_mgr globals ==========\n
print hq_mgr.cb_run_cnt
print hq_mgr.req_funcs
print hq_mgr.req_funcs_init_q
print hq_mgr.next_sched_func
print hq_mgr.func_id
print hq_mgr.num_vfs
print hq_mgr.cq[0].initialized
print/x hq_mgr.cq[0].base_addr
print hq_mgr.cq[0].ci
print hq_mgr.cq[0].pi
print hq_mgr.cq[0].size
print hq_mgr.cq[0].mask

echo \n========== cb_mgr state ==========\n
print cb_mgr
print cb_mgr.ready_list.next == &cb_mgr.ready_list
print cb_mgr.wait_list.next == &cb_mgr.wait_list
print *cb_mgr.htask

echo \n========== hdma_mgr state ==========\n
print hdma_mgr
print hdma_mgr.hdma_queue[0]
print hdma_mgr.hdma_queue[1]
print *hdma_mgr.htask

echo \n========== cfg-shadow region (DDH + FUNC_SCRATCH) ==========\n
x/16wx 0x102000C0
x/8wx  0x10200FFC

echo \n========== HDMA RD ch0 register block (chiplet-0 0x1D80380000) ==========\n
monitor xp /16wx 0x1D80380300
monitor xp /16wx 0x1D80380400

echo \n========== LL cursor neighborhood (NPU PA 0x42B86700..0x42B867BF) ==========\n
monitor xp /16wx 0x42B86700
monitor xp /16wx 0x42B86740
monitor xp /16wx 0x42B86780

echo \n========== chiplet-0 DRAM head 1 KB ==========\n
monitor xp /16wx 0x07F00000

echo \n========== Threads ==========\n
info threads

echo \n========== Per-thread backtraces ==========\n
thread apply all bt 40

echo \n========== Per-thread registers (PC/SP/FP/LR) ==========\n
thread apply all info registers pc sp x29 x30

detach
quit
"""


def run_gdb_snapshot(gdb_bin):
    cmds_path = RUN_DIR / ("qcp-bt-%s.gdb" % ARTIFACT_NAME)
    cmds = GDB_CMDS_TEMPLATE.format(
        artifact=str(ARTIFACT),
        port=GDB_PORT,
        elf_freertos_qcp=str(ELF_FREERTOS_QCP),
        elf_bl31_cp0=str(ELF_BL31_CP0),
        elf_freertos_cp1=str(ELF_FREERTOS_CP1),
        elf_bl31_cp1=str(ELF_BL31_CP1),
    )
    cmds_path.write_text(cmds)
    print("  invoking %s -x %s" % (gdb_bin, cmds_path))
    proc = subprocess.run(
        [gdb_bin, "-batch", "-x", str(cmds_path)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=120,
    )
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    os.environ["PYTHONUNBUFFERED"] = "1"

    if not ELF_FREERTOS_QCP.exists():
        print("ERR: missing %s — run ./remucli fw-build" % ELF_FREERTOS_QCP)
        return 1
    gdb_bin = find_gdb()
    if not gdb_bin:
        print("ERR: aarch64-none-elf-gdb not found. Set REMU_QCP_GDB or "
              "COMPILER_PATH_ARM64, or place the binary on $PATH.")
        return 1
    print("  using gdb: %s" % gdb_bin)

    RUN_DIR.mkdir(parents=True, exist_ok=True)
    log_path = RUN_DIR / "run.log"

    # Drop stale state — boot would clean it anyway, but we need
    # doorbell.log to be the new run's so count_cq_doorbells() doesn't
    # latch onto a previous count.
    for stale in (HOST_SERIAL, NPU_MON_SOCK, HOST_MON_SOCK, DOORBELL_LOG,
                  HILS_LOG, ARTIFACT):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    proc = boot_remu(log_path)
    try:
        print("Booting --host with remu.run_p10=1, name=%s" % RUN_NAME)
        if not wait_for_monitors(proc):
            print("FAIL: monitor sockets never came up within %.0fs"
                  % BOOT_DEADLINE)
            return 1
        print("Monitors up. Watching doorbell.log for cb[1] CQ "
              "(target %d INTGR1=0x1 frames)..." % TARGET_CQ_DOORBELLS)
        n = wait_for_cq_count(proc, TARGET_CQ_DOORBELLS, BOOT_DEADLINE)
        if n < TARGET_CQ_DOORBELLS:
            print("FAIL: only %d CQ doorbells observed before deadline" % n)
            return 1
        print("cb[%d] doorbell delivered. Sleeping %.1fs to let q-cp wedge..."
              % (TARGET_CQ_DOORBELLS - 1, POST_DOORBELL_SETTLE_S))
        time.sleep(POST_DOORBELL_SETTLE_S)

        print("Starting gdbserver via NPU HMP (tcp::%d, wait=off)..."
              % GDB_PORT)
        out = hmp(NPU_MON_SOCK, "gdbserver tcp::%d" % GDB_PORT)
        if out:
            print("  HMP: %s" % out.strip())

        # Tiny breath — gdbstub bind is synchronous from HMP's view but
        # connection accept may need a tick.
        time.sleep(0.5)

        rc, transcript = run_gdb_snapshot(gdb_bin)
        # Stash the gdb stdout next to the artifact so we have both the
        # set-logging-redirected `info threads`/`bt` output AND any
        # symbol-load warnings.
        (RUN_DIR / ("qcp-bt-%s.gdb-stdout.txt" % ARTIFACT_NAME)
         ).write_text(transcript)

        print("\n--- gdb stdout (first 60 lines) ---")
        for line in transcript.splitlines()[:60]:
            print("  " + line)

        if not ARTIFACT.exists() or ARTIFACT.stat().st_size == 0:
            print("FAIL: gdb produced no artifact at %s (rc=%d)"
                  % (ARTIFACT, rc))
            return 1

        print("\nartifact: %s" % ARTIFACT)
        print("\n--- artifact head (first 80 lines) ---")
        with ARTIFACT.open() as f:
            for i, line in enumerate(f):
                if i >= 80:
                    print("  ...")
                    break
                print("  " + line.rstrip())

        return 0

    finally:
        teardown(proc)


if __name__ == "__main__":
    sys.exit(main())
