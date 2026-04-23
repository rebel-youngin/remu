#!/usr/bin/env python3
"""M6 doorbell end-to-end test.

Stands up a minimal NPU-only QEMU configured to receive doorbell frames
over a Unix socket, pretends to be the host-side r100-npu-pci BAR4, and
asserts the NPU's r100-doorbell device actually parses the frames and
pulses its GIC SPI line.

Why not use `./remucli run --host`? That path has the host QEMU as the
chardev server and the NPU QEMU as the client (see _build_host_cmd in
cli/remu_cli.py). A QEMU socket-chardev server only accepts one client,
so the test cannot join that conversation to inject synthetic writes,
and there is no HMP primitive for writing guest physical memory to
drive a real BAR4 write from SeaBIOS's point of view either. Instead
the test flips the server/client roles: the test itself binds a socket
and the NPU connects to it, which gives direct control over the wire
at the cost of not exercising the x86-side BAR4 intercept path (that
half is covered by the BAR4 MMIO-overlay mtree check already done
by `remucli run --host` at every invocation).

Wire protocol (must match src/machine/r100_doorbell.c and
src/host/r100_npu_pci.c):

    struct frame {
        uint32_t bar4_offset;   // little-endian
        uint32_t value;          // little-endian
    };  // 8 bytes total

Success criteria:
  1. The NPU-side debug chardev ("doorbell.log") logs one line per
     frame we send, with matching offset and value.
  2. `info qtree` on the NPU lists a r100-doorbell device.
  3. Sending a frame with an unrecognised offset produces a
     GUEST_ERROR log entry (proves the parser rejects malformed
     input instead of silently firing the IRQ).
"""
import os
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
QEMU_BIN = REPO / "build" / "qemu" / "qemu-system-aarch64"
IMAGES_DIR = REPO / "images"
RUN_NAME = "m6-doorbell"
RUN_DIR = REPO / "output" / RUN_NAME
DOORBELL_SOCK = RUN_DIR / "doorbell.sock"
DOORBELL_LOG = RUN_DIR / "doorbell.log"
QEMU_LOG = RUN_DIR / "qemu.log"
NPU_MON = RUN_DIR / "monitor.sock"

# MAILBOX_INTGR offsets as seen by the host QEMU on BAR4 (must match
# R100_BAR4_MAILBOX_INTGR0 / R100_BAR4_MAILBOX_INTGR1 in
# src/include/r100/remu_addrmap.h).
INTGR0 = 0x00000008
INTGR1 = 0x0000001C
# BOGUS must be outside BOTH the INTGR trigger offsets (0x08 / 0x1c)
# AND the M8-extended MAILBOX_BASE payload range (0x80..0x180, 64 u32
# ISSR scratch slots). 0x200 lands in the 4 KB BAR4 MMIO window but
# matches no real register on either silicon or our device model, so
# r100_doorbell_deliver must drop it with a GUEST_ERROR entry.
BOGUS  = 0x00000200


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
        raise RuntimeError("monitor socket %s didn't come up" % sock_path)

    def _drain():
        buf = b""
        while b"(qemu) " not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.decode("utf-8", errors="replace")

    _drain()
    s.sendall((cmd + "\n").encode())
    resp = _drain()
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


def frame(offset, value):
    return struct.pack("<II", offset, value)


def wait_for(predicate, timeout, poll=0.1, desc=""):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(poll)
    raise TimeoutError("timed out waiting for %s" % (desc or "predicate"))


