#!/usr/bin/env python3
"""M5 data-flow test.

Launches `./remucli run --host --name m5-flow` in the background, waits
for both QEMU monitor sockets, writes a magic pattern into the shared
/dev/shm file at an offset past the FW image load zone, and reads the
same bytes back from BOTH the NPU's chiplet-0 DRAM (via HMP `xp` on the
NPU monitor) and the x86 guest's BAR0 (via HMP `xp` on the host monitor).

Success = both xp outputs contain the magic pattern.
"""
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RUN_NAME = os.environ.get("REMU_RUN_NAME", "m5-flow")
RUN_DIR = REPO / "output" / RUN_NAME
SHM_FILE = Path("/dev/shm/remu-%s/remu-shm" % RUN_NAME)

# Magic pattern at an offset past the FW image region.
# Chiplet 0 DRAM layout: BL31_CP0=0..2MB, FreeRTOS_CP0=2..~20MB,
#                        BL31_CP1=321..322MB, FreeRTOS_CP1=322..~330MB.
# Pick 127 MB — inside the 128 MB shared slice, well past any FW load.
OFFSET = 0x07F00000
MAGIC = bytes.fromhex("deadbeef" "cafebabe" "baadf00d" "feedface")


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


def main():
    os.environ["PYTHONUNBUFFERED"] = "1"
    remucli = REPO / "remucli"
    log = RUN_DIR / "run.log"
    log.parent.mkdir(parents=True, exist_ok=True)

    # Spawn remucli run in a new session so we can kill the whole tree.
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
        # Wait for monitors to come up.
        deadline = time.time() + 30
        while time.time() < deadline:
            if npu_mon.exists() and host_mon.exists() and SHM_FILE.exists():
                break
            if proc.poll() is not None:
                print("remucli exited unexpectedly; tail:")
                print(log.read_text()[-2000:])
                return 1
            time.sleep(0.2)
        else:
            print("monitor sockets never came up")
            return 1

        # Write magic.
        with open(SHM_FILE, "r+b") as f:
            f.seek(OFFSET)
            f.write(MAGIC)
            f.flush()
            os.fsync(f.fileno())

        # Wait one extra tick for SeaBIOS to finish programming BAR0 and
        # for the FW to not have made it to offset 127 MB yet.
        time.sleep(2.0)

        npu_xp = hmp(npu_mon, "xp /4wx 0x%x" % OFFSET)
        host_xp = hmp(host_mon, "xp /4wx 0x%x" % (0xE000000000 + OFFSET))

        # `xp /4wx` reads 32-bit words as little-endian, so the 16-byte
        # MAGIC pattern DE AD BE EF CA FE BA BE BA AD F0 0D FE ED FA CE
        # appears as: 0xefbeadde 0xbebafeca 0x0df0adba 0xcefaedfe.
        expect_words = ["0xefbeadde", "0xbebafeca", "0x0df0adba", "0xcefaedfe"]
        npu_ok = all(w in npu_xp.lower() for w in expect_words)
        host_ok = all(w in host_xp.lower() for w in expect_words)

        print("NPU xp  @0x%x:   %s" % (OFFSET, npu_xp.strip()))
        print("HOST xp @0x%x: %s" % (0xE000000000 + OFFSET, host_xp.strip()))
        print()
        print("NPU  has magic:", npu_ok)
        print("HOST has magic:", host_ok)
        return 0 if (npu_ok and host_ok) else 2

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
