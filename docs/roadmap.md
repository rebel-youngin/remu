# REMU Roadmap

> **Policy**: per-fix debugging history lives in `git log`, not here.
> Each completed milestone is a one-liner + commit SHA — read the
> commit message for rationale, alternatives rejected, and post-mortem
> detail. This file focuses on ongoing and future work.

## Phase 1: FW Boot — complete

TF-A BL1 → BL2 → BL31 → FreeRTOS on all 4 chiplets, both CP0 and CP1
clusters online everywhere; `silicon` profile only.

**Success markers** on `./remucli run`:

- every chiplet's `uart{0,1,2,3}.log` reaches `Hello world FreeRTOS_CP`
  / `hw_init: done` / `REBELLIONS$`
- all 16 CP1 vCPUs in q-cp FreeRTOS steady-state (inspect with
  `tests/scripts/gdb_inspect_cp1.gdb`, see `docs/debugging.md`)
- every chiplet's `bootdone_task` prints `BOOT_DONE - 0x253c1f`
- HILS ring tail (`hils.log`) drains chiplet-0 CP1 task init
- 32 GDB threads visible, `aarch64-none-elf-gdb` can step any of them

Phase-1 device models and infra are catalogued in
`docs/architecture.md` (Source File Map).

**Out of scope**: single-chiplet builds, HBM3 PHY-training fidelity,
cycle accuracy, `-p zebu*` regression (silicon only).

## Phase 2: Host Drivers — foundation milestones (complete)

**Goal**: kmd loads in an x86_64 guest, probes the emulated CR03 PCI
device, handshakes with FW. All BAR / chardev / mailbox plumbing is
real.

| # | Milestone | Commit |
|---|---|---|
| M1 | Two-binary build (`qemu-system-{aarch64,x86_64}` from one tree) | `7b03328` |
| M2 | Shared `/dev/shm/remu-<name>/remu-shm`, signal-driven teardown | `7b03328` |
| M3 | `r100-npu-pci` skeleton (vendor 0x1eff / dev 0x2030, four BARs, MSI-X table) | `7b03328` |
| M4 | BAR0 splices shared memdev at offset 0, lazy RAM on tail | `e03b00f` |
| M5 | NPU chiplet-0 DRAM splices the same memdev | `72c98f0` |
| M6 | BAR4 INTGR doorbell → `r100-cm7` → mailbox → GIC SPI | `85b76bb`, `500856b` |
| M7 | `r100-imsix` MMIO trap → `msix` chardev → host `msix_notify()` | `db3d1df` |
| M8a | ISSR bridge both directions on `issr` + `doorbell` chardevs | `cd24aa9` |
| M8b | `FW_BOOT_DONE` + QINIT handshake; x86 Linux guest with kmd | `1ef7208`, `985fd58` |
| M9-1 | Mailbox + DNC + HDMA infrastructure for q-cp dispatch | (M9-1b/1c) |

Each milestone boots end-to-end and `./remucli run --host` auto-verifies
its bridge via `info pci/mtree/qtree` — see `CLAUDE.md` for the exact
HMP strings.

## Phase 2: architectural plan (P1..P11)

The unified plan for turning the M1..M9-1 plumbing into a
silicon-accurate path where every BD lifecycle event happens on the
side of silicon that actually owns it.

### Design principles

1. **`r100-cm7` is a bridge, not a CPU emulation.** It owns the
   doorbell ingress wire (BAR4 frames → mailbox INTGR / ISSR → GIC),
   the BAR2 cfg-head shared alias, and a small handful of stubs for
   silicon behaviours REMU does not yet model. It does not walk BDs,
   parse CBs, dispatch packets, or fake completions on the honest
   path. The "CM7" in the name refers to PCIE_CM7's silicon role as
   the BAR/doorbell front door, not a claim that REMU runs Cortex-M7
   firmware.
