# Testing & Debugging

This doc describes how an agent (or human) should drive REMU end-to-end:
build QEMU, build firmware, run the emulator in a reproducible output
directory, and inspect the result. It is intentionally prescriptive.

## Prerequisites

```
git submodule update --init --recursive         # ssw-bundle + external/qemu
pip install --user click                        # only runtime Python dep
```

No install step for the CLI itself — everything runs through the
`./remucli` wrapper at the repo root.

One-time environment sanity check (finds the QEMU source, lists device
models, reports which firmware images are present):

```
./remucli status
```

### Shell tab-completion (optional)

```
eval "$(./remucli completion bash)"   # or: completion zsh / completion fish
```

Persistent: add the same line to `~/.bashrc` with an absolute path, and
either put the repo root on PATH or `alias remucli=/abs/path/to/remucli`
so bare `remucli` tab-completes. The completion script hooks the
command name `remucli`, not `./remucli`.

## Build

QEMU + R100 device models:

```
./remucli build                  # incremental
./remucli build --clean          # wipe build/qemu/ and re-link sources
```

Firmware (CA73 boot set — `tf-a` + `cp0` + `cp1`, installed into `images/`):

```
./remucli fw-build -p silicon
```

Cross-reference the output with `./remucli status` — all six CA73
binaries (`bl1.bin`, `bl2.bin`, `bl31_cp{0,1}.bin`,
`freertos_cp{0,1}.bin`) must be present for a clean boot.

Manual ninja rebuild (skips the symlink / meson patching step the CLI
performs on first build):

```
cd build/qemu && ninja -j$(nproc)
```

## Run — output layout

Every `./remucli run` invocation writes into `output/<name>/`. If `--name`
is omitted, `<name>` defaults to `run-<YYYYmmdd-HHMMSS>`. The
`output/latest` symlink always points at the most recent run.

```
output/
  my-test/
    cmdline.txt     # exact QEMU invocation (newline-split, for eyeballing)
    uart0.log       # chiplet 0 UART — also muxed to stdio + QEMU monitor
    uart1.log       # chiplet 1 UART
    uart2.log       # chiplet 2 UART
    uart3.log       # chiplet 3 UART
    hils.log        # FreeRTOS HILS ring tail (RLOG_*/FLOG_* from DRAM 0x10000000)
    qemu.log        # present only when --trace is passed
  latest -> my-test
```

Re-using the same `--name` overwrites the log files in place. Nothing
lands in `/tmp`.

### Output layout with `--host` (Phase 2)

`./remucli run --host` spawns both QEMUs (aarch64 NPU + x86_64 host
guest) tied to the same shared-memory file. The run directory gains
two subdirs and a `shm` symlink:

```
output/
  my-test/
    cmdline.txt       # NPU QEMU invocation
    uart{0..3}.log    # NPU UARTs (as in Phase 1)
    hils.log          # NPU HILS ring tail
    shm -> /dev/shm/remu-my-test/
    doorbell.log      # NPU-side ASCII trace of every (offset, value) frame
    npu/
      monitor.sock    # NPU HMP monitor (unix socket)
      info-mtree.log  # captured info mtree — chiplet0.dram splice
      info-qtree.log  # captured info qtree — confirms r100-doorbell + r100-mailbox
    host/
      cmdline.txt     # x86 QEMU invocation
      qemu.stdout.log
      qemu.stderr.log
      serial.log      # x86 guest serial (SeaBIOS + "No bootable device" idle)
      monitor.sock    # host HMP monitor (unix socket)
      doorbell.sock   # unix-socket chardev listener (host = server, NPU = client)
      info-pci.log    # captured info pci — lists 1eff:2030 endpoint
      info-mtree.log  # captured info mtree — BAR0 shm splice
      info-mtree-bar4.log  # captured info mtree — BAR4 MMIO overlay (M6)
```

Everything under `/dev/shm/remu-<name>/` is cleaned up on exit. If
`./remucli run --host` is killed `SIGKILL`-style and leaves an orphan
behind, `rm -rf /dev/shm/remu-<name>` plus `pkill -f qemu-system-` is
the manual recovery.

## Recommended agent workflow

