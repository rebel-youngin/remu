# CLAUDE.md

## Project

REMU — R100 NPU System Emulator. QEMU-based functional emulator for the
R100 (CR03 quad, 4-chiplet) NPU SoC. Runs unmodified firmware (q-sys,
q-cp) and drivers (kmd, umd) on emulated hardware.

## Documentation

- [docs/architecture.md](docs/architecture.md) — System architecture, memory maps, device model design, FW source references
- [docs/roadmap.md](docs/roadmap.md) — Phase 1 (FW boot, complete), Phase 2 foundation milestones (M1..M9-1, complete) + the unified P1..P11 architectural plan that supersedes the old Phase 2 / Phase 3 boundary
- [docs/debugging.md](docs/debugging.md) — **How the agent should build / run / inspect logs / drive GDB.** Read this before running the emulator.

## Build

All commands go through `./remucli` at the repo root (bash wrapper
around `cli/remu_cli.py`). Only runtime dep: `pip install --user click`.

```
./remucli build                     # QEMU (aarch64 + x86_64) with R100 device models
./remucli fw-build                  # q-sys FW (tf-a + cp{0,1}) → images/ (silicon only)
./remucli status                    # environment sanity-check
./remucli run --name my-test        # Phase 1: NPU only → output/my-test/
./remucli run --name dbg --gdb      # Phase 1: paused, GDB on :1234
./remucli run --host --name pair    # Phase 2: NPU + x86 host QEMUs + r100-npu-pci bridge
./remucli test                      # M5/M6/M7/M8 bridge tests (`test m5 m7` for subset)
./remucli clean --name pair         # wipe orphan procs/shm/sockets from a SIGKILL'd run
./remucli clean --all               # nuke every REMU-shaped process + /dev/shm/remu-*
./guest/build-guest-image.sh        # M8b Stage 2: stage images/x86_guest/{bzImage,initramfs.cpio.gz}
./guest/build-kmd.sh                # M8b Stage 2: rebuild rebellions.ko + rblnfs.ko
```

`silicon` is the only FW profile exercised end-to-end. `zebu*` builds
but emits a deprecation warning and is not in regression.

`./remucli run` auto-runs `clean --name <name>` first, so SIGKILL'd
prior runs don't leave stale shm / sockets / QEMU mmaps. Concurrent
runs with different `--name` are untouched. `./remucli test` wraps the
same cleanup per-test.

`./remucli build` produces `build/qemu/qemu-system-{aarch64,x86_64}`
from one pinned source tree. Adding x86_64 on top of an aarch64-only
build tree re-runs configure automatically (no `--clean` needed).

### `--host` mode (Phase 2)

The x86 guest sees our `r100-npu-pci` endpoint (vendor/device
`0x1eff:0x2030`, matching real CR03 silicon — stock `rebellions.ko`
binds with no changes). BAR sizes match `rebel_check_pci_bars_size`:

| BAR | Size  | Role |
|-----|-------|------|
| 0   | 64 GB | DDR — first 128 MB = shm file, tail = lazy RAM |
| 2   | 64 MB | ACP / SRAM / logbuf; 4 KB prio-10 trap @ `FW_LOGBUF_SIZE` (M8b 3b cfg-head) forwards writes on `cfg` chardev, rest lazy RAM |
| 4   | 8 MB  | 4 KB MMIO head (INTGR M6 + MAILBOX_BASE M8a bidir + CM7-stub Stage 3a + BD-done queue-start doorbell Stage 3c); rest lazy RAM |
| 5   | 1 MB  | MSI-X table (32 vectors) + PBA (`msix_notify()` target for M7; Stage 3c BD-done completions) |

BAR4 details: `MAILBOX_INTGR{0,1}` / `MAILBOX_BASE` writes are
serialised as 8-byte chardev frames on the `doorbell` socket; reads in
`MAILBOX_BASE` range return the live shadow fed by `issr` chardev
frames (NPU → host). Cold-boot `FW_BOOT_DONE` is **real**: q-sys
`bootdone_task` writes `0xFB0D` to PF.ISSR[4] which egresses over the
`issr` chardev into the shadow (see `docs/debugging.md` →
"Post-mortems" for the GIC wiring fix that enabled this). `INTGR0`
bit 0 (`SOFT_RESET`) triggers a narrow QEMU-side CM7-stub that
*re-synthesises* `0xFB0D` on the kmd's post-probe soft-reset
handshake, because REMU does not yet model a real CA73 cluster reset
(`docs/roadmap.md` → Phase 2 pending: "real CA73 soft-reset").
`INTGR1` bit 7 (`QUEUE_INIT`) triggers the Stage 3b QINIT stub (see
below).