2. **CB walking + dispatch lives in q-cp on CA73 CP0, unmodified.**
   `hq_task` reads the BD; `cb_parse_*` walks the packet stream at
   `bd->addr`; q-cp's HAL drivers program engine MMIO; q-cp's
   `task_manager.c` builds the `dnc_one_task` and calls
   `mtq_push_task` for CP1.
3. **BD.DONE + MSI-X come from q-cp's `cb_complete`** on CA73 CP0.cpu0
   via `pcie_msix_trigger` → `r100-imsix` MMIO → host MSI-X.
4. **One QEMU device per silicon engine** (DNC, RBDMA, HDMA, sync).
   Per-engine fidelity may be staged (functional → behavioural), but
   the kick / done interface is silicon-accurate so q-cp's existing
   drivers drive them unmodified.
5. **All 4 DNC task-queue mailboxes instantiated** — q-cp's
   `_inst[HW_SPEC_DNC_QUEUE_NUM=4]` table maps cmd_types to mailbox
   indices: COMPUTE / UDMA / UDMA_LP / UDMA_ST. (DDMA / DDMA_HIGHP
   flow through the auto-fetch DDMA_AF path, not these polling
   mailboxes.)
6. **No new fw-patches.** Unmodelled hardware lives as a QEMU-side
   stub under `src/machine/` or `src/host/`, never as a firmware
   diff. `cli/fw-patches/` stays empty in regression.

### Done milestones

