# REMU Roadmap

## Phase 1: FW Boot (current)

**Goal**: TF-A BL1 → BL2 → BL31 → FreeRTOS boots on all 4 chiplets.

**Status**: Chiplet 0 now reaches FreeRTOS's `terminal_task` shell (`REBELLIONS$ ` prompt) with all four CP0 cores online and no SMMU sync retries in the HILS log. After BL1 → BL2 → BL31 → FreeRTOS EL1, `init_smp()` issues `psci_cpu_on()` for cores 1-3; BL31 services the SMC, each secondary CPU boots into BL31's warm-boot entry and `ERET`s to `secondary_prep_c` at EL1, flips its "up" flag, and prints `core N is online`. FreeRTOS then completes `hw_init: done`, `smmu_init()` finishes cleanly (every `CMD_SYNC` returns before the first loop iteration now), registers the CP interface, configures thermal thresholds, registers post-processing tasks, and presents the shell prompt. The PSCI fix was a memory-model gap rather than a PMU-logic change: BL31's `rebel_h_pm.c:set_rvbar()` writes the warm-boot entry to the SYSREG_CP0 **config-space** address (`0x1FF1010000 + RVBARADDR0_LOW + cpuid*8`), but REMU previously only mounted SYSREG_CP0 at the chiplet-local **private-alias** address (`0x1E01010000`), so those writes vanished into the unimplemented-device catch-all and `r100_pmu_read_rvbar()` read back 0, causing us to skip `arm_set_cpu_on()`. Fixed by overlaying a chiplet-local alias of the private-alias SYSREG_CP0 RAM at the config-space address inside each chiplet's CPU view — matching silicon, where each chiplet's decoder routes `0x1FF1010000` to its own SYSREG_CP0 instance. The CMD_SYNC fix layers CMDQ processing onto the SMMU TCU stub: the stub now caches the CMDQ base PA + log2 size from `SMMU_CMDQ_BASE`, and on every producer write to `SMMU_CMDQ_PROD` walks the newly-enqueued entries in guest DRAM via `cpu_physical_memory_read()`, writes a 32-bit zero (the SMMU's default `CMDQ_SYNC_MSI_DATA`) to the MSI address of each `CMD_SYNC` with CS=SIG_IRQ (which `smmu_sync()` points back at the sync slot itself), and advances `SMMU_CMDQ_CONS` to match PROD so the producer-side `smmu_q_has_space()` poll exits on the first iteration. The earlier blockers are unchanged: per-chiplet CPU memory views so each chiplet reads its own `CHIPLET_ID` via the 256 MB `0x1E00000000` private window; per-chiplet UARTs so boot traces are legible; the Samsung IMPDEF `MSR S1_1_C15_C14_0, x0` cache-flush stubbed as `ARM_CP_NOP` (otherwise BL31 UNDEFs right after `enable_mmu_direct_el3_bl31`); secondary chiplets released via QSPI-staged BL2 + PMU `CPU_CONFIGURATION`, starting at the private-alias `0x1E00028000` entry so PC-relative ADRP stays valid; shared mailbox RAM for inter-chiplet HBM-init; SMMU-600 TCU CR0/CR0ACK mirror + GBPA.UPDATE auto-clear (BL2's `smmu_early_init`); 5-instance-per-chiplet PVT stub so `pvt_init()`'s `PVT_ENABLE_{PROC,VOLT,TEMP}_CONTROLLER` idle-polls exit; HILS ring tail (`r100-logbuf-tail`) drains `RLOG_*/FLOG_*` entries from the 2 MB `.logbuf` ring at `0x10000000` onto its own chardev. Next blocker after the shell prompt: a stray `INFO: spi tx available timeout error` from q-sys `drivers/qspi_boot/qspi_boot.c` — the FreeRTOS-side QSPI driver polls the DW SSI status register (`DW_SR_TFNF_BIT` / `DW_SR_BUSY_BIT`) on top of our QSPI bridge stub, which only models the register-access instruction path. Needs either a status-register stub on the master DW SSI or routing the boot-path access through the existing bridge.

### What's done

- `r100-soc` machine type: 4 chiplets, 32 vCPUs, GICv3, per-chiplet 16550 UART
  - CPU is `cortex-a72` with MIDR overridden to Cortex-A73 r1p1 (0x411FD091) so TF-A's `get_cpu_ops_ptr` accepts it and CA73 errata workarounds skip
  - Each chiplet's CPUs use a dedicated QEMU `MemoryRegion` view (`r100_build_chiplet_view`) that aliases the 256 MB `PRIVATE_BASE` window (`0x1E00000000`) onto that chiplet's own slice of sysmem, so every chiplet reads its own `CHIPLET_ID` and `SECOND_CHIPLET_CNT` — the hardware "chiplet-local address-decoder alias" modelled in software
- UART: one QEMU `serial-mm` (16550, regshift=2) per chiplet, each bound to its own chardev — chiplet 0 goes to `mon:stdio`, chiplets 1-3 to `/tmp/remu_uart{1,2,3}.log` (separate streams avoid interleaved BL1/BL2 banners)
- HILS ring tail: `r100-logbuf-tail` polls chiplet 0's FreeRTOS `.logbuf` (2 MB ring of 128 B `struct rl_log` entries at physical `0x10000000`) from a 50 ms QEMU timer and drains newly-populated entries onto its own chardev (CLI default `/tmp/remu_hils.log`). Surfaces `RLOG_*` / `FLOG_*` messages (`[smmu] Failed to sync`, UCIe `TimeoutWindow` programming, etc.) even before FreeRTOS's own `terminal_task` drain can run
- CMU stubs (20 blocks per chiplet): PLL lock/mux_busy return instantly
- PMU stub: OM_STAT=NORMAL_BOOT, RST_STAT=PINRESET, all CPU/cluster/DCL/RBC status registers seeded to powered-on
- SYSREG stub: per-chiplet ID (0-3) via SYSREMAP registers
- HBM3 stub: training-complete status
- QSPI bridge: Designware SSI protocol, cross-chiplet register access; supports 1-word read (`0x70`), 1-word write (`0x80`), and 16-word burst write (`0x83`) — the last used by BL1 `qspi_bridge_load_image()` to stage `tboot_n` into secondary chiplets' iRAM. Latches the upper-address write (`DW_SPI_SYSREG_ADDRESS`, 24-bit field `0x0C4058`) into `s->upper_addr`; subsequent reads/writes rebuild the 28-bit offset into the slave chiplet's `PRIVATE_BASE` window (`addr & 0x0FFFFFFF`) so RBC accesses above the 64 MB 26-bit window reach the right block
- Per-chiplet SYSREG_CP0 RAM region (64 KB) backing BL1's `plat_set_cpu_rvbar()` writes **and** BL31's `rebel_h_pm.c:set_rvbar()` writes. Mounted at two addresses within each chiplet's CPU view: private-alias `0x1E01010000` (QSPI-driven BL1 path) and config-space `0x1FF1010000` (BL31's direct on-CPU path for PSCI CPU_ON). The config-space address is added as an overlay in `r100_build_chiplet_view()` pointing to the same backing RAM, mirroring the chiplet-local decoder used on silicon. The PMU reads RVBARADDR0_LOW/HIGH back from the private-alias copy when a CPU release is triggered — both paths converge on the same storage
- PMU secondary core release (PSCI path): FreeRTOS's `init_smp()` → `psci_cpu_on(mpidr, secondary_prep_c)` → BL31 `rebel_h_pwr_domain_on()` → `set_rvbar(cpuid, bl31_warm_entrypoint)` (writes to SYSREG_CP0 config-space; now reaches the chiplet's SYSREG_CP0 RAM via the CPU-view overlay) + `pmu_cpu_on(cpuid)` (writes `CPU_ON_WITH_INITIATE_WAKEUP` to PMU `CPU_CONFIGURATION`). The PMU write handler decodes cluster/cpu, mirrors `CPU_STATUS` so BL31's post-write poll exits, reads back the just-written RVBAR, and calls `arm_set_cpu_on(mpidr, bl31_warm_entrypoint, target_el=3)`. The secondary CPU starts at EL3 inside BL31's warm-boot trampoline, restores context, and `ERET`s to `secondary_prep_c` at EL1. All 4 CP0 cores on chiplet 0 come online; `init_smp()` completes
- PMU cold-boot release (BL1 path): `CPU_CONFIGURATION` writes for the initial BL2 hand-off decode cluster/cpu, update `CPU_STATUS`, and call `arm_set_cpu_on(mpidr, entry)` with the unmodified `PRIVATE_BASE`-relative RVBAR (`0x1E00028000`); the chiplet's CPU view redirects that PC to the chiplet's own iRAM, so ADRP-based linker-symbol resolution remains correct
- PMU D-Cluster power-state mirror: writes to `DCL{0,1}_CONFIGURATION` are reflected into `DCL{0,1}_STATUS` synchronously so BL2's `pmu_reset_dcluster()` poll on `DCL_STATUS_MASK` completes
- SMMU-600 TCU stub (`r100_smmu.c`) at `0x1FF4200000`: mirrors `SMMU_CR0` writes into `SMMU_CR0ACK` (masked), and auto-clears the `UPDATE` bit on `SMMU_GBPA` writes so BL2's `smmu_early_init()` `while (!(cr0ack & EVENTQEN))` / `while (gbpa & UPDATE)` polls terminate. Also processes FreeRTOS-side command-queue submissions: caches the NS CMDQ base + `log2size` from `SMMU_CMDQ_BASE` (64-bit register, extracted via `Q_BASE_ADDR_M`/`Q_LOG2SIZE_M`), and on every producer write to `SMMU_CMDQ_PROD` walks entries in `(old_cons, new_prod]` via `cpu_physical_memory_read()`. For each `CMD_SYNC` with `CS == SIG_IRQ`, writes a 32-bit 0 (the default `CMDQ_SYNC_MSI_DATA`) to `cmd[1] & SYNC_1_MSIADDRESS_M` — `smmu_sync()` in q-sys sets `msiaddr = slot_addr` so the MSI write overwrites `cmd[0]` with 0, exactly what the driver's `*base == 0` poll waits for. Finally clobbers `SMMU_CMDQ_CONS` with the new PROD (error field stays clear). FreeRTOS's `smmu_init()` → `smmu_invalidate_all_sid()` / `smmu_tlbi_all()` / per-SID `smmu_init_ste()` now all return on the first poll iteration; the HILS log shows zero `[smmu] Failed to sync` entries (previously 12+ per boot)
- PVT monitor stubs (`r100_pvt.c`), 5 instances per chiplet (ROT, DCL0_PVT0/1, DCL1_PVT0/1) at their silicon base addresses (`0x1FF0260000`, `0x1FF2120000`, etc.). Returns reset value `0x3` on `PVT_CON_STATUS` (+0x1C) reads so FreeRTOS's `PVT_ENABLE_{PROC,VOLT,TEMP}_CONTROLLER` idle-polls exit on the first iteration; returns `1` on per-sensor `ps_valid/vs_valid/ts_valid` status reads (offsets `0x400+r*0x40+0x0C`, `0x800+r*0x80+0x40`, `0x2800+r*0x40+0x0C`) so `PVT_WAIT_UNTIL_VALID` doesn't burn 10 k cycles per sensor. All other reads/writes fall through to a 64 KB read-write-back register file
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
INFO:    Entry point address = 0x0     [BL31_BASE]
INFO:    SPSR = 0x3cd
INFO:    BL31: Chiplet reset flag: 0x0 status: 0x0050
INFO:    rebel_h: Preparing to jump to the next O/S
NOTICE:  BL31: Chiplet-0 / Boot reason: Cold reset(POR)
NOTICE:  BL31: v2.9.0(debug):v3.2.0-dev-272-...
INFO:    GICv3 without legacy support detected.
INFO:    ARM GICv3 driver initialized in EL3
INFO:    Maximum SPI INTID supported: 255
NOTICE:  BL31: multi chiplet detected, init TZPC for RBC
INFO:    BL31: Initializing runtime services
INFO:    BL31: Preparing for EL3 exit to normal world
INFO:    Entry point address = 0x200000  [FreeRTOS EL1 entry]
INFO:    SPSR = 0x3c5
core 1 is up                  [FreeRTOS secondary_prep_c on CPU 1]
INFO:    cpuid: 1, cpu_on requested
core 1 is online
core 2 is up
INFO:    cpuid: 2, cpu_on requested
core 2 is online
core 3 is up
INFO:    cpuid: 3, cpu_on requested
core 3 is online

