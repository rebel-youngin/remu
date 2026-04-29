# RBDMA + SMMU — gap analysis

Cross-reference of the silicon design (Notion) against the current
REMU device models. Drives the SMMU v2 milestone (`docs/roadmap.md`)
and informs the prioritisation of RBDMA follow-on work.

Sources:
- Notion **REBELQ SMMU Design** (`a27418f9fef34eca8ed4c2dd27f55d26`)
- Notion **RBDMA v1 User Guide** (`2e60ab893cfc4d2f9769c94a295b1e5f`,
  R100 = REBEL_H = RBDMA v1)
- `src/machine/r100_smmu.c` + `r100_smmu.h`
- `src/machine/r100_rbdma.c` + `r100_rbdma.h`

---

## Notion docs — what the silicon does

### REBELQ SMMU Design

SMMU-600 with 18 SIDs split by master + function:

| SID   | Master                                  | S1                             | S2                                |
|-------|-----------------------------------------|--------------------------------|-----------------------------------|
| 0–4   | DNC(dma) / RBDMA / HDMA-IPA, PF + VF0–3 | VA → IPA via CD per SSID       | IPA → PA, page table by CP        |
| 8–12  | DNC(compute), PF + VF0–3                | strip htid (PA → PA)           | bypass                            |
| 16    | Device HDMA (PA mode)                   | bypass                         | bypass                            |
| 17    | Host inbound (PCIe TBU, all functions)  | bypass                         | PCIe addr → PA, table built in BL2 early-init |

Key invariants:

- SID 17 page table must be ready **before CRS release (~1 s)**, so BL2
  builds it (16 KB initial table). Host distinguishes PF / VFn purely
  by PCIe address (iATU mapping). FreeRTOS `smmu_init` then builds
  SID 0–16 (HDMA / DMA / RBDMA path).
- Stream table = 64 B × 32 = 2 KB; CD = 64 B; STE.S1CDMAX = 0.
- BL2 only programs `CR0.SMMUEN`, `STRTAB_BASE`,
  `STRTAB_BASE_CFG.{FMT=00, LOG2SIZE=5}`.
- Limitations: OAS = 40-bit; `{THID(4), IPA(40)}` exceeds OAS so
  CP-injected uTLB entries don't fit on R100 (fixed in REBEL_IO);
  DNCTLB write goes via TBU, read via RDSN mesh; SHM must never miss
  the DNCTLB.

### RBDMA v1 User Guide (REBEL_H = R100)

- cDMA + eDMA, **8 TE**, 4 TSTC, 4 PPID, **64 TQ / 128 ExtQ /
  128 PTQ**, 64 (or 32 in Quad-Process) sync groups.
- **11 task types**: OTO=0, CST=1, GTH=2, SCT=3, OTM=4, GTHR=6, SCTR=7,
  PTL=8, IVL=9, VCM=10, DUM=11, DAS=12 (+ SHR/SHL added in V1.1).
- **PTD push**: write `v2p / msb / info`, then `num` to push N PTs into
  the PT queue (granule 0..15 ⇒ 2 MB..64 GB).
- **Task descriptor**: ascending writes to 0x200..0x21C,
  **`td_run_conf1@0x21C` is the kickoff**. With
  `pkg_mode.td_expand=1` the descriptor grows to 36 B and the trigger
  moves to **`td_run_conf_ext@0x280`**.
- **Auto fetch**: 4 proc units at 0x7000 / 7010 / 7020 / 7030
  (`proc_ptr` + `proc_ptr_bytesize` is the trigger).
- **Sync** types: TSYNC (DNC↔RBDMA), LSYNC (intra-RBDMA),
  RLSYNC (cross-chiplet RBDMA, uses `ip_info2.chiplet_id`),
  MPSYNC (multi-process).
- Multi-chiplet: per-chiplet `ip_info2[7:0]=chiplet_id`, per-chiplet
  `tsync_dnc_cfg_baseaddr`.
- Errors: `te_par`, `dpdc`, `wrongcmd`, `pterror`, `overflow_pt/tq`,
  `bus_read/write_err`, `fnsh_fifo_fullwr`, `cbus_*_err` — exposed
  via `global_err_intr_fifo[0x140..0x148]`.
