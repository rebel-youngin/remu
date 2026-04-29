# Testing & Debugging

How to build REMU, run it into a reproducible output directory, and
inspect the result. Per-fix design rationale lives in `git log` — this
doc only covers what breaks in practice and how to drive the
emulator.

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

Phase 2 (`./remucli run --host` — both QEMUs + chardev bridges):

```
output/my-test/
  cmdline.txt + uart{0..3}.log + hils.log   # NPU side (as above)
  doorbell.log  # host → NPU frames (INTGR + MAILBOX_BASE)
  msix.log      # NPU → host MSI-X frames (sourced by q-cp's cb_complete
                #  → pcie_msix_trigger via the r100-imsix MMIO trap)
  issr.log      # NPU → host ISSR frames
  hdma.log      # bidirectional HDMA (OP_WRITE / OP_READ_REQ / OP_READ_RESP)
  rbdma.log     # chiplet 0's r100-rbdma kickoff / BH fire / FNSH pop,
                # one line per task lifecycle step
  shm -> /dev/shm/remu-my-test/        # remu-shm + host-ram + cfg-shadow shared backends
  npu/
    monitor.sock       # HMP over unix socket
    qemu.stderr.log    # device-model breadcrumbs (only way to see internal state)
    info-{mtree,qtree,mtree-imsix,qtree-issr,qtree-cfg-hdma}.log  # startup bridge checks
  host/
    cmdline.txt + qemu.{stdout,stderr}.log
    serial.log         # SeaBIOS idle, or Linux + kmd dmesg when guest image staged
    monitor.sock       # HMP
    {doorbell,msix,issr,hdma}.sock  # chardev listeners (host = server, NPU = client)
    info-{pci,mtree,mtree-bar4,mtree-bar5,qtree-issr,qtree-cfg-hdma}.log
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
per-test cleanup. Individual invocation: `./remucli test {m5,m6,m7,m8,p4a,p4b,p5,p11}`
or `python3 tests/<test>.py`. Each test wraps a `--name` so stale
shm / sockets / orphan QEMUs from a prior attempt are cleaned first.

Quick on-line checks:

```
# host BAR4 MMIO overlay + NPU cm7/mailbox in qtree
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg 'r100.bar4'
printf 'info qtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg 'r100-(cm7|mailbox)'

# NPU r100-imsix at 0x1BFFFFF000, host BAR5 msix-{table,pba}
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg 'r100-imsix'
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg 'msix-(table|pba)'

# ISSR shadow must match between host BAR4 + MAILBOX_BASE and NPU mailbox SFR
#   BAR4 base is in host/info-pci.log (currently 0xfe000000 under SeaBIOS)
printf 'xp /1wx 0xfe000090\nquit\n'    | socat - UNIX-CONNECT:output/<name>/host/monitor.sock
printf 'xp /1wx 0x1ff8160090\nquit\n'  | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock

# hdma chardev must be bound on both ends in qtree
printf 'info qtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg 'hdma *='
printf 'info qtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg 'hdma-chardev'

