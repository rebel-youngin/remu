#!/usr/bin/env python3
"""M7 iMSIX-DB end-to-end test.

Stands up a minimal HOST x86_64 QEMU configured with the r100-npu-pci
device's `msix` CharBackend wired to a Unix-socket chardev (server
side). The test impersonates the NPU-side r100-imsix device by
connecting as a client and emitting 8-byte (offset, db_data) frames
on that socket. Asserts the host-side debug tail records one parsed
frame per valid emission, with the correct vector extracted from
db_data, and that out-of-range / bad-offset frames are rejected with
the matching status tag (and a GUEST_ERROR log entry).

This is the reverse-direction mirror of tests/m6_doorbell_test.py:

    M6 flow:  x86 guest BAR4 write
              -> host QEMU r100-npu-pci doorbell intercept
              -> 8-byte frame on doorbell socket
              -> NPU QEMU r100-cm7 device
              -> GIC SPI on r100-mailbox

    M7 flow:  NPU CPU store to REBELH_PCIE_MSIX_ADDR (0x1BFFFFFFFC)
              -> NPU QEMU r100-imsix trap at R100_PCIE_IMSIX_DB_OFFSET
              -> 8-byte frame on msix socket
              -> host QEMU r100-npu-pci msix intercept
              -> msix_notify() for db_data[10:0]

Why not ./remucli run --host? Same reason m6 ships its own binary: the
production socket roles have the host as server and the NPU as client,
which is perfect for normal operation but prevents a third-party test
from joining the conversation. The test flips the socket role on the
NPU-emulating side (the test itself IS the NPU as far as the wire is
concerned), which lets it inject arbitrary frames end-to-end without
waiting on FW to actually issue a real store.

Wire protocol (must match src/machine/r100_imsix.c on the NPU side
and src/host/r100_npu_pci.c on the host side):

    struct frame {
        uint32_t offset;    // little-endian, always IMSIX_DB_OFFSET
        uint32_t db_data;   // little-endian, [10:0] = vector
    };

Success criteria:
  1. For each frame with offset == R100_PCIE_IMSIX_DB_OFFSET and
     vector in [0, R100_NUM_MSIX), the debug tail records
     `status=ok vector=<v>`.
  2. A frame with vector >= R100_NUM_MSIX is rejected with
     `status=oor` (matches r100_msix_deliver's bounds check).
  3. A frame with a wrong offset is rejected with
     `status=bad-offset`.
  4. Rejected frames leave a GUEST_ERROR trail in the host QEMU's
     `-D` log so a human can trace the rejection post-mortem.
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
QEMU_BIN_X86 = REPO / "build" / "qemu" / "qemu-system-x86_64"
RUN_NAME = "m7-msix"
RUN_DIR = REPO / "output" / RUN_NAME
HOST_DIR = RUN_DIR / "host"
MSIX_SOCK = HOST_DIR / "msix.sock"
MSIX_LOG = RUN_DIR / "msix.log"
HOST_MON = HOST_DIR / "monitor.sock"
HOST_QEMU_LOG = HOST_DIR / "qemu.log"

# Constants — must match src/include/r100/remu_addrmap.h and the REBEL_MSIX_ENTRIES
# constant the driver uses (32 on CR03).
IMSIX_DB_OFFSET = 0x00000FFC    # R100_PCIE_IMSIX_DB_OFFSET
VECTOR_MASK     = 0x000007FF    # R100_PCIE_IMSIX_VECTOR_MASK
NUM_MSIX        = 32            # R100_NUM_MSIX in src/host/r100_npu_pci.c

BAD_OFFSET      = 0x00000100    # within the 4 KB page but not IMSIX_DB_OFFSET


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


def frame(offset, db_data):
    return struct.pack("<II", offset, db_data)


def wait_for(predicate, timeout, poll=0.1, desc=""):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(poll)
    raise TimeoutError("timed out waiting for %s" % (desc or "predicate"))


def main():
    if not QEMU_BIN_X86.is_file():
        print("%s not built; run ./remucli build first" % QEMU_BIN_X86)
        return 1

    HOST_DIR.mkdir(parents=True, exist_ok=True)
    for p in (MSIX_SOCK, MSIX_LOG, HOST_QEMU_LOG, HOST_MON):
        if p.exists() or p.is_symlink():
            try:
                p.unlink()
            except OSError:
                pass

    # Test IS the NPU side: listen on msix.sock so the host QEMU's
    # socket chardev can connect to us as a client. Flipped vs. the
    # production roles in cli/remu_cli.py, same flip as m6.
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(str(MSIX_SOCK))
    srv.listen(1)
    srv.settimeout(15.0)

    # Minimal host QEMU: r100-npu-pci with only the msix chardev wired.
    # memdev/doorbell are deliberately left unset — the M7 receive path
    # only cares about the MSI-X capability being present and the
    # msix CharBackend having a `set_handlers` hook installed, both of
    # which happen unconditionally in r100_npu_pci_realize. SeaBIOS is
    # allowed to run (no -S) so BAR5 gets programmed and msix-table +
    # msix-pba overlays show up in `info mtree`, mirroring the
    # production verify path.
    cmd = [
        str(QEMU_BIN_X86),
        "-M", "pc",
        "-cpu", "qemu64",
        "-m", "256M",
        "-display", "none",
        "-nographic",
        "-no-reboot",
        "-chardev",
        "socket,id=msix,path=%s,reconnect=1" % MSIX_SOCK,
        "-chardev",
        "file,id=msix_dbg,path=%s,mux=off" % MSIX_LOG,
        "-device", "r100-npu-pci,msix=msix,msix-debug=msix_dbg",
        "-monitor", "unix:%s,server=on,wait=off" % HOST_MON,
        "-serial", "file:%s" % (HOST_DIR / "serial.log"),
        "-d", "guest_errors,unimp",
        "-D", str(HOST_QEMU_LOG),
    ]

    with open(HOST_DIR / "qemu.stdout.log", "wb") as _so, \
         open(HOST_DIR / "qemu.stderr.log", "wb") as _se:
        proc = subprocess.Popen(
            cmd, stdout=_so, stderr=_se, stdin=subprocess.DEVNULL,
            start_new_session=True,
        )

    client = None
    try:
        # Host QEMU connects as the msix client during realize; we
        # accept. Timeout trips if the device fails realize (missing
        # chardev id, etc.) or the QEMU binary is broken.
        client, _ = srv.accept()
        client.settimeout(5.0)

        wait_for(HOST_MON.exists, timeout=15.0,
                 desc="host HMP monitor socket")

        # Smoke: r100-npu-pci realized and msix-table overlay present.
        # SeaBIOS needs a moment to program BAR5; poll just like the
        # production _verify_msix_wired does.
        def _bar5_ready():
            try:
                mt = hmp(HOST_MON, "info mtree", timeout=5.0)
            except Exception:
                return False
            return "msix-table" in mt
        try:
            wait_for(_bar5_ready, timeout=15.0,
                     desc="SeaBIOS to program BAR5")
        except TimeoutError:
            print("FAIL: SeaBIOS did not program BAR5 / msix-table")
            return 2

        # --- Frame round 1: vector 0, the first one the driver uses ---
        client.sendall(frame(IMSIX_DB_OFFSET, 0x0000))
        # --- Frame round 2: vector 7, TC=1, PF=0 ---
        client.sendall(frame(IMSIX_DB_OFFSET, (1 << 12) | 7))
        # --- Frame round 3: highest valid vector (R100_NUM_MSIX - 1) ---
        client.sendall(frame(IMSIX_DB_OFFSET, NUM_MSIX - 1))
        # --- Frame round 4: out-of-range vector → rejected (oor) ---
        client.sendall(frame(IMSIX_DB_OFFSET, NUM_MSIX))
        # --- Frame round 5: wrong offset → rejected (bad-offset) ---
        client.sendall(frame(BAD_OFFSET, 0x0001))

        def _log_has_five():
            if not MSIX_LOG.exists():
                return False
            return MSIX_LOG.read_text().count("msix off=") >= 5
        wait_for(_log_has_five, timeout=10.0,
                 desc="msix.log to show 5 frames")

        log_text = MSIX_LOG.read_text()
        # Full expected lines — exact formatting matches
        # r100_msix_emit_debug() in src/host/r100_npu_pci.c.
        expected = [
            "msix off=0xffc db_data=0x0 vector=0 status=ok count=1",
            "msix off=0xffc db_data=0x1007 vector=7 status=ok count=2",
            "msix off=0xffc db_data=0x1f vector=31 status=ok count=3",
            "msix off=0xffc db_data=0x20 vector=32 status=oor count=3",
            "msix off=0x100 db_data=0x1 vector=1 status=bad-offset count=3",
        ]
        missing = [ln for ln in expected if ln not in log_text]
        if missing:
            print("FAIL: missing debug lines:")
            for m in missing:
                print("  " + repr(m))
            print("--- actual msix.log ---")
            print(log_text)
            return 3

        # The two rejected frames must each leave a GUEST_ERROR trail.
        def _qemu_log_has_errors():
            if not HOST_QEMU_LOG.exists():
                return False
            text = HOST_QEMU_LOG.read_text()
            return ("vector 32 out of range" in text
                    and "unexpected off=0x100" in text)
        try:
            wait_for(_qemu_log_has_errors, timeout=5.0,
                     desc="GUEST_ERROR entries for rejected frames")
        except TimeoutError:
            print("FAIL: rejected frames did not leave GUEST_ERROR "
                  "entries in qemu.log")
            if HOST_QEMU_LOG.exists():
                print("--- host qemu.log tail ---")
                print(HOST_QEMU_LOG.read_text()[-2000:])
            return 4

        print("PASS: M7 iMSIX-DB plumbing end-to-end")
        print("  run dir:       %s" % RUN_DIR)
        print("  frames sent:   5 (3 accepted, 2 rejected)")
        print("  debug log:     %s" % MSIX_LOG)
        print("  guest errors:  %s" % HOST_QEMU_LOG)
        for ln in log_text.strip().splitlines():
            print("    " + ln)
        return 0

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
            MSIX_SOCK.unlink()
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
