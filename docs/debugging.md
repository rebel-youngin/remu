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
  hdma.log      # M8b 3b+3c — bidirectional HDMA (OP_WRITE / OP_READ_REQ / OP_READ_RESP)
                # (P7 retired the cm7-debug chardev / cm7.log trace;
                #  P10-fix retired the cfg chardev / cfg.log — cfg-head
                #  propagation moved to a shared cfg-shadow shm alias)
  shm -> /dev/shm/remu-my-test/        # remu-shm + host-ram + cfg-shadow shared backends
  npu/
    monitor.sock       # HMP over unix socket
    qemu.stderr.log    # device-model breadcrumbs (only way to see internal state)
    info-{mtree,qtree,mtree-imsix,qtree-issr,qtree-cfg-hdma}.log  # startup bridge checks
  host/
    cmdline.txt + qemu.{stdout,stderr}.log
    serial.log         # SeaBIOS idle, or Linux + kmd dmesg when M8b Stage 2 staged
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

# cfg + hdma chardevs must be bound on both ends in qtree
# (P7 retired the cm7-debug chardev along with the gated stubs)
printf 'info qtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/host/monitor.sock | rg '(cfg|hdma) *='
printf 'info qtree\nquit\n' | socat - UNIX-CONNECT:output/<name>/npu/monitor.sock  | rg '(cfg|hdma)-chardev'
```

`{doorbell,msix,issr,cfg,hdma}.log` are per-bridge ASCII traces
(format `<bus> off=0x... val=0x... [status=...] count=N`, or for
hdma: `hdma <dir> op=<mnemonic> dst=0x... len=... req_id=N status=... count=N`
where `<dir>` is `tx` / `rx` and mnemonics are
`WRITE`/`READ_REQ`/`READ_RESP`/`CFG_WRITE`). `msix.log` is sourced
exclusively by q-cp's `cb_complete → pcie_msix_trigger` via the
`r100-imsix` MMIO trap (P2 deleted the cm7-side notify; P7 retired
the entire FSM that used to host it). The `cm7.log` BD-done trace
chardev was retired in P7 — pre-P7 runs produced it as scaffolding;
post-P7 there is no NPU-side cm7 FSM so the file is no longer
created.
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
(`docs/roadmap.md` → P8).

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

### Stage 3b — QINIT CM7 stub (retired in P7)

> **Status:** P1c gated this stub off by default; **P7 deleted it
> outright** along with `r100_cm7_qinit_stub`, `REMU_FW_VERSION_STR`,
> and the `qinit-stub` bool property. q-cp on CP0 publishes the real
> `fw_version` + `init_done = 1` natively through the P1a outbound
> iATU + P1b cfg-mirror trap. The historical scaffolding lives in
> `git log src/machine/r100_cm7.c` if a stub-style reproducer is
> needed for bisecting.

P10-fix moved cfg-head propagation off the `cfg` chardev onto a
shared `cfg-shadow` `memory-backend-file` aliased on both sides
(host x86 BAR2 cfg-head subregion ↔ NPU r100-cm7 cfg-mirror trap).
The `cfg` / `cfg-debug` chardevs are gone. The bidirectional `hdma`
chardev stays:

| chardev | dir | frame | role |
|---------|-----|-------|------|
| `hdma` | NPU↔host | 24 B header + payload | Bidirectional DMA. NPU→host: `OP_WRITE` (P5 HDMA LL host-leg writes), `OP_READ_REQ` (P5 HDMA LL host-leg reads, tagged by `req_id`). Host→NPU: `OP_READ_RESP` (paired response to a pending `OP_READ_REQ`). The historical `OP_CFG_WRITE` (NPU→host cfg shadow update; r100-cm7 P1b reverse path) was retired with the shm-backed cfg-shadow and the opcode stays unallocated. The P1a outbound iATU formerly issued `OP_READ_REQ`/`OP_WRITE` in the `0xC0..0xFF` `req_id` partition; P10-fix retired that path in favour of a direct `host-ram` `MemoryRegion` alias, so the partition is reserved. See `src/bridge/remu_hdma_proto.h`. |

`fw_version` round-trip — observe q-cp's real publish on a stock
`./remucli run --host`:

```
$ rg 'fw version' output/<name>/host/serial.log
rebellions rbln0: rbln_async_probe_worker: fw version: 3.2.0~dev~7~g47f862cb
```

If you see `3.remu-stub` instead, you are on a pre-P7 build (or
re-enabling the deleted stub by hand) — the string lived in
`REMU_FW_VERSION_STR` in `r100_cm7.c`, deleted by P7.

### Stage 3c — BD-done state machine (retired in P7)

> **Status:** P1c retired this scaffolding by default; **P7 deleted
> it outright** along with the `R100Cm7BdJob` per-queue state
> machine, the `r100_hdma_set_cm7_callback` plumbing, the
> `0x01..0x0F` cm7 `req_id` partition (now reserved up to `0x7F`
> for future UMQ multi-queue), the `imsix` / `mbtq-mailbox` QOM
> links on `r100-cm7`, the `bd-done-stub` / `mbtq-stub` /
> `qinit-stub` bool properties, and the `cm7-debug` chardev that
> produced `cm7.log`. q-cp on CP0 owns the entire BD walk natively
> (`hq_task → cb_task → cb_complete`) over the P1a outbound iATU
> + the P1b cfg-mirror trap; MSI-X comes from q-cp's
> `pcie_msix_trigger` (q-sys `osl/FreeRTOS/.../msix.c`) writing
> the `r100-imsix` MMIO. See "P1b/P1c" below for the current flow.
> The historical FSM phase ASCII (`bd-done qid=N WAIT_QDESC →
> WAIT_BD → WAIT_PKT → IDLE complete`) and the matching `hdma.log`
> `req_id=qid+1` slice it produced live in `git log
> src/machine/r100_cm7.c` (last in `198d8a2`) if a stub-style
> reproducer is needed.

#### Sanity-checking the bridges

The host-side `info qtree` must show the `hdma` CharBackend bound on
`r100-npu-pci`. The NPU-side must show a separate `r100-hdma` device
with its own `chardev = "hdma"` property (M9-1c moved the chardev
off cm7 onto the new device, which models q-cp's dw_hdma_v0 register
block — see `docs/roadmap.md` → M9-1c). `./remucli run --host`
auto-verifies both ends and logs snippets to
`host/info-qtree-cfg-hdma.log` and `npu/info-qtree-cfg-hdma.log`.
The pre-P10-fix `cfg` / `cfg-debug` chardev props on `r100-cm7` and
`r100-npu-pci` are gone; cfg-head propagation is verified separately
by checking that both QEMUs `mmap` the shared
`/dev/shm/remu-<name>/cfg-shadow` file (visible in `info mtree` as
the `r100.bar2.cfg.alias` and `r100.cm7.cfg-mirror.alias`
subregions). A missing `hdma` property means the
`-machine r100-soc,hdma=…` or `-device r100-npu-pci,hdma=…` option
didn't latch — check `cli/remu_cli.py` chardev id literals.

#### M9-1c — active DNC + HDMA reg block

> **Status post-P7:** the cm7-side mbtq push (`r100_cm7_mbtq_push` +
> `r100_cm7_synth_cmd_descr` + the `R100_CMD_DESCR_SYNTH_BASE`
> ring) was retired in P7 along with the BD-done FSM. Every
> queue-doorbell on `INTGR1` bit `qid` now relays as a single SPI
> 185 wake to q-cp's `hq_task` on CP0; q-cp's `cb_task` builds the
> real `cmd_descr` from CB content, and `mtq_push_task` publishes
> it onto the matching `r100-mailbox` block (P3: COMPUTE / UDMA /
> UDMA_LP / UDMA_ST on chiplet 0) by indexing
> `_inst[HW_SPEC_DNC_QUEUE_NUM=4]`. The
> `r100-dnc-cluster` kickoff → done passage → DNC SPI path
> (described below) stays default-on; q-cp's `dnc_send_task` is the
> producer either way.

The active path on `INTGR1` bit `qid`: q-cp's CP1 worker calls
`dnc_send_task` after popping a real `cmd_descr` from the mailbox;
the writes land on `r100-dnc-cluster`, the final 4-byte store to
slot+0x81C with `itdone=1` triggers a BH that synthesises a
`dnc_reg_done_passage` at slot+0xA00 and pulses the matching DNC GIC
SPI from `r100_dnc_intid()`.

Log signatures (NPU side, with dnc-debug active):

| Trace line | Means |
|---|---|
| `r100-dnc cl=C dcl=D slot=S kickoff dnc_id=N cmd_type=T desc_id=0x... → intid=I spi=I-32 fired=...` | q-cp reached `dnc_send_task` and wrote DESC_CFG1 with itdone=1; r100-dnc latched the done passage and pulsed the DNC SPI. |
| `r100-dnc cl=C dcl=D slot=S: completion FIFO full` | Pending completions exceeded `DNC_DONE_FIFO_DEPTH=32`. Almost certainly indicates an IRQ-storm bug (e.g. spurious tlb_invalidate-shaped writes). |

Common failure: the kickoff trace never fires even though the
mailbox shows `MBTQ_PI_IDX` advancing. That means q-cp's CP1 worker
isn't reaching `dnc_send_task` for the pushed task. First check
via CP1 GDB (`tests/scripts/gdb_inspect_cp1.gdb`): is the worker
thread on a DNC range that includes DNC0? Pre-P7 the cm7-side
synth at `0x20000000` was a fixed address you could `xp` to verify
the cmd_descr layout — post-P7 the address is q-cp-allocated, so
you have to chase it through the `mtq_push_task` payload at
`PERI0_MAILBOX_M9_CPU1` ISSR slots (or break on `dnc_send_task`
in the CP1 GDB session).

#### P1a / P10-fix — chiplet-0 PCIe outbound iATU stub

Real silicon translates AArch64 loads in the
`R100_PCIE_AXI_SLV_BASE_ADDR = 0x8000000000ULL` range into PCIe TLPs
via the chiplet-0 DesignWare iATU; the kmd publishes bus addresses
with `HOST_PHYS_BASE = 0x8000000000ULL` already added so q-cp on
CA73 CP0 can dereference them directly. REMU has no DW iATU. P1a
introduced `r100-pcie-outbound`; P10-fix replaced its chardev RPC
with a direct shared-memory alias after the chardev path deadlocked
under the kmd's busy-poll (the chardev RX iothread couldn't acquire
BQL while a vCPU was parked in `readl_poll_timeout_atomic`):

- **`r100-pcie-outbound`** (`src/machine/r100_pcie_outbound.c`,
  4 GB MMIO @ 0x8000000000, chiplet 0 PF only) — realised as a
  `MemoryRegion` alias over the shared `host-ram`
  `memory-backend-file` (the host x86 QEMU's main RAM). Both QEMUs
  `mmap` the same file, so q-cp's outbound loads/stores are plain
  TCG accesses against the same pages the kmd allocates with
  `dma_alloc_coherent`. No chardev hop, no `qemu_cond_wait_bql()`,
  no BQL contention. The pre-P10-fix chardev path
  (`OP_READ_REQ` / `OP_WRITE` in req_id partition `0xC0..0xFF`,
  parked vCPU on a per-device condvar via
  `r100_hdma_set_outbound_callback()`) is gone.
- **BAR2 cfg-head ↔ NPU MMIO alias** in `r100-cm7` (P1a → P1b →
  P10-fix). Both QEMUs alias a shared 4 KB `cfg-shadow`
  `memory-backend-file`: the host x86 QEMU's `r100-npu-pci` over the
  BAR2 cfg-head subregion at `FW_LOGBUF_SIZE`, the NPU `r100-cm7`
  over `R100_DEVICE_COMM_SPACE_BASE = 0x10200000` (prio 10 over
  chiplet-0 DRAM). q-cp's `hil_init_descs` reads `DDH_BASE_LO/HI/SIZE`
  from the alias, which returns the kmd's writes directly. q-sys
  CP0's cold-boot `memset(DEVICE_COMMUNICATION_SPACE_BASE, 0,
  CP1_LOGBUF_MAGIC)` flows through unmodified (the alias is plain
  TCG RAM, any access width / alignment works). The pre-P10-fix
  `cfg`/`cfg-debug` chardev path + NPU-side `cfg_shadow[1024]` u32
  array + host-side `cfg_mmio_regs[]` + `OP_CFG_WRITE` reverse-emit
  are gone.

`req_id` partitions on the `hdma` wire (canonical in
`src/include/r100/remu_addrmap.h`):

| Range | Owner |
|-------|-------|
| `0x00..0x7F` | reserved (legacy cm7 BD-done partition lived at `0x01..0x0F` until P7 retired the FSM; the P1b cfg-mirror reverse-emit at `0x00` was retired with the shm-backed cfg-shadow alias; available for UMQ multi-queue) |
| `0x80..0xBF` | `r100-hdma` MMIO-driven channel ops (M9-1c) |
| `0xC0..0xFF` | reserved (formerly `r100-pcie-outbound` synchronous PF-window reads; the device now aliases shared host-ram instead) |

Verifying P1a on a `--host` run (output paths under `output/<name>/`):

```
$ grep "Device descriptor\|Queue descriptor\|Context descriptor" \
       output/<name>/hils.log
