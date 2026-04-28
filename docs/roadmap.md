# REMU Roadmap

> **Policy**: per-fix debugging history lives in `git log`, not here.
> Phase 1 was pruned in commit `b869d00`; Phase 2 follows the same
> pattern. Each status row below is a one-liner + commit SHA — read
> the commit message for rationale, alternatives rejected, and
> post-mortem detail.

## Phase 1: FW Boot — complete

**Goal**: TF-A BL1 → BL2 → BL31 → FreeRTOS on all 4 chiplets, both CP0
and CP1 clusters online everywhere. Platform defaults to `silicon`
(the `zebu*` profiles warn-and-build but are not part of regression).

**Success markers** (all on `./remucli run`):

- every chiplet's `uart{0,1,2,3}.log` reaches `Hello world FreeRTOS_CP`
  / `hw_init: done` / `REBELLIONS$`
- all 16 CP1 vCPUs in q-cp FreeRTOS steady-state (inspect with
  `tests/scripts/gdb_inspect_cp1.gdb`, see `docs/debugging.md`)
- every chiplet's `bootdone_task` prints `BOOT_DONE - 0x253c1f` (proves
  generic-timer → GIC PPI delivery; see `680f964`)
- HILS ring tail (`hils.log`) drains chiplet-0 CP1 task init
- 32 GDB threads visible, `aarch64-none-elf-gdb` can step any of them

Phase-1 device models and infra are catalogued in
`docs/architecture.md` (Source File Map).

**Out of scope**: single-chiplet builds, HBM3 PHY-training fidelity,
cycle accuracy, `-p zebu*` regression (silicon only).

## Phase 2: Host Drivers — foundation milestones

**Prerequisite**: Phase 1 complete (q-cp on CP1 services part of the
host-facing FW interface).

**Foundation goal** (this section): kmd loads in an x86_64 guest,
probes the emulated CR03 PCI device, handshakes with FW. Everything
needed to make the BAR / chardev / mailbox plumbing real. Below this
table is the architectural plan for turning that plumbing into an
honest end-to-end path (where every BD lifecycle event happens on the
side of silicon that actually owns it).

### Foundation milestones

| # | Milestone | Status | Commit |
|---|---|---|---|
| M1 | Two-binary build (`qemu-system-{aarch64,x86_64}` from one tree) | done | `7b03328` |
| M2 | Shared `/dev/shm/remu-<name>/remu-shm`, signal-driven teardown | done | `7b03328` |
| M3 | `r100-npu-pci` skeleton (vendor 0x1eff / dev 0x2030, four BARs, MSI-X table) | done | `7b03328` |
| M4 | BAR0 splices shared memdev at offset 0, lazy RAM on tail | done | `e03b00f` |
| M5 | NPU chiplet-0 DRAM splices the same memdev; `tests/m5_dataflow_test.py` | done | `72c98f0` |
| M6 | BAR4 INTGR → `doorbell` chardev → `r100-cm7` (formerly `r100-doorbell`) → `r100-mailbox` → chiplet-0 GIC INTID 184/185 (`qdev_get_gpio_in` index via `R100_INTID_TO_GIC_SPI_GPIO`) | done | `85b76bb`, `500856b` |
| M7 | `r100-imsix` MMIO trap at `0x1BFFFFF000 + 0xFFC` → `msix` chardev → `msix_notify()` | done | `db3d1df` |
| M8a | ISSR bridge both dirs (NPU→host on new `issr` chardev, host→NPU on same `doorbell` wire, offset-disambiguated) | done | `cd24aa9` |
| M8b | `FW_BOOT_DONE` + QINIT handshake on top of M8a | done (3a + 3b + 3c) | see sub-table |
| M9-1 | Mailbox + DNC + HDMA infrastructure for q-cp dispatch | done (1b + 1c) | see sub-table |

Each milestone boots end-to-end and the run-harness (`./remucli run --host`)
auto-verifies its bridge via `info pci/mtree/qtree` — see the checklist in
`CLAUDE.md` for the exact HMP strings.

#### M8b sub-milestones

| Stage | What | Status | Commit |
|---|---|---|---|
| 1 | q-sys FreeRTOS writes `0xFB0D` to PF.ISSR[4]; second `r100-mailbox` instance at `R100_PCIE_MAILBOX_PF_BASE`; `CM7_BOOT_DONE_STATE` bits in chiplet-0 PMU; generic-timer → GIC PPI wiring | done | `1ef7208`, `680f964` |
| 2a | `guest/` shared-folder payload (build-kmd.sh, build-guest-image.sh, setup.sh) | done | `1ef7208` |
| 2b | `--host` auto-wires `-kernel/-initrd/-fsdev virtio-9p`, `-cpu max` for BMI2 in kmd | done | `1ef7208` |
| 2c | GIC CPU-interface `bpr > 0` assertion fix (QEMU patch `0002-arm-gicv3-reset-cpuif-after-init.patch`) | done | `985fd58` |
| 3a | KMD soft-reset handshake: `r100-cm7` CM7-stub synthesises `FW_BOOT_DONE` → PF.ISSR[4] on `INTGR0 bit 0`, bypassing unmodelled PCIE_CM7 | done (regression scaffolding — retired in P8) | `a01d2b5` |
| 3a-fix | PCIe mailbox GIC wiring corrected (`qdev_get_gpio_in` takes a 0-based SPI index, not an INTID) — removes silent IRQ storm on CA73 CPU0 that froze `bootdone_task` during `--host` runs. Cold-boot `FW_BOOT_DONE` now travels the real path (q-sys `bootdone_task` → PF.ISSR[4] → `issr` chardev → host BAR4 shadow). Stage 3a's stub is retained but narrowed to the post-soft-reset re-handshake only (see P8 below); post-mortem in `docs/debugging.md` | done | — |
| 3b | `rebel_hw_init` QINIT handshake: new `cfg` chardev (host → NPU BAR2 cfg-head mirror, incl. `DDH_BASE_{LO,HI}`) + `hdma` chardev (NPU → host DMA writes, 24 B header + payload) — NPU-side CM7 stub handles `INTGR1 bit 7 = QUEUE_INIT` by emitting HDMA writes for `fw_version = "3.remu-stub"` and `init_done = 1` | done | — |
| 3c | `rbln_queue_test` BD-done completion: rename `r100-doorbell` → `r100-cm7`, extend HDMA protocol bidirectional (`OP_READ_REQ/RESP`, `OP_CFG_WRITE`, `flags` → `req_id` tag), add BD-done async state machine on NPU-side CM7 that reads `queue_desc`+`bd`+`pkt` via host DMA, writes `FUNC_SCRATCH` via cfg, sets `BD_FLAGS_DONE_MASK`+advances `ci`, then `r100_imsix_notify` vec=0 → MSI-X | done (regression scaffolding — retired in P7) | — |

#### HDMA opcodes (Stage 3c wire format)

`struct RemuHdmaHeader { op, req_id, dst, len }` — all 24 B, little-endian:

| Opcode | Direction | Use |
|---|---|---|
| `REMU_HDMA_OP_WRITE = 1` | NPU → host | write `len` bytes at guest DMA `dst` (QINIT, BD-done bd+ci) |
| `REMU_HDMA_OP_READ_REQ = 2` | NPU → host | request `len` bytes from guest DMA `dst`, tagged by `req_id` |
| `REMU_HDMA_OP_READ_RESP = 3` | host → NPU | response for `req_id`, payload is the bytes read |
| `REMU_HDMA_OP_CFG_WRITE = 4` | NPU → host | update host-side `cfg_mmio_regs[dst>>2]` (reverse of Stage-3b cfg forwarding) |

`req_id` partitioning (M9-1c, `src/include/r100/remu_addrmap.h`):

- `0x00`: untagged QINIT (`fw_version` + `init_done` from r100-cm7)
- `0x01..0x0F`: r100-cm7 BD-done (`qid + 1`)
- `0x80..0xBF`: r100-hdma channel ops (`0x80 | (dir<<5) | ch`)

