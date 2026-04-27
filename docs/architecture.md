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

Two QEMU instances connected by shared memory. `./remucli build`
produces both `qemu-system-{aarch64,x86_64}` from one pinned source
tree; `./remucli run --host` wires them up.

```
┌─ Host QEMU (x86_64 + KVM) ───────────────────────────────┐
│  umd + kmd (unmodified) → PCI 1eff:2030                   │
│    BAR0=DRAM  BAR2=ACP  BAR4=doorbell  BAR5=MSI-X         │
│  r100-npu-pci device                                       │
│    BAR0 memdev splice (M4)                                 │
│    BAR2 4 KB cfg-head trap at FW_LOGBUF_SIZE (M8b 3b)      │
│    BAR4 4 KB MMIO head: INTGR + MAILBOX_BASE (M6+M8a)      │
│    BAR5 msix_init table + lazy RAM (M3)                    │
│  chardevs: doorbell, cfg (out) / msix, issr (in) /         │
│            hdma (bidir)                                    │
└──┬──────┬────────┬─────┬──────┬─────┬─────────────────────┘
   │ shm  │doorbell│ cfg │ msix │issr │ hdma
┌──┴──────┴────────┴─────┴──────┴─────┴─────────────────────┐
│  NPU QEMU (aarch64, bare-metal)                            │
│  32 × CA73 (cortex-a72 w/ MIDR=A73 r1p1)                   │
│  TF-A BL1 → BL2 → BL31 → FreeRTOS                          │
│  Chiplet-0 peripherals wired to chardev bridges:           │
│    r100-cm7  (bidir)  — INTGR / ISSR ingress, cfg_shadow,  │
│                         CM7 stubs (SOFT_RESET, QINIT,      │
│                         BD-done SM per queue, mbtq push    │
│                         with cmd_descr synth)              │
│    r100-hdma (bidir)  — dw_hdma_v0 reg block + chardev     │
│                         owner; cm7 talks through QOM link  │
│    r100-mailbox VF0   — INTID 184/185 (gpio_in[152/153]),  │
│                         INTGR                              │
│    r100-mailbox PF    — bootdone, issr egress              │
│    r100-mailbox M9    — q-cp DNC compute task queue        │
│    r100-imsix (out)   — MSI-X egress                       │
│  Per-chiplet: CMU, PMU, HBM3, QSPI, RBC, SMMU,             │
│   PVT, DNC cfg (active task-completion path on chiplet 0), │
│   SYSREG CP{0,1} triple-mount,                             │
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

**Scaffolding vs destination.** A subset of `r100-cm7`'s
responsibilities are intentionally regression scaffolding rather than
the silicon-accurate target: specifically the synthetic `FW_BOOT_DONE`
re-handshake on `INTGR0 bit 0` (Stage 3a), the QINIT write-back on
`INTGR1 bit 7` (Stage 3b QINIT stub), the BD-done state machine on
`INTGR1 bits 0..N` (Stage 3c), and the M9-1b mbtq push. P1c
(`docs/roadmap.md`) gated all of (3b + 3c + M9-1b) behind
`-global r100-cm7.{qinit,bd-done,mbtq}-stub=on` boolean properties
(default off): q-cp on CP0 now owns the QINIT write-back natively
(`hil_init_descs` + DDH publish through the P1a outbound iATU + P1b
cfg-mirror trap), the BD walk (`hq_task → cb_task → cb_complete`),
and the mbtq push (`mtq_push_task`). MSI-X completion goes through
q-sys's `osl/FreeRTOS/.../msix.c` `pcie_msix_trigger` writing the
`r100-imsix` MMIO — that path was always present, P1 just made it
reachable. The stubs stay compiled in for bisecting q-cp regressions
(see `docs/debugging.md` → "P1b/P1c"). Stage 3a (the synthetic
`FW_BOOT_DONE`) remains the only default-on cm7 stub; **P8** retires
it once a real CA73 soft-reset model lands. **P2** then deleted the
trailing `r100_imsix_notify(s->imsix, j->qid)` call inside the
Stage 3c FSM so even with `bd-done-stub=on` only `cb_complete →
pcie_msix_trigger` fires MSI-X (no duplicate fires desyncing
`host/msix.log` during bisects); the `imsix` QOM link + state field
stay wired so **P7** retires them with the rest of the gated-off
Stage 3c FSM in one cleanup pass. Treat the file map below as
"what the binary does today (with everything default-on)", the
default-off paragraph as "what runs end-to-end on a stock
`./remucli run --host`", and the roadmap as "what we are moving
toward".

### Key Design Decisions

| Decision | Choice | Why |
|---|---|---|
| CPU model | `cortex-a72` + MIDR spoofed to CA73 r1p1 (`0x411FD091`) + two IMPDEF sysreg tables (`r100_samsung_impdef_regs`, `r100_cortex_a73_impdef_regs` in `r100_soc.c`) | Closest QEMU model; r1p1 MIDR skips revision-gated CA73 errata; the WA paths TF-A always runs (CVE-2018-3639) read/write IMPDEF regs we register as RAZ/WI. See commit `7a2e232`. |
| GIC | One `arm-gicv3` per chiplet (`num-cpu=8`, `first-cpu-index=N*8`) | Matches silicon; `first-cpu-index` added by `cli/qemu-patches/0001-arm-gicv3-first-cpu-index.patch` so multi-GIC machines bind disjoint CPU ranges. Generic-timer outs wired to PPIs 30/27/26/29/28 (same layout as `hw/arm/virt.c`); without this FreeRTOS CNTVIRQ tick wedges on the first `vTaskDelay` — see commit `680f964`. |
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
    remu_doorbell_proto.h BAR4 offset classifier + BAR2 cfg-head layout + DDH desc offsets
    remu_hdma_proto.h     24 B header + payload bidirectional HDMA protocol
                          (OP_WRITE / READ_REQ / READ_RESP / CFG_WRITE)
```