| # | Milestone | Commit |
|---|---|---|
| P1 | Honest BD lifecycle on q-cp/CP0 — P1a chiplet-0 PCIe outbound iATU stub at `R100_PCIE_AXI_SLV_BASE_ADDR = 0x8000000000` (4 GB); P1b BAR2 cfg-head ↔ NPU MMIO bidirectional alias at `R100_DEVICE_COMM_SPACE_BASE = 0x10200000`; P1c retired the QEMU-side scaffolding (Stage 3c BD-done FSM, Stage 3b QINIT, M9-1b mbtq push). q-cp now owns `hq_task → cb_task → cb_complete` end-to-end. | `198d8a2` |
| P2 | Single source of truth for MSI-X — `cb_complete → pcie_msix_trigger` is the only path; cm7-side `r100_imsix_notify` deleted. | `b54e7ba` |
| P3 | All 4 DNC task-queue mailboxes (COMPUTE / UDMA / UDMA_LP / UDMA_ST) instantiated as real Samsung-IPM SFRs on chiplet 0. | `7ac2489` |
| P4A | `r100-rbdma` device — sparse regstore, IP_INFO seeds (8 TEs, 32-deep TQ/UTQ/PTQ/TEQ/FNSH), kick on `RUN_CONF1` → BH → `INT_ID_RBDMA1 = 978` GIC pulse + FNSH FIFO pop. | `71e00c8` |
| P4B | `r100-rbdma` OTO behavioural byte-mover — `task_type=OTO=0` reconstructs 41-bit byte SAR/DAR, runs `address_space_read → buf → write` against `&address_space_memory` (cap 32 MiB). | `cb48f6f` |
| P5 | `r100-hdma` linked-list mode — full `dw_hdma_v0_lli` / `dw_hdma_v0_llp` chain walk on `CTRL1.LLEN=1` doorbell; per-LLI routing by SAR/DAR ≷ `REMU_HOST_PHYS_BASE` (D2D in-process / NPU→host OP_WRITE / host→NPU OP_READ_REQ + parked `qemu_cond_wait_bql()`). | `ecfcc6f` |
| P7 | Retired the gated cm7 stubs — Stage 3c BD-done FSM, Stage 3b QINIT, M9-1b mbtq push, the three bool properties, the `imsix` / `mbtq-mailbox` QOM links, `r100_hdma_set_cm7_callback` plumbing, `cm7-debug` chardev, the `R100_CMD_DESCR_SYNTH_*` synth ring, and the related constants in `remu_addrmap.h` / `remu_doorbell_proto.h`. ~700 LOC of scaffolding gone; `r100-cm7` reduced to BAR4 doorbell forward + P1b cfg-mirror alias. | `fa9eb20` |
| P10-fix | Replaced chardev RPC for both BAR2 cfg-head and chiplet-0 PCIe outbound with `memory-backend-file` aliases — the kmd's busy-poll could not be served by the chardev iothread under BQL contention. Both sides `mmap` the same `cfg-shadow` and `host-ram` shm files; `OP_CFG_WRITE` opcode 4 retired, `0xC0..0xFF` `req_id` partition reserved. | `d986302` |
| P11 | SMMU stage-2 walker (PF only, SID 0). `r100_smmu.c` decodes STE / dispatches `smmu_ptw_64_s2` against in-DRAM page tables FW publishes. `r100-rbdma` OTO and `r100-hdma` LL walker translate via `r100_smmu_translate(SID=0, …)` before `address_space_*`; identity fallback when `CR0.SMMUEN=0`. CMDQ recognises `CFGI_*` / `TLBI_*` / `PREFETCH_*` opcodes (no-op in v1: no cache to invalidate). | `1f04ff2` |
| SMMU v2 — eventq | Eventq + GERROR fault delivery. `r100_smmu_emit_event` builds 32 B SMMUv3 records on every translate fault (mapped from `R100SMMUFault` to FW event_id per `q/sys/drivers/smmu/smmu.c:165`), writes to in-DRAM eventq at `EVENTQ_BASE_PA + idx*32`, advances PROD with wrap encoding, and pulses GIC SPI 762 (`R100_INT_ID_SMMU_EVT`) when `IRQ_CTRL.EVENTQ_IRQEN=1`. Eventq overflow → `r100_smmu_raise_gerror(EVTQ_ABT_ERR)` toggles `GERROR ^ GERRORN` to make FW's `smmu_gerr_intr` see an active error and pulses SPI 765 (`R100_INT_ID_SMMU_GERR`). Both wired in `r100_soc.c:r100_create_smmu`. MMIO region bumped from 64 KB → 128 KB so `EVENTQ_PROD/CONS` at offset 0x100A8/AC (page 1) hit the device. New `tests/p11b_smmu_evtq_test.py`: V=0 STE + RBDMA OTO kick → expects PROD=1 + slot-0 content (event_id=0x04 C_BAD_STE, sid=0, input_addr=IPA_SRC). | `1f04ff2`+ |
| SMMU debug surface | Optional `debug-chardev` property on `r100-smmu` (chiplet 0 only — single-frontend `CharBackend`); plumbed through `-machine r100-soc,smmu-debug=<id>`. CLI `--host` mode auto-creates `output/<name>/smmu.log` next to `rbdma.log` / `hdma.log`. One ASCII line per translate (`xlate_in` / `ste` / `ptw` / `xlate_out [ok | FAULT …]`), STE-relevant store (`CR0` / `IRQ_CTRL` / `STRTAB_BASE` / `EVENTQ_BASE` / `CMDQ_BASE`), CMDQ command walked, eventq emit/drop, and GERROR raise — bounded throughput (one line per significant event), no `-d`/`--trace` dependency. New survives-reset counters (`translates_total` / `translates_bypass` / `translates_ok` / `translates_fault` / `cmdq_processed`) sit alongside the existing `events_emitted` / `gerror_raised`. Companion offline tools: `tests/scripts/mem_dump.py` (generic shm-backed memory dumper across `remu-shm` / `host-ram` / `cfg-shadow`, hex / u32 / u64 / raw, PROT_READ — region-agnostic, useful for any debug session) and `tests/scripts/smmu_decode.py` (pure-Python STE / stage-2 PTE / 3-level walk decoder against a live shm). m5..p5 + p11 + p11b regression green. | (this commit) |

### Open / pending milestones

