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

Phase 2 (`./remucli run --host` — both QEMUs + 5 chardev bridges):

```
output/my-test/
  cmdline.txt + uart{0..3}.log + hils.log   # NPU side (as above)
  doorbell.log  # M6+M8a    — host → NPU frames (INTGR + MAILBOX_BASE)
  msix.log      # M7        — NPU → host MSI-X frames
  issr.log      # M8a       — NPU → host ISSR frames
  cfg.log       # M8b 3b    — host → NPU BAR2 cfg-head writes (DDH_BASE_{LO,HI}, …)
  hdma.log      # M8b 3b+3c — bidirectional HDMA (OP_WRITE / OP_READ_REQ / OP_READ_RESP / OP_CFG_WRITE)
  cm7.log       # M8b 3c    — NPU r100-cm7 BD-done state-machine ASCII trace
  shm -> /dev/shm/remu-my-test/
  npu/
    monitor.sock       # HMP over unix socket
    qemu.stderr.log    # device-model breadcrumbs (only way to see internal state)
    info-{mtree,qtree,mtree-imsix,qtree-issr,qtree-cfg-hdma}.log  # startup bridge checks
  host/
    cmdline.txt + qemu.{stdout,stderr}.log
    serial.log         # SeaBIOS idle, or Linux + kmd dmesg when M8b Stage 2 staged
    monitor.sock       # HMP
    {doorbell,msix,issr,cfg,hdma}.sock  # chardev listeners (host = server, NPU = client)
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
per-test cleanup. Individual invocation: `./remucli test {m5,m6,m7,m8}`
or `python3 tests/mN_*.py`. Each test wraps a `--name` (`m5-flow`,
`m6-doorbell`, `m7-msix`, `m8-issr`), so stale shm / sockets / orphan
QEMUs from a prior attempt are cleaned first.

Bridge-wiring design detail is in `docs/architecture.md` and commit
messages (`7b03328` M1-M4, `72c98f0` M5, `85b76bb`/`500856b` M6,
`db3d1df` M7, `cd24aa9` M8a). Quick on-line checks:

```
# M6: host BAR4 MMIO overlay + NPU cm7/mailbox in qtree
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg 'r100.bar4'
printf 'info qtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg 'r100-(cm7|mailbox)'

# M7: NPU r100-imsix at 0x1BFFFFF000, host BAR5 msix-{table,pba}
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg 'r100-imsix'
printf 'info mtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg 'msix-(table|pba)'

# M8a: ISSR shadow must match between host BAR4 + MAILBOX_BASE and NPU mailbox SFR
#   BAR4 base is in host/info-pci.log (currently 0xfe000000 under SeaBIOS)
printf 'xp /1wx 0xfe000090\nquit\n'    | socat - UNIX-CONNECT:output/<name>/host/monitor.sock
printf 'xp /1wx 0x1ff8160090\nquit\n'  | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock

# M8b 3b/3c: both ends of cfg + hdma + cm7-debug must be bound in qtree
printf 'info qtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg '(cfg|hdma) *='
printf 'info qtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg '(cfg|hdma|cm7-debug)-chardev'
```

`{doorbell,msix,issr,cfg,hdma}.log` are per-bridge ASCII traces
(format `<bus> off=0x... val=0x... [status=...] count=N`, or for
hdma: `hdma <dir> op=<mnemonic> dst=0x... len=... req_id=N status=... count=N`
where `<dir>` is `tx` / `rx` and mnemonics are
`WRITE`/`READ_REQ`/`READ_RESP`/`CFG_WRITE`). `cm7.log` is the
NPU-side `r100-cm7` BD-done state-machine ASCII trace (phase
transitions + `imsix_notify vec=N`).
Empty-after-traffic means the chardev is down — check both QEMUs are
up and the `*.sock` files exist. Rejected frames (bad offset / vector
≥ 32 / bad HDMA magic) only surface as `GUEST_ERROR` lines in
`qemu.log`, not the per-bridge log.

`./remucli run --host` auto-verifies all of the above on startup and
prints pass/fail; failures are non-fatal (NPU still boots for
post-mortem poking).

### `FW_BOOT_DONE` path — cold boot (real) + soft reset (CM7 stub)

There are **two** times the kmd reads `0xFB0D` out of BAR4+0x90, and
they travel different paths in REMU:

**Cold boot (real, post GIC-wiring fix).** q-sys FreeRTOS runs
`bootdone_task` pinned to chiplet-0 CPU 0. The task crosses its
internal gates (`CL_BOOTDONE_MASK`, etc.) and calls
`bootdone_notify_to_host(PCIE_PF)`, which writes `0xFB0D` to the
chiplet-0 **PF** mailbox's `ISSR[4]` via the Samsung-IPM register
path. `r100-mailbox` egresses that write as an 8-byte frame on the
`issr` chardev, and the host-side `r100-npu-pci` lands it in the
BAR4+0x90 shadow so the driver reads the real value. No stub in
the loop (i.e. `r100-cm7`'s SOFT_RESET synthesiser is not involved
on cold boot). Ground truth:

```
rg 'Notify Host - PF FW_BOOT_DONE' output/<name>/uart0.log
grep '0x90.*0xfb0d'                 output/<name>/issr.log
```

If `Notify Host - PF FW_BOOT_DONE` never prints in `--host` mode but
does print in `--host --no-guest-boot`, suspect an IRQ-storm
regression on CA73 CPU 0 — see "Post-mortems" below for the
diagnostic recipe.

**Soft reset (stubbed).** kmd `rebel_hw_init` always clears
`ISSR[4]` and rings `INTGR0 bit 0` (`REBEL_DOORBELL_SOFT_RESET`)
during probe — even when firmware is already up — then polls
`ISSR[4]` for a fresh `0xFB0D` in `rebel_reset_done`. On silicon
the PCIE_CM7 subcontroller physically resets the CA73 cluster,
firmware reboots from BL1, and `bootdone_task` re-emits `0xFB0D`
naturally. REMU models neither CM7 nor the PMU reset sequence, and
the CA73 FreeRTOS build short-circuits the mailbox ISR to
`default_cb` (CMake `FREERTOS_PORT != GCC_ARM_CA73` gate in
`drivers/pcie/pcie_mailbox_callback.c`). The CM7 stub in
`r100_cm7.c` catches `INTGR0 bit 0` and calls
`r100_mailbox_cm7_stub_write_issr(pf_mailbox, 4, 0xFB0D)` directly,
which updates PF.ISSR[4] **and** emits the NPU→host issr frame so
the BAR4 shadow converges. Other bits relay onto VF0.INTGR1 for
CA73 ISR visibility. `pf-mailbox` link wired from `r100_soc.c`.

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

The CM7 stub retires when REMU grows a real CA73 soft-reset model
(`docs/roadmap.md` → Phase 2 pending components).

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

### Stage 3b — QINIT CM7 stub (cfg + hdma chardevs)

After `FW_BOOT_DONE`, kmd's `rebel_hw_init` used to soft-lockup at
t+25 s polling `desc->init_done` in host RAM. Silicon's PCIE_CM7
would have ISR'd on `INTGR1 bit 7` (`REBEL_DOORBELL_QUEUE_INIT`),
read `desc` via HDMA, written back `fw_version` + `init_done = 1`.
REMU models neither CM7 nor HDMA, so Stage 3b splits the CM7 role
cleanly across both QEMUs via two new chardevs:

| chardev | dir | frame | role |
|---------|-----|-------|------|
| `cfg` | host→NPU | 8-byte `(cfg_off, val)` | BAR2 cfg-head mirror. Host-side `r100-npu-pci` traps the 4 KB MMIO window at BAR2 offset `FW_LOGBUF_SIZE` and forwards every write (notably `DDH_BASE_{LO,HI}` at +0xC0/+0xC4) to the NPU's `r100-cm7`, which maintains `cfg_shadow[1024]`. Stage 3c adds the reverse flow too: `OP_CFG_WRITE` HDMA frames from the NPU update the host-side `cfg_mmio_regs[]` directly (used by BD-done to publish `FUNC_SCRATCH`). |
| `hdma` | NPU↔host | 24 B header + payload | Bidirectional DMA. NPU→host: `OP_WRITE` (write descriptor into guest RAM, Stage 3b QINIT + Stage 3c `bd.header |= DONE` / `queue_desc.ci++`), `OP_READ_REQ` (Stage 3c — ask host to `pci_dma_read` a region tagged by `req_id`), `OP_CFG_WRITE` (Stage 3c — push a scalar into host-local `cfg_mmio_regs`). Host→NPU: `OP_READ_RESP` (paired response to a pending `OP_READ_REQ`). See `src/bridge/remu_hdma_proto.h`. |

On `INTGR1 bit 7`, the NPU-side CM7 stub in `src/machine/r100_cm7.c`
(function `r100_cm7_qinit_stub`) reads `DDH_BASE_{LO,HI}` from
`cfg_shadow`, recovers `desc_dma = (hi:lo) - HOST_PHYS_BASE`, and
emits two HDMA writes:

```
HDMA_OP_WRITE  dst = desc_dma + 0x5C  len = 52  payload = "3.remu-stub\0…"
HDMA_OP_WRITE  dst = desc_dma + 0x58  len = 4   payload = 0x00000001
```

Watch the split in action:

```
$ tail output/<name>/cfg.log
cfg off=0xc0 val=0x2b80000 status=ok count=4     # DDH_BASE_LO = desc_dma
cfg off=0xc4 val=0x80       status=ok count=5     # DDH_BASE_HI (HOST_PHYS_BASE>>32)