The standard loop when iterating on device models is:

1. Edit `src/machine/r100_*.c`.
2. `./remucli build` (rebuild QEMU with the new device logic).
3. Run the emulator **in a background terminal** so you can keep
   inspecting files while it's alive:

   ```
   ./remucli run --name iter-N
   ```

   Under the Cursor Shell tool, pass `is_background: true`. The process
   keeps running; QEMU replaces the Python process via `execvp`, so
   signals and `kill` target QEMU directly.

4. Poll the log files (they grow line-by-line in real time):

   ```
   rg -n 'BL1:|BL2:|BL31:|Hello world|REBELLIONS\$|panic|plat_panic_handler|guest_error|unimp' \
      output/iter-N/uart0.log output/iter-N/hils.log
   ```

5. When you're done (or when the boot stalls and stops emitting new
   output), stop QEMU. From the same terminal: `Ctrl-A X`. From another
   terminal / the agent: `pkill -f qemu-system-aarch64` (or kill the
   specific PID).

6. If you need a fresh run, pick a new `--name` or accept the default
   timestamped directory — old runs stay around under `output/` for
   comparison and are gitignored.

## GDB workflow

Two-terminal pattern:

- Terminal 1 — start QEMU paused, waiting for GDB:

  ```
  ./remucli run --name gdb-iter --gdb
  ```

- Terminal 2 — attach a cross GDB with debug symbols:

  ```
  ./remucli gdb -b external/ssw-bundle/products/rebel/q/sys/binaries/BootLoader_CP/bl31.elf
  ```

  `./remucli gdb` prefers `gdb-multiarch`, falling back to
  `aarch64-linux-gnu-gdb`. For FW ELFs built from `q-sys` the
  `aarch64-none-elf-gdb` bundled with the 13.3.rel1 toolchain is known
  to work (the vendor build has a broken Python path).

All 32 vCPUs are visible as GDB threads once GDB connects
(`info threads`). Threads are laid out as chiplet × cluster × core:

| Thread | Chiplet | Cluster | Core |
|--------|---------|---------|------|
| 1..4   | 0 | CP0 | 0..3 |
| 5..8   | 0 | CP1 | 0..3 |
| 9..12  | 1 | CP0 | 0..3 |
| …      | … | …   | …    |
| 29..32 | 3 | CP1 | 0..3 |

Useful inspection once attached to a halted thread:

```
thread N
frame 0
info registers
x/8i $pc
x/16wx $sp
p/x $ELR_EL3
p/x $ESR_EL3
p/x $FAR_EL3
```

### Scripted CP1 sweep across all chiplets

`tests/scripts/gdb_inspect_cp1.gdb` is a pure-GDB (no Python) batch
script that dumps `info threads` plus per-thread frame 0 + `ELR_EL3`
for every CP1 vCPU (threads 5-8, 13-16, 21-24, 29-32). Useful for
confirming each chiplet's CP1 half is running FreeRTOS steady-state
code rather than spinning in PSCI wait or faulted. Two-step workflow:

1. Start QEMU **with gdbstub but not paused** — current `./remucli
   run --gdb` bundles `-s -S`, which stops the VM before FW boot. To
   let FW boot for ~60 s first, cd into `output/<name>/cmdline.txt`
   and replay it by hand with `-S` dropped (keep `-s`). Then leave
   it running in the background.

2. After ~60 s of wall clock, attach GDB in batch mode — connect
   will interrupt the running inferior, the script dumps all 16 CP1
   threads, and detach leaves QEMU resumable:

   ```
   aarch64-none-elf-gdb -batch \
     -ex 'set pagination off' \
     -ex 'file external/ssw-bundle/products/rebel/q/sys/binaries/FreeRTOS_CP1/freertos_kernel.elf' \
     -ex 'add-symbol-file external/ssw-bundle/products/rebel/q/sys/binaries/FreeRTOS_CP1/bl31.elf' \
     -ex 'target remote :1234' \
     -x tests/scripts/gdb_inspect_cp1.gdb
   ```

   Healthy steady-state layout (per chiplet): `CP1.cpu0` in
   `ipm_samsung_receive`, `CP1.cpu{1,2,3}` in
   `taskmgr_fetch_dnc_task_worker_cp1` for DNC ranges 0-5 / 6-10 /
   11-15.

