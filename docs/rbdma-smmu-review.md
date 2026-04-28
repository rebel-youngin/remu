# RBDMA + SMMU design review (vs. Notion)

Date: 2026-04-28
Scope: review of the current `r100-smmu` (`src/machine/r100_smmu.c`,
~306 LOC) and `r100-rbdma` (`src/machine/r100_rbdma.c`, ~743 LOC)
device models against the silicon design captured in Notion.

Sources read:
- Notion **REBELQ SMMU Design** (`a27418f9fef34eca8ed4c2dd27f55d26`)
- Notion **RBDMA v1 User Guide** (`2e60ab893cfc4d2f9769c94a295b1e5f`,
  R100 = REBEL_H = RBDMA v1)
- `src/machine/r100_smmu.c` (full file)
- `src/machine/r100_rbdma.c` + `r100_rbdma.h` (full file)

---

## Notion docs — what the silicon actually does

### REBELQ SMMU Design

SMMU-600 with 18 SIDs split by master + function:

| SID   | Master                                  | S1                             | S2                                |
|-------|-----------------------------------------|--------------------------------|-----------------------------------|
| 0–4   | DNC(dma) / RBDMA / HDMA-IPA, PF + VF0–3 | VA → IPA via CD per SSID       | IPA → PA, page table by CP        |
| 8–12  | DNC(compute), PF + VF0–3                | strip htid (PA → PA)           | bypass                            |
| 16    | Device HDMA (PA mode)                   | bypass                         | bypass                            |
| 17    | Host inbound (PCIe TBU, all functions)  | bypass                         | PCIe addr → PA, table built in BL2 early-init |

Key invariants from the doc:

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

- Pure register-file write-back. **No STE / CD / page-table walk
  anywhere.**
- BL2 boot: `CR0` bits mirror into `CR0ACK`; `GBPA.UPDATE`
  auto-clears; `IRQ_CTRLACK` mirrors `IRQ_CTRL`. Just enough to
  clear the BL2 spin-loops.
- FreeRTOS path: only `CMDQ_BASE / PROD / CONS` + `CMD_SYNC` are
  interpreted — on PROD write, walks `(old_cons, new_prod]`, for any
  entry with opcode `0x46` / CS=SIG_IRQ writes `0` to
  `cmd[1].msiaddr` and advances CONS=PROD. Non-SYNC commands
  (CFGI_*, TLBI_*, PREFETCH_*, ATC_INV, RESUME) are silently dropped.
- Effective transform: `S1 ∘ S2 = identity`. Engines (`r100-rbdma`,
  `r100-hdma`, `r100-dnc-cluster`, `r100-pcie-outbound`) all assume
  this and bypass the SMMU.

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
  byte addresses (`{msb[1:0], lo[31:0]} << 7`), adds
  `chiplet_id × R100_CHIPLET_OFFSET`, and runs `address_space_read` →
  buf → `address_space_write` against `&address_space_memory`
  (cap 32 MiB). Other task_types log `LOG_UNIMP` and still push the
  FNSH entry so q-cp's done loop unwinds.
- BH walks the FNSH ring after the push, pulses
  `INT_ID_RBDMA1 = 978` per entry (skips when `intr_disable = 1`).
  `INT_ID_RBDMA0_ERR = 977` is wired but never pulsed.
- Counters (`kicks`, `oto_bytes`, `unimp_task_kicks`, …) survive
  reset.

---

## Review — gaps vs. silicon

### SMMU — register-only stub, identity translation

Documented honestly in code comments and matches the agreed scope, but
the gap is wide:

1. **No STE / CD / page-table walk.** None of the SID-based behaviour
   from the design doc (S1+S2 for SID 0–4, htid-strip for SID 8–12,
   S2-only for SID 17) exists. Deliberate pragmatic choice —
   `r100-rbdma`, `r100-hdma`, `r100-pcie-outbound` and
   `r100-dnc-cluster` all bypass and rely on flat AXI / chiplet-base
   plumbing.
2. **`STRTAB_BASE` / `STRTAB_BASE_CFG` are write-only no-ops.** BL2's
   stream-table init is silently dropped; nothing reads it back.
3. **Event queue / GERROR / PRIQ paths absent.** `EVENTQEN` / `PRIQEN`
   bits ack via `CR0ACK`, but no eventq pointer plumbing or MSI
   generation. Fault-injection paths (`pterror` etc.) don't work.
4. **CMDQ command set is single-opcode.** Only `CMD_SYNC` is honoured;
   `CFGI_STE / CD`, `TLBI_*`, `ATC_INV`, `PREFETCH_*` are dropped.
   Today this is benign (no STE / CD state to invalidate), but a
   future translation hook will need to make these coherent with
   whatever cache it holds.
5. **OAS / output-addr-size config is ignored.** Doc constrains
   OAS = 40-bit; REMU has no enforcement. Benign while identity is
   the regime.

The header docstring is candid that this is the bare minimum to clear
`smmu_early_init` and `smmu_sync` polls. Everything else in the design
doc (PCIE TBU, HDMA TBU, DNC/DDMA TBU, page-table BL2 staging,
SR-IOV reconfig) is structurally absent.

### RBDMA — partial reg-block + OTO byte mover

Conformant pieces (vs. the user guide):

- 8 TE, chiplet_id at `ip_info2[7:0]`, kick on `td_run_conf1@0x21C`,
  ascending descriptor write, `intr_disable` bit honoured, INT_ID
  977 / 978 wiring, OTO byte-mover with 128-B-grain address
  reconstruction. ✓

