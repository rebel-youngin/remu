# REMU Architecture

## Overview

REMU is a QEMU-based system emulator for the R100 (CR03 quad) NPU. It
functionally emulates the NPU hardware so the existing firmware (q-sys,
q-cp) and drivers (kmd, umd) run unmodified. Primary audience: FW
developers who need fast local iteration without hardware.

**Not cycle-accurate.** PLLs lock instantly, DMA completes in zero time,
no pipeline/cache simulation. Goal: functional correctness — FW sees
the right register values, memory maps, and interrupt behaviour to boot
and run.

## R100 SoC Hardware Overview

Multi-chiplet NPU:

- 4 chiplets, each at `0x2000000000` offset from the previous
- 8 ARM CA73 cores per chiplet (CP0 + CP1, 4 cores each) = 32 total
- Per-chiplet peripherals: CMU, PMU, HBM3, DNC, HDMA
- Inter-chiplet: UCIe via 6 RBC blocks, QSPI bridge for register access
- Host: PCIe endpoint (BAR-mapped DRAM / config / doorbell), MSI-X for
  FW→host interrupts

```
Host (x86/ARM server) ── PCIe ──▶ R100 NPU card (4 chiplets × [8 CA73,
                                   16 DNC, HDMA, HBM3], linked via UCIe)
```

## Emulator Architecture

### Dual-QEMU Model (Phase 2)

Two QEMU instances connected by shared memory and chardev sockets.
`./remucli build` produces both `qemu-system-{aarch64,x86_64}` from one
pinned source tree; `./remucli run --host` wires them up.

```
┌─ Host QEMU (x86_64 + KVM) ───────────────────────────────┐
│  umd + kmd (unmodified) → PCI 1eff:2030                   │
│    BAR0=DRAM  BAR2=ACP  BAR4=doorbell  BAR5=MSI-X         │
│  r100-npu-pci device                                       │
│    BAR0 memdev splice over remu-shm                        │
│    BAR2 4 KB cfg-head alias over cfg-shadow shm            │
│    BAR4 4 KB MMIO head: INTGR + MAILBOX_BASE shadow        │
│    BAR5 msix_init table + lazy RAM                         │
│  chardevs: doorbell (host→NPU) / msix, issr (NPU→host) /   │
│            hdma (bidir)                                    │
└──┬──────┬─────┬──────┬─────┬───────────────────────────────┘
   │ shm  │door │ msix │issr │ hdma
┌──┴──────┴─────┴──────┴─────┴───────────────────────────────┐
│  NPU QEMU (aarch64, bare-metal)                            │
│  32 × CA73 (cortex-a72 w/ MIDR=A73 r1p1)                   │
│  TF-A BL1 → BL2 → BL31 → FreeRTOS                          │
│  Chiplet-0 peripherals wired to chardev bridges:           │
│    r100-cm7      — INTGR / ISSR ingress + cfg-mirror alias │
│    r100-hdma     — dw_hdma_v0 reg block; owns hdma chardev │
│    r100-mailbox  PF / VF0 / 4 × DNC task queues            │
│    r100-imsix    — MSI-X egress on write to 0xFFC          │
│    r100-pcie-outbound — chiplet-0 4 GB PCIe AXI window     │
│                          aliased over host-ram shm         │
│  Per-chiplet: CMU, PMU, HBM3, QSPI, RBC, SMMU,             │
│   PVT, DNC cfg, RBDMA cfg, SYSREG CP{0,1} triple-mount,    │
│   arm-gicv3 (num-cpu=8, first-cpu-index=N*8, num-irq=992), │
│   16550 UART (gpio_in[33] = INTID 65, polled),             │
│   HILS ring tail (logbuf)                                  │
│  CPU gtimer→GIC PPI (CNTVIRQ = PPI 27 → tick)              │
└────────────────────────────────────────────────────────────┘
```

**Why two instances?** FW runs bare-metal AArch64 at EL3/EL1 with a
40-bit PA space at `0x1E00000000`; kmd runs inside a Linux kernel.
Fundamentally incompatible — heterogeneous single-QEMU (à la Xilinx
ZynqMP) would be far more complex to build and maintain.

**Why a QEMU fork?** Custom memory map, device models, and boot loading;
QEMU does not support out-of-tree device compilation, so remu device
sources are symlinked into the QEMU tree and compiled in-tree.

### Per-Chiplet CPU Memory View

Silicon routes the 256 MB `PRIVATE_BASE` window (`0x1E00000000`) as a
chiplet-local alias — each chiplet's CPUs see their own private
peripherals there. FW reads `CHIPLET_ID` from
`SYSREG_SYSREMAP_PRIVATE+0x444` to discover which chiplet it runs on,
so the same BL1/BL2 binary works on all 4.

REMU models this via per-chiplet `MemoryRegion` containers built in
`r100_build_chiplet_view()` (see `r100_soc.c`). Each view:

1. Aliases shared `sysmem` at offset 0 (so DRAM / GIC / UART / config
   space all behave normally).
