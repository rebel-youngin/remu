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

## Phase 2: Host Drivers — in progress (M1-M8a done)

**Prerequisite**: Phase 1 complete. `q-cp` tasks on CP1 service a non-trivial slice of the host-facing FW interface, so opening the PCIe endpoint without CP1 online would hit missing task-registration paths.

**Goal**: kmd loads in an x86_64 QEMU guest, probes virtual PCI device, handshakes with FW, umd opens device.

### Milestone breakdown

Phase 2 is sliced into 9 incremental milestones. Each one boots end-to-end and commits cleanly.

| # | Milestone | Status | Deliverable |
|---|---|---|---|
| M1 | Two-binary build | done | `./remucli build` produces `qemu-system-{aarch64,x86_64}` from the single pinned QEMU tree |
| M2 | Shared-memory plumbing | done | `/dev/shm/remu-<name>/remu-shm` opened `share=on` by both QEMUs; `./remucli run --host` orchestrates both with clean signal-driven teardown |
| M3 | `r100-npu-pci` skeleton | done | PCI function `0x1eff:0x2030` exposes BAR0/2/4/5 at the sizes `rebel.h` expects; lazy RAM backing + `msix_init` table on BAR5 |
| M4 | Host BAR0 → shm splice | done | BAR0 is a container: shared backend at offset 0, lazy RAM on the tail. `./remucli run --host` auto-verifies via `info pci` / `info mtree` / `/proc/<pid>/maps` |
| M5 | NPU DRAM → shm splice | done | Chiplet-0 DRAM is a container: same shared backend at offset 0, lazy RAM on the tail past the FW image region. `tests/m5_dataflow_test.py` proves coherency both ways |
| M6 | Doorbell path (guest → FW) | done | BAR4 `MAILBOX_INTGR0/1` writes on the x86 side emit 8-byte `(offset, value)` frames over a Unix socket chardev; NPU-side `r100-doorbell` parses frames and injects them into the chiplet-0 `r100-mailbox` peripheral (Samsung `ipm` SFR at `R100_PCIE_MAILBOX_BASE`), which asserts its two masked-status outputs on chiplet-0 GIC SPI 184 (Group0) / SPI 185 (Group1) — matching silicon routing. `tests/m6_doorbell_test.py` drives synthetic frames end-to-end |
| M7 | MSI-X path (FW → guest) | done | Reverse direction. NPU-side `r100-imsix` MMIO trap at `R100_PCIE_IMSIX_BASE` (`0x1B_FFFF_F000`) catches 4-byte stores to `R100_PCIE_IMSIX_DB_OFFSET` (`0xFFC` = `REBELH_PCIE_MSIX_ADDR` on silicon), emits 8-byte `(offset, db_data)` frames over a second Unix socket chardev (mirror of M6's wire format). Host-side `r100-npu-pci` consumes the frames, decodes `vector = db_data & 0x7FF`, and calls `msix_notify()` — identical semantics to the DW PCIe MAC's address-match-to-MSI-X-TLP path. `tests/m7_msix_test.py` drives synthetic frames end-to-end (3 accepted + 1 oor + 1 bad-offset) and checks the per-frame debug tail + `GUEST_ERROR` log entries |
| M8a | ISSR bridge (both dirs) | done | Bidirectional plumbing for the Samsung-IPM `ISSR0..63` scratch words on both sides. NPU → host: `r100-mailbox` emits every MMIO-path ISSR write as an 8-byte `(bar4_offset, value)` frame on a third Unix socket chardev (`issr`); host-side `r100-npu-pci` consumes the frames, mirrors them into `bar4_mmio_regs[]` so subsequent guest `readl`s at `BAR4 + MAILBOX_BASE + idx*4` return the live value. Host → NPU: host BAR4 writes into the `MAILBOX_BASE` (0x80..0x180) range of the 4 KB MMIO overlay egress on the *existing* M6 `doorbell` chardev (offset disambiguates INTGR-triggers vs ISSR payload on the NPU side); `r100-doorbell` routes `MAILBOX_BASE` frames to `r100_mailbox_set_issr()` which updates the NPU-side ISSR without asserting any INTGR SPI — and without re-emitting the value on the `issr` chardev, so the update does not loop back. `tests/m8_issr_test.py` drives synthetic frames end-to-end for both directions, verifying host BAR4 readback mirrors NPU writes, NPU ISSR registers mirror host writes, and out-of-range offsets log `GUEST_ERROR` on both sides. No protocol-specific state lives in the device model — this is pure MMIO transport |
| M8b | `FW_BOOT_DONE` + `TEST_IB` ring | pending | First real protocol driven over the M8a ISSR plumbing: kmd polls BAR4+MAILBOX_BASE+0x10 for `FW_BOOT_DONE` (0xFB0D in ISSR[4]), then kmd and q-cp HIL exchange `TEST_IB` messages on the shared-DRAM ring. Requires either (a) running kmd + full q-sys FW end-to-end, or (b) adding a minimal `r100-hil-stub` NPU device that fakes `FW_BOOT_DONE` and a `TEST_IB` reply for a kmd-only test. Hits the roadmap success criterion |
| M9 | DNC stub + trivial umd job | pending | umd opens `/dev/rebellions0`, submits a zero-op job, DNC stub signals completion via MSI-X |

### Components (current state after M8a)

- **`r100-npu-pci`** (`src/host/r100_npu_pci.c`): x86-side PCI device, vendor/device `0x1eff:0x2030`, four BARs. BAR0 splices in a `HostMemoryBackend` via the `memdev` link at offset 0 (M4); BAR2 is plain lazy RAM; BAR4 is a container with a 4 KB MMIO head overlay (priority 10) that intercepts `MAILBOX_INTGR0/1` (0x8 / 0x1c) and emits `(offset, value)` frames on the `doorbell` chardev (M6), plus the `MAILBOX_BASE` payload range (0x80..0x180) — writes egress on the *same* `doorbell` chardev (the NPU side routes them to ISSR by offset), reads return values written-through from NPU-emitted `issr` frames (M8a); the remaining BAR4 space is lazy RAM. BAR5 carries the 32-vector MSI-X table + PBA (`msix_vector_use` called for every vector so `msix_notify` latches PBA bits even before the guest driver enables masking). An optional `msix` `CharBackend` consumes 8-byte `(offset, db_data)` frames emitted by the NPU-side `r100-imsix` and calls `msix_notify(pdev, db_data & 0x7FF)` (M7). An optional `issr` `CharBackend` consumes 8-byte `(bar4_offset, value)` frames emitted by the NPU-side `r100-mailbox` on every local ISSR write and mirrors them into `bar4_mmio_regs[]` so the guest sees a live shadow (M8a). The 4 KB MMIO overlay is installed whenever *either* `doorbell` or `issr` is connected.
- **`r100-soc` machine** (`src/machine/r100_soc.c`): `memdev` string property splices chiplet 0 DRAM over a shared `HostMemoryBackend` (M5). `doorbell` / `doorbell-debug` string properties resolve to `Chardev` instances; the machine instantiates a chiplet-0 `r100-mailbox` at `R100_PCIE_MAILBOX_BASE` (`0x1FF8160000`, mapped with `sysbus_mmio_map_overlap` at priority 10 so it outranks the `cfg_mr` catch-all) and wires its two masked-status outputs to chiplet-0 GIC SPI 184 (Group0) / SPI 185 (Group1), matching the silicon routing described in the Samsung `ipm` driver. A `r100-doorbell` sysbus device is then linked to that mailbox via a `DEFINE_PROP_LINK` (M6). `msix` / `msix-debug` string properties cause the machine to instantiate a chiplet-0 `r100-imsix` device overlaid at `R100_PCIE_IMSIX_BASE` (`0x1B_FFFF_F000`) on the global sysmem (visible through every chiplet's `sysmem_alias`) so FW stores on CA73 cores and the CM7 PCIe subsystem both reach it (M7). `issr` / `issr-debug` string properties are forwarded to the `r100-mailbox` instance as `issr-chardev` / `issr-debug-chardev` and drive the NPU → host ISSR egress (M8a).
- **`r100-mailbox`** (`src/machine/r100_mailbox.c`): models the Samsung `ipm_samsung` register block. Implements `MCUCTRL` / `INTGR{0,1}` (write-1-to-set) / `INTCR{0,1}` (write-1-to-clear) / `INTMR{0,1}` (mask) / `INTSR{0,1}` (raw pending) / `INTMSR{0,1}` (masked pending = `INTSR & ~INTMR`) / `MIF_INIT` / `IS_VERSION` / `ISSR0..63`. The two `qemu_irq` outputs (`irq[0]` for INTMSR0, `irq[1]` for INTMSR1) are asserted whenever the corresponding masked-pending word is non-zero. Exposes `r100_mailbox_raise_intgr(group, val)` so the doorbell can inject pending bits without round-tripping through MMIO, and `r100_mailbox_set_issr(idx, val)` so the doorbell's M8a host-write path can update scratch words without re-emitting them on the chardev. Every MMIO-path ISSR write emits an 8-byte `(bar4_offset, value)` frame on the optional `issr-chardev` (M8a); an `issr-debug-chardev` echoes `issr off=... val=... status=... count=...` per frame.
- **`r100-doorbell`** (`src/machine/r100_doorbell.c`): receives 8-byte `(offset, value)` frames on a `CharBackend`. Offsets in the `MAILBOX_INTGR0/1` slots call `r100_mailbox_raise_intgr()` (M6 trigger path); offsets in the `MAILBOX_BASE` payload range (0x80..0x180) call `r100_mailbox_set_issr()` (M8a host → NPU scratch-write path, no INTGR side-effect); everything else logs `GUEST_ERROR`. Optional debug chardev traces every frame as `doorbell off=... val=... status={intgr,issr,bad-offset} count=...` lines.
- **`r100-imsix`** (`src/machine/r100_imsix.c`): 4 KB MMIO window at `R100_PCIE_IMSIX_BASE`. Trapped 4-byte writes to offset `0xFFC` (`R100_PCIE_IMSIX_DB_OFFSET` = `REBELH_PCIE_MSIX_ADDR & 0xFFF`) emit 8-byte `(offset, db_data)` frames on a `CharBackend` to the host side. FW stores to the full `REBELH_PCIE_MSIX_ADDR` (`0x1B_FFFF_FFFC`) land here on CA73 / CM7 alike — silicon's DW PCIe `MSIX_ADDRESS_MATCH_*` snoop is modelled by this device's MMIO trap. Non-doorbell offsets inside the page read/write a local register file so FW side-probes don't trip QEMU's unimplemented-device log. Optional `debug-chardev` echoes every emitted frame as `imsix off=0xfff db_data=0x... vector=... count=...` lines.
- **Shared memory bridge**: `memory-backend-file` at `/dev/shm/remu-<name>/remu-shm`, opened with `share=on` by both QEMUs. Declared on x86 side as `-device r100-npu-pci,memdev=remushm`, on NPU side as `-machine r100-soc,memdev=remushm`.
- **Doorbell bridge (M6+M8a)**: Unix socket chardev at `output/<name>/host/doorbell.sock`. Host QEMU owns the listener (`server=on,wait=off`), NPU QEMU is a `reconnect=1` client. Carries both INTGR triggers (M6) and `MAILBOX_BASE` scratch writes (M8a) on the same wire, disambiguated by offset on the NPU side. ASCII trace of each NPU-observed frame at `output/<name>/doorbell.log` (including the M8a `status=issr` tag).
- **iMSIX-DB bridge (M7)**: second Unix socket chardev at `output/<name>/host/msix.sock`, same wire format as M6 but flowing in the opposite direction (NPU → host). Host owns the listener; NPU `reconnect=1` connects as client. Per-frame ASCII trace at `output/<name>/msix.log` (`msix off=... db_data=... vector=... status={ok,oor,bad-offset} count=...`).
- **ISSR bridge (M8a)**: third Unix socket chardev at `output/<name>/host/issr.sock`, same 8-byte `(bar4_offset, value)` frame format as M6/M7, flowing NPU → host. Emitted by `r100-mailbox` on every MMIO ISSR write (MAILBOX_BASE+4*idx), consumed by `r100-npu-pci` which mirrors the value into `bar4_mmio_regs[]`. Per-frame ASCII trace at `output/<name>/issr.log` (`issr off=... val=... status={ok,bad-offset} count=...`). Reverse direction (host → NPU) reuses the M6 doorbell socket.
- **`./remucli run --host`**: dual-QEMU orchestrator. Auto-verifies every bridge end-to-end: M4/M5 via PCI enumeration + host/NPU `info mtree` + `/proc/<pid>/maps`; M6 via host `info mtree` (`r100.bar4.mmio` overlay) + NPU `info qtree` (both `r100-doorbell` and `r100-mailbox` present); M7 via NPU `info mtree` (`r100-imsix` subregion at `0x1B_FFFF_F000`) + host `info mtree` (`r100.bar5.msix` with `msix-table` / `msix-pba` overlays, proving `msix_init` succeeded); M8a via NPU `info qtree` showing `issr-chardev = "issr"` on `r100-mailbox` and host `info qtree` showing `issr = "issr"` on `r100-npu-pci`. Both QEMU monitors exposed as unix sockets under `output/<name>/{host,npu}/monitor.sock`.

### Pending components

- **DNC stub**: accepts tasks, immediately generates completion interrupt (M9).
- **HDMA stub**: performs actual memcpy between DRAM regions (M9).

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
| PCIe/doorbell | N/A | Shared-mem bridge (M4/M5) + BAR4 MMIO overlay → chardev frame → `r100-mailbox` INTGR → chiplet-0 GIC SPI 184/185 (M6). M8a extends same chardev wire to carry `MAILBOX_BASE` payload writes → `r100_mailbox_set_issr()` | Same |
| `r100-mailbox` (Samsung `ipm` SFR, PCIe instance) | N/A | Full `INTGR/INTCR/INTMR/INTSR/INTMSR/ISSR0..63` register model at `R100_PCIE_MAILBOX_BASE`; two masked-status IRQ outputs wired to chiplet-0 GIC SPI 184 (Group0) / SPI 185 (Group1); M6 doorbell asserts INTGR via `r100_mailbox_raise_intgr()`; M8a exposes `r100_mailbox_set_issr()` for host-direction writes and emits every MMIO ISSR write on the `issr` chardev for NPU→host shadow | Same | Full set of mailbox instances modelled |
| `r100-imsix` (DW PCIe iMSIX-DB trap) | N/A | 4 KB MMIO window at `R100_PCIE_IMSIX_BASE` (`0x1B_FFFF_F000`); 4-byte stores to offset `0xFFC` (`REBELH_PCIE_MSIX_ADDR` low 12 bits) emit 8-byte `(offset, db_data)` frames on the `msix` chardev → host-side `r100-npu-pci` → `msix_notify(db_data & 0x7FF)` (M7) | Same | Full silicon-accurate DW PCIe MAC with per-PF/VF MSI-X TLP generation |
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
