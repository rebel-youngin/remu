# Testing & Debugging

How to build REMU, run it into a reproducible output directory, and
inspect the result. Prescriptive. Per-milestone design rationale lives
in `git log` (see `docs/roadmap.md` for the commit map) — this doc
only covers what breaks in practice.

## Prerequisites

```
git submodule update --init --recursive         # ssw-bundle + external/qemu
pip install --user click                        # only runtime Python dep
```

No install step — everything goes through `./remucli` at the repo root.
`./remucli status` prints QEMU path, device models, and which FW
images are present.

### Shell tab-completion (optional)

```
eval "$(./remucli completion bash)"   # or: completion zsh / completion fish
```

For persistence, add the same line to `~/.bashrc` with an absolute
path. The hook is on `remucli`, not `./remucli`, so alias or `PATH`
accordingly.

## Build

```
./remucli build                  # incremental QEMU + device models
./remucli build --clean          # wipe build/qemu/ and re-link sources
./remucli fw-build               # CA73 boot set → images/ (silicon only)
cd build/qemu && ninja -j$(nproc)  # manual rebuild, skips symlink / patch steps
```

`-p zebu*` still builds but is deprecated — silicon is the only
regression profile. All six CA73 binaries (`bl1.bin`, `bl2.bin`,
`bl31_cp{0,1}.bin`, `freertos_cp{0,1}.bin`) must be present for clean
boot (check with `./remucli status`).

## Run — output layout

Everything lands in `output/<name>/` (or `output/run-<timestamp>/` if
`--name` omitted). `output/latest` is a symlink to the newest run.
Re-using the same name overwrites in place. Nothing goes to `/tmp`.

Phase 1 (NPU only):

```
output/my-test/
  cmdline.txt     # exact QEMU invocation
  uart{0..3}.log  # per-chiplet UARTs; uart0 also muxed to stdio + monitor
  hils.log        # FreeRTOS HILS ring tail (DRAM @ 0x10000000)
  qemu.log        # only if --trace
```

Phase 2 (`./remucli run --host` — both QEMUs + 3 chardev bridges):

```
output/my-test/
  cmdline.txt + uart{0..3}.log + hils.log   # NPU side (as above)
  doorbell.log  # M6+M8a — host → NPU frames (INTGR + MAILBOX_BASE)
  msix.log      # M7     — NPU → host MSI-X frames
  issr.log      # M8a    — NPU → host ISSR frames
  shm -> /dev/shm/remu-my-test/
  npu/
    monitor.sock       # HMP over unix socket
    qemu.stderr.log    # device-model breadcrumbs (only way to see internal state)
    info-{mtree,qtree,mtree-imsix,qtree-issr}.log  # startup bridge checks
  host/
    cmdline.txt + qemu.{stdout,stderr}.log
    serial.log         # SeaBIOS idle, or Linux + kmd dmesg when M8b Stage 2 staged
    monitor.sock       # HMP
    {doorbell,msix,issr}.sock  # chardev listeners (host = server, NPU = client)
    info-{pci,mtree,mtree-bar4,mtree-bar5,qtree-issr}.log
```

Cleanup is automatic on clean exit. For orphaned runs:

```
./remucli clean --name <name>   # SIGTERM/SIGKILL run's qemu-*; wipe shm + sockets
./remucli clean --all           # nuke every REMU-shaped process / tmpfs / output
```

The same sweep runs at the top of every `./remucli run --name <name>`,
so re-running after a crash is safe without manual steps.

## Recommended agent workflow

1. Edit `src/machine/r100_*.c` (or the host-side `src/host/*.c`).
2. `./remucli build`.
3. Run in a background terminal (`is_background: true` under Cursor):
   `./remucli run --name iter-N`. QEMU `execvp`s over Python, so
   `kill` / signals hit QEMU directly.
4. Poll logs as they grow:

   ```
   rg -n 'BL1:|BL2:|BL31:|Hello world|REBELLIONS\$|panic|guest_error|unimp' \
      output/iter-N/uart0.log output/iter-N/hils.log
   ```

