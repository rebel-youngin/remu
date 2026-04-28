#!/usr/bin/env python3
"""P5 r100-hdma linked-list-mode end-to-end check.

End-to-end shape (mirrors tests/p4b_rbdma_oto_test.py):

  1. Boot `./remucli run --host --name p5-hdma`. We use `--host` so
     the chiplet-0 DRAM head is spliced over a shared `/dev/shm/...`
     file and so r100-hdma is actually instantiated (the device only
     wires up when its `hdma` chardev is present).

  2. Wait for q-cp to finish booting, then mmap the shm and seed:
        - DESC_OFFSET (0x07900000): a single dw_hdma_v0_lli {ctrl=CB|LIE,
          transfer_size=4KB, sar=0x07000000, dar=0x07800000} followed
          by a record_llp(0,0) terminator (matches q-cp's
          dev_hdma_write_desc shape exactly).
        - SRC_OFFSET (0x07000000): a pseudo-random 4 KB pattern.
        - DST_OFFSET (0x07800000): zero-cleared landing zone.

     Both SAR and DAR are NPU-local DRAM offsets. The walker's
     SAR/DAR-vs-REMU_HOST_PHYS_BASE classifier therefore routes this
     LLI through r100_hdma_lli_d2d (direct address_space_read →
     address_space_write), which exercises the LL chain decode +
     control-bit walking without depending on a live host BAR.

     For the host-leg paths (dir=WR && DAR>=host; dir=RD && SAR>=host)
     coverage falls out of P10's umd `simple_copy` once that lands —
     they share the same walker plus a one-line address_space →
     OP_WRITE / OP_READ_REQ swap.

  3. Drive the doorbell sequence via QEMU's gdbstub in physical-memory
     mode (`Qqemu.PhyMemMode:1`):
        CTRL1   = LLEN
        LLP_LO  = (DESC_OFFSET) & 0xFFFFFFFF
        LLP_HI  = (DESC_OFFSET) >> 32
        ENABLE  = 1
        DOORBELL = HDMA_DB_START

     The kick handler runs synchronously inside the doorbell write,
     so the byte move completes before the M-packet returns.

  4. Read back DST_OFFSET via the same shm mmap. Assert byte-for-byte
     equality with the seeded SRC pattern.

The test exits 0 on PASS, 2 on byte mismatch, 1 on infrastructure
failure. Tail of `output/p5-hdma.log` is dumped on early exit.

SMMU note: like P4B, the SAR/DAR fields here are written as raw
chiplet-local DRAM offsets — the engine treats them as device PAs
under SMMU bypass (HDMA-SID LUT all-bypass; see
docs/hdma-notion-notes.md § 4 and docs/roadmap.md → "SMMU honour FW
page tables").
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
RUN_NAME = "p5-hdma"
RUN_DIR = REPO / "output" / RUN_NAME
SHM_PATH = Path("/dev/shm/remu-" + RUN_NAME) / "remu-shm"

# Chiplet-0 r100-hdma register block (R100_HDMA_BASE in
# src/include/r100/remu_addrmap.h:
#   U_PCIE_CORE_OFFSET (0x1C00000000) + PCIE_HDMA_OFFSET (0x180380000)).
HDMA_BASE = 0x1C00000000 + 0x180380000

# Per-channel stride (ch_sep=3 → 0x800 per slot, slots interleaved
# WR/RD per channel per HDMA_REG_{WR,RD}_CH_OFFSET in q-cp's hdma_if.c).
CH_STRIDE = 0x800
WR_CH0_BASE = HDMA_BASE + 0 * CH_STRIDE        # slot 0
# Per-channel register offsets (R100_HDMA_CH_REG_*).
ENABLE_OFF   = 0x00
DOORBELL_OFF = 0x04
LLP_LO_OFF   = 0x10
LLP_HI_OFF   = 0x14
CTRL1_OFF    = 0x34

CTRL1_LLEN_BIT = 1 << 0
DB_START_BIT   = 1 << 0
ENABLE_BIT     = 1 << 0

# DRAM offsets within the chiplet-0 shm splice (first 128 MB). Layout
# matches P4B (src/dst at 0x07000000 / 0x07800000) plus an LLI / LLP
# region at 0x07900000. All sit comfortably above FreeRTOS_CP0
# (~0x00200000, 2.2 MB) and below BL31_CP1 @ 0x14100000.
SRC_DRAM_OFFSET  = 0x07000000
DST_DRAM_OFFSET  = 0x07800000
DESC_DRAM_OFFSET = 0x07900000
COPY_SIZE = 0x1000

GDB_PORT = 4568   # disjoint from p4b's 4567 in case of overlap


# --------------------------------------------------------------------------
# HMP helper. Same wire-pattern as tests/p4b_rbdma_oto_test.py.
# --------------------------------------------------------------------------

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


# --------------------------------------------------------------------------
# Minimal GDB Remote Serial Protocol client (copy from p4b — small enough
# to dup, and the two tests can run in parallel without coupling).
# --------------------------------------------------------------------------

class GDBStub:
    def __init__(self, host, port, timeout=10.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self._buf = b""

    @staticmethod
    def _csum(body: bytes) -> bytes:
        return ("%02x" % (sum(body) & 0xFF)).encode()

    def _read_byte(self) -> bytes:
        if not self._buf:
            self._buf = self.s.recv(4096)
            if not self._buf:
                raise EOFError("gdbstub closed")
        ch, self._buf = self._buf[:1], self._buf[1:]
        return ch

    def _send(self, body: bytes):
        pkt = b"$" + body + b"#" + self._csum(body)
        self.s.sendall(pkt)
        for _ in range(8):
            ch = self._read_byte()
            if ch == b"+":
                return
            if ch == b"-":
                self.s.sendall(pkt)
                continue
        raise RuntimeError("no '+' ack from gdbstub for %r" % body)

    def _recv(self) -> bytes:
        while True:
            ch = self._read_byte()
            if ch == b"$":
                break
        body = b""
        while True:
            ch = self._read_byte()
            if ch == b"#":
                break
            body += ch
        self._read_byte()
        self._read_byte()
        self.s.sendall(b"+")
        return body

    def cmd(self, body: bytes) -> bytes:
        self._send(body)
        return self._recv()

    def set_phy_mem_mode(self, on: bool = True):
        body = b"Qqemu.PhyMemMode:" + (b"1" if on else b"0")
        resp = self.cmd(body)
        if resp != b"OK":
            raise RuntimeError("Qqemu.PhyMemMode failed: %r" % resp)

    def write_word(self, addr: int, value: int):
        data_hex = struct.pack("<I", value & 0xFFFFFFFF).hex().encode()
        body = ("M%x,4:" % addr).encode() + data_hex
        resp = self.cmd(body)
        if resp != b"OK":
            raise RuntimeError(
                "M @ 0x%x = 0x%08x failed: %r" % (addr, value, resp))

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# Test driver
# --------------------------------------------------------------------------

# dw_hdma_v0 control bits (qman_if_common.h).
DW_HDMA_V0_CB  = 1 << 0
DW_HDMA_V0_LLP = 1 << 2
DW_HDMA_V0_LIE = 1 << 3


def boot_remu(log_path):
    proc = subprocess.Popen(
        [str(REPO / "remucli"), "run", "--host", "--name", RUN_NAME],
        stdout=open(log_path, "wb"), stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        start_new_session=True,
    )
    return proc


def wait_for_monitors(proc, npu_mon, host_mon, log_path, timeout=30.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if npu_mon.exists() and host_mon.exists():
            return True
        if proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def make_pattern(size):
    # Distinct from p4b's pattern (offset constants differ) so a
    # mis-targeted test can't pass by accident.
    return bytes(((i * 53 + 7) ^ ((i >> 5) * 199)) & 0xFF
                 for i in range(size))


def build_lli_chain(src_pa, dst_pa, size):
    """Return a 40-byte buffer: one dw_hdma_v0_lli followed by a
    dw_hdma_v0_llp(0, 0) terminator. Layout per qman_if_common.h:

      dw_hdma_v0_lli (24 B):
        u32 control
        u32 transfer_size
        u64 sar.reg
        u64 dar.reg

      dw_hdma_v0_llp (16 B):
        u32 control
        u32 reserved
        u64 llp.reg
    """
    lli = struct.pack(
        "<II QQ",
        DW_HDMA_V0_CB | DW_HDMA_V0_LIE,
        size,
        src_pa,
        dst_pa,
    )
    llp = struct.pack("<II Q", 0, 0, 0)
    return lli + llp


def shm_seed(pattern, chain):
    if not SHM_PATH.exists():
        raise RuntimeError("shm file %s not found" % SHM_PATH)
    needed = max(SRC_DRAM_OFFSET, DST_DRAM_OFFSET,
                 DESC_DRAM_OFFSET) + max(COPY_SIZE, len(chain))
    size = SHM_PATH.stat().st_size
    if size < needed:
        raise RuntimeError(
            "shm too small: %d < %d (need src/dst/desc offsets)"
            % (size, needed))
    with open(SHM_PATH, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)
        try:
            mm[SRC_DRAM_OFFSET:SRC_DRAM_OFFSET + COPY_SIZE] = pattern
            mm[DST_DRAM_OFFSET:DST_DRAM_OFFSET + COPY_SIZE] = (
                b"\x00" * COPY_SIZE)
            mm[DESC_DRAM_OFFSET:DESC_DRAM_OFFSET + len(chain)] = chain
            mm.flush()
        finally:
            mm.close()


def shm_read_dst():
    with open(SHM_PATH, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
        try:
            return bytes(mm[DST_DRAM_OFFSET:DST_DRAM_OFFSET + COPY_SIZE])
        finally:
            mm.close()


def drive_hdma_ll(stub):
    # Programming order matches q-cp's hdma_ch_trigger:
    #   ctrl1 |= LLEN; func_num; llp_lo; llp_hi; enable; doorbell.
    # We skip func_num (PF=0 is the reset state).
    stub.write_word(WR_CH0_BASE + CTRL1_OFF, CTRL1_LLEN_BIT)
    stub.write_word(WR_CH0_BASE + LLP_LO_OFF, DESC_DRAM_OFFSET & 0xFFFFFFFF)
    stub.write_word(WR_CH0_BASE + LLP_HI_OFF, (DESC_DRAM_OFFSET >> 32) & 0xFFFFFFFF)
    stub.write_word(WR_CH0_BASE + ENABLE_OFF, ENABLE_BIT)
    stub.write_word(WR_CH0_BASE + DOORBELL_OFF, DB_START_BIT)


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


def first_diff(got, expected, ctx=8):
    for i, (a, b) in enumerate(zip(got, expected)):
        if a != b:
            lo = max(0, i - ctx)
            hi = min(len(got), i + ctx + 1)
            return ("byte %d: got 0x%02x exp 0x%02x; "
                    "got[%d..%d]=%s; exp[%d..%d]=%s"
                    % (i, a, b,
                       lo, hi, got[lo:hi].hex(),
                       lo, hi, expected[lo:hi].hex()))
    if len(got) != len(expected):
        return "length mismatch: got %d, exp %d" % (len(got),
                                                    len(expected))
    return None


def main():
    os.environ["PYTHONUNBUFFERED"] = "1"

    log_path = RUN_DIR.with_suffix(".log")
    log_path.parent.mkdir(parents=True, exist_ok=True)

    proc = boot_remu(log_path)
    npu_mon = RUN_DIR / "npu" / "monitor.sock"
    host_mon = RUN_DIR / "host" / "monitor.sock"

    try:
        if not wait_for_monitors(proc, npu_mon, host_mon, log_path):
            print("monitor sockets never came up")
            tail = log_path.read_text(errors="replace")[-2000:]
            print(tail)
            return 1

        # Same settle window as p4b — q-cp's BL31 → FreeRTOS → idle
        # is empirically <4 s on our build host.
        time.sleep(4.0)

        pattern = make_pattern(COPY_SIZE)
        chain = build_lli_chain(SRC_DRAM_OFFSET, DST_DRAM_OFFSET,
                                COPY_SIZE)
        shm_seed(pattern, chain)

        hmp(npu_mon, "gdbserver tcp::%d" % GDB_PORT)

        stub = None
        for _ in range(50):
            try:
                stub = GDBStub("127.0.0.1", GDB_PORT)
                break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.1)
        if stub is None:
            print("gdbstub never came up on tcp::%d" % GDB_PORT)
            return 1

        try:
            stub.set_phy_mem_mode(True)
            drive_hdma_ll(stub)
        finally:
            stub.close()

        # The walk runs synchronously inside the doorbell write
        # handler — the M packet has already returned by the time we
        # close the gdbstub. The 0.2 s sleep is a belt for any
        # deferred BH work (none on the D2D path today).
        time.sleep(0.2)

        got = shm_read_dst()
        diff = first_diff(got, pattern)
        if diff is not None:
            print("FAIL: dst != src — %s" % diff)
            return 2

        print("PASS: %d B copied via HDMA LL D2D "
              "(src=0x%08x → dst=0x%08x via desc=0x%08x)"
              % (COPY_SIZE, SRC_DRAM_OFFSET, DST_DRAM_OFFSET,
                 DESC_DRAM_OFFSET))
        return 0

    finally:
        teardown(proc)


if __name__ == "__main__":
    sys.exit(main())
