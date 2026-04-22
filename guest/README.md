# `guest/` — x86 guest shared mount

This directory is what the x86 host-side QEMU guest sees as `/mnt/remu`
(via virtio-9p pass-through). The files below live on the repo host,
get shared into the guest read-write, and are the payload the guest
uses to load the NPU driver and verify the M8b end-to-end boot path.

## Layout

```
guest/
├── README.md                this file
├── build-guest-image.sh     host-side: fetches Ubuntu linux-{image,modules}
│                            + busybox-static debs and builds bzImage +
│                            tiny initramfs under images/x86_guest/
├── build-kmd.sh             host-side: rebuilds rebellions.ko / rblnfs.ko
├── setup.sh                 guest-side: PCI sanity + insmod + FW_BOOT_DONE poll
├── rebellions.ko            (gitignored) built kmd, vermagic must match guest
└── rblnfs.ko                (gitignored) companion module (depends on rebellions)
```

Boot artifacts land outside this directory (they're large + binary):

```
images/x86_guest/
├── bzImage              Ubuntu distro kernel, ~15 MB
└── initramfs.cpio.gz    busybox + 9p modules + /init, ~1.3 MB
```

## Assumed guest

A prebuilt Ubuntu HWE kernel + a from-scratch busybox initramfs the
builder assembles for us.  `build-guest-image.sh` uses `apt-get
download` against whatever Ubuntu release you're on (jammy/noble/etc.)
and stages everything under `images/x86_guest/` — no sudo needed,
nothing installed system-wide.

| What             | Source                                                        |
|------------------|---------------------------------------------------------------|
| Kernel image     | `linux-image(-unsigned)-$(uname -r)` .deb → `/boot/vmlinuz-*` |
| Kernel modules   | `linux-modules-$(uname -r)` .deb → `/lib/modules/*/kernel/*.ko` |
| Static userland  | `busybox-static` .deb → `/bin/busybox`                        |
| Matching headers | `/lib/modules/$(uname -r)/build` (used by `build-kmd.sh`)     |

Any other distro works too — set `KVERSION=<ver>` to target a
different kernel, but keep it in sync between `build-guest-image.sh`
(initramfs + bzImage) and `build-kmd.sh` (kmd vermagic).

## Quick start

One-time setup:

```bash
# 1. Fetch + stage the x86 guest kernel + minimal initramfs.  Downloads
#    linux-{image,modules}-$(uname -r) and busybox-static from the
#    Ubuntu mirror (no sudo needed), extracts them, builds a tiny
#    busybox/cpio initramfs that knows how to mount our 9p share.
./guest/build-guest-image.sh

# 2. Rebuild the kmd against the guest kernel (vermagic must match).
./guest/build-kmd.sh
```

Then the usual:

```bash
./remucli run --host --name m8b-stage2
```

`remucli run --host` auto-detects `images/x86_guest/bzImage` and
`images/x86_guest/initramfs.cpio.gz` and passes them as `-kernel` /
`-initrd` to the x86 QEMU, alongside a virtio-9p `-fsdev local,id=remu`
share pointing at this directory (mount tag `remu`).  The x86 QEMU is
also switched from `-cpu qemu64` to `-cpu max` whenever a kernel is
configured, because the kmd is built with `-march=native` and emits
BMI2 instructions that trap as `#UD` on the minimal `qemu64` CPU.

Flags for overrides (all on `./remucli run`):

| Flag                    | Effect                                           |
|-------------------------|--------------------------------------------------|
| `--guest-kernel PATH`   | Use a different bzImage                          |
| `--guest-initrd PATH`   | Use a different initramfs                        |
| `--guest-share PATH`    | Share a different host dir (default: `guest/`)   |
| `--no-guest-boot`       | Skip all of the above, stay on SeaBIOS idle      |
| `--guest-cmdline-extra` | Extra kernel cmdline tokens (e.g. `loglevel=7`)  |

## What happens inside the guest

`/init` (baked into the initramfs by `build-guest-image.sh`):

1. Mounts `/proc`, `/sys`, `/dev` (devtmpfs).
2. `insmod`s `netfs.ko`, `9pnet.ko`, `9pnet_virtio.ko`, `9p.ko`
   (virtio-pci is built into the Ubuntu HWE kernel).
3. `mount -t 9p -o trans=virtio,version=9p2000.L remu /mnt/remu`.
4. Runs `sh /mnt/remu/setup.sh`.
5. Drops to a BusyBox shell on the QEMU serial for manual poking.

`setup.sh` (this directory, visible inside the guest as
`/mnt/remu/setup.sh`):

1. Walks `/sys/bus/pci/devices/` and confirms `1eff:2030` is present
   (saves the listing to `output/lspci.txt`).
2. `insmod /mnt/remu/rebellions.ko`, then `rblnfs.ko`.
3. Waits up to 10 s for `rebel_reset_done()` to print `FW_BOOT_DONE`
   to dmesg after polling `BAR4 + MAILBOX_BASE + 0x10` for `0xFB0D`.
4. Dumps `dmesg`, `lspci`, `/proc/interrupts` into
   `output/` (visible right back on the host at `guest/output/`).

## Why the `.ko` is gitignored

Kernel modules are vermagic-locked to the kernel they were built
against (`6.8.0-107-generic SMP preempt mod_unload modversions`), so
shipping a prebuilt `.ko` in the tree would be wrong for anyone on a
different host. Run `build-kmd.sh` once — the file it drops next to
this README is the one the guest picks up.
