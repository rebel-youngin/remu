#!/usr/bin/env python3
"""P4B r100-rbdma OTO byte-mover end-to-end check.

End-to-end shape:

  1. Boot `./remucli run --host --name p4b-rbdma`. We use `--host` so
     the chiplet-0 DRAM head is spliced over a shared `/dev/shm/...`
     file — that's the cheapest way to pre-fill SRC and read DST from
     the test process (we are NOT going through the kmd / umd path,
     this is a unit test of the RBDMA byte mover).

  2. Wait for q-cp to finish booting (NPU monitor socket up + a small
     settle-time). Then mmap the shm and seed:
        - SRC_OFFSET (0x07000000): a pseudo-random 4 KB pattern.
        - DST_OFFSET (0x07800000): zero-cleared landing zone.

  3. Drive a synthetic q-cp `rbdma_send_task → send_td` burst against
     chiplet-0 RBDMA via QEMU's gdbstub in physical-memory mode. The
     gdbstub is started on demand via the HMP `gdbserver tcp::PORT`
     command, so we don't need any extra `--gdb*` flag at run start
     and the VM stays running. The 6 register stores — PTID_INIT,
     SRCADDRESS_OR_CONST, DESTADDRESS, SIZEOF128BLOCK, RUN_CONF0,
     RUN_CONF1 — replicate exactly what
     `external/.../q/cp/.../hal/rbdma/rebel/rbdma_if.c:send_td` writes
     for an OTO task, with `intr_disable=1` so the synthetic kick
     doesn't fire a stray SPI into a q-cp `rbdma_done_handler` that
     has no matching outstanding cb (we'd just see GUEST_ERROR noise;
     the byte move itself is independent of the IRQ path).

  4. Read back DST_OFFSET via the same shm mmap. Assert byte-for-byte
     equality with the seeded SRC pattern.

Address-translation note: on real silicon SAR / DAR are device virtual
addresses (DVAs) translated by the per-chiplet SMMU-600 (S1 + S2 page
walks) before reaching DDR. P11 made `r100-rbdma` honour stage-2 via
`r100_smmu_translate(SID=0, …)` before each `address_space_*` call,
but this test deliberately operates in the **CR0.SMMUEN=0** regime:
q-sys's `m7_smmu_enable` only fires from a kmd-driven
`dram_init_done_cb` mailbox callback, which this gdbstub-driven
harness never triggers. So the SMMU walker stays in pre-enable
identity mode and raw chiplet-local DRAM offsets (0x07000000 /
0x07800000) land on the correct memory unchanged. The companion
`tests/p11_smmu_walk_test.py` is the focused stage-2-walk unit test
that programs SMMU registers and stages page tables explicitly.

The test exits 0 on PASS, 2 on byte mismatch, 1 on infrastructure
failure. Tail of `output/p4b-rbdma.log` is dumped on early exit so a
broken q-cp boot doesn't silently look like a P4B regression.

Once P5 lands and the umd command_buffer integration test boots
under guest Linux, this HMP-driven harness becomes redundant — but
it stays useful as a focused, fast-feedback unit test for the RBDMA
byte mover (no UMD / kmd / x86 boot stack on the critical path).
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
RUN_NAME = "p4b-rbdma"
RUN_DIR = REPO / "output" / RUN_NAME
SHM_PATH = Path("/dev/shm/remu-" + RUN_NAME) / "remu-shm"

# Chiplet-0 RBDMA register block (NBUS_L_RBDMA_CFG_BASE + offsets, see
# external/.../q/cp/src/hal/autogen/rebel/g_cdma_task_registers.h). The
# task descriptor sits at +0x200..+0x21C.
RBDMA_BASE = 0x1FF3700000
PTID_INIT_REG = RBDMA_BASE + 0x200
SRCADDR_REG = RBDMA_BASE + 0x204
DESTADDR_REG = RBDMA_BASE + 0x208
SIZE_REG = RBDMA_BASE + 0x20C
RUN_CONF0_REG = RBDMA_BASE + 0x218
RUN_CONF1_REG = RBDMA_BASE + 0x21C

RUN_CONF1_INTR_DISABLE = 1 << 0

# DRAM offsets within the chiplet-0 shm splice (full DRAM, 36 GB =
# R100_RBLN_DRAM_SIZE; previously 128 MB before the silicon-accurate
# bump). Both 0x07000000 and 0x07800000 sit comfortably above the
# FreeRTOS_CP0 image (loaded at 0x00200000, ~2.2 MB) and below the
# next FW slot (BL31_CP1 @ 0x14100000), well clear of q-cp's
# high-DRAM SHM_BASE (~63 GB on quad). 4 KB == 32 × 128 B blocks
# satisfies the RBDMA block-aligned size encoding.
SRC_DRAM_OFFSET = 0x07000000
DST_DRAM_OFFSET = 0x07800000
COPY_SIZE = 0x1000

GDB_PORT = 4567


# --------------------------------------------------------------------------
# HMP helper. Same wire-pattern as tests/p4a_rbdma_stub_test.py — a
# blocking unix-socket round trip with prompt sniffing.
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
# Minimal GDB Remote Serial Protocol client. We only need three
# operations: switch to physical-memory mode (`Qqemu.PhyMemMode:1`),
# write a 32-bit word (`M<addr>,4:<hex>`), and (optionally) read a
# 32-bit word (`m<addr>,4`). Packets are `$<body>#<csum>`; gdbstub
# acks every well-formed packet with `+`. We send `+` back for each
# response packet so the gdbstub doesn't re-transmit.
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
        # Wait for '+' ack. Skip a leading '-' (request retransmit)
        # plus stray bytes that may show up between packets.
        for _ in range(8):
            ch = self._read_byte()
            if ch == b"+":
                return
            if ch == b"-":
                self.s.sendall(pkt)
                continue
        raise RuntimeError("no '+' ack from gdbstub for %r" % body)

    def _recv(self) -> bytes:
        # Consume up to '$', then payload up to '#', then 2 csum bytes.
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

    def read_word(self, addr: int) -> int:
        body = ("m%x,4" % addr).encode()
        resp = self.cmd(body)
        if len(resp) != 8 or not all(c in b"0123456789abcdefABCDEF"
                                     for c in resp):
            raise RuntimeError(
                "m @ 0x%x bad response: %r" % (addr, resp))
        return struct.unpack("<I", bytes.fromhex(resp.decode()))[0]

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# Test driver
# --------------------------------------------------------------------------

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
    # Deterministic non-trivial pattern. Avoid 0x00 / 0xFF runs so a
    # partial copy / wrong endianness shows up immediately.
    return bytes(((i * 37 + 13) ^ ((i >> 7) * 91)) & 0xFF
                 for i in range(size))


def shm_seed_and_zero(pattern):
    if not SHM_PATH.exists():
        raise RuntimeError("shm file %s not found" % SHM_PATH)
    needed = max(SRC_DRAM_OFFSET, DST_DRAM_OFFSET) + COPY_SIZE
    size = SHM_PATH.stat().st_size
    if size < needed:
        raise RuntimeError(
            "shm too small: %d < %d (need both src/dst offsets)"
            % (size, needed))
    with open(SHM_PATH, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)
        try:
            mm[SRC_DRAM_OFFSET:SRC_DRAM_OFFSET + COPY_SIZE] = pattern
            mm[DST_DRAM_OFFSET:DST_DRAM_OFFSET + COPY_SIZE] = (
                b"\x00" * COPY_SIZE)
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


def drive_rbdma_oto(stub):
    # 128 B granularity. Both offsets fit in 32 bits with addr_msb=0.
    src_128 = (SRC_DRAM_OFFSET >> 7) & 0xFFFFFFFF
    dst_128 = (DST_DRAM_OFFSET >> 7) & 0xFFFFFFFF
    blk_128 = (COPY_SIZE >> 7) & 0xFFFFFFFF

    # task_type=OTO=0, src_addr_msb=0, dst_addr_msb=0; the rest left
    # zero (split_granule_l2, ext_num_of_chunk, fid_max — irrelevant
    # to the byte-move stub).
    run_conf0 = 0
    # intr_disable=1: we don't have a q-cp cb registered for this
    # synthetic kick, so suppress the SPI to avoid a GUEST_ERROR
    # rabbit-hole in the rbdma_done_handler match path.
    run_conf1 = RUN_CONF1_INTR_DISABLE

    # PTID_INIT carries q-cp's task->id_info — arbitrary marker for
    # the (suppressed) FNSH FIFO entry; pick something visible in
    # post-mortem regs.
    ptid = 0xC0FFEE00

    stub.write_word(PTID_INIT_REG, ptid)
    stub.write_word(SRCADDR_REG, src_128)
    stub.write_word(DESTADDR_REG, dst_128)
    stub.write_word(SIZE_REG, blk_128)
    stub.write_word(RUN_CONF0_REG, run_conf0)
    stub.write_word(RUN_CONF1_REG, run_conf1)


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

        # Let q-cp's BL31 → FreeRTOS → idle settle. 4 s is empirically
        # enough on our build host for `rbdma_init` and the cb worker
        # task to park; bumping to 8 s is harmless for retries.
        time.sleep(4.0)

        pattern = make_pattern(COPY_SIZE)
        shm_seed_and_zero(pattern)

        # Spawn gdbstub on demand. tcp:: prefix gets `wait=off`
        # appended by gdbserver_start, so the VM stays running and we
        # can connect any time.
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
            drive_rbdma_oto(stub)
        finally:
            stub.close()

        # The byte move runs synchronously inside the RUN_CONF1 store
        # handler (see r100_rbdma_kickoff). The 0.2 s sleep is a belt
        # for any deferred BH work — not strictly required for the
        # byte assertion below.
        time.sleep(0.2)

        got = shm_read_dst()
        diff = first_diff(got, pattern)
        if diff is not None:
            print("FAIL: dst != src — %s" % diff)
            return 2

        print("PASS: %d B copied via RBDMA OTO "
              "(src=0x%08x → dst=0x%08x)"
              % (COPY_SIZE, SRC_DRAM_OFFSET, DST_DRAM_OFFSET))
        return 0

    finally:
        teardown(proc)


if __name__ == "__main__":
    sys.exit(main())
