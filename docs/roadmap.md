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

## Phase 2: Host Drivers

**Prerequisite**: Phase 1 complete (q-cp on CP1 services part of the
host-facing FW interface).

**Goal**: kmd loads in an x86_64 guest, probes the emulated CR03 PCI
device, handshakes with FW, umd opens the device.

### Milestones

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
| M9  | DNC stub + trivial umd job | in progress (1b done) | see sub-table |

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
| 3a | KMD soft-reset handshake: `r100-cm7` CM7-stub synthesises `FW_BOOT_DONE` → PF.ISSR[4] on `INTGR0 bit 0`, bypassing unmodelled PCIE_CM7 | done | `a01d2b5` |
| 3a-fix | PCIe mailbox GIC wiring corrected (`qdev_get_gpio_in` takes a 0-based SPI index, not an INTID) — removes silent IRQ storm on CA73 CPU0 that froze `bootdone_task` during `--host` runs. Cold-boot `FW_BOOT_DONE` now travels the real path (q-sys `bootdone_task` → PF.ISSR[4] → `issr` chardev → host BAR4 shadow). Stage 3a's stub is retained but narrowed to the post-soft-reset re-handshake only (see Pending: "real CA73 soft-reset"); post-mortem in `docs/debugging.md` | done | — |
| 3b | `rebel_hw_init` QINIT handshake: new `cfg` chardev (host → NPU BAR2 cfg-head mirror, incl. `DDH_BASE_{LO,HI}`) + `hdma` chardev (NPU → host DMA writes, 24 B header + payload) — NPU-side CM7 stub handles `INTGR1 bit 7 = QUEUE_INIT` by emitting HDMA writes for `fw_version = "3.remu-stub"` and `init_done = 1` | done | — |
| 3c | `rbln_queue_test` BD-done completion: rename `r100-doorbell` → `r100-cm7`, extend HDMA protocol bidirectional (`OP_READ_REQ/RESP`, `OP_CFG_WRITE`, `flags` → `req_id` tag), add BD-done async state machine on NPU-side CM7 that reads `queue_desc`+`bd`+`pkt` via host DMA, writes `FUNC_SCRATCH` via cfg, sets `BD_FLAGS_DONE_MASK`+advances `ci`, then `r100_imsix_notify` vec=0 → MSI-X | done | — |

#### HDMA opcodes (Stage 3c wire format)

`struct RemuHdmaHeader { op, req_id, dst, len }` — all 24 B, little-endian:

| Opcode | Direction | Use |
|---|---|---|
| `REMU_HDMA_OP_WRITE = 1` | NPU → host | write `len` bytes at guest DMA `dst` (QINIT, BD-done bd+ci) |
| `REMU_HDMA_OP_READ_REQ = 2` | NPU → host | request `len` bytes from guest DMA `dst`, tagged by `req_id` |
| `REMU_HDMA_OP_READ_RESP = 3` | host → NPU | response for `req_id`, payload is the bytes read |
| `REMU_HDMA_OP_CFG_WRITE = 4` | NPU → host | update host-side `cfg_mmio_regs[dst>>2]` (reverse of Stage-3b cfg forwarding) |

`req_id = 0` reserved for the untagged QINIT `OP_WRITE` pair
(`fw_version` + `init_done`). BD-done uses `req_id = qid + 1` on
every frame it emits — `OP_READ_REQ`, the follow-up `OP_CFG_WRITE`,
and both bookkeeping `OP_WRITE`s — so logs for concurrent queues
stay disentangled.

#### M9 sub-milestones

PCIE_CM7 walks BDs directly for simple packets (Stage 3c, unchanged)
and forwards DNC tasks to q-cp via the `PERI0_MAILBOX_M9_CPU1`
compute task queue (`mb_task_queue.c`). REMU has no Cortex-M7 vCPU,
so `r100-cm7` emulates PCIE_CM7's effect on this path too.

| Stage | What | Status | Commit |
|---|---|---|---|
| 1b | Real `r100-mailbox` instance at `R100_PERI0_MAILBOX_M9_BASE` (chiplet 0) — replaces lazy-RAM placeholder. `r100-cm7` pushes a 24 B placeholder `dnc_one_task` (`{cmd_id=qid+1, 0, 0}`) + bumps `MBTQ_PI_IDX` on every `INTGR1` queue-doorbell. New `r100_mailbox_set_issr_words` helper bypasses the issr_store funnel (no chardev egress, no host-relay accounting). cm7-debug emits `mbtq qid=N slot=M pi=P status=ok` for observability. Stage 3c BD-done unaffected — `rbln_queue_test` still passes. Push reaches storage at the right slots; q-cp consumption still under investigation (likely waits for DNC stub in 1c) | done | — |
| 1c | `r100-dnc` register-block model (accept task, fire completion IRQ to q-cp); HDMA register-block model so q-cp can DMA-read host RAM | pending | — |
| 2 | DNC task BD discrimination (decode packet type from BD payload, populate real `cmd_descr` pointer in NPU SRAM, replace placeholder push). q-cp completion → BD.DONE write-back + MSI-X | pending | — |
| 3 | umd test binary in `guest/` (`rblnCreateContext` → submit → wait → exit 0) | pending | — |

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
  compute task queue at `R100_PERI0_MAILBOX_M9_BASE`), optional
  `r100-cm7` / `r100-imsix` gated on chardev machine-props. Wires
  `cfg` + `hdma` + `cm7-debug` chardevs onto `r100-cm7`; links
  `r100-cm7` to `r100-imsix` (BD-done MSI-X) and to the M9 mailbox
  (M9-1b mbtq pushes). Skips lazy-RAM at the M9 slot to avoid overlap.
