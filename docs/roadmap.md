# REMU Roadmap

## Phase 1: FW Boot (current)

**Goal**: TF-A BL1 → BL2 → BL31 → FreeRTOS boots on all 4 chiplets.

**Status**: Real q-sys TF-A BL1 runs and prints boot log to UART. PL330 DMA stub unblocks the UCIe firmware load; boot proceeds through `load_and_enable_ucie_link_for_CP()` and into QSPI-bridged secondary-chiplet UCIe loads. The QSPI NOR flash staging region (`0x1F80000000`, 64 MB, zero-filled) is now modeled — HW-CFG lookup misses gracefully and BL1 falls through to "UCIe speed: default", then stages `tboot_n` and sets RVBAR for chiplets 1-3. Next blocker: secondary-chiplet CPU0 release (PMU `CPU_CONFIGURATION` write must map to QEMU `cpu_resume()`) and the flood of `r100-qspi: unknown instruction 0x83` operations once BL1 polls the secondary chiplets after reset release.

### What's done

- `r100-soc` machine type: 4 chiplets, 32 vCPUs, GICv3, 16550 UART
  - CPU is `cortex-a72` with MIDR overridden to Cortex-A73 r1p1 (0x411FD091) so TF-A's `get_cpu_ops_ptr` accepts it and CA73 errata workarounds skip
- UART: QEMU `serial-mm` (16550, regshift=2) matching FW's `console_16550_register` driver
- CMU stubs (20 blocks per chiplet): PLL lock/mux_busy return instantly
- PMU stub: OM_STAT=NORMAL_BOOT, RST_STAT=PINRESET, all CPU/cluster/DCL/RBC status registers seeded to powered-on
- SYSREG stub: per-chiplet ID (0-3) via SYSREMAP registers
- HBM3 stub: training-complete status
- QSPI bridge: Designware SSI protocol, cross-chiplet register access (with `PRIVATE_BASE` prefix for slave-side addressing)
- RBC stubs: UCIe LTSM ACTIVE for all 6 blocks per chiplet
- PL330 DMA stub at `0x1FF02C0000` per chiplet: fake-completion on `ch_stat[0].csr`, `dbgstatus`, `dbgcmd` polls — no real memcpy (RBC stub hides the missing data)
- Dual-mapped PMU and SYSREG devices at both config-space (`0x1FF0xxxxxx`) and private-alias (`0x1E00xxxxxx`) addresses via `memory_region_init_alias`
- PCIe sub-controller stub (for `pmu_release_cm7` writes) and CSS600 CNTGEN stub (generic timer reset)
- Chiplet-wide `unimplemented_device` fallbacks for config space and the 256MB private-alias window (OTP, RBC private aliases, etc.) — graceful handling of unmodeled registers
- QSPI NOR flash staging region at `0x1F80000000` (64 MB RAM, shared and aliased into every chiplet's local flash window) — backs FW's direct `flash_nor_read()` memcpys and `nvmem_flash_hw_cfg_read()`; optional `images/flash.bin` preloaded via `-device loader`
- CLI tool (`remu build`, `remu run`, `remu status`, `remu gdb`, `remu images`)
- q-sys integration via `external/ssw-bundle`: submodules initialized, TF-A builds with CA73 errata + Spectre v4 workarounds disabled (see platform.mk)

### Observed boot trace

```
NOTICE:  BL1: check QSPI bridge
INFO:    BL1: QSPI bridge status check time 0, chiplet{1,2,3}
NOTICE:  BL1: Boot mode: NORMAL_BOOT (5)
NOTICE:  BL1: Boot reason: Cold reset(POR) (0x00010000)
NOTICE:  BL1: Chiplet reset flag: 0x0 status: 0x0010
NOTICE:  BL1: Load tboot_p0
INFO:    Release reset of CM7
NOTICE:  BL1: pmu_release_cm7 complete
NOTICE:  BL1: check QSPI bridge
INFO:    BL1: QSPI bridge status check time 0, chiplet{1,2,3}
NOTICE:  BL1: Load tboot_u
NOTICE:  BL1: Detected secondary chiplet count: 3
NOTICE:  BL1: ZEBU_CI: 0
NOTICE:  UCIe speed: default           [flash HW-CFG magic miss → default]
NOTICE:  BL1: Load tboot_n
INFO:    Set RVBAR of chiplet: {1,2,3}, cluster: 0, cpu: 0, ep: 0x1E00028000
NOTICE:  BL1: Release reset of CP0.cpu0 of chiplet-1   [hang — secondary cores not released]
```

### What remains

- [x] Back the flash staging region (`0x1F80000000`, 64 MB) with a RAM model so `nvmem_flash_hw_cfg_read()` and `flash_nor_read()` memcpys succeed; optional `images/flash.bin` preload wired into the CLI
- [ ] Extend the QSPI bridge instruction decoder — BL1 post-reset sequence emits `unknown instruction 0x83` in a tight loop on chiplets 1-3 after `Release reset of CP0.cpu0`
- [ ] Pre-load BL2/BL31/FreeRTOS to each secondary chiplet's iRAM/DRAM so BL1's cross-chiplet copy finds the data in place (alternatively: populate `flash.bin` with a real GPT image and let BL1 DMA-stage everything itself)
- [ ] Refine PMU for secondary core release — wire `CPU_CONFIGURATION` writes to QEMU `cpu_resume()` so chiplets 1-3 CPU0 actually start executing
- [ ] Add per-chiplet UART instances (currently only chiplet 0 has UART mapped)
- [ ] Build system: fix `sys/build.sh` path where `debug` COMMAND resets CLI flags, so `CHIPLET_COUNT=1` builds work (alternative to the DMA stub for single-chiplet boot)
- [ ] Test GDB attach and multi-core debugging (`thread N` to switch vCPUs)

### Success criteria

- All 4 chiplets print FreeRTOS startup banner on UART
- Chiplet 0 discovers 3 secondary chiplets via QSPI bridge ✓
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
| PMU (x4) | Boot-status + RBC/boot-mode defaults | Full register bank | Same |
| GIC600 | QEMU built-in | Same | Same |
| UART | 16550 (serial-mm) | Per-chiplet instances | Same |
| Timer | QEMU built-in + CSS600_CNTGEN stub | Same | Same |
| QSPI bridge | Cross-chiplet R/W via `PRIVATE_BASE` prefix | Same | Same |
| RBC/UCIe (6 x4) | Link-ready stub + PMU RBC status=ON | Same | Same |
| DMA (PL330) | Fake-completion stub (csr/dbgstatus/dbgcmd return 0) | Stub | Behavioral model |
| PCIe sub-controller | RAM stub (pmu_release_cm7) | Real model | Same |
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