**Header discipline.** Each device's `struct R100XxxState`,
`DECLARE_INSTANCE_CHECKER(...)`, and private register-count defines
live in its own `.c` file. `r100_soc.h` only exposes the machine-state
subclass, the `TYPE_R100_*` type names, and (via `#include
"r100_mailbox.h"` / `#include "r100_imsix.h"`) the mailbox + imsix
helper prototypes that `r100_cm7.c` needs to drive both devices from
outside their own MMIO path. This keeps the state layout private,
forces cross-device communication through either QOM properties (e.g.
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
| `r100_soc.c` | Machine | 1 | Builds chiplet views + CPUs + per-chiplet GIC/UART + mailbox cluster. Installs MIDR spoof + two IMPDEF sysreg tables per CPU. Wires gtimer→GIC PPIs. GIC `num-irq=992` (was 256 pre-M9-1c) so DNC INTIDs (up to 617) and the broader q-cp interrupt set fit. Instantiates `r100-cm7` / `r100-imsix` / `r100-hdma` on demand (and wires the QOM `imsix` + `mbtq-mailbox` + `hdma` links on `r100-cm7` — the `imsix` link is dead since P2, kept only so P7 can retire it alongside the rest of the gated Stage 3c FSM; the `mbtq-mailbox` link routes the M9-1b mbtq push to the right mailbox; the `hdma` link feeds the new chardev-owning device); also instantiates four real `r100-mailbox` blocks on chiplet 0 for the q-cp DNC task queues (P3): COMPUTE at `R100_PERI0_MAILBOX_M9_BASE`, UDMA at `R100_PERI0_MAILBOX_M10_BASE`, UDMA_LP at `R100_PERI1_MAILBOX_M9_BASE`, UDMA_ST at `R100_PERI1_MAILBOX_M10_BASE` — matching q-cp's `_inst[HW_SPEC_DNC_QUEUE_NUM=4]`. The chiplet/mailbox lazy-RAM loop skips all four chiplet-0 task-queue slots; chiplets 1..3 keep lazy-RAM since q-cp/CP1 only runs on chiplet 0. For each DCL on each chiplet, wires 8 slots × 4 cmd_types = 32 DNC done SPIs to that chiplet's GIC via `r100_dnc_intid()` (M9-1c). 13 `-machine r100-soc,<key>=<id>` string props — `memdev` plus 6 chardev slots (`doorbell`, `msix`, `issr`, `cfg`, `hdma`, `cm7_debug`) each with a paired `*_debug` log target — registered via the `R100_SOC_DEF_STRPROP` macro-generated table. The `hdma` chardev now binds to `r100-hdma` (M9-1c) instead of `r100-cm7`. Inline stubs: PCIE sub-controller (PHY{0..3}_SRAM_INIT_DONE seed + SUBCTRL_EDMA_INT_CA73 RAM at `0x1FF8184368` used as the per-channel HDMA pending bitmap), CSS600 CNTGEN, inter-chiplet HBM mailbox RAM, cfg/private-window unimpl catch-alls. |
| `r100_cmu.c` | CMU | 20 / chiplet | PLL-lock instant, `mux_busy=0`. |
| `r100_pmu.c` | PMU | 1 / chiplet | Cold-reset, `CPU_CONFIGURATION` → `arm_set_cpu_on(mpidr, RVBAR, EL3)` covers BL1 cold, BL2 CP1, BL31 PSCI warm. `read_rvbar()` indexes `SYSREG_CP0_PRIVATE + cluster * PER_SYSREG_CP`. Dual-mapped (cfg + private alias). |
| `r100_sysreg.c` | SYSREG | 1 / chiplet | Returns chiplet ID. CP0/CP1 RAM triple-mounted (private + cfg alias + CPU-view overlay). |
| `r100_hbm.c` | HBM3 | 1 / chiplet | Sparse `GHashTable` write-back (default `0xFFFFFFFF`), ICON RMW, PHY-training sentinels. |
| `r100_qspi.c` | QSPI bridge | 1 / chiplet | DW SSI inter-chiplet R/W (`0x70`/`0x80`/`0x83`) via `PRIVATE_BASE` + upper-addr latch. |
| `r100_qspi_boot.c` | QSPI_ROT (DWC_SSI) | 1 / chiplet | Dual-mapped; status always idle, DRX = `0x82`; covers BL1 flash-load + BL31 `NOR_FLASH_SVC_*`. |
| `r100_rbc.c` | RBC/UCIe | 6 / chiplet | Dual-mapped; `scratch_reg1 = 0xFFFFFFFF`, `lstatus_link_status = 1`. |
| `r100_smmu.c` | SMMU-600 TCU | 1 / chiplet | `CR0→CR0ACK` + `GBPA.UPDATE` auto-clear; CMDQ walker zeroes `CMD_SYNC.MSI_ADDRESS` and auto-advances `CMDQ_CONS=PROD`. |
| `r100_pvt.c` | PVT monitor | 5 / chiplet | `PVT_CON_STATUS=0x3`, per-sensor `_valid=1`. |
| `r100_dma.c` | PL330 DMA | 1 / chiplet | Fake-completion stub. |
| `r100_dnc.c` | DCL cluster + RBDMA | 2+1 / chiplet | Sparse stub for DNC slots, SHM banks, MGLUE (RDSN), RBDMA `IP_INFO`. **M9-1c active path:** writes to slot+0x81C (TASK_DESC_CFG1) with `access_size=4` and `itdone=1` enqueue a completion onto a per-cluster FIFO; a bottom-half drains the FIFO, latches a synthesised `dnc_reg_done_passage` (done_rpt0 = desc_id, done_rpt1 packed with dnc_id / chiplet_id / event_type=DONE / cmd_type) at slot+0xA00, and pulses the matching DNC GIC SPI from `r100_dnc_intid()` so q-cp's `dnc_X_done_handler` runs. The 64-bit writeq path used by `DNC_TASK_DESC_CONFIG_WRITE_NORECORD` (turq prologue) is filtered out by access-size to avoid spurious IRQs. `impl.max_access_size=8` so q-cp's `DNC_TASK_DONE_READQ` returns the latched payload coherently. |
| `r100_logbuf.c` | HILS ring tail | 1 (chiplet 0) | 50 ms poll of `.logbuf` ring, drains to chardev. |
| `r100_mailbox.{c,h}` | Samsung IPM SFR | 6 on chiplet 0 (VF0, PF; plus the four DNC task queues PERI0_MAILBOX_M9_CPU1 / PERI0_MAILBOX_M10_CPU1 / PERI1_MAILBOX_M9_CPU1 / PERI1_MAILBOX_M10_CPU1, P3) | Full register model (`INTGR/INTCR/INTMR/INTSR/INTMSR/ISSR0..63`) + two `qemu_irq` outs (INTMSR0/1). All ISSR writes funnel through `r100_mailbox_issr_store(src, ...)` with an `MbxIssrSrc` tag (`NPU_MMIO` / `CM7_STUB` / `HOST_RELAY`); NPU-sourced writes egress an `(bar4_off, val)` frame on `issr-chardev` (M8a), host-relayed writes deliberately skip the re-emit to avoid loopback. Public helpers exposed via `r100_mailbox.h`: `r100_mailbox_{raise_intgr,set_issr,get_issr,cm7_stub_write_issr,set_issr_words}` — last is the M9-1b in-process multi-slot write that bypasses the funnel for NPU-internal mailboxes (q-cp task queue) where there is no host mirror. Everything else stays `static`. |
| `r100_cm7.c` | PCIe doorbell ingress + CM7 stubs | 1 (chiplet 0; M6+M8a+3a+3b+3c+M9-1b/c+P1b/c) | Reassembles 8-byte frames via `remu_frame_rx_feed`; dispatches each via `remu_doorbell_classify()` onto INTGR / ISSR / CM7-stub paths. The "CM7" name comes from PCIE_CM7's silicon role as the BAR/doorbell front door — REMU has no Cortex-M7 vCPU, so this device emulates that front door plus a small set of stubs that stand in for behaviours we don't fully model. Five stubs in total: stubs (2) and (3) are honest infrastructure (host↔NPU cfg-head mirror + DDH publish handshake), stubs (1), (4), (5) are scaffolding (P1c retired (4)+(5) and bypassed (3) — kept compiled in behind `-global r100-cm7.{bd-done,mbtq,qinit}-stub=on` for bisecting q-cp regressions). **(1)** `INTGR0 bit 0 = SOFT_RESET` re-synthesises `FW_BOOT_DONE` into PF.ISSR[4] (M8b 3a, `a01d2b5`); **(2)** `cfg_shadow[1024]` mirror of host BAR2 cfg-head — host writes ingress on the `cfg` chardev and update the shadow; P1b adds a 4 KB MMIO trap (`r100_cm7_cfg_mirror_ops`, prio 10 over chiplet-0 DRAM at `R100_DEVICE_COMM_SPACE_BASE = 0x10200000`) so NPU-side reads of DDH fields hit the same shadow (real silicon: inbound iATU; REMU: a single source of truth on the NPU side). NPU-side writes (q-cp `cb_complete → writel(FUNC_SCRATCH, magic)`) update `cfg_shadow` and forward an `OP_CFG_WRITE` over `hdma` so the host's `cfg_mmio_regs[]` stays consistent — this is the path that makes `rbln_queue_test`'s `rebel_cfg_read(FUNC_SCRATCH)` see the magic the firmware wrote. The trap accepts 1/2/4/8-byte accesses (impl-size 4) so q-sys CP0's cold-boot `memset(DEVICE_COMMUNICATION_SPACE_BASE, 0, CP1_LOGBUF_MAGIC)` flows through unmodified; **(3)** `INTGR1 bit 7 = QUEUE_INIT` runs `r100_cm7_qinit_stub` which emits two `HDMA_OP_WRITE` frames (`fw_version = "3.remu-stub"` + `init_done = 1`, M8b 3b) — default off post-P1c; q-cp on CP0 publishes the real `fw_version` + `init_done = 1` itself; **(4)** `INTGR1 bits 0..N-1 = QUEUE_M_START` drove a per-queue `R100Cm7BdJob` async state machine (M8b 3c) — snapshots `ISSR[qid]` as `pi`, issues tagged `OP_READ_REQ` for queue descriptor / buffer descriptor / packet payload (`req_id = qid + 1`), commits via `OP_CFG_WRITE FUNC_SCRATCH` + `OP_WRITE bd.header |= DONE` + `OP_WRITE queue_desc.ci++`, then transitioned to `IDLE complete` (P2 deleted the trailing `r100_imsix_notify(vec = qid)` so `cb_complete → pcie_msix_trigger` is the single MSI-X source even with `bd-done-stub=on`). Default off post-P1c; q-cp's `hq_task → cb_task → cb_complete` runs the same walk natively via the P1a outbound iATU + this trap; **(5)** on the same `INTGR1 bits 0..N-1`, `r100_cm7_mbtq_push` wrote a 24 B `dnc_one_task` to the `mbtq-mailbox` link (PERI0_MAILBOX_M9_CPU1 ISSR ring) + bumped `MBTQ_PI_IDX` to wake q-cp's poll-based DNC task master, with a synthesised `cmd_descr` (cmd_type=COMPUTE, `dnc_task_conf.core_affinity=BIT(0)`) staged in chiplet-0 private DRAM at `R100_CMD_DESCR_SYNTH_BASE = 0x20000000` (16 × 256 B ring). Default off post-P1c; the natural `mtq_push_task` on CP0 owns this once `cb_task` runs end-to-end. Hosts the optional `cm7-debug` chardev and four QOM links (`pf-mailbox`, `imsix`, `mbtq-mailbox`, `hdma`). The `hdma` chardev moved to `r100-hdma` in M9-1c — cm7's emit helpers call through `r100_hdma_emit_*`, and BD-done OP_READ_RESPs come back via `r100_hdma_set_cm7_callback()`. Offsets classified as `OTHER` emit `GUEST_ERROR`. |
| `r100_hdma.{c,h}` | DesignWare dw_hdma_v0 reg block | 1 (chiplet 0; M9-1c) | MMIO at `R100_HDMA_BASE = 0x1D80380000` (32 × 0x800 channel slots, 16 WR + 16 RD). Per-channel state for SAR/DAR/XferSize/doorbell/status/int_status. WR doorbell → `address_space_read` payload from chiplet sysmem at SAR + emit OP_WRITE; RD doorbell → emit OP_READ_REQ tagged `req_id = R100_HDMA_REQ_ID_CH_MASK_BASE \| (dir<<5) \| ch`, complete on matching OP_READ_RESP by `address_space_write` to chiplet sysmem at DAR. Single-line GIC SPI 186 + per-channel pending bit set in SUBCTRL_EDMA_INT_CA73 (`0x1FF8184368`, plain RAM in pcie-subctrl). Owns the `hdma` chardev (CharBackend is single-frontend); demuxes incoming OP_READ_RESP by req_id range — local for 0x80..0xBF channel ops, dispatches via `R100HDMARespCb` callback into `r100-cm7` for cm7's BD-done partition (1..R100_CM7_MAX_QUEUES) and into `r100-pcie-outbound` for the P1a partition (0xC0..0xFF). Public emit API in `r100_hdma.h` (cm7 + outbound callbacks registered separately). |
| `r100_pcie_outbound.c` | Chiplet-0 PCIe outbound iATU stub | 1 (chiplet 0; P1a) | 4 GB MMIO at `R100_PCIE_AXI_SLV_BASE_ADDR = 0x8000000000ULL`. Models the chiplet-0 DesignWare iATU PF window (`pcie_ep.c`: `cpu_addr=0x8000000000`, `pci_addr=0`, `size=4 GB`) so q-cp on CA73 CP0 can dereference kmd-published bus addresses (which already include `HOST_PHYS_BASE = 0x8000000000ULL`). Reads emit `HDMA_OP_READ_REQ` over the existing `hdma` chardev with `req_id` in the `0xC0..0xFF` partition (rotating 6-bit cookie; single in flight) and block on `qemu_cond_wait_bql()` until `r100-hdma` delivers the matching `OP_READ_RESP` via `r100_hdma_set_outbound_callback()`. Writes are fire-and-forget `OP_WRITE`. `valid.{min,max}_access_size = {1,8}`, `valid.unaligned = true` so any AArch64 load/store width works. Only realised when `hdma` chardev is bound — single-QEMU `./remucli run` boots leave the AXI window unmapped, so unmodelled accesses still surface as guest errors. |
| `r100_imsix.{c,h}` | PCIe MSI-X trap | 1 (chiplet 0; M7) | 4 KB MMIO at `R100_PCIE_IMSIX_BASE`; 4-byte write @ `0xFFC` → `remu_frame_emit` of `(offset, db_data)` on `msix` chardev. Driven exclusively by q-cp's `pcie_msix_trigger` (`q/sys/osl/FreeRTOS/.../msix.c`) on CA73 CP0 — `cb_complete` MSI-X stores trap into this device naturally. P2 deleted the only other caller (`r100_cm7`'s Stage 3c FSM `r100_imsix_notify(j->qid)`) to make `cb_complete` the single source of truth, even with `bd-done-stub=on`. The public `r100_imsix_notify(vec)` helper in `r100_imsix.h` is retained as a no-current-caller hook for any future post-soft-reset stub (P8). |
| `r100_npu_pci.c` (x86) | PCIe endpoint `0x1eff:0x2030` | 1 | Four BARs + five chardev bridges. Host → NPU BAR4 writes use `remu_doorbell_classify()` to decide whether to forward (and to which sub-protocol). BAR2 has a 4 KB prio-10 MMIO trap at `FW_LOGBUF_SIZE` that re-emits writes on the `cfg` chardev (M8b 3b). NPU → host MSI-X / ISSR ingress uses `RemuFrameRx`; the `hdma` chardev is bidirectional (M8b 3b+3c) — `OP_WRITE` → `pci_dma_write`, `OP_READ_REQ` → `pci_dma_read` + `OP_READ_RESP`, `OP_CFG_WRITE` → updates host-local `cfg_mmio_regs`. See commits `7b03328` / `e03b00f` / `cd24aa9` for per-milestone detail. |
| `src/include/r100/remu_addrmap.h` | — | — | Address constants from `g_sys_addrmap.h` + M6/M7/M8a bridge offsets (BAR4 INTGR0/INTGR1/MAILBOX_BASE, BAR5 MSI-X, `R100_PCIE_IMSIX_*`). M9-1c added the DNC INTID lookup table (`r100_dnc_exc_intid_table[16]` + `r100_dnc_intid()` helper), HDMA region constants (`R100_HDMA_BASE/SIZE`, per-channel reg offsets, doorbell bits, status enum), the SUBCTRL_EDMA pending-bitmap offset, the COMPUTE cmd_type, the synthesised cmd_descr ring base/stride/count, and the req_id partitioning constants. Included as `"r100/remu_addrmap.h"`. |
| `src/bridge/remu_frame.h` | — | — | Header-only: `RemuFrameRx` streaming decoder, `remu_frame_rx_feed` + `remu_frame_emit` helpers, `RemuFrameEmitResult` for emit status. Used by every 8-byte chardev endpoint (doorbell / msix / issr / cfg). |
| `src/bridge/remu_doorbell_proto.h` | — | — | Header-only: `RemuDoorbellKind` + `remu_doorbell_classify(bar4_off, &issr_idx)` — single source of truth for the BAR4 wire protocol. Also carries the BAR2 cfg-head layout (`REMU_BAR2_CFG_HEAD_{OFF,SIZE}`), the DDH desc field offsets (`REMU_DDH_{DRIVER_VERSION,INIT_DONE,FW_VERSION}_OFF`), `REMU_HOST_PHYS_BASE` for host→chiplet address translation, and the `REMU_DB_QUEUE_INIT_INTGR1_BIT` constant used by the M8b 3b stub. |
| `src/bridge/remu_hdma_proto.h` | — | — | Header-only: `RemuHdmaHeader` (24 B: magic `'HDMA'` LE, op, 64-bit dst, len, `req_id`) + payload, `RemuHdmaRx` streaming decoder, per-opcode emit helpers (`remu_hdma_emit_write{,_tagged}`, `remu_hdma_emit_read_req`, `remu_hdma_emit_read_resp`, `remu_hdma_emit_cfg_write`). Four opcodes: `OP_WRITE` (NPU→host, QINIT + BD-done writes), `OP_READ_REQ/RESP` (bidirectional tagged read, Stage 3c BD walk + P1a outbound iATU), `OP_CFG_WRITE` (NPU→host cfg shadow update). `req_id` partitioned across NPU-side senders (canonical in `r100/remu_addrmap.h`): `0x00` untagged QINIT, `0x01..0x0F` r100-cm7 BD-done (`qid+1`), `0x80..0xBF` r100-hdma MMIO-driven channel ops (encoded `0x80 \| (dir<<5) \| ch`), `0xC0..0xFF` r100-pcie-outbound synchronous PF-window reads (P1a — cookie rotates over the low 6 bits, only one in flight). |

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