- Kill / clear / process kill: multi-step
  `stop → kill_cmd → poll kill_status` flows on `0x1C0..0x1C8` and
  per-TE `0x80xx`.

---

## Current REMU implementation

### `r100_smmu.c` — `r100-smmu`, MMIO 64 KB @ TCU_OFFSET 0x1FF4200000, per chiplet

**v1 (current):** stage-2 walker, PF only (SID 0).

- BL2 boot: `CR0` bits mirror into `CR0ACK`; `GBPA.UPDATE`
  auto-clears; `IRQ_CTRLACK` mirrors `IRQ_CTRL`. Just enough to
  clear the BL2 spin-loops.
- `STRTAB_BASE` / `STRTAB_BASE_CFG` cache the in-DRAM stream-table
  geometry (LINEAR only; q-sys uses LINEAR for the ≤32-SID R100).
- CMDQ recognises and logs `CMD_SYNC` (zeroes msiaddr per
  `CS=SIG_IRQ`) + the full `CMD_TLBI_*` / `CMD_CFGI_*` /
  `CMD_PREFETCH_*` opcode set (advance CONS as no-ops — v1 has no
  STE / IOTLB cache to invalidate; every translate re-reads the STE).
- Public `r100_smmu_translate(s, sid, ssid, dva, access, *out)`:
  pre-`CR0.SMMUEN` → identity; post-enable → read STE for `sid`,
  decode `STE0.{V, config}` (`BYPASS`→identity, `ABORT`→`INV_STE`
  fault, `S1_TRANS`→v1 identity+LOG_UNIMP, `S2_TRANS` /
  `ALL_TRANS`→build `SMMUTransCfg` from STE2/STE3 and dispatch to
  QEMU's existing `smmu_ptw_64_s2`).
- Wired into `r100-rbdma` (OTO src+dst before chiplet base) and
  `r100-hdma` (LL walker LLP cursor + per-LLI SAR/DAR for D2D,
  NPU→host OP_WRITE chunks, host→NPU OP_READ_REQ chunks) via QOM
  `link<r100-smmu>` properties.

### `r100_rbdma.c` — `r100-rbdma`, MMIO 1 MB @ NBUS_L_RBDMA_CFG_BASE 0x1FF3700000, per chiplet

- Sparse `GHashTable` regstore + synthetic `IP_INFO0..5` defaults:
  `num_of_executer = 8`, all queue-depth fields = **32** (TQ / UTQ /
  PTQ / TEQ / FNSH), `num_of_err_fifo = 8`.
- Live read taps: `INTR_FIFO_NUM` (live FIFO depth),
  `FNSH_INTR_FIFO` (pop), `*TQUEUE_STATUS` (return seeded depth as
  "all free").
- Kick on `td_run_conf1@0x21C` write (size = 4): re-reads
  `td_ptid_init@0x200`, decodes `td_run_conf0.task_type`. **OTO=0**
  is the only behavioural path — pulls `srcaddr / destaddr /
  sizeof128blk / run_conf0` from the regstore, reconstructs 41-bit
  byte addresses (`{msb[1:0], lo[31:0]} << 7`), calls
  `r100_smmu_translate(SID=0, …)` to honour stage-2 (identity when
  `CR0.SMMUEN=0`), adds `chiplet_id × R100_CHIPLET_OFFSET`, and runs
  `address_space_read` → buf → `address_space_write` against
  `&address_space_memory` (cap 32 MiB). Other task_types log
  `LOG_UNIMP` and still push the FNSH entry so q-cp's done loop
  unwinds.
- BH walks the FNSH ring after the push, pulses
  `INT_ID_RBDMA1 = 978` per entry (skips when `intr_disable = 1`).
  `INT_ID_RBDMA0_ERR = 977` is wired but never pulsed.
- Counters (`kicks`, `oto_bytes`, `unimp_task_kicks`, …) survive
  reset.

---

## SMMU v2 gaps

In rough priority order:

1. **Stage-1 walk via CD per SSID.** q-cp's `smmu_init_ste` sets
   `STE1.S1DSS=BYPASS` for SIDs 0..4 today; v2 honours CD per SSID.
   DNC compute uses SSID-keyed translation. Plug point: the
   `S1_TRANS` arm in `r100_smmu_translate` (currently identity +
   `LOG_UNIMP`). QEMU's `smmu_ptw_64_s1` is already linked via
   `CONFIG_ARM_SMMUV3=y` — REMU only needs CD decode + dispatch.
2. **Multi-VF (SIDs 1..4).** Each VF has its own page table; per-VF
   `vttb` resolved through STE per-SID. SID 0 hardcoded today on
   both engines — v2 either threads SID through engine state or
   exposes a SID resolver from the engine's QOM context. RBDMA's
   `ip_info2.chiplet_id` does not fold into SID; a separate
   chiplet/VF→SID mapping table is needed.
3. **STE / IOTLB cache + honest `CMD_TLBI_*` / `CMD_CFGI_*`
   invalidation.** v1 re-reads STE on every translate (correct but
   uncached). v2 needs:
   - per-SID STE cache invalidated by `CMD_CFGI_STE`,
   - per-SSID CD cache invalidated by `CMD_CFGI_CD`,
   - VA / IPA IOTLB invalidated by `CMD_TLBI_NH_*` / `CMD_TLBI_S2_IPA`,
   - whole-cache invalidate on `CMD_TLBI_NSNH_ALL`.
   Today's CMDQ already recognises and logs every opcode — adding
   the cache itself is the work.
4. ~~**Eventq / GERROR fault delivery.**~~ **Landed.** v1's
   `R100SMMUFault` enum is now mapped to FW event_id (per
   `q/sys/drivers/smmu/smmu.c:smmu_print_event` table) by
   `r100_smmu_fault_to_event_id`. `r100_smmu_emit_event` builds the
   32 B SMMUv3 record (event_id + sid + input_addr + ipa) into the
   in-DRAM ring at `EVENTQ_BASE + (PROD & MASK)*32`, advances PROD
   with the wrap bit, and pulses GIC SPI 762
   (`R100_INT_ID_SMMU_EVT`) when `IRQ_CTRL.EVENTQ_IRQEN=1`. Overflow
   (next-PROD == CONS) raises `GERROR.EVTQ_ABT_ERR` via
   `r100_smmu_raise_gerror`, which toggles the bit against
   `GERRORN` so `smmu_gerr_intr`'s `(GERROR ^ GERRORN) & ACTIVE_MASK`
   check fires, and pulses GIC SPI 765 (`R100_INT_ID_SMMU_GERR`).
   `EVENTQ_PROD/CONS` are wired into the regstore — note the
   registers live on **page 1** of the SMMU MMIO window
   (offset 0x100A8/AC), so `R100_SMMU_REG_SIZE` had to grow from
   64 KB → 128 KB. SMMU sysbus device now has 2 IRQ outputs
   (`evt_irq` index 0, `gerr_irq` index 1) wired in
   `r100_create_smmu`. End-to-end coverage:
   `tests/p11b_smmu_evtq_test.py` deliberately faults a SID via
   STE0.V=0 + RBDMA OTO kick, and asserts both PROD=1 and the
   slot-0 event payload (`event_id=C_BAD_STE`, `sid=0`,
   `input_addr=IPA_SRC`).
5. **2LVL stream-table format.** v1 supports LINEAR only. q-sys's
   ≤32-SID R100 uses LINEAR; 2LVL is purely for capacity headroom.
   `STRTAB_BASE_CFG.fmt` is already cached — `r100_smmu_read_ste`
   needs a 2LVL branch that walks the L1 → L2 indirection.
6. **Chiplet-0 PCIe-side TBU SID 17.** Host-inbound translation via
   the BL2-staged 16 KB initial table from
   `tf-a/.../smmu/smmu.c:smmu_early_init`. Distinct path from the
   NPU-side engine SIDs; it lives on the *host* side of the PCIe
   boundary. REMU's `r100-pcie-outbound` is the natural anchor — a
   second translate hook that runs the kmd-published bus address
   through SID 17's stage-2 before the `host-ram` alias materialises
   it. Today the alias bypasses the SMMU entirely (correct under
   the all-bypass regime kmd uses on bring-up).
7. **Dedicated HDMA-PA-mode SID 16.** Notion's regime where the
   bypass region `0x0..0x80_0000_0000` passes through unchanged.
   Today's `r100-hdma` model already assumes PA passthrough on the
   NPU-side; SID 16 wiring becomes meaningful when stage-1 lands and
   some channels need IPA translation while others need PA bypass.
8. **OAS / output-addr-size enforcement.** Doc constrains
   OAS = 40-bit. Benign while identity / stage-2 only is the regime.

---

## RBDMA gaps (rough priority)

1. **PTD path absent.** `td_ptl_v2p / msb / info / num` writes hit
   the regstore as anonymous words; the PT queue is invisible.
   **PTL=8 / IVL=9** task types fall through to `LOG_UNIMP`. Anything
   that exercises page-table life-cycle silently no-ops. Comes back
   into focus once SMMU v2 stage-1 lands and q-cp pushes per-task
   PTs.
2. **`td_expand` / `td_run_conf_ext@0x280`.** REMU only kicks on
   `0x21C`. With `pkg_mode.td_expand = 1` (SP Parallel mode + MP
   Sync) the silicon trigger moves to 0x280 — REMU would store the
   descriptor and silently never kick. Easy fix: also trip the
   kickoff handler when `pkg_mode[0] == 1 && addr == 0x280`.
3. **Sync (TSYNC / LSYNC / RLSYNC / MPSYNC) totally unmodelled.**
   Sync registers (`tsync_g000_dpdc_*` @ 0x2000+, `tsync_dnc_cfg_*`,
   `mp_hash_pos*`, sync masks in TD) all hit the regstore. The
   kickoff path doesn't gate on dependency state, so a task that
   should block on a DNC put-tsync runs *immediately*. Fine while
   OTO-only umd pulls everything sequentially; regresses the moment
   a mixed DNC / RBDMA workload arrives.
4. **Auto-fetch (`proc[0..3]_*` @ 0x7000 / 7010 / 7020 / 7030).** No
   reaction to `proc_ptr_bytesize` writes. The user guide flags this
   as the standard CP path (and the recommended double-buffered
   shape). q-cp's `simple_copy` doesn't use it today; if it ever
   does, the descriptor stream will be silently ignored.
5. **CST / DAS / GTH / SCT / GTHR / SCTR / OTM / VCM / DUM / PTL / IVL.**
   All `LOG_UNIMP` no-op; they complete the FNSH push so q-cp's done
   loop unblocks. CST is trivial (broadcast 4 B → 128 B); DUM is
   even cheaper (no data move). DAS is OTO with overlap-safe
   ordering. The gather / scatter family will need real work the
   moment compiler-generated workloads land.
6. **Process kill / task kill / clear** (`global_cdma_stop_resume_kill@0x1C0`,
   `kill_status@0x1C8`, `te_pause / te_clear@0x80x0`). Regstore-only
   — the doc's `polling until kill_status == 1` would spin forever.
   Off the umd happy path today, but any error-injection test hits
   this.
7. **Error injection.** `err_irq` wired but unused; no synth path
   into `global_err_intr_fifo[0x140..0x148]`. Trivial follow-on.
8. **VCM / multi-chiplet TSYNC routing.** `chiplet_id` is plumbed in
   `ip_info2`, but the `tsync_dnc_cfg_baseaddr`-relative emit path
   the doc describes (cross-chiplet TSYNC delivery) is unmodelled.
9. **Queue-depth mismatch.** REMU seeds **32-deep** TQ / UTQ / PTQ /
   TEQ / FNSH; doc says **64 / 64 / 128 / N / N**. Pragmatic since
   the credit-report path returns "all free" regardless of depth.
   A future credit-tracking implementation would otherwise inherit
   the wrong cap.
10. **Wrongcmd / RO / WO / undefined-addr enforcement absent.**
    `LOG_GUEST_ERROR` opportunity if/when q-cp is buggy. Low value.
11. **Log Manager (CDMA `0x500..0x538`) / Bus Profiling
    (`0x84F8..`)** registers are entirely passive. Defer until
    profiling becomes interesting.

---

## Cross-cutting note

`r100-rbdma`'s OTO mover, `r100-hdma`'s D2D / chunked walker, and any
future SID-17 host-inbound trap on `r100-pcie-outbound` all share the
same `r100_smmu_translate(s, sid, ssid, dva, access, *out)` entry.
v2 changes shape on the SMMU side; the engine call sites stay the
same. v1 plug points already in place are the natural insertion
points for the v2 walker extensions.
