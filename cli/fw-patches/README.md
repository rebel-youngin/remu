# `cli/fw-patches/`

**Empty by design.**

## Policy

REMU firmware patches are **disallowed** as a matter of project policy.
The q-sys submodule (`external/ssw-bundle/products/rebel/q/sys`) is kept
byte-identical to upstream so that REMU boots *exactly* the same binary
silicon ships with.

If an unmodelled hardware block makes a firmware step hang or `-EBUSY`
under REMU, the fix is to **model that block in QEMU**, not to `#ifdef`
around it in firmware. Even a one-line MMIO stub in `src/machine/` is
preferable to a firmware diff.

This covers both categories we used to carry here:

- **Behavioural shortcuts** (skipping unmodelled RoT / SPI / DVFS /
  rl_sysinfo_broadcast phases) — always model the block instead.
- **Debug breadcrumbs** (extra `printf()`s on the --host path) — use
  existing `RLOG_DBG` in firmware, `fprintf(stderr, ...)` in QEMU
  device models (captured at `output/<name>/npu/qemu.stderr.log`),
  or `./remucli gdb`.

## Plumbing

The `_apply_fw_patches()` machinery in `cli/remu_cli.py` (idempotent
forward/reverse-check apply dance, identical to `cli/qemu-patches/`)
is kept around as a no-op when this directory is empty. It exists so
that:

1. A developer can drop a **local, uncommitted** `*.patch` here while
   bisecting an FW wedge and have every `./remucli fw-build` apply it
   automatically — then revert the submodule and `rm` the patch when
   done.
2. If policy ever changes (e.g. we need to carry a legitimate upstream
   bug-fix temporarily while it's reported), the mechanism is already
   wired up.

Please keep this directory empty of committed `*.patch` files.
