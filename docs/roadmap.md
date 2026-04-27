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
| 1c | Active `r100-dnc-cluster` task-completion path (TASK_DESC_CFG1.itdone trigger → BH-scheduled GIC SPI on the matching DNC INTID, synthesised done_passage at TASK_DONE). New `r100-hdma` device modelling DesignWare dw_hdma_v0 at `R100_HDMA_BASE = 0x1D80380000` (chiplet 0); takes ownership of the `hdma` chardev (single-frontend), with `r100-cm7` reaching it through a QOM link + public emit API in `r100_hdma.h`. r100-cm7's mbtq push synthesises a minimal valid `cmd_descr` (cmd_type=COMPUTE, core_affinity=BIT(0)) into chiplet-0 private DRAM at `R100_CMD_DESCR_SYNTH_BASE = 0x20000000`, replacing the M9-1b NULL-deref placeholder. GIC `num-irq` bumped 256 → 992 to fit DNC INTIDs (up to 617). Phase 1 boot + m5/m6/m7/m8 + Stage 3c BD-done qid=0 all preserved. The COMPUTE-only synthesis is intentionally minimal — replaced by real BD parsing in P2 | done | — |

The previously planned **M9-2** (q-cp end-to-end on the COMPUTE
shortcut) and **M9-3** (umd test binary) are superseded by the
architectural plan below. M9-2's verification step lives on as P1;
M9-3's umd test becomes the smoke target after P2+P4A+P5 land.

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
  instantiates three `r100-mailbox` blocks (PF, VF0, and the M9-1b
  COMPUTE task queue at `R100_PERI0_MAILBOX_M9_BASE`), optional
  `r100-cm7` / `r100-imsix` / `r100-hdma` gated on chardev machine-props.
  Wires `cfg` + `cm7-debug` chardevs onto `r100-cm7`; the `hdma` chardev
  goes to `r100-hdma` (M9-1c). Links `r100-cm7` to `r100-imsix` (Stage 3c
  scaffolding MSI-X — retired in P7), to `r100-hdma` (chardev egress),
  and to the M9 mailbox (M9-1b mbtq pushes). Wires DNC done SPIs from
  each DCL (8 × 4 lines per DCL) to the chiplet GIC via
  `r100_dnc_intid()` (M9-1c). Skips lazy-RAM at the M9 slot to avoid
  overlap. GIC `num-irq` is 992 (was 256 pre-M9-1c) to fit DNC INTIDs.
- `src/machine/r100_mailbox.c` — Samsung IPM SFR (`INTGR/INTCR/INTMR/INTSR/INTMSR/ISSR0..63`),
  two `qemu_irq` outs tracking INTMSR. API: `r100_mailbox_raise_intgr`,
  `r100_mailbox_set_issr`, `r100_mailbox_get_issr`,
  `r100_mailbox_cm7_stub_write_issr`,
  `r100_mailbox_set_issr_words` (M9-1b in-process multi-slot write
  for q-cp task-queue pushes; bypasses the issr_store funnel).
- `src/machine/r100_cm7.c` — reassembles 8-byte `(offset, value)` frames,
  routes by offset into mailbox INTGR / ISSR / CM7-stub. Today the file
  hosts five PCIE_CM7 stubs; three are honest infrastructure that stays,
  two are regression scaffolding that the P-plan retires:
  (1) `INTGR0 bit 0` SOFT_RESET → synthetic `FW_BOOT_DONE` (Stage 3a) —
  *scaffolding, retired in P8*;
  (2) `cfg_shadow[1024]` mirror of host BAR2 cfg-head and the QINIT
  stub on `INTGR1 bit 7` (Stage 3b) — *real PCIE_CM7 work, kept*;
  (3) the BD-done state machine on `INTGR1 bits 0..N` that drives
  `rbln_queue_test` (Stage 3c) — *scaffolding, retired in P7*;
  (4) `r100_cm7_mbtq_push` on the same INTGR1 queue-doorbell —
  *kept, but P2 replaces synthesised payload with real BD-derived
  cmd_descr + per-cmd_type routing*;
  (5) cmd_descr synthesis at `R100_CMD_DESCR_SYNTH_BASE` —
  *kept as a ring buffer, but populated by the P2 packet walker
  rather than a constant COMPUTE template*.
  The hdma chardev moved to `r100-hdma` in M9-1c — cm7 still drives the
  same wire-level traffic via `r100_hdma_emit_*` helpers and registers
  an RX callback via `r100_hdma_set_cm7_callback()` for OP_READ_RESP
  responses tagged `req_id = qid + 1`.