[HILS … cpu=0 INFO  func=0] Device descriptor addr 0x8002d00000, size 286720
[HILS … cpu=0 INFO  func=0] Queue descriptor  addr 0x8002d000ec, size 32
[HILS … cpu=0 INFO  func=0] Context descriptor addr 0x8002d0012c, size 1112

$ grep "outbound" output/<name>/hdma.log
hdma cl=0 tx op=READ_REQ  req_id=0xc0 dst=0x2d0000c len=… tag=outbound-rd …
hdma cl=0 rx op=READ_RESP req_id=0xc0 dst=0x2d0000c len=… tag=resp …

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

- NPU monitor `xp /4wx 0x102000c0` reads zero while the kmd
  `dmesg` shows the DDH publish — both QEMUs aren't `mmap`ing the
  same `cfg-shadow` file. Check that
  `/proc/<host-pid>/maps` and `/proc/<npu-pid>/maps` both contain
  `/dev/shm/remu-<name>/cfg-shadow`, and that
  `info mtree -f` on each shows the `r100.bar2.cfg.alias` /
  `r100.cm7.cfg-mirror.alias` subregion at the right base. The
  `host-ram` / `cfg-shadow` aliases require the
  `-object memory-backend-file,...,share=on` objects to land on
  both QEMU command lines (see `cli/remu_cli.py`).
