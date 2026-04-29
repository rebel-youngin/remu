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
  smmu.log      # chiplet 0's r100-smmu — translate / STE decode /
                # PT-walk dispatch / CMDQ op / eventq emit / GERROR raise
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

## SMMU debug surface (P10 / P11 post-mortems)

`smmu.log` captures every interesting event on chiplet 0's
`r100-smmu` — written one line at a time by the device's
`debug-chardev` backend. Always-on once the file is opened (no `-d /
--trace` dependency); silent on standalone `./remucli run` (no
`--host`). Format mirrors `rbdma.log` / `hdma.log` so a single
grep across `output/<run>/*.log` correlates events on the same BD
lifecycle:

```
smmu cl=0 CR0 0x0→0x4 smmuen=0→0 eventqen=0→1 cmdqen=0→0
smmu cl=0 STRTAB_BASE base_pa=0x14000000 log2size=5 fmt=LINEAR n_sids=32
smmu cl=0 cmdq idx=0 op=0x04 CFGI_STE_RANGE cmd[0]=0x4 cmd[1]=0x5
smmu cl=0 xlate_in sid=5 ssid=0 dva=0x100000000 rd cr0_smmuen=1 strtab_base_pa=0x14000000
smmu cl=0 ste sid=5 v=1 cfg=ALL_TRANS s2t0sz=25 s2sl0=1 s2tg=0 s2ps=5 s2aa64=1 s2affd=1 s2r=0 vmid=0 s2ttb=0x0000000006000000
smmu cl=0 ptw sid=5 dva=0x100000000 vttb=0x6000000 tsz=25 sl0=1 gran=12 eff_ps=48 perm=rd
smmu cl=0 xlate_out sid=5 dva=0x100000000 ok pa=0x7000000 page_base=0x7000000 mask=0xfff
```

Faults look the same but with a `FAULT <kind>` token — easy to grep
for `xlate_out.*FAULT` to find every walk that didn't translate, or
`eventq_emit` to find every event the FW handler should have seen.

### Companion scripts

Two thin Python helpers under `tests/scripts/` complete the offline
debug loop. They mmap the `/dev/shm/remu-<name>/` backends read-only —
no monitor / gdbstub round trip, no race against the live guest
beyond ordinary tearing.