# cfg-mirror alias must overlap on both sides (cfg-shadow shm)
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg 'r100.bar2.cfg.alias'
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg 'r100.cm7.cfg-mirror.alias'
```

`{doorbell,msix,issr,hdma}.log` are per-bridge ASCII traces (format
`<bus> off=0x... val=0x... [status=...] count=N`, or for hdma:
`hdma <dir> op=<mnemonic> dst=0x... len=... req_id=N status=... count=N`
where `<dir>` is `tx` / `rx` and mnemonics are
`WRITE`/`READ_REQ`/`READ_RESP`). Empty-after-traffic means the chardev
is down — check both QEMUs are up and the `*.sock` files exist.
Rejected frames (bad offset / vector ≥ 32 / bad HDMA magic) only
surface as `GUEST_ERROR` lines in `qemu.log`, not the per-bridge log.

`./remucli run --host` auto-verifies all of the above on startup and
prints pass/fail; failures are non-fatal (NPU still boots for
post-mortem poking).

## `FW_BOOT_DONE` path — cold boot vs. soft reset

There are **two** times the kmd reads `0xFB0D` out of BAR4+0x90, and
they travel different paths in REMU:

**Cold boot (real).** q-sys FreeRTOS runs `bootdone_task` pinned to
chiplet-0 CPU 0. The task crosses its internal gates
(`CL_BOOTDONE_MASK`, etc.) and calls
`bootdone_notify_to_host(PCIE_PF)`, which writes `0xFB0D` to the
chiplet-0 **PF** mailbox's `ISSR[4]` via the Samsung-IPM register
path. `r100-mailbox` egresses that write as an 8-byte frame on the
`issr` chardev, and the host-side `r100-npu-pci` lands it in the
BAR4+0x90 shadow so the driver reads the real value. No stub in the
loop. Ground truth:

```
rg 'Notify Host - PF FW_BOOT_DONE' output/<name>/uart0.log
grep '0x90.*0xfb0d'                 output/<name>/issr.log
```

**Soft reset (CM7 stub, gated).** kmd `rebel_hw_init` always clears
`ISSR[4]` and rings `INTGR0 bit 0` (`REBEL_DOORBELL_SOFT_RESET`)
during probe — even when firmware is already up — then polls
`ISSR[4]` for a fresh `0xFB0D` in `rebel_reset_done`. On silicon the
PCIE_CM7 subcontroller physically resets the CA73 cluster, firmware
reboots from BL1, and `bootdone_task` re-emits `0xFB0D` naturally.
REMU models neither CM7 nor the PMU reset sequence, so the CM7 stub
in `r100_cm7.c` catches `INTGR0 bit 0` and synthesises
`PF.ISSR[4] = R100_FW_BOOT_DONE`, then emits the NPU→host issr frame
so the BAR4 shadow converges. **The synthesis is gated on
`r100_mailbox_fw_boot_done_seen(pf_mailbox)`** — a one-shot latch
flipped only when q-sys's own `bootdone_task` writes `0xFB0D` from
the NPU-MMIO source. Pre-cold-boot SOFT_RESETs are dropped (logged at
LOG_TRACE) so the kmd parks on its own `FW_BOOT_DONE` poll until
q-sys publishes naturally over the existing `issr` chardev.
Post-cold-boot SOFT_RESETs find the latch already set and synthesise
as before. The CM7 stub retires when REMU grows a real CA73
soft-reset model (`docs/roadmap.md` → P8).

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

## x86 Linux guest boot

Without `images/x86_guest/{bzImage,initramfs.cpio.gz}`, the x86 side
idles at SeaBIOS. When staged, `./remucli run --host` auto-wires:

- `-kernel <bzImage> -initrd <initramfs> -append "console=ttyS0 rdinit=/init earlyprintk=serial"`
- `-fsdev local,id=remu,path=guest/,security_model=none -device virtio-9p-pci,fsdev=remu,mount_tag=remu`
- `-cpu max` (stock kmd is `-march=native` with BMI2 instructions
  that trap `#UD` on `qemu64`).

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

Healthy `output/<name>/host/serial.log`:

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

## BD lifecycle on `--host` — verifying the loop

q-cp on CP0 owns the entire BD walk: `hq_task` reads the BD via the
chiplet-0 PCIe outbound iATU window (host-ram alias);
`cb_task → cb_parse_*` walks the CB packet stream; engine HAL drivers
program `r100-rbdma` / `r100-hdma` MMIO; `cb_complete` writes
`FUNC_SCRATCH` through the cfg-mirror alias and calls
`pcie_msix_trigger` for MSI-X.

```
$ tail output/<name>/host/serial.log
… rbln_queue_test: queue test ok …                # silent on success; this line on the kmd's debug build only

$ tail output/<name>/hils.log
[HILS … cpu=0 INFO  func=0] cb_parse_write_data dst=0x50200ffc val=0xcafedead
[HILS … cpu=0 INFO  func=0] cb_complete: send MSIx interrupt to host

$ tail output/<name>/msix.log
msix off=0xffc db_data=0x0 vector=0 status=ok count=1

$ python3 - <<'EOF'
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("output/<name>/host/monitor.sock")
s.sendall(b"xp /1wx 0xf000200ffc\n"); print(s.recv(4096).decode())
EOF
# 0xf000200ffc: 0xcafedead   ← matches RBLN_MAGIC_CODE
#                              (FUNC_SCRATCH is at BAR2 + 0xFFC; q-cp
#                              wrote it through the shared cfg-shadow
#                              alias, kmd reads it via rebel_cfg_read)
```

Verifying the chiplet-0 outbound iATU stub (P1a):

