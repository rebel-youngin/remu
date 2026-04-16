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
│  32x CA73 vCPUs (cortex-a72 model)               │
│  TF-A BL1 → BL2 → BL31 → FreeRTOS              │
│  ┌────────────────────────────────────────────┐  │
│  │ Device Models                              │  │
│  │  CMU (20 blocks) — PLL lock stubs          │  │
│  │  PMU — boot status, power states           │  │
│  │  SYSREG — chiplet ID                       │  │
│  │  HBM3 — training-complete                  │  │
│  │  QSPI bridge — cross-chiplet register I/O  │  │
│  │  RBC — UCIe link-up status                 │  │
│  │  GIC600, PL011 UART, Generic Timer         │  │
│  └────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────┘
```

**Why two instances?** The FW runs bare-metal AArch64 at EL3/EL1 with a 40-bit physical address space starting at `0x1E00000000`. The host kmd runs inside a Linux kernel. These are fundamentally incompatible execution environments. A single QEMU with heterogeneous clusters (like Xilinx ZynqMP) would be far more complex to build and maintain.

**Why a QEMU fork?** The machine type needs a custom memory map, custom device models, and custom boot loading. QEMU does not support out-of-tree device compilation, so the device models are symlinked into the QEMU source tree and compiled in-tree.

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

BL2 (iRAM @ 0x1E0004B000)
  ├─ HBM3: Initialize memory controller (stub returns ready)
  ├─ SMMU: Early init
  ├─ Load BL31 + FreeRTOS to DRAM
  └─ Release secondary cores via PMU

BL31 (DRAM @ 0x00000000)
  ├─ GIC: Configure interrupt controller
  ├─ TrustZone: Configure TZPC for all SFR blocks
  └─ Jump to FreeRTOS (EL3 → EL1)

FreeRTOS (DRAM @ 0x00200000)
  ├─ Scheduler starts (5 priority levels, 1ms tick, 32KB heap)
  └─ q-cp tasks run: hq_mgr, cb_mgr, cs_mgr, proc_mgr, etc.
```

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
| QEMU CPU model | `cortex-a72` | Functionally closest to CA73 available in QEMU |
| GIC model | Single GICv3 for all 32 CPUs | Simpler than per-chiplet GIC; sufficient for Phase 1 |
| QSPI bridge | Address-space read/write | Uses QEMU's `address_space_read/write` for cross-chiplet access |
| UART priority | `memory_region_add_subregion_overlap` prio=10 | UART must take precedence over config space container |
| Machine name | `r100-soc-machine` | QEMU requires `-machine` suffix for `MachineClass` types |
| Timer frequency | 500MHz (`CNTFRQ_EL0`) | Matches real hardware (`CORE_TIMER_FREQ`) |
| DRAM size | 1GB per chiplet | Sufficient for FW boot; real hardware has up to 36GB |

## Source File Map

| File | Device | Instances | Key Behavior |
|------|--------|-----------|--------------|
| `r100_soc.c` | Machine type | 1 | Creates chiplets, CPUs, GIC, UART |
| `r100_cmu.c` | CMU (Clock) | 20 per chiplet | PLL lock = instant, mux_busy = 0 |
| `r100_pmu.c` | PMU (Power) | 1 per chiplet | Cold reset, all CPUs/clusters ON |
| `r100_sysreg.c` | SYSREG | 1 per chiplet | Returns chiplet ID (0-3) |
| `r100_hbm.c` | HBM3 controller | 1 per chiplet | Returns training-complete |
| `r100_qspi.c` | QSPI bridge | 1 per chiplet | Designware SSI protocol, cross-chiplet R/W |
| `r100_rbc.c` | RBC/UCIe | 6 per chiplet | LTSM state = ACTIVE (link up) |
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
| RBC/UCIe init | `q-sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_rbc.c` |
| PCI BARs + memory map | `kmd/rebellions/rebel/rebel.h` |
| FW-host handshake | `kmd/rebellions/common/{fw_if.c,ring.c,queue.c}` |
| Host interface (FW side) | `q-cp/src/hil/hil.c` |
