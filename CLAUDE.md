# CLAUDE.md

## Project

REMU — R100 NPU System Emulator. QEMU-based functional emulator for the
R100 (CR03 quad, 4-chiplet) NPU SoC. Runs unmodified firmware (q-sys,
q-cp) and drivers (kmd, umd) on emulated hardware.

## Documentation

- [docs/architecture.md](docs/architecture.md) — System architecture, memory maps, device model design, FW source references
- [docs/roadmap.md](docs/roadmap.md) — Phase 1 (FW boot, complete), Phase 2 foundation milestones (M1..M9-1, complete) + the unified P1..P11 architectural plan that supersedes the old Phase 2 / Phase 3 boundary
- [docs/debugging.md](docs/debugging.md) — **How the agent should build / run / inspect logs / drive GDB.** Read this before running the emulator.

## Build

All commands go through `./remucli` at the repo root (bash wrapper
around `cli/remu_cli.py`). Only runtime dep: `pip install --user click`.

```
./remucli build                     # QEMU (aarch64 + x86_64) with R100 device models
./remucli fw-build                  # q-sys FW (tf-a + cp{0,1}) → images/ (silicon only)
./remucli status                    # environment sanity-check
./remucli run --name my-test        # Phase 1: NPU only → output/my-test/
./remucli run --name dbg --gdb      # Phase 1: paused, GDB on :1234
./remucli run --host --name pair    # Phase 2: NPU + x86 host QEMUs + r100-npu-pci bridge
./remucli test                      # M5/M6/M7/M8/P4A/P4B/P5/P11 bridge tests (`test m5 p5` for subset)
./remucli clean --name pair         # wipe orphan procs/shm/sockets from a SIGKILL'd run
./remucli clean --all               # nuke every REMU-shaped process + /dev/shm/remu-*
./guest/build-guest-image.sh        # M8b Stage 2: stage images/x86_guest/{bzImage,initramfs.cpio.gz}
./guest/build-kmd.sh                # M8b Stage 2: rebuild rebellions.ko + rblnfs.ko
```

`silicon` is the only FW profile exercised end-to-end. `zebu*` builds
but emits a deprecation warning and is not in regression.

`./remucli run` auto-runs `clean --name <name>` first, so SIGKILL'd
prior runs don't leave stale shm / sockets / QEMU mmaps. Concurrent
runs with different `--name` are untouched. `./remucli test` wraps the
same cleanup per-test.

`./remucli build` produces `build/qemu/qemu-system-{aarch64,x86_64}`
from one pinned source tree. Adding x86_64 on top of an aarch64-only
build tree re-runs configure automatically (no `--clean` needed).

### `--host` mode (Phase 2)

The x86 guest sees our `r100-npu-pci` endpoint (vendor/device
`0x1eff:0x2030`, matching real CR03 silicon — stock `rebellions.ko`
binds with no changes). BAR sizes match `rebel_check_pci_bars_size`:

| BAR | Size  | Role |
|-----|-------|------|
| 0   | 64 GB | DDR — next power of 2 above `RBLN_DRAM_SIZE = 36 GB` (PCI BARs must be `is_power_of_2`); first 36 GB shared with chiplet-0 DRAM via `remu-shm` (default `--shm-size = 36 GB`), upper 28 GB is host-private `bar0_tail` lazy RAM (= reserved alias on real silicon). Shorter `--shm-size` extends the lazy tail downward |
| 2   | 64 MB | ACP / SRAM / logbuf; 4 KB prio-10 trap @ `FW_LOGBUF_SIZE` (M8b 3b cfg-head) forwards writes on `cfg` chardev, rest lazy RAM |
| 4   | 8 MB  | 4 KB MMIO head (INTGR M6 + MAILBOX_BASE M8a bidir + CM7-stub Stage 3a + queue-start doorbell relayed as SPI 185 to wake q-cp's `hq_task` post-P1c); rest lazy RAM |
| 5   | 1 MB  | MSI-X table (32 vectors) + PBA (`msix_notify()` target for M7; q-cp's `cb_complete → pcie_msix_trigger` post-P1c — formerly Stage 3c BD-done) |

BAR4 details: `MAILBOX_INTGR{0,1}` / `MAILBOX_BASE` writes are
serialised as 8-byte chardev frames on the `doorbell` socket; reads in
`MAILBOX_BASE` range return the live shadow fed by `issr` chardev
frames (NPU → host). Cold-boot `FW_BOOT_DONE` is **real**: q-sys
`bootdone_task` writes `0xFB0D` to PF.ISSR[4] which egresses over the
`issr` chardev into the shadow (see `docs/debugging.md` →
"Post-mortems" for the GIC wiring fix that enabled this). `INTGR0`
bit 0 (`SOFT_RESET`) triggers a narrow QEMU-side CM7-stub that
*re-synthesises* `0xFB0D` on the kmd's post-probe soft-reset
handshake, because REMU does not yet model a real CA73 cluster reset
(`docs/roadmap.md` → P8). The synthesis is gated on PF's
`fw_boot_done_seen` one-shot latch (set by q-sys's own
`bootdone_task` publish): pre-cold-boot SOFT_RESETs are dropped so
the kmd parks on its FW_BOOT_DONE poll until q-sys's
`bootdone_task` runs naturally — without this gate, kmd's
`RBLN_RESET_FIRST` could race ahead of q-sys's `main.c:250` DCS
memset and lose its `DDH_BASE_LO` write into the shared
`cfg-shadow` shm (`docs/debugging.md` → Side bug 2 fix). All
`INTGR1` bits relay verbatim as SPI 185;
q-cp on CP0 owns every interesting bit (queue doorbells wake
`hq_task → cb_task → cb_complete`; `QUEUE_INIT` bit 7 wakes
`hq_init / rl_cq_init` to publish the QINIT descriptor and write back
`init_done` over the P1a outbound iATU). No CM7-side special-case
dispatch on this register post-P7.

BAR2 details (M8b 3b → P1b → P10-fix): kmd writes `DDH_BASE_{LO,HI}`
(at `FW_LOGBUF_SIZE + 0xC0/0xC4`) to publish the host-RAM
`rbln_device_desc` to firmware. **P10-fix** carves a 4 KB
`cfg-shadow` memory-backend-file shared between the two QEMUs; the
host-side `r100-npu-pci` aliases it over the BAR2 cfg-head subregion
(prio 10 over BAR2 lazy RAM) and the NPU-side `r100-cm7` aliases the
same backend over `R100_DEVICE_COMM_SPACE_BASE = 0x10200000` (prio
10 over chiplet-0 DRAM, the address q-cp's `hil_init_descs` reads
through `FUNC_READQ(hil_reg_base[PCIE_PF] + DDH_BASE_LO)`). Result:
kmd writes are visible on q-cp's next read with no chardev queue
and no ordering race against the doorbell — the bug that the prior
`cfg`-chardev path exhibited under the kmd's busy-poll. The pre-P10
plumbing (per-write 8-byte `(cfg_off, val)` frames on a `cfg`
chardev, NPU-side `cfg_shadow[1024]` u32 array, NPU→host
`OP_CFG_WRITE` reverse-emit on `hdma`, host-side `cfg_mmio_regs[]`)
has been retired together with the `cfg` / `cfg-debug` chardevs and
the `OP_CFG_WRITE` opcode; q-cp's `cb_complete → writel(FUNC_SCRATCH,
magic)` lands directly in the shared backend through the cfg-mirror
alias and is observable on the kmd's next `rebel_cfg_read` for
`rbln_queue_test`. The paired **NPU ↔ host DMA executor** is the
`hdma` chardev with a 24 B header + payload protocol
(`src/bridge/remu_hdma_proto.h`).

Opcodes: `OP_READ_REQ` (NPU → host: "please `pci_dma_read` `len` bytes
at `dst`, tag with `req_id`"), `OP_READ_RESP` (host → NPU: payload for
a pending `req_id`), and `OP_WRITE` (NPU → host: "please
`pci_dma_write` `len` bytes at `dst`"). The historical `OP_CFG_WRITE`
opcode (NPU → host store into `cfg_mmio_regs[]`) was retired with the
shm-backed cfg-shadow; opcode 4 stays unallocated. P1c made q-cp's
native
`hq_task → cb_task → cb_complete` the single source of truth for the
BD lifecycle: queue-doorbell `INTGR1` bits relay as SPI 185 to wake
`hq_task`, which walks BD / packet over the P1a outbound iATU, writes
back `BD_FLAGS_DONE_MASK` + advances `queue_desc.ci`, publishes
`FUNC_SCRATCH` through the P1b cfg-mirror trap, and calls
`pcie_msix_trigger` (q-sys `osl/FreeRTOS/.../msix.c`) which fires
MSI-X through the `r100-imsix` MMIO. **P7 deleted** the gated
QEMU-side scaffolding that used to live under
`-global r100-cm7.{bd-done,qinit,mbtq}-stub=on` (Stage 3c per-queue
`R100Cm7BdJob` walker, Stage 3b `qinit` `fw_version` / `init_done=1`
write-back, M9-1b `cmd_descr` synth + mbtq push) along with the
`0x01..0x0F` `req_id` partition on the `hdma` wire that fed it. The
helpers had no callers post-P1c; q-cp owns the equivalent silicon
paths end-to-end.

#### P1a / P10-fix — chiplet-0 PCIe outbound iATU stub + DDH mirror

q-cp on CA73 CP0 dereferences PCIe-bus addresses directly (the kmd
publishes them with `HOST_PHYS_BASE = 0x8000000000ULL` added; real
silicon's chiplet-0 DesignWare iATU translates the AXI loads into
PCIe TLPs). REMU has no DW iATU, so until P1a the loads silently
read garbage from chiplet-0 lazy RAM. **P1a** introduced
`r100-pcie-outbound` to close that gap; **P10-fix** then replaced
its chardev RPC with a direct shared-memory alias after the chardev
RPC deadlocked under the kmd's busy-poll (the chardev RX iothread
couldn't acquire BQL while a vCPU was parked in
`readl_poll_timeout_atomic`).

1. **`r100-pcie-outbound`** (`src/machine/r100_pcie_outbound.c`) —
   sysbus device that traps the 4 GB AXI window at
   `R100_PCIE_AXI_SLV_BASE_ADDR = 0x8000000000ULL` (chiplet 0, PF
   only). The window is realised as a `MemoryRegion` alias over a
   shared `host-ram` `memory-backend-file` that the host x86 QEMU
   uses for its main RAM. Both QEMUs `mmap` the same file, so q-cp's
   outbound loads/stores are plain TCG accesses against the same
   pages the kmd allocates with `dma_alloc_coherent`. No chardev,
   no `qemu_cond_wait_bql()`, no BQL contention. The pre-P10-fix
   chardev path (per-access `OP_READ_REQ` / `OP_WRITE` in `req_id`
   partition `0xC0..0xFF`, parked vCPU on a per-device condvar)
   has been removed; the partition is reserved.

2. **BAR2 cfg-head ↔ NPU MMIO trap** in `r100-cm7`. Real silicon's
   inbound iATU maps host BAR2 onto NPU local memory at
   `FW_LOGBUF_SIZE` so kmd writes show up at
   `DEVICE_COMMUNICATION_SPACE_BASE = 0x10200000` for q-cp's
   `hil_init_descs` (which reads `DDH_BASE_LO/HI/SIZE` via
   `FUNC_READQ(hil_reg_base[PCIE_PF] + DDH_BASE_LO)`). P10-fix
   models this as a `MemoryRegion` alias over the shared
   `cfg-shadow` 4 KB `memory-backend-file` — same file the host
   x86 QEMU's `r100-npu-pci` aliases over its BAR2 cfg-head
   subregion. The pre-P10 path (host→NPU `cfg` chardev frames feeding
   an NPU-side `cfg_shadow[1024]` u32 array + NPU→host `OP_CFG_WRITE`
   reverse-emit feeding host `cfg_mmio_regs[]`) is gone.

`req_id` partitions on the `hdma` wire (single source of truth in
`src/include/r100/remu_addrmap.h`):
`0x00..0x7F` = reserved (legacy cm7 BD-done partition lived at
`0x01..0x0F` until P7 retired the FSM; the P1b cfg-mirror
`OP_CFG_WRITE` reverse-emit at `0x00` was retired with the
shm-backed cfg-shadow alias; UMQ multi-queue may reclaim this
range), `0x80..0xBF` = `r100-hdma` MMIO-driven channel ops,
`0xC0..0xFF` = reserved (formerly `r100-pcie-outbound`'s
synchronous PF-window reads; now serviced by the host-ram alias).

Verified end-to-end on `--host` boot: q-cp logs `Device descriptor
addr 0x80…, size 286720`, `Queue descriptor addr 0x80…0xec,
size 32`, `Context descriptor addr 0x80…0x12c, size 1112` — all
satisfied by direct alias accesses against the kmd's coherent DMA
buffer. m5/m6/m7/m8 bridge regression + p4a/p4b/p5 RBDMA/HDMA
regression unaffected.

#### P1b/P1c/P7 — honest BD lifecycle on q-cp/CP0

**P1b** (NPU→host cfg reverse mirror) closed the cfg loop both ways
through the trap above. **P10-fix** subsumed it: the host x86 and
NPU QEMUs alias the same `cfg-shadow` `memory-backend-file` over
their BAR2 cfg-head and cfg-mirror MMIO traps, so q-cp's
`cb_complete → writel(FUNC_SCRATCH, magic)` lands directly in the
shared backend and is observable on the kmd's next
`rebel_cfg_read(FUNC_SCRATCH)` for `rbln_queue_test` with no chardev
queue and no ordering race against the doorbell. The cfg-mirror
alias accepts any access width / alignment that hits TCG RAM (so
q-sys CP0's cold-boot `memset(DEVICE_COMMUNICATION_SPACE_BASE, 0,
CP1_LOGBUF_MAGIC)` flows through unmodified). Single-QEMU
NPU-only tests (m6 / m8) leave the link unset; the alias is
skipped and chiplet-0 lazy RAM services any incidental access at
the window.

**P1c** routed every interesting host doorbell to q-cp on CP0 and
gated the legacy QEMU-side scaffolding off by default. **P7 deleted
it outright**: the gated `r100-cm7` paths
(`bd-done-stub` / `qinit-stub` / `mbtq-stub`) had no callers post-P1c
and have been removed from the source tree. q-cp now owns the full
BD lifecycle natively: `hq_task → cb_task → cb_complete` walks
queue_desc / BD / packet over the P1a outbound iATU, writes
`FUNC_SCRATCH` through the P1b cfg-mirror trap, sets
`BD_FLAGS_DONE_MASK` + advances `queue_desc.ci`, and calls
`pcie_msix_trigger` on the `r100-imsix` MMIO. Bisecting a future
regression no longer means flipping a `-global` — the only path is
the silicon-accurate one. Pre-P7 this section listed the
`r100_cm7.bd-done-stub` knobs; check `git log src/machine/r100_cm7.c`
for the historical implementation if a stub-style reproducer is
needed.

Verification: `rbln_queue_test` passes silently (no
`failed to test queue` / `fence error` on the host serial),
`hils.log` shows `cb_complete: send MSIx interrupt to host` on the
NPU side, and `xp /1wx 0xf000200ffc` on the host monitor returns
`0xcafedead` (BAR2 + 0xFFC = `FUNC_SCRATCH`).

#### CM7 mailbox stub — chiplet-0 `MAILBOX_CP0_M4` (P10-fix)

q-sys's `notify_dram_init_done` and `rbln_cm7_get_values` /
`rbln_pcie_get_ats_enabled` etc. all talk to PCIE_CM7 over the
chiplet-0 `MAILBOX_CP0_M4` Samsung-IPM block at
`R100_CP0_MAILBOX_M4_BASE = 0x1FF1050000`. REMU doesn't model
PCIE_CM7, so a `r100-mailbox` instance with `cm7-stub=true` and
`cm7-smmu-base=R100_SMMU_TCU_BASE` terminates the channel:

- ISSR reads always return `0`. This matches the pre-fix
 lazy-RAM-via-FREERTOS-VA shape every CM7 poll loop relied on
 (`ipm_samsung_write` lands at the cross-chiplet PA path,
 `ipm_samsung_receive` at the FREERTOS-VA path — different
 unmapped PAs, so the receive side reads 0 on the first
 iteration and the loop exits cleanly with `val == 0`).
- Writes to `INTGR1` bit `cm7-dram-init-done-channel` (default
 11 = `CM7_DRAM_INIT_DONE_CHANNEL`) with the just-stored
 `ISSR[<channel>] == 0xFB0D` synchronously poke
 `CR0.SMMUEN = 1` at `cm7-smmu-base + 0x20` via an
 `ldl_le_phys` / `stl_le_phys` RMW (so existing `CMDQEN` /
 `EVENTQEN` survive) — silicon-equivalent to
 `pcie_cp2cm7_callback → dram_init_done_cb → m7_smmu_enable`.
 Counter `cm7_dram_init_done_acks` (vmstate-tracked) records
 fires.

The stub is the entire mechanism that lets the chiplet-0 SMMU
go from `CR0.SMMUEN = 0` (identity bypass) to `CR0.SMMUEN = 1`
(real STE/PTW walks against the FW-published stage-2 PT in
`hq_init`'s `ptw_init_smmu_s2 → smmu_s2_enable`). Without it,
HDMA's reads of LL chains at IPA `buf_PA + PF_SYSTEM_IPA_BASE`
return zeros instead of the stage-2-translated PA.

**Chiplets 1..3 don't need this stub** — and never did, even before
the fix. PCIE_CM7 is physically on chiplet 0 (the chiplet attached
to the PCIe block), so the dram-init-done handshake only happens
there. Worker chiplets have no PCIe and no CM7, so q-sys's
`smmu_init` branches:

```
if (CHIPLET_ID == CHIPLET_ID0)
    notify_dram_init_done();   /* chiplet 0: wait for CM7 ack */
else
    smmu_enable();             /* chiplets 1..3: MMIO-write CR0 directly */
```

On chiplets 1..3 the `smmu_enable()` path is a plain MMIO write
that lands on `r100-smmu`'s normal handler — it always worked. The
pre-fix all-zero HDMA bug was chiplet-0-only because every umd-side
DMA workload (q-cp's HDMA / RBDMA / DNC tasks) runs on chiplet 0,
and only chiplet 0's SMMU was the one stuck off. See
`docs/debugging.md` → "Open issue: P10".

The DNC task-queue mailboxes at `R100_PERI0_MAILBOX_M9_BASE` (COMPUTE),
`R100_PERI0_MAILBOX_M10_BASE` (UDMA), `R100_PERI1_MAILBOX_M9_BASE`
(UDMA_LP), and `R100_PERI1_MAILBOX_M10_BASE` (UDMA_ST) — all on
chiplet 0 — are real `r100-mailbox` instances (P3) so `mtq_init`'s
writes to `MBTQ_PI_IDX/CI_IDX` land on real Samsung-IPM SFRs across
all four cmd_types. q-cp's `taskmgr_fetch_dnc_task_master_cp1` polls
`MBTQ_PI_IDX` (ISSR[0]) on each; q-cp's `mtq_push_task` on CP0 owns
the publish side natively post-P1c (the dnc_one_task content comes
from the CB the kmd submitted, not synthesised by REMU). The legacy
M9-1b `r100_cm7_mbtq_push` + `r100_cm7_synth_cmd_descr` helpers are
gone with P7, along with the `R100_CMD_DESCR_SYNTH_BASE` private-DRAM
ring they used.

#### r100-hdma + active r100-dnc (M9-1c)

The `hdma` chardev moved from `r100-cm7` onto a new `r100-hdma` device
(QOM type `r100-hdma`, MMIO at `R100_HDMA_BASE = 0x1D80380000` on
chiplet 0). The motivation is that q-cp itself programs a DesignWare
dw_hdma_v0 register block at that address — REMU now models that
register block, and the chardev sits on the model's host-side
counterpart since CharBackends are single-frontend. Post-P10-fix
`r100-hdma` is the sole NPU-side sender on the `hdma` chardev (q-cp
drives its MMIO directly; no QOM link from `r100-cm7` anymore — the
cfg reverse path went away with the shm-backed cfg-shadow). req_id
partitioning on the wire (documented in
`src/bridge/remu_hdma_proto.h`):
0x00..0x7F = reserved (legacy cm7 BD-done partition lived at
0x01..0x0F until P7 retired the FSM; the P1b cfg-mirror reverse-emit
at 0x00 was retired with the shm-backed cfg-shadow alias; available
for UMQ multi-queue),
0x80..0xBF = r100-hdma MMIO-driven channel ops,
0xC0..0xFF = reserved (formerly r100-pcie-outbound synchronous
PF-window reads; the device now aliases shared host-ram instead).

The passive `r100-dnc-cluster` register-file stub (sparse regstore
seeding IP_INFO / SHM TPG / RDSN bits for boot) gained an active
task-completion path: writes to slot+0x81C (TASK_DESC_CFG1) with
`access_size=4` and `itdone=1` schedule a BH that pulses the matching
DNC GIC SPI (lookup table at `r100_dnc_intid()` in
`r100/remu_addrmap.h`; INTIDs are non-contiguous — DNC0=410,
DNC1=422…) and latches a synthesised `dnc_reg_done_passage` at
slot+0xA00 so q-cp's `dnc_X_done_handler` reads a coherent record.
GIC `num-irq` was bumped 256 → 992 so the wider DNC INTID set fits.

#### r100-rbdma (P4A reg-block + P4B OTO byte-mover — DDMA / DDMA_HIGHP cmd_types)

Per-chiplet `r100-rbdma` device (QOM type `r100-rbdma`, MMIO at
`R100_NBUS_L_RBDMA_CFG_BASE = 0x1FF3700000`, 1 MB) extracted from the
prior passive RBDMA section that lived inside `r100_dnc.c`. Models the
RBDMA register block defined by the q-cp HAL autogen
(`g_rbdma_memory_map.h` / `g_cdma_global_registers.h` /
`g_cdma_task_registers.h`). Sparse `R100RBDMARegStore` (GHashTable
offset → u32, mirror of `r100-dnc` / `r100-hbm`) so q-cp's RMW init
sequences round-trip. Synthetic IP_INFO seeds (info0..5) so
`rbdma_get_ip_info` / `rbdma_update_credit` see plausible silicon
shape: 8 TEs, 32-deep TQ / UTQ / PTQ / TEQ / FNSH FIFO. Without these,
the original passive-zero stub left credit at zero and q-cp would
never push a task. NORMALTQUEUE / URGENTTQUEUE / PTQUEUE status reads
return the configured queue depth ("all free between kicks"). Kick on
the final descriptor word: q-cp's `rbdma_send_task` writes 8 ascending
words into `RBDMA_CDMA_TASK_BASE_OFF = 0x200`, ending on `RUN_CONF1`
(`+0x01C`). The store on RUN_CONF1 captures the regstore-resident
PTID_INIT, pushes an `R100RBDMAFnshEntry` onto a 32-deep ring, and
schedules a BH that pulses `INT_ID_RBDMA1 = 978` per pushed entry
whose `RUN_CONF1.intr_disable` bit is clear. q-cp's `rbdma_done_handler`
then loops on `INTR_FIFO_READABLE_NUM` (live `fnsh_depth() & 0xFF`,
served above the regstore) and pops `FNSH_INTR_FIFO` (decrements the
ring head and returns `ptid_init`). `intr_disable=1` (q-cp's
`dump_shm` / `shm_clear` paths) still pushes the FIFO entry but
skips the GIC pulse. ERR slot (`INT_ID_RBDMA0_ERR = 977`, idx 0) is
reserved for symmetry — never pulsed today.

P4B layers an OTO byte-mover on top of the same RUN_CONF1 trigger:
the kickoff handler reads `SRCADDRESS_OR_CONST`, `DESTADDRESS`,
`SIZEOF128BLOCK`, and `RUN_CONF0` from the regstore, decodes
`RUN_CONF0.task_type`, and for `RBDMA_TASK_TYPE_OTO=0` reconstructs
the full 41-bit byte SAR/DAR (lower 32 bits in the address registers,
top 2 bits in `RUN_CONF0.{src,dst}_addr_msb`, both shifted left by 7
to convert 128 B-units → bytes). **SAR / DAR carry DVAs on real
silicon** — RBDMA's AXI burst traverses the per-chiplet SMMU-600 (S1
+ S2 walk) before hitting DDR. P11 made REMU model that walk
honestly: SAR and DAR are translated through
`r100_smmu_translate(SID=0, …)` (Notion REBELQ SMMU Design § 1: PF =
SID 0) before the chiplet base is added. The walker handles stage-2
(q-sys's `smmu_s2_enable` regime — `STE0.config=ALL_TRANS` with
S1_DSS=BYPASS); when `CR0.SMMUEN=0` (early boot, single-QEMU runs,
p4b/p5 tests) the translate is identity, so existing harnesses that
mmap raw chiplet-0 DRAM offsets keep working. The chiplet base
(`chiplet_id * R100_CHIPLET_OFFSET`) is added on top to translate
chiplet-local NoC addresses into QEMU's flat global
`&address_space_memory` — that's REMU plumbing, separate from SMMU
translation. Then `address_space_read` SAR → temp buf →
`address_space_write` DAR. Capped at `RBDMA_OTO_MAX_BYTES = 32 MiB`.
Other task_types (CST/DAS/PTL/IVL/DUM/VCM/OTM/GTH/SCT/GTHR/SCTR)
fall through to a `LOG_UNIMP` + `unimp_task_kicks++` no-op so q-cp's
done handler still drains the FNSH FIFO. Stats counters survive
reset: `oto_kicks`, `oto_bytes`, `oto_dma_errors`,
`unimp_task_kicks`. Verified end-to-end via `./remucli test p4b`
(4 KB byte move between chiplet-0 DRAM offsets, gdbstub-driven via
the on-demand HMP `gdbserver tcp::PORT` + `Qqemu.PhyMemMode:1`
packet sequence) and `./remucli test p11` (same harness with a
hand-staged 3-level stage-2 page table + STE, RBDMA driven with IPA
SAR/DAR).

#### r100-smmu (P11 — stage-2 walker, PF only; v2 — eventq + GERROR)

Per-chiplet `r100-smmu` device (QOM type `r100-smmu`, MMIO at
`R100_SMMU_TCU_BASE = 0x1FF4200000`, 128 KB — page 0 holds the
SMMU-600 control block, page 1 holds `EVENTQ_PROD/CONS` at offset
`0x100A8/AC`) with three roles:

1. **MMIO surface for FW init paths** (BL2 / FreeRTOS / q-cp).
   `CR0 → CR0ACK` mirroring (with `EVENTQEN`/`SMMUEN`/`CMDQEN` mask),
   `GBPA.UPDATE` auto-clear, `STRTAB_BASE` / `STRTAB_BASE_CFG` cache
   the stream-table geometry, `EVENTQ_BASE` / `EVENTQ_PROD` /
   `EVENTQ_CONS` + `IRQ_CTRL.{EVENTQ,GERROR}_IRQEN` (mirrored into
   `IRQ_CTRLACK`) drive the fault-delivery path below, `CMDQ_PROD`
   writes walk the CMDQ from the cached `(old_cons, new_prod]`
   range. Recognised opcodes: `CMD_SYNC` (writes 0 to msiaddr per
   `CS=SIG_IRQ`), `CMD_TLBI_NH_*` / `CMD_TLBI_S12_*` /
   `CMD_TLBI_S2_IPA` / `CMD_TLBI_NSNH_ALL` /
   `CMD_CFGI_STE{,_RANGE}` / `CMD_CFGI_CD{,_ALL}` /
   `CMD_PREFETCH_*` (logged + advance CONS as no-ops — v1 has no
   STE / IOTLB cache to invalidate, every translate re-reads STE).

2. **Public translate API** (`r100_smmu.h`):
   `r100_smmu_translate(s, sid, ssid, dva, access, *out)`. RBDMA OTO
   and HDMA LL walker call this on every NPU-side SAR/DAR/LLP before
   the chiplet base is added. Pre-`CR0.SMMUEN`: identity (matches
   Arm-SMMU pre-enable bypass). Post-enable: read STE from
   `STRTAB_BASE_PA + sid * 64`, decode `STE0.{V, config}`:
   `BYPASS` → identity, `ABORT` → `INV_STE` fault, `S1_TRANS` →
   v1 identity + `LOG_UNIMP` (q-cp's `smmu_init_ste` sets
   `STE1.S1DSS=BYPASS` for the SIDs it leaves at S1_TRANS, so the
   effective behaviour is identity), `S2_TRANS`/`ALL_TRANS` → build
   `SMMUTransCfg` from STE2/STE3 (`tsz` / `sl0` / `granule_sz` /
   `eff_ps` / `vmid` / `affd` / `vttb`) and dispatch to QEMU's
   existing `smmu_ptw()` with `stage=SMMU_STAGE_2`. STE3's `S2TTB` is
   converted from chiplet-local to QEMU global PA (add `chiplet_id *
   R100_CHIPLET_OFFSET`) before being handed to the walker so its
   `address_space_memory` PTE reads land at the right slot.

Engines connect via QOM `link<r100-smmu>` properties on
`r100-rbdma` / `r100-hdma`, set up in `r100_soc.c`. SID 0 (PF) is
hardcoded in v1 per Notion REBELQ SMMU Design § 1; multi-VF
(SIDs 1..4) is v2.

3. **Eventq + GERROR fault delivery (v2 — this commit).** Every
   `r100_smmu_translate` fault (`INV_STE`, `STE_FETCH`, page-table
   walk fault) is mapped to a FW event_id by
   `r100_smmu_fault_to_event_id` (per `q/sys/drivers/smmu/smmu.c`'s
   `smmu_print_event` table — `INV_STE` → `C_BAD_STE=0x04`,
   `WALK` → `F_TRANSLATION=0x10`, `STE_FETCH` →
   `F_STE_FETCH=0x06`), packaged as a 32 B SMMUv3 event record
   (event_id + sid + input_addr + ipa, words 4-7 reserved), and
   written to `EVENTQ_BASE_PA + (PROD & MASK) * 32` via
   `address_space_write` against the chiplet's flat global PA
   (FW writes already include the chiplet offset, so no
   `chiplet_id * R100_CHIPLET_OFFSET` add here — distinct from the
   pre-existing `STRTAB_BASE` double-add bug noted below). PROD
   then advances with the wrap bit, and GIC SPI 762
   (`R100_INT_ID_SMMU_EVT`) is pulsed when
   `IRQ_CTRL.EVENTQ_IRQEN=1`. If `((PROD+1) & MASK) == CONS`
   (overflow) the new event is dropped, `events_dropped++`, and
   `r100_smmu_raise_gerror(EVTQ_ABT_ERR)` toggles
   `SMMU_GERROR.EVTQ_ABT_ERR` against `GERRORN` — FW's
   `smmu_gerr_intr` reads `(GERROR ^ GERRORN) & ACTIVE_MASK`, so
   the toggle is what makes the bit "active". GIC SPI 765
   (`R100_INT_ID_SMMU_GERR`) pulses next. Both IRQ outputs
   (`evt_irq` index 0, `gerr_irq` index 1) are wired to the
   chiplet GIC in `r100_create_smmu`. Verified end-to-end by
   `tests/p11b_smmu_evtq_test.py`: V=0 STE + RBDMA OTO kick →
   PROD=1 + slot-0 event payload matches.

**Known v1 → v2 carry-over bug.** `r100_smmu_update_strtab` adds
`chiplet_id * R100_CHIPLET_OFFSET` to `strtab_base_pa`, but FW
already writes a global PA (you can see this in the cold-boot log:
chiplet 1 writes `0x4014000000` while the local-PA target is
`0x2014000000`). Latent because no test currently exercises a
non-zero chiplet's STRTAB; the v2 eventq path is written without
the double-add to keep new code correct. Fix scheduled for a
separate dedicated commit.

**v1 → v2 deferred** (each unblockable when a workload demands it):
STE / IOTLB cache (LL chains are 3-4 entries; cost is chain reads,
not page walks — re-reading STE every translate also makes
invalidation a free no-op), 2-level stream tables (q-sys uses
LINEAR for ≤32 SIDs), stage-1 walk, multi-VF, chiplet-0 PCIe-side
TBU SID 17, dedicated HDMA-PA SID 16. **Done in v2:** eventq /
GERROR fault delivery (this commit).

**Debug surface.** Optional `debug-chardev` property on `r100-smmu`
(wired to chiplet 0 only — same single-frontend reasoning as
`rbdma-debug` / `hdma-debug`); plumbed through
`-machine r100-soc,smmu-debug=<id>` and the CLI's `--host` mode
auto-creates `output/<name>/smmu.log`. Always-on once the file is
opened (no `-d`/`--trace` dependency); silent when not wired so
single-QEMU NPU smoke runs pay nothing. Format mirrors `rbdma.log` /
`hdma.log` — one line per significant event:

```
smmu cl=0 CR0 0x0→0x4 smmuen=0→0 eventqen=0→1 cmdqen=0→0
smmu cl=0 STRTAB_BASE base_pa=0x14000000 log2size=5 fmt=LINEAR n_sids=32
smmu cl=0 cmdq idx=0 op=0x04 CFGI_STE_RANGE cmd[0]=0x4 cmd[1]=0x5
smmu cl=0 xlate_in sid=0 ssid=0 dva=0x100000000 rd cr0_smmuen=1 strtab_base_pa=0x14000000
smmu cl=0 ste sid=0 v=1 cfg=ALL_TRANS s2t0sz=25 s2sl0=1 s2tg=0 s2ps=5 s2aa64=1 s2affd=1 s2r=0 vmid=0 s2ttb=0x6000000
smmu cl=0 ptw sid=0 dva=0x100000000 vttb=0x6000000 tsz=25 sl0=1 gran=12 eff_ps=48 perm=rd
smmu cl=0 xlate_out sid=0 dva=0x100000000 ok pa=0x7000000 page_base=0x7000000 mask=0xfff
```

Companion offline tools under `tests/scripts/`:
- `mem_dump.py` — generic shm-backed memory dumper (region-agnostic
  across `remu-shm` / `host-ram` / `cfg-shadow`; hex / u32 / u64 /
  raw output; PROT_READ). Useful for any debug session — BD
  descriptors, queue_descs, command buffers, RBDMA OTO src/dst,
  stream-tables, page tables, q-cp's stack/heap.
- `smmu_decode.py` — pure-Python SMMU-v3.2 byte decoder (STE, stage-2
  PTE, 3-level stage-2 walk against a live `remu-shm`). Pairs with
  `mem_dump.py --format raw --output …` to decode bytes pulled from
  any address the `smmu.log` trace points to. See
  `docs/debugging.md` → "SMMU debug surface" for the full loop.

`r100-pcie-outbound` keeps its `host-ram` alias (P10-fix); it does
not currently go through this walker. `r100-dnc-cluster` cmd_descr
fields stay untranslated until P6 surfaces a workload.

Verification: `./remucli test p11` plants a 3-level stage-2 page
table + STE in chiplet-0 DRAM via shm mmap, programs `STRTAB_BASE` /
`STRTAB_BASE_CFG` / `CR0.SMMUEN=1` via gdbstub, drives a 4 KB RBDMA
OTO with IPAs `0x100000000` / `0x100001000` mapped to chiplet-0
PAs `0x07000000` / `0x07800000`, asserts byte-for-byte equality at
the destination shm region. `./remucli test p11b` (this commit)
mirrors the harness: plants a deliberately invalid STE (`STE0.V=0`)
at SID 0 and a zeroed in-DRAM eventq, programs `STRTAB_BASE` +
`EVENTQ_BASE` (log2size=10, 1024-slot ring) + `IRQ_CTRL` (both EVT
and GERR IRQEN) + `CR0.{SMMUEN,EVENTQEN}=1`, kicks a 4 KB RBDMA
OTO, asserts `EVENTQ_PROD=1` + slot-0 = `(event_id=C_BAD_STE,
sid=0, input_addr=IPA_SRC)`. m5..p5 + p11 + p11b regression green.

Both QEMUs `mmap` three shared `memory-backend-file`s under
`/dev/shm/remu-<name>/` with `share=on`:

- **`remu-shm`** (36 GB by default — `R100_RBLN_DRAM_SIZE`, matches
  real CR03 silicon's per-PF DRAM window; M4/M5) —
  `r100-npu-pci.memdev` over BAR0 offset 0 on the host;
  `r100-soc.memdev` over chiplet-0 DRAM offset 0 on the NPU. Layout
  per chiplet, as the kmd allocates it: 1 GB `CP_SYSTEM_SIZE` system
  region (FW images / MMU page-table pool / sync block) + 35 GB user
  region (kmd's DVA pool for umd buffers); aggregate device memory
  across the 4 CR03 chiplets is 144 GB but only chiplet 0 is
  PCIe-exposed via BAR0 — chiplets 1..3 stay NPU-private. Tmpfs is
  sparse so the 36 GB ftruncate only reserves address range; the
  hard requirement is `/dev/shm` with ≥ 36 GB free (default tmpfs
  size = 50 % of host RAM, so a host with ≥ 72 GB RAM clears it).
  Override with `./remucli run --shm-size <bytes>` on tighter hosts;
  the lazy `bar0_tail` / `dram_tail` paths in
  `r100_npu_pci.c` / `r100_soc.c` keep BL31_CP1 / FreeRTOS_CP1 at
  `0x14100000` / `0x14200000` reachable for sub-default `--shm-size`,
  but the historical "tail stays private lazy RAM" comment no longer
  applies — at the default the FW image area is inside the splice,
  visible to host. (No security boundary here: REMU is a functional
  emulator, the host x86 is just another vCPU.)
- **`host-ram`** (`--host-mem`, default 512 MB; P10-fix) — host x86
  QEMU's main RAM `memory-backend-file`. The NPU's
  `r100-pcie-outbound` aliases the same backend over the chiplet-0
  PCIe AXI window at `R100_PCIE_AXI_SLV_BASE_ADDR = 0x8000000000`,
  so q-cp's outbound iATU loads/stores hit the same pages the kmd
  allocates with `dma_alloc_coherent`. Replaces the prior chardev
  RPC (deadlocked under the kmd's busy-poll).
- **`cfg-shadow`** (4 KB; P10-fix) — host x86 QEMU's BAR2 cfg-head
  subregion at `FW_LOGBUF_SIZE` aliases this backend; NPU
  `r100-cm7` cfg-mirror trap at `R100_DEVICE_COMM_SPACE_BASE`
  aliases the same backend. Replaces the prior `cfg` chardev path
  (8-byte frames + NPU `cfg_shadow[1024]` + host `cfg_mmio_regs[]`
  reverse-mirror via `OP_CFG_WRITE`).

HMP monitors: NPU on `output/<name>/npu/monitor.sock` (plus the
stdio-muxed readline monitor on uart0), host on
`output/<name>/host/monitor.sock`. By default SeaBIOS runs and the x86
guest idles at "No bootable device", serial to `host/serial.log`.

**M8b Stage 2 (x86 Linux guest):** with
`images/x86_guest/{bzImage,initramfs.cpio.gz}` staged (via
`./guest/build-guest-image.sh`), `--host` auto-adds `-kernel/-initrd`
+ virtio-9p share of `guest/` at `/mnt/remu` + `-cpu max` (stock kmd is
`-march=native` with BMI2 → `#UD` on minimal `qemu64`). The guest
boots Ubuntu HWE + busybox initramfs, runs `setup.sh` (insmods
`rebellions.ko` and watches for `FW_BOOT_DONE`). Overrides:
`--guest-kernel`, `--guest-initrd`, `--guest-share`, `--no-guest-boot`,
`--guest-cmdline-extra`. See commits `1ef7208` / `985fd58` for
artifact pipeline and GIC-bpr QEMU patch detail.

### Auto-verification on startup

Every `--host` run captures and checks:

- **M4/M5**: shm file in both `/proc/<pid>/maps`; `remushm` subregion
  at offset 0 of both `r100.bar0.ddr` (host) and `r100.chiplet0.dram`
  (NPU); host `info pci` lists `1eff:2030`.
- **M6**: host shows `r100.bar4.mmio` prio-10 overlay; NPU `info qtree`
  lists `r100-cm7` + `r100-mailbox`.
- **M7**: NPU lists `r100-imsix` @ `0x1BFFFFF000`; host shows
  `msix-{table,pba}` overlaying `r100.bar5.msix`.
- **M8a**: NPU `r100-mailbox` has `issr-chardev = "issr"`; host
  `r100-npu-pci` has `issr = "issr"`.
- **hdma + M9-1c**: NPU `r100-hdma` exists with
 `chardev = "hdma"` (M9-1c moved the chardev off cm7 onto the new
 device); host `r100-npu-pci` has `hdma = "hdma"` (logged to
 `{host,npu}/info-qtree-cfg-hdma.log`). The pre-P10-fix `cfg`
 chardev verifier (NPU `r100-cm7.cfg-chardev`, host
 `r100-npu-pci.cfg`) is gone — cfg-head propagation moved off
 chardev onto a shared `cfg-shadow` `memory-backend-file` aliased
 by both QEMUs.

All results go to `host/info-*.log` + `npu/info-*.log`. Failures print
to stdout (non-fatal — NPU still boots for post-mortem poking).

### End-to-end bridge tests

Driven by `./remucli test` (or individually). Each has pre-run
cleanup so SIGKILL'd prior state never poisons the next:

- `tests/m5_dataflow_test.py` — write magic to shm @ `0x07F00000`,
  `xp /4wx` on both monitors must agree.
- `tests/m6_doorbell_test.py` — drive 8-byte frames on
  `host/doorbell.sock`, check `doorbell.log` + `GUEST_ERROR`.
- `tests/m7_msix_test.py` — reverse: test impersonates NPU on
  `host/msix.sock`, emits 5 frames (3 ok + 1 oor + 1 bad-offset),
  checks `msix.log` + `GUEST_ERROR`.
- `tests/m8_issr_test.py` — two phases against two minimal QEMUs: NPU
  writes ISSR → host BAR4 shadow mirror via `xp`; host writes BAR4 →
  NPU mailbox ISSR update without spurious GIC SPI.
- `tests/p4a_rbdma_stub_test.py` — `--host` smoke; HMP `xp` against
  chiplet-0 `r100-rbdma` at `0x1FF3700000` asserts `IP_INFO3.num_of_executer = 8`,
  `NORMALTQUEUE_STATUS = 32`, `PTQUEUE_STATUS = 32`, `INTR_FIFO_NUM = 0`
  (idle). Reads-only — the kick path is covered by `p4b` (gdbstub-driven
  RUN_CONF1 burst) and end-to-end through real q-cp once P10's umd
  workload lands.
- `tests/p4b_rbdma_oto_test.py` — `--host` boot, then `mmap` the
  chiplet-0 DRAM head (the shm splice) to seed a 4 KB pseudo-random
  pattern at offset `0x07000000` and zero offset `0x07800000`. Spawns
  the NPU's gdbstub on demand via HMP `gdbserver tcp::4567`, switches
  to physical-memory mode (`Qqemu.PhyMemMode:1`), then `M<addr>,4:<hex>`
  packets drive the 6-word OTO descriptor against `r100-rbdma`'s
  RUN_CONF1 trigger. Asserts byte-for-byte equality at the destination
  via the same shm mmap. Exercises the full P4B byte-mover path end-to-end
  in a single test process (no umd/kmd/x86 boot stack on the critical path).
- `tests/p5_hdma_ll_test.py` — `--host` boot, mirrors the p4b shm-splice
  + gdbstub harness (gdbstub on `tcp::4568` to avoid clashing with p4b).
  Stages a single `dw_hdma_v0_lli{ctrl=CB|LIE, transfer_size=4 KB,
  sar=0x07000000, dar=0x07800000}` followed by a
  `record_llp(0,0)` terminator at chiplet-0 DRAM offset `0x07900000`,
  then drives `CTRL1=LLEN`, `LLP_LO|HI=desc`, `ENABLE=1`,
  `DOORBELL=START` against `r100-hdma` WR-ch0 at
  `0x1C00000000 + 0x180380000`. Both SAR/DAR are NPU-local DRAM so the
  walker takes the D2D in-process loop (`address_space_read` →
  `address_space_write`) — covers the LL chain decode + control-bit
  walking without depending on a live host BAR. The host-leg paths
  (NPU→host OP_WRITE chunking, host→NPU OP_READ_REQ + parked
  `qemu_cond_wait_bql()` round-trip) fall out of P10's umd `simple_copy`
  via a one-line address_space ↔ chardev swap from the D2D path.
- `tests/p11_smmu_walk_test.py` — `--host` boot, same shm-splice
  + gdbstub harness as p4b/p5 (gdbstub on `tcp::4569`). Stages a 3-level
  stage-2 page table (L1 @ `0x06000000`, L2 @ `0x06001000`, L3 @
  `0x06002000`) + one `r100-smmu` STE @ `0x06010000` mapping
  IPA `0x100000000` → PA `0x07000000` and IPA `0x100001000` → PA
  `0x07800000` directly through the shm mmap, then writes
  `STRTAB_BASE` / `STRTAB_BASE_CFG` / `CR0.SMMUEN=1` and drives a
  4 KB RBDMA OTO with the IPAs as SAR / DAR via gdbstub. Asserts
  byte-for-byte equality at the destination shm region — covers the
  full `r100_smmu_translate` → `smmu_ptw_64_s2` 3-level walk +
  STE-decode path end-to-end without an UMD/kmd workload. q-cp's own
  `m7_smmu_enable` only fires from a kmd-driven `dram_init_done_cb`
  mailbox callback, which we don't trigger, so chiplet-0 SMMU stays at
  reset until the test programs it.
- `tests/p11b_smmu_evtq_test.py` — `--host` boot, same shm-splice +
  gdbstub harness (gdbstub on `tcp::4570`). Stages a deliberately
  invalid STE (`STE0.V=0`) at SID 0 in chiplet-0 DRAM and zeroes a
  1024-slot eventq region (`log2size=10`, 32 KB), then writes
  `STRTAB_BASE` + `EVENTQ_BASE` + `IRQ_CTRL.{EVENTQ,GERROR}_IRQEN=1`
  + `CR0.{SMMUEN,EVENTQEN}=1` via gdbstub and kicks an RBDMA OTO with
  any source IPA so `r100_smmu_translate` returns `INV_STE`. Asserts
  the SMMU advanced `EVENTQ_PROD` to 1 and that slot 0 of the in-DRAM
  ring contains a 32 B SMMUv3 record with `event_id=C_BAD_STE` (0x04),
  `sid=0`, `input_addr=IPA_SRC`. Verifies the v2 fault-delivery path
  (`r100_smmu_emit_event`, GIC SPI 762 wiring) end-to-end — the
  GERROR overflow path is exercised by a follow-up if/when overflow
  semantics need their own test.

All `./remucli run` invocations write into `output/<name>/` (or
`output/run-<timestamp>/` if `--name` omitted). Never pass `/tmp/`
paths — stick to per-run directory. See `docs/debugging.md` for the
full agent loop.

### Shell completion (optional)

```
eval "$(./remucli completion bash)"   # or: completion zsh / completion fish
```

Persistent: add to `~/.bashrc` with an absolute path; alias or `PATH`
so bare `remucli` tab-completes.

### Manual ninja rebuild

Skips the CLI's symlink / meson-patch / `cli/qemu-patches/*.patch`
apply step — use only when `external/qemu` is already set up by a
prior `./remucli build`:

```
cd build/qemu && ninja -j$(nproc)
```

### `fw-build` internals

Wraps `external/ssw-bundle/.../q/sys/build.sh`. Components repeatable
(`-c tf-a -c cp0 -c cp1`, default = CA73 boot set); `--install` (on by
default) copies 6 CA73 binaries into `images/`. Toolchains from
`COMPILER_PATH_{ARM64,ARM32}` env vars (defaults under
`/mnt/data/tools/...`).

Before `build.sh`, `fw-build` idempotently applies every
`cli/fw-patches/*.patch` (forward/reverse-check dance, same as
`cli/qemu-patches/`). **Policy: `cli/fw-patches/` is empty.** The q-sys
submodule stays byte-identical to upstream; any unmodelled hardware
that would hang or `-EBUSY` boot gets a `src/machine/` or `src/host/`
QEMU stub (however minimal), never an `#ifdef` in firmware. The
plumbing stays so developers can drop a **local, uncommitted** debug
patch while bisecting — see `cli/fw-patches/README.md`. Runtime-side
example of this policy: the CM7-stub in `src/machine/r100_cm7.c`
(commit `a01d2b5`) — see BAR4 row above.

## Project layout

```
remucli               Bash wrapper — the one entry point
src/machine/          NPU-side QEMU device models (symlinked into external/qemu/hw/arm/r100/)
                        r100_soc.{c,h}      machine + QOM type-name registry
                        r100_mailbox.{c,h}  mailbox — .c private state, .h public helpers
                        r100_hdma.{c,h}     HDMA reg block — .c private state,
                                            .h public emit API (M9-1c) +
                                            outbound RX callback hook (P1a)
                        r100_pcie_outbound.c PCIe outbound iATU stub —
                                            chiplet-0 4 GB AXI window aliased
                                            over the shared host-ram backend
                                            (P1a + P10-fix)
                        r100_rbdma.{c,h}    RBDMA reg block — sparse regstore +
                                            kick → BH → done IRQ; per-chiplet (P4A)
                        r100_<dev>.c        one file per device (state struct private)
src/host/             Host-side (x86 guest) PCI device models (symlinked into external/qemu/hw/misc/r100-host/)
src/include/          Added to -I during QEMU configure
                        r100/remu_addrmap.h — `#include "r100/remu_addrmap.h"`
src/bridge/           Added to -I during QEMU configure — cross-side shared headers
                        remu_frame.h          8-byte frame codec (RX accumulator + emit)
                        remu_doorbell_proto.h BAR4 offset classifier + BAR2 cfg-head layout
                        remu_hdma_proto.h     bidirectional HDMA protocol (24 B header + payload,
                                              OP_WRITE / READ_REQ / READ_RESP);
                                              req_id partitions live in
                                              src/include/r100/remu_addrmap.h:
                                              0x00..0x7F reserved (legacy cm7 BD-done
                                                   partition + P1b cfg-mirror reverse-emit
                                                   retired by P7 + P10-fix shm cfg-shadow),
                                              0x80..0xBF r100-hdma channels,
                                              0xC0..0xFF reserved (formerly r100-pcie-outbound;
                                                   now serviced by shm host-ram alias)
                      Header-only `static inline`, so both host-side (system_ss)
                      and NPU-side (arm_ss) TUs pick up the same definitions
                      without introducing a shared object.
cli/remu_cli.py       Click-based CLI implementation
tests/                Test binaries and end-to-end Python tests
                        scripts/
                          gdb_inspect_cp1.gdb  CP1 vCPU sweep (frame 0 + ELR_EL3)
                          mem_dump.py          Generic shm-backed memory dumper
                                               (remu-shm / host-ram / cfg-shadow,
                                               hex / u32 / u64 / raw); PROT_READ
                          smmu_decode.py       Offline SMMU-v3.2 decoder
                                               (STE, stage-2 PTE, 3-level walk
                                               against a live remu-shm)
docs/                 Architecture, roadmap, debugging
external/             Read-only: ssw-bundle (q-sys, q-cp, kmd, umd, ...), qemu
guest/                M8b Stage 2 virtio-9p share (rebellions.ko / rblnfs.ko gitignored)
images/               FW binaries (bl1.bin, bl31_cp0.bin, freertos_cp0.bin, ...)
images/x86_guest/     M8b Stage 2 staged x86 guest kernel + initramfs (gitignored)
build/qemu/           QEMU build output (gitignored)
output/               Per-run log directories (gitignored; see docs/debugging.md)
```

## Code style

- QEMU conventions: 4-space indent, snake_case, QOM type system
- `device_class_set_legacy_reset()` (not `dc->reset`) for QEMU 9.2.0+
- `qdev_prop_set_array()` + `QList` for array properties (e.g. GIC `redist-region-count`)
- Machine type names must end with `-machine` (QEMU assertion)
- All remu source uses `r100_` prefix; external repos keep their own naming
- **Header discipline**: per-device `struct R100XxxState` and `DECLARE_INSTANCE_CHECKER(...)`
  live in the device's `.c` file, not in `r100_soc.h`. Cross-device access goes through
  QOM `link<>` properties or a small public-API header (see `r100_mailbox.h`), never
  by dereferencing another device's state.
- **Logging**: use `qemu_log_mask(LOG_{TRACE,UNIMP,GUEST_ERROR}, ...)` in hot paths, never
  `fprintf(stderr, ...)` — it bypasses the `-d`/`qemu.log` knobs and pollutes terminals.
- **Inter-QEMU wire format**: use `remu_frame_emit` / `remu_frame_rx_feed` from
  `src/bridge/remu_frame.h` for any 8-byte `(a, b)` chardev channel; don't hand-roll
  byte-swap + short-write bookkeeping inline.
- **BAR4 doorbell offsets**: classify via `remu_doorbell_classify()` from
  `src/bridge/remu_doorbell_proto.h`; don't add ad-hoc `off == 0x8 || off == 0x1c` checks
  in new code — the wire protocol must stay single-sourced across host + NPU.

## Hardware context

- **Target**: CR03 quad (`PCI_ID 0x2030`, `ASIC_REBEL_QUAD`, 4 chiplets)
- **CPU**: 8× CA73 per chiplet (2 clusters × 4 cores) = 32 total
- **CHIPLET_OFFSET**: `0x2000000000` between chiplets
- **Boot**: TF-A BL1 → BL2 → BL31 → FreeRTOS (q-sys), then q-cp tasks
- **UART**: PL011 @ `0x1FF9040000` (PERI0_UART0), 250 MHz
- **GIC**: GICv3 dist @ `0x1FF3800000`, redist @ `0x1FF3840000`
- **Timer**: 500 MHz (`CORE_TIMER_FREQ`)

## Key external files

Cross-references for FW/driver sources — `$BUNDLE` = `external/ssw-bundle/products`:

| What to check | File |
|---|---|
| SoC address map | `$BUNDLE/rebel/q/sys/.../autogen/g_sys_addrmap.h` |
| Platform defs | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/include/platform_def.h` |
| CMU polling | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/clk/clk_samsung/cmu.c` |
| PMU registers | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_pmu.c` |
| QSPI bridge | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/synopsys/qspi_bridge/qspi_bridge.c` |
| PCI BAR layout | `$BUNDLE/common/kmd/rebellions/rebel/rebel.h` |
| FW-host handshake | `$BUNDLE/common/kmd/rebellions/common/{fw_if.c,ring.c,queue.c}` |