BAR2 details (M8b 3b): kmd writes `DDH_BASE_{LO,HI}` (at `FW_LOGBUF_SIZE
+ 0xC0/0xC4`) to publish the host-RAM `rbln_device_desc` to firmware.
Stage 3b widens this into a reusable **host → NPU configuration
mirror**: the host-side `r100-npu-pci` traps the 4 KB window and emits
8-byte `(cfg_off, val)` frames on the `cfg` chardev; the NPU-side
`r100-cm7` consumes them into a 1024-entry `cfg_shadow[]`. The
paired **NPU ↔ host DMA executor** is the `hdma` chardev with a 24 B
header + payload protocol (`src/bridge/remu_hdma_proto.h`). On `INTGR1`
bit 7 the NPU CM7 stub (`r100_cm7_qinit_stub`) reads `DDH_BASE`
from `cfg_shadow[]`, then emits two `HDMA_OP_WRITE` frames — one
for `fw_version = "3.remu-stub"`, one for `init_done = 1`. The host
decodes and runs `pci_dma_write` against the x86 guest DMA space.

Stage 3c turns the single-opcode `hdma` pipe into a bidirectional
protocol so the NPU can drive BD completion without a second chardev.
New opcodes: `OP_READ_REQ` (NPU → host: "please `pci_dma_read`
`len` bytes at `dst`, tag with `req_id`"), `OP_READ_RESP` (host →
NPU: payload for a pending `req_id`), and `OP_CFG_WRITE` (NPU → host:
"update host-local `cfg_mmio_regs[dst>>2]`"). The `flags` header byte
is renamed `req_id` — same wire layout, new semantics. `INTGR1` bits
`0..N-1` are the per-queue BD-done doorbells: the NPU-side `r100-cm7`
snapshots `ISSR[qid]` as `pi`, drives a per-queue `R100Cm7BdJob` state
machine (`IDLE → WAIT_QDESC → WAIT_BD → WAIT_PKT → IDLE` using
`OP_READ_REQ` / `OP_READ_RESP` pairs), commits via
`OP_CFG_WRITE FUNC_SCRATCH`, `OP_WRITE bd.header |= DONE`, and
`OP_WRITE queue_desc.ci++`, then fires `r100_imsix_notify(vec=qid)`
so the kmd's `dma_fence` resolves. See
`docs/debugging.md` → "Stage 3c — BD-done state machine" for the log
signatures.

In parallel with Stage 3c BD-done, every queue-doorbell also pushes a
24 B `dnc_one_task` entry onto the q-cp DNC compute task queue (real
`r100-mailbox` instance at `R100_PERI0_MAILBOX_M9_BASE`, chiplet 0).
q-cp's `taskmgr_fetch_dnc_task_master_cp1` polls `MBTQ_PI_IDX` (ISSR[0])
on this mailbox; the push wakes that loop without IRQ wiring (poll-based
protocol). PCIE_CM7 firmware does this on silicon — REMU has no
Cortex-M7 vCPU so `r100-cm7` takes the role via
`r100_mailbox_set_issr_words()` (in-process; no chardev). M9-1c
synthesises a minimal `cmd_descr` (cmd_type=COMPUTE,
`dnc_task_conf.core_affinity=BIT(0)`) into a chiplet-0 private DRAM
slot at `R100_CMD_DESCR_SYNTH_BASE = 0x20000000` (16 × 256 B ring) and
points the entry's `cmd_descr` field at it, so q-cp's worker can
dereference + reach `dnc_send_task` instead of NULL-deref'ing.
cm7-debug emits `mbtq qid=N slot=M pi=P cmd_descr=0x... status=ok`
traces; q-cp consumption is verified end-to-end as part of P2 (see
`docs/roadmap.md` — q-cp's `cb_complete` owns BD.DONE + MSI-X once
P1 lands the real CP1-side `pcie_msix_trigger`).