```
$ grep "Device descriptor\|Queue descriptor\|Context descriptor" \
       output/<name>/hils.log
[HILS … cpu=0 INFO  func=0] Device descriptor addr 0x8002d00000, size 286720
[HILS … cpu=0 INFO  func=0] Queue descriptor  addr 0x8002d000ec, size 32
[HILS … cpu=0 INFO  func=0] Context descriptor addr 0x8002d0012c, size 1112

$ python3 - <<'EOF' && echo OK
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("output/<name>/npu/monitor.sock")
s.sendall(b"xp /4wx 0x102000c0\n")
print(s.recv(4096).decode())
EOF
# 0x102000c0: 0x02d00000 0x00000080 0x00046000 0x00000000
#   ^ DDH_BASE_LO (= kmd's coherent DMA addr, no HOST_PHYS_BASE)
#                ^ DDH_BASE_HI = 0x80 (HOST_PHYS_BASE >> 32)
#                              ^ DDH_SIZE = 0x46000
```

Failure modes:

- `rbln_queue_test: command-queue-0 has not same value, 0 != cafedead`
  on the host serial — q-cp wrote `FUNC_SCRATCH` but the host
  doesn't see it. Check that both QEMUs `mmap` the
  `/dev/shm/remu-<name>/cfg-shadow` file (`info mtree -f` on each
  must show the `r100.bar2.cfg.alias` / `r100.cm7.cfg-mirror.alias`
  subregion).
- `Device descriptor addr 0, size 0` on q-cp's hils log even though
  the kmd dmesg shows `rbln_alloc_dma_buffer ok` — both QEMUs aren't
  `mmap`ing the same `cfg-shadow` file. Check
  `/proc/<host-pid>/maps` and `/proc/<npu-pid>/maps`.
- `hdma … status=dma-fail` on a `OP_READ_REQ` from `r100-hdma`'s
  `0x80..0xBF` channel partition — host's `pci_dma_read` returned
  non-`MEMTX_OK` for a bus address q-cp's HDMA-LL chain
  dereferenced. Usual cause: kmd has already freed / unmapped the
  DMA buffer.

## DNC kickoff trace

Active path on `INTGR1` bit `qid`: q-cp's CP1 worker calls
`dnc_send_task` after popping a real `cmd_descr` from the mailbox;
the writes land on `r100-dnc-cluster`, the final 4-byte store to
slot+0x81C with `itdone=1` triggers a BH that synthesises a
`dnc_reg_done_passage` at slot+0xA00 and pulses the matching DNC GIC
SPI from `r100_dnc_intid()`.

Log signatures (NPU side):

| Trace line | Means |
|---|---|
| `r100-dnc cl=C dcl=D slot=S kickoff dnc_id=N cmd_type=T desc_id=0x... → intid=I spi=I-32 fired=...` | q-cp reached `dnc_send_task` and wrote DESC_CFG1 with itdone=1; r100-dnc latched the done passage and pulsed the DNC SPI. |
| `r100-dnc cl=C dcl=D slot=S: completion FIFO full` | Pending completions exceeded `DNC_DONE_FIFO_DEPTH=32`. Almost certainly indicates an IRQ-storm bug. |

Common failure: the kickoff trace never fires even though the mailbox
shows `MBTQ_PI_IDX` advancing. That means q-cp's CP1 worker isn't
reaching `dnc_send_task` for the pushed task. Check via CP1 GDB
(`tests/scripts/gdb_inspect_cp1.gdb`): is the worker thread on a DNC
range that includes DNC0?

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
| FreeRTOS banner prints then hangs; a CPU sits in `vApplicationPassiveIdleHook` | gtimer outputs not wired to GIC PPI inputs (CNTVIRQ never delivered, `vTaskDelay` never returns) | `r100_soc.c` — the `qdev_connect_gpio_out(cpudev, GTIMER_*, ... )` block per CPU |
| `--host` hangs after `Start FreeRTOS scheduler`; host sees fake `FW_BOOT_DONE` (via CM7 stub) while NPU uart0 is silent | SPI INTID mis-wiring — `qdev_get_gpio_in(gic, N)` raises INTID `N+32`. Always pass `R100_INTID_TO_GIC_SPI_GPIO(INTID)`. | `r100_soc.c` mailbox wiring + `remu_addrmap.h:R100_INTID_TO_GIC_SPI_GPIO` |

FW ground truth lives in `external/ssw-bundle/products/rebel/q/sys/bootloader/cp/tf-a/`
— cross-reference via `CLAUDE.md` ("Key external files").

## Open issue: P10 — cb[1] HDMA LL chain reads all zeros