2. Overlays a 256 MB priority-10 window at `PRIVATE_WIN_BASE` that
   aliases into `chiplet_id * CHIPLET_OFFSET + PRIVATE_WIN_BASE`.
3. Overlays `SYSREG_CP{0,1}` config-space windows at
   `R100_CP{0,1}_SYSREG_BASE` pointing to the chiplet's own
   private-alias RAM. This makes BL1's QSPI-bridge RVBAR writes, BL2's
   CP1 release, and BL31's PSCI warm-boot `set_rvbar` all converge on
   the same backing RAM.
4. A fourth absolute cross-chiplet form (`chiplet_N * CHIPLET_OFFSET +
   SYSREG_CP{0,1}_BASE + ...`) is handled by a separate alias of the
   same RAM mounted inside each chiplet's `cfg_mr` with overlap
   priority 1, outranking the chiplet-wide unimpl catch-all.

Each CPU's `memory` link is set to its chiplet's view before realise;
all TLB walks and instruction fetches observe chiplet-local routing.
Secondary CPUs therefore start at the unmodified `0x1E00028000` entry
(the private-alias BL2 base) — adding `chiplet_id * CHIPLET_OFFSET`
would corrupt PC-relative ADRP symbols in BL2's MMU setup. The PSCI
CPU_ON warm-boot path follows the same mechanism: PMU hands
`bl31_warm_entrypoint` to `arm_set_cpu_on(mpidr, entry, target_el=3)`,
BL31 warm-boots at EL3, then `ERET`s to EL1.

### Per-Chiplet Memory Map

Each chiplet is identical at offset `chiplet_id * 0x2000000000`:

| Region | Base (chiplet 0) | Size | Description |
|---|---|---|---|
| DRAM   | `0x00_0000_0000` | 1 GB (emu) | FW images, user data |
| iROM   | `0x1E_0000_0000` | 64 KB | Boot ROM |
| iRAM   | `0x1E_0001_0000` | 256 KB | BL1 load target |
| SP_MEM | `0x1F_E000_0000` | 64 MB | Scratchpad (2 × 32 MB D-Clusters) |
| SH_MEM | `0x1F_E400_0000` | 64 MB | Shared (2 × 32 MB D-Clusters) |
| Config | `0x1F_F000_0000` | ~3 GB | Peripheral MMIO (CMU/PMU/GIC/…) |

### Boot Flow

```
BL1  (iRAM @ 0x1E00010000)  — CMU/PMU/QSPI init, discover 3 secondary
                               chiplets, load BL2 from flash, jump.
BL2  (iRAM @ 0x1E00028000)  — HBM3 init, inter-chiplet handshake via
                               mailbox RAM, SMMU TCU early init, MMU,
                               load BL31+FreeRTOS images, release
                               CP1.cpu0 (#ifndef ZEBU_CI), jump BL31.
BL31_CP0 (DRAM @ 0x00000000)              BL31_CP1 (DRAM @ 0x14100000)
  ├─ GIC shared + TZPC for CP0              ├─ GIC shared + TZPC for CP1
  └─ ERET → FreeRTOS_CP0 (EL1)              └─ ERET → FreeRTOS_CP1 (EL1)
FreeRTOS_CP0 (DRAM @ 0x00200000)          FreeRTOS_CP1 (DRAM @ 0x14200000)
  ├─ init_smp() PSCI CPU_ON → warm-boot → q-cp tasks on both halves
```

### CP0 / CP1 cluster split

Each chiplet has two independent 4-core clusters (CP0, CP1), each
running its own `bl31_cp{0,1}.bin` → `freertos_cp{0,1}.bin`. The q-sys
build differs mainly in `__TARGET_CP` (= 0 or 1) which picks the
`SYSREG_CP0` vs `SYSREG_CP1` RVBAR write address.

Release sequence:

- **CP0.cpu0** of primary: started by QEMU at reset via `rvbar` property.
- **CP0.cpu0** of chiplets 1-3: released cross-chiplet by BL1 on primary
  via QSPI bridge (`plat_pmu_cpu_on` + `qspi_bridge_write_1word` to each
  slave's `CPMU_PRIVATE + CPU0_CONFIGURATION`).
- **CP1.cpu0** of each chiplet: released by BL2 on the same chiplet's
  CP0.cpu0 via direct MMIO (`#ifndef ZEBU_CI` branch in
  `rebel_h_bl2_setup.c`).
- **CP0/CP1.cpu{1,2,3}**: PSCI `CPU_ON` SMC into `BL31_CP{0,1}` warm-boot.

PMU device handles all four variants uniformly: decodes cluster/cpu
from `CPU_CONFIGURATION + cluster*PERCLUSTER_OFFSET + cpu*PERCPU_OFFSET`,
reads RVBAR back from `SYSREG_CP{0,1}`, calls `arm_set_cpu_on()`.

### Log routing

FW emits boot messages through two independent paths:

1. **Direct UART** (`printf_`, TF-A `NOTICE/INFO`): QEMU `serial-mm`
   (16550, `regshift=2`) at `PERI0_UART0_BASE` (`0x1FF9040000`) on each
   chiplet, priority-10 overlap over the config-space container.
   Chiplet 0 is muxed to stdio + QEMU monitor and tee'd to `uart0.log`;
   chiplets 1-3 into `uart{1,2,3}.log`.

2. **HILS ring buffer** (`RLOG_*/FLOG_*`): rate-limited structured log
   written as 128-byte records into a 2 MB DRAM ring at `0x10000000`.
   `r100-logbuf-tail` polls on a 50 ms `QEMU_CLOCK_VIRTUAL` timer and
   writes each entry as `[HILS <tick> cpu=N LEVEL task|func] <msg>` to
   its own chardev (`hils.log`). Independent of `terminal_task`
   scheduling on silicon.

### Device Model Design

QEMU QOM pattern: `type_init` registration, `MemoryRegionOps` for
MMIO, properties for parameterisation, `reset` handler for defaults.

**Stub philosophy**: return the minimum register values needed for FW
to proceed. Most stubs are store-on-write / return-on-read with
specific overrides for status bits (e.g. CMU PLL lock).

**Scaffolding stubs** — `r100-cm7`'s synthetic `FW_BOOT_DONE`
re-handshake on `INTGR0 bit 0` is the only QEMU-side behavioural lie
remaining on a stock run. It exists because REMU does not yet model
a real CA73 cluster soft-reset path — the kmd's `rebel_hw_init`
SOFT_RESET would otherwise hang. Slated for retirement in **P8**
(`docs/roadmap.md`) once a real CA73 cluster reset model lands.
Cold-boot `FW_BOOT_DONE` already travels the real path (q-sys's
`bootdone_task` → PF.ISSR[4] → `issr` chardev → host BAR4 shadow);
the stub is gated on a one-shot latch flipped by that real publish,
so it only fires on subsequent soft-resets. Every other lifecycle
event (BD walk, cmd_descr push, MSI-X completion) lives on the side
of silicon that owns it.

### Key Design Decisions

| Decision | Choice | Why |
|---|---|---|
| CPU model | `cortex-a72` + MIDR spoofed to CA73 r1p1 (`0x411FD091`) + two IMPDEF sysreg tables (`r100_samsung_impdef_regs`, `r100_cortex_a73_impdef_regs` in `r100_soc.c`) | Closest QEMU model; r1p1 MIDR skips revision-gated CA73 errata; the WA paths TF-A always runs (CVE-2018-3639) read/write IMPDEF regs we register as RAZ/WI. |
| GIC | One `arm-gicv3` per chiplet (`num-cpu=8`, `first-cpu-index=N*8`) | Matches silicon; `first-cpu-index` added by `cli/qemu-patches/0001-arm-gicv3-first-cpu-index.patch` so multi-GIC machines bind disjoint CPU ranges. Generic-timer outs wired to PPIs 30/27/26/29/28 (same layout as `hw/arm/virt.c`). |
| QSPI bridge | QEMU `address_space_read/write` | Cross-chiplet register I/O. |
| UART overlap | prio 10 over config-space | Outranks the unimpl catch-all. |
| Machine name | `r100-soc-machine` | `-machine` suffix required by `MachineClass`. |
| Timer freq | 500 MHz (`CNTFRQ_EL0`) | Matches silicon (`CORE_TIMER_FREQ`). |
| DRAM size | 1 GB / chiplet | Enough for FW boot (silicon has up to 36 GB). |

## Source Tree Layout

```
src/
  machine/          NPU-side QEMU device models (aarch64-softmmu)
    r100_soc.{c,h}     machine + type-name registry (no device state structs)
    r100_mailbox.{c,h} mailbox state (private to .c) + public helper API
    r100_<dev>.c       one file per device — state struct, ops, type_init
  host/             x86 host-side PCIe endpoint (x86_64-softmmu)
    r100_npu_pci.c
  include/          Headers added to QEMU's -I search path
    r100/remu_addrmap.h  SoC address constants, BAR4/BAR5 offsets, PCI IDs
  bridge/           Cross-side shared headers added to -I (host + NPU TUs)
    remu_frame.h          8-byte (a, b) frame codec: RX accumulator + emit
    remu_doorbell_proto.h BAR4 offset classifier + BAR2 cfg-head layout
    remu_hdma_proto.h     24 B header + payload bidirectional HDMA protocol
                          (OP_WRITE / READ_REQ / READ_RESP)
```

**Header discipline.** Each device's `struct R100XxxState`,
`DECLARE_INSTANCE_CHECKER(...)`, and private register-count defines
live in its own `.c` file. `r100_soc.h` only exposes the machine-state
subclass, the `TYPE_R100_*` type names, and (via `#include
"r100_mailbox.h"` / `#include "r100_imsix.h"`) the mailbox + imsix
helper prototypes that other devices need to drive both from outside
their own MMIO path. This keeps the state layout private, forces
cross-device communication through either QOM properties (e.g.
`link<>`) or purpose-specific helper headers, and means touching
`R100MailboxState` fields no longer rebuilds every NPU-side device.

**Cross-side shared code.** The host side (`r100_npu_pci.c`, compiled
into `system_ss`) and the NPU side (`r100_cm7.c` /
`r100_imsix.c` / `r100_mailbox.c`, compiled into `arm_ss`) live in
different meson subsystems, so a single `.c` would need to link twice.
`src/bridge/*.h` sidesteps this: the wire format (`remu_frame.h`) and
the BAR4 protocol classifier (`remu_doorbell_proto.h`) are header-only
`static inline` — both sides pick up the same definitions from `-I
src/bridge` without introducing a shared TU.

## Source File Map

| File | Device | Instances | Behaviour |
|---|---|---|---|
| `r100_soc.c` | Machine | 1 | Builds chiplet views + CPUs + per-chiplet GIC/UART + mailbox cluster. Installs MIDR spoof + two IMPDEF sysreg tables per CPU. Wires gtimer→GIC PPIs. GIC `num-irq=992` so DNC INTIDs (up to 617) and the broader q-cp interrupt set fit. Instantiates `r100-cm7` / `r100-imsix` / `r100-hdma` / `r100-pcie-outbound` on demand and wires the QOM links between them; instantiates four real `r100-mailbox` blocks on chiplet 0 for the q-cp DNC task queues (COMPUTE / UDMA / UDMA_LP / UDMA_ST, matching `_inst[HW_SPEC_DNC_QUEUE_NUM=4]`). The chiplet/mailbox lazy-RAM loop skips all four chiplet-0 task-queue slots; chiplets 1..3 keep lazy-RAM since q-cp/CP1 only runs on chiplet 0. For each DCL on each chiplet, wires 8 slots × 4 cmd_types = 32 DNC done SPIs to that chiplet's GIC via `r100_dnc_intid()`. Per chiplet instantiates one `r100-rbdma` at `R100_NBUS_L_RBDMA_CFG_BASE` with two GPIO-out lines connected to that chiplet's GIC at `R100_INT_ID_RBDMA0_ERR = 977` (idx 0) and `R100_INT_ID_RBDMA1 = 978` (idx 1). Resolves the `cfg-shadow=<id>` and `host-ram=<id>` machine string-props and threads the resulting `MemoryRegion` links into `r100-cm7.cfg-shadow` and `r100-pcie-outbound.host-ram`. Inline stubs: PCIE sub-controller (PHY{0..3}_SRAM_INIT_DONE seed + SUBCTRL_EDMA_INT_CA73 RAM at `0x1FF8184368` used as the per-channel HDMA pending bitmap), CSS600 CNTGEN, inter-chiplet HBM mailbox RAM, cfg/private-window unimpl catch-alls. |
| `r100_cmu.c` | CMU | 20 / chiplet | PLL-lock instant, `mux_busy=0`. |
| `r100_pmu.c` | PMU | 1 / chiplet | Cold-reset, `CPU_CONFIGURATION` → `arm_set_cpu_on(mpidr, RVBAR, EL3)` covers BL1 cold, BL2 CP1, BL31 PSCI warm. `read_rvbar()` indexes `SYSREG_CP0_PRIVATE + cluster * PER_SYSREG_CP`. Dual-mapped (cfg + private alias). |
| `r100_sysreg.c` | SYSREG | 1 / chiplet | Returns chiplet ID. CP0/CP1 RAM triple-mounted (private + cfg alias + CPU-view overlay). |
| `r100_hbm.c` | HBM3 | 1 / chiplet | Sparse `GHashTable` write-back (default `0xFFFFFFFF`), ICON RMW, PHY-training sentinels. |
| `r100_qspi.c` | QSPI bridge | 1 / chiplet | DW SSI inter-chiplet R/W (`0x70`/`0x80`/`0x83`) via `PRIVATE_BASE` + upper-addr latch. |
| `r100_qspi_boot.c` | QSPI_ROT (DWC_SSI) | 1 / chiplet | Dual-mapped; status always idle, DRX = `0x82`; covers BL1 flash-load + BL31 `NOR_FLASH_SVC_*`. |
| `r100_rbc.c` | RBC/UCIe | 6 / chiplet | Dual-mapped; `scratch_reg1 = 0xFFFFFFFF`, `lstatus_link_status = 1`. |
| `r100_smmu.{c,h}` | SMMU-600 TCU | 1 / chiplet | **MMIO** (128 KB region — page 0 SMMU-600 control block, page 1 `EVENTQ_PROD/CONS` at offset `0x100A8/AC`): `CR0→CR0ACK` + `GBPA.UPDATE` auto-clear; `STRTAB_BASE` / `STRTAB_BASE_CFG` / `EVENTQ_BASE` / `EVENTQ_PROD` / `EVENTQ_CONS` / `IRQ_CTRL.{EVENTQ,GERROR}_IRQEN` (mirrored into `IRQ_CTRLACK`) / `GERROR` / `GERRORN` cache the in-DRAM queue geometry and gate the fault-delivery path; `CMDQ_PROD` walks `(old_cons, new_prod]` and recognises `CMD_SYNC` + the full `CMD_TLBI_*` / `CMD_CFGI_*` / `CMD_PREFETCH_*` opcode set (logged, advance CONS as no-ops — v1 has no STE / IOTLB cache to invalidate). **Public translate API** (`r100_smmu_translate`): NPU-side engines that consume DVAs call this before `address_space_*`. Pre-`CR0.SMMUEN`: identity. Post-enable: read STE for `sid` from `STRTAB_BASE_PA + sid * 64`, decode `STE0.{V, config}` (`BYPASS`→identity, `ABORT`→`INV_STE` fault, `S1_TRANS`→v1 identity+LOG_UNIMP, `S2_TRANS`/`ALL_TRANS`→build `SMMUTransCfg` from STE2/STE3 and dispatch to QEMU's `smmu_ptw_64_s2`). `r100-rbdma` (OTO src+dst) and `r100-hdma` (LL walker LLP cursor + per-LLI SAR/DAR for D2D, NPU→host OP_WRITE chunks, host→NPU OP_READ_REQ chunks) translate through the hook via QOM `link<r100-smmu>` properties; SID 0 (PF) hardcoded per Notion REBELQ SMMU Design § 1. **Eventq + GERROR fault delivery (v2)**: every translate fault (`INV_STE`/`STE_FETCH`/walk) maps to a FW event_id (`r100_smmu_fault_to_event_id`, mirrors `q/sys/drivers/smmu/smmu.c:smmu_print_event` table — `INV_STE`→`C_BAD_STE=0x04`, `WALK`→`F_TRANSLATION=0x10`, `STE_FETCH`→`F_STE_FETCH=0x06`), packed as a 32 B SMMUv3 record (event_id + sid + input_addr + ipa, words 4-7 reserved), written to `EVENTQ_BASE_PA + (PROD&MASK)*32`, PROD advances with the wrap bit, GIC SPI 762 (`R100_INT_ID_SMMU_EVT`) pulses when `IRQ_CTRL.EVENTQ_IRQEN=1`. Eventq full → `r100_smmu_raise_gerror(EVTQ_ABT_ERR)` toggles `GERROR ^ GERRORN` (matches FW's `(GERROR ^ GERRORN) & ACTIVE_MASK` predicate), pulses GIC SPI 765 (`R100_INT_ID_SMMU_GERR`). Both IRQ outputs (`evt_irq` index 0, `gerr_irq` index 1) wired in `r100_create_smmu`. **v1 scope**: stage-2 only, PF only. **v2 done in this commit**: eventq + GERROR fault delivery. **Debug surface**: optional `debug-chardev` property (chiplet 0 only — single-frontend `CharBackend`) wired via `-machine r100-soc,smmu-debug=<id>`; one ASCII line per translate / STE decode / PT-walk dispatch / CMDQ op / eventq emit / GERROR raise. CLI `--host` mode auto-creates `output/<name>/smmu.log`; `tests/scripts/{mem_dump,smmu_decode}.py` decode the in-DRAM STE / PT bytes the trace points to. See `docs/debugging.md` → "SMMU debug surface". **v2 follow-ons** (gated on workload need): stage-1 + CD walk, multi-VF (SIDs 1..4), STE/IOTLB cache + honest invalidation, 2LVL stream tables, chiplet-0 PCIe-side TBU SID 17, dedicated HDMA-PA SID 16. See `docs/roadmap.md` → "SMMU v2" + `docs/rbdma-smmu-review.md`. |
| `r100_pvt.c` | PVT monitor | 5 / chiplet | `PVT_CON_STATUS=0x3`, per-sensor `_valid=1`. |
| `r100_dma.c` | PL330 DMA | 1 / chiplet | Fake-completion stub. |
| `r100_dnc.c` | DCL cluster | 2 / chiplet | Sparse stub for DNC slots, SHM banks, MGLUE (RDSN). Active task-completion path: writes to slot+0x81C (TASK_DESC_CFG1) with `access_size=4` and `itdone=1` enqueue a completion onto a per-cluster FIFO; a bottom-half drains the FIFO, latches a synthesised `dnc_reg_done_passage` (done_rpt0 = desc_id, done_rpt1 packed with dnc_id / chiplet_id / event_type=DONE / cmd_type) at slot+0xA00, and pulses the matching DNC GIC SPI from `r100_dnc_intid()` so q-cp's `dnc_X_done_handler` runs. The 64-bit writeq path used by `DNC_TASK_DESC_CONFIG_WRITE_NORECORD` (turq prologue) is filtered out by access-size to avoid spurious IRQs. `impl.max_access_size=8` so q-cp's `DNC_TASK_DONE_READQ` returns the latched payload coherently. |
| `r100_rbdma.{c,h}` | RBDMA (Rebel Block DMA) | 1 / chiplet | 1 MB MMIO at `R100_NBUS_L_RBDMA_CFG_BASE = 0x1FF3700000` per chiplet (offsets from `g_rbdma_memory_map.h` / `g_cdma_global_registers.h` / `g_cdma_task_registers.h`). Sparse `R100RBDMARegStore` (GHashTable offset → u32). **Synthetic IP_INFO seeds** (info0 = release-date, info1 = version stamp, info2 = chiplet_id, info3.num_of_executer = 8, info4.{num_tq,num_utq,num_ptq} = 32, info5.{num_teq,num_uetq,num_fnsh,num_err} = {32,32,32,8}). **"All-free" credit reporting** on `*TQUEUE_STATUS` reads. **Kick → done IRQ:** q-cp's `rbdma_send_task` writes 8 ascending words into `RBDMA_CDMA_TASK_BASE_OFF = 0x200`. The store on `RUN_CONF1` (+0x01C) is the silicon kickoff trigger — captures regstore-resident PTID_INIT, pushes an `R100RBDMAFnshEntry` onto a 32-deep ring, schedules a BH that pulses `INT_ID_RBDMA1 = 978` per pushed entry whose `RUN_CONF1.intr_disable` bit is clear. q-cp's done handler loops on `INTR_FIFO_READABLE_NUM` (live `fnsh_depth() & 0xFF`) and pops `FNSH_INTR_FIFO`. ERR slot (`INT_ID_RBDMA0_ERR = 977`, idx 0) is reserved for symmetry — never pulsed today. **OTO byte-mover:** for `task_type=OTO=0`, `r100_rbdma_do_oto` reconstructs 41-bit byte SAR/DAR (low 32 b in `SRCADDRESS_OR_CONST` / `DESTADDRESS`, high 2 b in `RUN_CONF0.{src,dst}_addr_msb`, all left-shifted by 7 from 128 B-units), translates each through `r100_smmu_translate(SID=0, …)` (identity when `CR0.SMMUEN=0`; honours stage-2 page tables otherwise), adds the chiplet base, and runs `address_space_read SAR → temp buf → address_space_write DAR` against `&address_space_memory`. Capped at `RBDMA_OTO_MAX_BYTES = 32 MiB`. Other task_types (CST/DAS/PTL/IVL/DUM/VCM/OTM/GTH/SCT/GTHR/SCTR) fall through to a `LOG_UNIMP` no-op + `unimp_task_kicks++`. Stats (`oto_kicks` / `oto_bytes` / `oto_dma_errors` / `unimp_task_kicks`) survive reset. |
| `r100_logbuf.c` | HILS ring tail | 1 (chiplet 0) | 50 ms poll of `.logbuf` ring, drains to chardev. |
| `r100_mailbox.{c,h}` | Samsung IPM SFR | 6 on chiplet 0 (VF0, PF; plus the four DNC task queues) | Full register model (`INTGR/INTCR/INTMR/INTSR/INTMSR/ISSR0..63`) + two `qemu_irq` outs (INTMSR0/1). All ISSR writes funnel through `r100_mailbox_issr_store(src, ...)` with an `MbxIssrSrc` tag (`NPU_MMIO` / `CM7_STUB` / `HOST_RELAY`); NPU-sourced writes egress an `(bar4_off, val)` frame on `issr-chardev`, host-relayed writes deliberately skip the re-emit to avoid loopback. Public helpers exposed via `r100_mailbox.h`: `r100_mailbox_{raise_intgr,set_issr,get_issr,cm7_stub_write_issr,set_issr_words,fw_boot_done_seen}`. Everything else stays `static`. |
| `r100_cm7.c` | PCIe doorbell ingress + cfg-mirror alias | 1 (chiplet 0) | Reassembles 8-byte frames via `remu_frame_rx_feed`; dispatches each via `remu_doorbell_classify()` onto INTGR / ISSR paths. The "CM7" name comes from PCIE_CM7's silicon role as the BAR/doorbell front door — REMU has no Cortex-M7 vCPU. Two responsibilities: **(1)** `INTGR0 bit 0 = SOFT_RESET` re-synthesises `FW_BOOT_DONE` into PF.ISSR[4], gated on the `r100_mailbox_fw_boot_done_seen(pf_mailbox)` latch (kept until P8 lands a real CA73 cluster soft-reset); **(2)** a 4 KB `MemoryRegion` alias at `R100_DEVICE_COMM_SPACE_BASE = 0x10200000` (prio 10 over chiplet-0 DRAM) over the shared `cfg-shadow` `memory-backend-file`. The host x86 QEMU's `r100-npu-pci` aliases the same backend over its BAR2 cfg-head subregion, so kmd writes to `FUNC_SCRATCH` / `DDH_BASE_LO` are visible to q-cp's next read with no chardev queue, and q-cp's `cb_complete → writel(FUNC_SCRATCH, magic)` lands in the shared backend and is observable on the kmd's next `rebel_cfg_read(FUNC_SCRATCH)`. `INTGR1 bit qid` (queue-doorbell) raises GIC SPI 185 to wake q-cp's `hq_task` on CP0 — q-cp owns the BD walk + mbtq push + DONE writeback + MSI-X natively (`hq_task → cb_task → cb_complete → pcie_msix_trigger`). One QOM link required (`pf-mailbox`, for the FW_BOOT_DONE re-handshake) plus the optional `cfg-shadow` `MemoryRegion *` link (unset for single-QEMU NPU-only m6/m8 tests; the alias is then skipped and chiplet-0 lazy RAM services any incidental access). Offsets classified as `OTHER` emit `GUEST_ERROR`. |
| `r100_hdma.{c,h}` | DesignWare dw_hdma_v0 reg block | 1 (chiplet 0) | MMIO at `R100_HDMA_BASE = 0x1D80380000` (32 × 0x800 channel slots, 16 WR + 16 RD). Per-channel state covers the full `struct hdma_ch_regs` surface from `q/cp/.../hdma_regs.h`. **Non-LL doorbell**: WR doorbell → `address_space_read` payload from chiplet sysmem at SAR + emit OP_WRITE; RD doorbell → emit OP_READ_REQ tagged `req_id = R100_HDMA_REQ_ID_CH_MASK_BASE | (dir<<5) | ch`, complete on matching OP_READ_RESP by `address_space_write` to chiplet sysmem at DAR. **LL doorbell** (`CTRL1.LLEN=1`): walks the chain at `LLP_LO|HI`, discriminating `dw_hdma_v0_lli` (24 B) from `dw_hdma_v0_llp` (16 B jump) on `control & LLP`; terminates on `LIE`. Per-LLI routing by SAR/DAR ≷ `REMU_HOST_PHYS_BASE`: WR && DAR≥host → chunked OP_WRITE; RD && SAR≥host → chunked OP_READ_REQ + parked `qemu_cond_wait_bql()` on a per-channel `resp_cond` until OP_READ_RESP arrives, then `address_space_write` at DAR; both NPU → D2D in-process loop (`address_space_read` → `address_space_write`, 64 KB scratch chunks). NPU-side SAR/DAR/cursor go through `r100_smmu_translate(SID=0, …)` before each `address_space_*` call (identity fallback when `CR0.SMMUEN=0`). Single GIC SPI 186 line + per-channel pending bit set in SUBCTRL_EDMA_INT_CA73 (`0x1FF8184368`, plain RAM in pcie-subctrl) on chain end. STOP-doorbell while idle is silently absorbed (matches DW silicon — gated on `c->enable & R100_HDMA_ENABLE_BIT`). Owns the `hdma` chardev (CharBackend is single-frontend); demuxes incoming OP_READ_RESP locally for the 0x80..0xBF channel-op partition. Public emit API in `r100_hdma.h` (`r100_hdma_emit_{write,write_tagged,read_req,read_resp}`). |
| `r100_pcie_outbound.c` | Chiplet-0 PCIe outbound iATU stub | 1 (chiplet 0) | 4 GB MMIO at `R100_PCIE_AXI_SLV_BASE_ADDR = 0x8000000000ULL`. Models the chiplet-0 DesignWare iATU PF window (`pcie_ep.c`: `cpu_addr=0x8000000000`, `pci_addr=0`, `size=4 GB`) so q-cp on CA73 CP0 can dereference kmd-published bus addresses (which already include `HOST_PHYS_BASE = 0x8000000000ULL`). Realised as a `MemoryRegion` alias over the shared `host-ram` `memory-backend-file` (the host x86 QEMU's main RAM). Both QEMUs `mmap` the same file, so q-cp's outbound loads/stores are plain TCG accesses against the same pages the kmd allocates with `dma_alloc_coherent` — no chardev hop, no `qemu_cond_wait_bql()`, no BQL contention with the kmd's `readl_poll_timeout_atomic`. The `host-ram` link is required when the device is instantiated; single-QEMU `./remucli run` boots don't create it, so the AXI window stays unmapped and unmodelled accesses surface as guest errors. |
| `r100_imsix.{c,h}` | PCIe MSI-X trap | 1 (chiplet 0) | 4 KB MMIO at `R100_PCIE_IMSIX_BASE`; 4-byte write @ `0xFFC` → `remu_frame_emit` of `(offset, db_data)` on `msix` chardev. Driven exclusively by q-cp's `pcie_msix_trigger` (`q/sys/osl/FreeRTOS/.../msix.c`) on CA73 CP0. The public `r100_imsix_notify(vec)` helper in `r100_imsix.h` is retained as a no-current-caller hook for any future post-soft-reset stub (P8). |
| `r100_npu_pci.c` (x86) | PCIe endpoint `0x1eff:0x2030` | 1 | Four BARs + four chardev bridges + two shared-memory backend links. BAR0 aliases the shared `remu-shm` `memory-backend-file`; BAR2 lazy RAM with a 4 KB prio-10 cfg-head subregion at `FW_LOGBUF_SIZE` aliased over the shared `cfg-shadow` `memory-backend-file`; BAR4 doorbell + ISSR shadow; BAR5 MSI-X. Host → NPU BAR4 writes use `remu_doorbell_classify()` to decide whether to forward (and to which sub-protocol). NPU → host MSI-X / ISSR ingress uses `RemuFrameRx`; the `hdma` chardev is bidirectional — `OP_WRITE` → `pci_dma_write`, `OP_READ_REQ` → `pci_dma_read` + `OP_READ_RESP`. |
| `src/include/r100/remu_addrmap.h` | — | — | Address constants from `g_sys_addrmap.h` + bridge offsets (BAR4 INTGR0/INTGR1/MAILBOX_BASE, BAR5 MSI-X, `R100_PCIE_IMSIX_*`), the DNC INTID lookup table (`r100_dnc_exc_intid_table[16]` + `r100_dnc_intid()` helper), HDMA region constants (`R100_HDMA_BASE/SIZE`, per-channel reg offsets, doorbell bits, status enum), the SUBCTRL_EDMA pending-bitmap offset, RBDMA INTIDs (`R100_INT_ID_RBDMA0_ERR = 977`, `R100_INT_ID_RBDMA1 = 978` from `q/cp/include/hal/interrupt.h`), and the `req_id` partitioning constants. Included as `"r100/remu_addrmap.h"`. |
| `src/bridge/remu_frame.h` | — | — | Header-only: `RemuFrameRx` streaming decoder, `remu_frame_rx_feed` + `remu_frame_emit` helpers, `RemuFrameEmitResult` for emit status. Used by every 8-byte chardev endpoint (doorbell / msix / issr). |
| `src/bridge/remu_doorbell_proto.h` | — | — | Header-only: `RemuDoorbellKind` + `remu_doorbell_classify(bar4_off, &issr_idx)` — single source of truth for the BAR4 wire protocol. Also carries the BAR2 cfg-head layout (`REMU_BAR2_CFG_HEAD_{OFF,SIZE}`), `REMU_HOST_PHYS_BASE` for host→chiplet address translation, and `REMU_CFG_FUNC_SCRATCH_OFF` for the cfg-mirror's `cb_complete` round-trip target. |
| `src/bridge/remu_hdma_proto.h` | — | — | Header-only: `RemuHdmaHeader` (24 B: magic `'HDMA'` LE, op, 64-bit dst, len, `req_id`) + payload, `RemuHdmaRx` streaming decoder, per-opcode emit helpers (`remu_hdma_emit_write{,_tagged}`, `remu_hdma_emit_read_req`, `remu_hdma_emit_read_resp`). Three live opcodes: `OP_WRITE`, `OP_READ_REQ/RESP`. `req_id` partitioning (canonical in `r100/remu_addrmap.h`): `0x00..0x7F` reserved (available for UMQ multi-queue per P9), `0x80..0xBF` r100-hdma MMIO-driven channel ops (encoded `0x80 \| (dir<<5) \| ch`), `0xC0..0xFF` reserved. |

## FW Source References

These files in `external/ssw-bundle` define the hardware behaviour the
emulator models. Shorthand: `$BUNDLE` = `external/ssw-bundle/products`.

| What | File |
|---|---|
| SoC address map | `$BUNDLE/rebel/q/sys/.../autogen/g_sys_addrmap.h` |
| Platform defs | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/include/platform_def.h` |
| CMU PLL polling | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/clk/clk_samsung/cmu.c` |
| PMU boot status | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_pmu.c` |
| Boot stages | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_bl{1,2,31}_setup.c` |
| QSPI bridge protocol | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/synopsys/qspi_bridge/qspi_bridge.c` |
| DWC_SSI / NOR flash | `$BUNDLE/rebel/q/sys/drivers/qspi_boot/qspi_boot.{c,h}` + `services/std_svc/nor_flash/nor_flash_main.c` |
| RBC/UCIe init | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_rbc.c` |
| PCI BARs + mmap | `$BUNDLE/common/kmd/rebellions/rebel/rebel.h` |
| FW-host handshake | `$BUNDLE/common/kmd/rebellions/common/{fw_if.c,ring.c,queue.c}` |
| Host interface (FW) | `$BUNDLE/rebel/q/cp/src/hil/hil.c` |
| RBDMA HAL driver | `$BUNDLE/rebel/q/cp/src/hal/rbdma/rebel/rbdma_if.c` |
| RBDMA register layout | `$BUNDLE/rebel/q/cp/src/hal/autogen/rebel/{g_rbdma_memory_map,g_cdma_global_registers,g_cdma_task_registers}.h` |
| q-cp interrupt IDs | `$BUNDLE/rebel/q/cp/include/hal/interrupt.h` |