#### r100-hdma + active r100-dnc (M9-1c)

The `hdma` chardev moved from `r100-cm7` onto a new `r100-hdma` device
(QOM type `r100-hdma`, MMIO at `R100_HDMA_BASE = 0x1D80380000` on
chiplet 0). The motivation is that q-cp itself programs a DesignWare
dw_hdma_v0 register block at that address — REMU now models that
register block, and the chardev sits on the model's host-side
counterpart since CharBackends are single-frontend. r100-cm7 reaches
the chardev through a QOM link (`hdma` link prop) and the public emit
helpers in `src/machine/r100_hdma.h`. req_id partitioning on the wire
(documented in `src/bridge/remu_hdma_proto.h`):
0x00 = QINIT untagged, 0x01..0x0F = cm7 BD-done per qid+1,
0x80..0xBF = r100-hdma MMIO-driven channel ops.

The passive `r100-dnc-cluster` register-file stub (sparse regstore
seeding IP_INFO / SHM TPG / RDSN bits for boot) gained an active
task-completion path: writes to slot+0x81C (TASK_DESC_CFG1) with
`access_size=4` and `itdone=1` schedule a BH that pulses the matching
DNC GIC SPI (lookup table at `r100_dnc_intid()` in
`r100/remu_addrmap.h`; INTIDs are non-contiguous — DNC0=410,
DNC1=422…) and latches a synthesised `dnc_reg_done_passage` at
slot+0xA00 so q-cp's `dnc_X_done_handler` reads a coherent record.
GIC `num-irq` was bumped 256 → 992 so the wider DNC INTID set fits.

Both QEMUs `mmap` the same 128 MB `/dev/shm/remu-<name>/remu-shm` with
`share=on`. Splice points (same backend, two places):

- **x86 host** (M4) — `r100-npu-pci` `memdev` link over BAR0 offset 0.
- **NPU** (M5) — `r100-soc` machine's `memdev` string property (resolved
  at machine-init time, after `-object memory-backend-*`) over chiplet-0
  DRAM offset 0. Tail `0x08000000..0x40000000` stays private lazy RAM
  so BL31_CP1 / FreeRTOS_CP1 loaded at `0x14100000` / `0x14200000` don't
  leak to host.

HMP monitors: NPU on `output/<name>/npu/monitor.sock` (plus the
stdio-muxed readline monitor on uart0), host on
`output/<name>/host/monitor.sock`. By default SeaBIOS runs and the x86
guest idles at "No bootable device", serial to `host/serial.log`.

**M8b Stage 2 (x86 Linux guest):** with
`images/x86_guest/{bzImage,initramfs.cpio.gz}` staged (via
`./guest/build-guest-image.sh`), `--host` auto-adds `-kernel/-initrd`
+ virtio-9p share of `guest/` at `/mnt/remu` + `-cpu max` (stock kmd is
`-march=native` with BMI2 → `#UD` on minimal `qemu64`). The guest
boots Ubuntu HWE + busybox initramfs, runs `setup.sh` (insmods
`rebellions.ko` and watches for `FW_BOOT_DONE`). Overrides:
`--guest-kernel`, `--guest-initrd`, `--guest-share`, `--no-guest-boot`,
`--guest-cmdline-extra`. See commits `1ef7208` / `985fd58` for
artifact pipeline and GIC-bpr QEMU patch detail.

### Auto-verification on startup

Every `--host` run captures and checks:

- **M4/M5**: shm file in both `/proc/<pid>/maps`; `remushm` subregion
  at offset 0 of both `r100.bar0.ddr` (host) and `r100.chiplet0.dram`
  (NPU); host `info pci` lists `1eff:2030`.
- **M6**: host shows `r100.bar4.mmio` prio-10 overlay; NPU `info qtree`
  lists `r100-cm7` + `r100-mailbox`.
- **M7**: NPU lists `r100-imsix` @ `0x1BFFFFF000`; host shows
  `msix-{table,pba}` overlaying `r100.bar5.msix`.
- **M8a**: NPU `r100-mailbox` has `issr-chardev = "issr"`; host
  `r100-npu-pci` has `issr = "issr"`.
