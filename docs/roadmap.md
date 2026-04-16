# REMU Roadmap

## Phase 1: FW Boot (current)

**Goal**: TF-A BL1 → BL2 → BL31 → FreeRTOS boots on all 4 chiplets.

**Status**: Machine type boots, UART output verified. Awaiting real FW images.

### What's done

- `r100-soc` machine type: 4 chiplets, 32 CA73 vCPUs, GICv3, PL011 UART
- CMU stubs (20 blocks per chiplet): PLL lock/mux_busy return instantly
- PMU stub: cold reset status, all CPUs/clusters/DCLs powered on
- SYSREG stub: per-chiplet ID (0-3) via SYSREMAP registers
- HBM3 stub: training-complete status
- QSPI bridge: Designware SSI protocol, cross-chiplet register access
- RBC stubs: UCIe LTSM ACTIVE for all 6 blocks per chiplet
- CLI tool (`remu build`, `remu run`, `remu status`, `remu gdb`, `remu images`)
- Build verified on QEMU 9.2.0, UART test binary prints "REMU OK"

### What remains

- [ ] Build q-sys FW with `-p zebu` and test full BL1 → FreeRTOS boot
- [ ] Identify and stub additional registers that the FW reads during boot
- [ ] Add per-chiplet UART instances (currently only chiplet 0 has UART)
- [ ] Refine PMU for secondary core release (PSCI / CPU power-on wakeup)
- [ ] Test GDB attach and multi-core debugging (`thread N` to switch vCPUs)

### Success criteria

- All 4 chiplets print FreeRTOS startup banner on UART
- Chiplet 0 discovers 3 secondary chiplets via QSPI bridge
- GDB can step through FW code on any chiplet's vCPU

## Phase 2: Host Drivers

**Goal**: kmd loads in an x86_64 QEMU guest, probes virtual PCI device, handshakes with FW, umd opens device.

### New components

- **r100-npu-pci**: QEMU PCI device model for the host side
  - Vendor `0x1eff`, Device `0x2030` (CR03 quad)
  - BAR0 (DRAM), BAR2 (config), BAR4 (doorbell), BAR5 (MSI-X)
- **Shared memory bridge**: POSIX shm (`/dev/shm/remu-*`) connecting both QEMU instances
- **Doorbell/MSI-X routing**: eventfd-based interrupt forwarding
- **DNC stub**: accepts tasks, immediately generates completion interrupt
- **HDMA stub**: performs actual memcpy between DRAM regions

### Success criteria

- `insmod rebellions.ko` succeeds, device probes, BAR sizes match
- MSI-X vectors allocated, `FW_BOOT_DONE` handshake completes
- Message ring `TEST_IB` passes
- umd opens device, creates context, submits simple job

## Phase 3: Full Inference

**Goal**: Real tensor operations on emulated DNCs.

### New components

- **DNC behavioral model**: parse task descriptors, execute tensor operations on host CPU
- **HDMA scatter-gather**: full DMA with address translation
- **SMMU model**: honor page tables set by FW
- **Performance counters**: return synthetic cycle counts

## Peripheral Model Fidelity

| Peripheral | Phase 1 | Phase 2 | Phase 3 |
|------------|---------|---------|---------|
| CMU (20 blocks x4) | PLL-lock stub | Same | Same |
| PMU (x4) | Boot-status stub | Full register bank | Same |
| GIC600 | QEMU built-in | Same | Same |
| UART | QEMU built-in | Same | Same |
| Timer | QEMU built-in | Same | Same |
| QSPI bridge | Cross-chiplet R/W | Same | Same |
| RBC/UCIe (6 x4) | Link-ready stub | Same | Same |
| PCIe/doorbell | N/A | Shared mem bridge | Same |
| DNC (16 x4) | N/A | Return-success | Behavioral model |
| HDMA (x4) | N/A | Memcpy | Scatter-gather |
| RBDMA (x4) | N/A | Stub | Behavioral model |
| HBM3 (x4) | Ready stub | Same | Same |
| SMMU (x4) | N/A | Bypass | Translation model |

## Timing Considerations

REMU is purely functional — no timing model. Implications:

- PLL locks, DMA, and HBM training complete instantly
- Timeout paths in FW may never trigger
- Race conditions that depend on hardware timing may not reproduce
- Performance counter values are meaningless
- FreeRTOS tick timer fires correctly, but wall-clock ratios differ

For timing-sensitive tests, annotated delays can be added to device models later.