- q-cp logs `Device descriptor addr 0, size 0` even though the kmd
  dmesg shows `rbln_alloc_dma_buffer ok` — pre-P10-fix the cfg
  chardev RX could lag behind the doorbell, leaving DDH unset
  when q-cp's `hq_task` ran. P10-fix removes the race by sharing
  the cfg-head backend; if you still see it, double-check the
  `cfg-shadow` mmap on both sides. Pre-P1b dcache-alias variants
  of the same symptom are obsolete (the alias is plain TCG RAM,
  no MMIO uncacheable bit needed).
- `hdma … status=dma-fail` on a `OP_READ_REQ` from
  `r100-hdma`'s 0x80..0xBF channel partition — host's
  `pci_dma_read` returned non-`MEMTX_OK` for a bus address q-cp's
  HDMA-LL chain dereferenced. Usual cause: kmd has already freed /
  unmapped the DMA buffer, or the bus address is outside the kmd's
  allocation.

#### P1b/P1c — honest BD lifecycle on q-cp/CP0

P1a wired q-cp's outbound iATU and gave q-cp's `hil_init_descs` a
working DDH publish path. P1b and P1c finish the loop:

- **P1b** (NPU→host cfg reverse mirror) — closed the cfg loop both
  ways through the cfg-mirror trap. **P10-fix subsumed it**: the
  host x86 and NPU QEMUs alias the same `cfg-shadow`
  `memory-backend-file` over their BAR2 cfg-head and cfg-mirror
  MMIO traps, so q-cp's `cb_complete → writel(FUNC_SCRATCH, magic)`
  lands directly in the shared backend and is observable on the
  kmd's next `rebel_cfg_read(FUNC_SCRATCH)` for `rbln_queue_test`
  with no chardev queue and no ordering race against the doorbell.
  The pre-P10-fix `OP_CFG_WRITE` reverse-emit on the `hdma` chardev
  + host-side `cfg_mmio_regs[]` array are gone.
