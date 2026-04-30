#!/usr/bin/env python3
"""P11b SMMU eventq + GERROR fault-delivery test.

Pairs with `tests/p11_smmu_walk_test.py`. Where p11 verifies the
*successful* stage-2 walk (RBDMA OTO with valid IPAs → bytes land at
PA), p11b deliberately programs an INVALID STE so the translate path
takes the fault arm and lands an event on the in-DRAM event queue.

Shape (mirrors p11):

  1. Boot `./remucli run --host --name p11b-evtq` so the chiplet-0
     DRAM head is spliced over `/dev/shm/remu-...` for direct seeding.

  2. Wait for q-cp to settle (4 s — same as p11).

  3. Seed chiplet-0 DRAM via shm mmap:
       - **Bad STE** at 0x06010000 with `STE0.V=0` (config + stage-2
         fields irrelevant). Whatever SID 0 the engine presents,
         `r100_smmu_translate` short-circuits at the validity check
         and emits an `INV_STE` fault (FW event_id 0x04 = C_BAD_STE).
       - 32 KB **eventq** at 0x06020000 (1024 entries × 32 B), cleared.
       - SRC/DST at 0x07000000 / 0x07800000 — same DVAs we'll feed
         RBDMA so the input_addr in the event is observable.

  4. Drive SMMU MMIO via QEMU gdbstub:
       - STRTAB_BASE = 0x06010000, log2size=1, fmt=LINEAR.
       - EVENTQ_BASE = 0x06020000 | log2size=10. Combined value
         encodes both PA (bits[51:5]) and queue size (bits[4:0]).
       - IRQ_CTRL = EVENTQ_IRQEN | GERROR_IRQEN (must precede
         CR0 enable so r100-smmu's emit path pulses SPI 762 on
         the first fault).
       - CR0 = SMMUEN | EVENTQEN. Order matters: EVENTQEN must
         be set before the engine kicks, or `r100_smmu_emit_event`
         drops with `events_dropped++` instead of writing the slot.

  5. Drive a chiplet-0 RBDMA OTO with SAR=IPA_SRC, DAR=IPA_DST. The
     kick path's `r100_rbdma_translate` calls `r100_smmu_translate`,
     which reads our V=0 STE and bails with `R100_SMMU_FAULT_INV_STE`.
     The fault arm in `r100_smmu_translate` calls
     `r100_smmu_emit_event(0x04, sid=0, input_addr=IPA_SRC)`, which
     writes 32 B to slot 0 of our eventq, advances PROD to 1, and
     pulses evt_irq if IRQ_CTRL.EVENTQ_IRQEN=1.

  6. Verify two observables:
       a) **Eventq slot 0 content** (read via shm mmap): evt[0]&0xff =
          0x04 (C_BAD_STE), evt[1] = 0 (sid), evt[4..5] = IPA_SRC.
       b) **EVENTQ_PROD = 1** (read via gdbstub `m` packet against
          SMMU MMIO at TCU + 0x100A8). Confirms the SMMU's index
          advance is visible at the MMIO surface FW polls.

We do **not** assert FW dequeues the event (`smmu_event_intr` →
`smmu_evtq_dequeue` → CONS bump): FW maintains its own
`smmu_evtq.paddr` (CMDQ_BASE-style local cache from `smmu_init_queue`)
that points at FW's eventq, not ours. When we override EVENTQ_BASE in
MMIO, FW's IRQ handler still dereferences its own cached paddr — so
its smmu_print_event reads garbage. Verifying SPI 762 actually fires
is best done via a future `info qtree` extension or by asserting
GIC pending state; the emit + PROD-bump assertions above already
prove the wire crosses correctly inside r100-smmu, which is the v2
delta we're verifying.

Two RBDMA kicks land two events (same fault), so a separate kick at
the end with a *valid* OTO that DOES translate cleanly serves as
a sanity check that the fault path didn't poison the engine. Skip
this if the IPA contents would race; the p11 test already covers
the success path independently.

Exit: 0 on PASS, 2 on assertion mismatch, 1 on infrastructure failure.
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
RUN_NAME = os.environ.get("REMU_RUN_NAME", "p11b-evtq")
RUN_DIR = REPO / "output" / RUN_NAME
SHM_PATH = Path("/dev/shm/remu-" + RUN_NAME) / "remu-shm"

# Chiplet-0 SMMU MMIO. R100_SMMU_TCU_BASE = 0x1FF4200000.
SMMU_BASE = 0x1FF4200000
SMMU_CR0 = SMMU_BASE + 0x20
SMMU_IRQ_CTRL = SMMU_BASE + 0x50
SMMU_STRTAB_BASE_LO = SMMU_BASE + 0x80
SMMU_STRTAB_BASE_HI = SMMU_BASE + 0x84
SMMU_STRTAB_BASE_CFG = SMMU_BASE + 0x88
SMMU_EVENTQ_BASE_LO = SMMU_BASE + 0xA0
SMMU_EVENTQ_BASE_HI = SMMU_BASE + 0xA4
# EVENTQ_PROD/CONS sit at MMIO page 1 above (offset 0x100A8/AC). The
# r100-smmu device size was bumped from 0x10000 to 0x20000 in this
# milestone to expose them to FW + tests.
SMMU_EVENTQ_PROD = SMMU_BASE + 0x100A8
SMMU_EVENTQ_CONS = SMMU_BASE + 0x100AC

SMMU_CR0_SMMUEN = 1 << 0
SMMU_CR0_EVENTQEN = 1 << 2
SMMU_IRQ_CTRL_GERROR_IRQEN = 1 << 0
SMMU_IRQ_CTRL_EVENTQ_IRQEN = 1 << 2

# Chiplet-0 RBDMA MMIO — same offsets as p4b/p11.
RBDMA_BASE = 0x1FF3700000
PTID_INIT_REG = RBDMA_BASE + 0x200
SRCADDR_REG = RBDMA_BASE + 0x204
DESTADDR_REG = RBDMA_BASE + 0x208
SIZE_REG = RBDMA_BASE + 0x20C
RUN_CONF0_REG = RBDMA_BASE + 0x218
RUN_CONF1_REG = RBDMA_BASE + 0x21C
RUN_CONF1_INTR_DISABLE = 1 << 0

# DRAM offsets (chiplet 0, in shm splice). Avoid p11's regions so a
# concurrent run wouldn't collide if test sequencing breaks down.
STE_TABLE_PA = 0x06010000
EVENTQ_TABLE_PA = 0x06020000
EVENTQ_LOG2SIZE = 10                              # 1024 entries
EVENTQ_BYTES = (1 << EVENTQ_LOG2SIZE) * 32        # 32 KB

# DVAs. Both arbitrary — the V=0 STE bails before stage-2 fields are
# touched, so neither IPA needs page-table backing. Picking values
# distinct from chiplet-local PAs (bit-32 set) keeps "did the SMMU
# actually fault?" unambiguous in the eventq input_addr decode.
IPA_SRC = 0x100000000
IPA_DST = 0x100001000
COPY_SIZE = 0x1000

# Avoid colliding with p4b (4567) / p5 (4568) / p11 (4569).
GDB_PORT = 4570

# FW event_id = 0x04 (C_BAD_STE). See q-sys events[] in
# `q/sys/drivers/smmu/smmu.c:165` — `r100_smmu_fault_to_event_id` in
# `src/machine/r100_smmu.c` maps R100_SMMU_FAULT_INV_STE to this.
EXPECTED_EVENT_ID = 0x04
EXPECTED_SID = 0


# --------------------------------------------------------------------------
# HMP helper. Same wire-pattern as tests/p11_smmu_walk_test.py.
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
# GDB Remote Serial Protocol client. Subset matching p11; adds read_word
# (`m<addr>,4`) so the test can poll EVENTQ_PROD post-kick without
# spinning up a separate HMP `xp` round-trip per check.
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

    def read_word(self, addr: int) -> int:
        body = ("m%x,4" % addr).encode()
        resp = self.cmd(body)
        if not resp or len(resp) != 8:
            raise RuntimeError("m @ 0x%x → unexpected resp %r" % (addr, resp))
        return struct.unpack("<I", bytes.fromhex(resp.decode()))[0]

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# Test driver.
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


def shm_seed_bad_ste_and_eventq():
    """Plant a V=0 STE at SID 0 and zero the eventq region. Both live in
    chiplet-0 DRAM via the shm splice."""
    if not SHM_PATH.exists():
        raise RuntimeError("shm file %s not found" % SHM_PATH)
    needed = EVENTQ_TABLE_PA + EVENTQ_BYTES
    size = SHM_PATH.stat().st_size
    if size < needed:
        raise RuntimeError("shm too small: %d < %d" % (size, needed))
    with open(SHM_PATH, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)
        try:
            # STE table — zero a page, leave SID 0 as V=0. The full 64 B
            # entry stays zeroed; r100_smmu_translate's V check (STE0
            # bit 0) trips first, no other fields matter.
            mm[STE_TABLE_PA:STE_TABLE_PA + 4096] = b"\x00" * 4096
            # Eventq — zero so post-emit slot 0 contents are
            # unambiguously what we wrote (no leftover from a previous
            # run; ./remucli run --name auto-cleans before each test
            # but belt-and-braces).
            mm[EVENTQ_TABLE_PA:EVENTQ_TABLE_PA + EVENTQ_BYTES] = (
                b"\x00" * EVENTQ_BYTES)
            mm.flush()
        finally:
            mm.close()


def shm_read_event_slot(idx):
    """Read the 32 B event record at eventq slot `idx`. Returns 8
    little-endian uint32_t as a tuple."""
    with open(SHM_PATH, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
        try:
            off = EVENTQ_TABLE_PA + idx * 32
            return struct.unpack("<8I", bytes(mm[off:off + 32]))
        finally:
            mm.close()


def configure_smmu(stub):
    """Override q-cp's SMMU state on chiplet 0 with our test fixtures.

    Order matters: program STRTAB + EVENTQ pointers and IRQ_CTRL
    *before* enabling CR0 — once SMMUEN is high, any speculative
    translate from another path (e.g. a stray HDMA/RBDMA touch) would
    use whatever (possibly stale) state was latched. CR0 last seals
    everything.
    """
    # STRTAB → our V=0 STE table, log2size=1 (room for SID 0 + the
    # unused 1 to satisfy the >=1<<log2size SID-bounds check), LINEAR.
    stub.write_word(SMMU_STRTAB_BASE_LO, STE_TABLE_PA & 0xFFFFFFFF)
    stub.write_word(SMMU_STRTAB_BASE_HI, (STE_TABLE_PA >> 32) & 0xFFFFFFFF)
    stub.write_word(SMMU_STRTAB_BASE_CFG, 1)

    # EVENTQ_BASE encodes PA in bits[51:5] | log2size in bits[4:0].
    # Lower 5 bits of LO carry log2size; upper bits carry the PA. The
    # split mirrors q-sys's smmu_init_queue: `q->base = paddr |
    # (log2size << Q_LOG2SIZE_S)`.
    eventq_lo = (EVENTQ_TABLE_PA & 0xFFFFFFFF) | EVENTQ_LOG2SIZE
    eventq_hi = (EVENTQ_TABLE_PA >> 32) & 0xFFFFFFFF
    stub.write_word(SMMU_EVENTQ_BASE_LO, eventq_lo)
    stub.write_word(SMMU_EVENTQ_BASE_HI, eventq_hi)

    # IRQ_CTRL — enable both wired SPIs. r100_smmu_emit_event /
    # r100_smmu_raise_gerror gate qemu_irq_pulse on these bits.
    stub.write_word(SMMU_IRQ_CTRL,
                    SMMU_IRQ_CTRL_EVENTQ_IRQEN | SMMU_IRQ_CTRL_GERROR_IRQEN)

    # Reset PROD/CONS to a known state. FW likely already wrote 0 to
    # both during smmu_enable_queues, but explicit zeros eliminate any
    # latent off-by-one from a partially-completed init.
    stub.write_word(SMMU_EVENTQ_PROD, 0)
    stub.write_word(SMMU_EVENTQ_CONS, 0)

    # CR0 last — flips the walker live + opens the eventq pipe.
    stub.write_word(SMMU_CR0, SMMU_CR0_SMMUEN | SMMU_CR0_EVENTQEN)


def drive_rbdma_oto(stub, src_dva, dst_dva, size_bytes):
    """Issue one RBDMA OTO kick. Same shape as p11."""
    src_128 = src_dva >> 7
    dst_128 = dst_dva >> 7
    blk_128 = size_bytes >> 7

    src_lo = src_128 & 0xFFFFFFFF
    src_msb = (src_128 >> 32) & 0x3
    dst_lo = dst_128 & 0xFFFFFFFF
    dst_msb = (dst_128 >> 32) & 0x3

    run_conf0 = (src_msb << 20) | (dst_msb << 22)
    run_conf1 = RUN_CONF1_INTR_DISABLE

    ptid = 0xFA17ED01  # marker — visible in the engine's regstore.

    stub.write_word(PTID_INIT_REG, ptid)
    stub.write_word(SRCADDR_REG, src_lo)
    stub.write_word(DESTADDR_REG, dst_lo)
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


def main():
    os.environ["PYTHONUNBUFFERED"] = "1"

    log_path = RUN_DIR / "run.log"
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

        # 4 s settle — same as p11. Lets q-cp's smmu_init complete +
        # park in notify_dram_init_done; FW will eventually time-out
        # and call smmu_enable() itself, which is harmless because
        # configure_smmu below clobbers CR0/STRTAB/EVENTQ to our
        # fixtures.
        time.sleep(4.0)

        shm_seed_bad_ste_and_eventq()

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
            configure_smmu(stub)

            # Pre-kick state — PROD must be 0 before the OTO kick. If
            # CR0.SMMUEN was already on with a stale STRTAB and an
            # earlier translate already faulted, the asserts below
            # would point at a phantom event from before configure_smmu.
            pre_prod = stub.read_word(SMMU_EVENTQ_PROD)
            if pre_prod != 0:
                print("FAIL: EVENTQ_PROD = 0x%x before kick (expected 0); "
                      "earlier fault leaked" % pre_prod)
                return 2

            drive_rbdma_oto(stub, IPA_SRC, IPA_DST, COPY_SIZE)

            # Translate fault is synchronous inside the RUN_CONF1 store
            # handler (r100_rbdma_kickoff → r100_rbdma_translate →
            # r100_smmu_translate → r100_smmu_emit_event); no BH involved.
            # Tiny sleep is a belt for any background work and not
            # strictly required.
            time.sleep(0.2)

            # ASSERT 1: PROD bumped to 1.
            post_prod = stub.read_word(SMMU_EVENTQ_PROD)
            if post_prod != 1:
                print("FAIL: EVENTQ_PROD = 0x%x post-kick (expected 1) — "
                      "SMMU did not emit. Check %s for "
                      "'r100-smmu cl=0 event dropped' or "
                      "'r100-rbdma cl=0 OTO: SMMU' lines"
                      % (post_prod, log_path))
                return 2
        finally:
            stub.close()

        # ASSERT 2: slot 0 content matches expected event encoding.
        evt = shm_read_event_slot(0)
        evt_id = evt[0] & 0xFF
        evt_sid = evt[1]
        evt_input = evt[4] | (evt[5] << 32)

        if evt_id != EXPECTED_EVENT_ID:
            print("FAIL: event[0]&0xff = 0x%02x (expected 0x%02x = "
                  "C_BAD_STE)" % (evt_id, EXPECTED_EVENT_ID))
            print("  full event: %s" % " ".join("%08x" % w for w in evt))
            return 2
        if evt_sid != EXPECTED_SID:
            print("FAIL: event[1] = 0x%x (expected 0x%x — sid)"
                  % (evt_sid, EXPECTED_SID))
            return 2
        if evt_input != IPA_SRC:
            print("FAIL: event input_addr = 0x%x (expected 0x%x = IPA_SRC)"
                  % (evt_input, IPA_SRC))
            return 2

        print("PASS: SMMU emitted INV_STE event after RBDMA OTO kick "
              "(event_id=0x%02x sid=%u input_addr=0x%x; PROD bumped 0→1)"
              % (evt_id, evt_sid, evt_input))
        return 0

    finally:
        teardown(proc)


if __name__ == "__main__":
    sys.exit(main())