| # | Milestone | Status | Notes |
|---|---|---|---|
| P6 | `r100-dnc` behavioural — parse `cmd_descr_dnc`, run a host-CPU kernel against input tensors, write outputs, fire `itdone` BH. | pending | gated by umd workload that needs DNC kernel emulation |
| P8 | Real CA73 soft-reset — replace M8b 3a's synthetic `0xFB0D` from `INTGR0 bit 0` with a bracketed reset of CP0/CP1 cluster CPUs + their GIC redistributor state + the PCIe mailbox regs (preserve DRAM/SRAM/cfg-head/kmd-loaded firmware images), restart from BL1; q-sys `bootdone_task` re-emits `0xFB0D` through the existing `issr` chardev path naturally. Removes the last QEMU-side cold-boot lie. Candidate hooks: `DeviceReset` on an aggregated `r100-ca73-cluster` QOM wrapper, or `qemu_system_reset_request` with a fine-grained `ShutdownCause`. | pending | independent of P1..P7 |
| P9 | UMQ multi-queue — `NUMBER_OF_CMD_QUEUES > 1`. q-cp's `hq_task` already loops over `qid`; work is mostly host-side allocation + per-queue MSI-X vector wiring. The reserved `0x01..0x7F` `req_id` partition on the `hdma` wire is available for per-queue tagging. | pending | depends on P1..P7 (done) |
| P10 | umd smoke test in `guest/` — `command_submission -y 5` → 2 CBs submitted → 2 MSI-X completions → exit 0. **CM7 mailbox stub landed** (chiplet-0 `MAILBOX_CP0_M4` instantiated as a real `r100-mailbox` with `cm7-stub=true` + `cm7-smmu-base=R100_SMMU_TCU_BASE`). On q-sys's `notify_dram_init_done` raising `INTGR1` bit 11 with `ISSR[11] == 0xFB0D`, the stub pokes `CR0.SMMUEN=1` synchronously inside the MMIO write handler — silicon-equivalent to `pcie_cp2cm7_callback → dram_init_done_cb → m7_smmu_enable`. ISSR reads always return 0 to preserve the lazy-RAM-via-FREERTOS-VA "first read returns 0" shape every other CM7 poll loop in q-sys depends on (`rbln_cm7_get_values` / `rbln_pcie_get_ats_enabled` etc.) for early exit. With this, kmd sees `FW_BOOT_DONE`, the kmd's PCIe probe completes, and the umd test launches `command_submission`. The HDMA LL chain at IPA `0x42b86640` now walks through stage-2 → PA `0x02b86640` and the first valid LLI (`xsize=0x200000 sar=0x0b800000 dar=0x140000000`) is decoded — the original "LL chain reads all zeros" wedge is **gone**. Next-edge: the LLI's payload SAR (chiplet-local NPU PA) and DAR (host PCIe PA) currently route through the same PF SID, hit `STE.Config=ALL_TRANS`, and fault on stage-2. Fix shape: route HDMA SAR/DAR through stage-1 bypass via the FW's `STE[8]` (`smmu_init_ste_bypass(0)`), keep SID 0 for the IPA-bearing LL pointer. See `docs/debugging.md` → "P10 — HDMA payload SAR/DAR via SMMU stage-2 (post-CM7-stub)". | partially fixed | excluded from `./remucli test` defaults via `in_default=False` |

## SMMU v2 — next focus area

P11 landed v1: stage-2 only, PF only (SID 0), no STE/IOTLB cache,
LINEAR stream-table format. The deferred-v2 list, gated on workload
need:

- **Stage-1 walk** via CD per SSID — q-cp's `smmu_init_ste` sets
  `STE1.S1DSS=BYPASS` for SIDs 0..4 and stage-1 stays bypass under v1;
  v2 honours CD per SSID (DNC compute uses SSID-keyed translation).
- **Multi-VF (SIDs 1..4)** — each VF has its own page table;
  per-VF `vttb` resolved through STE per-SID.
