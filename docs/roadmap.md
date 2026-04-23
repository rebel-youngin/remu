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
| M6 | BAR4 INTGR → `doorbell` chardev → `r100-doorbell` → `r100-mailbox` → chiplet-0 GIC SPI 184/185 | done | `85b76bb`, `500856b` |
| M7 | `r100-imsix` MMIO trap at `0x1BFFFFF000 + 0xFFC` → `msix` chardev → `msix_notify()` | done | `db3d1df` |
| M8a | ISSR bridge both dirs (NPU→host on new `issr` chardev, host→NPU on same `doorbell` wire, offset-disambiguated) | done | `cd24aa9` |
| M8b | `FW_BOOT_DONE` + `TEST_IB` ring on top of M8a | in progress (3a done, 3b active) | see sub-table |
| M9  | DNC stub + trivial umd job | pending | — |

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
| 3a | KMD soft-reset handshake: `r100-doorbell` CM7-stub synthesises `FW_BOOT_DONE` → PF.ISSR[4] in QEMU, bypassing unmodelled PCIE_CM7 | done | `a01d2b5` |
| 3b | `rebel_hw_init` soft-lockup at t+25 s — decode the next unmodelled FW wait-state (likely `TEST_IB` ring or another ISSR poll) and either wire it on FW side or synthesise in QEMU | in progress | — |

### Components (current state)

Device models + chardev bridges — see `docs/architecture.md`
"Source File Map" for per-file behaviour, and `docs/debugging.md`
for the HMP sanity recipes.

- `src/host/r100_npu_pci.c` — x86-side endpoint. BAR0 splice, BAR2 lazy RAM,
  BAR4 container with 4 KB MMIO head (INTGR + MAILBOX_BASE shadow),
  BAR5 MSI-X + RAM fill.
- `src/machine/r100_soc.c` — machine. Splices memdev into chiplet-0 DRAM,
  instantiates two PF/VF0 `r100-mailbox` blocks, optional `r100-doorbell`
  / `r100-imsix` gated on chardev machine-props.
- `src/machine/r100_mailbox.c` — Samsung IPM SFR (`INTGR/INTCR/INTMR/INTSR/INTMSR/ISSR0..63`),
  two `qemu_irq` outs tracking INTMSR. API: `r100_mailbox_raise_intgr`,
  `r100_mailbox_set_issr`, `r100_mailbox_cm7_stub_write_issr`.
- `src/machine/r100_doorbell.c` — reassembles 8-byte `(offset, value)` frames,
  routes by offset into mailbox INTGR / ISSR / CM7-stub.
- `src/machine/r100_imsix.c` — 4 KB MMIO trap at `R100_PCIE_IMSIX_BASE`,
  emits `(offset, db_data)` frames on write to `0xFFC`.
- Three Unix-socket chardevs under `output/<name>/host/`: `doorbell.sock`
  (M6+M8a, host → NPU), `msix.sock` (M7, NPU → host), `issr.sock`
  (M8a, NPU → host).
- `./remucli run --host` orchestrates both QEMUs + auto-verifies bridges;
  M8b Stage 2 adds optional `-kernel/-initrd/-fsdev virtio-9p` wiring when
  `images/x86_guest/{bzImage,initramfs.cpio.gz}` are staged.

### Pending components

- DNC stub (accept task, generate completion MSI-X) — M9
- HDMA stub (actual memcpy between DRAM regions) — M9

### Success criteria

- `insmod rebellions.ko` succeeds, device probes, BAR sizes match
- MSI-X vectors allocated, `FW_BOOT_DONE` handshake completes (M8b 3a ✓)
- `TEST_IB` ring passes (M8b 3b)
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
