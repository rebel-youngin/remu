#!/usr/bin/env python3
"""P10 cfg-shadow probe — verify NPU/host views agree at the first QUEUE_INIT.

This is **not** a regression test (no pass/fail assertion) — it is the
diagnostic recipe for the open P10 side bug documented in
`docs/debugging.md` → "P10 cfg-shadow NULL-deref (intermittent)" and
`docs/roadmap.md` → P10. Run it manually when investigating the
~30%-of-runs `hil_init_descs` NULL-dereference at boot.

What it does
============

Boots `./remucli run --host --name p10-debug` with `remu.run_p10=1`,
watches `output/p10-debug/doorbell.log` for the first
`INTGR1=0x80` (QUEUE_INIT) doorbell, then samples the kmd-published
device-descriptor pointer (`DDH_BASE_LO/HI/SIZE` at cfg-shadow offsets
`0x00C0..0x00CB`) from three independent vantage points:

  1. The shared mmap file directly:
        /dev/shm/remu-<name>/cfg-shadow @ 0x00C0..
  2. The NPU side via HMP `xp /4wx 0x102000C0` (q-cp's
     `R100_DEVICE_COMM_SPACE_BASE` view, served by the
     `r100-cm7` cfg-mirror MMIO trap aliased over cfg-shadow).
  3. The host x86 side via HMP `xp /4wx <BAR2 cfg-head phys>+0xC0`
     (the kmd's `mmio.cfg + DDH_BASE_LO` write target, served by
     `r100-npu-pci`'s BAR2 cfg-head subregion aliased over the same
     cfg-shadow file).

If (1)/(2)/(3) all show the same dma_addr, the cfg-shadow plumbing
is healthy and any q-cp NULL-deref on this run came from somewhere
else (a stale read raced against the write, FW logic, etc).

If (1) shows the dma_addr but (2) or (3) reads zero, the
`memory-backend-file` mmap isn't actually shared (check
`info mtree -f` on each QEMU; the alias must overlay the
`memory-backend-file` object created in `cli/remu_cli.py`).

If (1) reads zero, the kmd never wrote to the cfg-head — verify
`rebel_cfg_write` traffic order in the host `dmesg` log.

A second sample is taken 2 s later to catch any late propagation
from a slow chardev egress (none should exist post-P10-fix; this
is a sanity check).

Output goes to stdout — the artifact is the printed transcript.
Re-run the probe; if you get a clean trace twice in a row, the
flakiness is in the q-cp consumer side, not the cfg-shadow plumbing.

Exit codes
==========

  0 — three views captured and printed (read the output for the answer)
  1 — infrastructure failure (boot, doorbell never reached, missing
      cfg-shadow file)
"""
import mmap
import os
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RUN_NAME = "p10-debug"
RUN_DIR = REPO / "output" / RUN_NAME
NPU_MON = RUN_DIR / "npu" / "monitor.sock"
HOST_MON = RUN_DIR / "host" / "monitor.sock"
DOORBELL_LOG = RUN_DIR / "doorbell.log"
SHM_PATH = Path("/dev/shm/remu-" + RUN_NAME) / "cfg-shadow"

# DDH_BASE_LO = 0x00C0, DDH_BASE_HI = 0x00C4, DDH_SIZE = 0x00C8.
# NPU view: R100_DEVICE_COMM_SPACE_BASE + reg = 0x10200000 + reg.
NPU_DDH_BASE = 0x10200000


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