- **STE / IOTLB cache** + honest `CMD_TLBI_NH_*` /
  `CMD_TLBI_S2_IPA` / `CMD_CFGI_STE` / `CMD_CFGI_CD` invalidation —
  v1 re-reads STE on every translate (correct but uncached); v2 needs
  a cache + invalidate path.
- ~~**Eventq / GERROR fault delivery**~~ — **landed (this commit)**:
  `INV_STE`, `STE_FETCH`, and stage-2 PT-walk faults now build a
  32 B SMMUv3 event record, write to the in-DRAM eventq at
  `EVENTQ_BASE + (PROD&MASK)*32`, and pulse GIC SPI 762 when
  `IRQ_CTRL.EVENTQ_IRQEN=1`. Eventq overflow toggles
  `GERROR.EVTQ_ABT_ERR` (vs `GERRORN`) and pulses SPI 765
  (`R100_INT_ID_SMMU_GERR`) for `smmu_gerr_intr`. Verified end-to-end
  by `tests/p11b_smmu_evtq_test.py`.
- **2LVL stream-table format** — v1 supports LINEAR only (q-sys's
  ≤32-SID R100 uses LINEAR); v2 adds the two-level layout.
- **Chiplet-0 PCIe-side TBU SID 17** — host-inbound translation via
  the BL2-staged 16 KB initial table from
  `tf-a/.../smmu/smmu.c:smmu_early_init`. Distinct path from the
  NPU-side engine SIDs; needs a second translate hook on
  `r100-pcie-outbound` (or its successor).
- **Dedicated HDMA-PA-mode SID 16** — Notion's `HDMA-PA` regime where
  the bypass region `0x0..0x80_0000_0000` passes through unchanged.
  Today's `r100-hdma` model already assumes PA passthrough on the
  NPU-side; SID 16 wiring becomes meaningful when stage-1 lands and
  some channels need IPA translation while others need PA bypass.

`docs/rbdma-smmu-review.md` carries the design-doc cross-reference for
each of these — read it before touching `r100_smmu.c`.

## Order of attack (current)

1. **P10** — gated on the kmd `rebel_soft_reset` / `rbln_register_p2pdma`
   path (independent of SMMU, see `docs/debugging.md`).
2. **SMMU v2** — most of the deferred list above; pick the v2 items
   the next workload actually exercises rather than implementing the
   whole list.
3. **P6** — DNC behavioural; gated by a workload that needs DNC kernel
   emulation.
4. **P8** — real CA73 soft-reset; orthogonal, tackle whenever the
   synthetic `FW_BOOT_DONE` blocks something concrete.
5. **P9** — UMQ multi-queue; falls out of the per-queue parser q-cp
   already implements.

## Components (current state)

Device models + chardev bridges — see `docs/architecture.md`
"Source File Map" for per-file behaviour, and `docs/debugging.md`
for the HMP sanity recipes.

- `src/host/r100_npu_pci.c` — x86-side endpoint. BAR0 splice over the
  shared `remu-shm` backend; BAR2 lazy RAM with a 4 KB cfg-head
  subregion at `FW_LOGBUF_SIZE` aliased over the shared `cfg-shadow`
  `memory-backend-file`; BAR4 container with 4 KB MMIO head (INTGR +
  MAILBOX_BASE shadow); BAR5 MSI-X + RAM fill. `hdma` chardev
  receiver: `OP_WRITE` → `pci_dma_write`, `OP_READ_REQ` →
  `pci_dma_read` + `OP_READ_RESP`.
- `src/machine/r100_soc.c` — machine. Splices the `remu-shm` backend
  into chiplet-0 DRAM; instantiates six `r100-mailbox` blocks (PF +
  VF0 for the host-facing PCIe Samsung-IPM SFRs; plus the four q-cp
  DNC task queues); optional `r100-cm7` / `r100-imsix` / `r100-hdma`
  / `r100-pcie-outbound` gated on chardev / shared-backend
  machine-props. Wires DNC done SPIs from each DCL (8 × 4 lines per
  DCL) to the chiplet GIC via `r100_dnc_intid()`. GIC `num-irq` is
  992 to fit DNC INTIDs.