## Trace mode

`--trace` turns on QEMU's `guest_errors,unimp` logs (missed MMIO regions,
CPU trapped instructions). The log goes to `output/<name>/qemu.log`
rather than stderr so it's persisted alongside the UART logs.

```
./remucli run --name trace --trace
rg -n 'unassigned|unimp|guest_error' output/trace/qemu.log
```

The `cfg_mr` / `private_alias` catch-alls in `r100_soc.c` absorb most
undefined accesses silently; anything surfacing in `qemu.log` means the
miss fell outside even those regions and is genuinely unmapped.

## QEMU monitor (address-map debugging)

Chiplet 0's UART is muxed with the QEMU monitor on stdio. Press
`Ctrl-A C` to toggle between the UART console and `(qemu)`:

```
(qemu) info mtree -f     # flattened memory tree — confirms per-chiplet aliases
(qemu) info qtree        # device tree
(qemu) stop              # pause all vCPUs
(qemu) cont              # resume
(qemu) info registers -a # every CPU
```

### Unix-socket monitors with `--host`

When Phase 2's `--host` is in play, the stdio-muxed monitor is still
live on the NPU console, *and* both QEMUs expose a second monitor over
a unix socket. Use `socat` interactively, or `nc -U` for one-shot
queries:

```
# interactive
socat - UNIX-CONNECT:output/my-test/npu/monitor.sock
socat - UNIX-CONNECT:output/my-test/host/monitor.sock

# one-shot, read back the shared splice
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/my-test/npu/monitor.sock  | less
printf 'xp /4wx 0x07f00000\nquit\n' | socat - UNIX-CONNECT:output/my-test/npu/monitor.sock
printf 'xp /4wx 0xe007f00000\nquit\n' | socat - UNIX-CONNECT:output/my-test/host/monitor.sock
```

On the x86 side, the container-BAR0 window is at physical
`0xE000000000` (top of the 64-bit PCI bridge pool), so shared-memory
offsets become `0xE000000000 + <shm offset>`. On the NPU side,
chiplet-0 DRAM starts at physical `0x0`, so the same bytes live at
`<shm offset>`. `tests/m5_dataflow_test.py` is the canonical
end-to-end check.

### Doorbell + mailbox path (M6)

A host-guest write to `BAR4 + MAILBOX_INTGR{0,1}` (offsets `0x8` / `0x1c`) is
intercepted by `r100-npu-pci`'s 4 KB MMIO overlay, serialised as an 8-byte
`(offset, value)` frame on the `doorbell.sock` unix-socket chardev, and
consumed by the NPU-side `r100-doorbell` sysbus device. The doorbell no
longer asserts a placeholder SPI directly — it now calls
`r100_mailbox_raise_intgr(group, val)` on the chiplet-0 `r100-mailbox`
peripheral mapped at `R100_PCIE_MAILBOX_BASE` (`0x1FF8160000`). That sets
the corresponding bits in `INTSR{0,1}`, and if the mask allows it
(`INTMSR = INTSR & ~INTMR`), the mailbox raises chiplet-0 GIC
SPI 184 (Group0) / SPI 185 (Group1). This matches the silicon routing
described in the `ipm_samsung` driver.

Quick sanity poking from the monitor:

```
# Host side: MMIO overlay must be present and priority 10
printf 'info mtree\nquit\n' \
  | socat - UNIX-CONNECT:output/<name>/host/monitor.sock \
  | rg 'r100.bar4'

# NPU side: both r100-doorbell and r100-mailbox must be in qtree
printf 'info qtree\nquit\n' \
  | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock \
  | rg 'r100-(doorbell|mailbox)'

# Drive a synthetic frame end-to-end without the kmd
python3 tests/m6_doorbell_test.py
```

`output/<name>/doorbell.log` is the ASCII trace of every
frame the NPU actually received (one line per frame, format
`doorbell off=0x... val=0x... count=N`). If it's empty after the host
has written INTGR, the chardev bridge is down — check that both
QEMUs are still up and that `output/<name>/host/doorbell.sock` exists.

