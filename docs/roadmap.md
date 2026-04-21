# REMU Roadmap

## Phase 1: FW Boot — complete

**Goal**: TF-A BL1 → BL2 → BL31 → FreeRTOS boots on all 4 chiplets, with both CP0 and CP1 clusters online on every chiplet.

**Status**: done. `./remucli fw-build -p silicon` + `./remucli run` boots every chiplet end-to-end:

- CP0 on every chiplet reaches `Hello world FreeRTOS_CP` / `hw_init: done` / `REBELLIONS$` on its own UART (`output/<run>/uart{0,1,2,3}.log`).
- All 16 CP1 vCPUs run q-cp FreeRTOS steady-state: `CP1.cpu0` in `ipm_samsung_receive`, `CP1.cpu{1,2,3}` in `taskmgr_fetch_dnc_task_worker_cp1`. Verified via `aarch64-none-elf-gdb -ex 'target remote :1234' -x tests/scripts/gdb_inspect_cp1.gdb` (pure-GDB script — the bundled `aarch64-none-elf-gdb` has no Python).
- HILS ring tail (`output/<run>/hils.log`) drains chiplet 0's CP1 task init (DNC cdump, RDSN / SHM / RBDMA HW INFO dumps, ICPI init, CS tasks started).

Per-fix debugging history (MPIDR encoding, per-chiplet GIC, `CP1_NONCPU_STATUS` pre-seed, `SYSREG_CP1` triple-mount, DCL/SHM/RDSN/RBDMA register stubs, per-chiplet CP0 image staging, …) lives in `git log`.

**Out of scope** (deliberately):
- Single-chiplet `CHIPLET_COUNT=1` builds — target is CR03 quad, always 4 chiplets.
- HBM3 PHY training fidelity — current stub drives `hbm3_phy_training_evt1` end-to-end on every channel; the only residual log line (`read optimal vref (vert) FAILED. No valid window found.`) is a documented FW fallback path.
- The `-p zebu` / `zebu_ci` / `zebu_vdk` FW profiles. Still buildable, but silicon is the only configuration we boot.

### Device models (`src/machine/`)

