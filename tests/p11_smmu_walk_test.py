#!/usr/bin/env python3
"""P11 SMMU stage-2 walker end-to-end test.

This is the unit test for the v1 SMMU implementation
(`src/machine/r100_smmu.c` translate API + RBDMA wiring). The shape:

  1. Boot `./remucli run --host --name p11-smmu`. We use `--host` so the
     chiplet-0 DRAM head is spliced over `/dev/shm/remu-...`; the test
     drives both data + page-table seeding through that mmap and reads
     the destination back the same way (no UMD / kmd / x86 boot stack
     on the critical path).

  2. Wait for q-cp to settle. q-sys's `m7_smmu_enable` only fires from
     `dram_init_done_cb` on a CM7 mailbox channel, which the kmd never
     pokes in the absence of a workload — so chiplet-0 SMMU stays at
     reset (`CR0.SMMUEN=0`) and we have free rein to program it. (We
     could overwrite q-cp's SMMU state if it had set things up, but
     today it hasn't.)

  3. Seed chiplet-0 DRAM via shm mmap:
       - SRC_PA = 0x07000000 — 4 KB pseudo-random pattern.
       - DST_PA = 0x07800000 — zero-cleared landing zone.
       - L3 / L2 / L1 stage-2 page tables at 0x06002000 / 0x06001000 /
         0x06000000, mapping IPA_SRC=0x100000000 → SRC_PA and
         IPA_DST=0x100001000 → DST_PA.
       - One STE at 0x06010000 with `config=ALL_TRANS` + stage-2
         fields pointing at the L1 base.

  4. Drive SMMU MMIO via QEMU gdbstub (HMP `gdbserver tcp::PORT`,
     same trick as p4b/p5):
       - STRTAB_BASE = 0x06010000 (chiplet-local).
       - STRTAB_BASE_CFG = log2size=1 (2 SIDs), fmt=LINEAR.
       - CR0 = SMMUEN.

  5. Drive a chiplet-0 RBDMA OTO kick with SAR=IPA_SRC, DAR=IPA_DST,
     SIZE=4 KB. The byte mover hits `r100_rbdma_translate` →
     `r100_smmu_translate` → `smmu_ptw_64_s2`, walks our 3-level
     stage-2 tables, lands SRC_PA→buf→DST_PA via address_space_*.

  6. Read DST_PA via shm mmap. Assert byte-for-byte equality with
     the SRC pattern. Translation succeeded ↔ DST equals SRC.

The page-table layout intentionally puts L1[4]/L2[0]/L3[0,1] on the
walk path so a single L1 + L2 + L3 page covers both legs of the
copy. Both IPAs share the same L1 entry (index 4, derived from bit 32
of the IPA) and L2 entry (index 0); they differ only at L3 (entries
0 vs. 1, derived from bit 12). This validates the full 3-level walk
with the smallest possible scaffolding.

Stage-2 PTE format (from Arm SMMU v3.2 § "VMSAv8 stage-2 page
descriptor"; cross-checked against `external/qemu/hw/arm/smmu-internal.h`'s
`is_table_pte` / `is_page_pte` / `PTE_AP` / `PTE_AF` macros):
  - bits[1:0] = 0b11 (table descriptor at L1/L2; page at L3)
  - bits[7:6] = S2AP (0b11 = RW)
  - bit[10]   = AF (Access Flag)
  - bits[51:12] = output PA[51:12]

STE format (from `external/.../q/sys/.../drivers/smmu/smmu.h:313-432`,
mirrored in `src/machine/r100_smmu.c`'s `R100_STE0_*` / `R100_STE2_*`):
  - STE[0] bit[0]    = V (valid)
  - STE[0] bits[3:1] = config (0b110=S2_TRANS, 0b111=ALL_TRANS)
  - STE[2] bits[37:32] = S2T0SZ (input_size = 64 - tsz)
  - STE[2] bits[39:38] = S2SL0 (start_level encoding)
  - STE[2] bits[47:46] = S2TG (4KB=0)
  - STE[2] bits[50:48] = S2PS (5 = 48-bit OAS)
  - STE[2] bit[51]   = S2AA64
  - STE[2] bit[53]   = S2AFFD (set so AF=0 PTEs don't fault — belt
                       for our PTEs which already have AF=1)
  - STE[3] = S2TTB (chiplet-local PA of L1)

The test exits 0 on PASS, 2 on byte mismatch, 1 on infrastructure
failure (boot timeout / gdbstub failure / shm mismatch). On early exit
we dump the tail of `output/p11-smmu.log` so a broken q-cp boot is
distinguishable from an SMMU walker bug.
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
RUN_NAME = "p11-smmu"
RUN_DIR = REPO / "output" / RUN_NAME
SHM_PATH = Path("/dev/shm/remu-" + RUN_NAME) / "remu-shm"

# Chiplet-0 SMMU MMIO. Per src/include/r100/remu_addrmap.h:
#   R100_SMMU_TCU_BASE = 0x1FF4200000.
SMMU_BASE = 0x1FF4200000
SMMU_CR0 = SMMU_BASE + 0x20
SMMU_STRTAB_BASE_LO = SMMU_BASE + 0x80
SMMU_STRTAB_BASE_HI = SMMU_BASE + 0x84
SMMU_STRTAB_BASE_CFG = SMMU_BASE + 0x88

SMMU_CR0_SMMUEN = 1 << 0

# Chiplet-0 RBDMA MMIO. Same offsets as tests/p4b_rbdma_oto_test.py.
RBDMA_BASE = 0x1FF3700000
PTID_INIT_REG = RBDMA_BASE + 0x200
SRCADDR_REG = RBDMA_BASE + 0x204
DESTADDR_REG = RBDMA_BASE + 0x208
SIZE_REG = RBDMA_BASE + 0x20C
RUN_CONF0_REG = RBDMA_BASE + 0x218
RUN_CONF1_REG = RBDMA_BASE + 0x21C
RUN_CONF1_INTR_DISABLE = 1 << 0

# Page-table + STE locations in chiplet-0 DRAM (within the shm splice;
# all comfortably above FreeRTOS_CP0 image @ 0x00200000 and below
# BL31_CP1 image @ 0x14100000, well clear of q-cp's high-DRAM SHM_BASE).
L1_TABLE_PA = 0x06000000
L2_TABLE_PA = 0x06001000
L3_TABLE_PA = 0x06002000
STE_TABLE_PA = 0x06010000

# Source / destination data buffers (also in shm splice).
SRC_PA = 0x07000000
DST_PA = 0x07800000
COPY_SIZE = 0x1000  # one page (32 × 128 B blocks)

# IPAs (DVAs the engine programs). Both fit in a 39-bit input range
# (S2T0SZ=25). The bit-32 set distinguishes them clearly from any
# chiplet-local PA — a partial / bypassed walk would land outside the
# shm region and the byte verification would fail loudly.
#   IPA_SRC = 0x100000000 → bits[38:30]=4 → L1[4]
#                          bits[29:21]=0 → L2[0]
#                          bits[20:12]=0 → L3[0]
#   IPA_DST = 0x100001000 → bits[38:30]=4 → L1[4]
#                          bits[29:21]=0 → L2[0]
#                          bits[20:12]=1 → L3[1]
# So a single L1 + L2 + L3 page covers both legs.
IPA_SRC = 0x100000000
IPA_DST = 0x100001000

# Stage-2 PTE field encodings.
PTE_TYPE_TABLE = 0x3      # table descriptor at L1/L2
PTE_TYPE_PAGE = 0x3       # page descriptor at L3 (same low-bit pattern)
PTE_S2AP_RW = 0x3 << 6    # bits[7:6] = read+write
PTE_AF = 1 << 10          # access flag

# Avoid colliding with p4b (4567) and p5 (4568).
GDB_PORT = 4569

# Stream Table Entry layout. v1 only walks stage-2, but we set
# AA64 + AFFD + ALL_TRANS so the test exercises the most-flexible path.
STE_VALID = 0x1
STE_CONFIG_ALL_TRANS = 0x7 << 1     # bits[3:1] = 0b111
STE_S2T0SZ_25 = 25 << 32            # bits[37:32]
STE_S2SL0_1 = 1 << 38               # bits[39:38] — start_level=1
STE_S2TG_4KB = 0 << 46              # bits[47:46]
STE_S2PS_48 = 5 << 48               # bits[50:48]
STE_S2AA64 = 1 << 51                # bit[51]
STE_S2AFFD = 1 << 53                # bit[53]


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
# GDB Remote Serial Protocol client. Cribbed from p4b/p5; same trio of
# operations: switch to physical-memory mode, write a 32-bit word, read
# a 32-bit word.
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
# Page-table + STE constructors.
# --------------------------------------------------------------------------

def build_pte_table(child_pa):
    """Stage-2 table descriptor: bits[51:12]=child_pa[51:12], bits[1:0]=0b11."""
    return (child_pa & ~0xFFF) | PTE_TYPE_TABLE


def build_pte_page(out_pa):
    """Stage-2 page descriptor at L3: bits[51:12]=out_pa[51:12],
    bits[7:6]=S2AP=RW, bit[10]=AF=1, bits[1:0]=0b11."""
    return (out_pa & ~0xFFF) | PTE_S2AP_RW | PTE_AF | PTE_TYPE_PAGE


def build_ste(vttb_pa):
    """64-byte STE for stage-2 ALL_TRANS. STE3 carries vttb (chiplet-local
    PA of the L1 table); the SMMU adds the chiplet base when wiring
    SMMUTransCfg.s2cfg.vttb (we're chiplet 0 so the add is a no-op). The
    other STE words are zero — STE1's stage-1 fields are ignored when
    config==S2_TRANS/ALL_TRANS in v1 (q-cp's `smmu_init_ste_bypass` sets
    S1DSS=BYPASS for the same effect on real silicon)."""
    ste = [0] * 8
    ste[0] = STE_VALID | STE_CONFIG_ALL_TRANS
    ste[2] = (STE_S2T0SZ_25 | STE_S2SL0_1 | STE_S2TG_4KB | STE_S2PS_48 |
              STE_S2AA64 | STE_S2AFFD)
    ste[3] = vttb_pa & ~0xFFF
    return struct.pack("<8Q", *ste)


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


def make_pattern(size):
    # Same pseudo-random recipe as p4b — avoid 0x00 / 0xFF runs so a
    # partial/wrong copy is loud.
    return bytes(((i * 37 + 13) ^ ((i >> 7) * 91)) & 0xFF
                 for i in range(size))


def shm_seed(pattern):
    """Seed the chiplet-0 DRAM splice with: source pattern, zeroed dst,
    populated L1 / L2 / L3 / STE tables. We touch the shm directly
    (instead of via gdbstub `M` packets) because the latter would need
    >8 KB of round-trips for the page tables alone."""
    if not SHM_PATH.exists():
        raise RuntimeError("shm file %s not found" % SHM_PATH)
    needed = max(SRC_PA, DST_PA, STE_TABLE_PA + 64) + COPY_SIZE
    size = SHM_PATH.stat().st_size
    if size < needed:
        raise RuntimeError(
            "shm too small: %d < %d" % (size, needed))
    with open(SHM_PATH, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)
        try:
            # Source / destination data buffers.
            mm[SRC_PA:SRC_PA + COPY_SIZE] = pattern
            mm[DST_PA:DST_PA + COPY_SIZE] = b"\x00" * COPY_SIZE

            # L1 table — zero, then L1[4] = table → L2.
            mm[L1_TABLE_PA:L1_TABLE_PA + 4096] = b"\x00" * 4096
            mm[L1_TABLE_PA + 4 * 8:L1_TABLE_PA + 4 * 8 + 8] = struct.pack(
                "<Q", build_pte_table(L2_TABLE_PA))

            # L2 table — zero, then L2[0] = table → L3.
            mm[L2_TABLE_PA:L2_TABLE_PA + 4096] = b"\x00" * 4096
            mm[L2_TABLE_PA:L2_TABLE_PA + 8] = struct.pack(
                "<Q", build_pte_table(L3_TABLE_PA))

            # L3 table — zero, then L3[0]/L3[1] = page → SRC/DST.
            mm[L3_TABLE_PA:L3_TABLE_PA + 4096] = b"\x00" * 4096
            mm[L3_TABLE_PA:L3_TABLE_PA + 8] = struct.pack(
                "<Q", build_pte_page(SRC_PA))
            mm[L3_TABLE_PA + 8:L3_TABLE_PA + 16] = struct.pack(
                "<Q", build_pte_page(DST_PA))

            # STE table — zero a page, then STE[0] = our entry.
            mm[STE_TABLE_PA:STE_TABLE_PA + 4096] = b"\x00" * 4096
            mm[STE_TABLE_PA:STE_TABLE_PA + 64] = build_ste(L1_TABLE_PA)
            mm.flush()
        finally:
            mm.close()


def shm_read_dst():
    with open(SHM_PATH, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
        try:
            return bytes(mm[DST_PA:DST_PA + COPY_SIZE])
        finally:
            mm.close()


def configure_smmu(stub):
    """Override q-cp's SMMU state on chiplet 0:
       - STRTAB_BASE → our STE table.
       - STRTAB_BASE_CFG → log2size=1 (room for SID 0 + the unused 1),
         fmt=LINEAR=0.
       - CR0 → SMMUEN.
    """
    stub.write_word(SMMU_STRTAB_BASE_LO, STE_TABLE_PA & 0xFFFFFFFF)
    stub.write_word(SMMU_STRTAB_BASE_HI, (STE_TABLE_PA >> 32) & 0xFFFFFFFF)
    stub.write_word(SMMU_STRTAB_BASE_CFG, 1)
    stub.write_word(SMMU_CR0, SMMU_CR0_SMMUEN)


def drive_rbdma_oto(stub, src_dva, dst_dva, size_bytes):
    """Issue one RBDMA OTO kick. SAR/DAR carry DVAs (IPAs) — the kick
    handler's `r100_rbdma_translate` runs them through the SMMU before
    the address_space_* read/write."""
    src_128 = src_dva >> 7
    dst_128 = dst_dva >> 7
    blk_128 = size_bytes >> 7

    src_lo = src_128 & 0xFFFFFFFF
    src_msb = (src_128 >> 32) & 0x3
    dst_lo = dst_128 & 0xFFFFFFFF
    dst_msb = (dst_128 >> 32) & 0x3

    # task_type=OTO=0, src_addr_msb at bits[21:20], dst_addr_msb at [23:22]
    # — matches RBDMA_RUN_CONF0_{SRC,DST}_ADDR_MSB_SHIFT in
    # src/machine/r100_rbdma.c.
    run_conf0 = (src_msb << 20) | (dst_msb << 22)
    # intr_disable=1: no q-cp cb is registered for this synthetic kick;
    # suppressing the SPI keeps the rbdma_done_handler match path quiet.
    run_conf1 = RUN_CONF1_INTR_DISABLE

    ptid = 0xC0FFEEFF  # arbitrary marker, observable in regs post-kick.

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

        # Same 4 s settle as p4b — empirically enough for q-cp's
        # `rbdma_init` and the cb worker to park. SMMU state on chiplet 0
        # stays at reset (CR0=0) since q-sys's `m7_smmu_enable` only
        # fires from the kmd-driven `dram_init_done_cb` mailbox path,
        # which we don't trigger.
        time.sleep(4.0)

        pattern = make_pattern(COPY_SIZE)
        shm_seed(pattern)

        # Spawn gdbstub on demand. tcp:: prefix appends `wait=off`, so
        # the VM stays running and we can connect any time.
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
            drive_rbdma_oto(stub, IPA_SRC, IPA_DST, COPY_SIZE)
        finally:
            stub.close()

        # The byte move runs synchronously inside the RUN_CONF1 store
        # handler. Tiny sleep is a belt for any deferred BH work and
        # not strictly required.
        time.sleep(0.2)

        got = shm_read_dst()
        diff = first_diff(got, pattern)
        if diff is not None:
            print("FAIL: dst != src — %s" % diff)
            print("(this means the SMMU walk landed at the wrong PA, "
                  "or a fault was raised — check %s for "
                  "'r100-smmu cl=0 TRANSLATE FAULT' / "
                  "'r100-rbdma cl=0 OTO: SMMU' lines)" % log_path)
            return 2

        print("PASS: %d B copied via RBDMA OTO + SMMU stage-2 walk "
              "(IPA 0x%x → PA 0x%x, IPA 0x%x → PA 0x%x)"
              % (COPY_SIZE, IPA_SRC, SRC_PA, IPA_DST, DST_PA))
        return 0

    finally:
        teardown(proc)


if __name__ == "__main__":
    sys.exit(main())