- **M8b 3b/3c + M9-1c**: NPU `r100-cm7` has `cfg-chardev = "cfg"` +
  `cm7-debug-chardev = "cm7_dbg"`; NPU `r100-hdma` exists with
  `chardev = "hdma"` (M9-1c moved the chardev off cm7 onto the new
  device); host `r100-npu-pci` has `cfg = "cfg"` + `hdma = "hdma"`
  (logged to `{host,npu}/info-qtree-cfg-hdma.log`).

All results go to `host/info-*.log` + `npu/info-*.log`. Failures print
to stdout (non-fatal — NPU still boots for post-mortem poking).

### End-to-end bridge tests

Driven by `./remucli test` (or individually). Each has pre-run
cleanup so SIGKILL'd prior state never poisons the next:

- `tests/m5_dataflow_test.py` — write magic to shm @ `0x07F00000`,
  `xp /4wx` on both monitors must agree.
- `tests/m6_doorbell_test.py` — drive 8-byte frames on
  `host/doorbell.sock`, check `doorbell.log` + `GUEST_ERROR`.
- `tests/m7_msix_test.py` — reverse: test impersonates NPU on
  `host/msix.sock`, emits 5 frames (3 ok + 1 oor + 1 bad-offset),
  checks `msix.log` + `GUEST_ERROR`.
- `tests/m8_issr_test.py` — two phases against two minimal QEMUs: NPU
  writes ISSR → host BAR4 shadow mirror via `xp`; host writes BAR4 →
  NPU mailbox ISSR update without spurious GIC SPI.

All `./remucli run` invocations write into `output/<name>/` (or
`output/run-<timestamp>/` if `--name` omitted). Never pass `/tmp/`
paths — stick to per-run directory. See `docs/debugging.md` for the
full agent loop.

### Shell completion (optional)

```
eval "$(./remucli completion bash)"   # or: completion zsh / completion fish
```

Persistent: add to `~/.bashrc` with an absolute path; alias or `PATH`
so bare `remucli` tab-completes.

### Manual ninja rebuild

Skips the CLI's symlink / meson-patch / `cli/qemu-patches/*.patch`
apply step — use only when `external/qemu` is already set up by a
prior `./remucli build`:

```
cd build/qemu && ninja -j$(nproc)
```

### `fw-build` internals

Wraps `external/ssw-bundle/.../q/sys/build.sh`. Components repeatable
(`-c tf-a -c cp0 -c cp1`, default = CA73 boot set); `--install` (on by
default) copies 6 CA73 binaries into `images/`. Toolchains from
`COMPILER_PATH_{ARM64,ARM32}` env vars (defaults under
`/mnt/data/tools/...`).

Before `build.sh`, `fw-build` idempotently applies every
`cli/fw-patches/*.patch` (forward/reverse-check dance, same as
`cli/qemu-patches/`). **Policy: `cli/fw-patches/` is empty.** The q-sys
submodule stays byte-identical to upstream; any unmodelled hardware
that would hang or `-EBUSY` boot gets a `src/machine/` or `src/host/`
QEMU stub (however minimal), never an `#ifdef` in firmware. The
plumbing stays so developers can drop a **local, uncommitted** debug
patch while bisecting — see `cli/fw-patches/README.md`. Runtime-side
example of this policy: the CM7-stub in `src/machine/r100_cm7.c`
(commit `a01d2b5`) — see BAR4 row above.

## Project layout

```
remucli               Bash wrapper — the one entry point
src/machine/          NPU-side QEMU device models (symlinked into external/qemu/hw/arm/r100/)
                        r100_soc.{c,h}      machine + QOM type-name registry
                        r100_mailbox.{c,h}  mailbox — .c private state, .h public helpers
                        r100_hdma.{c,h}     HDMA reg block — .c private state,
                                            .h public emit API (M9-1c)
                        r100_<dev>.c        one file per device (state struct private)
src/host/             Host-side (x86 guest) PCI device models (symlinked into external/qemu/hw/misc/r100-host/)
src/include/          Added to -I during QEMU configure
                        r100/remu_addrmap.h — `#include "r100/remu_addrmap.h"`