$ tail output/<name>/doorbell.log
doorbell off=0x1c val=0x80 count=…                # INTGR1 bit 7 = QUEUE_INIT

$ tail output/<name>/hdma.log
hdma tx op=WRITE dst=0x2b8005c len=52 req_id=0 status=ok count=1  # fw_version
hdma tx op=WRITE dst=0x2b80058 len=4  req_id=0 status=ok count=2  # init_done=1
```

The major-version prefix compared by `rbln_device_version_check` is
`"3"` against `"3.remu-stub"` — passes cleanly. When the kmd bumps
past major 3, update `REMU_FW_VERSION_STR` in `r100_cm7.c` (or
replace the stub with a real `OP_READ_REQ` + `OP_READ_RESP` pair
now that the protocol supports it).

After Stage 3b the kmd boots past `hw_init` into `rbln_queue_test`.
Stage 3c closes that loop — see next section.

### Stage 3c — BD-done state machine (r100-cm7)

`rbln_queue_test` in `rebellions.ko` submits a BD that writes a
scalar via `packet_write_data`, then waits on a `dma_fence` for
completion. On silicon, PCIE_CM7 firmware handles the `INTGR1 bit 0`
= `REBEL_DOORBELL_QUEUE_0_START` doorbell: it walks the queue
descriptor, reads each BD, reads the packet payload, applies the
side-effect (here: write into `FUNC_SCRATCH`), marks the BD done,
advances `queue_desc.ci`, and fires an MSI-X on the completion
vector. REMU models this as a file called `r100_cm7.c` that is
finally allowed to live up to its name — no more "doorbell"
misnomer.

**State machine per-queue** (`R100Cm7BdJob`, slot per qid):

```
IDLE
  │ INTGR1 bit <qid>: snapshot pi = ISSR[qid]
  ▼
WAIT_QDESC ── OP_READ_REQ queue_desc[qid] (req_id=qid+1)
  │  OP_READ_RESP: remember ci, ring_dma
  ▼
WAIT_BD ───── OP_READ_REQ bd[wrap(ci)]
  │  OP_READ_RESP: decode
  ▼
WAIT_PKT ──── OP_READ_REQ pkt[bd.addr]
  │  OP_READ_RESP: decode value
  ▼
(commit) OP_CFG_WRITE FUNC_SCRATCH := pkt.value
         OP_WRITE     bd.header |= BD_FLAGS_DONE_MASK
         OP_WRITE     queue_desc[qid].ci = ci + 1
         r100_imsix_notify(vec = qid)
  │ ci + 1 < pi ? loop into WAIT_BD : IDLE
```

`req_id = qid + 1` tags every frame of an in-flight BD job —
`OP_READ_REQ`, the matching `OP_READ_RESP`, and the side-effect
writes (`OP_CFG_WRITE` + both `OP_WRITE`s) — so logs for
concurrent queues stay disentangled. `req_id = 0` is reserved
for the untagged QINIT pair (`OP_WRITE fw_version` and
`OP_WRITE init_done`) emitted once at Stage 3b.

Watch a successful `rbln_queue_test` (from an actual
`--host cm7-smoke` run):

```
$ cat output/<name>/cm7.log
bd-done qid=0 WAIT_QDESC ci=0 pi=1 READ_REQ queue_desc completed=0
bd-done qid=0 WAIT_BD    ci=0 pi=1 READ_REQ bd        completed=0
bd-done qid=0 WAIT_PKT   ci=0 pi=1 READ_REQ pkt       completed=0
bd-done qid=0 IDLE       ci=1 pi=1 imsix_notify       completed=1

