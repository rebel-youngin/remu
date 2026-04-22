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
./remucli build                     # builds QEMU (aarch64 + x86_64) with R100 device models
./remucli fw-build -p silicon       # builds q-sys FW (tf-a + cp0 + cp1) → images/
./remucli status                    # sanity-check environment
./remucli run --name my-test        # Phase 1: boot NPU only; logs land in output/my-test/
./remucli run --name dbg --gdb      # Phase 1: boot paused, wait for GDB on :1234
./remucli run --host --name pair    # Phase 2: launch NPU + x86 host QEMUs with r100-npu-pci bridge
./guest/build-guest-image.sh        # M8b Stage 2: stage images/x86_guest/{bzImage,initramfs.cpio.gz}
./guest/build-kmd.sh                # M8b Stage 2: rebuild rebellions.ko + rblnfs.ko against host kernel
```

`./remucli build` produces two QEMU binaries from one pinned source
tree: `build/qemu/qemu-system-aarch64` (NPU side, Phase 1 FW boot) and
`build/qemu/qemu-system-x86_64` (host side, Phase 2 kmd/umd guest). On
an existing aarch64-only build tree the configure step is re-run
automatically to pick up the x86_64 target — no `--clean` needed.

With `--host`, the x86 guest has our custom `r100-npu-pci` endpoint
attached (vendor/device `0x1eff:0x2030` matching the real CR03 silicon,
so the stock `rebellions.ko` binds with no changes). Its four BARs
match the driver's size checks in `rebel_check_pci_bars_size`:

| BAR | Size | Role |
|-----|------|------|
| 0   | 64 GB (>= 36 GB `RBLN_DRAM_SIZE`) | DDR — first 128 MB = `/dev/shm` shared file, tail = lazy RAM |
| 2   | 64 MB (= `RBLN_SRAM_SIZE`)        | ACP / SRAM / logbuf (lazy RAM) |
| 4   | 8 MB  (= `RBLN_PERI_SIZE`)        | Doorbells — 4 KB MMIO head intercepts `MAILBOX_INTGR{0,1}` writes (M6) and `MAILBOX_BASE` payload writes (M8a, host → NPU ISSR) and forwards them to the NPU as 8-byte chardev frames on the same `doorbell` socket; the `MAILBOX_BASE` range also acts as a read-through shadow of the NPU's ISSR state, fed by frames arriving on the `issr` chardev (M8a, NPU → host ISSR); rest is lazy RAM |
| 5   | 1 MB  (= `RBLN_PCIE_SIZE`)        | MSI-X table (32 vectors) + PBA — `msix_notify()` target for frames coming back from the NPU `r100-imsix` device (M7) |

Both QEMUs open the same 128 MB memory-backed file at
`/dev/shm/remu-<name>/remu-shm` with `share=on`. The **same** backend is
spliced into **two** places — one per QEMU process — via containers
that layer the shared file at offset 0 and lazy RAM over the tail:

- **x86 host QEMU** (M4) — aliased through the `r100-npu-pci` device's
  `memdev` link over **BAR0 offset 0**, so stores into
  BAR0[0..128MB) land in tmpfs pages.
- **NPU QEMU** (M5) — aliased through the `r100-soc` machine's
  `memdev` string property (resolved at machine-init time because
  `-object memory-backend-*` is processed after `-machine` options)
  over **chiplet 0 DRAM offset 0**, so CA73 loads/stores into
  chiplet-0 physical 0x0..0x07FFFFFF hit the same file. The tail
  (0x08000000..0x40000000) stays plain lazy RAM, so BL31_CP1 / FreeRTOS_CP1
  loaded at 0x14100000 / 0x14200000 remain private to the NPU.

The NPU QEMU's HMP monitor is exposed on a second unix socket at
`output/<name>/npu/monitor.sock` (on top of the existing stdio-muxed
readline monitor on uart0). SeaBIOS is allowed to run so the x86 BAR
addresses get programmed; by default the guest then idles at "No
bootable device" with serial redirected to `host/serial.log`. The
host-side HMP monitor lives at `output/<name>/host/monitor.sock` (use
`socat - UNIX-CONNECT:...` for interactive HMP).

**M8b Stage 2 (x86 Linux guest):** when the artifacts
`images/x86_guest/bzImage` and `images/x86_guest/initramfs.cpio.gz`
exist (staged by `./guest/build-guest-image.sh`), the same
`--host` invocation auto-adds `-kernel / -initrd` + a virtio-9p
`-fsdev local,path=guest/,mount_tag=remu` share and flips the x86
CPU to `-cpu max`. The guest then boots a real Ubuntu HWE kernel
with a busybox initramfs that mounts `guest/` at `/mnt/remu` and
runs `setup.sh` (which insmod's `rebellions.ko` and waits for the
kmd to print `FW_BOOT_DONE` in dmesg). Overrides:
`--guest-kernel`, `--guest-initrd`, `--guest-share`,
`--no-guest-boot`, `--guest-cmdline-extra`. `-cpu max` is required
because the stock kmd is built with `-march=native` and emits BMI2
instructions that trap `#UD` on the minimal `qemu64` CPU.