- `src/machine/r100_mailbox.c` — Samsung IPM SFR (`INTGR/INTCR/INTMR/INTSR/INTMSR/ISSR0..63`),
  two `qemu_irq` outs tracking INTMSR. API: `r100_mailbox_raise_intgr`,
  `r100_mailbox_set_issr`, `r100_mailbox_get_issr`,
  `r100_mailbox_cm7_stub_write_issr`,
  `r100_mailbox_set_issr_words` (M9-1b in-process multi-slot write
  for q-cp task-queue pushes; bypasses the issr_store funnel).
- `src/machine/r100_cm7.c` — reassembles 8-byte `(offset, value)` frames,
  routes by offset into mailbox INTGR / ISSR / CM7-stub. Hosts every
  CM7-firmware responsibility that silicon's PCIE_CM7 would own:
  (1) `INTGR0 bit 0` SOFT_RESET → synthetic `FW_BOOT_DONE` (Stage 3a);
  (2) `cfg_shadow[1024]` mirror of host BAR2 cfg-head and the QINIT
  stub on `INTGR1 bit 7` (Stage 3b); (3) the BD-done state machine on
  `INTGR1 bits 0..N` that drives `rbln_queue_test` (Stage 3c);
  (4) `r100_cm7_mbtq_push` on the same INTGR1 queue-doorbell — pushes
  a placeholder `dnc_one_task` to the M9 task-queue mailbox to wake
  q-cp's `taskmgr_fetch_dnc_task_master_cp1` poll loop (M9-1b).
  Optional `cm7-debug` chardev emits ASCII phase-transition + mbtq
  push traces.
- `src/machine/r100_imsix.c` — 4 KB MMIO trap at `R100_PCIE_IMSIX_BASE`,
  emits `(offset, db_data)` frames on write to `0xFFC`. Public API
  `r100_imsix_notify(vector)` called by `r100-cm7` BD-done completion.
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

### Pending components

- DNC stub (accept task, generate completion IRQ to q-cp; q-cp drives
  BD.DONE + MSI-X) — M9-1c / M9-2
- HDMA register-block model — M9-1c (q-cp side needs real DMA-read of
  host RAM, complementing the cross-process `hdma` chardev shortcut
  used by `r100-cm7` today)
- NPU-local DRAM scratch write on BD-done (silicon mirrors `FUNC_SCRATCH`
  into chiplet DRAM too; kmd currently reads it via BAR2 cfg-head path
  only, so Stage 3c's `OP_CFG_WRITE` suffices) — future work
- UMQ queues (`NUMBER_OF_CMD_QUEUES > 1`, not exercised by
  `rbln_queue_test`) — future work
- **Real CA73 soft-reset model** — today `r100-cm7`'s `INTGR0 bit 0`
  handler writes a synthetic `0xFB0D` into `PF.ISSR[4]` to satisfy kmd's
  `rebel_hw_init → rebel_soft_reset → rebel_reset_done` loop, because
  REMU leaves the CA73 firmware running instead of physically resetting
  it the way silicon's PCIE_CM7 does. A faithful model would, on that
  doorbell, reset the CP0 / CP1 cluster CPUs + their GIC redistributor
  state + the PCIe mailbox regs (but preserve DRAM / SRAM / any kmd-
  loaded firmware images and cfg-head), then restart execution from
  `BL1` so `bootdone_task` naturally re-emits `0xFB0D` through the
  existing `issr` chardev path. Blocks the path from Phase 2 "functional
  enough for driver tests" → Phase 3 "truly end-to-end". Candidate
  hooks: a custom `DeviceReset` on an aggregated "r100-ca73-cluster"
  QOM wrapper, or `qemu_system_reset_request` with a fine-grained
  `ShutdownCause` so the x86 host QEMU is unaffected.

### Success criteria

- `insmod rebellions.ko` succeeds, device probes, BAR sizes match
- MSI-X vectors allocated, `FW_BOOT_DONE` handshake completes (M8b 3a ✓)
- `rebel_hw_init` / `rebel_queue_init` pass (M8b 3b ✓)
- `rbln_queue_test` completes (M8b 3c ✓ — BD-done MSI-X)
- umd opens device, creates context, submits simple job (M9)

## Phase 3: Full Inference

Real tensor ops on emulated DNCs.

- **DNC behavioral model** — parse task descriptors, execute on host CPU
- **HDMA scatter-gather** — full DMA with address translation
- **SMMU model** — honour FW page tables (today: bypass)
- **Performance counters** — synthetic cycle counts

## Timing Considerations

REMU is purely functional. Consequences:

- PLL / DMA / HBM training complete instantly
- FW time-out paths may never trigger
- Hardware-timing race conditions may not reproduce
- Performance counter values are meaningless
- FreeRTOS tick fires, but wall-clock ratios differ

Annotated delays can be added per-device if a future workload needs them.
