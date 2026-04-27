# CLAUDE.md

## Project

REMU — R100 NPU System Emulator. QEMU-based functional emulator for the
R100 (CR03 quad, 4-chiplet) NPU SoC. Runs unmodified firmware (q-sys,
q-cp) and drivers (kmd, umd) on emulated hardware.

## Documentation

- [docs/architecture.md](docs/architecture.md) — System architecture, memory maps, device model design, FW source references
- [docs/roadmap.md](docs/roadmap.md) — Phase 1 (FW boot, complete), Phase 2 foundation milestones (M1..M9-1, complete) + the unified P1..P11 architectural plan that supersedes the old Phase 2 / Phase 3 boundary
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
./remucli test                      # M5/M6/M7/M8 bridge tests (`test m5 m7` for subset)
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
| 0   | 64 GB | DDR — first 128 MB = shm file, tail = lazy RAM |
| 2   | 64 MB | ACP / SRAM / logbuf; 4 KB prio-10 trap @ `FW_LOGBUF_SIZE` (M8b 3b cfg-head) forwards writes on `cfg` chardev, rest lazy RAM |
| 4   | 8 MB  | 4 KB MMIO head (INTGR M6 + MAILBOX_BASE M8a bidir + CM7-stub Stage 3a + queue-start doorbell relayed as SPI 185 to wake q-cp's `hq_task` post-P1c); rest lazy RAM |
| 5   | 1 MB  | MSI-X table (32 vectors) + PBA (`msix_notify()` target for M7; q-cp's `cb_complete → pcie_msix_trigger` post-P1c — formerly Stage 3c BD-done) |

BAR4 details: `MAILBOX_INTGR{0,1}` / `MAILBOX_BASE` writes are
serialised as 8-byte chardev frames on the `doorbell` socket; reads in
`MAILBOX_BASE` range return the live shadow fed by `issr` chardev
frames (NPU → host). Cold-boot `FW_BOOT_DONE` is **real**: q-sys
`bootdone_task` writes `0xFB0D` to PF.ISSR[4] which egresses over the
`issr` chardev into the shadow (see `docs/debugging.md` →
"Post-mortems" for the GIC wiring fix that enabled this). `INTGR0`
bit 0 (`SOFT_RESET`) triggers a narrow QEMU-side CM7-stub that
*re-synthesises* `0xFB0D` on the kmd's post-probe soft-reset
handshake, because REMU does not yet model a real CA73 cluster reset
(`docs/roadmap.md` → P8).
`INTGR1` bit 7 (`QUEUE_INIT`) triggers the Stage 3b QINIT stub (see
below).

BAR2 details (M8b 3b → P1b): kmd writes `DDH_BASE_{LO,HI}` (at
`FW_LOGBUF_SIZE + 0xC0/0xC4`) to publish the host-RAM
`rbln_device_desc` to firmware. The host-side `r100-npu-pci` traps the
4 KB window and emits 8-byte `(cfg_off, val)` frames on the `cfg`
chardev; the NPU-side `r100-cm7` consumes them into a 1024-entry
`cfg_shadow[]`. P1b makes that shadow the single source of truth on the
NPU side: a 4 KB MMIO trap (`r100_cm7_cfg_mirror_ops`, prio 10) overlays
chiplet-0 DRAM at `R100_DEVICE_COMM_SPACE_BASE = 0x10200000`, so q-sys/
q-cp loads of `DEVICE_COMMUNICATION_SPACE_BASE + N` read `cfg_shadow[N
>> 2]` directly (the inbound iATU silicon would normally provide).
NPU-side stores update `cfg_shadow` *and* forward an `OP_CFG_WRITE`
upstream over `hdma`, closing the loop with the host's
`cfg_mmio_regs[]` — this is the path that makes q-cp's `cb_complete →
writel(FUNC_SCRATCH, magic)` round-trip back into the kmd's
`rebel_cfg_read(FUNC_SCRATCH)` for `rbln_queue_test`. The paired
**NPU ↔ host DMA executor** is the `hdma` chardev with a 24 B header +
payload protocol (`src/bridge/remu_hdma_proto.h`). The legacy
`INTGR1` bit 7 stub (`r100_cm7_qinit_stub`) that read `DDH_BASE` from
`cfg_shadow[]` and emitted two `HDMA_OP_WRITE` frames (fw_version,
init_done) is now off by default post-P1c — q-cp on CP0 owns the QINIT
write-back natively over the P1a outbound iATU. The stub is still
linked in behind `-global r100-cm7.qinit-stub=on` for bisects.

Stage 3c turned the single-opcode `hdma` pipe into a bidirectional
protocol so the NPU can drive BD completion without a second chardev.
Opcodes: `OP_READ_REQ` (NPU → host: "please `pci_dma_read` `len` bytes
at `dst`, tag with `req_id`"), `OP_READ_RESP` (host → NPU: payload for
a pending `req_id`), and `OP_CFG_WRITE` (NPU → host: "update host-local
`cfg_mmio_regs[dst>>2]`"). The `flags` header byte is renamed `req_id`.
P1c retired the `r100-cm7`-side BD-done state machine: `INTGR1` bits
`0..N-1` still relay as SPI 185 to wake q-cp's `hq_task` on CP0, but the
QEMU-side per-queue `R100Cm7BdJob` walk (`IDLE → WAIT_QDESC → WAIT_BD →
WAIT_PKT → IDLE` issuing `OP_READ_REQ` pairs and committing
`OP_CFG_WRITE FUNC_SCRATCH` / `OP_WRITE bd.header |= DONE` /
`OP_WRITE queue_desc.ci++` / `r100_imsix_notify(vec=qid)`) is off by
default — q-cp's native `hq_task → cb_task → cb_complete` does the same
walk over the P1a outbound iATU + the P1b cfg-mirror trap, with
`pcie_msix_trigger` (q-sys `osl/FreeRTOS/.../msix.c`) firing MSI-X
through the existing `r100-imsix` MMIO. To re-enable the QEMU-side
state machine for bisecting, pass `-global r100-cm7.bd-done-stub=on`
(or the matching `mbtq-stub` / `qinit-stub` knob); see
`docs/debugging.md` → "Stage 3c — BD-done state machine" for the log
signatures.

#### P1a — chiplet-0 PCIe outbound iATU stub + DDH mirror

q-cp on CA73 CP0 dereferences PCIe-bus addresses directly (the kmd
publishes them with `HOST_PHYS_BASE = 0x8000000000ULL` added; real
silicon's chiplet-0 DesignWare iATU translates the AXI loads into
PCIe TLPs). REMU has no DW iATU, so until P1a the loads silently
read garbage from chiplet-0 lazy RAM. **P1a** lands two pieces of
infrastructure that close that gap (`docs/roadmap.md` → P1):

1. **`r100-pcie-outbound`** (`src/machine/r100_pcie_outbound.c`) —
   sysbus device that traps the 4 GB AXI window at
   `R100_PCIE_AXI_SLV_BASE_ADDR = 0x8000000000ULL` (chiplet 0, PF
   only). Reads emit `OP_READ_REQ` over the existing `hdma` chardev
   tagged with a `req_id` in the new `0xC0..0xFF` partition (single
   in flight; cookie rotates over the low 6 bits) and block on
   `qemu_cond_wait_bql()` until the matching `OP_READ_RESP` arrives.
   Writes are fire-and-forget `OP_WRITE` (no kmd fence on these
   bytes; the doorbells that *do* need ordering go through the
   `INTGR1`-bit-`qid` → SPI 185 → q-cp's `hq_task` path on CP0,
   which `r100-pcie-outbound` is just the read backend of).
   `r100-hdma` demuxes responses to a per-device callback
   registered via `r100_hdma_set_outbound_callback()`; the legacy
   cm7 callback (`r100_hdma_set_cm7_callback`) keeps the
   `0x01..0x0F` partition reachable when `bd-done-stub=on` is used
   for bisecting. Host-side `pci_dma_read` failures synthesise a
   zero-payload `OP_READ_RESP` so the parked vCPU surfaces a
   guest-visible `0` instead of dead-locking.

2. **BAR2 cfg-head ↔ NPU MMIO trap** in `r100-cm7`. Real silicon's
   inbound iATU maps host BAR2 onto NPU local memory at
   `FW_LOGBUF_SIZE` so kmd writes show up at
   `DEVICE_COMMUNICATION_SPACE_BASE = 0x10200000` for q-cp's
   `hil_init_descs` (which reads `DDH_BASE_LO/HI/SIZE` via
   `FUNC_READQ(hil_reg_base[PCIE_PF] + DDH_BASE_LO)`). P1a originally
   shipped this as a cfg-frame-to-NPU-DRAM write-through; **P1b**
   (below) replaced that with an MMIO subregion overlay
   (`r100_cm7_cfg_mirror_ops`, prio 10, 4 KB) so `cfg_shadow[1024]`
   is the single source of truth on both ends — host writes ingress
   on the `cfg` chardev and update the shadow, NPU reads/writes hit
   the trap, and NPU writes additionally egress as `OP_CFG_WRITE`
   over `hdma` so the host's `cfg_mmio_regs[]` stays consistent.

`req_id` partitions on the `hdma` wire (single source of truth in
`src/include/r100/remu_addrmap.h`):
`0x00` = QINIT untagged, `0x01..0x0F` = cm7 BD-done per qid+1
(post-P1c default off), `0x80..0xBF` = `r100-hdma` MMIO-driven
channel ops, `0xC0..0xFF` = `r100-pcie-outbound` synchronous
PF-window reads.

Verified end-to-end on `--host` boot: q-cp logs `Device descriptor
addr 0x80…, size 286720`, `Queue descriptor addr 0x80…0xec,
size 32`, `Context descriptor addr 0x80…0x12c, size 1112` — all
fetched through the new outbound stub against the kmd's coherent
DMA buffer. m5/m6/m7/m8 bridge regression unaffected.

#### P1b/P1c — honest BD lifecycle on q-cp/CP0

**P1b** (NPU→host cfg reverse mirror) closes the cfg loop both ways
through the trap above. The NPU-side write helper
(`r100_cm7_cfg_mirror_write`) updates `cfg_shadow` *and* emits an
`OP_CFG_WRITE` `(off, val)` over `hdma`, which the host's
`r100-npu-pci` decodes into `cfg_mmio_regs[off >> 2] = val`. That
makes q-cp's `cb_complete → writel(FUNC_SCRATCH, magic)` visible
to the kmd's `rebel_cfg_read(FUNC_SCRATCH)` — the path
`rbln_queue_test` polls. The trap accepts 1/2/4/8-byte and
unaligned access (`impl.access_size = 4`) so q-sys CP0's cold-boot
`memset(DEVICE_COMMUNICATION_SPACE_BASE, 0, CP1_LOGBUF_MAGIC)`
flows through unmodified.

**P1c** (retire `r100-cm7` BD-done + mbtq-push stubs) — q-cp on
CP0 now owns the entire BD lifecycle natively:
`hq_task → cb_task → cb_complete` walks queue_desc / BD / packet
over the P1a outbound iATU, writes `FUNC_SCRATCH` through the P1b
trap, sets `BD_FLAGS_DONE_MASK` + advances `queue_desc.ci`, and
calls `pcie_msix_trigger` on the `r100-imsix` MMIO. The legacy
QEMU-side state machines in `r100-cm7` are still compiled in but
default off:

| `-global` | Re-enables |
|---|---|
| `r100-cm7.bd-done-stub=on` | Stage 3c BD-done state machine on `INTGR1` bits `0..N-1` |
| `r100-cm7.mbtq-stub=on` | M9-1c `cmd_descr` synth + mailbox push |
| `r100-cm7.qinit-stub=on` | Stage 3b `fw_version` + `init_done = 1` HDMA write-back |

Use these knobs for bisecting q-cp regressions (e.g.
`bd-done-stub=on` if you suspect q-cp's BD walk; `qinit-stub=on`
if `init_done` polling hangs on the host).

Verification: `rbln_queue_test` passes silently (no
`failed to test queue` / `fence error` on the host serial),
`hils.log` shows `cb_complete: send MSIx interrupt to host` on the
NPU side, and `xp /1wx 0xf000200ffc` on the host monitor returns
`0xcafedead` (BAR2 + 0xFFC = `FUNC_SCRATCH`).

Pre-P1c, every queue-doorbell also pushed a 24 B `dnc_one_task`
entry onto the q-cp DNC compute task queue (real `r100-mailbox`
instance at `R100_PERI0_MAILBOX_M9_BASE`, chiplet 0). q-cp's
`taskmgr_fetch_dnc_task_master_cp1` polls `MBTQ_PI_IDX` (ISSR[0])
on this mailbox; the push wakes that loop without IRQ wiring
(poll-based protocol). PCIE_CM7 firmware does this on silicon —
REMU has no Cortex-M7 vCPU so `r100-cm7` took the role via
`r100_mailbox_set_issr_words()` (in-process; no chardev). M9-1c
synthesised a minimal `cmd_descr` (cmd_type=COMPUTE,
`dnc_task_conf.core_affinity=BIT(0)`) into chiplet-0 private DRAM
at `R100_CMD_DESCR_SYNTH_BASE = 0x20000000` (16 × 256 B ring) and
pointed the entry's `cmd_descr` field at it. **P1c retired this**:
q-cp's `mtq_push_task` on CP0 owns the mailbox push naturally once
`cb_task` runs end-to-end. The push code stays compiled in behind
`-global r100-cm7.mbtq-stub=on` (default off) for bisecting; cm7-debug
emits `mbtq qid=N slot=M pi=P cmd_descr=0x... status=ok` traces when
the stub is enabled.

#### r100-hdma + active r100-dnc (M9-1c)

The `hdma` chardev moved from `r100-cm7` onto a new `r100-hdma` device
(QOM type `r100-hdma`, MMIO at `R100_HDMA_BASE = 0x1D80380000` on
chiplet 0). The motivation is that q-cp itself programs a DesignWare
dw_hdma_v0 register block at that address — REMU now models that
register block, and the chardev sits on the model's host-side
counterpart since CharBackends are single-frontend. r100-cm7 reaches
the chardev through a QOM link (`hdma` link prop) and the public emit
helpers in `src/machine/r100_hdma.h`. req_id partitioning on the wire
(documented in `src/bridge/remu_hdma_proto.h`):
0x00 = QINIT untagged (default off post-P1c),
0x01..0x0F = cm7 BD-done per qid+1 (default off post-P1c),
0x80..0xBF = r100-hdma MMIO-driven channel ops,
0xC0..0xFF = r100-pcie-outbound synchronous PF-window reads (P1a).

The passive `r100-dnc-cluster` register-file stub (sparse regstore
seeding IP_INFO / SHM TPG / RDSN bits for boot) gained an active
task-completion path: writes to slot+0x81C (TASK_DESC_CFG1) with
`access_size=4` and `itdone=1` schedule a BH that pulses the matching
DNC GIC SPI (lookup table at `r100_dnc_intid()` in
`r100/remu_addrmap.h`; INTIDs are non-contiguous — DNC0=410,
DNC1=422…) and latches a synthesised `dnc_reg_done_passage` at
slot+0xA00 so q-cp's `dnc_X_done_handler` reads a coherent record.
GIC `num-irq` was bumped 256 → 992 so the wider DNC INTID set fits.

Both QEMUs `mmap` the same 128 MB `/dev/shm/remu-<name>/remu-shm` with
`share=on`. Splice points (same backend, two places):

- **x86 host** (M4) — `r100-npu-pci` `memdev` link over BAR0 offset 0.
- **NPU** (M5) — `r100-soc` machine's `memdev` string property (resolved
  at machine-init time, after `-object memory-backend-*`) over chiplet-0
  DRAM offset 0. Tail `0x08000000..0x40000000` stays private lazy RAM
  so BL31_CP1 / FreeRTOS_CP1 loaded at `0x14100000` / `0x14200000` don't
  leak to host.

HMP monitors: NPU on `output/<name>/npu/monitor.sock` (plus the
stdio-muxed readline monitor on uart0), host on
`output/<name>/host/monitor.sock`. By default SeaBIOS runs and the x86
guest idles at "No bootable device", serial to `host/serial.log`.

**M8b Stage 2 (x86 Linux guest):** with
`images/x86_guest/{bzImage,initramfs.cpio.gz}` staged (via
`./guest/build-guest-image.sh`), `--host` auto-adds `-kernel/-initrd`
+ virtio-9p share of `guest/` at `/mnt/remu` + `-cpu max` (stock kmd is
`-march=native` with BMI2 → `#UD` on minimal `qemu64`). The guest
boots Ubuntu HWE + busybox initramfs, runs `setup.sh` (insmods
`rebellions.ko` and watches for `FW_BOOT_DONE`). Overrides:
`--guest-kernel`, `--guest-initrd`, `--guest-share`, `--no-guest-boot`,
`--guest-cmdline-extra`. See commits `1ef7208` / `985fd58` for
artifact pipeline and GIC-bpr QEMU patch detail.

### Auto-verification on startup

Every `--host` run captures and checks:

- **M4/M5**: shm file in both `/proc/<pid>/maps`; `remushm` subregion
  at offset 0 of both `r100.bar0.ddr` (host) and `r100.chiplet0.dram`
  (NPU); host `info pci` lists `1eff:2030`.
- **M6**: host shows `r100.bar4.mmio` prio-10 overlay; NPU `info qtree`
  lists `r100-cm7` + `r100-mailbox`.
- **M7**: NPU lists `r100-imsix` @ `0x1BFFFFF000`; host shows
  `msix-{table,pba}` overlaying `r100.bar5.msix`.
- **M8a**: NPU `r100-mailbox` has `issr-chardev = "issr"`; host
  `r100-npu-pci` has `issr = "issr"`.
- **M8b 3b/3c + M9-1c**: NPU `r100-cm7` has `cfg-chardev = "cfg"` +
  `cm7-debug-chardev = "cm7_dbg"`; NPU `r100-hdma` exists with
  `chardev = "hdma"` (M9-1c moved the chardev off cm7 onto the new
  device); host `r100-npu-pci` has `cfg = "cfg"` + `hdma = "hdma"`
  (logged to `{host,npu}/info-qtree-cfg-hdma.log`).

All results go to `host/info-*.log` + `npu/info-*.log`. Failures print
to stdout (non-fatal — NPU still boots for post-mortem poking).

### End-to-end bridge tests

Driven by `./remucli test` (or individually). Each has pre-run
cleanup so SIGKILL'd prior state never poisons the next:

- `tests/m5_dataflow_test.py` — write magic to shm @ `0x07F00000`,
  `xp /4wx` on both monitors must agree.
- `tests/m6_doorbell_test.py` — drive 8-byte frames on
  `host/doorbell.sock`, check `doorbell.log` + `GUEST_ERROR`.
- `tests/m7_msix_test.py` — reverse: test impersonates NPU on
  `host/msix.sock`, emits 5 frames (3 ok + 1 oor + 1 bad-offset),
  checks `msix.log` + `GUEST_ERROR`.
- `tests/m8_issr_test.py` — two phases against two minimal QEMUs: NPU
  writes ISSR → host BAR4 shadow mirror via `xp`; host writes BAR4 →
  NPU mailbox ISSR update without spurious GIC SPI.

All `./remucli run` invocations write into `output/<name>/` (or
`output/run-<timestamp>/` if `--name` omitted). Never pass `/tmp/`
paths — stick to per-run directory. See `docs/debugging.md` for the
full agent loop.

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
patch while bisecting — see `cli/fw-patches/README.md`. Runtime-side
example of this policy: the CM7-stub in `src/machine/r100_cm7.c`
(commit `a01d2b5`) — see BAR4 row above.

## Project layout

```
remucli               Bash wrapper — the one entry point
src/machine/          NPU-side QEMU device models (symlinked into external/qemu/hw/arm/r100/)
                        r100_soc.{c,h}      machine + QOM type-name registry
                        r100_mailbox.{c,h}  mailbox — .c private state, .h public helpers
                        r100_hdma.{c,h}     HDMA reg block — .c private state,
                                            .h public emit API (M9-1c) +
                                            outbound RX callback hook (P1a)
                        r100_pcie_outbound.c PCIe outbound iATU stub —
                                            chiplet-0 4 GB AXI window
                                            tunnelled over `hdma` chardev (P1a)
                        r100_<dev>.c        one file per device (state struct private)
src/host/             Host-side (x86 guest) PCI device models (symlinked into external/qemu/hw/misc/r100-host/)
src/include/          Added to -I during QEMU configure
                        r100/remu_addrmap.h — `#include "r100/remu_addrmap.h"`
src/bridge/           Added to -I during QEMU configure — cross-side shared headers
                        remu_frame.h          8-byte frame codec (RX accumulator + emit)
                        remu_doorbell_proto.h BAR4 offset classifier + BAR2 cfg-head layout
                        remu_hdma_proto.h     bidirectional HDMA protocol (24 B header + payload,
                                              OP_WRITE / READ_REQ / READ_RESP / CFG_WRITE);
                                              req_id partitions live in
                                              src/include/r100/remu_addrmap.h:
                                              0x00 QINIT (default off post-P1c),
                                              0x01..0x0F cm7 BD-done (default off post-P1c),
                                              0x80..0xBF r100-hdma channels,
                                              0xC0..0xFF r100-pcie-outbound (P1a)
                      Header-only `static inline`, so both host-side (system_ss)
                      and NPU-side (arm_ss) TUs pick up the same definitions
                      without introducing a shared object.
cli/remu_cli.py       Click-based CLI implementation
tests/                Test binaries and test scripts
docs/                 Architecture, roadmap, debugging
external/             Read-only: ssw-bundle (q-sys, q-cp, kmd, umd, ...), qemu
guest/                M8b Stage 2 virtio-9p share (rebellions.ko / rblnfs.ko gitignored)
images/               FW binaries (bl1.bin, bl31_cp0.bin, freertos_cp0.bin, ...)
images/x86_guest/     M8b Stage 2 staged x86 guest kernel + initramfs (gitignored)
build/qemu/           QEMU build output (gitignored)
output/               Per-run log directories (gitignored; see docs/debugging.md)
```

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
