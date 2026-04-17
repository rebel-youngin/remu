# REMU Roadmap

## Phase 1: FW Boot (current)

**Goal**: TF-A BL1 → BL2 → BL31 → FreeRTOS boots on all 4 chiplets.

**Status**: All 4 chiplets now complete BL2 through platform setup and reach the `BL2: Booting BL31` handoff. Each chiplet's CPUs see their own `0x1E00000000` private window via a dedicated QEMU memory view, so firmware reads of `SYSREG_SYSREMAP_PRIVATE+CHIPLET_ID_OFFSET` correctly return 0-3 and the `IS_PRIMARY_CHIPLET` branch is taken only on chiplet 0. Each chiplet prints to its own UART (chiplet 0 → stdio, 1-3 → `/tmp/remu_uartN.log`), making per-chiplet boot traces legible. Secondary chiplets are released by the primary's BL1 via QSPI-staged BL2 in iRAM + PMU `CPU_CONFIGURATION` write, start executing at the private-alias `0x1E00028000` entry point (no cross-chiplet offset added, so PC-relative ADRP symbol resolution in BL2 stays valid), synchronise HBM-init completion with chiplet 0 through shared mailbox RAM, and pass through `smmu_early_init()` thanks to an SMMU-600 TCU stub that mirrors `CR0` into `CR0ACK` and auto-clears the `GBPA[UPDATE]` poll bit. MMU enable (`xlat_tables_v2`) now completes cleanly on every chiplet. Next blocker is the BL2→BL31 handoff itself: `Entry point address = 0x0` is printed but no BL31 output follows — investigate whether BL31 was actually staged into DRAM and whether the next-image params propagate correctly through `bl2_plat_get_bl31_ep_info()`.

### What's done

- `r100-soc` machine type: 4 chiplets, 32 vCPUs, GICv3, per-chiplet 16550 UART
  - CPU is `cortex-a72` with MIDR overridden to Cortex-A73 r1p1 (0x411FD091) so TF-A's `get_cpu_ops_ptr` accepts it and CA73 errata workarounds skip
  - Each chiplet's CPUs use a dedicated QEMU `MemoryRegion` view (`r100_build_chiplet_view`) that aliases the 256 MB `PRIVATE_BASE` window (`0x1E00000000`) onto that chiplet's own slice of sysmem, so every chiplet reads its own `CHIPLET_ID` and `SECOND_CHIPLET_CNT` — the hardware "chiplet-local address-decoder alias" modelled in software
