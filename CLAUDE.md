# CLAUDE.md

> Quick-reference for working in this repo. Device-model internals,
> per-chiplet memory map, and shared-backend layout live in
> [`docs/architecture.md`](docs/architecture.md) (especially the Source
> File Map). Per-fix history lives in `git log` and
> [`docs/roadmap.md`](docs/roadmap.md). This file does not narrate
> "X used to be Y".

## Project

REMU — R100 NPU System Emulator. QEMU-based functional emulator for the
R100 (CR03 quad, 4-chiplet) NPU SoC. Runs unmodified firmware (q-sys,
q-cp) and drivers (kmd, umd) on emulated hardware.

## Documentation

- [docs/architecture.md](docs/architecture.md) — System architecture, memory maps, device model design, FW source references
- [docs/roadmap.md](docs/roadmap.md) — Phase 1 / Phase 2 milestone log + the unified P1..P11 architectural plan
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
./remucli test                      # M5/M6/M7/M8/P4A/P4B/P5/P11 bridge tests (`test m5 p5` for subset)
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
| 0   | 64 GB | DDR splice over `remu-shm` (36 GB) + lazy tail |
| 2   | 64 MB | ACP / SRAM / logbuf; 4 KB cfg-head alias over `cfg-shadow` |
| 4   | 8 MB  | 4 KB MMIO head: INTGR + MAILBOX_BASE shadow + queue doorbell |
| 5   | 1 MB  | MSI-X table (32 vectors) + PBA |

All four BARs, the chardev wire format, the shared-memory backends
(`remu-shm` / `host-ram` / `cfg-shadow`), and per-device behaviour
(SMMU, HDMA, RBDMA, CM7 mailbox stub, PCIe outbound iATU, etc.) are
documented in `docs/architecture.md` → "Shared Memory Backends" +
"Source File Map". The high-level invariant: **q-cp on CA73 CP0 owns
the BD lifecycle natively** — queue-doorbell `INTGR1` bits relay as
SPI 185 to wake `hq_task → cb_task → cb_complete`, which walks
BD/packet over the PCIe outbound iATU, writes back through the
cfg-mirror, and fires MSI-X via `pcie_msix_trigger` →
`r100-imsix` MMIO. There is no QEMU-side BD walker on the honest
path; the only behavioural lie remaining is `r100-cm7`'s synthetic
`FW_BOOT_DONE` re-handshake on `INTGR0 bit 0` (gated until P8 lands
real CA73 cluster soft-reset; see roadmap).

### Auto-verification on startup

Every `--host` run captures and checks (logs to
`{host,npu}/info-*.log`; failures non-fatal):

- **M4/M5**: shm file in both `/proc/<pid>/maps`; `remushm` subregion
  at offset 0 of both `r100.bar0.ddr` (host) and `r100.chiplet0.dram`
  (NPU); host `info pci` lists `1eff:2030`.
- **M6**: host shows `r100.bar4.mmio` prio-10 overlay; NPU `info qtree`
  lists `r100-cm7` + `r100-mailbox`.
- **M7**: NPU lists `r100-imsix` @ `0x1BFFFFF000`; host shows
  `msix-{table,pba}` overlaying `r100.bar5.msix`.
- **M8a**: NPU `r100-mailbox` has `issr-chardev = "issr"`; host
  `r100-npu-pci` has `issr = "issr"`.
- **hdma**: NPU `r100-hdma` exists with `chardev = "hdma"`; host
  `r100-npu-pci` has `hdma = "hdma"`.

### End-to-end bridge tests

Driven by `./remucli test` (or individually). Each has pre-run
cleanup. Most p-series tests use the same shm-splice + on-demand
gdbstub harness (HMP `gdbserver tcp::PORT` + `Qqemu.PhyMemMode:1` +
`M<addr>,4:<hex>` packets) so they exercise the kick path without
booting the full umd/kmd/x86 stack.

| Test | Coverage |
|---|---|
| `m5_dataflow_test.py` | shm splice — write magic, both monitors agree |
| `m6_doorbell_test.py` | host → NPU 8-byte frames on `doorbell.sock` |
| `m7_msix_test.py` | NPU → host MSI-X frames on `msix.sock` |
| `m8_issr_test.py` | bidirectional ISSR mirror, no spurious GIC SPI |
| `p4a_rbdma_stub_test.py` | RBDMA IP_INFO seeds + idle queue depth |
| `p4b_rbdma_oto_test.py` | RBDMA OTO 4 KB byte-mover via `RUN_CONF1` |
| `p5_hdma_ll_test.py` | HDMA 1-LLI D2D walk (pre-`SMMUEN`) |
| `p11_smmu_walk_test.py` | hand-staged 3-level S2 PT + STE → 4 KB OTO |
| `p11b_smmu_evtq_test.py` | invalid STE → eventq emit + GIC SPI 762 |

Every `./remucli run` invocation requires `--name <run_name>`; all
artifacts (UART logs, monitor sockets, info dumps, the captured QEMU
stdout, and — when launched via `./remucli test` — the orchestrator's
`test.log`) land under `output/<run_name>/`. Run names must be
explicit so `clean --name <run_name>` can scope teardown precisely
and concurrent runs with different names never collide. Never pass
`/tmp/` paths. See `docs/debugging.md` for the full agent loop.

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
patch while bisecting — see `cli/fw-patches/README.md`.

## Project layout

```
remucli               Bash wrapper — the one entry point
src/machine/          NPU-side QEMU device models (symlinked into external/qemu/hw/arm/r100/)
src/host/             Host-side (x86 guest) PCI device models (symlinked into external/qemu/hw/misc/r100-host/)
src/include/r100/     Address constants, BAR layout, INTID tables, req_id partitions
src/bridge/           Cross-side header-only `static inline` (frame codec, doorbell classifier, hdma proto)
cli/remu_cli.py       Click-based CLI implementation
tests/                Bridge tests + p-series engine tests; `tests/scripts/` has gdb + shm-decode helpers
docs/                 Architecture, roadmap, debugging
external/             Read-only: ssw-bundle (q-sys, q-cp, kmd, umd, ...), qemu
guest/                M8b Stage 2 virtio-9p share (rebellions.ko / rblnfs.ko gitignored)
images/               FW binaries; images/x86_guest/ for staged kernel + initramfs (gitignored)
build/qemu/           QEMU build output (gitignored)
output/               Per-run log directories (gitignored; see docs/debugging.md)
```

Per-file device behaviour: `docs/architecture.md` → "Source File Map".

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