- **P1c** (gate `r100-cm7` BD-done + mbtq + QINIT stubs default off)
  + **P7** (delete them outright) — q-cp on CP0 owns the BD walk
  (`hq_task → cb_task → cb_complete`), the cmd_descr push to the
  M9 mailbox (`mtq_push_task`), and the MSI-X completion
  (`pcie_msix_trigger`). The legacy QEMU-side scaffolding (the
  Stage 3c BD-done FSM, the M9-1b mbtq push + cmd_descr synth ring,
  and the Stage 3b QINIT `fw_version` + `init_done` write-back) was
  removed in P7 along with the three `bd-done-stub` / `mbtq-stub` /
  `qinit-stub` boolean properties, the `imsix` + `mbtq-mailbox` QOM
  links, the `r100_hdma_set_cm7_callback` plumbing, and the
  `cm7-debug` chardev. **No `-global r100-cm7.*-stub=on` knobs are
  available post-P7** — bisect q-cp regressions with GDB on q-cp
  itself, or cherry-pick the historical scaffolding from
  `git log src/machine/r100_cm7.c` (last in `198d8a2`).

Verifying the loop on a `--host` run:

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

Failure modes:

- `rbln_queue_test: command-queue-0 has not same value, 0 != cafedead`
  on the host serial — q-cp wrote `FUNC_SCRATCH` but the host
  doesn't see it. Check that both QEMUs `mmap` the
  `/dev/shm/remu-<name>/cfg-shadow` file (`info mtree -f` on each
  must show the `r100.bar2.cfg.alias` / `r100.cm7.cfg-mirror.alias`
  subregion). If they do but the magic still isn't visible, check
  the host monitor `xp /1wx 0xf000200ffc` directly — if it shows
  `0xcafedead` but the kmd doesn't, the kmd's `rebel_cfg_read`
  path itself is broken.
