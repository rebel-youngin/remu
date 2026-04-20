# REMU Roadmap

## Phase 1: FW Boot (current)

**Goal**: TF-A BL1 → BL2 → BL31 → FreeRTOS boots on all 4 chiplets, with **both** CP0 and CP1 clusters online on every chiplet.

**Status**: `remu fw-build -p silicon` is the target FW configuration and produces the full CA73 image set (BL1/BL2/BL31 + FreeRTOS for both CP0 and CP1). BL1 completes on all four chiplets — past `QSPI bridge check`, `Load tboot_p0`, `pmu_release_cm7`, `cm7_wait_phy_sram_init_done` (unblocked by seeding `PHY{0..3}_SRAM_INIT_DONE` bit [0] in the `PCIE_SUBCTRL` RAM stub), `Load tboot_p1/u/n`, secondary-chiplet release, and BL1 → BL2 handoff. BL2 then fails in `HBM3 init` at 6400 Mbps with `CH[0] Unrepairable PKG` → `HBM Boot on chiplet{N} FAIL` on all four chiplets; the silicon DBI lane-repair scan reads default values from `r100_hbm.c`. See "Other remaining items" for the open work list.

> Historical: an earlier `-p zebu_ci` configuration reached the `REBELLIONS$` shell on chiplet 0's CP0 cluster (BL1 → BL2 → BL31 → FreeRTOS all four CP0 cores online, secondaries paused at BL31 entry, CP1 powered off via the `ZEBU_CI: skip CP1 reset release` guard). The zebu profiles are still accepted by `remu fw-build -p zebu_ci` / `-p zebu` / `-p zebu_vdk` but are no longer on the critical path — all new work targets silicon.

### Device models (`src/machine/`)

| File | Device | Behaviour |
|---|---|---|
| `r100_soc.c` | Machine type | 4 chiplets × 8 `cortex-a72` CPUs (MIDR overridden to CA73 r1p1 `0x411FD091`); GICv3; per-chiplet `MemoryRegion` view overlays the 256 MB `PRIVATE_BASE` (`0x1E00000000`) window onto each chiplet's own slice of `sysmem`, and the 64 KB `SYSREG_CP0` block at config-space `0x1FF1010000` onto the private-alias copy (`0x1E01010000`); `MSR S1_1_C15_C14_0, x0` stubbed as `ARM_CP_NOP` (BL31 EL3 MMU enable); per-chiplet 16550 UART on dedicated chardevs; shared flash RAM at `0x1F80000000` (64 MB, aliased per chiplet). |
| `r100_cmu.c` | CMU | 20 blocks / chiplet; PLL lock + mux_busy return idle instantly. |
| `r100_pmu.c` | PMU | `CPU_CONFIGURATION` writes mirror `CPU_STATUS` and invoke `arm_set_cpu_on(mpidr, RVBAR, target_el=3)` with the FW-written RVBAR (covers BL1 cold-boot *and* BL31 PSCI warm-boot release); `DCL{0,1}_CONFIGURATION` → `DCL{0,1}_STATUS` mirror. Dual-mapped at config-space + private alias. |
| `r100_sysreg.c` | SYSREG | Chiplet ID (0-3) via SYSREMAP. Dual-mapped. |
| `r100_hbm.c` | HBM3 | Sparse `GHashTable` write-back (6 MB window, default `0xFFFFFFFF`); ICON `test_instruction_req0/req1` RMW mirror. |
| `r100_qspi.c` | QSPI bridge | DW SSI inter-chiplet register access: `0x70` (1-word read), `0x80` (1-word write), `0x83` (16-word burst write, used by BL1 `qspi_bridge_load_image()` to stage `tboot_n`); latches `DW_SPI_SYSREG_ADDRESS` upper-addr, rebuilds 28-bit offset into the slave chiplet's `PRIVATE_BASE`. |
| `r100_qspi_boot.c` | QSPI_ROT (DWC_SSI master) | Status register permanently idle (`TFNF\|TFE\|RFNE`, `BUSY=0`), `TXFLR=0`, DRX reads return `0x82` (`FLASH_READY\|WRITE_ENABLE_LATCH`) so `qspi_boot.c:tx_available()` / `check_read_*_status()` exit on iteration 0; covers BL1 flash-load and BL31 `NOR_FLASH_SVC_*` SMC paths. Dual-mapped (`0x1FF0500000` / `0x1E00500000`). |
| `r100_rbc.c` | RBC/UCIe | 6 blocks / chiplet (V00/V01/V10/V11/H00/H01); dual-mapped cfg-space (`0x1FF5xxxxxx`) + private alias (`0x1E05xxxxxx`); link-up sentinel `global_reg_cmn_mcu_scratch_reg1 = 0xFFFFFFFF` + `lstatus_link_status = 1`. |
| `r100_smmu.c` | SMMU-600 TCU | `CR0 → CR0ACK` mirror + `GBPA.UPDATE` auto-clear (BL2 `smmu_early_init`); CMDQ walker: on each `CMDQ_PROD` write, walks `(old_cons, new_prod]` in guest DRAM, writes 32-bit 0 to the `MSI_ADDRESS` of every `CMD_SYNC` with `CS=SIG_IRQ` (FreeRTOS `smmu_sync()` points MSI back at the sync slot), and advances `CMDQ_CONS=PROD`. |
| `r100_pvt.c` | PVT monitor | 5 instances / chiplet (ROT + 4 DCL); `PVT_CON_STATUS=0x3` (idle), per-sensor `_valid=1`, rest is RAM. |
| `r100_dma.c` | PL330 DMA | Fake completion on `ch_stat[0].csr` / `dbgstatus` / `dbgcmd` polls. |
| `r100_logbuf.c` | HILS ring tail | Polls chiplet 0's 2 MB `.logbuf` ring at `0x10000000` on a 50 ms `QEMU_CLOCK_VIRTUAL` timer; drains 128 B `struct rl_log` entries to a dedicated chardev (CLI default `/tmp/remu_hils.log`). |
| `remu_addrmap.h` | — | All address constants (from `g_sys_addrmap.h`). |