$ cat output/<name>/hdma.log   # BD-done slice, req_id=qid+1=1
hdma rx op=READ_REQ  req_id=1 dst=0x...ec len=32 status=ok recv=3 sent=0
hdma tx op=READ_RESP req_id=1 dst=0x...ec len=32 status=ok recv=3 sent=1
hdma rx op=READ_REQ  req_id=1 dst=0x...00 len=24 status=ok recv=4 sent=1
hdma tx op=READ_RESP req_id=1 dst=0x...00 len=24 status=ok recv=4 sent=2
hdma rx op=READ_REQ  req_id=1 dst=0x...00 len=16 status=ok recv=5 sent=2
hdma tx op=READ_RESP req_id=1 dst=0x...00 len=16 status=ok recv=5 sent=3
hdma rx op=CFG_WRITE req_id=1 dst=0xffc   len=4  status=ok recv=6 sent=3
hdma rx op=WRITE     req_id=1 dst=0x...00 len=4  status=ok recv=7 sent=3
hdma rx op=WRITE     req_id=1 dst=0x...00 len=4  status=ok recv=8 sent=3

$ cat output/<name>/msix.log
msix off=0xffc db_data=0x0 vector=0 status=ok count=1

# rbln_queue_test is silent on success — only dev_err's on fail.
$ grep -E 'failed to test queue|fence error|failed to get done flag' \
       output/<name>/host/serial.log