5. Stop with `Ctrl-A X` on the console, or `./remucli clean --name
   iter-N` from another terminal. Re-use the same name for the next
   iteration.

## GDB workflow

Two-terminal pattern:

```
# Terminal 1: start QEMU paused, gdbstub on :1234
./remucli run --name gdb-iter --gdb

# Terminal 2: attach with debug symbols
./remucli gdb -b external/ssw-bundle/products/rebel/q/sys/binaries/BootLoader_CP/bl31.elf
```

`./remucli gdb` prefers `gdb-multiarch`, falling back to
`aarch64-linux-gnu-gdb`. For `q-sys` ELFs, `aarch64-none-elf-gdb` from
the 13.3.rel1 toolchain works (the vendor build has a broken Python
path).

All 32 vCPUs appear as threads `1..32`, laid out chiplet × cluster × core:
threads `N*8+1..N*8+4` = chiplet N CP0 cores 0..3, `N*8+5..N*8+8` = CP1.

### Scripted CP1 sweep

`tests/scripts/gdb_inspect_cp1.gdb` dumps frame 0 + `ELR_EL3` for all
16 CP1 vCPUs. Two-step because `--gdb` bundles `-s -S` and stops
before FW boot:

1. Replay `output/<name>/cmdline.txt` by hand with `-S` dropped (keep
   `-s`), let FW boot ~60 s.
2. Batch-attach:

   ```
   aarch64-none-elf-gdb -batch \
     -ex 'set pagination off' \
     -ex 'file external/ssw-bundle/.../FreeRTOS_CP1/freertos_kernel.elf' \
     -ex 'add-symbol-file external/ssw-bundle/.../FreeRTOS_CP1/bl31.elf' \
     -ex 'target remote :1234' \
     -x tests/scripts/gdb_inspect_cp1.gdb
   ```

Healthy steady state (per chiplet): CP1.cpu0 in `ipm_samsung_receive`,
CP1.cpu{1,2,3} in `taskmgr_fetch_dnc_task_worker_cp1` for DNC ranges
0-5 / 6-10 / 11-15.

## Trace mode

`--trace` turns on `guest_errors,unimp`; log goes to
`output/<name>/qemu.log`. The `cfg_mr` / `private_alias` catch-alls in
`r100_soc.c` absorb most undefined accesses — anything surfacing is
genuinely unmapped.

### Early-boot UNDEFs on IMPDEF sysregs

If BL1 emits zero UART bytes and `qemu.log` has:

```
read access to unsupported AArch64 system register op0:3 op1:0 crn:15 crm:<C> op2:<O>
Taking exception 1 [Undefined Instruction] with ELR 0x1e000xxxxx
```

…FW hit an A73 IMPDEF register that `cortex-a72` doesn't model. Known
cases (`CPUACTLR/ECTLR/MERRSR_EL1` = `S3_0_C15_C0_{0,1,2}`) are RAZ/WI
via `r100_cortex_a73_impdef_regs[]` in `r100_soc.c`. Extend that table
for any new encoding — don't disable the TF-A workaround. Grep any FW
ELF for new encodings:

```
aarch64-none-elf-objdump -d path/to/binary.elf \
  | rg -i 's3_[0-9]_c1[15]_c[0-9]+_[0-9]+' \
  | awk '{print $NF}' | sort -u
```

## QEMU monitor

Chiplet 0 UART is muxed with the HMP monitor on stdio. `Ctrl-A C`
toggles between UART console and `(qemu)`:

```
(qemu) info mtree -f     # flattened memory tree — per-chiplet aliases
(qemu) info qtree        # device tree
(qemu) info registers -a # every CPU
(qemu) stop / cont
```

### Unix-socket monitors with `--host`

Both QEMUs also expose HMP over unix sockets. `socat` for interactive,
`nc -U` for one-shots:

```
socat - UNIX-CONNECT:output/<name>/npu/monitor.sock
socat - UNIX-CONNECT:output/<name>/host/monitor.sock

# NPU chiplet-0 DRAM @ offset 0x07f00000  (== host-side BAR0 at 0xE007f00000)
printf 'xp /4wx 0x07f00000\nquit\n'    | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock
printf 'xp /4wx 0xe007f00000\nquit\n'  | socat - UNIX-CONNECT:output/<name>/host/monitor.sock
```

Container-BAR0 is at physical `0xE000000000` on x86 (top of the 64-bit
PCI bridge pool); chiplet-0 DRAM starts at `0x0` on NPU. Same shm
bytes, two addresses.

## Bridge sanity tests

`./remucli test all` runs every milestone's end-to-end Python test with
per-test cleanup. Individual invocation: `./remucli test {m5,m6,m7,m8}`
or `python3 tests/mN_*.py`. Each test wraps a `--name` (`m5-flow`,
`m6-doorbell`, `m7-msix`, `m8-issr`), so stale shm / sockets / orphan
QEMUs from a prior attempt are cleaned first.

Bridge-wiring design detail is in `docs/architecture.md` and commit
messages (`7b03328` M1-M4, `72c98f0` M5, `85b76bb`/`500856b` M6,
`db3d1df` M7, `cd24aa9` M8a). Quick on-line checks:

```
# M6: host BAR4 MMIO overlay + NPU doorbell/mailbox in qtree
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg 'r100.bar4'
printf 'info qtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg 'r100-(doorbell|mailbox)'

# M7: NPU r100-imsix at 0x1BFFFFF000, host BAR5 msix-{table,pba}
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg 'r100-imsix'
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg 'msix-(table|pba)'

# M8a: ISSR shadow must match between host BAR4 + MAILBOX_BASE and NPU mailbox SFR
#   BAR4 base is in host/info-pci.log (currently 0xfe000000 under SeaBIOS)
printf 'xp /1wx 0xfe000090\nquit\n'    | socat - UNIX-CONNECT:output/<name>/host/monitor.sock
printf 'xp /1wx 0x1ff8160090\nquit\n'  | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock
```

`{doorbell,msix,issr}.log` are per-bridge ASCII traces (format `<bus>
off=0x... val=0x... [status=...] count=N`). Empty-after-traffic means
the chardev is down — check both QEMUs are up and the `*.sock` files
exist. Rejected frames (bad offset / vector ≥ 32) only surface as
`GUEST_ERROR` lines in `qemu.log`, not the per-bridge log.

`./remucli run --host` auto-verifies all of the above on startup and
prints pass/fail; failures are non-fatal (NPU still boots for
post-mortem poking).

### KMD soft-reset handshake (M8b Stage 3a, commit `a01d2b5`)

kmd `rbln_init` rings `REBEL_DOORBELL_SOFT_RESET` on BAR4
`MAILBOX_INTGR0` bit 0, then blocks in `rebel_reset_done` for 3 s
waiting for PF.ISSR[4] to re-become `FW_BOOT_DONE` (`0xFB0D`). On
silicon this is the PCIe CM7 subcontroller's job (catches SPI 184,
runs `pcie_soft_reset_handler`, PMU cluster down/up, `bootdone_task`
rerun). REMU models neither CM7 nor the PMU reset sequence, and the
CA73 FreeRTOS build short-circuits the mailbox ISR to `default_cb`
(CMake `FREERTOS_PORT != GCC_ARM_CA73` gate in
`drivers/pcie/pcie_mailbox_callback.c`).

Fix lives entirely in QEMU as a CM7-stub in `r100_doorbell.c`: on an
`INTGR0` frame with bit 0 set, call
`r100_mailbox_cm7_stub_write_issr(pf_mailbox, 4, 0xFB0D)` (updates
PF.ISSR[4] **and** emits the NPU→host issr frame so the host BAR4
shadow converges). Other bits relay onto VF0.INTGR1 for CA73 ISR
visibility. `pf-mailbox` link wired from `r100_soc.c`.