src/bridge/           Added to -I during QEMU configure — cross-side shared headers
                        remu_frame.h          8-byte frame codec (RX accumulator + emit)
                        remu_doorbell_proto.h BAR4 offset classifier + BAR2 cfg-head layout
                        remu_hdma_proto.h     bidirectional HDMA protocol (24 B header + payload,
                                              OP_WRITE / READ_REQ / READ_RESP / CFG_WRITE)
                      Header-only `static inline`, so both host-side (system_ss)
                      and NPU-side (arm_ss) TUs pick up the same definitions
                      without introducing a shared object.
cli/remu_cli.py       Click-based CLI implementation
tests/                Test binaries and test scripts
docs/                 Architecture, roadmap, debugging
external/             Read-only: ssw-bundle (q-sys, q-cp, kmd, umd, ...), qemu
guest/                M8b Stage 2 virtio-9p share (rebellions.ko / rblnfs.ko gitignored)
images/               FW binaries (bl1.bin, bl31_cp0.bin, freertos_cp0.bin, ...)
images/x86_guest/     M8b Stage 2 staged x86 guest kernel + initramfs (gitignored)
build/qemu/           QEMU build output (gitignored)
output/               Per-run log directories (gitignored; see docs/debugging.md)
```

## Code style

- QEMU conventions: 4-space indent, snake_case, QOM type system
- `device_class_set_legacy_reset()` (not `dc->reset`) for QEMU 9.2.0+
- `qdev_prop_set_array()` + `QList` for array properties (e.g. GIC `redist-region-count`)
- Machine type names must end with `-machine` (QEMU assertion)
- All remu source uses `r100_` prefix; external repos keep their own naming
- **Header discipline**: per-device `struct R100XxxState` and `DECLARE_INSTANCE_CHECKER(...)`
  live in the device's `.c` file, not in `r100_soc.h`. Cross-device access goes through
  QOM `link<>` properties or a small public-API header (see `r100_mailbox.h`), never
  by dereferencing another device's state.
- **Logging**: use `qemu_log_mask(LOG_{TRACE,UNIMP,GUEST_ERROR}, ...)` in hot paths, never
  `fprintf(stderr, ...)` — it bypasses the `-d`/`qemu.log` knobs and pollutes terminals.
- **Inter-QEMU wire format**: use `remu_frame_emit` / `remu_frame_rx_feed` from
  `src/bridge/remu_frame.h` for any 8-byte `(a, b)` chardev channel; don't hand-roll
  byte-swap + short-write bookkeeping inline.
- **BAR4 doorbell offsets**: classify via `remu_doorbell_classify()` from
  `src/bridge/remu_doorbell_proto.h`; don't add ad-hoc `off == 0x8 || off == 0x1c` checks
  in new code — the wire protocol must stay single-sourced across host + NPU.

## Hardware context

- **Target**: CR03 quad (`PCI_ID 0x2030`, `ASIC_REBEL_QUAD`, 4 chiplets)
- **CPU**: 8× CA73 per chiplet (2 clusters × 4 cores) = 32 total
- **CHIPLET_OFFSET**: `0x2000000000` between chiplets
- **Boot**: TF-A BL1 → BL2 → BL31 → FreeRTOS (q-sys), then q-cp tasks
- **UART**: PL011 @ `0x1FF9040000` (PERI0_UART0), 250 MHz
- **GIC**: GICv3 dist @ `0x1FF3800000`, redist @ `0x1FF3840000`
- **Timer**: 500 MHz (`CORE_TIMER_FREQ`)

## Key external files

Cross-references for FW/driver sources — `$BUNDLE` = `external/ssw-bundle/products`:

| What to check | File |
|---|---|
| SoC address map | `$BUNDLE/rebel/q/sys/.../autogen/g_sys_addrmap.h` |
| Platform defs | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/include/platform_def.h` |
| CMU polling | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/clk/clk_samsung/cmu.c` |
| PMU registers | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_pmu.c` |
| QSPI bridge | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/synopsys/qspi_bridge/qspi_bridge.c` |
| PCI BAR layout | `$BUNDLE/common/kmd/rebellions/rebel/rebel.h` |
| FW-host handshake | `$BUNDLE/common/kmd/rebellions/common/{fw_if.c,ring.c,queue.c}` |