Additional RAM stubs created inline in `r100_soc.c`: PCIe sub-controller (`pmu_release_cm7` writes + `PHY{0..3}_SRAM_INIT_DONE` bit [0] seeded so silicon BL1's `cm7_wait_phy_sram_init_done` poll exits on iteration 0), CSS600 CNTGEN (generic timer reset), inter-chiplet HBM-init mailboxes (CP0.M4 + per-chiplet C0-C3 slots). Chiplet-wide `unimplemented_device` fallbacks cover both the config-space container and the 256 MB private-alias window.

### Infrastructure

- `r100-soc` machine type registered with `-machine` suffix.
- CLI: `remu build` / `remu run` / `remu status` / `remu gdb` / `remu images`.
- q-sys integration via `external/ssw-bundle`; TF-A builds with CA73 errata + Spectre v4 workarounds disabled (`platform.mk`).

### Observed boot trace (`-p silicon`)

Chiplet 0 (stdio):

```
NOTICE:  BL1: check QSPI bridge
NOTICE:  BL1: Boot mode: NORMAL_BOOT (5)
NOTICE:  BL1: Boot reason: Cold reset(POR)
NOTICE:  BL1: Load tboot_p0
INFO:    Release reset of CM7
NOTICE:  BL1: pmu_release_cm7 complete
NOTICE:  BL1: Load tboot_p1
NOTICE:  BL1: check QSPI bridge
NOTICE:  BL1: Load tboot_u
NOTICE:  BL1: Detected secondary chiplet count: 3
NOTICE:  BL1: ZEBU_CI: 0
NOTICE:  BL1: Load tboot_n
NOTICE:  BL1: Release reset of CP0.cpu0 of chiplet-{1,2,3}
NOTICE:  BL1: Booting BL2
NOTICE:  BL2: Detected secondary chiplet count: 3
NOTICE:  BL2: Init CMU of chiplet-0
NOTICE:  BL2: Init HBM of chiplet-0
NOTICE:  HBM3 6400Mbps
   ... EXTEST + DBI lane-repair scan ...
ERROR:   CH[0] Unrepairable PKG
ERROR:   HBM Boot on chiplet0 FAIL
```

Chiplets 1-3 (`/tmp/remu_uart{1,2,3}.log`) each reach their own `BL2: Init HBM of chiplet-{1,2,3}` and stop with the same `Unrepairable PKG` verdict — the four chiplets are running the same BL2 code path in parallel against the common `r100_hbm.c` stub.

### CP1 boot — remaining REMU work

Each chiplet has two independent clusters (CP0 and CP1), each running its own BL31 + FreeRTOS. On silicon, BL2 on each chiplet's CP0.cpu0 releases that chiplet's CP1.cpu0 via three MMIO writes (`rebel_h_bl2_setup.c:865-871`):

```c
plat_set_cpu_rvbar(CHIPLET_ID, CLUSTER_CP1, CPU0, GPT_DEST_ADDR_BL31_CP1);  /* 0x14100000 */
plat_pmu_cl_on(CHIPLET_ID, CLUSTER_CP1);                                    /* CPUSEQ + NONCPU_CONFIG + L2_CONFIG + status polls */
plat_pmu_cpu_on(CHIPLET_ID, CLUSTER_CP1, CPU0);                             /* CPU0_CONFIG + PERCLUSTER_OFFSET*1 */
```

CP1.cpu0 then boots `bl31_cp1.bin` → `freertos_cp1.bin`, which runs its own `init_smp()` issuing PSCI CPU_ON SMCs to *its own* BL31_CP1 for CP1.cpu{1,2,3} — same mechanism CP0 uses, just landing at `CPU0_CONFIGURATION + PERCLUSTER_OFFSET*1 + PERCPU_OFFSET*cpuid`. Under `__TARGET_CP=1`, `rebel_h_pm.c:set_rvbar()` writes `SYSREG_CP1_OFFSET` (`0x1FF1810000` = `SYSREG_CP0_OFFSET + PER_SYSREG_CP*1`).

`remu fw-build -p silicon` already produces `bl31_cp1.bin` + `freertos_cp1.bin` and installs them into `images/` — the release path is unconditional under silicon. REMU-side work remaining to exercise CP1:

- [ ] **Per-chiplet CP1 image loaders** in the `remu run` command: currently `FW_IMAGES` maps CP1 binaries at chiplet-0 addresses only. Replicate `-device loader,addr=CHIPLET_OFFSET*n + base` for each secondary chiplet.
- [ ] **SYSREG_CP1 RAM** (`src/machine/r100_soc.c`): mount a 64 KB region per chiplet at `chiplet_base + 0x1E01810000` (private alias) and overlay the config-space address `chiplet_base + 0x1FF1810000` in each chiplet's CPU view — one-liner following the existing SYSREG_CP0 template so CP1 RVBAR writes land on readable RAM.
- [ ] **PMU** (`src/machine/r100_pmu.c`): drop the `if (cluster != 0) return 0` short-circuit in `r100_pmu_read_rvbar()` and compute `base = SYSREG_CP0_PRIVATE + cluster * PER_SYSREG_CP`. The existing `r100_pmu_handle_cpu_config()` decoder already computes MPIDR `(chiplet<<16)|(cluster<<8)|cpu` for cluster ∈ {0,1} and calls `arm_set_cpu_on()` — it just needs a non-zero RVBAR to return.

Cluster-level `CP0_NONCPU_CONFIGURATION` / `CP0_L2_CONFIGURATION` writes don't need a write-through mirror: `r100_pmu_set_defaults()` already pre-seeds both status registers to ON at reset, so BL2's `plat_pmu_cl_on` polls pass on the first iteration.

This work is gated behind the BL2 HBM3 blocker below — no point wiring CP1 until BL2 gets past HBM init.

### Other remaining items

- [ ] Pre-load BL2/BL31/FreeRTOS (both CP0 and CP1) into each secondary chiplet's iRAM/DRAM so BL1's cross-chiplet copy lands on real data (alternative: populate `flash.bin` with a GPT image and let BL1 DMA-stage from flash).
- [ ] Fix `sys/build.sh` where `debug` command resets CLI flags, so `CHIPLET_COUNT=1` builds work.
- [ ] Test GDB attach and multi-core debugging (`thread N` across all 32 vCPUs, including CP1 halves).
- [ ] **HBM3 init** (current BL2 blocker): BL2 runs real HBM PHY training at 6400 Mbps with EXTEST + DBI lane-repair scan; the stub in `r100_hbm.c` returns default values that BL2 reads as "Unrepairable PKG" → `HBM Boot on chiplet{N} FAIL` on all four chiplets. Either add DBI/repair sentinels to the HBM stub or build with `HBM_TEST=0` to skip the training loop.

### Success criteria

- All 4 chiplets print FreeRTOS startup banner on their UART (both CP0 and CP1 `Hello world FreeRTOS_CP`)
- Chiplet 0 discovers 3 secondary chiplets via QSPI bridge ✓
- All 32 vCPUs (4 chiplets × 2 clusters × 4 cores) reach the FreeRTOS shell or its equivalent on their respective cluster
- GDB can step through FW code on any chiplet's CP0 or CP1 vCPU

## Phase 2: Host Drivers

**Prerequisite**: Phase 1 complete — including the CP1 cluster release on every chiplet (see "CP1 boot — remaining REMU work" above). `q-cp` tasks on CP1 service a non-trivial slice of the host-facing FW interface, so opening the PCIe endpoint without CP1 online would hit missing task-registration paths.

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
| PMU (x4) | Boot-status + `CPU_CONFIGURATION` → `arm_set_cpu_on/off` for CP0 only today (BL1 cold + BL31 PSCI warm); DCL status mirror. Phase 1 completion requires dropping the `r100_pmu_read_rvbar()` `cluster!=0` short-circuit so CP1 release fans out the same way | TZPC enforcement | Same |
| SYSREG_CP0 (x4) | RAM (RVBAR), dual-mounted at private-alias + config-space via per-chiplet CPU view | Same | Same |
| SYSREG_CP1 (x4) | **Not yet modelled** — Phase 1 closer: RAM, dual-mounted at `0x1E01810000` / `0x1FF1810000` (CP0 template, `+PER_SYSREG_CP`) so BL2 CP1 RVBAR writes land | Same | Same |
| GIC600 | QEMU built-in | Same | Same |
| UART | Per-chiplet 16550 on dedicated chardevs + HILS ring tail | Same | Same |
| Timer | QEMU built-in + CSS600 CNTGEN stub | Same | Same |
| QSPI bridge | Cross-chiplet R/W via `PRIVATE_BASE` + upper-addr latch | Same | Same |
| QSPI_ROT (DWC_SSI) | Status-idle + `FLASH_READY\|WRITE_ENABLE_LATCH` DRX stub | Same | Same |
| RBC/UCIe (6 x4) | Link-ready stub + PMU RBC status=ON | Same | Same |
| DMA (PL330) | Fake-completion stub | Stub | Behavioral model |
| PCIe sub-controller | RAM stub (pmu_release_cm7 writes + PHY{0..3}_SRAM_INIT_DONE seed) | Real model | Same |
| PCIe/doorbell | N/A | Shared-mem bridge | Same |
| DNC (16 x4) | N/A | Return-success | Behavioral model |
| HDMA (x4) | N/A | Memcpy | Scatter-gather |
| RBDMA (x4) | N/A | Stub | Behavioral model |
| HBM3 (x4) | Sparse-writeback + ICON RMW mirror | Same | Same |
| SMMU TCU (x4) | CR0/GBPA mirror + CMDQ walker (CMD_SYNC MSI=0 + auto-advance CONS) | Bypass | Translation model |
| Mailboxes (x4) | Shared RAM (HBM-init handshake) | Same | Same |

## Timing Considerations

REMU is purely functional — no timing model. Implications:

- PLL locks, DMA, and HBM training complete instantly
- Timeout paths in FW may never trigger
- Race conditions that depend on hardware timing may not reproduce
- Performance counter values are meaningless
- FreeRTOS tick timer fires correctly, but wall-clock ratios differ

For timing-sensitive tests, annotated delays can be added to device models later.