`mem_dump.py` is region-agnostic. Anything routed through chiplet-0
DRAM (BD descriptors, queue_descs, command buffers, RBDMA OTO src/dst,
SMMU stream-tables, page tables, q-cp's stack/heap, …) lands in
`/dev/shm/remu-<name>/remu-shm` and can be dumped from there:

```
# xxd-style hex view of 256 B at chiplet-PA 0x14000000 (FW STRTAB_BASE):
./tests/scripts/mem_dump.py -n my-run -o 0x14000000 -s 256

# 64-bit LE words view at the same offset:
./tests/scripts/mem_dump.py -n my-run -o 0x14000000 -s 256 -f u64

# Dump host-ram (x86 guest's physical memory; same backend the kmd
# allocates coherent DMA from, also aliased over the chiplet-0
# outbound iATU window):
./tests/scripts/mem_dump.py -n my-run -r host-ram -o 0x10000000 -s 4096

# List which backends exist for a run:
./tests/scripts/mem_dump.py -n my-run --list
```

`smmu_decode.py` is a pure-Python decoder for SMMU-v3.2 bytes — STE
(64 B), stage-2 PTE (8 B), or a 3-level walk against a live shm
file. It's the natural follow-up when `smmu.log` says "fault" or
"sid=N config=…" and you want to see the actual bytes:

```
# Pull SID 5's STE out of the live stream-table and decode it:
./tests/scripts/mem_dump.py -n my-run -o $((0x14000000 + 5*64)) \
    -s 64 -f raw -O /tmp/sid5.bin
./tests/scripts/smmu_decode.py ste --input /tmp/sid5.bin --sid 5

# Replay the page-table walk for an IPA the smmu.log line called
# out, against the same shm the device walker reads (fields come
# straight from the `ste sid=5 …` and `ptw …` lines):
./tests/scripts/smmu_decode.py walk \
    --shm /dev/shm/remu-my-run/remu-shm \
    --vttb 0x06000000 --tsz 25 --sl0 1 --granule 12 \
    --ipa 0x100000000

# Decode a whole 4 KB L3 page (512 PTEs):
./tests/scripts/mem_dump.py -n my-run -o 0x06002000 -s 4096 \
    -f raw -O /tmp/l3.bin
./tests/scripts/smmu_decode.py pte --input /tmp/l3.bin --level 3
```

Both scripts read PROT_READ — they will never modify the run state.
Pass `mem_dump.py --snapshot` to copy bytes into Python before
formatting if you're chasing a tearing race against a live store.

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

## Open issue: P10 — cb[0] complete; cb[1] gated on upstream KMD timeout bug

**Status.** Three REMU-side layers landed (CM7 stub for
`CR0.SMMUEN`, honest stage-1 walking via FW bypass STE 8,
direction-based LLI dispatch in `r100_hdma_walk_ll`); cb[0] now
runs end-to-end through the silicon-accurate path with one MSI-X
delivery. `./remucli test p10` still times out at 180 s, but the
failure mode has moved well past every HDMA / SMMU / cfg-mirror /
CM7-handshake issue that held earlier versions of this entry. The
remaining hang is in the kmd's `rebel_hw_init+0x439`
busy-poll — the documented stock-KMD
`readl_poll_timeout_atomic(..., jiffies_to_usecs(RBLN_REBEL_TASK_DONE_US))`
unit-mismatch (the inner conversion turns a 3 s wait into ~3 h
under TCG; stock-KMD bug, not REMU). cb[1] is unobservable on the
NPU side because the kmd never makes it past the post-cb[0]
re-init busy-loop into the second `command_submission` step.

**What just landed (this commit) — direction-based LLI dispatch.**
`r100_hdma_walk_ll` no longer classifies LLI fields by `addr ≷
REMU_HOST_PHYS_BASE`. The dispatch now gates on
`r100_smmu_enabled(s->smmu)` — a new public CR0.SMMUEN getter on
`r100-smmu`:

- **SMMUEN=0** (p5 test, NPU-only smoke, very early boot before
  q-sys's `notify_dram_init_done`): D2D — both LLI endpoints are
  chiplet-local DRAM and the walker runs `address_space_read` →
  `address_space_write`. Confirmed against `output/p5-hdma/smmu.log`
  showing the gdbstub kick fires while `cr0_smmuen=0`.
- **SMMUEN=1** (production: q-cp drove `cmd_descr_hdma` after the
  CM7 stub flipped CR0.SMMUEN): direction-based.
    - `dir == WR` ⇒ SAR translates through SID 8 (FW bypass STE),
      then `address_space_read` feeds chunked `OP_WRITE` frames;
      DAR is the raw host PCIe IOVA the kmd published (per
      `kmd/.../memory.c:rbln_dma_host_convert`, "MAP_USERPTR uses
      the raw IOVA") and the host's `pci_dma_write` resolves it.
    - `dir == RD` ⇒ SAR is the raw host IOVA;
      `r100_hdma_emit_read_req` + park on the channel's resp_cond
      via `qemu_cond_wait_bql()` (BQL released so the chardev
      iothread can deliver `OP_READ_RESP`); DAR translates
      through SID 8 and `address_space_write`s the payload.

Verification (`output/p10-umd/hdma.log`): cb[0]'s 3-LLI chain
(2 MB kernel copy + 8 KB init + 4 KB CS) walks cleanly with 515
`OP_READ_REQ` / `OP_READ_RESP` round-trips, every chunk
`status=ok`. `ll_walk_end ... last_seen=1`,
`signal_completion completions=1`. `output/p10-umd/msix.log`
shows one MSI-X frame at vector 0 — q-cp's `cb_complete →
pcie_msix_trigger` reached the host. SMMU traces show clean
`xlate_out sid=0 dva=0x42b86640 stage-2 ok` for every cursor
walk and `xlate_out sid=8 dva=0x140000000 ok(s1)` for every
DAR translate, no faults across the full chain. m5..p5 + p11 +
p11b regression green throughout.

**Why direction-based is silicon-equivalent.** On real silicon
the HDMA controller's outbound bus connects directly to the
chiplet-0 PCIe RC for host-bound TLPs and to the chiplet's NoC
(SMMU-600 in front) for device-local accesses; the address itself
selects the route. REMU has no PCIe RC and no DW iATU on the
HDMA path, so the address heuristic doesn't carry over —
production q-cp uses HDMA WR strictly for NPU→host and HDMA RD
strictly for host→NPU on the `cmd_descr_hdma` MAP_USERPTR copy
path, so direction is the equivalent silicon-truthful selector.
A future D2D-HDMA workload (none today) would need either a
marker on the LLI or splitting `r100-hdma` per-channel by
`SM_HDMA_SID_LUT`.

**What was wrong before.** The previous version of this entry
described the front-edge as "REMU classifies host-bound only when
`addr >= REMU_HOST_PHYS_BASE` (`0x8000000000`)" — but the kmd
publishes raw PCIe IOVAs (`0xec00000`-shaped) which fall well
below the marker, so they fell through to the chiplet-local SMMU.
SID 8 walked the FW's bypass PT honestly and faulted on the
`0x40000000..0x80_0000_0000`-window mismatch — silicon-truthful,
but the address was never meant for the chiplet's SMMU in the
first place. That entire heuristic is **gone**; the address never
reaches the SMMU on the host-bound leg now.

The pre-fix "x86 guest OOM during `rbln_register_p2pdma`" is gone —
`tests/p10_umd_smoke_test.py` and `tests/p10_qcp_gdb_probe.py` both
pass `--host-mem 4G`, and `SHM_SIZE_DEFAULT` / `R100_DRAM_INIT_SIZE`
/ `R100_BAR0_DDR_SIZE` are all aliased to
`R100_RBLN_DRAM_SIZE = 36 GB` (one chiplet's full DRAM = real CR03
BAR0) so the kmd's MMU page-table pool at `0x51800000+0x2800000` and
q-cp's user-allocator hits in the `0x40000000+` PF system IPA window
are both addressable on the shared shm splice across the full BAR0
— there are no host-private lazy-RAM holes inside the silicon-visible
window (see `CLAUDE.md` → "M5 splice points" + the inline comment at
`src/include/r100/remu_addrmap.h:R100_RBLN_DRAM_SIZE`). The 36 GB
ftruncate is sparse on tmpfs so phase-1 boot tests (m5..p5, p11) still
only commit ~tens of MB; the only host requirement is `/dev/shm` with
≥ 36 GB available — `df -h /dev/shm` shows the cap, default tmpfs is
50 % of host RAM. `--shm-size <bytes>` overrides on tighter hosts and
the shorter splice falls back to a host-private `bar0_tail` /
NPU-private `dram_tail` for the rest of BAR0.

**Symptom (current).** `command_submission -y 5` from the umd
staged into the guest by `guest/build-umd.sh` submits two CBs to
queue 0. cb[0] now runs end-to-end: small-packet
`PACKET_WRITE_DATA` lands `0xcafedead` on `FUNC_SCRATCH` via the
cfg-mirror trap; the `PACKET_LINKED_DMA` 3-LLI chain (2 MB
kernel copy + 8 KB init + 4 KB CS) is fetched through SMMU
stage-2 on SID 0 (LLP IPA → PA), each LLI's DAR walks SID 8
through the FW bypass PT (`0x140000000`-shaped → identity), and
the SAR side fires `OP_READ_REQ` over the host-leg `hdma`
chardev — 515 round-trips, every chunk `status=ok`. q-cp's
`hdma_done_handler` advances cb[0]; `cb_complete` writes the
BD flags, advances `queue_desc.ci`, publishes `FUNC_SCRATCH`,
and calls `pcie_msix_trigger`. `output/p10-umd/msix.log` shows
one MSI-X frame at vector 0 — the host receives the cb[0]
completion. The hang is now in the kmd's `rebel_hw_init+0x439`
post-cb[0] busy-loop; cb[1] is unobservable on the NPU side
because the kmd never exits the timeout cascade. Diagnostic
shape: `[  119.292556] queue_timedout: command-queue-0 TDR 1
Qseq 1/1/2 job cnt 0`, then watchdog soft-lockup blames
`rebel_hw_init+0x439`. Side-note section "KMD
`readl_poll_timeout_atomic` argument unit mismatch" below has
the upstream KMD bug detail.

**Diagnostic surface (cb[0] verification).** When picking this
back up, the relevant lines on a clean run:

```
# output/p10-umd/hdma.log — chain decode + 515 host-leg round-trips
hdma cl=0 ll_walk_read dir=rd ch=0 elem=1 cursor=0x42b8X640 \
        ctrl=0x00000001 xsize=2097152 sar=0x0b800000 dar=0x140000000
hdma cl=0 ll_lli      dir=rd ch=0 elem=1 sar=0x0b800000 dar=0x140000000 \
        size=2097152 route=rd-host-leg
hdma cl=0 tx op=READ_REQ req_id=0xa0 dst=0xb800000 len=4096 tag=ll-rd ...
hdma cl=0 rx op=READ_RESP req_id=0xa0 dst=0xb800000 len=4096 tag=resp ...
... [×512 chunks for elem=1, +×2 for elem=2, +×1 for elem=3] ...
hdma cl=0 ll_walk_end  dir=rd ch=0 elems=3 last_seen=1 cursor=0x42b8X670
hdma cl=0 signal_completion dir=rd ch=0 pending_mask=0x10000 completions=1

# output/p10-umd/msix.log — q-cp's cb_complete reaches the host
msix off=0xffc db_data=0x0 vector=0 status=ok count=1

# output/p10-umd/smmu.log — every walk clean
smmu cl=0 xlate_out sid=0 dva=0x42b8X640 ok pa=0x02b8X640        # cursor (stage-2)
smmu cl=0 xlate_out sid=8 dva=0x140000000 ok(s1) pa=0x140000000  # DAR (bypass stage-1)
```

The `dva=0xb800000`-style raw IOVA we used to fault on never
reaches the SMMU now — it's routed straight to the host's
`pci_dma_read` over the chardev.

**Proximate cause (current — kmd-side).** The hang trace is in
`rebellions.ko`'s `rebel_hw_init+0x439` (resolved via the
`Workqueue: rsd_device_reset_wq rsd_sched_device_init`
`Call Trace` in `output/p10-umd/host/serial.log`); on stock kmd
with HZ=250 this is the `readl_poll_timeout_atomic(...,
jiffies_to_usecs(RBLN_REBEL_TASK_DONE_US))` that the side-note
below explains. The watchdog prints `soft lockup ... CPU#0 stuck
for 26s` then `52s` and so on until the 180 s test budget
expires.

**Root cause for the LL-chain pointer (now fixed).**
q-cp's `cb_parse_linked_dma`
(`external/ssw-bundle/products/rebel/q/cp/src/cb_mgr/command_buffer_manager.c:509`)
transforms the kmd-supplied LL chain pointer:

```
buf = rl_malloc(pkt_size_ext + sizeof(struct packet_linked_dma));
memcpy(buf + total_size, (void *)addr, size);   /* copy chain into buf */
flush_dcache_range(buf, pkt_size_ext);
internal_pkt = (struct packet_linked_dma *)((uint64_t)buf + pkt_size_ext);
*internal_pkt = *pkt;
pkt = internal_pkt;
/* System region from CPVA to IPA */
pkt->addr = (uint64_t)buf & (FREERTOS_VA_OFFSET - 1);
pkt->addr += (func_id > 0) ? VF_SYSTEM_HIDDEN_OFFSET : PF_SYSTEM_IPA_BASE;
```

For PF (`func_id == 0`), `PF_SYSTEM_IPA_BASE = 0x40000000`
(`external/ssw-bundle/products/rebel/q/cp/include/hal/ptw.h:54`).
So the value q-cp writes to `regs->llp` is **the buffer's PA + 0x40000000**
— a PCIe-PF system **IPA**, not a PA — and the HDMA AXI fetch
relies on the chiplet-0 SMMU to walk it through a stage-2 PT to
the real backing PA. q-cp's `ptw_init_smmu_s2(num_vfs=0)`
(`external/ssw-bundle/products/rebel/q/cp/src/hal/common/ptw.c:642`,
called from `host_queue_manager.c:hq_init` after the first
QUEUE_INIT doorbell) builds the matching aarch64 stage-2 PT at
`PF_S2TT_ADDR = 0x14080000`, mapping IPA
`0x40000000..0x80000000` → PA `0x00000000..0x40000000`. P11's v1
stage-2 walker (`r100_smmu.c:1109`) consumes `STE.Config =
ALL_TRANS` correctly and resolves `0x42b86640` → `0x02b86640`.
**That part works once `CR0.SMMUEN = 1`.**

**The CR0 stub (the fix this entry pivots on).** All those
STE/S2TTB updates only matter if the TCU is powered on, and FW
only flips `CR0.SMMUEN` via a CM7 mailbox handshake REMU doesn't
model. Pre-fix:

```
external/ssw-bundle/products/rebel/q/sys/drivers/smmu/smmu.c:875
static inline void notify_dram_init_done(void)
{
    uint32_t val = 0xFB0D;
    int timeout = ACK_TIMEOUT;
    ipm_samsung_write(IDX_MAILBOX_CP0_M4, 0, &val, sizeof(val), CM7_DRAM_INIT_DONE_CHANNEL);
    ipm_samsung_send (IDX_MAILBOX_CP0_M4, 0, CM7_DRAM_INIT_DONE_CHANNEL, CPU1);
    do {
        ipm_samsung_receive(IDX_MAILBOX_CP0_M4, &val, sizeof(val), CM7_DRAM_INIT_DONE_CHANNEL);
    } while (val && timeout--);
    if (timeout <= 0) {
        RLOG_ERR("Time out to notify DRAM init done.\n");
        smmu_enable();
    }
}
```

`MAILBOX_CP0_M4` was unmapped lazy RAM — `ipm_samsung_write` stores
via the cross-chiplet PA path (`base + target_id*OFFSET -
FREERTOS_VA_OFFSET`), `ipm_samsung_receive` reads via the
FREERTOS-VA path (`base` directly). Those land on different PAs in
the unmapped region, so the receive side reads `0` on the first
iteration, the loop exits with `val == 0 && timeout > 0`, the
in-tree `smmu_enable()` fallback is **not** called, and
`CR0.SMMUEN` stays `0`. q-cp then emits PF-IPA LLPs
(`PA + 0x40000000`) into a walker stuck in identity bypass.
Every HDMA RD walk read zeros.

**Why this is chiplet-0-only.** PCIE_CM7 is on chiplet 0 (the
chiplet with the PCIe block). Chiplets 1..3 have no PCIe and no
CM7, so q-sys's `smmu_init` branches:

```
external/ssw-bundle/products/rebel/q/sys/drivers/smmu/smmu.c:932-935
if (CHIPLET_ID == CHIPLET_ID0)
    notify_dram_init_done();   /* chiplet 0: wait for CM7 ack */
else
    smmu_enable();             /* chiplets 1..3: MMIO-write CR0 */
```

On chiplets 1..3 the in-tree `smmu_enable()` writes `CR0.SMMUEN = 1`
to that chiplet's `r100-smmu` MMIO directly — REMU's normal write
handler already serves it. Worker chiplets have always had SMMU on.
The pre-fix all-zero LL bug was chiplet-0-only because q-cp's
HDMA / RBDMA / DNC tasks all run on chiplet 0 (where the
umd-submitted CBs land), and only that one SMMU was stuck off.

The fix is a minimal **CM7 mailbox stub** on the chiplet-0
`MAILBOX_CP0_M4` block, opted in via two new `r100-mailbox`
properties (`cm7-stub` + `cm7-smmu-base`) wired in `r100_soc.c`.
ISSR reads always return `0` to preserve the lazy-RAM-via-FREERTOS-VA
"first read returns 0" shape every CM7 poll loop in q-sys depends on
for early exit (notably `rbln_cm7_get_values` /
`rbln_pcie_get_ats_enabled` which the boot path runs immediately
after `notify_dram_init_done`); writes to `INTGR1` bit
`cm7-dram-init-done-channel` (default 11, =
`CM7_DRAM_INIT_DONE_CHANNEL`) with `ISSR[<channel>] == 0xFB0D` poke
`CR0.SMMUEN = 1` at `cm7-smmu-base + 0x20` via the
`ldl_le_phys` / `stl_le_phys` RMW pair (so the existing
`CMDQEN` / `EVENTQEN` bits set earlier by `smmu_enable_queues`
survive) — silicon-equivalent to
`pcie_cp2cm7_callback → dram_init_done_cb → m7_smmu_enable`.
Counter `cm7_dram_init_done_acks` (vmstate-tracked) records
how many times it fires.

**SID split + honest stage-1 walk (landed earlier — historical context).**
`r100_hdma_translate` takes a `sid` parameter; the LL chain
*cursor* uses `R100_HDMA_SMMU_SID_LL_PTR = 0` (stage-2 ALL_TRANS
via the user PT) while the LLI's *payload* SAR/DAR use
`R100_HDMA_SMMU_SID_PAYLOAD = 8` (the FW's bypass STE). On the
SMMU side, `r100_smmu`'s S1_TRANS path walks honestly (no v1
identity shortcut): reads STE1.S1DSS, fetches the CD
(`STE0.S1ContextPtr`), decodes the FW's `smmu_init_cd_bypass`
programming (T0SZ=20, TG0=4 KB, IPS=40, EPD1=1, AFFD=1, R=1,
AA64=1) and dispatches QEMU's `smmu_ptw_64_s1` against the FW's
`SMMU_BYPASS_PT` (`smmu_create_bypass_table` builds it from
`bypass_regions[]`, HTID0 identity-maps
`0x40000000..0x8000000000` for the local chiplet). The
CD-validation path drops QEMU's strict `CD_A=1` check because
`smmu_init_cd_bypass` deliberately leaves CD_A unset (the line
`val |= CD0_A;` is commented out in
`q/sys/drivers/smmu/smmu.c:479`); real SMMU-600 silicon doesn't
fault that bypass CD, and rejecting it would defeat the "make
the FW SMMU init's impact real" point. With the new
direction-based dispatch on top, the SID 8 walker's honest
fault on raw IOVAs (`0xec00000`-shaped) **is no longer reached
on cb[0]** — those addresses now go straight to the host-leg
chardev — but the walker remains in place for legitimate
silicon errors (a kmd buffer the FW's bypass mapping doesn't
cover) and surfaces them on the eventq same as silicon.

Diagnostic recipe (post-direction-based-dispatch):

```
./remucli fw-build                                   # ELFs needed by gdb
./remucli test p10                                   # reproduces the rebel_hw_init hang
python3 tests/p10_qcp_gdb_probe.py                   # snapshot CA73 state
less output/p10-umd/hdma.log                         # cb[0] full chain: ll_walk_read elem=1..3,
                                                     # 515 OP_READ_REQ/OP_READ_RESP, ll_walk_end last_seen=1
less output/p10-umd/msix.log                         # one MSI-X frame at vector 0 (cb[0] complete)
less output/p10-umd/smmu.log                         # CR0 0xc→0xd smmuen=0→1; clean xlate_out for sid=0 cursor + sid=8 DAR
less output/p10-umd/host/serial.log                  # rebel_hw_init+0x439 soft lockup
less output/p10-debug/qcp-bt-cb1.txt                 # gdb dump (tag still 'cb1')
```

The probe pauses 1.5 s after the second `INTGR1=0x1` doorbell so
both CBs are queued + the first one mid-flight on q-cp; with
direction-based dispatch landed, the snapshot now catches q-cp
mid-cb[0] with `cb_run_cnt = 1`, `cq[0].ci = 1`, `cq[0].pi = 2`,
both `cb_mgr.{ready,wait}_list` empty, and HDMA's resp_cond
parking on `qemu_cond_wait_bql` for an `OP_READ_RESP` that the
host-side `pci_dma_read` is actively serving. Post-cb[0] the
`hdma_done_handler` runs `cb_complete` and the kmd-side hang
takes over (see "Symptom" / "Side note" below). None of the
running threads in `info threads` is in cb_task or hdma_task —
they're all runtime-idle CSes / DNC fetch workers. Override the
settle window with `REMU_P10_DEBUG_DELAY_S=8.0` to catch the
post-completion / kmd-hang state instead.

Two more diagnostic-only probes (no `TEST_REGISTRY` entry):

- `tests/p10_cfgshadow_probe.py` — boots, waits for the first
  QUEUE_INIT doorbell, samples `cfg-shadow` from three vantage points
  (direct shm read, NPU `xp`, host `xp`) to verify the alias is
  working.

**Fix shape — next hop: kmd timeout cascade.** The
`rebel_hw_init+0x439` busy-loop is the upstream KMD bug
documented in the side-note below. From the REMU side it is
**not currently** something we can shorten — the kmd's loop
runs in atomic context with timer-driven deadline checks, and
TCG's wall-clock progress under `rebel_quiesce_device`'s
`mdelay`s makes the deadline arithmetic blow out by ~1000×.
Three diagnostic options, none of which change REMU device
models:

1. **Local kmd patch** — drop `jiffies_to_usecs()` from the
   `RBLN_REBEL_TASK_DONE_US` call site; the project policy is
   FW-side (`cli/fw-patches/` stays empty), so a `cli/kmd-patches/`
   directory would be a new pattern. Smallest diff, unblocks
   cb[1] visibility, but adds a new patch surface to the
   project.
2. **Bump `--host-cpu` performance hints** — try `-cpu max,kvm=off`
   on a host with KVM available + a TCG-accel hint. KVM execution
   would let `mdelay` advance closer to wall-clock and the
   timeout actually fire at ~3 s. Largest behavioral change.
3. **Wait for an upstream KMD fix.** A bug report against
   `rebellions.ko`'s `rebel.c:700` is the cleanest long-term
   path; tracked here so the next person picking up P10 can
   recognize the loop on sight.

Once one of those clears the kmd-side hang, the next
REMU-visible failure mode (cb[1] dispatch, post-soft-reset
re-init, RBDMA / DNC tasks) comes into view. If subsequent SMMU
translates turn up unrelated faults (missing `T0SZ` / wrong
`SL0` / size mismatches), `smmu.log` logs them as
`xlate_out … FAULT` lines and `r100_smmu_emit_event` publishes
them on the eventq — honest follow-on SMMU v2 bugs, not blocking
unknowns.

The `tests/p10_qcp_gdb_probe.py` artefact stays useful for
chasing later regressions: the ELF-symbol view (`hdma_mgr`,
`hdev[0].chan_info[1].chans[0].desc`, `cfg-shadow` window) lines
up cleanly with the live `xp` reads above and is the fastest way
to confirm "did q-cp pick a different LLP this run, and where did
it actually write the chain" without re-deriving the
`PF_SYSTEM_IPA_BASE` offset from the FW source each time.

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
the kmd's TDR fires for cb[1] (cb[0] now completes; cb[1] is
gated on the kmd-side timeout bug above), the
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
the 180 s test budget. With the direction-based HDMA dispatch
landed, the cascade now triggers strictly downstream of the kmd
`readl_poll_timeout_atomic` unit-mismatch — clearing that bug
(see "Fix shape" above) cuts off the soft-lockup chain at the
source.

## Cleanup

```
rm -rf output/run-20260101-*    # drop older timestamped runs
rm -rf output/                  # nuke everything; next run recreates
```