**Status.** Open. `./remucli test p10` times out at 180 s on every
run. Excluded from `./remucli test` defaults via `in_default=False`
in `TEST_REGISTRY`. The pre-P11 "cb[1] SMMU translate fault" mode
is gone. The pre-fix "x86 guest OOM during `rbln_register_p2pdma`"
mode is also gone — `tests/p10_umd_smoke_test.py` and
`tests/p10_qcp_gdb_probe.py` now both pass `--host-mem 4G`, and
`SHM_SIZE_DEFAULT` / `R100_DRAM_INIT_SIZE` / `R100_BAR0_DDR_SIZE` are
all aliased to `R100_RBLN_DRAM_SIZE = 36 GB` (one chiplet's full DRAM
= real CR03 BAR0) so the kmd's MMU page-table pool at
`0x51800000+0x2800000` and q-cp's user-allocator hits in the
`0x40000000+` PF system IPA window are both addressable on the shared
shm splice across the full BAR0 — there are no host-private lazy-RAM
holes inside the silicon-visible window (see `CLAUDE.md` → "M5 splice
points" + the inline comment at
`src/include/r100/remu_addrmap.h:R100_RBLN_DRAM_SIZE`). The 36 GB
ftruncate is sparse on tmpfs so phase-1 boot tests (m5..p5, p11) still
only commit ~tens of MB; the only host requirement is `/dev/shm` with
≥ 36 GB available — `df -h /dev/shm` shows the cap, default tmpfs is
50 % of host RAM. `--shm-size <bytes>` overrides on tighter hosts and
the shorter splice falls back to a host-private `bar0_tail` /
NPU-private `dram_tail` for the rest of BAR0.