Gaps that matter, in rough priority for the umd workload roadmap:

1. **Address translation (SMMU-coupled).** SAR / DAR are documented
   as DVAs in the user guide; REMU treats them as raw chiplet-local
   PAs. Fine while SMMU is identity (umd's allocator hands out
   PA-equal-DVA today), but the moment SMMU honours FW page tables,
   every `r100_rbdma_do_oto` call needs an
   `r100_smmu_translate(SID, SSID, dva, …)` hook in front of
   `address_space_*`. Plug point already noted in the OTO comment
   block.
2. **PTD path absent.** `td_ptl_v2p / msb / info / num` writes hit the
   regstore as anonymous words; the PT queue is invisible. **PTL=8 /
   IVL=9** task types fall through to `LOG_UNIMP`. Today's regression
   tests don't push PTs (umd's `simple_copy` is OTO-only with
   identity SMMU), but anything that exercises page-table life-cycle
   will silently no-op.
3. **`td_expand` / `td_run_conf_ext@0x280`.** REMU only kicks on
   `0x21C`. With `pkg_mode.td_expand = 1` (SP Parallel mode + MP
   Sync) the silicon trigger moves to 0x280 — REMU would store the
   descriptor and silently never kick. Easy fix: also trip the
   kickoff handler when `pkg_mode[0] == 1 && addr == 0x280`.
4. **Sync (TSYNC / LSYNC / RLSYNC / MPSYNC) totally unmodelled.**
   Sync registers (`tsync_g000_dpdc_*` @ 0x2000+, `tsync_dnc_cfg_*`,
   `mp_hash_pos*`, sync masks in TD) all hit the regstore. The
   kickoff path doesn't gate on dependency state, so a task that
   should block on a DNC put-tsync runs *immediately*. Fine while
   OTO-only umd pulls everything sequentially, but regresses the
   moment a mixed DNC / RBDMA workload arrives.
5. **Auto-fetch (`proc[0..3]_*` @ 0x7000 / 7010 / 7020 / 7030).** No
   reaction to `proc_ptr_bytesize` writes. The user guide flags this
   as the standard CP path (and the recommended double-buffered
   shape). q-cp's `simple_copy` doesn't use it today; if it ever
   does, the descriptor stream will be silently ignored.
6. **CST / DAS / GTH / SCT / GTHR / SCTR / OTM / VCM / DUM / PTL / IVL.**
   All `LOG_UNIMP` no-op; they complete the FNSH push so q-cp's done
   loop unblocks. CST is trivial (broadcast 4 B → 128 B); DUM is
   even cheaper (no data move). DAS is OTO with overlap-safe ordering
   — fine since `memcpy` is overlap-safe. The gather / scatter family
   will need real work the moment compiler-generated workloads land.
7. **Process kill / task kill / clear** (`global_cdma_stop_resume_kill@0x1C0`,
   `kill_status@0x1C8`, `te_pause / te_clear@0x80x0`). Regstore-only
   — the doc's `polling until kill_status == 1` would spin forever.
   Off the umd happy path today, but any error-injection test hits
   this.
8. **Error injection.** `err_irq` wired but unused; no synth path into
   `global_err_intr_fifo[0x140..0x148]`. Trivial follow-on.
9. **VCM / multi-chiplet TSYNC routing.** `chiplet_id` is plumbed in
   `ip_info2`, but the `tsync_dnc_cfg_baseaddr`-relative emit path
   the doc describes (cross-chiplet TSYNC delivery) is unmodelled.
10. **Queue-depth mismatch.** REMU seeds **32-deep** TQ / UTQ / PTQ /
    TEQ / FNSH; doc says **64 / 64 / 128 / N / N**. Pragmatic since
    the credit-report path returns "all free" regardless of depth,
    but worth a one-liner in the comment that 32 is "small-but-not-
    silicon-shape" rather than the real number — a future
    credit-tracking implementation would otherwise inherit the wrong
    cap.
11. **Wrongcmd / RO / WO / undefined-addr enforcement absent.**
    `LOG_GUEST_ERROR` opportunity if/when q-cp is buggy. Low value.
12. **Log Manager (CDMA `0x500..0x538`) / Bus Profiling (`0x84F8..`)
    registers are entirely passive.** q-cp profiling reads return
    synthesised IP_INFO seeds or zeros. Defer until profiling becomes
    interesting.

### Cross-cutting note

`r100-rbdma`'s OTO mover and `r100-hdma`'s D2D walker share the same
convention: chiplet base added on top of "DVA treated as PA". When
SMMU translation lands, a single
`r100_smmu_translate(asid, dva, &pa)` helper called from both engines
+ `r100-pcie-outbound` (and from any future SID-17 host-inbound trap)
would close the gap consistently. The REMU code already calls this
out in two places (`docs/roadmap.md → "SMMU honour FW page tables"`
and the OTO header comment) — the plumbing is just waiting for an
STE / CD / PT walker on the SMMU side.

---

## Summary

SMMU is a deliberate identity-translation register stub — fine for the
current boot / test surface but missing every functional path from the
design doc (STE / CD walk, eventq, GERROR, command-set beyond
CMD_SYNC). RBDMA conforms on the OTO byte-mover + IP_INFO + done IRQ;
the next concrete gaps in priority order are:

1. honor the SMMU translation when it lands,
2. `td_expand` triggering on `0x280`,
3. PTD push + PTL / IVL behaviour,
4. sync-group dependency gating,
5. auto-fetch (`proc0..3`) descriptor streams.