| File | Device | Behaviour |
|---|---|---|
| `r100_soc.c` | Machine type | 4 chiplets × 8 `cortex-a72` CPUs (MIDR overridden to CA73 r1p1 `0x411FD091`; MPIDR = `chiplet << 24 \| cluster << 8 \| core`, FW masks bits 24-31 when computing `plat_core_pos_by_mpidr`). One `arm-gicv3` per chiplet (`num-cpu=8`, `first-cpu-index = chiplet*8`, one 1 MB / 8-frame redistributor region: frames 0-3 = CP0.cpu0..3, 4-7 = CP1.cpu0..3) mounted in sysmem at priority 1 with priority-10 aliases in each chiplet's CPU view. Per-chiplet 16550 UART on dedicated chardevs, IRQ on SPI 33 of its own GIC. Shared flash RAM at `0x1F80000000`. |
| `r100_cmu.c` | CMU | 20 blocks / chiplet; PLL-lock + mux-busy idle instantly. |
| `r100_pmu.c` | PMU | `CPU_CONFIGURATION` → `arm_set_cpu_on(mpidr, RVBAR, EL3)` for both CP0 and CP1 (covers BL1 cold + BL31 PSCI warm). `DCL{0,1}_CONFIGURATION` → `_STATUS` mirror. `{CP0,CP1}_{NONCPU,L2}_STATUS` pre-seeded ON so `plat_pmu_cl_on(cluster)` polls pass on iteration 0. Dual-mapped cfg-space + private-alias. |
| `r100_sysreg.c` | SYSREG | Chiplet ID via SYSREMAP. Dual-mapped. |
| `r100_hbm.c` | HBM3 | Sparse `GHashTable` write-back (default `0xFFFFFFFF` outside PHY range / `0` inside). ICON `test_instruction_req0/req1` RMW mirror. EXTEST-aware `rd_wdr` compare-vector stub. PHY training sentinels (`phy_train_done`, `prbs_{read,write}_done`, `schd_fifo_empty_status` all "done=1"); runs `hbm3_run_phy_training` end-to-end on all 16 channels / chiplet. |
| `r100_qspi.c` | QSPI bridge | DW SSI inter-chiplet R/W (`0x70` read, `0x80` single write, `0x83` 16-word burst) via `PRIVATE_BASE` + upper-addr latch. |
| `r100_qspi_boot.c` | QSPI_ROT (DWC_SSI) | Status-idle + DRX `FLASH_READY\|WRITE_ENABLE_LATCH`; covers BL1 flash-load and BL31 `NOR_FLASH_SVC_*`. Dual-mapped. |
| `r100_rbc.c` | RBC/UCIe | 6 blocks / chiplet; link-up sentinel (`scratch_reg1=0xFFFFFFFF`, `lstatus_link_status=1`). Dual-mapped. |
| `r100_dnc.c` | DCL CFG + RBDMA | Two `r100-dnc-cluster` / chiplet covering DCL0/DCL1 cfg windows: DNC slots (`ip_ver=V1_1`, `SP_STATUS01.test_done=1`), SHM banks (`INTR_VEC.tpg_done=1` + seeded IP info), MGLUE (RDSN `STATUS0=ALL_PREPARED`, `TE{0,3}_RPT0=valid+pass`). One `r100-rbdma` / chiplet seeding `IP_INFO0..2`. Sparse `GHashTable` write-back. |
| `r100_smmu.c` | SMMU-600 TCU | `CR0 → CR0ACK` + `GBPA.UPDATE` auto-clear. CMDQ walker: on `CMDQ_PROD` write, zeros each `CMD_SYNC.MSI_ADDRESS` and advances `CMDQ_CONS=PROD`. |
| `r100_pvt.c` | PVT monitor | 5 / chiplet; `PVT_CON_STATUS=0x3`, per-sensor `_valid=1`. |
| `r100_dma.c` | PL330 DMA | Fake-completion stub. |
| `r100_logbuf.c` | HILS ring tail | 50 ms poll of chiplet 0's 2 MB `.logbuf` ring; drains `struct rl_log` to a chardev (CLI writes `output/<run>/hils.log`). |
| `remu_addrmap.h` | — | All address constants (from `g_sys_addrmap.h`). |

Additional inline RAM stubs in `r100_soc.c`: PCIe sub-controller (`PHY{0..3}_SRAM_INIT_DONE` seeded so BL1 `cm7_wait_phy_sram_init_done` exits on iteration 0), CSS600 CNTGEN (generic timer reset), inter-chiplet HBM-init mailboxes, triple-mounted `SYSREG_CP{0,1}` RAM (private alias + cfg-space overlay + chiplet-local CPU-view overlay). Chiplet-wide `unimplemented_device` catch-alls on both the config-space container and the 256 MB private-alias window.

### Infrastructure

- `r100-soc` machine type registered with `-machine` suffix.
- `./remucli` (thin wrapper around `cli/remu_cli.py`): `build` / `run` / `status` / `gdb` / `images` / `fw-build`.
- q-sys FW source in `external/ssw-bundle` (TF-A with CA73 errata + Spectre v4 workarounds disabled in `platform.mk`).
- `external/qemu` pinned at upstream `v9.2.0`, kept unmodified in git terms. `./remucli build` idempotently symlinks remu sources into `hw/arm/r100/`, injects `subdir('r100')` into `hw/arm/meson.build`, and re-applies `cli/qemu-patches/*.patch` (currently one: `0001-arm-gicv3-first-cpu-index.patch` — adds a `first-cpu-index` property so per-chiplet GIC instances bind to disjoint CPU ranges; default 0 preserves upstream behaviour, upstreamable as-is). `.gitmodules` uses `ignore = dirty` so build-time mods stay off the superproject's `git status`.

### Success criteria

