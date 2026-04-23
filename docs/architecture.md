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
┌─ Host QEMU (x86_64 + KVM) ──────────────────────┐
│  umd + kmd (unmodified) → PCI 1eff:2030          │
│    BAR0=DRAM  BAR2=ACP  BAR4=doorbell  BAR5=MSI-X│
│  r100-npu-pci device                              │
│    BAR0 memdev splice (M4)                        │
│    BAR4 4 KB MMIO head: INTGR + MAILBOX_BASE (M6+M8a)│
│    BAR5 msix_init table + lazy RAM (M3)           │
│  chardevs: doorbell (out) / msix (in) / issr (in) │
└────────┬─────────┬─────────┬──────────┬──────────┘
         │ shm     │ doorbell│ msix     │ issr
┌────────┴─────────┴─────────┴──────────┴──────────┐
│  NPU QEMU (aarch64, bare-metal)                  │
│  32 × CA73 (cortex-a72 w/ MIDR=A73 r1p1)         │
│  TF-A BL1 → BL2 → BL31 → FreeRTOS                │
│  Chiplet-0 peripherals wired to chardev bridges: │
│    r100-doorbell (in)  — INTGR / ISSR ingress    │
│    r100-mailbox VF0    — SPI 184/185, INTGR      │
│    r100-mailbox PF     — bootdone, issr egress   │
│    r100-imsix (out)    — MSI-X egress            │
│  Per-chiplet: CMU, PMU, HBM3, QSPI, RBC, SMMU,   │
│   PVT, DNC cfg, SYSREG CP{0,1} triple-mount,     │
│   arm-gicv3 (num-cpu=8, first-cpu-index=N*8),    │
│   16550 UART (SPI 33), HILS ring tail (logbuf)   │
│  CPU gtimer→GIC PPI (CNTVIRQ = PPI 27 → tick)    │
└──────────────────────────────────────────────────┘
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

## Source File Map

| File | Device | Instances | Behaviour |
|---|---|---|---|
| `r100_soc.c` | Machine | 1 | Builds chiplet views + CPUs + per-chiplet GIC/UART + mailbox cluster. Installs MIDR spoof + two IMPDEF sysreg tables per CPU. Wires gtimer→GIC PPIs. Instantiates `r100-doorbell` / `r100-imsix` on demand. Inline stubs: PCIE sub-controller (PHY{0..3}_SRAM_INIT_DONE seed), CSS600 CNTGEN, inter-chiplet HBM mailbox RAM, cfg/private-window unimpl catch-alls. |
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
| `r100_dnc.c` | DCL cluster + RBDMA | 2+1 / chiplet | Sparse stub for DNC slots, SHM banks, MGLUE (RDSN), RBDMA `IP_INFO`. |
| `r100_logbuf.c` | HILS ring tail | 1 (chiplet 0) | 50 ms poll of `.logbuf` ring, drains to chardev. |
| `r100_mailbox.c` | Samsung IPM SFR | 2 on chiplet 0 (VF0, PF) | Full register model (`INTGR/INTCR/INTMR/INTSR/INTMSR/ISSR0..63`), two `qemu_irq` outs (INTMSR0/1), API `r100_mailbox_{raise_intgr,set_issr,cm7_stub_write_issr}`. Every MMIO-path ISSR write emits `(bar4_off, val)` frame on optional `issr-chardev` (M8a). |
| `r100_doorbell.c` | PCIe doorbell ingress | 1 (chiplet 0; M6+M8a+CM7-stub) | Reassembles 8-byte `(offset, value)` frames; routes by offset to INTGR / ISSR / CM7-stub (INTGR0 bit 0 = SOFT_RESET → synthesises `FW_BOOT_DONE` into PF.ISSR[4], commit `a01d2b5`). |
| `r100_imsix.c` | PCIe MSI-X trap | 1 (chiplet 0; M7) | 4 KB MMIO at `R100_PCIE_IMSIX_BASE`; 4-byte write @ `0xFFC` emits `(offset, db_data)` frame on `msix` chardev. |
| `r100_npu_pci.c` (x86) | PCIe endpoint `0x1eff:0x2030` | 1 | Four BARs + three chardev bridges — see commit `7b03328` / `e03b00f` / `cd24aa9` for per-milestone detail. |
| `remu_addrmap.h` | — | — | Address constants from `g_sys_addrmap.h` + M6/M7/M8a bridge offsets. |

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