- `src/machine/r100_hdma.{c,h}` — DesignWare dw_hdma_v0 register-block
  model (M9-1c) at `R100_HDMA_BASE` (chiplet 0). Per-channel state for
  16 WR + 16 RD channels (SAR, DAR, XferSize, doorbell, status,
  int_status). On WR doorbell → emit OP_WRITE with payload pulled from
  chiplet sysmem at SAR; on RD doorbell → emit OP_READ_REQ tagged with
  `req_id = R100_HDMA_REQ_ID_CH_MASK_BASE | (dir << 5) | ch`, complete
  on matching OP_READ_RESP by `address_space_write` into chiplet sysmem
  at DAR. Single GIC SPI 186 line + per-channel pending bit set in
  SUBCTRL_EDMA_INT_CA73 (`0x1FF8184368`, plain RAM in pcie-subctrl).
  Owns the `hdma` chardev (single-frontend constraint); cm7 reaches
  it via a QOM link + public emit API. Used by P5 as the target for
  q-cp-driven `cmd_descr_hdma` packets.
- `src/machine/r100_dnc.c` — DCL CFG-window stub. Passive sparse
  regstore seeds IP_INFO / SHM TPG / RDSN bits during Phase 1 boot.
  M9-1c added an active path: writes to slot+0x81C
  (TASK_DESC_CFG1) with `access_size=4` and `itdone=1` schedule a BH
  that synthesises a `dnc_reg_done_passage` at slot+0xA00 and pulses
  the matching DNC GIC SPI (lookup at `r100_dnc_intid()`). Passive
  reads still served by regstore fall-through. P6 may extend with a
  behavioural compute kernel.