**Symptom (post-memory-fix).** `command_submission -y 5` from the
umd staged into the guest by `guest/build-umd.sh` submits two CBs
to queue 0. cb[0] completes — `output/<name>/npu/cfg.log` /
`xp /1wx 0xf000200ffc` shows `FUNC_SCRATCH = 0xcafedead` and
`output/<name>/host/msix.log` records one MSI-X frame. cb[1] then
stalls. The kmd's TDR fires (`queue_timedout: command-queue-0 TDR 1
Qseq 1/1/2`), then `rsd_device_reset` runs the dump+unload+reset
cascade documented below, then a second `rebel_queue_init` from
`rsd_sched_device_init` busy-polls forever (the
`jiffies_to_usecs` side-bug below) and the host watchdog
eventually prints `soft lockup ... rebel_hw_init+0x439`.

**Proximate cause.** `output/<name>/hdma.log` shows q-cp kicked HDMA
RD ch0 with `llp=0x42b86740` (NPU-local PA in chiplet-0 DRAM, well
inside the 36 GB shm splice). The walker fetches a 24-byte LL
element at that cursor and reads **all zeros** (`ctrl=0`,
`xsize=0`, `sar=0`, `dar=0`), breaks immediately, signals
completion, and pulses `INT_ID_HDMA = 186` to q-cp. The gdb probe
confirms the same: `monitor xp /16wx 0x42B86700` over a 192-byte
window centred on the cursor returns 48 contiguous zero words. So
either q-cp programmed `regs->llp = 0x42b86740` without ever
populating the chain at that NPU IPA, or the CA73 stage-1 stores
the chain elsewhere than where `CPVA_TO_PA(CHIPLET_ID, chan->mem->addr)`
claims. Distinguishing the two requires walking q-cp's heap on the
gdb probe — there's a follow-on note below.

`output/<name>/npu/qemu.stderr.log` shows **no** `TRANSLATE FAULT`
or `RBDMA OTO` activity for the cb[1] window — both SMMU (P11) and
RBDMA (P4B) are silent on the cb[1] slot, consistent with the
"HDMA fired but transferred 0 bytes, downstream RBDMA never gated
in" reading. cb[0] uses `PACKET_WRITE_DATA` and goes straight
through the cfg-mirror trap to `FUNC_SCRATCH` so it doesn't touch
HDMA at all (which is why cb[0] passes).

Diagnostic recipe:

```
./remucli fw-build                                   # ELFs needed by gdb
./remucli test p10                                   # reproduces the hang
python3 tests/p10_qcp_gdb_probe.py                   # snapshot cb[1] state
less output/p10-umd/hdma.log                         # ll_walk_read elem=1 ctrl=0
less output/p10-debug/qcp-bt-cb1.txt                 # gdb dump
```

The probe pauses 1.5 s after the second `INTGR1=0x1` doorbell —
exactly between cb[1] doorbell delivery and TDR — so the snapshot
catches q-cp with `cb_run_cnt = 1`, `cq[0].ci = 1`, `cq[0].pi = 2`,
all `cb_mgr.{ready,wait}_list` empty, and `cb_task` / `hdma_task`
both suspended in `xTaskNotifyWait`. None of the running threads
in `info threads` is in cb_task or hdma_task — they're all
runtime-idle CSes / DNC fetch workers. Override the settle window
with `REMU_P10_DEBUG_DELAY_S=8.0` to catch the unload cascade
instead.

Two more diagnostic-only probes (no `TEST_REGISTRY` entry):

- `tests/p10_cfgshadow_probe.py` — boots, waits for the first
  QUEUE_INIT doorbell, samples `cfg-shadow` from three vantage points
  (direct shm read, NPU `xp`, host `xp`) to verify the alias is
  working.

**Follow-on hypothesis (next debugging step).** The cb[1] HDMA path
in q-cp is `cb_parse → cmd_descr_hdma → hdma_ll_trigger →
hdma_ch_trigger`, which writes `regs->llp = chan->desc =
hdma_pkt->addr` (the address from the `PACKET_LINKED_DMA` packet
the kmd emitted). For `cmdgen_rebel_record_rbdma` simple_copy
flows the LL chain is **not** populated by the kmd — it's emitted
by q-cp's own `record_lli` / `record_llp` helpers writing into
`chan->mem->addr`. If q-cp set `chan->desc` to the packet addr but
emitted the chain into `chan->mem`, the two will only line up if
the PACKET_LINKED_DMA's addr was reused as the mem buffer. Spot
check: dump `chan->mem->addr` and `chan->mem->used_size` on the
hdma channel q-cp picked, compare to `regs->llp.{lsb,msb}` and to
`hdma_pkt->addr` from the cb body, and check whether
`CPVA_TO_PA(CHIPLET_ID, chan->mem->addr) == llp` modulo the
chiplet-base offset. The gdb probe's `print hdma_mgr` already pulls
the htask handle; the missing piece is walking from the active
hdma_chan back to its mem pool — cheapest route is to add a `print
hdev` there once the q-cp source is mapped to the running ELF.

**Side note — KMD `readl_poll_timeout_atomic` argument unit
mismatch.** `rebel.c:700` calls
`readl_poll_timeout_atomic(..., 10, jiffies_to_usecs(RBLN_REBEL_TASK_DONE_US))`
where `RBLN_REBEL_TASK_DONE_US = 3 * 1000000` is **already in
microseconds** but the macro is being run through
`jiffies_to_usecs()` again. With HZ=250 the inner conversion
expands to ~12000 s, so the kmd's "3 s queue_init timeout" is
actually a ~3-hour busy-loop on TCG. This is a stock-KMD bug,
upstream — not REMU. Means the soft lockup runs for the full 180 s
test budget instead of the 6 s wall the kmd was meant to wait.
Tracked here so future people picking up P10 don't try to "fix" the
timeout from the REMU side.

**Side note — TDR `URG_EVENT_UNLOAD` register-dump cascade.** When
the kmd's TDR fires (because cb[1] never completed), the
`rebel_quiesce_device` path writes `REBEL_EVENT_DUMP = 4` to ISSR[6]
and rings INTGR1 bit 6. The kmd-side `REBEL_EVENT_DUMP = 4`
intentionally aliases the q-cp-side `URG_EVENT_UNLOAD = 4`, so
q-cp's `handle_unload_event` runs `DNC_DUMP_ESSENTIAL` +
`RBDMA_DUMP_CDMA` + `rbcm_dump_chiplet_incomplete_ttreg` — a
~700-line register dump peppered with `mdelay(500000)` busy-waits
that block the doorbell ISR on CP0.cpu0 for many seconds under TCG.
The kmd then proceeds with `rebel_soft_reset`, REMU's CM7 stub
synthesises `FW_BOOT_DONE` (because we don't model a real CA73
cluster reset — see roadmap → P8), the kmd thinks the device is
fresh, and `rebel_hw_init → rebel_queue_init` rings `INTGR1` bit 7
(QUEUE_INIT). q-cp's `hq_task` is still mid-dump, never gets to
re-run `hq_init`, `desc->init_done` is never written back through
the P1a outbound iATU, and the busy-loop above eats the rest of
the 180 s test budget. Fixing the underlying cb[1] path removes
the entire cascade.

## Cleanup

```
rm -rf output/run-20260101-*    # drop older timestamped runs
rm -rf output/                  # nuke everything; next run recreates
```
