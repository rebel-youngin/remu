#!/usr/bin/env python3
"""P4A r100-rbdma functional-stub register check.

Boots `./remucli run --host --name p4a-rbdma`, waits for the NPU HMP
monitor, and reads four chiplet-0 RBDMA register offsets via `xp`.
Confirms:

  * IP_INFO3.num_of_executer = 8  (RBDMA_INFO3_SEED)
  * NORMALTQUEUE_STATUS      = 32 (R100_RBDMA_NUM_TQ)
  * PTQUEUE_STATUS           = 32 (R100_RBDMA_NUM_PTQ)
  * INTR_FIFO_READABLE_NUM   = 0  (idle, no pending FNSH entries)

Doesn't exercise the kick path — that needs a real CPU writing
RUN_CONF1 (which q-cp will do once its CB worker grows DDMA dispatch
under P4B). This test only fences the seed-side regression so a future
refactor that drops the IP_INFO seeding doesn't silently break q-cp's
rbdma_init credit math.
"""
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RUN_NAME = os.environ.get("REMU_RUN_NAME", "p4a-rbdma")
RUN_DIR = REPO / "output" / RUN_NAME

# Chiplet-0 RBDMA register addresses (NBUS_L_RBDMA_CFG_BASE + offset).
RBDMA_BASE = 0x1FF3700000
IP_INFO3 = RBDMA_BASE + 0x00C
NORMALTQ_STATUS = RBDMA_BASE + 0x180
PTQUEUE_STATUS = RBDMA_BASE + 0x190
INTR_FIFO_NUM = RBDMA_BASE + 0x120


def hmp(sock_path, cmd, timeout=5.0):
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


def read_word(npu_mon, addr):
    """One 32-bit LE word at `addr` via `xp /1wx`."""
    line = hmp(npu_mon, "xp /1wx 0x%x" % addr)
    m = re.search(r"0x([0-9a-fA-F]+)", line.split(":", 1)[-1])
    if not m:
        raise RuntimeError("can't parse xp output: %r" % line)
    return int(m.group(1), 16)


def main():
    os.environ["PYTHONUNBUFFERED"] = "1"
    remucli = REPO / "remucli"
    log = RUN_DIR / "run.log"
    log.parent.mkdir(parents=True, exist_ok=True)

    with open(log, "wb") as f:
        proc = subprocess.Popen(
            [str(remucli), "run", "--host", "--name", RUN_NAME],
            stdout=f, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
        )

    npu_mon = RUN_DIR / "npu" / "monitor.sock"
    host_mon = RUN_DIR / "host" / "monitor.sock"

    try:
        deadline = time.time() + 30
        while time.time() < deadline:
            if npu_mon.exists() and host_mon.exists():
                break
            if proc.poll() is not None:
                print("remucli exited unexpectedly; tail:")
                print(log.read_text()[-2000:])
                return 1
            time.sleep(0.2)
        else:
            print("monitor sockets never came up")
            return 1

        # Let q-cp's rbdma_init run before reading. It only writes
        # IP_INFO2 (chiplet_id), so the other seeds are untouched.
        time.sleep(2.0)

        info3 = read_word(npu_mon, IP_INFO3)
        ntq = read_word(npu_mon, NORMALTQ_STATUS)
        ptq = read_word(npu_mon, PTQUEUE_STATUS)
        fnsh = read_word(npu_mon, INTR_FIFO_NUM)

        # IP_INFO3.num_of_executer is bits 8..15. Mask off the rest so
        # this test stays robust to future changes in num_of_max_sgr /
        # num_of_max_sgi (bits 16..31) which q-cp doesn't currently
        # consume.
        num_te = (info3 >> 8) & 0xFF

        print("IP_INFO3            = 0x%08x  (num_of_executer=%d)" % (info3, num_te))
        print("NORMALTQUEUE_STATUS = 0x%08x  (= %d)" % (ntq, ntq))
        print("PTQUEUE_STATUS      = 0x%08x  (= %d)" % (ptq, ptq))
        print("INTR_FIFO_NUM       = 0x%08x  (= %d)" % (fnsh, fnsh))

        ok = True
        if num_te != 8:
            print("FAIL: num_of_executer expected 8, got %d" % num_te)
            ok = False
        if ntq != 32:
            print("FAIL: NORMALTQUEUE_STATUS expected 32, got %d" % ntq)
            ok = False
        if ptq != 32:
            print("FAIL: PTQUEUE_STATUS expected 32, got %d" % ptq)
            ok = False
        if fnsh != 0:
            print("FAIL: INTR_FIFO_NUM expected 0, got %d" % fnsh)
            ok = False

        return 0 if ok else 2

    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 15)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(proc.pid), 9)


if __name__ == "__main__":
    sys.exit(main())