- `src/machine/r100_mailbox.c` — Samsung IPM SFR
  (`INTGR/INTCR/INTMR/INTSR/INTMSR/ISSR0..63`), two `qemu_irq` outs
  tracking INTMSR. Public helpers in `r100_mailbox.h`:
  `r100_mailbox_{raise_intgr,set_issr,get_issr,cm7_stub_write_issr,
  set_issr_words,fw_boot_done_seen}`.
- `src/machine/r100_cm7.c` — reassembles 8-byte `(offset, value)`
  frames from BAR4 doorbell ingress; routes by offset into mailbox
  INTGR / ISSR. Two responsibilities: (1) `INTGR0 bit 0` SOFT_RESET
  → synthetic `FW_BOOT_DONE` (gated on the cold-boot real-publish
  latch — retires in **P8**); (2) the cfg-mirror MMIO alias at
  `R100_DEVICE_COMM_SPACE_BASE` over the shared `cfg-shadow`
  `memory-backend-file` (single source of truth for BAR2 cfg-head on
  both sides).
- `src/machine/r100_hdma.{c,h}` — DesignWare dw_hdma_v0 register-block
  model at `R100_HDMA_BASE` (chiplet 0). Per-channel state for 16 WR
  + 16 RD channels. **Non-LL doorbell**: WR emits OP_WRITE pulled
  from chiplet sysmem at SAR; RD emits OP_READ_REQ tagged
  `req_id = R100_HDMA_REQ_ID_CH_MASK_BASE | (dir<<5) | ch`. **LL
  doorbell** (`CTRL1.LLEN=1`): walks the chain at `LLP_LO|HI`,
  discriminating `dw_hdma_v0_lli` (24 B) from `dw_hdma_v0_llp` (16 B
  jump) on `control & LLP`; terminates on `LIE`. Per-LLI routing by
  SAR/DAR ≷ `REMU_HOST_PHYS_BASE`. Single GIC SPI 186 line +
  per-channel pending bit set in SUBCTRL_EDMA_INT_CA73 on chain end.
  Owns the `hdma` chardev. SMMU translate hook on every NPU-side
  cursor / SAR / DAR via QOM `link<r100-smmu>`.
- `src/machine/r100_pcie_outbound.c` — chiplet-0 PCIe outbound iATU
  stub. 4 GB MMIO at `R100_PCIE_AXI_SLV_BASE_ADDR = 0x8000000000`,
  realised as a `MemoryRegion` alias over the shared `host-ram`
  `memory-backend-file`. q-cp's outbound loads/stores are plain TCG
  accesses against the same pages the kmd allocates with
  `dma_alloc_coherent`.
- `src/machine/r100_rbdma.{c,h}` — RBDMA register block + OTO
  byte-mover. Per chiplet at `R100_NBUS_L_RBDMA_CFG_BASE`. Sparse
  `R100RBDMARegStore`; synthetic `IP_INFO0..5` seeds; FNSH FIFO ring;
  kick → BH → SPI 978 done IRQ; OTO `address_space_read → buf →
  write` against `&address_space_memory` (cap 32 MiB). SMMU translate
  hook on src/dst before chiplet-base add.
- `src/machine/r100_dnc.c` — DCL CFG-window stub. Sparse regstore
  seeds IP_INFO / SHM TPG / RDSN bits during Phase 1 boot. Active
  task-completion path: writes to slot+0x81C (TASK_DESC_CFG1) with
  `access_size=4` and `itdone=1` schedule a BH that synthesises a
  `dnc_reg_done_passage` at slot+0xA00 and pulses the matching DNC
  GIC SPI from `r100_dnc_intid()`.