Verification on a settled `./remucli run --host`:

```
rg -nE 'rebel_soft_reset|FW_BOOT_DONE' output/<name>/host/serial.log
#   → rebellions rbln0: [rbln-rbl] rebel_soft_reset + 0
#   → rebellions rbln0: [rbln-rbl] FW_BOOT_DONE
grep '0x90.*0xfb0d' output/<name>/issr.log
rg 'doorbell_deliver' output/<name>/npu/qemu.stderr.log | head
```

If `rebel_soft_reset + 0` appears but `FW_BOOT_DONE` doesn't: walk
`issr.log` (egress frame) → `qemu.stderr.log` (doorbell ingress) →
host BAR4 `xp /1wx 0xfe000090` (shadow). Empty `qemu.stderr.log`
means the run-directory redirect failed.

The stub is silicon-agnostic and retires when REMU grows a real CM7
model.

### x86 Linux guest boot (M8b Stage 2, commit `1ef7208`)

Without `images/x86_guest/{bzImage,initramfs.cpio.gz}`, the x86 side
idles at SeaBIOS. When staged, `./remucli run --host` auto-wires:

- `-kernel <bzImage> -initrd <initramfs> -append "console=ttyS0 rdinit=/init earlyprintk=serial"`
- `-fsdev local,id=remu,path=guest/,security_model=none -device virtio-9p-pci,fsdev=remu,mount_tag=remu`
- `-cpu qemu64 → -cpu max` (stock kmd is `-march=native` with BMI2
  instructions that trap `#UD` on `qemu64`; commit `985fd58` also
  patches the GIC CPU-interface `bpr > 0` assertion that bit once `-cpu max`
  was enabled).

Stage artifacts:

```
./guest/build-guest-image.sh   # → images/x86_guest/{bzImage,initramfs.cpio.gz}
./guest/build-kmd.sh           # → guest/{rebellions,rblnfs}.ko vs host kernel
```

`build-guest-image.sh` uses `apt-get download` + `dpkg-deb -x` (no
sudo) to assemble an HWE-kernel initramfs with busybox + 9p modules.
`guest/` is the 9p share — anything `setup.sh` writes to
`/mnt/remu/output/` in-guest shows up in `guest/output/` on host in
real time. Missing artifacts → silent fallback to SeaBIOS idle (M5-M8a
tests still work).

CLI overrides on `./remucli run`: `--guest-kernel`, `--guest-initrd`,
`--guest-share`, `--no-guest-boot`, `--guest-cmdline-extra`.

Healthy `output/<name>/host/serial.log` on Stage 3a:

```
Linux version 6.8.0-107-generic … boot banner
[init] mounting 9p share 'remu' at /mnt/remu
[setup] insmod rebellions.ko
rebellions 0000:00:05.0: pci main[1eff,2030] sub[1af4,1100] rev[1]
rebellions 0000:00:05.0: [BAR-0/DDR]      size 0x1000000000
rebellions 0000:00:05.0: [BAR-2/ACP]      size 0x4000000
rebellions 0000:00:05.0: [BAR-4/DOORBELL] size 0x800000
rebellions 0000:00:05.0: [BAR-5/PCIE]     size 0x100000
rebellions rbln0: msix vectors count: requested 32, supported 32
rebellions rbln0: [rbln-rbl] rebel_soft_reset + 0
rebellions rbln0: [rbln-rbl] FW_BOOT_DONE
```

No `FW_BOOT_DONE` → see CM7-stub shadow-check recipe above.

### Stage 3b (in progress)

After `FW_BOOT_DONE`, kmd's `rebel_hw_init` soft-lockups at t+25 s:

```
watchdog: BUG: soft lockup - CPU#0 stuck for 26s! [kworker/…:rbln_probe]
  rebel_hw_init+0x439/0xa60 [rebellions]
  rbln_device_init_async+0x46/0x470 [rebellions]
```

Next unmodelled FW wait-state — likely another ISSR poll or the
`TEST_IB` shared-DRAM ring handshake. Decode, then wire up FW-side or
synthesise in QEMU analogous to the CM7-stub.