- `src/machine/r100_imsix.c` — 4 KB MMIO trap at `R100_PCIE_IMSIX_BASE`,
  emits `(offset, db_data)` frames on write to `0xFFC`. Public API
  `r100_imsix_notify(vector)` called by `r100-cm7` BD-done completion
  (Stage 3c scaffolding) today. After P7 (Stage 3c retirement), this
  trap is driven by q-cp's `cb_complete` on CA73 CP0, whose
  `pcie_msix_trigger` is already linked from upstream
  `q/sys/osl/FreeRTOS/Source/rbln/msix.c` (no FW-side glue required).
  The QEMU-side `r100_imsix_notify` API is retained only for any
  remaining stub paths.
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
| Program engines (DNC / RBDMA / HDMA) | q-cp HAL drivers writing engine MMIO | Engine MMIO partly modelled (DNC done-IRQ ✓, HDMA reg block ✓, RBDMA pending P4A); q-cp never programs them because Stage 3c short-circuits the chain |
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
6. **All 6 task-queue mailboxes instantiated** — q-cp's
   `_inst[cmd_type]` table covers COMPUTE / UDMA / UDMA_LP / UDMA_ST
   / DDMA / DDMA_HIGHP; we instantiate all of them.
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
| P1 | q-cp end-to-end on the existing COMPUTE synth — keep M9-1b/1c's `r100-cm7`-driven mbtq push and synthesised `cmd_descr` (cmd_type=COMPUTE, no `cb_jd_addr` yet), but stop firing MSI-X from `r100-cm7`. Verify q-cp's CP1 worker pops mbtq → `dnc_send_task` → DNC stub `itdone` BH → q-cp on CP0's `cb_complete` → BD.DONE writeback + MSI-X via the existing `r100-imsix` MMIO path (driven by upstream `q/sys/osl/FreeRTOS/Source/rbln/msix.c` already linked into CP0) → kmd sees completion. From this point on, half of Stage 3c is unnecessary. | pending | absorbs the old M9-2 verification step |
| P2 | Engine MMIO surface honest enough for q-cp's existing CB walker — q-cp on CA73 CP0 already implements packet parsing in full (`cb_parse_nop/write_data/linked_dma/lin_dma/ctx_dma/flush/...` in `q/cp/src/cb_mgr/command_buffer_manager.c`). The REMU-side work is to (a) stop Stage 3c from short-circuiting BD reads, so `hq_task` → `cb_task` actually wakes on host-submitted BDs, and (b) make each cmd_type's HAL writes land on a device that responds: DNC done IRQ (M9-1c ✓), HDMA kick/done (P5 — q-cp drives the same `dw_hdma_v0` channel registers M9-1c added), RBDMA kick/done (P4A). The mbtq push moves out of `r100-cm7`'s queue-doorbell stub and into q-cp on CP0, where `cmd_descr` is built from real CB content with the real `cmd_type`. `r100-cm7` no longer parses CBs, synthesises packets, or touches `BD.header`. | pending | requires P1 + P4A + P5; the parser already exists in firmware |
| P3 | All 6 task-queue mailboxes instantiated — add UDMA / UDMA_LP / UDMA_ST / DDMA / DDMA_HIGHP `r100-mailbox` blocks at the M9 offsets (currently only COMPUTE). Wire ISSR shadow bypass (`r100_mailbox_set_issr_words`) for each. q-cp's `_inst[cmd_type]` per-cmd_type pollers all resolve. | pending | infrastructure for P2's per-cmd_type dispatch |
| P4A | `r100-rbdma` device, functional stub — new `src/machine/r100_rbdma.{c,h}` modelling the RBDMA register block (base from `g_rbdma_memory_map.h`, layout from `rbdma_regs.h`). Per-channel state for WR + RD; kick register, done IRQ, status. GIC SPI lines wired into chiplet-0 GIC at the right INTIDs. Trap kick → BH-schedule fake done passage + GIC SPI; mirror of `r100-dnc` M9-1c. q-cp's `cb_complete` fires; no DMA performed. | pending | covers cmd_type ∈ {DDMA, DDMA_HIGHP} |
| P4B | `r100-rbdma` behavioural — parse rbdma `task_type` (OTO/CST/DAS/PTL/IVL/DUM/VCM/OTM/GTH/SCT/GTHR/SCTR), execute via `address_space_read/write` between SAR and DAR, then fire done. Real tensor bytes move. Gated by workload need. | pending | depends on P4A |
| P5 | `r100-hdma` as a first-class engine — extend the existing M9-1c device to handle q-cp-driven `cmd_descr_hdma` (cmd_type_ext = HDMA). q-cp programs the same `dw_hdma_v0` channel registers; the existing engine pulls/pushes via `address_space_*`, fires done. No new device, just new entry point. | pending | replaces today's r100-cm7-driven QINIT/BD-done shim usage with q-cp-driven usage |
| P6 | `r100-dnc` behavioural — parse `cmd_descr_dnc`, run a host-CPU kernel against input tensors, write outputs, then fire `itdone` BH. Defer until P2 + P4A land and a real workload exists to drive it. | pending | gated by umd workload |
| P7 | Retire Stage 3c BD-done — after P1 + P2, `r100-cm7`'s BD-done state machine (the `OP_READ_REQ` walk + `OP_CFG_WRITE FUNC_SCRATCH` + `OP_WRITE bd.header \|= DONE` + `OP_WRITE queue_desc.ci++` + `r100_imsix_notify`) has no reason to exist. Delete it. Single source of truth for BD lifecycle: q-cp's `cb_complete`. The HDMA bidirectional protocol stays — `r100-cm7` still uses `OP_READ_REQ`/`OP_READ_RESP` to fetch packet streams from host DMA in P2. | pending | requires P1+P2 |
| P8 | Real CA73 soft-reset — replace M8b 3a's synthetic `0xFB0D` from `INTGR0 bit 0` with a bracketed reset of CP0/CP1 cluster CPUs + their GIC redistributor state + the PCIe mailbox regs (preserve DRAM/SRAM/cfg-head/any kmd-loaded firmware images), restart from BL1; q-sys `bootdone_task` re-emits `0xFB0D` through the existing `issr` chardev path naturally. Removes the last QEMU-side cold-boot lie. Candidate hooks: a `DeviceReset` on an aggregated "r100-ca73-cluster" QOM wrapper, or `qemu_system_reset_request` with a fine-grained `ShutdownCause` so the x86 host QEMU is unaffected. | pending | independent of P1..P7 |
| P9 | UMQ multi-queue — `NUMBER_OF_CMD_QUEUES > 1`. Falls out of P2's per-queue parser. Includes BD-done qid=1 (kmd "message ring") which Stage 3c skips today. | pending | depends on P2 |
| P10 | umd smoke test in `guest/` — `rblnCreateContext` → submit → wait → exit 0. After P1 + P3 + P4A + P5 + P7, this exercises the full silicon-accurate path. | pending | absorbs the old M9-3 |