def main():
    if not QEMU_BIN.is_file():
        print("%s not built; run ./remucli build first" % QEMU_BIN)
        return 1

    RUN_DIR.mkdir(parents=True, exist_ok=True)
    for p in (DOORBELL_SOCK, DOORBELL_LOG, QEMU_LOG, NPU_MON):
        if p.exists() or p.is_symlink():
            try:
                p.unlink()
            except OSError:
                pass

    # Bind the doorbell socket first — QEMU's chardev client will
    # connect to it during machine init, so a missing listener would
    # fail realize or spin on reconnect.
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(str(DOORBELL_SOCK))
    srv.listen(1)
    srv.settimeout(10.0)

    # Minimal NPU QEMU invocation: machine init wires r100-doorbell
    # regardless of whether FW loads, but we still load BL1 / BL31 /
    # FreeRTOS if they exist so info qtree and the log-mask filter
    # behave like a normal run. All serials redirect to the run dir so
    # this test is silent on its own stdout.
    cmd = [
        str(QEMU_BIN),
        "-M",
        "r100-soc,doorbell=doorbell,doorbell-debug=doorbell_dbg",
        "-display", "none",
        "-nographic",
        "-chardev", "socket,id=doorbell,path=%s,reconnect=1" % DOORBELL_SOCK,
        "-chardev", "file,id=doorbell_dbg,path=%s,mux=off" % DOORBELL_LOG,
        "-chardev", "file,id=uart0,path=%s/uart0.log,mux=off" % RUN_DIR,
        "-serial", "chardev:uart0",
        "-chardev", "file,id=uart1,path=%s/uart1.log,mux=off" % RUN_DIR,
        "-serial", "chardev:uart1",
        "-chardev", "file,id=uart2,path=%s/uart2.log,mux=off" % RUN_DIR,
        "-serial", "chardev:uart2",
        "-chardev", "file,id=uart3,path=%s/uart3.log,mux=off" % RUN_DIR,
        "-serial", "chardev:uart3",
        "-chardev", "file,id=hils,path=%s/hils.log,mux=off" % RUN_DIR,
        "-serial", "chardev:hils",
        "-monitor", "unix:%s,server=on,wait=off" % NPU_MON,
        "-d", "guest_errors,unimp",
        "-D", str(QEMU_LOG),
    ]
    # Optional FW images — if present let the machine proceed past
    # reset into BL1. Missing images are fine; r100-doorbell is wired
    # in machine-init and does not depend on guest code running.
    fw_set = [
        ("bl1.bin",          0x14000000),
        ("bl31_cp0.bin",     0x14040000),
        ("freertos_cp0.bin", 0x14200000),
        ("bl31_cp1.bin",     0x14E00000),
        ("freertos_cp1.bin", 0x15000000),
    ]
    for fname, addr in fw_set:
        p = IMAGES_DIR / fname
        if p.is_file():
            cmd += ["-device", "loader,file=%s,addr=0x%x" % (p, addr)]

    with open(RUN_DIR / "qemu.stdout.log", "wb") as _so, \
         open(RUN_DIR / "qemu.stderr.log", "wb") as _se:
        proc = subprocess.Popen(
            cmd, stdout=_so, stderr=_se, stdin=subprocess.DEVNULL,
            start_new_session=True,
        )

    client = None
    rc = 0
    try:
        # Accept the NPU's chardev connection (times out if realize
        # never fires, e.g. because the machine option was wrong).
        client, _ = srv.accept()
        client.settimeout(5.0)

        # Monitor-socket wait: makes the HMP checks race-free.
        wait_for(NPU_MON.exists, timeout=10.0, desc="NPU monitor socket")

        # Smoke: r100-doorbell is in the device tree.
        qtree = hmp(NPU_MON, "info qtree")
        (RUN_DIR / "info-qtree.log").write_text(qtree + "\n")
        if "r100-doorbell" not in qtree:
            print("FAIL: r100-doorbell not listed in info qtree")
            print(qtree)
            return 2

        # --- Frame round 1: INTGR1 with bitmask 0x1 (db_idx 0) ---
        client.sendall(frame(INTGR1, 0x1))
        # --- Frame round 2: INTGR0 with bitmask 0x8000 (db_idx 47) ---
        client.sendall(frame(INTGR0, 0x8000))
        # --- Frame round 3: BOGUS offset → must be rejected ---
        client.sendall(frame(BOGUS, 0xDEADBEEF))

        # The debug chardev flushes each frame as an ASCII line. Poll
        # the file until two valid frames show up (the third is
        # expected to be rejected before it reaches the debug tail).
        def _log_has_two():
            if not DOORBELL_LOG.exists():
                return False
            return DOORBELL_LOG.read_text().count("doorbell off=") >= 2
        wait_for(_log_has_two, timeout=5.0, desc="debug log to show 2 frames")

        log_text = DOORBELL_LOG.read_text()
        expected = [
            "doorbell off=0x1c val=0x1 count=1",
            "doorbell off=0x8 val=0x8000 count=2",
        ]
        missing = [line for line in expected if line not in log_text]
        if missing:
            print("FAIL: missing debug lines:")
            for m in missing:
                print("  " + repr(m))
            print("--- actual doorbell.log ---")
            print(log_text)
            return 3

        # The bogus frame must leave a guest-error trail in -D.
        def _qemu_log_has_reject():
            if not QEMU_LOG.exists():
                return False
            return "r100-doorbell: unexpected frame" in QEMU_LOG.read_text()
        try:
            wait_for(_qemu_log_has_reject, timeout=3.0,
                     desc="guest-error entry for bogus frame")
        except TimeoutError:
            print("FAIL: bogus offset 0x%x did not trigger GUEST_ERROR "
                  "in qemu.log" % BOGUS)
            if QEMU_LOG.exists():
                print("--- qemu.log tail ---")
                print(QEMU_LOG.read_text()[-2000:])
            return 4

        # Report.
        print("PASS: M6 doorbell plumbing end-to-end")
        print("  run dir:       %s" % RUN_DIR)
        print("  frames sent:   3 (2 accepted, 1 rejected)")
        print("  debug log:     %s" % DOORBELL_LOG)
        print("  guest errors:  %s" % QEMU_LOG)
        for ln in log_text.strip().splitlines():
            print("    " + ln)
        return rc

    finally:
        if client is not None:
            try:
                client.close()
            except OSError:
                pass
        try:
            srv.close()
        except OSError:
            pass
        if proc.poll() is None:
            try:
                os.killpg(os.getpgid(proc.pid), 15)
                proc.wait(timeout=5)
            except (subprocess.TimeoutExpired, ProcessLookupError):
                try:
                    os.killpg(os.getpgid(proc.pid), 9)
                except ProcessLookupError:
                    pass
        try:
            DOORBELL_SOCK.unlink()
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