- All 4 chiplets print `Hello world FreeRTOS_CP` on their UART (both CP0 and CP1) ✓
- Chiplet 0 discovers 3 secondary chiplets via QSPI bridge ✓
- All 32 vCPUs (4 chiplets × 2 clusters × 4 cores) reach FreeRTOS steady-state ✓
- GDB can step through FW code on any chiplet's CP0 or CP1 vCPU ✓ (see `docs/debugging.md`)

## Phase 2: Host Drivers

**Prerequisite**: Phase 1 complete. `q-cp` tasks on CP1 service a non-trivial slice of the host-facing FW interface, so opening the PCIe endpoint without CP1 online would hit missing task-registration paths.

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
| PMU (x4) | Boot-status + `CPU_CONFIGURATION` → `arm_set_cpu_on/off` for both CP0 and CP1 (BL1 cold, BL2 CP1 release, BL31 PSCI warm); DCL status mirror; `{CP0,CP1}_{NONCPU,L2}_STATUS` pre-seeded ON so `plat_pmu_cl_on(cluster)` polls pass on iteration 0 for either cluster. `read_rvbar()` indexes `SYSREG_CP0_PRIVATE + cluster * PER_SYSREG_CP` so both clusters share the same code path | TZPC enforcement | Same |
| SYSREG_CP{0,1} (x4) | RAM (RVBAR), triple-mounted per chiplet: private alias + cfg-space overlay inside `cfg_mr` (absolute cross-chiplet form) + chiplet-local overlay in the per-chiplet CPU view (local form) | Same | Same |
| GIC600 (x4) | Four `arm-gicv3` instances (num-cpu=8 each, one 8-frame redistributor region per GIC), mounted into the owning chiplet's CPU view at priority 10 at the FW-hardcoded chiplet-local `R100_GIC_DIST_BASE` / `R100_GIC_REDIST_CP0_BASE` | Same (cross-chiplet SPI routing if ever needed) | Same |
| UART (x4) | Per-chiplet 16550 on dedicated chardevs + HILS ring tail; each UART's IRQ wired to SPI 33 of its own chiplet's GIC | Same | Same |
| Timer | QEMU built-in + CSS600 CNTGEN stub | Same | Same |
| QSPI bridge | Cross-chiplet R/W via `PRIVATE_BASE` + upper-addr latch | Same | Same |
| QSPI_ROT (DWC_SSI) | Status-idle + `FLASH_READY\|WRITE_ENABLE_LATCH` DRX stub | Same | Same |
| RBC/UCIe (6 x4) | Link-ready stub + PMU RBC status=ON | Same | Same |
| DMA (PL330) | Fake-completion stub | Stub | Behavioral model |
| PCIe sub-controller | RAM stub (pmu_release_cm7 writes + PHY{0..3}_SRAM_INIT_DONE seed) | Real model | Same |
| PCIe/doorbell | N/A | Shared-mem bridge | Same |
| DNC (16 x4) | Register-window stub via DCL CFG row (ip_ver + SP test) | Return-success task dispatch | Behavioral model |
| HDMA (x4) | N/A | Memcpy | Scatter-gather |
| HBM3 (x4) | Sparse-writeback + ICON RMW mirror + EXTEST-aware `rd_wdr` sentinels + PHY fail/busy default 0 with `phy_train_done` / `prbs_{read,write}_done` / `schd_fifo_empty_status` overrides | Same | Same |
| DCL CFG / SHM / MGLUE (x4 × 2 DCLs) | `r100-dnc-cluster` sparse stub: DNC `ip_ver=V1_1` + `SP_STATUS01.test_done=1`, SHM `INTR_VEC.tpg_done=1` + seeded IP info, RDSN `STATUS0=ALL_PREPARED` + `TE{0,3}_RPT0=valid+pass` | Behavioral (DNC task dispatch) | Full behavioral model |
| RBDMA (x4) | `r100-rbdma` sparse stub: seeded `IP_INFO0..2` (rel_date, ver, chiplet_id) | Stub → real HDMA | Behavioral model |
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