# (no output → PASS)
```

The `recv` / `sent` counter fields on each line are cumulative
across the entire run; the pre-BD QINIT `OP_WRITE` pair (at
`req_id=0`) accounts for `recv={1,2}` before BD-done kicks in.

If the state machine wedges mid-flight, `cm7.log` stops at the
failing phase. Common fixes:

- `WAIT_QDESC` with no RESP: `queue_desc` DMA address mangled.
  Check `DDH_BASE_{LO,HI}` in `cfg.log` and kmd's
  `rbln_dma_host_convert` — `cm7_host_dma()` in `r100_cm7.c` strips
  `REMU_HOST_PHYS_BASE` only when present, so raw `bd->addr` from
  `rbln_queue_test` is passed through unchanged.
- `WAIT_BD` loops but driver still waits: verify `BD_FLAGS_DONE_MASK`
  position — kmd reads the header word via `BD_FLAGS_DONE_MASK`
  bit-field, not a byte flag.
- No `imsix_notify` line: check `r100-cm7`'s `imsix` QOM link is
  populated (auto-verified by `./remucli run --host` startup).
- `qdesc ring_log2 range` in `cm7.log`: kmd is ringing bit 1 of
  INTGR1 for the fwi **message ring** (qid=1), not a command queue.
  Expected — the ring queue's `queue_desc[1].size` field stores
  `ring->max_dw` (absolute count) rather than log2, so the guard
  trips and BD-done exits cleanly. Not in Stage 3c scope.

#### Sanity-checking the stub

The host-side `info qtree` must show `cfg` and `hdma` CharBackends
bound on `r100-npu-pci`. The NPU-side must show `cfg-chardev` and
(if `cm7_log` is wired) `cm7-debug-chardev` non-empty on `r100-cm7`,
and a separate `r100-hdma` device with its own `chardev = "hdma"`
property (M9-1c moved the chardev off cm7 onto the new device,
which models q-cp's dw_hdma_v0 register block — see
`docs/roadmap.md` → M9-1c). `./remucli run --host` auto-verifies
both ends and logs snippets to `host/info-qtree-cfg-hdma.log` and
`npu/info-qtree-cfg-hdma.log`. A missing property means the
`-machine r100-soc,cfg=/hdma=/cm7-debug=…` or
`-device r100-npu-pci,cfg=/hdma=…` option didn't latch — check
`cli/remu_cli.py` chardev id literals.

#### M9-1c — active DNC + HDMA reg block

After M9-1c, every queue-doorbell on `INTGR1` bit `qid` does three
things in parallel: (a) the BD-done state machine walks queue_desc /
BD / packet via `r100-hdma` reads (Stage 3c, unchanged); (b) cm7
synthesises a minimal `cmd_descr` in chiplet-0 private DRAM at
`R100_CMD_DESCR_SYNTH_BASE = 0x20000000` (16-slot ring, 256 B
stride) and pushes a `dnc_one_task` carrying that pointer onto the
M9 mailbox so q-cp's `taskmgr_fetch_dnc_task_master_cp1` can pop a
non-NULL descriptor; (c) — when q-cp's CP1 worker eventually calls
`dnc_send_task` — the writes land on `r100-dnc-cluster`, the final
4-byte store to slot+0x81C with `itdone=1` triggers a BH that
synthesises a `dnc_reg_done_passage` at slot+0xA00 and pulses the
matching DNC GIC SPI from `r100_dnc_intid()`.

Log signatures (NPU side, with cm7-debug + dnc-debug active):

| Trace line | Means |
|---|---|
| `mbtq qid=N slot=M pi=P cmd_descr=0x20000000 status=ok` | `r100_cm7_mbtq_push` published a fresh entry. cmd_descr=0 means the address-space write into the synth slot failed — check that 0x20000000 is in chiplet-0 DRAM range. |
| `r100-dnc cl=C dcl=D slot=S kickoff dnc_id=N cmd_type=T desc_id=0x... → intid=I spi=I-32 fired=...` | q-cp reached `dnc_send_task` and wrote DESC_CFG1 with itdone=1; r100-dnc latched the done passage and pulsed the DNC SPI. |
| `r100-dnc cl=C dcl=D slot=S: completion FIFO full` | Pending completions exceeded `DNC_DONE_FIFO_DEPTH=32`. Almost certainly indicates an IRQ-storm bug (e.g. spurious tlb_invalidate-shaped writes). |

Common failure: the kickoff trace never fires even though mbtq
pushes are landing. That means q-cp's CP1 worker isn't reaching
`dnc_send_task` for our pushed task. First check via CP1 GDB
(`tests/scripts/gdb_inspect_cp1.gdb`): is the worker thread on a
DNC range that includes DNC0? Read 0x20000000 directly and confirm
`cmd_descr_dnc.dnc_task_conf.core_affinity` matches what r100-cm7
wrote (offset 20, byte 0 = 0x01). Bitfield mismatch → adjust the
synth in `r100_cm7_synth_cmd_descr`.

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
| `--host` hangs after `Start FreeRTOS scheduler` but `--host --no-guest-boot` finishes; host sees fake-looking `FW_BOOT_DONE` (via CM7 stub) while NPU uart0 is silent | SPI INTID mis-wiring — `qdev_get_gpio_in(gic, N)` raises INTID `N+32`, so passing the raw INTID (e.g. 185) collides with *another* INTID's handler and the level-triggered line re-fires forever. See "Post-mortems → PCIe mailbox INTID off-by-32" | `r100_soc.c` mailbox wiring + `remu_addrmap.h:R100_INTID_TO_GIC_SPI_GPIO` |

FW ground truth lives in `external/ssw-bundle/products/rebel/q/sys/bootloader/cp/tf-a/`
— cross-reference via `CLAUDE.md` ("Key external files").

## Post-mortems

### PCIe mailbox INTID off-by-32 (IRQ storm on CA73 CPU 0)

**Symptom.** In `--host` (dual-QEMU with Linux guest), the NPU
uart0.log halts on the "`Start FreeRTOS scheduler`" line. The host
driver still prints `[rbln-rbl] FW_BOOT_DONE` in dmesg (because the
CM7 stub forges it), and a casual observer concludes the system is
working. Running `--host --no-guest-boot` makes the NPU finish boot
cleanly and emit `Notify Host - PF FW_BOOT_DONE` — proving the
freeze is triggered by guest doorbell traffic, not by firmware alone.

**Diagnostic recipe.** While the `--host` run is stuck, live-sample
NPU CPU 0 via HMP over the unix socket:

```
python3 <<'EOF' | head -60
import socket, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("output/<name>/npu/monitor.sock")
s.settimeout(2.0)
time.sleep(0.1)
try: s.recv(65536)
except: pass
s.sendall(b"info registers\n")
time.sleep(0.5)
print(s.recv(65536).decode(errors='ignore'))
EOF
```

Fingerprint of the storm:
- `PC` lives inside `FreeRTOS_IRQ_Handler` (resolve via
  `nm -n external/ssw-bundle/products/rebel/q/sys/binaries/FreeRTOS_CP/freertos_kernel.elf`).
- `PSTATE` has `DAIF = 1111` (all interrupts masked — `EL1h`,
  `-ZC-` or similar flag chars).
- `x0` consistently equals **`0xd9`** across samples. `0xd9 = 217` —
  the INTID being acknowledged in the ICC EOI register.
- Multiple samples 500ms apart show the same PC: CPU 0 is not
  advancing.

**Root cause.** QEMU's `arm_gicv3` exposes incoming SPI lines as
`gpio_in[0..num_spi-1]`; `gpio_in[N]` raises INTID `N +
GIC_INTERNAL`, with `GIC_INTERNAL = 32`. So
`qdev_get_gpio_in(gic, 185)` raises INTID **217**, not 185. FW's
`mailbox_data[]` (`drivers/mailbox/mailbox.c`) registers the VF0
mailbox handler for INTID 185 — *and* registers a **different**
mailbox's handler (`IDX_MAILBOX_PERI0_M7_CPU0`) for INTID 217.

When the host rings any VF0 doorbell the sequence is:

1. `r100-mailbox` asserts VF0 group-1 IRQ line high.
2. QEMU raises INTID 217 (the off-by-32 result).
3. CA73 enters `FreeRTOS_IRQ_Handler` → `ipm_samsung_isr`.
4. ISR looks up INTID 217 in `mailbox_data[]`, finds the PERI0-M7
   entry, reads PERI0-M7's `INTSR` (= 0, because PERI0-M7 isn't
   the caller), clears nothing, returns.
5. VF0's group-1 pending bit is still set → line still high → GIC
   immediately re-raises INTID 217. Goto step 3.

CPU 0 is trapped with all IRQs masked. Every task pinned to CPU 0
(`abort_task`, `bootdone_task`, per-CPU idle) starves. The FreeRTOS
scheduler on CPU 0 never runs again. `bootdone_notify_to_host`
never fires. The `issr.log` stays empty. The CM7 stub fills the gap
from the driver's perspective and the freeze is silent.

**Fix.** `src/include/r100/remu_addrmap.h` exposes the GIC
convention via `R100_INTID_TO_GIC_SPI_GPIO(intid)` and names the
constants for what they are (`_INTID`, not `_SPI`). `r100_soc.c`
wires the VF0 mailbox through that macro. Rule for future
device-to-GIC wiring in this repo: **always pass
`R100_INTID_TO_GIC_SPI_GPIO(INTID)` to `qdev_get_gpio_in(gic, …)`,
never a raw INTID**.

**Latent cousins.** The per-chiplet 16550 UART is wired as
`qdev_get_gpio_in(gic_dev[chiplet], 33)` (i.e. raw index 33 → INTID
65). q-sys' `console_uart_init()` doesn't register an RX ISR and TX
is polled, so this INTID is dead code today — but if anyone adds an
IRQ-driven RX path to `terminal_task`, the same conversion must be
applied or the PERI0_M7-style collision repeats. Callout comment
lives adjacent to the UART block in `r100_soc.c`.

**Verification after the fix:**

```
./remucli build
./remucli run --host --name gic-fix-smoke
# Wait ~45 s then:
rg 'Notify Host - PF FW_BOOT_DONE' output/gic-fix-smoke/uart0.log
#   → Notify Host - PF FW_BOOT_DONE
grep '0x90.*0xfb0d' output/gic-fix-smoke/issr.log
#   → issr off=0x90 val=0xfb0d status=ok count=1
```

`issr.log count=1` is the crisp witness: pre-fix that file is empty
in `--host` mode; post-fix the real firmware emits one frame, then
the CM7 stub covers any subsequent soft-reset re-handshakes.

## Cleanup

```
rm -rf output/run-20260101-*    # drop older timestamped runs
rm -rf output/                  # nuke everything; next run recreates
```
