# CLAUDE.md

## Project

REMU — R100 NPU System Emulator. QEMU-based functional emulator for the R100 (CR03 quad, 4-chiplet) NPU SoC. Runs unmodified firmware (q-sys, q-cp) and drivers (kmd, umd) on emulated hardware.

## Documentation

- [docs/architecture.md](docs/architecture.md) — System architecture, memory maps, device model design, FW source references
- [docs/roadmap.md](docs/roadmap.md) — Phased implementation plan (Phase 1: FW boot, Phase 2: host drivers, Phase 3: inference)
- [docs/debugging.md](docs/debugging.md) — **How the agent should build / run / inspect logs / drive GDB.** Read this before running the emulator.

## Build

All commands go through `./remucli` at the repo root — a thin bash
wrapper around `cli/remu_cli.py`. **No install step** — just make sure
`click` is available (`pip install --user click`).

```
./remucli build                     # builds QEMU with R100 device models
./remucli fw-build -p silicon       # builds q-sys FW (tf-a + cp0 + cp1) → images/
./remucli status                    # sanity-check environment
./remucli run --name my-test        # boot; logs land in output/my-test/
./remucli run --name dbg --gdb      # boot paused, wait for GDB on :1234
```

All `./remucli run` invocations write into `output/<name>/` (or
`output/run-<timestamp>/` if `--name` is omitted). Never pass paths
under `/tmp/` — stick with the per-run output directory so multiple
runs don't clobber each other. See `docs/debugging.md` for the full
agent loop, log interpretation, and GDB workflow.

Shell tab-completion (optional, but handy):

```
eval "$(./remucli completion bash)"   # or: completion zsh / completion fish
```

For persistent completion, add the eval to `~/.bashrc` with the
absolute path to `remucli`, and either put the repo root on PATH or
`alias remucli=/abs/path/to/remucli` so bare `remucli` tab-completes too.

Manual ninja rebuild (skips the CLI's symlink / meson patch step):
```
cd build/qemu && ninja -j$(nproc)
```

`./remucli fw-build` wraps `external/ssw-bundle/.../q/sys/build.sh`. Components
are repeatable (`-c tf-a -c cp0 -c cp1`, default covers the CA73 boot set);
platform is `silicon` / `zebu` / `zebu_ci` / `zebu_vdk`; `--install` (default
on) copies the 6 CA73 binaries into `images/`. Toolchains are taken from
`COMPILER_PATH_ARM64` / `COMPILER_PATH_ARM32` env vars, with defaults under
`/mnt/data/tools/...`.

## Project layout

```
remucli               Bash wrapper — the one entry point (`./remucli <cmd>`)
src/machine/          QEMU device models (symlinked into external/qemu/hw/arm/r100/)
src/include/          Shared headers (remu_addrmap.h)
cli/remu_cli.py       Click-based CLI implementation (invoked by ./remucli)
tests/                Test binaries and test scripts
docs/                 Architecture, roadmap, debugging docs
external/             Read-only: ssw-bundle (q-sys, q-cp, kmd, umd, ...), qemu
images/               FW binaries (bl1.bin, bl31_cp0.bin, freertos_cp0.bin, ...)
build/qemu/           QEMU build output (gitignored)
output/               Per-run log directories (gitignored; see docs/debugging.md)
```

## Code style

- Device models follow QEMU coding conventions: 4-space indent, snake_case, QOM type system
- Use `device_class_set_legacy_reset()` (not `dc->reset`) for QEMU 9.2.0+
- Use `qdev_prop_set_array()` with `QList` for array properties (e.g., GIC `redist-region-count`)
- Machine type names must end with `-machine` suffix (QEMU assertion requirement)
- All remu source uses `r100_` prefix; external repos keep their own naming (`rebel_h_*` in q-sys)

## Hardware context

- **Target**: CR03 quad (`PCI_ID 0x2030`, `ASIC_REBEL_QUAD`, 4 chiplets)
- **CPU**: 8x CA73 per chiplet (2 clusters x 4 cores) = 32 total
- **CHIPLET_OFFSET**: `0x2000000000` between chiplets
- **Boot**: TF-A BL1 → BL2 → BL31 → FreeRTOS (q-sys), then q-cp tasks
- **FW build**: `./remucli fw-build -p silicon` (default). The zebu / zebu_ci / zebu_vdk profiles are still accepted by `-p <name>` but are deprecated — see `docs/roadmap.md` Phase 1 Status
- **UART**: PL011 at `0x1FF9040000` (PERI0_UART0), 250MHz clock
- **GIC**: GICv3 distributor at `0x1FF3800000`, redistributor at `0x1FF3840000`
- **Timer**: 500MHz generic timer (`CORE_TIMER_FREQ`)

## Key external files

When modifying device models, cross-reference these FW/driver sources:

| What to check | File |
|----------------|------|
All FW/driver sources live inside `external/ssw-bundle` (initialized via
`git submodule update --init --recursive`). Shorthand: `$BUNDLE` below is
`external/ssw-bundle/products`.

| What to check | File |
|----------------|------|
| SoC address map | `$BUNDLE/rebel/q/sys/.../autogen/g_sys_addrmap.h` |
| Platform defs | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/include/platform_def.h` |
| CMU polling patterns | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/clk/clk_samsung/cmu.c` |
| PMU registers | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_pmu.c` |
| QSPI bridge protocol | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/synopsys/qspi_bridge/qspi_bridge.c` |
| PCI BAR layout | `$BUNDLE/common/kmd/rebellions/rebel/rebel.h` |
| FW-host handshake | `$BUNDLE/common/kmd/rebellions/common/{fw_if.c,ring.c,queue.c}` |