`./remucli run --host` auto-verifies every bridge end-to-end:
  - **M4**: `info pci` on host must list `1eff:2030` (logged to
    `host/info-pci.log`);
  - **M4**: `info mtree` on host must place the `remushm` subregion at
    offset 0 of `r100.bar0.ddr` (logged to `host/info-mtree.log`);
  - **M5**: `info mtree` on the NPU must place the same `remushm`
    subregion at offset 0 of `r100.chiplet0.dram` (logged to
    `npu/info-mtree.log`);
  - **M4/M5**: both QEMU PIDs must have the shm file in
    `/proc/<pid>/maps`;
  - **M6**: `info mtree` on host must show the `r100.bar4.mmio`
    priority-10 overlay, and `info qtree` on the NPU must list both
    `r100-doorbell` and `r100-mailbox` (logged to
    `host/info-mtree-bar4.log` + `npu/info-qtree.log`);
  - **M7**: `info mtree` on the NPU must list the `r100-imsix`
    MemoryRegion at `0x1BFFFFF000`, and `info mtree` on host must show
    `msix-table` + `msix-pba` overlaying `r100.bar5.msix` (logged to
    `npu/info-mtree-imsix.log` + `host/info-mtree-bar5.log`);
  - **M8a**: `info qtree` on the NPU must show the chiplet-0
    `r100-mailbox` with `issr-chardev = "issr"`, and `info qtree` on
    host must show `r100-npu-pci` with `issr = "issr"` (logged to
    `npu/info-qtree-issr.log` + `host/info-qtree-issr.log`).

Any check failing prints to the terminal — the NPU still boots for
post-mortem poking. End-to-end sanity scripts, one per milestone
bridge:
  - `tests/m5_dataflow_test.py` writes a magic pattern to
    `/dev/shm/.../remu-shm` at offset 0x07F00000 and asserts `xp /4wx`
    on both monitors returns the same bytes.
  - `tests/m6_doorbell_test.py` drives 8-byte `(offset, value)`
    frames on `host/doorbell.sock` and checks `doorbell.log` plus
    `GUEST_ERROR` entries for rejected frames.
  - `tests/m7_msix_test.py` does the reverse: connects as an NPU-
    impersonating client to `host/msix.sock` (test IS the NPU in this
    scenario), emits 5 frames (3 `ok` + 1 `oor` + 1 `bad-offset`),
    and checks `msix.log` plus `GUEST_ERROR` entries.
  - `tests/m8_issr_test.py` runs two phases against two minimal QEMUs:
    phase 1 acts as the NPU writing ISSRs (server on `issr.sock`) and
    verifies the host's BAR4 shadow mirrors them via `xp` over HMP;
    phase 2 acts as the host writing BAR4 (server on `doorbell.sock`)
    and verifies the NPU's mailbox ISSR registers update without any
    spurious GIC SPI assertion.

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

Manual ninja rebuild (skips the CLI's symlink / meson patch / `cli/qemu-patches/*.patch`
apply step — use only when `external/qemu` is already set up by a prior
`./remucli build`):
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
src/machine/          NPU-side QEMU device models (symlinked into external/qemu/hw/arm/r100/)
src/host/             Host-side (x86 guest) PCI device models (symlinked into external/qemu/hw/misc/r100-host/)
src/include/          Shared headers (remu_addrmap.h)
cli/remu_cli.py       Click-based CLI implementation (invoked by ./remucli)
tests/                Test binaries and test scripts
docs/                 Architecture, roadmap, debugging docs
external/             Read-only: ssw-bundle (q-sys, q-cp, kmd, umd, ...), qemu
guest/                M8b Stage 2: virtio-9p share mounted by the x86 Linux guest
                      (build-guest-image.sh, build-kmd.sh, setup.sh; rebellions.ko + rblnfs.ko are gitignored)
images/               FW binaries (bl1.bin, bl31_cp0.bin, freertos_cp0.bin, ...)
images/x86_guest/     M8b Stage 2: staged x86 guest kernel + initramfs (gitignored)
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

All FW/driver sources live inside `external/ssw-bundle` (initialized via
`git submodule update --init --recursive`). Shorthand: `$BUNDLE` below is
`external/ssw-bundle/products`.

When modifying device models, cross-reference these FW/driver sources:

| What to check | File |
|----------------|------|
| SoC address map | `$BUNDLE/rebel/q/sys/.../autogen/g_sys_addrmap.h` |
| Platform defs | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/include/platform_def.h` |
| CMU polling patterns | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/clk/clk_samsung/cmu.c` |
| PMU registers | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/plat/rebel/rebel_h/rebel_h_pmu.c` |
| QSPI bridge protocol | `$BUNDLE/rebel/q/sys/bootloader/cp/tf-a/drivers/synopsys/qspi_bridge/qspi_bridge.c` |
| PCI BAR layout | `$BUNDLE/common/kmd/rebellions/rebel/rebel.h` |
| FW-host handshake | `$BUNDLE/common/kmd/rebellions/common/{fw_if.c,ring.c,queue.c}` |