## Interpreting a boot log

Chiplet 0's `uart0.log` on a healthy `-p silicon` boot progresses
through these markers in order. Missing markers pinpoint the stall:

| Marker | Meaning / on failure look at |
|---|---|
| `BL1: check QSPI bridge` | BL1 entered. If absent: image not loaded at `0x1E00010000` — run `./remucli status`. |
| `BL1: Load tboot_p0/p1/u/n` | BL1 QSPI-bridge image staging. Needs `r100_qspi_boot.c` idle + `r100_qspi.c` write paths. |
| `BL1: pmu_release_cm7 complete` | `PHY{0..3}_SRAM_INIT_DONE` bit [0] seed in `r100_pcie_subctrl` (see `r100_soc.c`). |
| `BL1: Detected secondary chiplet count: 3` | QSPI bridge cross-chiplet read path works. |
| `BL1: Release reset of CP0.cpu0 of chiplet-{1,2,3}` | PMU `CPU_CONFIGURATION` triggers `arm_set_cpu_on`. |
| `BL1: Booting BL2` | Handoff via ERET. |
| `BL2: Init HBM of chiplet-0` | HBM3 PHY training entry. Expect 16x `CBT Done` / `HBM Training Complete` / benign `read optimal vref (vert) FAILED` lines. |
| `BL2: Load CP{0,1} BL31/FREERTOS` | File-backed DRAM was pre-staged by `FW_PER_CHIPLET_IMAGES` in `cli/remu_cli.py`. |
| `BL2: Release a reset of CP1.cpu0` | Requires `SYSREG_CP1` triple mount in `r100_soc.c` + `CP1_NONCPU_STATUS` pre-seed in `r100_pmu_set_defaults`. |
| `BL31: Chiplet-N / Boot reason: Cold reset(POR)` | BL31 banner on every chiplet. Requires per-chiplet GIC distributor + redistributor aliases (see `r100_build_chiplet_view`). |
| `Hello world FreeRTOS_CP` / `hw_init: done` / `REBELLIONS$` | Chiplet 0 CP0 FreeRTOS shell. |
| `chiplet count is 4` / `cp_create_tasks_impl` in `hils.log` | CP1 FreeRTOS task init (drained from DRAM ring by `r100_logbuf.c`). |

## Common failure patterns

| Symptom | Likely cause | Where to look |
|---|---|---|
| No output at all | FW images not loaded | `./remucli status`, then re-run `./remucli fw-build -p silicon`. |
| `cpu_on without RVBAR` in `qemu.log` | PMU read_rvbar path missing a cluster | `r100_pmu.c:r100_pmu_read_rvbar`. |
| BL2 spins after `Set RVBAR ... cluster: 1` | `CP1_NONCPU_STATUS` / `CP1_L2_STATUS` not pre-seeded ON | `r100_pmu.c:r100_pmu_set_defaults`. |
| BL31 secondary crashes into `plat_panic_handler` before banner | MPIDR encoding mismatch, or GIC distributor read hitting `cfg_mr` catch-all | `r100_soc.c:r100_build_chiplet_view` (GIC aliases) + MPIDR layout. |
| BL2 ERETs to zero page on a secondary | `bl31_cp0.bin` / `freertos_cp0.bin` not staged at `chiplet_id * CHIPLET_OFFSET + {0x0, 0x200000}` | `cli/remu_cli.py:FW_PER_CHIPLET_IMAGES`. |
| HBM poll never clears | New "done" bit not whitelisted | `r100_hbm.c` PHY region defaults (default 0, plus `phy_train_done` / `prbs_*_done` / `schd_fifo_empty_status` overrides). |

Cross-reference the FW side via the table in `CLAUDE.md` ("Key external
files"). In practice the bootloader sources under
`external/ssw-bundle/products/rebel/q/sys/bootloader/cp/tf-a/` are the
ground truth for whichever MMIO the emulator is mis-modelling.

## Cleanup

Output directories are gitignored but do accumulate on disk:

```
rm -rf output/run-20260101-*        # drop older timestamped runs
rm -rf output/                      # nuke everything, next `./remucli run` recreates it
```