def boot_remu(log_path):
    return subprocess.Popen(
        [str(REPO / "remucli"), "run", "--host", "--name", RUN_NAME,
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


def count_qinit_doorbells():
    if not DOORBELL_LOG.exists():
        return 0
    return sum(1 for ln in DOORBELL_LOG.read_text(errors="replace").splitlines()
               if "off=0x1c val=0x80" in ln)


def hexdump_shm(off, n):
    with open(SHM_PATH, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
        try:
            data = bytes(mm[off:off + n])
        finally:
            mm.close()
    words = struct.unpack("<%dI" % (n // 4), data)
    return " ".join("0x%08x" % w for w in words)


def main():
    log_path = RUN_DIR.with_suffix(".log")
    log_path.parent.mkdir(parents=True, exist_ok=True)

    for stale in (DOORBELL_LOG, NPU_MON, HOST_MON):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    proc = boot_remu(log_path)
    deadline = time.time() + 180
    try:
        while time.time() < deadline:
            if NPU_MON.exists() and HOST_MON.exists():
                break
            if proc.poll() is not None:
                print("FAIL: remucli exited rc=%d" % proc.returncode)
                return 1
            time.sleep(0.2)
        else:
            print("FAIL: monitors never came up")
            return 1
        print("Monitors up. Polling for QUEUE_INIT doorbell...")

        last = -1
        while time.time() < deadline:
            if proc.poll() is not None:
                print("FAIL: remucli died early")
                return 1
            n = count_qinit_doorbells()
            if n != last:
                print("  qinit doorbells observed: %d" % n)
                last = n
            if n >= 1:
                # First Q_INIT seen — kmd already wrote DDH cfg before
                # this doorbell. Sample now.
                break
            time.sleep(0.2)
        else:
            print("FAIL: never saw QUEUE_INIT doorbell")
            return 1

        # Tiny breath to let cfg-shadow stabilise (should be
        # instantaneous via mmap; this is the coherence sanity check).
        time.sleep(0.2)

        print("\n=== Direct shm file read ===")
        if not SHM_PATH.exists():
            print("FAIL: %s missing" % SHM_PATH)
            return 1
        # DDH_BASE_LO=0xC0, _HI=0xC4, _SIZE=0xC8
        print("  cfg-shadow @0x00C0..0x00CF = %s"
              % hexdump_shm(0xC0, 16))

        print("\n=== NPU HMP xp at 0x10200000+0xC0 ===")
        npu_xp = hmp(NPU_MON, "xp /4wx 0x%x" % (NPU_DDH_BASE + 0xC0))
        print("  %s" % npu_xp)

        print("\n=== Host HMP xp at BAR2 cfg-head + 0xC0 ===")
        # info-mtree.log captured at boot lists the alias at e.g.
        # 0x000000f000200000 — grep that out instead of poking PCI BARs.
        info_mtree_path = RUN_DIR / "host" / "info-mtree.log"
        host_cfg_phys = None
        if info_mtree_path.exists():
            info_mtree = info_mtree_path.read_text(errors="replace")
            m = re.search(
                r"^\s*([0-9a-f]+)-[0-9a-f]+ \(prio 10, ram\): "
                r"alias r100\.bar2\.cfg\.alias",
                info_mtree, re.MULTILINE)
            if m:
                host_cfg_phys = int(m.group(1), 16)
        if host_cfg_phys is not None:
            print("  host BAR2 cfg-head phys = 0x%x" % host_cfg_phys)
            host_xp = hmp(HOST_MON, "xp /4wx 0x%x" % (host_cfg_phys + 0xC0))
            print("  %s" % host_xp)
        else:
            print("  could not find r100.bar2.cfg.alias in "
                  "%s — skipping host xp" % info_mtree_path)

        print("\n=== Re-sample 2 s later ===")
        time.sleep(2.0)
        print("  cfg-shadow @0x00C0..0x00CF = %s"
              % hexdump_shm(0xC0, 16))
        npu_xp2 = hmp(NPU_MON, "xp /4wx 0x%x" % (NPU_DDH_BASE + 0xC0))
        print("  npu xp: %s" % npu_xp2)
        if host_cfg_phys is not None:
            host_xp2 = hmp(HOST_MON,
                           "xp /4wx 0x%x" % (host_cfg_phys + 0xC0))
            print("  host xp: %s" % host_xp2)

        return 0
    finally:
        teardown(proc)


if __name__ == "__main__":
    sys.exit(main())