## Interpreting a boot log

`uart0.log` on a healthy `silicon` boot progresses through these
markers in order — missing marker pinpoints the stall:

| Marker | If absent: |
|---|---|
| `BL1: check QSPI bridge` | image not loaded at `0x1E00010000` — run `./remucli status`. |
| `BL1: Load tboot_p0/p1/u/n` | `r100_qspi_boot.c` idle + `r100_qspi.c` write paths. |
| `BL1: pmu_release_cm7 complete` | `PHY{0..3}_SRAM_INIT_DONE` bit-0 seed in `r100_pcie_subctrl` (`r100_soc.c`). |
| `BL1: Detected secondary chiplet count: 3` | QSPI bridge cross-chiplet read. |
| `BL1: Release reset of CP0.cpu0 of chiplet-{1,2,3}` | PMU `CPU_CONFIGURATION` → `arm_set_cpu_on`. |
| `BL2: Init HBM of chiplet-0` | HBM3 PHY training. Expect 16× `CBT Done`/`HBM Training Complete` + benign `read optimal vref (vert) FAILED`. |
| `BL2: Load CP{0,1} BL31/FREERTOS` | `FW_PER_CHIPLET_IMAGES` in `cli/remu_cli.py`. |
| `BL2: Release a reset of CP1.cpu0` | `SYSREG_CP1` triple mount (`r100_soc.c`) + `CP1_NONCPU_STATUS` pre-seed (`r100_pmu_set_defaults`). |
| `BL31: Chiplet-N / Boot reason: Cold reset(POR)` | per-chiplet GIC distributor + redistributor aliases (`r100_build_chiplet_view`). |
| `Hello world FreeRTOS_CP` / `REBELLIONS$` | chiplet 0 CP0 FreeRTOS shell. |
| `chiplet count is 4` / `cp_create_tasks_impl` in `hils.log` | CP1 FreeRTOS task init (drained by `r100_logbuf.c`). |

## Common failure patterns

| Symptom | Likely cause | File |
|---|---|---|
| No output | FW images not loaded | `./remucli status` → `./remucli fw-build` |
| `cpu_on without RVBAR` in `qemu.log` | PMU read_rvbar path missing a cluster | `r100_pmu.c:r100_pmu_read_rvbar` |
| BL2 spins after `Set RVBAR ... cluster: 1` | `CP1_NONCPU_STATUS` / `CP1_L2_STATUS` not pre-seeded ON | `r100_pmu.c:r100_pmu_set_defaults` |
| BL31 secondary crashes into `plat_panic_handler` before banner | MPIDR encoding mismatch, or GIC read hitting `cfg_mr` catch-all | `r100_soc.c:r100_build_chiplet_view` + MPIDR layout |
| BL2 ERETs to zero page on a secondary | `bl31_cp0.bin` / `freertos_cp0.bin` not at `chiplet_id * CHIPLET_OFFSET + {0x0, 0x200000}` | `cli/remu_cli.py:FW_PER_CHIPLET_IMAGES` |
| HBM poll never clears | New "done" bit not whitelisted | `r100_hbm.c` PHY region defaults |
| FreeRTOS banner prints then hangs; a CPU sits in `vApplicationPassiveIdleHook` | gtimer outputs not wired to GIC PPI inputs (CNTVIRQ never delivered, `vTaskDelay` never returns) | `r100_soc.c` — the `qdev_connect_gpio_out(cpudev, GTIMER_*, ... )` block per CPU with `intidbase = (num_irq - GIC_INTERNAL) + local*32` (commit `680f964`) |

FW ground truth lives in `external/ssw-bundle/products/rebel/q/sys/bootloader/cp/tf-a/`
— cross-reference via `CLAUDE.md` ("Key external files").

## Cleanup

```
rm -rf output/run-20260101-*    # drop older timestamped runs
rm -rf output/                  # nuke everything; next run recreates
```
