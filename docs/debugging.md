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
./remucli fw-build                 # platform defaults to silicon (the only supported profile)
```

`-p zebu` / `-p zebu_ci` / `-p zebu_vdk` still build via `q-sys/build.sh`
but emit a deprecation warning and are not part of any REMU regression.

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
    doorbell.log      # NPU-side ASCII trace of every guest→NPU (offset, value) frame
                      # (both M6 INTGR triggers and M8a ISSR payload writes)
    msix.log          # host-side ASCII trace of every NPU→host (offset, db_data) frame (M7)
    issr.log          # host-side ASCII trace of every NPU→host ISSR (offset, value) frame (M8a)
    npu/
      monitor.sock    # NPU HMP monitor (unix socket)
      qemu.stderr.log # NPU QEMU stderr — captures fprintf(stderr, ...) breadcrumbs from
                      #   device models (r100-doorbell, r100-mailbox, r100-npu-pci), the
                      #   only way to see QEMU-internal state that doesn't flow through UART
      info-mtree.log  # captured info mtree — chiplet0.dram splice
      info-mtree-imsix.log  # captured info mtree — r100-imsix MMIO @ 0x1BFFFFF000 (M7)
      info-qtree.log  # captured info qtree — confirms r100-doorbell + r100-mailbox
      info-qtree-issr.log   # captured info qtree — confirms r100-mailbox has issr-chardev wired (M8a)
    host/
      cmdline.txt     # x86 QEMU invocation
      qemu.stdout.log
      qemu.stderr.log
      serial.log      # x86 guest serial:
                      #   - SeaBIOS + "No bootable device" idle (default),  OR
                      #   - Linux boot log + setup.sh trace + kmd dmesg (M8b Stage 2,
                      #     when images/x86_guest/bzImage + initramfs.cpio.gz exist)
      monitor.sock    # host HMP monitor (unix socket)
      doorbell.sock   # unix-socket chardev listener (host = server, NPU = client; M6+M8a)
      msix.sock       # unix-socket chardev listener (host = server, NPU = client; M7)
      issr.sock       # unix-socket chardev listener (host = server, NPU = client; M8a)
      info-pci.log    # captured info pci — lists 1eff:2030 endpoint
      info-mtree.log  # captured info mtree — BAR0 shm splice
      info-mtree-bar4.log  # captured info mtree — BAR4 MMIO overlay (M6+M8a)
      info-mtree-bar5.log  # captured info mtree — BAR5 msix-table + msix-pba overlays (M7)
      info-qtree-issr.log  # captured info qtree — confirms r100-npu-pci has issr chardev wired (M8a)
```

