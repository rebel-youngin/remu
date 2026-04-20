# REMU Architecture

## Overview

REMU is a QEMU-based system emulator for the R100 (CR03 quad) NPU. It functionally emulates the NPU hardware so that existing firmware (q-sys, q-cp) and drivers (kmd, umd) run unmodified. The primary audience is FW developers who need fast local iteration without hardware access.

This is **not** a cycle-accurate simulator. Timing is not modeled — PLLs lock instantly, DMA completes in zero time, and there is no pipeline or cache simulation. The goal is functional correctness: the FW sees the right register values, memory maps, and interrupt behavior to boot and run.

## R100 SoC Hardware Overview

The R100 is a multi-chiplet NPU with:

- **4 chiplets**, each at a `0x2000000000` address offset from the previous
- **8 ARM CA73 cores per chiplet**: 2 clusters (CP0, CP1) x 4 cores = 32 cores total
- **Per-chiplet peripherals**: CMU (clocks), PMU (power), HBM3 (memory), DNC (compute), HDMA (DMA)
- **Inter-chiplet links**: UCIe via 6 RBC blocks, QSPI bridge for register access
- **Host interface**: PCIe endpoint with BAR-mapped DRAM, config, and doorbell regions

Real hardware:

```
  Host (x86/ARM server)
    │ PCIe
    ▼
  ┌─ R100 NPU Card ─────────────────────────────┐
  │  Chiplet 0 (primary)    Chiplet 1            │
  │  ┌──────────────────┐  ┌──────────────────┐  │
  │  │ CP0 (4x CA73)    │  │ CP0 (4x CA73)    │  │
  │  │ CP1 (4x CA73)    │  │ CP1 (4x CA73)    │  │
  │  │ 16x DNC cores    │  │ 16x DNC cores    │  │
  │  │ HDMA, HBM3       │  │ HDMA, HBM3       │  │
  │  └───────┬──────────┘  └──────────┬───────┘  │
  │          │ UCIe (RBC)             │           │
  │  ┌───────┴──────────┐  ┌──────────┴───────┐  │
  │  │ Chiplet 2        │  │ Chiplet 3        │  │
  │  │ (same as above)  │  │ (same as above)  │  │
  │  └──────────────────┘  └──────────────────┘  │
  └──────────────────────────────────────────────┘
```

## Emulator Architecture

### Dual-QEMU Model

REMU uses two QEMU instances connected by shared memory:

```
┌─ Host QEMU (x86_64 + KVM) ─────────────────────┐
│  umd (userspace, unmodified)                     │
│  kmd (kernel module, compiled for x86_64)        │
│    └─ virtual PCI device: 0x1eff:0x2030 (CR03)  │
│       BAR0=DRAM  BAR2=config  BAR4=doorbell      │
├──────────────────────────────────────────────────┤
│  r100-npu-pci (QEMU device model)                │
│    └─ BARs backed by /dev/shm/remu-*             │
│    └─ doorbell → eventfd → FW QEMU               │
│    └─ MSI-X ← eventfd ← FW QEMU                 │
└────────────┬──────────────────┬──────────────────┘
             │ shared memory    │ eventfd
┌────────────┴──────────────────┴──────────────────┐
│  FW QEMU (aarch64, bare-metal)                   │
│  32x CA73 vCPUs (cortex-a72 w/ MIDR=A73 r1p1)    │
│  TF-A BL1 → BL2 → BL31 → FreeRTOS              │
│  ┌────────────────────────────────────────────┐  │
│  │ Device Models                              │  │
│  │  CMU (20 blocks) — PLL lock stubs          │  │
│  │  PMU — boot + arm_set_cpu_on on RVBAR      │  │
│  │  SYSREG — chiplet ID (per-chiplet view)    │  │
│  │  HBM3 — sparse regs, training-ready        │  │
│  │  QSPI bridge — cross-chiplet register I/O  │  │
│  │  QSPI_ROT (DWC_SSI) — flash boot + NOR SMC │  │
│  │  RBC — UCIe link-up status                 │  │
│  │  SMMU-600 TCU — CR0ACK / GBPA / CMDQ+SYNC  │  │
│  │  PVT — idle/valid-bit stub (pvt_init)      │  │
│  │  HILS ring tail — drains FreeRTOS .logbuf  │  │
│  │  Mailbox RAM — inter-chiplet handshake     │  │
│  │  GIC600, per-chiplet 16550 UART, Timer     │  │
│  └────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────┘
```