- `src/machine/r100_imsix.c` — 4 KB MMIO trap at `R100_PCIE_IMSIX_BASE`,
  emits `(offset, db_data)` frames on write to `0xFFC`. Driven
  exclusively by q-cp's `pcie_msix_trigger` on CA73 CP0.
- `src/machine/r100_smmu.{c,h}` — SMMU-600 TCU. **MMIO**:
  `CR0→CR0ACK` + `GBPA.UPDATE` auto-clear; `STRTAB_BASE` /
  `STRTAB_BASE_CFG` cache the in-DRAM stream-table geometry; `CMDQ`
  walks `(old_cons, new_prod]` and recognises the full
  `CMD_TLBI_*` / `CMD_CFGI_*` / `CMD_PREFETCH_*` opcode set
  (no-ops in v1 — no STE / IOTLB cache to invalidate). **Public
  translate API** (`r100_smmu_translate`): NPU-side engines call
  this before `address_space_*`. Pre-`CR0.SMMUEN`: identity.
  Post-enable: read STE for `sid` from `STRTAB_BASE_PA + sid * 64`,
  decode `STE0.{V, config}`, dispatch to QEMU's `smmu_ptw_64_s2`
  for stage-2 translation. v1: stage-2 only, PF only (SID 0).
- Four Unix-socket chardevs under `output/<name>/host/`:
  `doorbell.sock` (host → NPU), `msix.sock` (NPU → host),
  `issr.sock` (NPU → host), `hdma.sock` (bidirectional NPU ↔ host
  DMA). Three shared-memory files under `/dev/shm/remu-<name>/`
  back the splices: `remu-shm` (36 GB by default — `R100_RBLN_DRAM_SIZE`,
  full chiplet-0 DRAM = real BAR0 size; sparse on tmpfs so untouched
  pages cost nothing; override with `--shm-size`),
  `host-ram` (sized by `--host-mem`, chiplet-0 PCIe outbound iATU
  window), `cfg-shadow` (4 KB BAR2 cfg-head).
- `src/bridge/remu_hdma_proto.h` — 24 B `RemuHdmaHeader` + payload.
  Live opcodes: `OP_WRITE`, `OP_READ_REQ`, `OP_READ_RESP`. `req_id`
  partitions: `0x00..0x7F` reserved (available for UMQ multi-queue
  per P9), `0x80..0xBF` r100-hdma channel ops, `0xC0..0xFF` reserved.

## Success criteria (path complete)

- `insmod rebellions.ko` succeeds, device probes, BAR sizes match (M3 ✓)
- MSI-X vectors allocated, cold-boot `FW_BOOT_DONE` handshake completes
  via real `bootdone_task` → `issr` chardev egress (M8b 3a ✓);
  soft-reset re-handshake stays synthesised by `r100-cm7` until **P8**
- `rebel_hw_init` / `rebel_queue_init` pass (M8b 3b ✓)
- `rbln_queue_test` completes via q-cp's `cb_complete` (P1, P2 ✓)
- BD's actual packet stream is decoded and dispatched per cmd_type
  to the right engine device (P1 + P3 + P4A + P5 ✓)
- umd opens device, creates context, submits simple job (P10)
- Real tensor data flows for at least one cmd_type (P4B ✓; P6 still
  gates on a workload that needs DNC kernel emulation)

## Long-term fidelity follow-ons

Not blocking the P-plan above:

- **HDMA scatter-gather** — full multi-LL DMA with address translation;
  extension of P5's behavioural step.
- **Performance counters** — synthetic cycle counts for FW timing
  paths; meaningless today.

## Timing considerations

REMU is purely functional. Consequences:

- PLL / DMA / HBM training complete instantly
- FW time-out paths may never trigger
- Hardware-timing race conditions may not reproduce
- Performance counter values are meaningless
- FreeRTOS tick fires, but wall-clock ratios differ

Annotated delays can be added per-device if a future workload needs them.