Hello world FreeRTOS_CP
hw_init: done
REBELLIONS$  INFO:    spi tx available timeout error   [qspi_boot.c:spi_tx_available() — next blocker]
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

HILS ring tail (chiplet 0 `.logbuf` drain, `/tmp/remu_hils.log`):

```
[HILS 3776707506 cpu=0 INFO    func=255] [CL0_V10 (C0S0)] TimeoutWindow req=50000000 ns prog=50069504 ns (tick=190 window=65535 rbcm_clk=250
[HILS 3777491985 cpu=0 INFO    func=255] [CL0_V11 (C0S1)] TimeoutWindow req=50000000 ns prog=50069504 ns (...
[HILS 3777583196 cpu=0 INFO    func=255] [CL0_H00 (C0E0)] TimeoutWindow req=50000000 ns prog=50069504 ns (...
[HILS 3777656677 cpu=0 INFO    func=255] [CL0_H01 (C0E1)] TimeoutWindow req=50000000 ns prog=50069504 ns (...
[HILS 3808574691 cpu=0 DBG     func=255] chiplet count is 4
[HILS 3808805468 cpu=0 INFO    func=255] chiplet id 0
[HILS 3809181117 cpu=0 DBG     func=255] cp interface is registered
[HILS 3809483316 cpu=0 INFO    func=255] ATS is disabled by capability
[HILS 3809514266 cpu=0 DBG     func=255] sw_init: done
[HILS 3821809363 cpu=0 INFO    func=255] Thermal thresholds configured: Exit=75°C, T0=92°C, T1=102°C, Cat=112°C
[HILS 3836473216 cpu=0 DBG     func=255] register postproc task id 3
[HILS 3844873085 cpu=0 INFO    func=255] failed to restore pcie event log   [non-fatal; post-shell]

# No [smmu] Failed to sync entries — previously 12+ per boot, now zero.
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
- [x] BL2 → BL31 handoff: all 4 chiplets now execute BL31 past `enable_mmu_direct_el3_bl31`. The `ERET` from BL2 actually succeeded — BL31 was taking an `Unknown-reason` EL3 sync exception on the Samsung IMPDEF cache-flush instruction `MSR S1_1_C15_C14_0, x0` (encoded as `sys #1, C15, C14, #0`) that TF-A's `enable_mmu.S` issues right after enabling the EL3 MMU. `cortex-a72` doesn't model this IMPDEF encoding; `cortex-a73` (the R100 CP) does. Fixed by stubbing the SYS encoding as `ARM_CP_NOP` via `define_arm_cp_regs()` in `r100_soc_init()`
- [x] BL31 → FreeRTOS handoff: the `ERET` into EL1h at `0x200000` was correct all along — FreeRTOS just doesn't emit anything to the UART on its own. Its `printf_` impl buffers to a RAM ring at `0x10000000` (HILS logbuf) and a later task flushes it to the DW-APB UART. Confirmed by GDB breakpoint at `0x200000` (CPU0 hits it with CPSR=0x3c5) and by reading `[smmu] Failed to sync` ASCII out of `0x10000000` via QMP `xp`
- [x] PVT monitor stub (`r100_pvt.c`): FreeRTOS's `driver_init` level-5 `pvt_init()` calls `PVT_ENABLE_{PROC,VOLT,TEMP}_CONTROLLER` which does `while (!PVT_IS_*_CON_IDLE(regs)) ;`, polling `PVT_CON_STATUS` (+0x1C, reset value `0x3`) and the per-sensor `_valid` status bits. Without a stub the poll runs forever because unmapped MMIO returns 0. The stub returns `0x3` for `PVT_CON_STATUS` and `1` for sensor-validity reads, lets everything else fall through to a RAM-backed register file. Mounted at the 5 instance bases (`ROT_PVT_CON_BASE` + the 4 DCL PVT bases) on every chiplet
- [x] FreeRTOS UART routing — HILS ring tail: `r100-logbuf-tail` (`src/machine/r100_logbuf.c`) polls chiplet 0's 2 MB `.logbuf` ring at physical `0x10000000` on a 50 ms QEMU virtual-clock timer and drains newly-populated `struct rl_log` entries (`tick`, `type`, `cpu`, `func_id`, `task[16]`, `logstr[100]` @ 128 B stride, `RL_LOG_ENTRY_CNT=16384`) onto a dedicated chardev. The CLI wires the 5th `-serial` slot (`serial_hd(4)`) to `/tmp/remu_hils.log` by default (override with `--hils-log PATH` or `--hils-log stderr`). Confirmed surfacing FreeRTOS `[smmu] Failed to sync` retries and UCIe `CL0_V10/V11/H00/H01 (Cnn) TimeoutWindow req=... prog=...` INFO messages during boot, independent of the in-FW `terminal_task` drain (which still can't run because of the next item). `printf_`-based output (e.g. `core 1 is up`) continues to reach the chiplet's 16550 UART directly via `uart_putc` → `serial-mm`, so no DW-APB register model is required for Phase 1 visibility; option (b) — a full DW-APB UART register file at `PERI0_UART0_BASE` — remains a nice-to-have for interrupt-driven `uart_isr` / `uart_rx` parity
- [x] PSCI CPU_ON warm-boot path: the PMU's RVBAR read path was correct, but BL31's `rebel_h_pm.c:set_rvbar()` writes to the SYSREG_CP0 **config-space** address `0x1FF1010000 + RVBARADDR0_LOW + cpuid*8`, not the private-alias `0x1E01010000` that BL1's QSPI path uses. That address wasn't mounted, so the writes disappeared and `r100_pmu_read_rvbar()` read back 0. Fixed in `r100_build_chiplet_view()` by overlaying the chiplet's private-alias SYSREG_CP0 RAM at `R100_CP0_SYSREG_BASE` within each chiplet's CPU view — mirrors silicon's chiplet-local config-space decoder and keeps both write paths coherent. All 4 CP0 cores on chiplet 0 now come online; FreeRTOS reaches the `REBELLIONS$ ` shell prompt
- [x] SMMU command-queue `CMD_SYNC` acknowledgement: FreeRTOS's EL1 SMMU driver (`external/.../q/sys/drivers/smmu/smmu.c:smmu_sync()`) issues `CMD_SYNC` entries with `CS=SIG_IRQ` and `msiaddr` pointing at the sync-entry slot itself, then polls `*slot == 0` for completion. `r100_smmu.c` now caches the NS CMDQ base/size from `SMMU_CMDQ_BASE`, and on each producer write to `SMMU_CMDQ_PROD` walks `(old_cons, new_prod]` in guest DRAM: for every CMD_SYNC with IRQ completion it writes a 32-bit 0 to the slot (matching the SMMU-600 default `CMDQ_SYNC_MSI_DATA=0`), then clobbers `SMMU_CMDQ_CONS` to match PROD so `smmu_q_has_space()` returns immediately. `smmu_init()` → `smmu_invalidate_all_sid()` / `smmu_tlbi_all()` / per-SID `smmu_init_ste()` all complete on the first iteration; HILS log goes from 12+ `[smmu] Failed to sync` per boot to zero
- [ ] QSPI boot path `spi tx available timeout error`: after FreeRTOS hits the shell prompt, q-sys `drivers/qspi_boot/qspi_boot.c:spi_tx_available()` times out polling `DW_SR_TFNF_BIT`/`DW_SR_BUSY_BIT` on the DW SSI status register. Our `r100-qspi-bridge` stub only models the BL1-side inter-chiplet command-set; extend it (or add a secondary DW SSI status-register stub) so `TFNF=1, BUSY=0` is asserted whenever the TX FIFO is drained
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
| SYSREG_CP0 (x4) | RAM (RVBAR capture) — dual-mounted at private-alias `0x1E01010000` (BL1/QSPI) and config-space `0x1FF1010000` (BL31/PSCI `set_rvbar`) via per-chiplet CPU-view overlay | Add CP1 + TZPC enforcement | Same |
| GIC600 | QEMU built-in | Same | Same |
| UART | Per-chiplet 16550 (serial-mm) on dedicated chardevs + HILS ring tail (polls chiplet 0 `.logbuf` at 0x10000000, drains RLOG_*/FLOG_* to own chardev) | Same | Same |
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
| SMMU TCU (x4) | CR0→CR0ACK mirror + GBPA.UPDATE auto-clear (BL2 `smmu_early_init`); CMDQ walker fires MSI=0 on CMD_SYNC and auto-advances CONS=PROD (FreeRTOS `smmu_sync()` unblocker) | Bypass | Translation model |
| Mailboxes (x4) | Shared RAM for inter-chiplet HBM-init handshake | Same | Same |

## Timing Considerations

REMU is purely functional — no timing model. Implications:

- PLL locks, DMA, and HBM training complete instantly
- Timeout paths in FW may never trigger
- Race conditions that depend on hardware timing may not reproduce
- Performance counter values are meaningless
- FreeRTOS tick timer fires correctly, but wall-clock ratios differ

For timing-sensitive tests, annotated delays can be added to device models later.