**Why two instances?** The FW runs bare-metal AArch64 at EL3/EL1 with a 40-bit physical address space starting at `0x1E00000000`. The host kmd runs inside a Linux kernel. These are fundamentally incompatible execution environments. A single QEMU with heterogeneous clusters (like Xilinx ZynqMP) would be far more complex to build and maintain.

**Why a QEMU fork?** The machine type needs a custom memory map, custom device models, and custom boot loading. QEMU does not support out-of-tree device compilation, so the device models are symlinked into the QEMU source tree and compiled in-tree.

### Per-Chiplet CPU Memory View

On silicon, the 256 MB `PRIVATE_BASE` window (`0x1E00000000`) is a chiplet-local address-decoder alias: each chiplet's CPUs see their own private peripherals (SYSREMAP, CPMU, CP0/CP1 SYSREG, RBC, OTP, ...) at these addresses. The FW relies on this routing to read `CHIPLET_ID` from `SYSREG_SYSREMAP_PRIVATE+0x444` without having to know which chiplet it runs on, and to execute the same BL1/BL2 binary on all 4 chiplets.

REMU models this by giving each chiplet's CPUs their own `MemoryRegion` container (built in `r100_build_chiplet_view()` in `r100_soc.c`). Each view:

1. Aliases the entire shared `sysmem` at offset 0 (so DRAM, GIC, UART, config-space peripherals, etc. behave normally).
2. Overlays a 256 MB subregion at `PRIVATE_WIN_BASE` with overlap priority 10 that aliases into `chiplet_id * CHIPLET_OFFSET + PRIVATE_WIN_BASE` — that chiplet's own slice of `sysmem`. The higher priority ensures this overlay wins over the flat `sysmem` alias.
3. Overlays a 64 KB `SYSREG_CP0` window at the config-space address `R100_CP0_SYSREG_BASE` (`0x1FF1010000`), and a 64 KB `SYSREG_CP1` window at `R100_CP1_SYSREG_BASE` (`0x1FF1810000` = `SYSREG_CP0 + PER_SYSREG_CP`), each aliasing back to the chiplet's own private-alias `SYSREG_CP{0,1}` RAM at `chiplet_base + R100_SYSREG_CP{0,1}_PRIVATE_BASE` (`0x1E01010000` / `0x1E01810000`). BL1's cold-boot CP0 release path writes RVBAR via the QSPI bridge to the private-alias address; BL2's CP1 release (`plat_set_cpu_rvbar(CLUSTER_CP1, ...)` on each chiplet's own CP0.cpu0) writes it to the `SYSREG_CP1` config-space address; BL31's `rebel_h_pm.c:set_rvbar()` (invoked from the PSCI `CPU_ON` SMC handler, CP0 or CP1 half depending on `__TARGET_CP`) writes it to the matching config-space address. The overlays make all three paths converge on the same backing RAM — matching silicon, where each chiplet's decoder routes `0x1FF1010000` / `0x1FF1810000` chiplet-locally. A fourth access path — BL2 running on chiplet M writing the *absolute* cross-chiplet form `chiplet_N * CHIPLET_OFFSET + SYSREG_CP{0,1}_BASE + ...` — is handled by a separate alias of the same RAM mounted inside each chiplet's own `cfg_mr` at offset `SYSREG_CP{0,1}_BASE - CFG_BASE`, with overlap priority 1 so it outranks the chiplet-wide unimpl catch-all.

Each CPU's `"memory"` link is set to its chiplet's view before realisation, so all TLB walks and instruction fetches observe the chiplet-local routing. Consequently, secondary CPUs released from power-off must start at the unmodified `0x1E00028000` entry point (the private-alias BL2 base); adding `chiplet_id * CHIPLET_OFFSET` would put PC at an absolute cross-chiplet address and shift every PC-relative ADRP-resolved linker symbol by `CHIPLET_OFFSET`, corrupting expressions like `BL_CODE_END - BL2_BASE` in BL2's MMU setup. The PSCI CPU_ON warm-boot path uses the same mechanism: the PMU hands the FW-written `bl31_warm_entrypoint` straight to `arm_set_cpu_on(mpidr, entry, target_el=3)`, the CPU resumes inside BL31's warm-boot trampoline at EL3, and BL31 `ERET`s to the original `secondary_prep_c` entry at EL1.

### Per-Chiplet Memory Map

Each of the 4 chiplets has an identical memory layout at offset `chiplet_id * 0x2000000000`:

| Region | Base Address (chiplet 0) | Size | Description |
|--------|--------------------------|------|-------------|
| DRAM | `0x00_0000_0000` | 1GB (emulation) | Device memory, FW images, user data |
| iROM | `0x1E_0000_0000` | 64KB | Boot ROM |
| iRAM | `0x1E_0001_0000` | 256KB | BL1 load target, reset vector |
| SP_MEM | `0x1F_E000_0000` | 64MB | Scratchpad memory (2x 32MB D-Clusters) |
| SH_MEM | `0x1F_E400_0000` | 64MB | Shared memory (2x 32MB D-Clusters) |
| Config | `0x1F_F000_0000` | ~3GB | Peripheral MMIO registers |

Config space contains all peripheral registers: CMU, PMU, SYSREG, GIC, UART, DNC, HDMA, RBC, etc.

### Boot Flow

```
BL1 (iRAM @ 0x1E00010000)
  ├─ CMU: Initialize PLLs (stubs return locked instantly)
  ├─ PMU: Check reset status (stub returns cold boot)
  ├─ QSPI: Discover secondary chiplets (reads their SYSREMAP_CHIPLET)
  ├─ RBC: Initialize UCIe links (stubs return link-up)
  ├─ Load BL2 from flash (or preloaded in emulation)
  └─ Jump to BL2

BL2 (iRAM @ 0x1E00028000, same binary on all 4 chiplets)
  ├─ CMU: Chiplet-local PLL init
  ├─ HBM3: Initialize memory controller (sparse stub returns ready, ICON req RMW works)
  ├─ Inter-chiplet HBM handshake via mailbox RAM (primary waits for N=1..3)
  ├─ SMMU-600 TCU: Early init (stub CR0ACK mirror + GBPA.UPDATE auto-clear)
  ├─ MMU: xlat_tables_v2 — per-chiplet mappings (private window + DRAM + flash + SMMU)
  ├─ Load BL31 + FreeRTOS images (CP0 + CP1, backed-up copies on secondary chiplets)
  ├─ Release CP1.cpu0 via plat_pmu_cl_on + plat_pmu_cpu_on    [#ifndef ZEBU_CI]
  └─ Hand off to BL31_CP0 at BL31_BASE (DRAM @ 0)

BL31_CP0 (DRAM @ 0x00000000)           BL31_CP1 (DRAM @ 0x14100000)
  ├─ GIC: shared with CP0                ├─ GIC: shared
  ├─ TZPC for CP0                         ├─ TZPC for CP1
  └─ ERET → FreeRTOS_CP0 (EL1)            └─ ERET → FreeRTOS_CP1 (EL1)

FreeRTOS_CP0 (DRAM @ 0x00200000)        FreeRTOS_CP1 (DRAM @ 0x14200000)
  ├─ init_smp() PSCI CPU_ON cpu1..3        ├─ init_smp() PSCI CPU_ON cpu1..3
  │     → BL31_CP0 warm-boot → EL1         │     → BL31_CP1 warm-boot → EL1
  └─ q-cp tasks (hq_mgr, cs_mgr, ...)     └─ q-cp tasks (CP1 half)
```

### CP0 / CP1 cluster split

Each chiplet has two independent 4-core clusters (CP0 and CP1), each running its own `bl31_cp{0,1}.bin` → `freertos_cp{0,1}.bin` — built from the same TF-A source with `CP=0` vs `CP=1` (`__TARGET_CP`), differing mainly in where `rebel_h_pm.c:set_rvbar()` writes RVBAR (`SYSREG_CP0_OFFSET` vs `SYSREG_CP1_OFFSET = SYSREG_CP0_OFFSET + PER_SYSREG_CP*1`) and which `PERCLUSTER_OFFSET` the PSCI CPU_ON handler uses.

The release sequence is asymmetric:

- **CP0.cpu0** of the primary chiplet is the boot ROM target — started by QEMU at reset with its `rvbar` property.
- **CP0.cpu0** of chiplets 1-3 is released cross-chiplet by BL1 on the primary via the QSPI bridge (`plat_pmu_cpu_on` with `IMAGE_BL1` branch → `qspi_bridge_write_1word` to each slave's `CPMU_PRIVATE + CPU0_CONFIGURATION`).
- **CP1.cpu0** of each chiplet is released by BL2 on the *same* chiplet's CP0.cpu0 via direct MMIO (same `plat_pmu_cl_on` / `plat_pmu_cpu_on` helpers, non-`IMAGE_BL1` branch). This is the `#ifndef ZEBU_CI` path in `rebel_h_bl2_setup.c:865-871`.
- **CP0/CP1.cpu{1,2,3}** are released by their respective FreeRTOS via `init_smp() → psci_cpu_on()` into `BL31_CP{0,1}`'s warm-boot entry — a pure PSCI SMC path, no MMIO from the FreeRTOS side.

The PMU device handles all four release variants uniformly: `CPU_CONFIGURATION + cluster*PERCLUSTER_OFFSET + cpu*PERCPU_OFFSET` writes decode cluster/cpu, the stub reads RVBAR back from the matching SYSREG_CP{0,1} backing, and `arm_set_cpu_on(mpidr, RVBAR, target_el=3)` releases the vCPU to either BL2 (cold) or `bl31_warm_entrypoint` (PSCI warm).

### Log routing

The R100 FW emits boot messages through two independent paths, and REMU captures both:

1. **Direct UART (`printf_` / TF-A `NOTICE`/`INFO`)** — TF-A and FreeRTOS's `printf_` call into `uart_putc`, which polls `LSR.THRE|TEMT` and writes to `THR`. REMU wires a QEMU `serial-mm` (16550, `regshift=2`) at `PERI0_UART0_BASE` (`0x1FF9040000`) on each chiplet, with priority-10 overlap over the config-space container. Every chiplet has its own chardev: chiplet 0 goes to `mon:stdio`, chiplets 1-3 to `/tmp/remu_uart{1,2,3}.log`. The DW-APB UART extras (`DLF` at `0xC0`, `TFL` at `0x80`, etc.) fall into the cfg-space unimplemented catch-all, which is harmless — the FW's init path tolerates zero reads and dropped writes.

2. **HILS ring buffer (`RLOG_*` / `FLOG_*`)** — FreeRTOS's rate-limited structured logging doesn't touch the UART directly. It writes 128-byte `struct rl_log` records (`{tick, type, cpu, func_id, task[16], logstr[100]}`) into a 2 MB ring in DRAM at physical `0x10000000` (16384 entries, see `FreeRTOS.ld` `.logbuf` at virt `0x10010000000`). On silicon a `terminal_task` later drains the ring onto the UART; in REMU we keep the ring tail live from the emulator side too (independent of `terminal_task` scheduling). The `r100-logbuf-tail` device polls the ring on a 50 ms `QEMU_CLOCK_VIRTUAL` timer and writes each new entry as `[HILS <tick> cpu=N LEVEL task|func] <msg>` to its own chardev (CLI default `/tmp/remu_hils.log`, override with `--hils-log PATH` / `--hils-log stderr`).

### Device Model Design

All device models follow the QEMU QOM (QEMU Object Model) pattern:

1. **TypeInfo registration** via `type_init()` — called at QEMU startup
2. **MemoryRegionOps** for MMIO read/write — the core of each model
3. **Properties** for parameterization — chiplet ID, block name, etc.
4. **Reset** handler — sets default register values

**Stub philosophy**: Return the minimum register values needed for the FW to proceed. Most stubs use store-on-write / return-on-read semantics with specific overrides for status bits (e.g., CMU PLL lock).

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| QEMU CPU model | `cortex-a72` + MIDR override to 0x411FD091 (CA73 r1p1) | Functionally closest to CA73 in QEMU; r1p1 MIDR skips CA73 errata workarounds that probe IMP_DEF sysregs cortex-a72 doesn't model |
| GIC model | Single GICv3 for all 32 CPUs | Simpler than per-chiplet GIC; sufficient for Phase 1 |
| QSPI bridge | Address-space read/write | Uses QEMU's `address_space_read/write` for cross-chiplet access |
| UART priority | `memory_region_add_subregion_overlap` prio=10 | UART must take precedence over config space container |
| Machine name | `r100-soc-machine` | QEMU requires `-machine` suffix for `MachineClass` types |
| Timer frequency | 500MHz (`CNTFRQ_EL0`) | Matches real hardware (`CORE_TIMER_FREQ`) |
| DRAM size | 1GB per chiplet | Sufficient for FW boot; real hardware has up to 36GB |

## Source File Map

| File | Device | Instances | Key Behavior |
|------|--------|-----------|--------------|
| `r100_soc.c` | Machine type | 1 | Creates chiplets, per-chiplet CPU views, CPUs, GIC, per-chiplet UARTs, mailbox RAM |
| `r100_cmu.c` | CMU (Clock) | 20 per chiplet | PLL lock = instant, mux_busy = 0 |
| `r100_pmu.c` | PMU (Power) | 1 per chiplet | Cold reset, all CPUs/clusters ON; `CPU_CONFIGURATION` → `arm_set_cpu_on(mpidr, RVBAR, target_el=3)` at FW-written RVBAR (covers BL1's QSPI-driven cold-boot release, BL2's direct CP1 release, and BL31's PSCI CPU_ON warm-boot release); `read_rvbar()` indexes `SYSREG_CP0_PRIVATE + cluster * PER_SYSREG_CP` so the same path serves CP0 and CP1; DCL config→status mirror |
| `r100_sysreg.c` | SYSREG | 1 per chiplet | Returns chiplet ID (0-3); CP0 and CP1 RAM triple-mounted per chiplet (private alias + cfg_mr alias + per-chiplet CPU view overlay) — CP0 at `0x1E01010000` / `0x1FF1010000`, CP1 at `0x1E01810000` / `0x1FF1810000` |
| `r100_hbm.c` | HBM3 controller | 1 per chiplet | Sparse write-back (6 MB window, default 0xFFFFFFFF), ICON req0/req1 RMW mirror |
| `r100_qspi.c` | QSPI bridge | 1 per chiplet | Designware SSI, cross-chiplet R/W + upper-addr latch (28-bit offset) |
| `r100_qspi_boot.c` | QSPI_ROT (DWC_SSI master) | 1 per chiplet | Dual-mapped (cfg-space `0x1FF0500000` + private alias `0x1E00500000`); DW SSI status always idle (`TFNF\|TFE\|RFNE`, `BUSY=0`), `TXFLR=0`, DRX reads return `0x82` (`FLASH_READY\|WRITE_ENABLE_LATCH`) so every `qspi_boot.c:tx_available()` and `check_read_*_status()` poll exits on iteration 0; covers BL1 flash-load and BL31 `NOR_FLASH_SVC_*` SMC paths |
| `r100_rbc.c` | RBC/UCIe | 6 per chiplet | Dual-mapped (cfg-space `0x1FF5xxxxxx` + private alias `0x1E05xxxxxx`); `global_reg_cmn_mcu_scratch_reg1 = 0xFFFFFFFF` (ZEBU link-up) + `lstatus_link_status=1` |
| `r100_smmu.c` | SMMU-600 TCU | 1 per chiplet (primary only wired) | `CR0→CR0ACK` mirror + `GBPA.UPDATE` auto-clear (BL2 `smmu_early_init`); CMDQ walker: on each `CMDQ_PROD` write, walks entries in DRAM via `cpu_physical_memory_read`, writes 32-bit 0 to MSI address of every `CMD_SYNC` (CS=SIG_IRQ), and auto-advances `CMDQ_CONS` to PROD so FreeRTOS EL1 `smmu_sync()` returns on first iteration |
| `r100_pvt.c` | PVT monitor | 5 per chiplet (ROT + 4 DCL) | `PVT_CON_STATUS=0x3` (idle), per-sensor `_valid=1`, rest RAM — unblocks FreeRTOS `pvt_init()` `PVT_ENABLE_{PROC,VOLT,TEMP}_CONTROLLER` polls |
| `r100_dma.c` | PL330 DMA | 1 per chiplet | Fake completion on csr/dbgstatus/dbgcmd polls |
| `r100_logbuf.c` | HILS ring tail | 1 (chiplet 0 only) | Polls DRAM `.logbuf` ring at 0x10000000 on a 50 ms timer, drains `RLOG_*`/`FLOG_*` entries to own chardev |
| `remu_addrmap.h` | — | — | All address constants (from `g_sys_addrmap.h`) |

## FW Source References

These files in the external repos define the hardware behavior that the emulator models:

| What | File |
|------|------|
| SoC address map | `q-sys/.../autogen/g_sys_addrmap.h` |
| Platform defs (memory, GIC, UART) | `q-sys/bootloader/cp/tf-a/plat/rebel/rebel_h/include/platform_def.h` |
| CMU PLL polling | `q-sys/bootloader/cp/tf-a/drivers/clk/clk_samsung/cmu.c` |
| PMU boot status | `q-sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_pmu.c` |
| Boot stages | `q-sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_bl{1,2,31}_setup.c` |
| QSPI bridge protocol | `q-sys/bootloader/cp/tf-a/drivers/synopsys/qspi_bridge/qspi_bridge.c` |
| DWC_SSI master (QSPI_ROT) | `q-sys/drivers/qspi_boot/qspi_boot.{c,h}` + `services/std_svc/nor_flash/nor_flash_main.c` |
| RBC/UCIe init | `q-sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_rbc.c` |
| PCI BARs + memory map | `kmd/rebellions/rebel/rebel.h` |
| FW-host handshake | `kmd/rebellions/common/{fw_if.c,ring.c,queue.c}` |
| Host interface (FW side) | `q-cp/src/hil/hil.c` |