#### M9-1 sub-milestones (mailbox / DNC / HDMA infrastructure)

The M9 mailbox is a CA73 **CP0 → CP1** task queue, both ends of
which live in q-cp firmware: q-cp on CP0 builds a `dnc_one_task`
inside `cb_parse_*` and calls `mtq_push_task`
(`q/cp/src/task_mgr/task_manager.c:515` + `cp1/mb_task_queue.c:66`)
to write into `PERI0_MAILBOX_M9_CPU1`; q-cp on CP1's
`taskmgr_fetch_dnc_task_master_cp1` polls `MBTQ_PI_IDX` and pops
via `mtq_pop_task` (`cp1/fetch_task.c:231`). PCIE_CM7 is not on
this path — its silicon role is PCIE link bring-up, BAR
programming, reset coordination, and doorbell forwarding. The
reason `r100-cm7` performs the mbtq push today is that Stage 3c
short-circuits q-cp's CP0 work before `cb_task` ever runs, so the
natural CP0-side push never fires; r100-cm7 fakes one to keep CP1's
fetch loop alive. Once Stage 3c retires (progressively across
P1 → P2 → P7) and Stage 3a retires (P8), the push moves back to
q-cp on CP0 and r100-cm7's stub deletes. M9-1b/1c
nevertheless land real infrastructure (the M9 mailbox device, the
DNC done-IRQ path, and the `r100-hdma` register block) that every
later milestone depends on.

| Stage | What | Status | Commit |
|---|---|---|---|
| 1b | Real `r100-mailbox` instance at `R100_PERI0_MAILBOX_M9_BASE` (chiplet 0; COMPUTE slot only) — replaces lazy-RAM placeholder. `r100-cm7` pushes a 24 B placeholder `dnc_one_task` (`{cmd_id=qid+1, 0, 0}`) + bumps `MBTQ_PI_IDX` on every `INTGR1` queue-doorbell. New `r100_mailbox_set_issr_words` helper bypasses the issr_store funnel (no chardev egress, no host-relay accounting). cm7-debug emits `mbtq qid=N slot=M pi=P status=ok` for observability. Stage 3c BD-done unaffected — `rbln_queue_test` still passes. Push reaches storage at the right slots; q-cp consumption verified separately as part of P1 | done | — |
| 1c | Active `r100-dnc-cluster` task-completion path (TASK_DESC_CFG1.itdone trigger → BH-scheduled GIC SPI on the matching DNC INTID, synthesised done_passage at TASK_DONE). New `r100-hdma` device modelling DesignWare dw_hdma_v0 at `R100_HDMA_BASE = 0x1D80380000` (chiplet 0); takes ownership of the `hdma` chardev (single-frontend), with `r100-cm7` reaching it through a QOM link + public emit API in `r100_hdma.h`. r100-cm7's mbtq push synthesises a minimal valid `cmd_descr` (cmd_type=COMPUTE, core_affinity=BIT(0)) into chiplet-0 private DRAM at `R100_CMD_DESCR_SYNTH_BASE = 0x20000000`, replacing the M9-1b NULL-deref placeholder. GIC `num-irq` bumped 256 → 992 to fit DNC INTIDs (up to 617). Phase 1 boot + m5/m6/m7/m8 + Stage 3c BD-done qid=0 all preserved. The COMPUTE-only synthesis is intentionally minimal — replaced by real BD parsing in P1 | done | — |

The previously planned **M9-2** (q-cp end-to-end on the COMPUTE
shortcut) and **M9-3** (umd test binary) are superseded by the
architectural plan below. M9-2's verification step lives on as P1;
M9-3's umd test becomes the smoke target after P1+P3+P4A+P5+P7 land (see P10).

### Components (current state)

Device models + chardev bridges — see `docs/architecture.md`
"Source File Map" for per-file behaviour, and `docs/debugging.md`
for the HMP sanity recipes.

- `src/host/r100_npu_pci.c` — x86-side endpoint. BAR0 splice, BAR2 lazy RAM
  with a 4 KB cfg-head trap at `FW_LOGBUF_SIZE` (M8b 3b, forwards writes on
  `cfg` chardev), BAR4 container with 4 KB MMIO head (INTGR + MAILBOX_BASE
  shadow), BAR5 MSI-X + RAM fill. `hdma` chardev receiver is bidirectional
  (M8b 3c): `OP_WRITE` → `pci_dma_write`, `OP_READ_REQ` → `pci_dma_read` +
  `OP_READ_RESP`, `OP_CFG_WRITE` → updates host-local `cfg_mmio_regs`.
- `src/machine/r100_soc.c` — machine. Splices memdev into chiplet-0 DRAM,
  instantiates six `r100-mailbox` blocks (PF + VF0 for the host-facing
  PCIe Samsung-IPM SFRs; plus the four q-cp/CP1 DNC task queues at
  `R100_PERI0_MAILBOX_M9_BASE` / `M10_BASE` / `R100_PERI1_MAILBOX_M9_BASE`
  / `M10_BASE` — COMPUTE / UDMA / UDMA_LP / UDMA_ST per
  `_inst[HW_SPEC_DNC_QUEUE_NUM=4]`, P3), optional
  `r100-cm7` / `r100-imsix` / `r100-hdma` gated on chardev machine-props.
  Wires `cfg` chardev onto `r100-cm7`; the `hdma` chardev goes to
  `r100-hdma` (M9-1c). Links `r100-cm7` to `r100-hdma` (chardev egress
  for the P1b reverse-cfg `OP_CFG_WRITE` upstream); the prior
  `r100-cm7` ↔ `r100-imsix` and `r100-cm7` ↔ M9 mailbox links were
  retired in P7 along with the BD-done FSM and mbtq-push stubs.
  Wires DNC done SPIs from each DCL (8 × 4 lines per DCL) to the
  chiplet GIC via `r100_dnc_intid()` (M9-1c). Skips lazy-RAM at the
  M9 slot to avoid overlap. GIC `num-irq` is 992 (was 256 pre-M9-1c)
  to fit DNC INTIDs.
- `src/machine/r100_mailbox.c` — Samsung IPM SFR (`INTGR/INTCR/INTMR/INTSR/INTMSR/ISSR0..63`),
  two `qemu_irq` outs tracking INTMSR. API: `r100_mailbox_raise_intgr`,
  `r100_mailbox_set_issr`, `r100_mailbox_get_issr`,
  `r100_mailbox_cm7_stub_write_issr`,
  `r100_mailbox_set_issr_words` (M9-1b in-process multi-slot write
  for q-cp task-queue pushes; bypasses the issr_store funnel).
- `src/machine/r100_cm7.c` — reassembles 8-byte `(offset, value)` frames,
  routes by offset into mailbox INTGR / ISSR / CM7-stub. Post-P7 the
  file hosts exactly two responsibilities, both honest infrastructure:
  (1) `INTGR0 bit 0` SOFT_RESET → synthetic `FW_BOOT_DONE` (Stage 3a) —
  *scaffolding, retired in P8 once a real CA73 cluster reset path
  exists*;
  (2) the P1b cfg-mirror MMIO trap at `R100_DEVICE_COMM_SPACE_BASE`
  — `cfg_shadow[1024]` is the single source of truth on the NPU side
  (host BAR2 writes ingress on `cfg` chardev → update shadow; NPU
  reads/writes hit the trap; NPU writes additionally egress as
  `OP_CFG_WRITE` over `r100-hdma` so the host's `cfg_mmio_regs[]`
  stays consistent for `rebel_cfg_read(FUNC_SCRATCH)`).
  The Stage 3b QINIT stub, Stage 3c BD-done FSM, M9-1b mbtq push,
  and Stage 3c `cmd_descr` synth ring at `R100_CMD_DESCR_SYNTH_BASE`
  were retired in P7. The hdma chardev lives on `r100-hdma` (M9-1c);
  cm7 reaches it via a QOM `hdma` link only to emit `OP_CFG_WRITE`
  upstream — there is no longer a cm7-side `OP_READ_RESP` consumer.