- Soft lockup in `rebel_hw_init` waiting on `init_done` — pre-P10-fix
  the cfg chardev RX could lag behind the doorbell, leaving DDH
  unread when q-cp's `hq_task` ran. P10-fix removes this race
  (cfg-head propagates through the shared `cfg-shadow` alias; the
  alias is plain TCG RAM, any access width / alignment works for
  q-sys's cold-boot `memset`).
- `[cb] Current packet(0x1) is not supported` in `hils.log` —
  pre-P7 symptom where `r100-cm7`'s legacy BD-done state machine
  completed the BD before q-cp could parse it. Post-P7 the FSM is
  deleted; if you still see this on a current build, the only way
  to reproduce it is to cherry-pick the historical FSM back in.

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

### P10 cb[1] non-completion (open)

**Status.** Open. `./remucli test p10` times out at 180 s on every
run. The diagnosis below was assembled from `tests/p10_qcp_gdb_probe.py`
captures + log forensics; it is recorded here so the next person
picking up P10 starts from a known map of the failure modes rather
than from `LOG_LEVEL` archaeology.

**Symptom.** `command_submission -y 5` (the umd integration test
staged into the guest by `guest/build-umd.sh`) submits two command
buffers (`cb[0]`, `cb[1]`) and waits for two MSI-X completions on
vector 0. Only one fires. The kmd's TDR (`RBLN_REBEL_TASK_DONE_US`
expansion bug aside, see "Side bug 3" below) eventually times out,
sends `URG_EVENT_UNLOAD`, and the host kernel logs:

```
watchdog: BUG: soft lockup - CPU#0 stuck for ...s! [insmod or command_submission]
RIP: ... rebel_hw_init+0x439 (= rebel_queue_init busy-poll)
```

**Pinning the hang.** `tests/p10_qcp_gdb_probe.py` snapshots q-cp
state ~1.5 s after the second CQ doorbell, before TDR fires:

```
hq_mgr.cb_run_cnt   = 1                # cb_task pulled cb[1] off, never decremented
hq_mgr.cq[0].pi     = 2
hq_mgr.cq[0].ci     = 2                # hq_task fully drained both BDs
hq_mgr.req_funcs    = 0
cb_mgr.ready_list  empty (next == &ready_list)
cb_mgr.wait_list   empty (next == &wait_list)
```

`cb_run_cnt = 1` plus both lists empty means: `cb_task` started
processing cb[1], dispatched its packets to RBDMA / HDMA, and
parked on `xTaskNotifyWait` for the engine done IRQ. That IRQ
never fires. `host/msix.log` reads `count=1` for the entire run.

cb[0]'s completion goes through cleanly (one MSI-X observed, host
sees `RBLN_MAGIC_CODE` in `FUNC_SCRATCH`). cb[1] picks up state
that cb[0] did not fully restore — *which* engine's done line
never fires is the live unknown. `output/<name>/hdma.log` is
empty across the whole run, which suggests both CB workloads use
NPU-local addresses (RBDMA OTO between two chiplet-0 DRAM
offsets) and never cross the chardev — i.e. the missing IRQ is
likely RBDMA's GIC SPI 978 (`INT_ID_RBDMA1`) on the second kick,
not an HDMA wire ordering issue. Confirmation requires GDB on
`r100_rbdma_kickoff` / `r100_rbdma_fnsh_bh` between cb[0] and
cb[1] — open work.

**Diagnostic recipe.**

```
./remucli fw-build                          # ELFs needed by the gdb probe
REMU_P10_DEBUG_TAG=cb1 \
REMU_P10_DEBUG_DELAY_S=1.5 \
python3 tests/p10_qcp_gdb_probe.py
ls -la output/p10-debug/qcp-bt-cb1.txt      # the artifact
```