- UART: one QEMU `serial-mm` (16550, regshift=2) per chiplet, each bound to its own chardev — chiplet 0 goes to `mon:stdio`, chiplets 1-3 to `/tmp/remu_uart{1,2,3}.log` (separate streams avoid interleaved BL1/BL2 banners)
- CMU stubs (20 blocks per chiplet): PLL lock/mux_busy return instantly
- PMU stub: OM_STAT=NORMAL_BOOT, RST_STAT=PINRESET, all CPU/cluster/DCL/RBC status registers seeded to powered-on
- SYSREG stub: per-chiplet ID (0-3) via SYSREMAP registers
- HBM3 stub: training-complete status
- QSPI bridge: Designware SSI protocol, cross-chiplet register access; supports 1-word read (`0x70`), 1-word write (`0x80`), and 16-word burst write (`0x83`) — the last used by BL1 `qspi_bridge_load_image()` to stage `tboot_n` into secondary chiplets' iRAM. Latches the upper-address write (`DW_SPI_SYSREG_ADDRESS`, 24-bit field `0x0C4058`) into `s->upper_addr`; subsequent reads/writes rebuild the 28-bit offset into the slave chiplet's `PRIVATE_BASE` window (`addr & 0x0FFFFFFF`) so RBC accesses above the 64 MB 26-bit window reach the right block
- Per-chiplet SYSREG_CP0 RAM region (64 KB at private-alias `0x1E01010000`) backing BL1's `plat_set_cpu_rvbar()` writes; the PMU reads RVBARADDR0_LOW/HIGH back when a CPU release is triggered
- PMU secondary core release: `CPU_CONFIGURATION` writes decode cluster/cpu, update `CPU_STATUS`, and call `arm_set_cpu_on(mpidr, entry)` with the unmodified `PRIVATE_BASE`-relative RVBAR; the chiplet's CPU view redirects `0x1E00028000` to that chiplet's own iRAM, so PC stays in the private-alias range and ADRP-based linker-symbol resolution remains correct
- PMU D-Cluster power-state mirror: writes to `DCL{0,1}_CONFIGURATION` are reflected into `DCL{0,1}_STATUS` synchronously so BL2's `pmu_reset_dcluster()` poll on `DCL_STATUS_MASK` completes
- SMMU-600 TCU stub (`r100_smmu.c`) at `0x1FF4200000`: mirrors `SMMU_CR0` writes into `SMMU_CR0ACK` (masked), and auto-clears the `UPDATE` bit on `SMMU_GBPA` writes so BL2's `smmu_early_init()` `while (!(cr0ack & EVENTQEN))` / `while (gbpa & UPDATE)` polls terminate
- HBM3 controller stub (`r100_hbm.c`): sparse write-back store (`GHashTable`) returning `0xFFFFFFFF` for unwritten offsets across the full 6 MB HBM window (16 CON + 16 PHY + ICON blocks), plus custom behaviour for the ICON `test_instruction_req0/req1` RMW pattern so DFI/PHY training polls complete on every channel
- Inter-chiplet HBM-init mailbox RAM: 6 shared `MemoryRegion` instances (CP0.M4 + per-chiplet C0/C1/C2/C3 slots, plus CP0 status) at their absolute silicon addresses with priority-overlap so primary-chiplet BL2 `wait_for_2nd_chiplet_hbm_init_done()` observes the notification writes from chiplets 1-3
- RBC stubs: 6 blocks per chiplet (V00/V01/V10/V11/H00/H01), dual-mapped at config-space (`0x1FF5xxxxxx`) and chiplet-private alias (`0x1E05xxxxxx` via `PRIVATE_BASE`). UCIe SS returns ZEBU link-up sentinel `0xFFFFFFFF` at `global_reg_cmn_mcu_scratch_reg1` (UCIe SS offset `0x2e038`) so BL2's `check_link_up()` returns true immediately, plus `lstatus_link_status=1` at `dvsec1_ucie_link_status` (+0x20014) for non-ZEBU builds
- PL330 DMA stub at `0x1FF02C0000` per chiplet: fake-completion on `ch_stat[0].csr`, `dbgstatus`, `dbgcmd` polls — no real memcpy (RBC stub hides the missing data)
- Dual-mapped PMU and SYSREG devices at both config-space (`0x1FF0xxxxxx`) and private-alias (`0x1E00xxxxxx`) addresses via `memory_region_init_alias`
- PCIe sub-controller stub (for `pmu_release_cm7` writes) and CSS600 CNTGEN stub (generic timer reset)
- Chiplet-wide `unimplemented_device` fallbacks for config space and the 256MB private-alias window (OTP, RBC private aliases, etc.) — graceful handling of unmodeled registers
- QSPI NOR flash staging region at `0x1F80000000` (64 MB RAM, shared and aliased into every chiplet's local flash window) — backs FW's direct `flash_nor_read()` memcpys and `nvmem_flash_hw_cfg_read()`; optional `images/flash.bin` preloaded via `-device loader`
- CLI tool (`remu build`, `remu run`, `remu status`, `remu gdb`, `remu images`)
- q-sys integration via `external/ssw-bundle`: submodules initialized, TF-A builds with CA73 errata + Spectre v4 workarounds disabled (see platform.mk)

### Observed boot trace

Chiplet 0 (stdio):

```
NOTICE:  BL1: Boot mode: NORMAL_BOOT (5)
NOTICE:  BL1: Detected secondary chiplet count: 3
NOTICE:  BL1: Load tboot_n
INFO:    Set RVBAR of chiplet: {1,2,3}, cluster: 0, cpu: 0, ep: 0x1E00028000
NOTICE:  BL1: Release reset of CP0.cpu0 of chiplet-{1,2,3}
NOTICE:  BL1: Booting BL2
INFO:    Entry point address = 0x1e00028000
NOTICE:  BL2: Detected secondary chiplet count: 3
NOTICE:  BL2: Init CMU of chiplet-0, 0
NOTICE:  BL2: ZEBU_CI: Skip for hbm init(chiplets)
NOTICE:  BL2: EVT version: 0x0
NOTICE:  BL2: Load CP0 BL31
NOTICE:  BL2: Load CP0 FREERTOS
NOTICE:  BL2: Load CP1 BL31
NOTICE:  BL2: Load CP1 FREERTOS
NOTICE:  BL2: Backup flash images to Chiplet-0 DRAM
NOTICE:  BL2: Booting BL31
INFO:    Entry point address = 0x0     [BL31_BASE; next blocker — no BL31 output yet]
```

Chiplet N (N=1..3, `/tmp/remu_uartN.log`):

```
NOTICE:  BL2: Chiplet reset flag: 0x0 status: 0x0020
NOTICE:  BL2: Init CMU of chiplet-N, 0
NOTICE:  BL2: ZEBU_CI: Skip for hbm init(chiplets)
NOTICE:  BL2: EVT version: 0x0
NOTICE:  BL2: Copy flash backup images from Chiplet-0 DRAM to Chiplet-N DRAM
NOTICE:  BL2: Load CP0 BL31 for chiplet N
NOTICE:  BL2: Load CP0 FREERTOS for chiplet N
NOTICE:  BL2: Booting BL31
INFO:    Entry point address = 0x0
```

### What remains

- [x] Back the flash staging region (`0x1F80000000`, 64 MB) with a RAM model so `nvmem_flash_hw_cfg_read()` and `flash_nor_read()` memcpys succeed; optional `images/flash.bin` preload wired into the CLI
- [x] Extend the QSPI bridge instruction decoder with `0x83` (WRITE_16WORD) — BL1's `qspi_bridge_load_image()` stages `tboot_n` into each secondary chiplet's iRAM through this burst write
- [x] Refine PMU for secondary core release — `CPU_CONFIGURATION` writes now update `CPU_STATUS` and call `arm_set_cpu_on()` with the FW-written RVBAR routed through the chiplet's private-alias view
- [x] Wire UCIe PHY/link status so BL2's `Ch N (base) UCIe Linkup` poll completes — added private-alias mounts for all 6 RBC blocks per chiplet and seeded `global_reg_cmn_mcu_scratch_reg1 = 0xFFFFFFFF` (the ZEBU-build link-up sentinel checked by `check_link_up()`)
- [x] Per-chiplet UART instances — chiplet 0 → `mon:stdio`, chiplets 1-3 → dedicated file-backed chardevs, so BL1/BL2 banners are no longer interleaved
- [x] Per-chiplet CPU memory view — secondary chiplets now read their own `CHIPLET_ID`/`SECOND_CHIPLET_CNT` and take the secondary branch in BL2
- [x] Shared mailbox RAM for inter-chiplet HBM-init notifications so the primary's `wait_for_2nd_chiplet_hbm_init_done()` poll terminates
- [x] SMMU-600 TCU stub so BL2's `smmu_early_init()` `CR0ACK` / `GBPA.UPDATE` polls terminate
- [x] Fix PMU RVBAR translation that caused PC-relative ADRP corruption on secondary chiplets — don't add `chiplet_id * CHIPLET_OFFSET` to the entry; the per-chiplet CPU view already handles the routing. Without this, `BL_CODE_END - BL2_BASE` in `bl2_el3_plat_arch_setup` evaluated to `N * CHIPLET_OFFSET + 0xd000` and `mmap_add_region_check()` bailed out with `-ERANGE`
- [ ] BL2 → BL31 handoff: all 4 chiplets print `BL2: Booting BL31` then `Entry point address = 0x0` (BL31_BASE) but no BL31 output follows — investigate whether BL31 is actually staged at DRAM[0] and whether the next-image params reach BL31 with `SCTLR_EL3 / SP_EL3` properly set
- [ ] Pre-load BL2/BL31/FreeRTOS to each secondary chiplet's iRAM/DRAM so BL1's cross-chiplet copy finds the data in place (alternatively: populate `flash.bin` with a real GPT image and let BL1 DMA-stage everything itself)
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
| PMU (x4) | Boot-status + RBC/boot-mode defaults + CPU_CONFIGURATION → `arm_set_cpu_on/off` (RVBAR routed via private-alias view) + DCL status mirror | Full register bank | Same |
| SYSREG_CP0 (x4) | RAM (RVBAR capture) | Add CP1 + TZPC enforcement | Same |
| GIC600 | QEMU built-in | Same | Same |
| UART | Per-chiplet 16550 (serial-mm) on dedicated chardevs | Same | Same |
| Timer | QEMU built-in + CSS600_CNTGEN stub | Same | Same |
| QSPI bridge | Cross-chiplet R/W via `PRIVATE_BASE` + `upper_addr` latching (28-bit offset reconstruction) | Same | Same |
| RBC/UCIe (6 x4) | Link-ready stub + PMU RBC status=ON | Same | Same |
| DMA (PL330) | Fake-completion stub (csr/dbgstatus/dbgcmd return 0) | Stub | Behavioral model |
| PCIe sub-controller | RAM stub (pmu_release_cm7) | Real model | Same |
| PCIe/doorbell | N/A | Shared mem bridge | Same |
| DNC (16 x4) | N/A | Return-success | Behavioral model |
| HDMA (x4) | N/A | Memcpy | Scatter-gather |
| RBDMA (x4) | N/A | Stub | Behavioral model |
| HBM3 (x4) | Sparse-writeback hash (6 MB window, default 0xFFFFFFFF) + ICON req0/req1 RMW mirror | Same | Same |
| SMMU TCU (x4) | CR0→CR0ACK mirror + GBPA.UPDATE auto-clear (BL2 `smmu_early_init` unblocker) | Bypass | Translation model |
| Mailboxes (x4) | Shared RAM for inter-chiplet HBM-init handshake | Same | Same |

## Timing Considerations

REMU is purely functional — no timing model. Implications:

- PLL locks, DMA, and HBM training complete instantly
- Timeout paths in FW may never trigger
- Race conditions that depend on hardware timing may not reproduce
- Performance counter values are meaningless
- FreeRTOS tick timer fires correctly, but wall-clock ratios differ

For timing-sensitive tests, annotated delays can be added to device models later.