### Order of attack

Shortest path to honest end-to-end:

1. **P1** — `rbln_queue_test` now passes via the real path (q-cp's
   `cb_complete`, not a QEMU shortcut). Half of Stage 3c is already
   dead weight.
2. **P2 → P3** — q-cp's existing `cb_parse_*` walker drives real
   work, all six mailboxes are live, and every cmd_type the host
   submits routes correctly through q-cp on CP0 — even if downstream
   engines are stubs.
3. **P4A + P5** — RBDMA + HDMA reachable as functional stubs. Every
   cmd_type completes through its real engine entry point.
4. **P7** — delete Stage 3c. Single source of truth for BD lifecycle.
5. **P10** — umd smoke test on the now-silicon-accurate path.
6. **P4B + P6 + P9** — behavioural fidelity + multi-queue, gated by
   workload need.
7. **P8** — orthogonal; tackle whenever the synthetic `FW_BOOT_DONE`
   blocks something concrete (e.g. driver re-bind testing).

After P1..P3 + P4A + P5 + P7 + P10 (seven milestones), every BD the
host submits flows through the silicon-accurate path: `r100-cm7`
forwards the doorbell → q-cp on CA73 CP0 reads the BD, walks the CB,
programs the engines, pushes DNC tasks to CP1, and closes the loop
with `cb_complete` → DONE writeback + MSI-X via `r100-imsix`. No
fake DONEs, no synthetic cmd_types, no QEMU-side completions. From
that base, P4B / P6 / P9 become real-tensor work, not architectural
fixes.

### Success criteria

- `insmod rebellions.ko` succeeds, device probes, BAR sizes match
  (M3 ✓)
- MSI-X vectors allocated, `FW_BOOT_DONE` handshake completes
  (M8b 3a ✓ — synthesised; honest in P8)
- `rebel_hw_init` / `rebel_queue_init` pass (M8b 3b ✓)
- `rbln_queue_test` completes via q-cp's `cb_complete`, not a
  QEMU-side BD-done shortcut (P1 + P7)
- BD's actual packet stream is decoded and dispatched per cmd_type
  to the right engine device (P2 + P3 + P4A + P5)
- umd opens device, creates context, submits simple job (P10)
- Real tensor data flows for at least one cmd_type (P4B or P6)

### Long-term fidelity follow-ons

Not blocking the P-plan above, but the natural follow-ons once the
dispatch graph is honest:

- **HDMA scatter-gather** — full multi-LL DMA with address translation;
  extension of P5's behavioural step.
- **SMMU honour FW page tables** — today's bypass is fine for
  functional tests; required for security / multi-tenant fidelity.
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