The probe is a diagnostic, not a regression test — it always exits
0 if it produced an artifact. Read the file to see the actual q-cp
state. Two interesting settle points to compare are documented in
the script header (~1.5 s = pre-TDR; ~8 s = mid `handle_unload_event`
register dump cascade).

**Side bug 1 — TDR `URG_EVENT_UNLOAD` register-dump cascade.**
Once cb[1] hangs, the kmd's TDR fires and sends
`URG_EVENT_UNLOAD` over the doorbell wire. q-cp's
`hq_proc_urg_event` calls `handle_unload_event`, which in turn
calls `DNC_DUMP_ESSENTIAL`, `RBDMA_DUMP_CDMA`, and
`rbcm_dump_chiplet_incomplete_ttreg` — a ~700-line register dump
peppered with `mdelay(500000)` busy-waits. Those `mdelay`s execute
*synchronously inside the doorbell ISR on CP0.cpu0* under TCG and
block the CA73 from servicing any other interrupt for many seconds.
The host kernel's `watchdog` then prints `soft lockup` for
`rebel_hw_init+0x439` (the `readl_poll_timeout_atomic` busy-poll).
This is a downstream symptom of the cb[1] hang, **not** the root
cause — fixing cb[1] removes it. Logged here so future hangs that
look like a `rebel_hw_init` lockup are not mis-attributed to the
queue_init handshake (which the P10-fix shm cfg-shadow alias
already resolved; see commit `d986302`).

**Side bug 2 — intermittent `cfg-shadow` NULL-deref at boot
(~30% of runs).** A subset of `--host` boots fault inside q-cp's
`hil_init_descs` at `hil.c:519` dereferencing
`dev_desc[func_id]->qd_addr_lo` (offset `+0x0c` of NULL). The
direct-shm and HMP `xp` reads from `tests/p10_cfgshadow_probe.py`
all show the kmd-published DDH at `0x80_04800000` *was* mirrored
into the cfg-shadow file before the QUEUE_INIT doorbell — yet
q-cp's `FUNC_READQ(PCIE_PF, DDH_BASE_LO)` returns 0 on the
faulting run. The smell is a TCG TLB caching race: q-cp's MMU
translation for `R100_DEVICE_COMM_SPACE_BASE` was filled before
the `r100_cm7_init` MMIO subregion overlay landed, leaving the
cached translation pointing at the underlying chiplet-0 DRAM
zero-fill rather than the cfg-shadow alias. Worth checking with
`-d in_asm,exec,page,mmu -singlestep` next time the run reproduces
on the first try; until then, just retry the run. Recipe:

```
python3 tests/p10_cfgshadow_probe.py    # prints all three views
                                        # of the DDH publish
```

**Side bug 3 — KMD `readl_poll_timeout_atomic` argument unit
mismatch.** `rebel.c:700` calls
`readl_poll_timeout_atomic(..., 10, jiffies_to_usecs(RBLN_REBEL_TASK_DONE_US))`
where `RBLN_REBEL_TASK_DONE_US = 3 * 1000000` is **already in
microseconds** but the macro is being run through
`jiffies_to_usecs()` again. With HZ=250 the inner conversion
expands to ~12000 s, so the kmd's "3 s queue_init timeout" is
actually a ~3-hour busy-loop on TCG. This is a stock-KMD bug,
upstream — not REMU. It does not cause the cb[1] hang but does
mean the soft lockup runs for the full 180 s test budget instead
of the 6 s wall the kmd was meant to wait. Tracked here so future
people picking up P10 don't try to "fix" the timeout from the REMU
side.

**Where things stand.** queue_init handshake is fixed (P10-fix /
`d986302`). cb[0] passes. cb[1] hangs in q-cp's `cb_task` waiting
for an engine done IRQ — most likely RBDMA SPI 978 not firing on
the second kick, given `hdma.log` stays empty. Next step is a
gdb session on `r100-rbdma`'s kick → BH → fnsh_pulse path with
breakpoints on `r100_rbdma_kickoff` (`src/machine/r100_rbdma.c`)
straddling cb[0] and cb[1] to compare the FNSH FIFO depth + GIC
pulse counts. Until cb[1] clears, P10 stays excluded from
`./remucli test` defaults (`in_default=False` in `TEST_REGISTRY`).

## Cleanup

```
rm -rf output/run-20260101-*    # drop older timestamped runs
rm -rf output/                  # nuke everything; next run recreates
```