- `src/machine/r100_hdma.{c,h}` — DesignWare dw_hdma_v0 register-block
  model (M9-1c + P5) at `R100_HDMA_BASE` (chiplet 0). Per-channel state
  covers the full `struct hdma_ch_regs` surface from
  `q/cp/.../hdma_regs.h` (enable, doorbell, elem_pf, handshake, llp,
  cycle, xfer_size, sar, dar, watermark, ctrl1, func_num, qos, status,
  int_status, int_setup, int_clear, msi_{stop,watermark,abort},
  msi_msgd) for 16 WR + 16 RD channels. **Non-LL doorbell** (M9-1c
  hand-driven test path): on WR → emit OP_WRITE with payload pulled from
  chiplet sysmem at SAR; on RD → emit OP_READ_REQ tagged with
  `req_id = R100_HDMA_REQ_ID_CH_MASK_BASE | (dir << 5) | ch`, complete
  on matching OP_READ_RESP by `address_space_write` into chiplet sysmem
  at DAR. **LL doorbell** (P5; q-cp's `cmd_descr_hdma` /
  `hdma_ll_trigger` production path): when `CTRL1.LLEN=1` the kick
  walks the chain at `LLP_LO|HI`, discriminating `dw_hdma_v0_lli`
  (24 B) from `dw_hdma_v0_llp` (16 B jump) on `control & LLP`,
  terminating on `LIE`. Per-LLI routing by SAR/DAR ≷
  `REMU_HOST_PHYS_BASE`: WR && DAR≥host → chunked OP_WRITE; RD &&
  SAR≥host → chunked OP_READ_REQ + parked `qemu_cond_wait_bql()` on a
  per-channel `resp_cond` until OP_READ_RESP arrives, then
  `address_space_write` at DAR; both NPU → D2D in-process loop
  (`address_space_read` → `address_space_write`, 64 KB scratch chunks).
  Single GIC SPI 186 line + per-channel pending bit set in
  SUBCTRL_EDMA_INT_CA73 (`0x1FF8184368`, plain RAM in pcie-subctrl) on
  chain end. Owns the `hdma` chardev (single-frontend constraint); cm7
  reaches it via a QOM link + public emit API. SMMU bypass regime
  applies (HDMA-SID LUT all-bypass per `docs/hdma-notion-notes.md` § 4)
  — SAR/DAR consumed as device PAs; honouring real FW page tables
  remains as a long-term follow-on.
- `src/machine/r100_dnc.c` — DCL CFG-window stub. Passive sparse
  regstore seeds IP_INFO / SHM TPG / RDSN bits during Phase 1 boot.
  M9-1c added an active path: writes to slot+0x81C
  (TASK_DESC_CFG1) with `access_size=4` and `itdone=1` schedule a BH
  that synthesises a `dnc_reg_done_passage` at slot+0xA00 and pulses
  the matching DNC GIC SPI (lookup at `r100_dnc_intid()`). Passive
  reads still served by regstore fall-through. P6 may extend with a
  behavioural compute kernel.
- `src/machine/r100_imsix.c` — 4 KB MMIO trap at `R100_PCIE_IMSIX_BASE`,
  emits `(offset, db_data)` frames on write to `0xFFC`. Driven by
  q-cp's `cb_complete` on CA73 CP0, whose `pcie_msix_trigger` is
  already linked from upstream `q/sys/osl/FreeRTOS/Source/rbln/msix.c`
  (no FW-side glue required). P2 deleted the call from `r100-cm7`'s
  Stage 3c BD-done FSM; P7 deleted the rest of Stage 3c (BD-done FSM,
  QINIT, mbtq push) and along with it the `imsix` QOM link from
  `r100-cm7` itself. The public `r100_imsix_notify` API is retained
  as a no-current-caller helper that any future post-soft-reset stub
  (P8) could re-use.
- Six Unix-socket chardevs under `output/<name>/host/`: `doorbell.sock`
  (M6+M8a, host → NPU), `msix.sock` (M7, NPU → host), `issr.sock`
  (M8a, NPU → host), `cfg.sock` (M8b 3b, host → NPU BAR2 cfg-head),
  `hdma.sock` (M8b 3b+3c, bidirectional NPU ↔ host DMA).
- `src/bridge/remu_hdma_proto.h` — 24 B `RemuHdmaHeader` + payload
  wire format; bidirectional since Stage 3c with `OP_WRITE`,
  `OP_READ_REQ/RESP` (`req_id`-tagged), and `OP_CFG_WRITE`.
- `./remucli run --host` orchestrates both QEMUs + auto-verifies bridges;
  M8b Stage 2 adds optional `-kernel/-initrd/-fsdev virtio-9p` wiring when
  `images/x86_guest/{bzImage,initramfs.cpio.gz}` are staged.

## Architectural plan: honest end-to-end