Everything under `/dev/shm/remu-<name>/` is cleaned up on exit. If
`./remucli run --host` is killed `SIGKILL`-style and leaves an orphan
behind, `./remucli clean --name <name>` is the recovery — it SIGTERMs
(then SIGKILLs) any `qemu-system-*` process whose cmdline mentions
this run's `/dev/shm/remu-<name>/` or `output/<name>/`, wipes the
tmpfs dir, and unlinks stale `*.sock` files so the next bind doesn't
`EADDRINUSE`. The same sweep is invoked automatically at the top of
every `./remucli run --name <name>` invocation, so re-running with
the same name after a crash is safe without manual steps. `./remucli
clean --all` is the "something is broken, nuke every REMU-shaped
thing" escape hatch for multi-run messes.

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
   terminal / the agent: `./remucli clean --name iter-N` (preferred —
   kills just this run's QEMU children, wipes its tmpfs and sockets)
   or `pkill -f qemu-system-aarch64` if you want to nuke every REMU
   NPU QEMU at once.

6. If you need a fresh run, reuse the same `--name` (the auto-cleanup
   at the top of every `./remucli run` call clears any prior state)
   or accept the default timestamped directory. Old runs stay around
   under `output/` for comparison and are gitignored.

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

### Early-boot UNDEFs on IMPDEF sysregs

If BL1 produces **zero** bytes of UART output and `--trace`'s
`qemu.log` shows lines like:

```
read access to unsupported AArch64 system register op0:3 op1:0 crn:15 crm:<C> op2:<O>
Taking exception 1 [Undefined Instruction] with ELR 0x1e000xxxxx
```

…the FW has hit an A73 implementation-defined CPU control register
that `cortex-a72` doesn't model. The known cases are handled by
`r100_cortex_a73_impdef_regs[]` in `r100_soc.c` (`S3_0_C15_C0_{0,1,2}`
= CPUACTLR/ECTLR/MERRSR_EL1), which are registered as RAZ/WI to
unblock TF-A's unconditional CVE-2018-3639 workaround. If a new TF-A
release or a new FreeRTOS task ever probes a *different* IMPDEF reg,
the fix is to extend that table — not to disable the workaround in the
FW build. Grep any FW ELF for new encodings with:

```
aarch64-none-elf-objdump -d path/to/binary.elf \
  | rg -i 's3_[0-9]_c1[15]_c[0-9]+_[0-9]+' \
  | awk '{print $NF}' | sort -u
```

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
end-to-end check — invoke it directly, or via the packaged CLI
runner:

```
./remucli test m5          # one bridge
./remucli test m5 m6 m7 m8 # explicit list
./remucli test all         # every bridge, with per-test auto-cleanup
```

`./remucli test` shells out to each `tests/mN_*.py` after calling
`_cleanup_run_trash` on its per-test `--name` (`m5-flow`, `m6-doorbell`,
`m7-msix`, `m8-issr`), so back-to-back invocations don't trip over
stale shm / sockets / orphan QEMUs from a prior attempt. Use
`--stop-on-fail` to abort at the first failure, `--skip-clean` if you
want to inspect leftover state (not recommended).

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
python3 tests/m6_doorbell_test.py   # or: ./remucli test m6
```

`output/<name>/doorbell.log` is the ASCII trace of every
frame the NPU actually accepted — both M6 INTGR triggers and M8a
ISSR payload writes (one line per frame, format `doorbell off=0x...
val=0x... count=N`; the offset identifies which class). Frames
rejected for bad offsets do not appear here — they only surface as
`GUEST_ERROR` entries in `qemu.log`. If `doorbell.log` is empty after
the host has written INTGR / MAILBOX_BASE, the chardev bridge is
down — check that both QEMUs are still up and that
`output/<name>/host/doorbell.sock` exists.

### MSI-X path (M7, FW → guest)

The reverse direction of M6. A FW-side 4-byte store to
`REBELH_PCIE_MSIX_ADDR` (`0x1B_FFFF_FFFC`) — the same address silicon's
DW PCIe MAC snoops to emit an MSI-X TLP — is trapped by the chiplet-0
`r100-imsix` sysbus device's 4 KB MMIO window at
`R100_PCIE_IMSIX_BASE` (`0x1B_FFFF_F000`), offset `0xFFC`. The device
serialises it as an 8-byte `(offset, db_data)` frame on the
`msix.sock` unix-socket chardev (mirror of M6's wire format, opposite
direction — host is server, NPU is `reconnect=1` client). The host-side
`r100-npu-pci` receiver masks `vector = db_data & 0x7FF` and calls
`msix_notify()`, which delivers the interrupt to the x86 guest via the
MSI-X table on BAR5. Vectors ≥ 32 (beyond the BAR5 table) and writes
at any other offset in the iMSIX page are counted as `oor` /
`bad-offset` and also logged to `qemu.log` as `GUEST_ERROR` entries.

Quick sanity poking from the monitor:

```
# NPU side: r100-imsix MMIO region must be mapped at 0x1BFFFFF000
printf 'info mtree\nquit\n' \
  | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock \
  | rg 'r100-imsix'

# Host side: BAR5 must carry msix-table + msix-pba overlays
printf 'info mtree\nquit\n' \
  | socat - UNIX-CONNECT:output/<name>/host/monitor.sock \
  | rg 'msix-(table|pba)'

# Drive a synthetic frame end-to-end without the FW
python3 tests/m7_msix_test.py       # or: ./remucli test m7
```

`output/<name>/msix.log` is the ASCII trace of every frame the host
actually accepted or rejected (one line per frame, format
`msix off=0x... db_data=0x... vector=N status={ok|oor|bad-offset} count=N`).
If it's empty after the FW has written `REBELH_PCIE_MSIX_ADDR`, the
chardev bridge is down — check that both QEMUs are up and that
`output/<name>/host/msix.sock` exists. `./remucli run --host`
auto-verifies both mtree checks on startup and prints pass/fail to
stdout.

### ISSR bridge (M8a, both directions)

M8a is pure register-shadow transport over the shared-mailbox window
(`MAILBOX_BASE..MAILBOX_END` = `0x80..0x180` in both BAR4 and the NPU
Samsung-IPM SFR). No silicon peripheral moves: the NPU's `r100-mailbox`
still owns the authoritative ISSR0..63 state, and the host's BAR4
still looks like an ordinary register window to the guest. What M8a
adds is a pair of one-way forwarders that keep the two copies in sync.

**NPU → host** (firmware writes ISSR, host BAR4 reflects it). Every
MMIO write to an ISSR register inside `r100-mailbox` emits an 8-byte
`(bar4_offset, value)` frame on a third unix socket (`issr.sock`,
same wire format as M6/M7). Host-side `r100-npu-pci` receives it and
mirrors the value into `bar4_mmio_regs[]` — so the very next guest
`readl` at `BAR4 + MAILBOX_BASE + idx*4` returns the firmware's
latest write, without going back through the wire. The `r100-mailbox`
instance is told about the chardev through a pair of machine-level
string properties (`issr=<id>`, `issr-debug=<id>`) forwarded onto the
device as `issr-chardev` / `issr-debug-chardev`.

**Host → NPU** (guest writes BAR4, firmware's ISSR updates). Rather
than open a fourth socket, M8a reuses the M6 doorbell chardev.
Writes into `MAILBOX_BASE..MAILBOX_END` on the host side are emitted
as the same `(offset, value)` frames — except the offset puts them in
the payload range instead of on `INTGR{0,1}`. The NPU-side
`r100-doorbell` disambiguates by offset: `0x8` / `0x1c` → call
`r100_mailbox_raise_intgr()` (M6 behaviour, asserts SPI); `0x80..0x180`
→ call `r100_mailbox_set_issr()` (M8a, updates the scratch word but
asserts nothing and does *not* re-emit on the `issr` chardev, so the
host's write doesn't loop back to itself). Offsets outside both
ranges log `GUEST_ERROR`.

Concretely, that makes the `FW_BOOT_DONE` handshake (firmware writes
`0xFB0D` to ISSR[4]) and the reset-counter ping (ISSR[7]) into plain
MMIO exchanges with no protocol-specific code on either side — the
logical step M8b will bolt the real HIL protocol on top of.

Quick sanity poking from the monitor:

```
# NPU side: r100-mailbox must advertise an issr-chardev wiring
printf 'info qtree\nquit\n' \
  | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock \
  | rg -A 1 'r100-mailbox'
#   → "issr-chardev = \"issr\""

# Host side: r100-npu-pci must advertise an issr chardev
printf 'info qtree\nquit\n' \
  | socat - UNIX-CONNECT:output/<name>/host/monitor.sock \
  | rg -A 8 'r100-npu-pci' | rg 'issr'
#   → "issr = \"issr\"" and "issr-debug = \"issr_dbg\""

# Read the live ISSR shadow from the host side at BAR4 + MAILBOX_BASE
#   BAR4 base comes from info-pci.log (currently 0xfe000000 with SeaBIOS)
printf 'xp /4wx 0xfe000090\nquit\n' \
  | socat - UNIX-CONNECT:output/<name>/host/monitor.sock
# Read the same word on the NPU side straight off the mailbox SFR
printf 'xp /1wx 0x1ff8160090\nquit\n' \
  | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock
# The two must agree at steady state.

# Drive a synthetic bidirectional exchange without the kmd or FW
python3 tests/m8_issr_test.py       # or: ./remucli test m8
```

`output/<name>/issr.log` is the ASCII trace of every ISSR frame the
host actually accepted or rejected (one line per frame, format
`issr off=0x... val=0x... status={ok|bad-offset} count=N`). If it's
empty after the FW has written an ISSR, the chardev is down — check
that both QEMUs are up and that `output/<name>/host/issr.sock` exists.
`./remucli run --host` auto-verifies both qtree checks on startup and
prints pass/fail to stdout (non-fatal — the NPU still boots for
post-mortem poking if the wiring is missing).

### KMD soft-reset handshake — CM7-stub (M8b Stage 3a)

The host `kmd`'s `rbln_init` probe path always rings
`REBEL_DOORBELL_SOFT_RESET` on BAR4 `MAILBOX_INTGR0` bit 0 and then
blocks in `rebel_reset_done` waiting for PF.ISSR[4] to re-become
`FW_BOOT_DONE` (0xFB0D). On silicon this is the PCIe CM7
subcontroller's job — it catches the INTGR on SPI 184, runs
`pcie_soft_reset_handler` (PMU cluster down/up, then
`bootdone_task` reruns and rewrites ISSR[4] on the way back up).
REMU models neither the CM7 nor the PMU reset sequence, and on
CA73 FreeRTOS builds the PCIe mailbox ISR resolves to `default_cb`
(`drivers/pcie/pcie_mailbox_callback.c` is gated
`FREERTOS_PORT != GCC_ARM_CA73` by its `CMakeLists.txt`). Net
effect without a fix: kmd times out after 3 s and the probe
fails.

The fix lives **entirely in QEMU** as a CM7-stub in
`src/machine/r100_doorbell.c`. Schematic of the shortcut:

```
host guest           (silicon)         REMU shortcut
   │                   │                   │
   │ BAR4+0x08 = 1     │                   │
   │──────────────────▶│ SPI 184 → CM7     │ ──┐
                       │ ipm_samsung_isr   │   │  intercept at
                       │ pcie_host2cm7_cb  │   │  doorbell frame
                       │ pcie_soft_reset   │   │  decode
                       │ PMU down/up       │   │
                       │ bootdone_task     │   │
                       │ PF.ISSR[4]=0xFB0D │◀──┘  r100_mailbox_cm7_stub_write_issr
                                                  updates state + emits issr frame
                   ▲                        ▲
                   └──── same visible ──────┘
                         side-effect
```

Concretely, on an `INTGR0` frame with bit 0 set the doorbell
calls `r100_mailbox_cm7_stub_write_issr(pf_mailbox, 4, 0xFB0D)` —
a helper in `r100_mailbox.c` that updates PF.ISSR[4] **and**
emits the NPU→host `issr` egress frame so the host BAR4 shadow
converges. Other bits in the same write are relayed onto
VF0.INTGR1 for CA73-ISR visibility (`default_cb` is a no-op, but
the IRQ trace is still useful). The `pf-mailbox` `DEFINE_PROP_LINK`
on the doorbell is wired from `r100_soc.c` at machine-init time.

Quick verification recipe (after `./remucli run --host` has
settled):

```
# 1. host guest saw FW_BOOT_DONE in dmesg?
rg -nE 'rebel_soft_reset|FW_BOOT_DONE' output/<name>/host/serial.log
#   → [... .XXXXXX] rebellions rbln0: [rbln-rbl] rebel_soft_reset + 0
#   → [... .XXXXXX] rebellions rbln0: [rbln-rbl] FW_BOOT_DONE

# 2. ISSR egress actually fired?
grep '0x90.*0xfb0d' output/<name>/issr.log
#   → issr off=0x90 val=0xfb0d status=ok count=1

# 3. Doorbell bit pattern from the kmd?
rg 'doorbell_deliver' output/<name>/npu/qemu.stderr.log | head
#   → REMU-TRACE: doorbell_deliver off=0x8 val=0x1 count=3
#     (bit 0 set = SOFT_RESET, triggers the stub)
```

If step 1 shows `rebel_soft_reset + 0` but no `FW_BOOT_DONE`
follow-up, walk step 2 (egress frame) → step 3 (doorbell frame
actually arrived) → the M8a shadow-check recipe above (host BAR4
`xp /1wx` on `0xfe000090` must read `0xfb0d`) to localise the
break. Empty `qemu.stderr.log` means the `./remucli run --host`
redirect failed to open the file — confirm the run directory
exists and is writable.

The stub is deliberately silicon-agnostic: it does not simulate
the PMU reset or re-run `bootdone_task`, it only reproduces the
one externally-visible side-effect the host kmd is actually
polling for. When REMU eventually grows a CM7 model,
`pcie_soft_reset_handler` will run for real and this shortcut
can be retired — at that point set breakpoints in
`drivers/pcie/pcie_mailbox_callback.c` via `./remucli gdb` (or
drop a local debug patch into `cli/fw-patches/` per the policy
in `cli/fw-patches/README.md`) to trace the sequence.

### x86 Linux guest boot (M8b Stage 2)

Until M8b Stage 2, the x86 side of `./remucli run --host` stopped at
SeaBIOS's "No bootable device" idle loop. Stage 2 adds the ability
to boot a real Linux kernel that loads the Rebellions kmd
(`rebellions.ko`) against our emulated PCI endpoint, so the
`FW_BOOT_DONE` handshake ends up in guest dmesg instead of just on
`issr.log`.

**Artifacts staged under `images/x86_guest/` (gitignored):**

```
images/x86_guest/
  bzImage              # Ubuntu HWE distro kernel, ~15 MB
  initramfs.cpio.gz    # busybox + 9p modules + /init, ~1.3 MB
```

Both are produced by `./guest/build-guest-image.sh`, which uses
`apt-get download` to fetch `linux-{image,modules}-$(uname -r)` and
`busybox-static` from the Ubuntu archive (no sudo, nothing installed
system-wide), `dpkg-deb -x` to extract them, cherry-picks the 4
modules needed for virtio-9p (`netfs`, `9pnet`, `9pnet_virtio`, `9p`
— virtio-pci is builtin in Ubuntu HWE kernels), and assembles a
minimal cpio initramfs with a `/init` that mounts the share and runs
`setup.sh`.

**Shared folder (`guest/`, tracked in git):**

```
guest/
  README.md
  build-guest-image.sh     # produces images/x86_guest/{bzImage,initramfs.cpio.gz}
  build-kmd.sh             # produces rebellions.ko + rblnfs.ko against host kernel headers
  setup.sh                 # runs INSIDE the guest; insmod + FW_BOOT_DONE probe
  rebellions.ko            # gitignored; rebuilt by build-kmd.sh for current kernel
  rblnfs.ko                # gitignored; companion module (depends on rebellions)
  output/                  # gitignored; setup.sh inside the guest drops dmesg/lspci here
```

Exposed to the guest via `-fsdev local,path=<repo>/guest,id=remu
-device virtio-9p-pci,fsdev=remu,mount_tag=remu`. Because this is a
9p pass-through (not a disk image), anything `setup.sh` writes to
`/mnt/remu/output/` inside the guest lands in `guest/output/` on the
host in real time.

**One-time setup (host):**

```
./guest/build-guest-image.sh   # stages images/x86_guest/{bzImage,initramfs.cpio.gz}
./guest/build-kmd.sh           # stages guest/rebellions.ko and guest/rblnfs.ko
```

If either file under `images/x86_guest/` is missing, `./remucli run
--host` silently falls back to the pre-M8b SeaBIOS-idle behaviour —
all earlier-milestone tests (M5/M6/M7/M8a) keep working untouched.

**Auto-wiring in `./remucli run --host`:**

- Adds `-kernel <bzImage> -initrd <initramfs.cpio.gz>
  -append "console=ttyS0 rdinit=/init earlyprintk=serial"`.
- Adds `-fsdev local,id=remu,path=guest/,security_model=none
  -device virtio-9p-pci,fsdev=remu,mount_tag=remu`.
- Flips `-cpu qemu64 → -cpu max` on the x86 QEMU. The stock kmd is
  built with `-march=native` and emits BMI2-encoded instructions
  (e.g. BZHI) that trap as `#UD` on the minimal `qemu64` CPU,
  killing `rbln_init` immediately. `-cpu max` exposes every TCG
  feature so the unmodified .ko runs with no patching.

CLI overrides (all on `./remucli run`):

| Flag                    | Effect                                           |
|-------------------------|--------------------------------------------------|
| `--guest-kernel PATH`   | Use a different bzImage                          |
| `--guest-initrd PATH`   | Use a different initramfs                        |
| `--guest-share PATH`    | Share a different host dir (default: `guest/`)   |
| `--no-guest-boot`       | Skip all of the above, stay on SeaBIOS idle      |
| `--guest-cmdline-extra` | Extra kernel cmdline tokens (e.g. `loglevel=7`)  |

**Expected `output/<name>/host/serial.log` timeline on a healthy Stage 3a run:**

```
Linux version 6.8.0-107-generic … boot banner
[init] kernel 6.8.0-107-generic up — loading 9p modules
9p: Installing v9fs 9p2000 file system support
[init] mounting 9p share 'remu' at /mnt/remu
[init] 9p mount OK
[init] running /mnt/remu/setup.sh
[setup] found CR03 quad at 0000:00:05.0
[setup] insmod rebellions.ko
rebellions 0000:00:05.0: pci main[1eff,2030] sub[1af4,1100] rev[1]
rebellions 0000:00:05.0: [BAR-0/DDR]      size 0x1000000000
rebellions 0000:00:05.0: [BAR-2/ACP]      size 0x4000000
rebellions 0000:00:05.0: [BAR-4/DOORBELL] size 0x800000
rebellions 0000:00:05.0: [BAR-5/PCIE]     size 0x100000
rebellions rbln0: msix vectors count: requested 32, supported 32
rebellions rbln0: [rbln-rbl] rebel_soft_reset + 0         ← kmd rings INTGR0 SOFT_RESET
rebellions rbln0: [rbln-rbl] FW_BOOT_DONE                 ← CM7-stub responded (Stage 3a)
```

If `FW_BOOT_DONE` never shows up, walk the NPU side: `issr.log`
should contain `off=0x90 val=0xfb0d`, then host-side `xp /1wx
0xfe000090` on `monitor.sock` should read back `0xfb0d`. See the
CM7-stub (Stage 3a) subsection above for the full shadow-check recipe.

**Known next wait-state (M8b Stage 3b, in progress).** After
`FW_BOOT_DONE`, kmd continues into `rebel_hw_init` where it hits a
soft-lockup at t+25 s:

```
watchdog: BUG: soft lockup - CPU#0 stuck for 26s! [kworker/…:rbln_probe]
…
Call Trace:
  rebel_hw_init+0x439/0xa60 [rebellions]
  rbln_device_init_async+0x46/0x470 [rebellions]
  rbln_async_probe_worker+0x42/0x2f0 [rebellions]
```

This is the subsequent unmodelled FW wait-state — decode what
`rebel_hw_init` is polling for (likely another ISSR or the
`TEST_IB` shared-DRAM ring handshake), then either wire it up on
the FW side or synthesise it in QEMU analogous to the CM7-stub.

## Interpreting a boot log

Chiplet 0's `uart0.log` on a healthy `silicon` boot progresses
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
| No output at all | FW images not loaded | `./remucli status`, then re-run `./remucli fw-build`. |
| `cpu_on without RVBAR` in `qemu.log` | PMU read_rvbar path missing a cluster | `r100_pmu.c:r100_pmu_read_rvbar`. |
| BL2 spins after `Set RVBAR ... cluster: 1` | `CP1_NONCPU_STATUS` / `CP1_L2_STATUS` not pre-seeded ON | `r100_pmu.c:r100_pmu_set_defaults`. |
| BL31 secondary crashes into `plat_panic_handler` before banner | MPIDR encoding mismatch, or GIC distributor read hitting `cfg_mr` catch-all | `r100_soc.c:r100_build_chiplet_view` (GIC aliases) + MPIDR layout. |
| BL2 ERETs to zero page on a secondary | `bl31_cp0.bin` / `freertos_cp0.bin` not staged at `chiplet_id * CHIPLET_OFFSET + {0x0, 0x200000}` | `cli/remu_cli.py:FW_PER_CHIPLET_IMAGES`. |
| HBM poll never clears | New "done" bit not whitelisted | `r100_hbm.c` PHY region defaults (default 0, plus `phy_train_done` / `prbs_*_done` / `schd_fifo_empty_status` overrides). |
| FreeRTOS banner prints then boot hangs silently; a CPU sits in `vApplicationPassiveIdleHook` forever (visible under GDB as `wfi` in the idle task) | CPU generic-timer outputs not wired to the GIC PPI inputs — FreeRTOS's CNTVIRQ (PPI 27) tick fires inside `target/arm/helper.c` but the GIC never sees it, so `vTaskDelay()` never returns and `bootdone_task`'s `hils_check_product_info_ready()` wedges | `r100_soc.c`: verify the `qdev_connect_gpio_out(cpudev, GTIMER_*, qdev_get_gpio_in(gic_dev[chiplet], intidbase + ARCH_TIMER_*_IRQ))` block runs per CPU with `intidbase = (num_irq - GIC_INTERNAL) + local*32`. |

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
