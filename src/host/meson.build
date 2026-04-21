# REMU — R100 NPU System Emulator
# Host-side device models (x86_64 guest)
#
# Included from the QEMU source tree at build time via a symlink
# (external/qemu/hw/misc/r100-host/ -> src/host/) and a single-line
# patch that adds `subdir('r100-host')` to hw/misc/meson.build.
#
# `system_ss` is QEMU's cross-target source set: files added here are
# compiled into every system emulator we build (both qemu-system-x86_64
# and qemu-system-aarch64). That's fine — the device is dormant unless
# a machine instantiates it via `-device r100-npu-pci`. A cleaner split
# (x86-only) would need a Kconfig symbol; we can add that later if the
# compile cost becomes visible.

system_ss.add(files(
    'r100_npu_pci.c',
))