The Phase 2 / Phase 3 boundary in earlier drafts of this roadmap is
not a useful seam — it punts the hard structural decisions ("does
PCIE_CM7 own BD.DONE? does q-cp? does QEMU?") into a vague "later".
Below is the unified plan for turning the M1..M9-1 plumbing into a
silicon-accurate path where every BD lifecycle event happens on the
side of silicon that actually owns it.

### Diagnosis

Today's `r100-cm7` carries one structural lie: it fakes the *entire*
host-submitted-BD lifecycle (BD read, CB walk, engine dispatch, DONE
writeback, MSI-X) inside a QEMU device, hiding work that on silicon
is split across PCIE_CM7 hardware and q-cp firmware. Earlier drafts
of this section called this "role conflation between PCIE_CM7 and
q-cp" — that framing was itself imprecise. PCIE_CM7's silicon role
is much narrower than "dispatch": it does PCIE link bring-up, BAR
programming, reset coordination, and doorbell forwarding. **Both
BD reading and CB packet dispatch are q-cp on CA73**, fully
implemented in `external/ssw-bundle/products/rebel/q/cp/src/`.

| Job | Real silicon | Today's REMU |
|---|---|---|
| BAR4 doorbell → IRQ to CA73 | PCIE_CM7 + chiplet mailbox HW | `r100-cm7` bridge + `r100-mailbox` (M6/M8a — honest) |
| Read BD (`header / addr / size / cb_jd_addr`) | q-cp `hq_task` on CA73 CP0.cpu0 (`q/cp/src/hq_mgr/host_queue_manager.c:300+`) | Stage 3c reads it inside `r100-cm7` and ignores most fields |
| Walk CB at `bd->addr`, dispatch packets | q-cp `cb_task` → `cb_parse_*` on CA73 CP0.cpu0 (`q/cp/src/cb_mgr/command_buffer_manager.c:1197/1378`) | Stage 3c skips the walk; synthesises `cmd_type = COMPUTE` regardless of CB content |
| Push DNC compute tasks to q-cp/CP1 | q-cp on CA73 CP0 (`task_mgr/task_manager.c:515 mtq_push_task`) | M9-1b/1c push from `r100-cm7` (placeholder; q-cp/CP0 never gets the chance) |
| Program engines (DNC / RBDMA / HDMA) | q-cp HAL drivers writing engine MMIO | Engine MMIO partly modelled (DNC done-IRQ ✓, HDMA reg block ✓, RBDMA done-IRQ ✓ P4A, RBDMA OTO byte-mover ✓ P4B); q-cp never programs them because Stage 3c short-circuits the chain |
| Mark `BD.header \|= DONE`, advance `cq.di`, fire MSI-X | q-cp `cb_complete` on CA73 CP0.cpu0 (`command_buffer_manager.c:882`) | Stage 3c does it from inside `r100-cm7` — the lie |

The reason q-cp's existing parser is dormant is **not** that it is
missing — `cb_parse_nop / write_data / linked_dma / lin_dma /
ctx_dma / flush / barrier / extern_sync / ib / cb_header / sub_stream`
all exist in `command_buffer_manager.c`. It is dormant because
Stage 3c grabs the BD first, marks DONE, and there is nothing left
for q-cp to act on. `rbln_queue_test` does not catch the lie because
the test only asserts that DONE arrives — it does not validate which
side wrote it, in which order, with which side-effects.

### Design principles

1. **`r100-cm7` is a bridge, not a CPU emulation.** It owns the
   doorbell ingress wire (BAR4 frames → mailbox INTGR / ISSR → GIC),
   the BAR2 cfg-head shadow, and a small handful of stubs for
   silicon behaviours REMU does not yet model (Stages 3a/3c — both
   slated for retirement). It does not walk BDs, parse CBs, dispatch
   packets, or fake completions on the honest path. The "CM7" in the
   name refers to PCIE_CM7's silicon role as the BAR/doorbell front
   door, not a claim that REMU runs Cortex-M7 firmware.
2. **CB walking + dispatch lives in q-cp on CA73 CP0, unmodified.**
   `hq_task` reads the BD; `cb_parse_*` walks the packet stream at
   `bd->addr`; q-cp's HAL drivers program engine MMIO; q-cp's
   `task_manager.c` builds the `dnc_one_task` and calls
   `mtq_push_task` for CP1. REMU's job is to make the *engine
   devices* respond to that programming — not to re-implement the
   parser on the QEMU side.
3. **BD.DONE + MSI-X come from q-cp's `cb_complete`**, like silicon.
   `cb_complete` runs on CA73 CP0.cpu0 (`cb_task` is pinned to
   `BIT(CPU_ID_0)` in `q/cp/src/cp_impl.c:84`). Its
   `pcie_msix_trigger` call resolves to a real MMIO write to
   `r100-imsix` via upstream `q/sys/osl/FreeRTOS/Source/rbln/msix.c`,
   which is already linked into CP0's freertos image — no FW-side
   glue is required for the BD path. (CP1's `rbln` library
   deliberately omits `msix.c` — upstream's `BUILD_TARGET=cp1` branch
   reflects the design choice that CP1 is not the host-facing
   cluster. The dormant `pcie_msix_trigger` reference in
   `services/terminal_service/console_vserial.c` is not pulled into
   the CP1 link by anything in `main_cp1.c`, so it never surfaces.)
4. **One QEMU device per silicon engine** (DNC, RBDMA, HDMA, sync).
   Per-engine fidelity may be staged (functional → behavioural), but
   the kick / done interface is silicon-accurate so q-cp's existing
   drivers drive them unmodified.
5. **The mbtq carries real work.** Today the COMPUTE-only template
   is synthesised by `r100-cm7` at queue-doorbell time (M9-1b/1c) as
   scaffolding around Stage 3c. The honest version moves the push
   back to q-cp on CP0, where `cb_parse_*` builds `cmd_descr` from
   actual CB content with the real `cmd_type`; the QEMU-side
   synthesis dies with Stage 3c.
6. **All 4 DNC task-queue mailboxes instantiated** — q-cp's
   `_inst[HW_SPEC_DNC_QUEUE_NUM=4]` table
   (`q/cp/src/task_mgr/cp1/mb_task_queue.c:44`) maps cmd_types to
   mailbox indices: `[0]=COMPUTE → PERI0_M9_CPU1`, `[1]=UDMA →
   PERI0_M10_CPU1`, `[2]=UDMA_LP → PERI1_M9_CPU1`, `[3]=UDMA_ST →
   PERI1_M10_CPU1`. (DDMA / DDMA_HIGHP exist as `common_cmd_type`
   enum values but flow through the auto-fetch DDMA_AF path, not
   these polling mailboxes.) We instantiate all four on chiplet 0.
7. **No new fw-patches.** Unmodelled hardware lives as a QEMU-side
   stub under `src/machine/` or `src/host/`, never as a firmware
   diff. `cli/fw-patches/` stays empty in regression. If a future
   milestone ever surfaces a missing FW-side symbol that no QEMU
   stub can provide (i.e. a function body the firmware itself needs
   at link time), the existing `CP_LIB_PATH_CP{0,1}` env-var seam in
   `q/sys/osl/FreeRTOS/Source/services/CMakeLists.txt` is a known
   escape hatch for injecting REMU-owned object files into a copy of
   `librebel-q-cp{0,1}.a`. We have not needed it.

### Milestones

Naming neutral — the M9-* labels are retired in favour of P*.

| # | Milestone | Status | Notes |
|---|---|---|---|
| P1 | Honest BD lifecycle on q-cp/CP0 — q-cp on CA73 CP0 already implements packet parsing in full (`cb_parse_nop/write_data/linked_dma/lin_dma/ctx_dma/flush/...` in `q/cp/src/cb_mgr/command_buffer_manager.c`). The blocker today is that `hq_task`'s BD-reading load (`bd = (rbln_bd *)(cq.base_addr + RBLN_BD_SIZE * ci); addr = bd->addr; ...` at `host_queue_manager.c:303-308`) dereferences a host-RAM bus address that q-cp/CP0 has no path to: real silicon translates the load into a PCIe outbound TLP via the chiplet-0 iATU; REMU does not model that translation, so the load reads garbage from chiplet-0 lazy RAM and the chain stalls (`hq_task` builds a `cb_tcb` with garbage `cb_addr`, `cb_task` walks bogus packets, `cb_complete` never fires). The REMU-side work is (a) introduce a `r100-pcie-outbound` stub modelling chiplet-0's outbound iATU window — any q-cp/CP0 load/store to that range gets serviced via `pci_dma_read/write` against the host's DMA address space (same backing path Stage 3c already uses through the `hdma` chardev's `OP_READ_REQ`/`OP_WRITE`); (b) ensure each cmd_type's HAL writes land on a device that responds: DNC done IRQ (M9-1c ✓), HDMA kick/done (P5 — q-cp drives the same `dw_hdma_v0` channel registers M9-1c added), RBDMA kick/done (P4A); (c) the mbtq push moves out of `r100-cm7`'s queue-doorbell stub into q-cp on CP0, where `cmd_descr` is built from real CB content with the real `cmd_type`. Stage 3c keeps running in parallel during the transition so `rbln_queue_test` does not regress — both Stage 3c and `cb_complete` will fire MSI-X for the same BD; kmd's IRQ handler is idempotent on the duplicate. Verification: q-cp's `FLOG_DBG(func_id, "send MSIx interrupt to host\r\n")` from `cb_complete` (`command_buffer_manager.c:953`) appears in CP0 firmware logs alongside Stage 3c's `imsix_notify` trace, on the same BD. **P1a (done):** `r100-pcie-outbound` (chiplet 0 only, PF window) at `R100_PCIE_AXI_SLV_BASE_ADDR = 0x8000000000` (4 GB), bidirectional via the existing `hdma` chardev — reads block on `qemu_cond_wait_bql()` for an `OP_READ_RESP`; writes are fire-and-forget `OP_WRITE`. Uses the new `0xC0..0xFF` `req_id` partition (cookie rotates per request, single in flight); `r100-hdma` demuxes to a per-device callback (`r100_hdma_set_outbound_callback`). Host-side `pci_dma_read` failures synthesise a zero-payload `OP_READ_RESP` so the parked vCPU surfaces a guest-visible 0 instead of deadlocking. **P1b (done):** the BAR2 cfg-head ↔ DEVICE_COMMUNICATION_SPACE inbound iATU is modelled as a 4 KB MMIO trap inside `r100-cm7` overlaid on chiplet-0 DRAM at `R100_DEVICE_COMM_SPACE_BASE = 0x10200000` (`r100_cm7_cfg_mirror_{read,write}`, priority 10). Reads return the `cfg_shadow[]` slot — host BAR2 writes still ingress on the `cfg` chardev and update the same shadow, so q-cp's `hil_init_descs` (`hil_reg_base[PCIE_PF] = DEVICE_COMMUNICATION_SPACE_BASE`) reads kmd-published DDH out of the trap. NPU-side writes (q-cp's `cb_complete → writel(FUNC_SCRATCH, magic)`) update `cfg_shadow` and forward an `OP_CFG_WRITE` upstream so the host's `cfg_mmio_regs[]` stays in sync — this is the path that makes `rbln_queue_test`'s `rebel_cfg_read(FUNC_SCRATCH)` see the magic the firmware wrote. The trap accepts 1/2/4/8-byte accesses (with a 4-byte impl) so q-sys/CP0's cold-boot `memset(DEVICE_COMMUNICATION_SPACE_BASE, 0, CP1_LOGBUF_MAGIC)` flows through unmodified. **P1c (done):** with P1a + P1b in place, `r100-cm7`'s Stage 3c BD-done FSM and the M9-1b mbtq push are no longer needed — both stub points are gated by per-instance `bd-done-stub` / `mbtq-stub` / `qinit-stub` boolean properties (default off). q-cp on CP0 now owns the entire BD lifecycle end-to-end: hq_task reads the BD via `r100-pcie-outbound`, cb_task walks the CB packet stream, cb_parse_write_data writes `0xcafedead` to FUNC_SCRATCH which the trap forwards to the host's BAR2 shadow, and cb_complete sets BD.DONE + advances ci + calls `pcie_msix_trigger`. Verified on a `--host` smoke run: kmd serial log shows `rbln_async_probe_worker: fw version: 3.2.0~dev~7~g47f862cb` (real fw_version DMA-written by q-sys, not the old `3.remu-stub`); `rbln0 probed, id=0x0 (0:0)` (no `rbln_queue_test` errors); host monitor `xp/1wx 0xf000200ffc` returns `0xcafedead`; `r100-imsix` egresses one frame on the `msix` chardev (vector=0). m5/m6/m7/m8 regression still passes. The runtime kill-switches (`-global r100-cm7.bd-done-stub=on` / `mbtq-stub=on` / `qinit-stub=on`) keep the Stage 3c paths compiled in for bisecting any future q-cp regression. | done (a + b + c) | P1a infrastructure: `src/machine/r100_pcie_outbound.c`, `0xC0..0xFF` hdma partition, `r100_hdma_set_outbound_callback`. P1b: 4 KB MMIO trap in `r100_cm7.c` (`r100_cm7_cfg_mirror_ops`) replacing the earlier one-shot `address_space_write` mirror. P1c: stub kill-switches default false; full BD lifecycle owned by q-cp/CP0. |
| P2 | Single source of truth for MSI-X — deleted `r100_imsix_notify(s->imsix, j->qid)` from `r100-cm7`'s Stage 3c BD-done state machine. With P1c the call was already unreachable on a stock run (`bd-done-stub` defaults off), but flipping the stub on for bisects fired MSI-X twice — once from the FSM, once from q-cp's `cb_complete → pcie_msix_trigger` via the `r100-imsix` MMIO trap — desyncing `host/msix.log`. P2 makes `cb_complete` the sole MSI-X source structurally so `host/msix.log` reads exactly one frame per BD regardless of bisect-mode flags. The `imsix` QOM link, the `R100IMSIXState *imsix` field on `R100Cm7State`, and the `r100_imsix.h` include all stay wired — P7 deletes them in one cleanup pass alongside the rest of the gated Stage 3c FSM. The FSM's final phase-transition log line was renamed `imsix_notify` → `complete` to reflect the new semantics. Verified mechanically (the deleted call lives in code gated by `bd_done_stub`, default false post-P1c — no observable change on a stock run) and via `./remucli test` (m5/m6/m7/m8 green). End-to-end `--host` validation (single `vector=0` in `msix.log` on stock; FSM `WAIT_QDESC → … → IDLE complete` without MSI-X on `bd-done-stub=on`) currently blocks on a pre-existing P1c-era flakiness in q-cp/CP0's `hil_init_descs` outbound DMA path (q-cp aborts on a NULL DDH read or the OP_READ_REQ never round-trips back as OP_READ_RESP — reproducible on bare `198d8a2` without P2 applied). Tracking that as a follow-up; it does not affect the structural correctness of P2's deletion. | done | — |
| P3 | All 4 DNC task-queue mailboxes instantiated — added three more `r100-mailbox` blocks at chiplet 0 (`PERI0_MAILBOX_M10` for UDMA, `PERI1_MAILBOX_M9` for UDMA_LP, `PERI1_MAILBOX_M10` for UDMA_ST), each as a 4 KB Samsung-IPM SFR like the existing COMPUTE slot at `PERI0_MAILBOX_M9`. The lazy-RAM placeholder loop now skips all four task-queue slots at chiplet 0 (the other three chiplets keep lazy-RAM since q-cp/CP1 only runs on chiplet 0). cm7's `mbtq-mailbox` link still points at slot 0 only — the mbtq stub indexed by INTGR1-bit `qid` (not by cmd_type) and only ever pushed COMPUTE entries; it's default off post-P1c with retirement scheduled in P7, so widening the stub here would only complicate the P7 cleanup. q-cp's `_inst[HW_SPEC_DNC_QUEUE_NUM=4]` table (COMPUTE / UDMA / UDMA_LP / UDMA_ST) now lands every `mtq_init` ISSR write on a real Samsung-IPM SFR rather than aliased lazy-RAM. (DDMA / DDMA_HIGHP go through the DDMA_AF auto-fetch path, not these polling mailboxes — the earlier "6 task-queue mailboxes" framing was based on the broader `common_cmd_type` enum and didn't reflect the firmware's actual `_inst[]` layout.) Verified: all four mailboxes appear in `info qtree` (NPU side: `peri0_m9_cpu1.chiplet0`, `peri0_m10_cpu1.chiplet0`, `peri1_m9_cpu1.chiplet0`, `peri1_m10_cpu1.chiplet0`); `info mtree` shows the four 4 KB i/o regions at `0x1FF9140000 / 0x1FF9150000 / 0x1FF9940000 / 0x1FF9950000` (no lazy-RAM overlap), with the other three chiplets retaining their 64 KB RAM placeholders; m5/m6/m7/m8 regression green; cold-boot `FW_BOOT_DONE` still travels the real path. | done | infrastructure for P1's per-cmd_type dispatch |
| P4A | `r100-rbdma` device, functional stub — new `src/machine/r100_rbdma.{c,h}` modelling the 1 MB `R100_NBUS_L_RBDMA_CFG_BASE = 0x1FF3700000` window per chiplet (offsets sourced from `g_rbdma_memory_map.h` / `g_cdma_global_registers.h` / `g_cdma_task_registers.h`). Sparse `R100RBDMARegStore` (GHashTable of offset → u32, mirror of `r100-dnc` / `r100-hbm`) so q-cp's RMW init sequences (`rbdma_clear_cdma`, autofetch enable, log-mgr setup, run-conf defaults) round-trip normally. **IP_INFO seeds:** `IP_INFO0..5` decoded from q-cp's `rbdma_get_ip_info` / `rbdma_update_credit` reads — info0 = release-date sentinel `0x20260101`, info1 = `{1,1,1,0}` version stamp, info2 = `chiplet_id` (overwritten by FW's explicit store), info3 = `num_of_executer = 8`, info4 = `{num_of_tqueue, num_of_utqueue, num_of_ptq} = {32, 32, 32}`, info5 = `{num_of_tequeue, num_of_uetqueue, num_of_fnsh_fifo, num_of_err_fifo} = {32, 32, 32, 8}`. Without these, the original passive-zero stub made `rbdma_update_credit` return zero credit and q-cp would never push a task. **Credit reporting:** `NORMALTQUEUE_STATUS` / `NORMALTQUEUE_EXT_STATUS` / `URGENTTQUEUE_{STATUS,EXT_STATUS}` / `PTQUEUE_STATUS` reads return the configured queue depth (`R100_RBDMA_NUM_{TQ,TEQ,UTQ,PTQ} = 32`) — "all free between kicks", which matches the model since the BH drains every kicked entry into the FNSH FIFO. **Kick → done IRQ:** q-cp's `rbdma_send_task` writes eight 32-bit words into `RBDMA_CDMA_TASK_BASE_OFF = 0x200`, ascending from `PTID_INIT` (`+0x000`) to `RUN_CONF1` (`+0x01C`). The store on `RUN_CONF1` is the silicon kickoff trigger — we capture the regstore-resident `PTID_INIT` (q-cp's `id_info.bits` payload, needed by `rbdma_done_handler` to match the cmd back), push an `R100RBDMAFnshEntry` onto an internal `R100_RBDMA_FIFO_DEPTH = 32` ring, and `qemu_bh_schedule` a BH that pulses `INT_ID_RBDMA1 = 978` on the chiplet's GIC for every entry whose `RUN_CONF1.intr_disable` bit is clear. q-cp's done handler then loops on `INTR_FIFO_READABLE_NUM` (live `fnsh_depth() & 0xFF`, served above the regstore) and reads `FNSH_INTR_FIFO` (which pops the head of our ring, returning `ptid_init`). `intr_disable=1` (q-cp's `dump_shm` / `shm_clear` paths) still pushes the FIFO entry but skips the GIC pulse, matching silicon. **Wiring:** `r100-rbdma` is instantiated per chiplet inside `r100_chiplet_init` (q-cp's `rbdma_init(cl_id)` runs on every CA73 CP0); two `sysbus_init_irq` slots are connected to that chiplet's GIC at `R100_INTID_TO_GIC_SPI_GPIO(R100_INT_ID_RBDMA0_ERR=977)` (idx 0, reserved for future synthesised faults — never pulsed today) and `R100_INT_ID_RBDMA1=978` (idx 1, fnsh-FIFO completion). Prio 1 in `cfg_mr` overlay beats the chiplet-wide unimpl catch-all. **Refactor:** the prior passive variant lived as a small section inside `r100_dnc.c` (alongside the DCL CFG stub it shared the regstore pattern with) — P4A carved it into dedicated `r100_rbdma.{c,h}` so the active task-completion path doesn't bloat the DNC file and so future RBDMA-only features (TE register banks, autofetch mirroring, behavioural P4B) have a clear home. **Verified:** `./remucli build` clean; m5/m6/m7/m8 regression green; new `tests/p4a_rbdma_stub_test.py` (registered as `./remucli test p4a`) launches `--host`, attaches to NPU HMP, and asserts via `xp` reads against chiplet-0 RBDMA at `0x1FF3700000` that `IP_INFO3.num_of_executer == 8`, `NORMALTQUEUE_STATUS == 32`, `PTQUEUE_STATUS == 32`, and `INTR_FIFO_NUM == 0` (idle). The kick path itself is exercised end-to-end once P1 lands a real workload — there's no synthetic injector today because the silicon trigger sequence (8-word descriptor with PTID_INIT then RUN_CONF1) is awkward to drive purely through HMP `xp` reads. | done | covers cmd_type ∈ {DDMA, DDMA_HIGHP}; kick-side coverage falls out of P10 once umd drives a real workload |
| P4B | `r100-rbdma` behavioural OTO byte-mover — extends `r100_rbdma_kickoff` (the RUN_CONF1 store handler from P4A) with a `task_type` switch. **OTO path:** reconstruct the full 41-bit byte addresses from `SRCADDRESS_OR_CONST` / `DESTADDRESS` (low 32 bits of the 128 B-unit address) ⊕ `RUN_CONF0.{src,dst}_addr_msb` (high 2 bits each) shifted left by 7, add `chiplet_id * R100_CHIPLET_OFFSET` to translate q-cp's chiplet-local view into QEMU global (same convention `r100-hdma` uses for SAR), then `address_space_read` SAR → temp buffer → `address_space_write` DAR via `&address_space_memory`. Capped at `RBDMA_OTO_MAX_BYTES = 32 MiB` (mirrors umd cmdgen's `args->size > SZ_32M` guard). On `address_space` failure: log `LOG_GUEST_ERROR`, bump `oto_dma_errors`, and still fall through to the FNSH push so q-cp's done loop unwinds (post-mortem log + the stale dst memory together signal the error). On success: bump `oto_kicks` + `oto_bytes` (observability counters survive reset). **Address translation scope — read carefully:** SAR / DAR are *device virtual addresses* (DVAs) on real silicon — kmd allocates them, and on the wire RBDMA's AXI burst traverses the per-chiplet SMMU-600 (S1 + S2 page-table walk) before hitting DDR. P4B does **not** model that translation. `r100_smmu.c` is a register-only stub (auto-acks `CR0→CR0ACK`, advances `CMDQ_CONS=PROD`, never walks STE / CD / page tables that FW sets up in DRAM), so REMU's effective transform is `S1 ∘ S2 = identity` — i.e. the regime where FW runs SMMU in pure-bypass / identity-mapped mode. The `chiplet_id * R100_CHIPLET_OFFSET` add is **not** an SMMU substitute; it's REMU's flat-global-vs-chiplet-local plumbing (every chiplet's DRAM is mounted at its own offset in `&address_space_memory`, and engines on chiplet N see chiplet-local addresses on their NoC). `r100-hdma` / `r100-dnc-cluster` use the same convention — DVA-equals-PA is REMU's cross-fleet baseline. Honouring real FW page tables is tracked separately as the long-term "SMMU honour FW page tables" follow-on (see § Long-term follow-ons); the natural plug point is a translation hook in `r100_rbdma_do_oto` just before the `address_space_{read,write}` call (and a sibling hook in `r100_hdma_kick_{wr,rd}`). **Other task_types** (CST/DAS/PTL/IVL/DUM/VCM/OTM/GTH/SCT/GTHR/SCTR) fall through to the kick-acknowledge stub (`unimp_task_kicks++` + `LOG_UNIMP` once per kick) — q-cp's done handler still observes the FNSH FIFO entry, but no real bytes move. Deferred to a P4B-extension once a workload that needs them lands (umd's `simple_copy` only emits OTO, but `q-sys/unit_test/rbdma_test.c` walks the full enum). **Verified:** `./remucli build` clean; m5/m6/m7/m8/p4a regression green; new `tests/p4b_rbdma_oto_test.py` (registered as `./remucli test p4b`) boots `--host`, mmap-pre-fills a 4 KB pseudo-random pattern at chiplet-0 DRAM offset `0x07000000` via the shm splice, drives the 6-word OTO descriptor (PTID_INIT, SRCADDRESS, DESTADDRESS, SIZEOF128BLK, RUN_CONF0=task_type=OTO, RUN_CONF1=intr_disable=1) through the NPU's gdbstub in physical-memory mode (`Qqemu.PhyMemMode:1` → `M<addr>,4:<hex>` packets, gdbstub spawned on demand via the HMP `gdbserver tcp::4567` command — no `--gdb*` flag needed), and asserts byte-for-byte equality at the destination offset `0x07800000`. SMMU is bypassed throughout (the test writes raw chiplet-local offsets straight into SRC / DST, exercising the same identity-mapped regime FW operates under). The full umd `command_buffer` integration test stays gated on P5 (host↔device staging via `rblnCmdCopy` → HDMA). | done | covers `task_type=OTO` (the only flavour umd `simple_copy` emits) under SMMU-bypass; other task_types are LOG_UNIMP fall-through; full umd `test_command_buffer` exercise lands with P5; SMMU honour is a separate long-term follow-on |
| P5 | `r100-hdma` linked-list mode — extends the M9-1c device's MMIO surface so q-cp's `cmd_descr_hdma` / `hdma_ll_trigger` path runs unmodified. Distilled the Notion HDMA / SMMU pages into `docs/hdma-notion-notes.md` first to pin down LL-element shape, channel separation (`ch_sep=3` → 0x800 stride), and the SMMU bypass-region rule (HDMA-SID LUT all-bypass means `S1 ∘ S2 = identity`, matching `r100_smmu.c`'s register-only stub). **Missing per-channel registers**: added `ELEM_PF` / `HANDSHAKE` / `LLP_LO|HI` / `CYCLE` / `WATERMARK` / `CTRL1` / `FUNC_NUM` / `QOS` / `INT_SETUP` / `MSI_{STOP,WATERMARK,ABORT}_LO|HI` / `MSI_MSGD` to the regstore; `LLP` and `CTRL1.LLEN` are load-bearing for the kick path, the rest are RAZ-or-RW persistence so q-cp's HAL init / dump-regs sequences round-trip. **LL walk**: when `CTRL1.LLEN=1`, the doorbell handler walks the chain rooted at `LLP_LO|HI` synchronously inside the MMIO write — `dw_hdma_v0_lli` (24 B: ctrl/transfer_size/sar.reg/dar.reg) and `dw_hdma_v0_llp` (16 B jump records) are pre-read as a 24 B blob and discriminated on `control & DW_HDMA_V0_LLP`; `LIE` on the last LLI signals end-of-chain (matching q-cp's `record_lli(..., is_last)`). **Per-LLI routing** by SAR/DAR ≷ `REMU_HOST_PHYS_BASE`: WR && DAR≥host → NPU→host (chunked OP_WRITE over the existing `hdma` chardev with stripped DAR); RD && SAR≥host → host→NPU (chunked OP_READ_REQ + parked `qemu_cond_wait_bql()` on a per-channel `resp_cond` until OP_READ_RESP arrives, then `address_space_write` at DAR); both NPU → D2D in-process loop (`address_space_read` → `address_space_write`, 64 KB scratch chunks). The existing 0x80..0xBF `req_id` partition is unchanged — only one OP_READ_REQ is in flight per RD channel at a time, matching the partition's one-tag-per-channel layout. **MSI fan-out** (write to `MSI_MSGD` at the `MSI_{STOP,WATERMARK,ABORT}` addresses) is deferred since q-cp's local-IRQ path (SPI 186 + `SUBCTRL_EDMA_INT_CA73` per-channel pending bit) is already wired and `cb_complete → pcie_msix_trigger` covers BD MSI-X via `r100-imsix`. The full long-form rationale + CTRL1 bit table + LL-chunk size choice live in the `r100_hdma.c` file banner so future readers don't need this row. **Verified**: `./remucli build` clean; existing M9-1c non-LL kick (`r100_hdma_kick_wr/rd`) untouched + still drives M9-1c regression; new `tests/p5_hdma_ll_test.py` (registered as `./remucli test p5`) seeds a single dw_hdma_v0_lli+terminator at chiplet-0 DRAM offset `0x07900000` (SRC=0x07000000, DST=0x07800000, both NPU-local DRAM exercising the D2D path), drives `CTRL1=LLEN; LLP_LO|HI=desc; ENABLE=1; DOORBELL=START` through the NPU's gdbstub in physical-memory mode (same harness as p4b), and asserts byte-for-byte equality at the destination via the shm splice — the host-leg paths fall out of P10's umd `simple_copy` (one address_space ↔ chardev swap from D2D). `--host` cold-boot still reaches `rbln0 probed, id=0x0 (0:0)` and `xp/1wx 0xf000200ffc → 0xcafedead` (q-cp's `cb_complete → writel(FUNC_SCRATCH)` round-trip via P1b), confirming P1c's BD lifecycle is unaffected by the widened MMIO surface. m5/m6/m7/m8/p4a/p4b regression green. | done | Notion-distilled into `docs/hdma-notion-notes.md`; LL chain + bypass SMMU. The cm7 QINIT / BD-done shim that used to drive the chardev was retired by P1c. |
| P6 | `r100-dnc` behavioural — parse `cmd_descr_dnc`, run a host-CPU kernel against input tensors, write outputs, then fire `itdone` BH. Defer until P1 + P4A land and a real workload exists to drive it. | pending | gated by umd workload |
| P7 | Retire the gated cm7 stubs — after P1 + P2, all three default-off paths in `r100-cm7` (Stage 3c BD-done state machine: `OP_READ_REQ` walk + `OP_CFG_WRITE FUNC_SCRATCH` + `OP_WRITE bd.header \|= DONE` + `OP_WRITE queue_desc.ci++` with the `r100_imsix_notify` already gone in P2; Stage 3b QINIT stub: `r100_cm7_qinit_stub` + `REMU_FW_VERSION_STR`; M9-1b mbtq push: `r100_cm7_mbtq_push` + `r100_cm7_synth_cmd_descr` + the synth ring at `R100_CMD_DESCR_SYNTH_BASE`) had no reason to exist. Deleted, along with the three bool properties (`bd-done-stub` / `qinit-stub` / `mbtq-stub`), the `r100_hdma_set_cm7_callback` plumbing + its `0x01..0x0F` `req_id` partition (now reserved up to `0x7F` for future UMQ multi-queue use), the `imsix` / `mbtq-mailbox` QOM links and `R100IMSIXState *` / `R100MailboxState *` fields on `R100Cm7State`, the `cm7-debug-chardev` debug knob (the `cm7_log` plumbing in `cli/remu_cli.py` and the matching `cm7-debug` chardev wiring in `r100_soc.c`), and the `R100_CMD_TYPE_COMPUTE` / `R100_CMD_DESCR_SYNTH_*` macros + `R100_BD_FLAGS_DONE_MASK` / `REMU_DB_QUEUE_INIT_INTGR1_BIT` / `REMU_BD_*` / `REMU_QDESC_*` / `REMU_DDH_*` constants from `src/include/r100/remu_addrmap.h` + `src/bridge/remu_doorbell_proto.h`. `r100-cm7` now does exactly two things: (1) forward host BAR4 doorbells (INTGR0 bit 0 → `r100_imsix_notify`-free synthetic FW_BOOT_DONE re-handshake via the existing mailbox path; INTGR1 bit `qid` → SPI 185 wake for q-cp's `hq_task`); (2) host the P1b cfg-mirror MMIO trap at `R100_DEVICE_COMM_SPACE_BASE` and emit `OP_CFG_WRITE` upstream over `r100-hdma`. Single source of truth for BD lifecycle: q-cp's `cb_complete`. The HDMA bidirectional protocol stays — `r100-pcie-outbound` from P1 keeps the `0xC0..0xFF` partition; q-cp on CP0 owns `OP_READ_REQ`/`OP_READ_RESP` end-to-end through that backend. **Verified:** `./remucli build` clean (aarch64 + x86_64); m5/m6/m7/m8 + p4a/p4b/p5 regression green; `--host` smoke run reaches `rbln0 probed, id=0x0 (0:0)` with `xp/1wx 0xf000200ffc → 0xcafedead` (q-cp `cb_complete → FUNC_SCRATCH` round-trip via P1b unaffected); `cm7.log` no longer produced (matches the deleted debug chardev). | done | required P1 + P2; replaces ~700 LOC of gated scaffolding with the silicon path |
| P8 | Real CA73 soft-reset — replace M8b 3a's synthetic `0xFB0D` from `INTGR0 bit 0` with a bracketed reset of CP0/CP1 cluster CPUs + their GIC redistributor state + the PCIe mailbox regs (preserve DRAM/SRAM/cfg-head/any kmd-loaded firmware images), restart from BL1; q-sys `bootdone_task` re-emits `0xFB0D` through the existing `issr` chardev path naturally. Removes the last QEMU-side cold-boot lie. Candidate hooks: a `DeviceReset` on an aggregated "r100-ca73-cluster" QOM wrapper, or `qemu_system_reset_request` with a fine-grained `ShutdownCause` so the x86 host QEMU is unaffected. | pending | independent of P1..P7 |
| P9 | UMQ multi-queue — `NUMBER_OF_CMD_QUEUES > 1`. Falls out of P1's per-queue parser; q-cp's `hq_task` already loops over `qid` in `host_queue_manager.c`, so the work is mostly host-side allocation + per-queue MSI-X vector wiring. Includes BD-done qid=1 (kmd "message ring") which Stage 3c's `qdesc ring_log2 range` guard used to skip; q-cp handles it natively post-P1c. | pending | depends on P1 |
| P10 | umd smoke test in `guest/` — `rblnCreateContext` → submit → wait → exit 0. After P1 + P2 + P3 + P4A + P5 + P7, this exercises the full silicon-accurate path. | pending | absorbs the old M9-3 |

### Order of attack

Shortest path to honest end-to-end:

1. **P1 (done — P1a + P1b + P1c)** — q-cp's `hq_task → cb_task →
   cb_complete` runs end-to-end on the COMPUTE synth via the P1a
   PCIe-outbound stub (chiplet-0 4 GB AXI window over `hdma`) and
   the P1b cfg-mirror trap (NPU↔host BAR2 cfg-head bidirectional).
   P1c gates the legacy QEMU-side scaffolding (Stage 3c BD-done FSM,
   Stage 3b QINIT stub, M9-1b mbtq push) behind default-off
   `-global r100-cm7.{bd-done,qinit,mbtq}-stub=on` knobs, so on a
   stock run only `cb_complete → pcie_msix_trigger` fires MSI-X.
   `rbln_queue_test` already passes via the real path
   (`xp /1wx 0xf000200ffc → 0xcafedead`); the gated paths stay
   compiled in for bisecting q-cp regressions.
2. **P2 (done)** — Deleted the now-dead `r100_imsix_notify` call
   from `r100-cm7`'s Stage 3c BD-done state machine. After P1c the
   call was unreachable on a stock run, but it still fired duplicate
   MSI-X if `bd-done-stub=on` was used for bisecting — making
   bisection results misleading. P2 makes `cb_complete` the sole
   MSI-X source structurally, not just by default config. The
   `imsix` QOM link / `R100IMSIXState *` field / `r100_imsix.h`
   include were left wired in P2 and retired alongside the rest of
   the gated Stage 3c FSM in P7 (one diff).
3. **P3 (done)** — All four DNC task-queue mailboxes live as real
   Samsung-IPM SFRs on chiplet 0 (COMPUTE / UDMA / UDMA_LP / UDMA_ST,
   matching q-cp's `_inst[HW_SPEC_DNC_QUEUE_NUM=4]`). Every cmd_type
   q-cp on CP0 dispatches to a DNC queue lands on a real ring rather
   than aliased lazy-RAM.
4. **P4A (done) + P5 (done)** — RBDMA + HDMA reachable as functional
   engines. Every cmd_type completes through its real engine entry
   point: `cmd_descr_rbdma` lands on `r100-rbdma`'s 8-word descriptor
   trigger and propagates through the FNSH FIFO + GIC SPI 978;
   `cmd_descr_hdma` lands on `r100-hdma`'s `dw_hdma_v0` channel
   registers and walks the LL chain (`dw_hdma_v0_lli` / `_llp`) with
   per-LLI routing into D2D in-process copy or chunked OP_WRITE /
   OP_READ_REQ over the `hdma` chardev (req_id partition 0x80..0xBF).
5. **P4B (done)** — RBDMA `task_type=OTO` is behavioural; other
   task_types log unimp + fall through. Real tensor bytes move
   between any two chiplet-local DRAM offsets that fit the 41-bit
   encoding. Verified via `./remucli test p4b`.
6. **P7 (done)** — deleted the rest of Stage 3c (BD reads + DONE
   writeback + ci advance + mbtq push) + the QINIT stub + the
   `cm7-debug` chardev. Single source of truth for BD lifecycle is
   now q-cp's `cb_complete`. `r100-cm7` is reduced to its two real
   responsibilities: BAR4 doorbell forward + P1b cfg-mirror trap.
7. **P10** — umd smoke test on the now-silicon-accurate path.
8. **P6 + P9** — behavioural DNC + multi-queue, gated by workload
   need.
9. **P8** — orthogonal; tackle whenever the synthetic `FW_BOOT_DONE`
   blocks something concrete (e.g. driver re-bind testing).

After P1..P3 + P4A + P4B + P5 + P7 (done) + P10 (eight milestones),
every BD the host submits flows through the silicon-accurate path:
`r100-cm7` forwards the doorbell → q-cp on CA73 CP0 reads the BD,
walks the CB, programs the engines, pushes DNC tasks to CP1, and
closes the loop with `cb_complete` → DONE writeback + MSI-X via
`r100-imsix`. No fake DONEs, no synthetic cmd_types, no QEMU-side
completions. From that base, P6 / P9 become real-tensor / multi-queue
work, not architectural fixes; P4B's non-OTO `task_type` extensions
fall out of the same workload-gated track.

### Success criteria

- `insmod rebellions.ko` succeeds, device probes, BAR sizes match
  (M3 ✓)
- MSI-X vectors allocated, `FW_BOOT_DONE` handshake completes
  (M8b 3a ✓ — synthesised; honest in P8)
- `rebel_hw_init` / `rebel_queue_init` pass (M8b 3b ✓)
- `rbln_queue_test` completes via q-cp's `cb_complete`, not a
  QEMU-side BD-done shortcut (P1 + P2)
- BD's actual packet stream is decoded and dispatched per cmd_type
  to the right engine device (P1 + P3 + P4A + P5)
- umd opens device, creates context, submits simple job (P10)
- Real tensor data flows for at least one cmd_type (P4B ✓ — OTO via
  `r100-rbdma` `address_space_read/write`; P6 still gates on a
  workload that needs DNC kernel emulation)

### Long-term fidelity follow-ons

Not blocking the P-plan above, but the natural follow-ons once the
dispatch graph is honest:

- **HDMA scatter-gather** — full multi-LL DMA with address translation;
  extension of P5's behavioural step.
- **SMMU honour FW page tables** — today's bypass is fine for
  functional tests; required for security / multi-tenant fidelity.
  `r100_smmu.c` is currently a register-only stub (acks `CR0→CR0ACK`,
  auto-advances `CMDQ_CONS=PROD`) — STE / CD / page tables FW sets up
  in DRAM are never walked, so every engine in REMU effectively
  operates at `S1 ∘ S2 = identity`. Engines that consume DVAs
  (`r100-rbdma` SAR/DAR per P4B, `r100-hdma` SAR/DAR per M9-1c/P5,
  `r100-dnc-cluster` cmd_descr fields per M9-1c) all add a
  `chiplet_id * R100_CHIPLET_OFFSET` REMU-flat-global offset and call
  `address_space_{read,write}` directly — no translation. When this
  follow-on lands, the natural plug points are translation hooks at
  each of those call sites (a single `r100_smmu_translate(asid, dva,
  size, &pa)` helper would suffice), driven by FW-published STE / CD
  / S1 / S2 tables already sitting in DRAM. Until then, kmd / q-cp
  must keep using identity-mapped IOVAs on REMU.
- **Performance counters** — synthetic cycle counts for FW timing
  paths; meaningless today.

## Timing Considerations

REMU is purely functional. Consequences:

- PLL / DMA / HBM training complete instantly
- FW time-out paths may never trigger
- Hardware-timing race conditions may not reproduce
- Performance counter values are meaningless
- FreeRTOS tick fires, but wall-clock ratios differ

Annotated delays can be added per-device if a future workload needs them.
