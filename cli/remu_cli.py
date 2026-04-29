#!/usr/bin/env python3
"""
REMU CLI — R100 NPU System Emulator management tool.

Invoked via the ./remucli wrapper at the repo root (no install step).
Dependency: click (`pip install --user click`).

`./remucli build` produces two QEMU binaries from the same pinned source
tree: qemu-system-aarch64 (NPU side, Phase 1 FW boot) and
qemu-system-x86_64 (host side, Phase 2 kmd/umd guest). Build time roughly
doubles on a fresh tree; incremental rebuilds only touch changed files.

Usage:
    ./remucli build [--clean] [--jobs N]
    ./remucli fw-build [-c tf-a -c cp0 -c cp1] [--clean]   # -p silicon implicit
    ./remucli run [--name NAME] [--gdb] [--trace] [--chiplets N]
    ./remucli gdb [--port PORT] [-b ELF]
    ./remucli status
    ./remucli images [--check | --from-dir PATH]
    ./remucli test [m5|m6|m7|m8|p4a|p4b|p5|p11|p10|all]...
    ./remucli clean [--name NAME | --all]
    ./remucli completion {bash,zsh,fish}
"""

import os
import sys
import time
import shutil
import signal
import subprocess
from pathlib import Path

import click

REMU_ROOT = Path(__file__).resolve().parent.parent
QEMU_SRC = REMU_ROOT / "external" / "qemu"
QEMU_BUILD = REMU_ROOT / "build" / "qemu"
QEMU_BIN = QEMU_BUILD / "qemu-system-aarch64"
QEMU_BIN_X86 = QEMU_BUILD / "qemu-system-x86_64"
QEMU_PATCHES = REMU_ROOT / "cli" / "qemu-patches"
FW_PATCHES = REMU_ROOT / "cli" / "fw-patches"
MACHINE_SRC = REMU_ROOT / "src" / "machine"
HOST_SRC = REMU_ROOT / "src" / "host"
INCLUDE_SRC = REMU_ROOT / "src" / "include"
BRIDGE_SRC = REMU_ROOT / "src" / "bridge"   # cross-side shared helpers (remu_frame.h, ...)
IMAGES_DIR = REMU_ROOT / "images"
OUTPUT_ROOT = REMU_ROOT / "output"

# QEMU --target-list passed at configure time. aarch64 hosts the NPU-side
# FW boot (Phase 1); x86_64 hosts the Linux guest that runs kmd/umd
# against our r100-npu-pci device model (Phase 2). Both targets build
# from the same pinned QEMU source tree so the shared-memory bridge +
# host-side PCI device compile into both binaries.
QEMU_TARGETS = "aarch64-softmmu,x86_64-softmmu"
QEMU_TARGETS_MARKER = QEMU_BUILD / ".remu-targets"
# Stores the full configure argv; any change (targets, extra-cflags, ...)
# triggers an automatic reconfigure. Independent of QEMU_TARGETS_MARKER
# so existing build trees from before this file existed still upgrade
# on the next run.
QEMU_CONFIGURE_MARKER = QEMU_BUILD / ".remu-configure"

# Phase 2 shared-memory bridge defaults.
#
# 36 GB matches R100_RBLN_DRAM_SIZE / R100_DRAM_INIT_SIZE: the shm covers
# ALL of chiplet-0 DRAM (the kmd's actual PCIe BAR0 is 64 GB — next power
# of 2 above the 36 GB DRAM, since PCI requires `is_power_of_2(bar_size)`
# — but the upper 28 GB is a reserved alias on real silicon and a
# host-private lazy-RAM `bar0_tail` here, so 36 GB is the right shm
# size). Any byte the kmd writes via memcpy_toio(BAR0 + N, ...) for
# N < 36 GB is visible at chiplet-0 DRAM offset N for q-cp on the NPU
# side. Layout per chiplet, as the kmd allocates it:
# 1 GB CP_SYSTEM_SIZE system region (FW images / MMU page tables / sync
# block) + 35 GB user region (kmd's DVA pool). Aggregate device memory
# across the 4 CR03 chiplets is 144 GB; only chiplet 0 is exposed via
# BAR0, the rest are NPU-private (cross-chiplet reach through RBDMA /
# ICPI on real silicon).
#
# Cost on Linux: tmpfs (`/dev/shm`) is sparse, so the 36 GB ftruncate
# just reserves the address range — only pages q-cp / kmd actually
# touch consume real memory. Phase-1 boot tests (m5..p5, p11) only
# touch the 0..1 GB FW-image head, so the bump from the historical
# 128 MB / 4 GB defaults is free for those. Hard requirement is that
# `/dev/shm` has at least 36 GB free; the kernel default sets tmpfs
# size = 50 % of host RAM, so a host with ≥ 72 GB RAM clears it. On a
# tighter host either `mount -o remount,size=64G /dev/shm` or override
# `./remucli run --shm-size <bytes>` (the lazy `bar0_tail` /
# `dram_tail` paths in r100_npu_pci.c / r100_soc.c still kick in for
# any shm smaller than R100_BAR0_DDR_SIZE / R100_DRAM_INIT_SIZE).
SHM_ROOT = Path("/dev/shm")
SHM_SIZE_DEFAULT = 36 * 1024 * 1024 * 1024
HOST_MEM_DEFAULT = "512M"
# P10-fix: 4 KB cfg-shadow buffer shared between the host x86 QEMU's
# BAR2 cfg-head trap and the NPU r100-cm7 cfg-mirror trap. Eliminates
# the cfg/doorbell ordering race where q-cp could observe DDH=0
# because the cfg chardev RX hadn't been drained before the doorbell
# chardev RX woke `hq_task`. See r100_cm7.c / r100_npu_pci.c banner
# notes; sized to match REMU_BAR2_CFG_HEAD_SIZE.
CFG_SHADOW_SIZE = 4096

# M8b Stage 2: x86 guest boot artifacts produced by
# guest/build-guest-image.sh. When both files exist `./remucli run
# --host` auto-wires -kernel/-initrd + a virtio-9p share mapped at
# guest/ so the guest boots straight into /mnt/remu/setup.sh and
# tries to insmod rebellions.ko. Absent either file, the host QEMU
# keeps its M3-era SeaBIOS idle behavior.
X86_GUEST_DIR = REMU_ROOT / "images" / "x86_guest"
X86_GUEST_KERNEL = X86_GUEST_DIR / "bzImage"
X86_GUEST_INITRD = X86_GUEST_DIR / "initramfs.cpio.gz"
X86_GUEST_SHARE = REMU_ROOT / "guest"

# q-sys (CP firmware) build paths
SSW_SYS = REMU_ROOT / "external" / "ssw-bundle" / "products" / "rebel" / "q" / "sys"
SSW_SYS_BUILD = SSW_SYS / "build.sh"
SSW_SYS_BINARIES = SSW_SYS / "binaries"

# Toolchain defaults — overridable via env (COMPILER_PATH_ARM64 etc.)
DEFAULT_COMPILER_PATH_ARM64 = "/mnt/data/tools/arm-gnu-toolchain-13.2.Rel1-x86_64-aarch64-none-elf/bin"
DEFAULT_COMPILER_PATH_ARM32 = "/mnt/data/query-setup/tools/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/bin"

# Maps q-sys output paths → remu image names (CA73 side only).
FW_INSTALL_MAP = [
    ("BootLoader_CP/bl1.bin",              "bl1.bin"),
    ("BootLoader_CP/bl2.bin",              "bl2.bin"),
    ("BootLoader_CP/bl31.bin",             "bl31_cp0.bin"),
    ("FreeRTOS_CP/freertos_kernel.bin",    "freertos_cp0.bin"),
    ("FreeRTOS_CP1/bl31.bin",              "bl31_cp1.bin"),
    ("FreeRTOS_CP1/freertos_kernel.bin",   "freertos_cp1.bin"),
]

# R100 chiplet offset (must match R100_CHIPLET_OFFSET in r100/remu_addrmap.h).
R100_CHIPLET_OFFSET = 0x2000000000
R100_NUM_CHIPLETS = 4

# R100 firmware image definitions: (filename, load_address, description)
#
# These base addresses are all chiplet-0 absolute addresses. BL1/BL2 live in
# iRAM inside the chiplet-local PRIVATE window and are replicated on each
# chiplet by the existing QSPI-bridge cross-chiplet copy path. BL31/FreeRTOS
# for CP0 live in chiplet 0's DRAM. BL31/FreeRTOS for CP1 need to exist in
# every chiplet's DRAM — see FW_PER_CHIPLET_IMAGES below.
FW_IMAGES = [
    ("bl1.bin",           0x1E00010000, "TF-A BL1 (iRAM)"),
    ("bl2.bin",           0x1E00028000, "TF-A BL2 (iRAM, GPT_DEST_ADDR_TBOOT_N)"),
    ("bl31_cp0.bin",      0x0000000000, "TF-A BL31 for CP0 (DRAM)"),
    ("bl31_cp1.bin",      0x0014100000, "TF-A BL31 for CP1 (DRAM)"),
    ("freertos_cp0.bin",  0x0000200000, "FreeRTOS for CP0 (DRAM)"),
    ("freertos_cp1.bin",  0x0014200000, "FreeRTOS for CP1 (DRAM)"),
    # Optional: raw NOR flash dump preloaded into the QSPI staging window.
    # When absent the region is zero-filled, which is enough to progress past
    # BL1 (HW-CFG magic-code check fails and UCIe falls back to default speed).
    ("flash.bin",         0x1F80000000, "QSPI NOR flash dump (optional)"),
]

# Images that must also be staged at each secondary chiplet's DRAM base.
# BL2 on each chiplet's CP0.cpu0 ERETs to BL31 at chiplet-local 0x0, and
# then BL31 ERETs to FreeRTOS at chiplet-local 0x200000; similarly each
# chiplet's CP1.cpu0 starts at CP1_BL31_BASE (0x14100000). On silicon,
# BL2 on chiplet 0 DMA-copies these images to each secondary chiplet's
# DRAM before releasing their CP0.cpu0 — REMU's DMA is a fake-completion
# stub, so we instead pre-load all four binaries into every chiplet's
# DRAM via QEMU -device loader. Chiplet 0 is already covered by
# FW_IMAGES above; duplicate for chiplets 1..N-1 at
# chiplet_id * CHIPLET_OFFSET + base.
FW_PER_CHIPLET_IMAGES = [
    ("bl31_cp0.bin",     0x0000000000, "TF-A BL31 for CP0 (DRAM, chiplet-local)"),
    ("freertos_cp0.bin", 0x0000200000, "FreeRTOS for CP0 (DRAM, chiplet-local)"),
    ("bl31_cp1.bin",     0x0014100000, "TF-A BL31 for CP1 (DRAM, chiplet-local)"),
    ("freertos_cp1.bin", 0x0014200000, "FreeRTOS for CP1 (DRAM, chiplet-local)"),
]


def _check_qemu_src():
    if not QEMU_SRC.is_dir():
        click.secho("QEMU source not found at: %s" % QEMU_SRC, fg="red")
        click.echo("Run:  git clone --depth 1 --branch v9.2.0 "
                    "https://gitlab.com/qemu-project/qemu.git %s" % QEMU_SRC)
        raise SystemExit(1)


def _check_qemu_bin(which="aarch64"):
    """Verify a built QEMU binary exists. `which` is 'aarch64' (NPU-side,
    default) or 'x86_64' (host-side; Phase 2)."""
    bin_path = QEMU_BIN if which == "aarch64" else QEMU_BIN_X86
    if not bin_path.is_file():
        click.secho("QEMU %s binary not found at %s." % (which, bin_path),
                    fg="red")
        click.secho("Run './remucli build' first.", fg="red")
        raise SystemExit(1)
    return bin_path


def _link_sources():
    """Symlink remu device models into the QEMU source tree."""
    r100_dir = QEMU_SRC / "hw" / "arm" / "r100"
    if not r100_dir.exists():
        r100_dir.symlink_to(MACHINE_SRC)
        click.echo("  Linked %s -> %s" % (r100_dir, MACHINE_SRC))

    r100_soc_h_link = QEMU_SRC / "include" / "hw" / "arm" / "r100_soc.h"
    if not r100_soc_h_link.exists():
        r100_soc_h_link.symlink_to(MACHINE_SRC / "r100_soc.h")
        click.echo("  Linked %s" % r100_soc_h_link)

    # Host-side (x86 guest) device models go under hw/misc/ — that's
    # where QEMU puts other generic PCI devices (edu, ivshmem) that
    # aren't tied to a specific target architecture.
    host_dir = QEMU_SRC / "hw" / "misc" / "r100-host"
    if not host_dir.exists():
        host_dir.symlink_to(HOST_SRC)
        click.echo("  Linked %s -> %s" % (host_dir, HOST_SRC))


def _patch_meson():
    """Ensure QEMU's meson.build files include our subdirs. Called
    twice (arm + misc) so fresh trees and upgraded trees both end up
    with both hooks wired. Returns True if anything changed."""
    changed = False

    arm_meson = QEMU_SRC / "hw" / "arm" / "meson.build"
    arm_marker = "subdir('r100')"
    arm_content = arm_meson.read_text()
    if arm_marker not in arm_content:
        arm_meson.write_text(arm_content + "\n%s\n" % arm_marker)
        click.echo("  Patched %s" % arm_meson)
        changed = True

    misc_meson = QEMU_SRC / "hw" / "misc" / "meson.build"
    misc_marker = "subdir('r100-host')"
    misc_content = misc_meson.read_text()
    if misc_marker not in misc_content:
        misc_meson.write_text(misc_content + "\n%s\n" % misc_marker)
        click.echo("  Patched %s" % misc_meson)
        changed = True

    return changed


def _apply_patches(patches_dir, target_dir, label):
    """Idempotently apply every *.patch file under `patches_dir` to the
    git tree at `target_dir`. `git apply --reverse --check` decides
    whether a patch is already present and skips it in that case, so
    repeat builds and fresh clones both end up in the same state."""
    if not patches_dir.is_dir():
        return
    patches = sorted(patches_dir.glob("*.patch"))
    if not patches:
        return
    for patch in patches:
        # Already applied? `git apply --reverse --check` succeeds iff the
        # current tree matches the post-patch state.
        reverse_check = subprocess.run(
            ["git", "apply", "--reverse", "--check", str(patch)],
            cwd=target_dir, capture_output=True,
        )
        if reverse_check.returncode == 0:
            continue
        # Confirm the patch applies cleanly to the current tree before doing it.
        forward_check = subprocess.run(
            ["git", "apply", "--check", str(patch)],
            cwd=target_dir, capture_output=True,
        )
        if forward_check.returncode != 0:
            click.secho("  %s patch %s does not apply cleanly:" %
                        (label, patch.name), fg="red")
            click.echo(forward_check.stderr.decode("utf-8", "replace"))
            raise SystemExit(1)
        subprocess.run(["git", "apply", str(patch)],
                       cwd=target_dir, check=True)
        click.echo("  Applied %s %s" % (label, patch.name))


def _apply_qemu_patches():
    """Idempotently apply every *.patch file under cli/qemu-patches/ to
    external/qemu."""
    _apply_patches(QEMU_PATCHES, QEMU_SRC, "qemu")


def _apply_fw_patches():
    """Idempotently apply every *.patch file under cli/fw-patches/ to the
    q-sys firmware submodule (external/ssw-bundle/products/rebel/q/sys).

    Project policy: cli/fw-patches/ is kept empty. Unmodelled-block
    workarounds are implemented on the QEMU side (src/machine/,
    src/host/) rather than as firmware diffs. See
    cli/fw-patches/README.md. This plumbing is retained as a no-op for
    ad-hoc local debugging patches that never get committed."""
    _apply_patches(FW_PATCHES, SSW_SYS, "fw")


def _run(cmd, cwd=None, check=True, **kwargs):
    """Run a subprocess, printing stdout/stderr live."""
    click.echo("  $ %s" % " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, cwd=cwd, check=check, **kwargs)


# ── CLI Group ────────────────────────────────────────────────────────────────

@click.group(context_settings={"help_option_names": ["-h", "--help"]})
@click.version_option("0.1.0", prog_name="remucli")
def cli():
    """REMU — R100 NPU System Emulator CLI."""


# ── build ────────────────────────────────────────────────────────────────────

@cli.command()
@click.option("--clean", is_flag=True, help="Remove build directory first.")
@click.option("--jobs", "-j", type=int, default=0,
              help="Parallel build jobs (0 = auto-detect).")
def build(clean, jobs):
    """Build QEMU with R100 device models.

    Produces two binaries from one source tree:

    \b
      build/qemu/qemu-system-aarch64  — NPU-side FW boot (Phase 1)
      build/qemu/qemu-system-x86_64   — host-side kmd/umd guest (Phase 2)

    On an existing aarch64-only build tree, the configure step is
    automatically re-run to pick up the new target list (no --clean
    needed).
    """
    _check_qemu_src()

    if clean:
        click.echo("Cleaning...")
        if QEMU_BUILD.exists():
            shutil.rmtree(QEMU_BUILD)
        r100_link = QEMU_SRC / "hw" / "arm" / "r100"
        if r100_link.is_symlink():
            r100_link.unlink()
        host_link = QEMU_SRC / "hw" / "misc" / "r100-host"
        if host_link.is_symlink():
            host_link.unlink()
        click.secho("Clean complete.", fg="green")
        if not click.confirm("Continue with build?"):
            return

    click.echo("Linking remu sources into QEMU tree...")
    _link_sources()

    click.echo("Patching meson.build...")
    _patch_meson()

    click.echo("Applying QEMU source patches...")
    _apply_qemu_patches()

    QEMU_BUILD.mkdir(parents=True, exist_ok=True)

    # Full configure argv — written verbatim to QEMU_CONFIGURE_MARKER so
    # any change (targets, extra-cflags, debug flags, ...) re-triggers
    # configure. QEMU_TARGETS_MARKER is still written for backward
    # compatibility with older build trees / tooling that reads it.
    configure_cmd = [
        str(QEMU_SRC / "configure"),
        "--target-list=%s" % QEMU_TARGETS,
        "--prefix=%s" % (REMU_ROOT / "install"),
        "--enable-debug",
        "--disable-werror",
        "--disable-docs",
        "--disable-guest-agent",
        "--extra-cflags=-I%s -I%s" % (INCLUDE_SRC, BRIDGE_SRC),
    ]
    configure_key = "\n".join(configure_cmd) + "\n"

    have_build = (QEMU_BUILD / "build.ninja").exists()
    have_configure = (
        QEMU_CONFIGURE_MARKER.is_file()
        and QEMU_CONFIGURE_MARKER.read_text() == configure_key
    )
    if not have_build or not have_configure:
        if have_build and not have_configure:
            click.echo("Configure argv changed; re-configuring...")
        else:
            click.echo("Configuring QEMU (%s)..." % QEMU_TARGETS)
        _run(configure_cmd, cwd=QEMU_BUILD)
        QEMU_TARGETS_MARKER.write_text(QEMU_TARGETS + "\n")
        QEMU_CONFIGURE_MARKER.write_text(configure_key)

    # Build
    nj = jobs if jobs > 0 else os.cpu_count()
    click.echo("Building with %d jobs..." % nj)
    _run(["ninja", "-j", str(nj)], cwd=QEMU_BUILD)

    click.secho("\nBuild complete:", fg="green")
    click.secho("  aarch64 (NPU side):  %s" % QEMU_BIN, fg="green")
    if QEMU_BIN_X86.is_file():
        click.secho("  x86_64 (host side):  %s" % QEMU_BIN_X86, fg="green")
    else:
        click.secho("  x86_64 (host side):  NOT BUILT", fg="yellow")


# ── fw-build ─────────────────────────────────────────────────────────────────

# q-sys build.sh targets that have real build_* functions. Note: the `rtos`
# alias advertised in build.sh --help has no corresponding build_rtos in
# q-sys scripts/, so we don't expose it here.
FW_COMPONENTS = ["all", "tf-a", "cp0", "cp1", "cm", "boot", "bootrom", "cmrt"]
FW_PLATFORMS = ["silicon", "zebu", "zebu_ci", "zebu_vdk"]
# `silicon` is the only profile REMU exercises end-to-end; the other
# three are passed through to q-sys `build.sh` for developer sanity
# checks but are no longer part of any REMU regression loop. `fw-build`
# emits a deprecation warning whenever one is selected explicitly.
FW_DEPRECATED_PLATFORMS = {"zebu", "zebu_ci", "zebu_vdk"}
FW_MODES = ["debug", "release", "profile"]

# Minimum component set for a CA73-only boot (no RoT/CMRT/PCIe-CM7).
FW_CA73_SEQUENCE = ["tf-a", "cp0", "cp1"]

# Per-component expected outputs (relative to q-sys binaries/). build.sh
# always runs generate_gpt after each invocation, and GPT packs *all*
# partitions — so a per-component build exits non-zero on the GPT step
# even though the component's own .bin files were produced. We ignore the
# exit code and validate via these output paths instead.
FW_COMPONENT_OUTPUTS = {
    "tf-a":    ["BootLoader_CP/bl1.bin",
                "BootLoader_CP/bl2.bin",
                "BootLoader_CP/bl31.bin"],
    "cp0":     ["FreeRTOS_CP/freertos_kernel.bin"],
    "cp1":     ["FreeRTOS_CP1/bl31.bin",
                "FreeRTOS_CP1/freertos_kernel.bin"],
    "cm":      ["FreeRTOS_PCIE/freertos_kernel.bin"],
    "boot":    ["BootLoader_CP/fboot_n.bin"],
    "bootrom": ["BootLoader_CP/fboot_n.bin"],
    "cmrt":    [],  # varies; skip strict validation
    "all":     [],
}


def _fw_env():
    """Build an env dict with toolchain paths; error out if any are missing."""
    env = os.environ.copy()
    env.setdefault("COMPILER_PATH_CMAKE", shutil.which("cmake") or "cmake")
    env.setdefault("COMPILER_PATH_ARM64", DEFAULT_COMPILER_PATH_ARM64)
    env.setdefault("COMPILER_PATH_ARM32", DEFAULT_COMPILER_PATH_ARM32)

    for key in ("COMPILER_PATH_ARM64", "COMPILER_PATH_ARM32"):
        bin_dir = Path(env[key])
        if not bin_dir.is_dir():
            click.secho(
                "Toolchain not found: %s=%s" % (key, bin_dir), fg="red")
            click.echo("Override with env, e.g. %s=/path/to/toolchain/bin" % key)
            raise SystemExit(1)
    return env


def _install_fw():
    """Copy q-sys output binaries into images/ under remu-expected names."""
    IMAGES_DIR.mkdir(parents=True, exist_ok=True)
    copied = 0
    for src_rel, dest_name in FW_INSTALL_MAP:
        src = SSW_SYS_BINARIES / src_rel
        dest = IMAGES_DIR / dest_name
        if src.is_file():
            shutil.copy2(src, dest)
            click.secho("  %s -> %s" % (src_rel, dest.name), fg="green")
            copied += 1
        else:
            click.secho("  [--] %s missing (not built)" % src_rel, dim=True)
    if copied == 0:
        click.secho("No images copied.", fg="yellow")


@cli.command("fw-build")
@click.option("--component", "-c", "components", multiple=True,
              type=click.Choice(FW_COMPONENTS),
              help="Build target(s). Repeatable. Default: tf-a + cp0 + cp1 "
                   "(minimum CA73 boot set).")
@click.option("--platform", "-p", default="silicon",
              type=click.Choice(FW_PLATFORMS),
              help="Target platform. Default 'silicon' (the only supported "
                   "profile; zebu/zebu_ci/zebu_vdk are deprecated).")
@click.option("--mode", "-m", default="debug",
              type=click.Choice(FW_MODES))
@click.option("--chiplets", "-cl", type=int, default=4,
              help="Chiplet count (1 or 4). Default 4.")
@click.option("--clean", is_flag=True, help="Clean before build.")
@click.option("--install/--no-install", default=True,
              help="Copy resulting binaries into images/ (default on).")
def fw_build(components, platform, mode, chiplets, clean, install):
    """Build R100 firmware from q-sys (external/ssw-bundle)."""
    if not SSW_SYS_BUILD.is_file():
        click.secho("q-sys not found at %s" % SSW_SYS, fg="red")
        click.echo("Initialize: git submodule update --init --recursive")
        raise SystemExit(1)

    targets = list(components) if components else list(FW_CA73_SEQUENCE)
    env = _fw_env()

    if platform in FW_DEPRECATED_PLATFORMS:
        click.secho(
            "Warning: platform '-p %s' is deprecated. REMU exercises only "
            "'silicon' end-to-end; the zebu* profiles build but are not "
            "part of any regression. Drop `-p` to use the implicit "
            "'silicon' default." % platform, fg="yellow")

    click.echo("q-sys fw-build: targets=%s platform=%s mode=%s chiplets=%d" %
               (",".join(targets), platform, mode, chiplets))
    click.echo("  ARM64: %s" % env["COMPILER_PATH_ARM64"])
    click.echo("  ARM32: %s" % env["COMPILER_PATH_ARM32"])

    # Apply any patches under cli/fw-patches/ before invoking build.sh.
    # Policy: this directory is kept empty (the q-sys submodule must stay
    # byte-identical to upstream; unmodelled-block workarounds live on the
    # QEMU side). The apply call is still made so any ad-hoc local
    # debugging patch a developer drops into cli/fw-patches/ gets picked
    # up automatically — see cli/fw-patches/README.md.
    click.echo("Applying FW source patches...")
    _apply_fw_patches()

    for i, target in enumerate(targets):
        click.echo()
        click.secho("[%d/%d] %s" % (i + 1, len(targets), target), fg="cyan")
        cmd = ["bash", str(SSW_SYS_BUILD), target,
               "-p", platform, "-m", mode, "-cl", str(chiplets)]
        if clean:
            cmd.append("-c")
        # Ignore exit code: build.sh always runs generate_gpt last, which
        # fails when intermediate artifacts are missing (e.g. cp1_bl31.sign
        # during a tf-a-only build). The per-component clean_ step deletes
        # the component's own outputs before rebuild, so existence of the
        # expected files after the run is sufficient proof of success.
        completed = _run(cmd, cwd=SSW_SYS, env=env, check=False)

        expected = FW_COMPONENT_OUTPUTS.get(target, [])
        missing = [rel for rel in expected
                   if not (SSW_SYS_BINARIES / rel).is_file()]
        if missing:
            click.secho("fw-build(%s) failed: missing outputs %s (exit=%d)" %
                        (target, missing, completed.returncode), fg="red")
            raise SystemExit(completed.returncode or 1)

        if completed.returncode != 0:
            click.secho("  build.sh exit=%d (GPT assembly incomplete — "
                        "expected for partial builds)" %
                        completed.returncode, fg="yellow")

    if install:
        click.echo("\nInstalling images into %s ..." % IMAGES_DIR)
        _install_fw()


# ── run ──────────────────────────────────────────────────────────────────────

def _make_run_dir(name, output_root):
    """Create <output_root>/<name>/ (defaulting to output/run-<ts>/) and
    refresh the output/latest -> <name> convenience symlink. Returns
    (run_dir, run_name)."""
    root = Path(output_root).resolve() if output_root else OUTPUT_ROOT
    root.mkdir(parents=True, exist_ok=True)
    if not name:
        name = "run-" + time.strftime("%Y%m%d-%H%M%S")
    run_dir = root / name
    run_dir.mkdir(parents=True, exist_ok=True)

    latest = root / "latest"
    try:
        if latest.is_symlink() or latest.exists():
            latest.unlink()
        latest.symlink_to(name)
    except OSError:
        pass
    return run_dir, name


def _build_npu_cmd(run_dir, gdb, trace, with_host=False,
                   npu_monitor_sock=None, doorbell_sock=None,
                   doorbell_log=None, msix_sock=None, msix_log=None,
                   issr_sock=None, issr_log=None,
                   hdma_sock=None, hdma_log=None,
                   rbdma_log=None,
                   smmu_log=None,
                   ):
    """Assemble the aarch64 QEMU cmdline for the R100 NPU side.
    Matches the previous in-line flow exactly; extracted so that
    --host can extend it (Phase 2) without duplicating boot-image
    and UART wiring logic.

    with_host: if True, wires `-machine r100-soc,memdev=remushm` so the
    r100-soc machine splices the shared memory-backend-file over chiplet
    0's DRAM head (M5). The `remushm` object itself is appended later by
    the `run` command along with the PCI-side plumbing.

    npu_monitor_sock: if given (path), an additional Unix-socket HMP
    monitor is attached to the NPU QEMU so the CLI can programmatically
    query `info mtree` / `xp`. The existing stdio-muxed readline monitor
    on uart0 keeps working for interactive use.

    doorbell_sock / doorbell_log: M6 plumbing. When `doorbell_sock` is a
    path, the NPU attaches a client socket chardev (connecting to the
    matching server on the host side) and wires it into the r100-soc
    machine's `doorbell` option. When `doorbell_log` is a path, a file
    chardev is also attached and fed to the machine's `doorbell-debug`
    option so every received frame appends an ASCII line to it.

    msix_sock / msix_log: M7 plumbing — reverse-direction (FW → host
    MSI-X) chardev. When `msix_sock` is a path, the NPU attaches a
    client socket chardev (host owns the listener, mirror of the M6
    doorbell socket) and wires it into the r100-soc machine's `msix`
    option. On FW stores to REBELH_PCIE_MSIX_ADDR (0x1BFFFFFFFC) the
    r100-imsix device emits an 8-byte (offset, db_data) frame on this
    chardev; the host's r100-npu-pci consumes it and fires
    msix_notify(). An optional `msix_log` file chardev echoes every
    emitted frame as an ASCII line for humans and tests.

    issr_sock / issr_log: M8 plumbing — NPU→host ISSR shadow-egress.
    When `issr_sock` is a path, the NPU attaches a client socket
    chardev (same host-as-server pattern as M6/M7) and wires it into
    the r100-soc machine's `issr` option, which the chiplet-0
    r100-mailbox picks up. Every MMIO-path write to one of its
    ISSR0..63 scratch registers emits an 8-byte (BAR4-offset, value)
    frame; the host-side r100-npu-pci write-throughs `value` into
    bar4_mmio_regs[BAR4-offset/4] so a later KMD readl() on the
    matching BAR4 offset observes the FW-written magic (FW_BOOT_DONE
    on ISSR[4], reset counters on ISSR[7], and so on). Optional
    `issr_log` file chardev echoes one ASCII line per egress.

    hdma_sock / hdma_log: NPU→host HDMA executor. Variable-length
    frames (24-byte header + payload, remu_hdma_proto.h):
    `r100-hdma`'s MMIO-driven channels (OP_WRITE / OP_READ_REQ in the
    0x80..0xBF req_id partition) for q-cp's `cmd_descr_hdma` LL kicks
    against the host. The host-side r100-npu-pci decodes each frame,
    performs the matching pci_dma_{read,write} against the x86 guest,
    and for OP_READ_REQ emits OP_READ_RESP back to the NPU. Optional
    `hdma_log` file chardev echoes one ASCII line per frame (either
    direction) — `dir=tx` is NPU→host, `dir=rx` is the host seeing
    NPU egress.

    P10-fix retired the prior `cfg` chardev (host→NPU 8-byte
    (cfg-head-offset, value) frames) and the matching r100-pcie-
    outbound chardev fallback (req_id 0xC0..0xFF for synchronous
    PF-window reads + the r100-cm7 cfg-mirror reverse path on
    req_id 0x00). The cfg-head propagation is now backed by the
    shared `cfg-shadow` memory-backend-file (4 KB) and the outbound
    iATU window by the shared `host-ram` memory-backend-file —
    both aliased identically on the host's r100-npu-pci side, so
    kmd writes are visible to q-cp on the next read with no
    chardev queueing and no BQL-stalled cond_wait."""
    uart0_log = run_dir / "uart0.log"
    hils_log = run_dir / "hils.log"

    machine_opt = "r100-soc"
    if with_host:
        machine_opt += ",memdev=remushm"
        # P10-fix: the same `host-ram` shm-backed memory-backend-file
        # the host x86 QEMU uses for its main RAM is also instantiated
        # on the NPU side here, so r100-pcie-outbound can install a
        # MemoryRegion alias over it (size = host RAM, mapped at
        # R100_PCIE_AXI_SLV_BASE_ADDR). q-cp's outbound iATU loads /
        # stores then resolve as plain RAM accesses against the same
        # mmap the kmd polls — no chardev hop, no BQL contention.
        machine_opt += ",host-ram=hostram"
        # P10-fix: 4 KB cfg-shadow shm shared with the host's BAR2
        # cfg-head trap. The r100-cm7 cfg-mirror at
        # DEVICE_COMMUNICATION_SPACE_BASE installs a
        # `memory_region_init_alias` over offset 0 of this backend so
        # kmd writes to FUNC_SCRATCH / DDH_BASE_LO are visible to
        # q-cp's next read with no chardev hop.
        machine_opt += ",cfg-shadow=cfgshadow"
    if doorbell_sock is not None:
        machine_opt += ",doorbell=doorbell"
        if doorbell_log is not None:
            machine_opt += ",doorbell-debug=doorbell_dbg"
    if msix_sock is not None:
        machine_opt += ",msix=msix"
        if msix_log is not None:
            machine_opt += ",msix-debug=msix_dbg"
    if issr_sock is not None:
        machine_opt += ",issr=issr"
        if issr_log is not None:
            machine_opt += ",issr-debug=issr_dbg"
    if hdma_sock is not None:
        machine_opt += ",hdma=hdma"
        if hdma_log is not None:
            machine_opt += ",hdma-debug=hdma_dbg"
    if rbdma_log is not None:
        machine_opt += ",rbdma-debug=rbdma_dbg"
    if smmu_log is not None:
        machine_opt += ",smmu-debug=smmu_dbg"

    cmd = [
        str(QEMU_BIN),
        "-M", machine_opt,
        "-display", "none",
        "-chardev",
        "stdio,id=uart0,mux=on,signal=off,logfile=%s,logappend=off" % uart0_log,
        "-mon", "chardev=uart0,mode=readline",
        "-serial", "chardev:uart0",
    ]
    if npu_monitor_sock is not None:
        cmd += ["-monitor",
                "unix:%s,server=on,wait=off" % npu_monitor_sock]
    if doorbell_sock is not None:
        # Client side: the host-side QEMU owns the listening socket (it
        # starts first and we poll for its bind before launching NPU).
        # reconnect=1 keeps retrying for a second in case the host is
        # still coming up when the NPU machine-init runs.
        cmd += [
            "-chardev",
            "socket,id=doorbell,path=%s,reconnect=1" % doorbell_sock,
        ]
        if doorbell_log is not None:
            cmd += [
                "-chardev",
                "file,id=doorbell_dbg,path=%s,mux=off" % doorbell_log,
            ]
    if msix_sock is not None:
        # Same client/server pattern as doorbell — host QEMU owns the
        # listener so the PCI device side has the r100-npu-pci
        # realize-time set_handlers path already installed by the time
        # the NPU opens the socket.
        cmd += [
            "-chardev",
            "socket,id=msix,path=%s,reconnect=1" % msix_sock,
        ]
        if msix_log is not None:
            cmd += [
                "-chardev",
                "file,id=msix_dbg,path=%s,mux=off" % msix_log,
            ]
    if issr_sock is not None:
        # M8: same host-as-server client pattern as M6/M7. Host owns
        # the listener (its realize installs the issr_chr receive
        # handler first); NPU connects as client and every FW-side
        # ISSR write egresses over this socket.
        cmd += [
            "-chardev",
            "socket,id=issr,path=%s,reconnect=1" % issr_sock,
        ]
        if issr_log is not None:
            cmd += [
                "-chardev",
                "file,id=issr_dbg,path=%s,mux=off" % issr_log,
            ]
    if hdma_sock is not None:
        # M8b 3b/3c: NPU<->host HDMA executor. Host listens; NPU connects
        # and emits variable-length frames. 3c makes the channel
        # bidirectional (OP_READ_RESP flows host→NPU), so the same
        # chardev carries both directions — the `reconnect=1` setting
        # is fine either way, QEMU socket chardevs are full-duplex.
        cmd += [
            "-chardev",
            "socket,id=hdma,path=%s,reconnect=1" % hdma_sock,
        ]
        if hdma_log is not None:
            cmd += [
                "-chardev",
                "file,id=hdma_dbg,path=%s,mux=off" % hdma_log,
            ]
    if rbdma_log is not None:
        cmd += [
            "-chardev",
            "file,id=rbdma_dbg,path=%s,mux=off" % rbdma_log,
        ]
    if smmu_log is not None:
        # P11/P10 SMMU post-mortem aid. Wired to chiplet 0's r100-smmu
        # only (CharBackend is single-frontend; q-cp only programs the
        # chiplet-0 SMMU on the BD lifecycle path). Always-on once the
        # file is opened — the device's emit short-circuits when the
        # backend isn't connected.
        cmd += [
            "-chardev",
            "file,id=smmu_dbg,path=%s,mux=off" % smmu_log,
        ]
    click.echo("  Chiplet 0 UART -> stdio (log: %s)" % uart0_log)

    for n in range(1, 4):
        log_path = run_dir / ("uart%d.log" % n)
        cmd += [
            "-chardev",
            "file,id=uart%d,path=%s,mux=off" % (n, log_path),
            "-serial", "chardev:uart%d" % n,
        ]
        click.echo("  Chiplet %d UART -> %s" % (n, log_path))

    cmd += [
        "-chardev",
        "file,id=hils,path=%s,mux=off" % hils_log,
        "-serial", "chardev:hils",
    ]
    click.echo("  HILS ring tail -> %s" % hils_log)

    if gdb:
        cmd += ["-s", "-S"]
        click.secho("Waiting for GDB on localhost:1234...", fg="yellow")

    if trace:
        cmd += ["-d", "guest_errors,unimp",
                "-D", str(run_dir / "qemu.log")]

    found = 0
    for fname, addr, desc in FW_IMAGES:
        path = IMAGES_DIR / fname
        if path.is_file():
            cmd += ["-device", "loader,file=%s,addr=0x%x" % (path, addr)]
            found += 1

    for chiplet_id in range(1, R100_NUM_CHIPLETS):
        for fname, base, _desc in FW_PER_CHIPLET_IMAGES:
            path = IMAGES_DIR / fname
            if path.is_file():
                addr = chiplet_id * R100_CHIPLET_OFFSET + base
                cmd += ["-device",
                        "loader,file=%s,addr=0x%x" % (path, addr)]
                found += 1

    return cmd, found


def _setup_shm(run_name, size, fname="remu-shm"):
    """Create /dev/shm/remu-<run>/<fname> of exactly `size` bytes.
    The directory lives on tmpfs to keep large backing files off the
    workspace filesystem. Returns (shm_dir, shm_file).

    Idempotent if the file already exists at the correct size; larger
    or smaller files are truncated to size (caller wiped the previous
    run, or the run name is new).

    Used twice per `--host` run: once for `remu-shm` (BAR0 / chiplet-0
    DRAM splice, M4/M5) and once for `host-ram` (the entire x86 host
    guest RAM, shared so r100-pcie-outbound can alias the kmd's
    coherent DMA pages directly — no chardev round trip)."""
    shm_dir = SHM_ROOT / ("remu-" + run_name)
    shm_dir.mkdir(parents=True, exist_ok=True)
    shm_file = shm_dir / fname
    if shm_file.exists() and shm_file.stat().st_size == size:
        pass
    else:
        with open(shm_file, "wb") as f:
            f.truncate(size)
    return shm_dir, shm_file


# Memory-suffix parser for `--host-mem`. QEMU accepts `512M`, `1G`,
# `0x10000000`, raw bytes, etc. The CLI's existing `--host-mem` is a
# pass-through to `-m`, but the host-ram memory-backend-file we splice
# under it needs an exact byte count — both for the file's truncate
# and for the matching `size=` arg on the `-object memory-backend-file`
# line (QEMU enforces the two are equal).
def _parse_qemu_size(s):
    """Parse a QEMU-style size string (e.g. '512M', '1G', '0x10000000',
    '12345') into bytes. Mirrors the suffix table QEMU's
    `qemu_strtosz` accepts; we keep this small list since `--host-mem`
    is the only consumer."""
    s = str(s).strip()
    if not s:
        raise ValueError("empty size")
    suffixes = {"k": 1024, "m": 1024 ** 2, "g": 1024 ** 3,
                "t": 1024 ** 4}
    if s[-1].lower() in suffixes:
        return int(float(s[:-1]) * suffixes[s[-1].lower()])
    if s.startswith(("0x", "0X")):
        return int(s, 16)
    return int(s)


# ── cleanup helpers ──────────────────────────────────────────────────────────
#
# The normal teardown path (atexit + SIGTERM/HUP/PIPE handlers in `run`) wipes
# /dev/shm/remu-<name>/ and kills the child QEMU processes. Anything that
# bypasses that path — SIGKILL on the python wrapper, a crashing test harness,
# a machine reboot mid-run — leaves orphan QEMU processes mmap'd over the shm
# file plus stale *.sock files that would EADDRINUSE-block the next bind.
# `_cleanup_run_trash` is the belt-and-braces sweep: narrowly scoped to a
# given run name so concurrent runs with different names stay untouched.

def _pid_alive(pid):
    """True iff pid currently exists and we can signal it."""
    try:
        os.kill(pid, 0)
    except (ProcessLookupError, PermissionError):
        return False
    return True


def _qemu_pids_referencing(needles):
    """Return list of (pid, cmdline) for `qemu-system-*` processes whose
    /proc/<pid>/cmdline contains ANY of the given string needles.
    `needles` is a list of absolute paths (shm dir, output dir, ...);
    passing run-specific paths keeps the sweep from touching unrelated
    simulations."""
    needles = [str(n) for n in needles if n]
    hits = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            comm = (entry / "comm").read_text().strip()
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if not comm.startswith("qemu-system"):
            continue
        try:
            cmdline = (entry / "cmdline").read_bytes().replace(
                b"\0", b" ").decode("utf-8", "replace")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if any(n in cmdline for n in needles):
            hits.append((int(entry.name), cmdline))
    return hits


def _kill_and_wait(pids, grace=3.0):
    """SIGTERM all, wait up to `grace` seconds for each to exit, then
    SIGKILL stragglers. Silent on lookup / permission races."""
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass
    deadline = time.time() + grace
    remaining = list(pids)
    while remaining and time.time() < deadline:
        time.sleep(0.1)
        remaining = [p for p in remaining if _pid_alive(p)]
    for pid in remaining:
        try:
            os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def _cleanup_run_trash(run_name, output_root=None, verbose=True):
    """Best-effort teardown of anything a prior run with `run_name` may
    have left behind. Matches are scoped by path so concurrent runs with
    *different* names keep working untouched.

      1. QEMU processes whose cmdline mentions /dev/shm/remu-<name>/ or
         output/<name>/ — they're the workers of a prior invocation.
      2. The /dev/shm/remu-<name>/ tmpfs directory (and anything inside).
      3. Stale *.sock files under output/<name>/ — QEMU socket-chardev
         servers refuse to bind an existing path.

    Returns a dict with counts (`procs`, `shm`, `socks`) so callers can
    summarize without re-walking /proc."""
    root = Path(output_root).resolve() if output_root else OUTPUT_ROOT
    out_dir = root / run_name
    shm_dir = SHM_ROOT / ("remu-" + run_name)

    needles = [str(shm_dir)]
    if out_dir.exists():
        needles.append(str(out_dir))
    hits = _qemu_pids_referencing(needles)
    if hits and verbose:
        click.echo("  Cleaning %d orphan QEMU process(es) from prior '%s':"
                   % (len(hits), run_name))
        for pid, _cmd in hits:
            click.echo("    pid %d" % pid)
    _kill_and_wait([p for p, _ in hits])

    shm_removed = False
    if shm_dir.exists():
        try:
            shutil.rmtree(shm_dir)
            shm_removed = True
            if verbose:
                click.echo("  Removed %s" % shm_dir)
        except OSError as e:
            if verbose:
                click.secho("  Warning: could not remove %s: %s"
                            % (shm_dir, e), fg="yellow")

    socks_removed = 0
    if out_dir.exists():
        for p in out_dir.rglob("*.sock"):
            try:
                p.unlink()
                socks_removed += 1
            except OSError:
                pass
        if socks_removed and verbose:
            click.echo("  Removed %d stale socket file(s) under %s"
                       % (socks_removed, out_dir))

    return {"procs": len(hits), "shm": 1 if shm_removed else 0,
            "socks": socks_removed}


def _build_host_cmd(host_dir, shm_file, shm_size, monitor_sock, mem,
                    doorbell_sock=None, msix_sock=None, msix_log=None,
                    issr_sock=None, issr_log=None,
                    hdma_sock=None, hdma_log=None,
                    host_ram_file=None, host_ram_size=None,
                    cfg_shadow_file=None,
                    kernel=None, initrd=None, share_dir=None,
                    cmdline_extra=None):
    """Assemble the x86_64 QEMU cmdline for the Phase-2 host side.

    M3 upgraded the placeholder ivshmem-plain to our own `r100-npu-pci`
    (0x1eff:0x2030) with the four BARs at the sizes rebellions.ko
    expects (36 GB DDR, 64 MB ACP, 8 MB doorbell, 1 MB MSI-X). M4 wires
    the memory-backend-file through the device's `memdev` link so
    BAR0 offset 0..shm_size is backed by the shared /dev/shm file;
    since the NPU-side QEMU holds the same file mapped, stores from
    the x86 guest into BAR0 end up page-visible to the NPU process
    (NPU-CPU-visible DRAM integration lands in M5). M6 adds the
    doorbell chardev: when `doorbell_sock` is set, the r100-npu-pci
    BAR4 becomes a hybrid MMIO+RAM region whose writes to
    MAILBOX_INTGR0/INTGR1 emit 8-byte (offset, value) frames on a
    Unix socket that the NPU QEMU is connected to.

    M7 adds the reverse-direction `msix` chardev: when `msix_sock` is
    set, the r100-npu-pci device receives 8-byte (offset, db_data)
    frames from the NPU-side r100-imsix peripheral (FW stores to
    REBELH_PCIE_MSIX_ADDR) and fires msix_notify() for the encoded
    vector. `msix_log`, when set, is a file chardev wired to the
    device's `msix-debug` option so every frame leaves an ASCII trail.

    M8 adds the `issr` chardev: the same 8-byte (offset, value) frame
    format but carrying NPU-side ISSR writes from the chiplet-0
    r100-mailbox into the host's BAR4 MMIO register file. `issr_log`,
    when set, is a file chardev wired to the device's `issr-debug`
    option so every ingressed frame leaves an ASCII trail. Host-side
    BAR4 MAILBOX_BASE writes flow OUT over the existing `doorbell`
    chardev (offset disambiguation is done by the NPU r100-cm7),
    so M8 only needs one new chardev, not two.

    SeaBIOS IS allowed to run (no `-S`) so the BAR addresses get
    programmed — the automated verify queries `info mtree`, which
    only shows BAR subregions after they're placed into the pci
    address space. The guest hangs harmlessly at "No bootable
    device" afterwards in an HLT idle loop (serial to file, no
    stdout pollution). KVM disabled; later milestones swap the
    bootrom-only setup for a real Linux guest (M8+).
    """
    stdout_log = host_dir / "qemu.stdout.log"
    stderr_log = host_dir / "qemu.stderr.log"

    device_arg = "r100-npu-pci,memdev=remushm"
    chardevs = []
    if doorbell_sock is not None:
        # Host is the server since it starts first; NPU connects as
        # client (see _build_npu_cmd). wait=off lets host QEMU proceed
        # without blocking on the NPU connection so BAR programming /
        # info-pci verification still races ahead.
        chardevs += [
            "-chardev",
            "socket,id=doorbell,path=%s,server=on,wait=off" % doorbell_sock,
        ]
        device_arg += ",doorbell=doorbell"
    if msix_sock is not None:
        chardevs += [
            "-chardev",
            "socket,id=msix,path=%s,server=on,wait=off" % msix_sock,
        ]
        device_arg += ",msix=msix"
        if msix_log is not None:
            chardevs += [
                "-chardev",
                "file,id=msix_dbg,path=%s,mux=off" % msix_log,
            ]
            device_arg += ",msix-debug=msix_dbg"
    if issr_sock is not None:
        chardevs += [
            "-chardev",
            "socket,id=issr,path=%s,server=on,wait=off" % issr_sock,
        ]
        device_arg += ",issr=issr"
        if issr_log is not None:
            chardevs += [
                "-chardev",
                "file,id=issr_dbg,path=%s,mux=off" % issr_log,
            ]
            device_arg += ",issr-debug=issr_dbg"
    if hdma_sock is not None:
        # M8b 3b hdma: host owns the listener, receives
        # HDMA_OP_WRITE frames from the NPU-side CM7 stub and
        # executes them as pci_dma_write into the guest's DMA
        # address space.
        chardevs += [
            "-chardev",
            "socket,id=hdma,path=%s,server=on,wait=off" % hdma_sock,
        ]
        device_arg += ",hdma=hdma"
        if hdma_log is not None:
            chardevs += [
                "-chardev",
                "file,id=hdma_dbg,path=%s,mux=off" % hdma_log,
            ]
            device_arg += ",hdma-debug=hdma_dbg"

    # `qemu64` is a minimal CPU model missing BMI2/AVX2/etc. Our kmd
    # (rebellions.ko) is built on the host with `-march=native` and
    # emits VEX-encoded BMI2 instructions (e.g. BZHI) that trap as
    # #UD on qemu64, killing `rbln_init` immediately. `max` under TCG
    # exposes every feature QEMU can emulate so the stock .ko just
    # works without patching the kmd Makefile.
    cpu_model = "max" if kernel is not None else "qemu64"
    # P10-fix: back the x86 guest's main RAM with a shareable
    # /dev/shm file so the NPU-side r100-pcie-outbound can alias the
    # same pages directly (kmd's `dma_alloc_coherent` lands inside this
    # file). Without this, q-cp's outbound iATU reads were forced to
    # chardev-RPC over `hdma`, and the host's chardev iothread was
    # starved while the kmd held BQL inside `readl_poll_timeout_atomic`
    # waiting for `init_done` — see docs/roadmap.md → P10. With the
    # shared backend, q-cp loads/stores against the iATU window are a
    # single TCG instruction against the same mmap the kmd polls.
    machine_arg = "pc"
    if host_ram_file is not None:
        machine_arg += ",memory-backend=hostram"
    cmd = [
        str(QEMU_BIN_X86),
        "-M", machine_arg,
        "-cpu", cpu_model,
        "-m", mem,
        "-display", "none",
        "-nographic",
        "-no-reboot",
        "-object",
        "memory-backend-file,id=remushm,mem-path=%s,size=%d,share=on"
        % (shm_file, shm_size),
    ]
    if host_ram_file is not None:
        cmd += [
            "-object",
            "memory-backend-file,id=hostram,mem-path=%s,size=%d,share=on"
            % (host_ram_file, host_ram_size),
        ]
    # P10-fix: 4 KB cfg-shadow shm; r100-npu-pci aliases offset 0 over
    # the BAR2 cfg-head subregion (REMU_BAR2_CFG_HEAD_OFF) and the NPU
    # r100-cm7 aliases the same file at DEVICE_COMMUNICATION_SPACE_BASE.
    # Same backing file → kmd writes to FUNC_SCRATCH/DDH_BASE_LO are
    # immediately visible to q-cp without going through the cfg
    # chardev queue.
    if cfg_shadow_file is not None:
        cmd += [
            "-object",
            "memory-backend-file,id=cfgshadow,mem-path=%s,size=%d,share=on"
            % (cfg_shadow_file, CFG_SHADOW_SIZE),
        ]
        device_arg += ",cfg-shadow=cfgshadow"

    # Boot a real Linux guest (M8b Stage 2): prebuilt distro kernel +
    # tiny busybox/initramfs (see guest/build-guest-image.sh). Without
    # these args the guest stays at the SeaBIOS "No bootable device"
    # prompt so earlier milestone tests keep working unchanged.
    if kernel is not None:
        append = "console=ttyS0 rdinit=/init earlyprintk=serial"
        if cmdline_extra:
            append += " " + cmdline_extra
        cmd += [
            "-kernel", str(kernel),
            "-append", append,
        ]
        if initrd is not None:
            cmd += ["-initrd", str(initrd)]

    # virtio-9p pass-through so the guest's /init can mount the repo's
    # guest/ directory at /mnt/remu and drop its kmd-insmod +
    # FW_BOOT_DONE probe artifacts right back onto the host FS.
    if share_dir is not None:
        cmd += [
            "-fsdev",
            "local,id=remu,path=%s,security_model=none" % share_dir,
            "-device", "virtio-9p-pci,fsdev=remu,mount_tag=remu",
        ]

    cmd += [
        *chardevs,
        "-device", device_arg,
        "-chardev",
        "socket,id=mon,path=%s,server=on,wait=off" % monitor_sock,
        "-mon", "chardev=mon,mode=readline",
        "-serial", "file:%s" % (host_dir / "serial.log"),
    ]
    return cmd, stdout_log, stderr_log


def _hmp_query(sock_path, cmd, timeout=5.0, connect_retries=20):
    """Talk to a QEMU HMP monitor on a unix socket and return the
    response to `cmd` (with the echoed command and trailing prompt
    stripped). Retries the connect for up to `connect_retries` * 0.1s
    while QEMU is still bringing the socket up.

    HMP in `mode=readline` echoes every keystroke with carriage returns
    and partial line redraws, so naive line-splitting picks up noise
    like "iininfinfoinfo info pci". We instead locate the final fully-
    echoed copy of `cmd` followed by a newline, and take everything
    between it and the trailing `(qemu) ` prompt as the real response.
    """
    import re
    import socket
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    for _ in range(connect_retries):
        try:
            s.connect(str(sock_path))
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.1)
    else:
        raise RuntimeError("HMP socket %s did not come up in time" % sock_path)

    def _read_until_prompt():
        buf = b""
        while b"(qemu) " not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.decode("utf-8", errors="replace")

    _read_until_prompt()
    s.sendall((cmd + "\n").encode())
    resp = _read_until_prompt()
    s.close()

    # Find the LAST fully-echoed `cmd\n` (or `cmd\r\n`) — everything
    # after that is the real response. Readline's partial-echo line
    # redraws use \r to return to column 0, so `cmd` reliably appears
    # once intact as the final echo before the response body.
    pattern = re.escape(cmd) + r"\r?\n"
    last = None
    for m in re.finditer(pattern, resp):
        last = m
    body = resp[last.end():] if last else resp

    # Drop trailing "(qemu) " prompt and any stray \r.
    body = body.replace("\r", "")
    if body.rstrip().endswith("(qemu)"):
        body = body.rstrip()[:-len("(qemu)")].rstrip()
    return body.strip("\n")


# r100-npu-pci IDs. Must match the constants compiled into
# src/host/r100_npu_pci.c (R100_PCI_VENDOR_ID / R100_PCI_DEVICE_ID) and
# the driver's PCI_VENDOR_ID_REBEL / PCI_ID_CR03 in
# external/ssw-bundle/.../kmd/rebellions/common/rebellions{,_drv}.{h,c}.
R100_PCI_VENDOR = "1eff"
R100_PCI_DEVICE = "2030"


def _verify_host_npu_pci(monitor_sock, info_pci_log):
    """Run `info pci` on the host QEMU monitor, save the raw output, and
    assert the r100-npu-pci device is present. Returns the captured
    output so the caller can show the relevant block to the user."""
    out = _hmp_query(monitor_sock, "info pci")
    info_pci_log.write_text(out + "\n")
    needle = "%s:%s" % (R100_PCI_VENDOR, R100_PCI_DEVICE)
    if needle not in out.lower().replace(" ", ""):
        click.secho(
            "Host QEMU did not expose r100-npu-pci (%s in info pci)" % needle,
            fg="red")
        click.echo(out)
        raise RuntimeError("r100-npu-pci verification failed")
    return out


def _proc_has_mmap(pid, shm_file):
    """Return True if /proc/<pid>/maps lists the given path. Any
    permission / race error is reported as False (the caller logs the
    discrepancy instead of bailing)."""
    try:
        maps = Path("/proc/%d/maps" % pid).read_text()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return False
    return str(shm_file) in maps


def _extract_mtree_container(mtree, container_name):
    """Return the `info mtree` snippet (header + indented children) for
    the first MemoryRegion whose line contains `container_name`, or None
    if not found. info mtree uses indent to denote sub-region nesting."""
    lines = mtree.splitlines()
    idx = None
    for i, ln in enumerate(lines):
        if container_name in ln:
            idx = i
            break
    if idx is None:
        return None
    header_indent = len(lines[idx]) - len(lines[idx].lstrip())
    snippet = [lines[idx]]
    for ln in lines[idx + 1:]:
        stripped = ln.lstrip()
        if not stripped:
            break
        indent = len(ln) - len(stripped)
        if indent <= header_indent:
            break
        snippet.append(ln)
    return "\n".join(snippet)


def _verify_shared_mapping(monitor_sock, shm_file, host_pid, npu_pid,
                           mtree_log, bar_poll_timeout=10.0):
    """Prove the shared-memory bridge actually reaches both processes:

      1. `info mtree` on the host QEMU monitor must list the
         /dev/shm backing file as a subregion at offset 0 of the
         pci@...r100.bar0.ddr container (so x86-guest stores into
         BAR0[0..shm_size) hit the shared file).
      2. Both the host and the NPU QEMU process must have the shm
         file present in /proc/<pid>/maps (i.e. both processes have
         the file mmap'd, which is what makes them coherent via the
         tmpfs page cache).

    The host QEMU is launched without `-S`, but SeaBIOS takes ~1-2 s
    to enumerate PCI and program BAR addresses; only after that does
    the BAR0 container show up in the pci address space. We poll
    `info mtree` for up to `bar_poll_timeout` s to let SeaBIOS catch
    up before failing.

    Returns the mtree snippet containing the BAR0 container so the
    caller can show it. Raises RuntimeError on any failure — the
    exception string is what gets printed to the user."""
    mtree = ""
    deadline = time.time() + bar_poll_timeout
    while time.time() < deadline:
        mtree = _hmp_query(monitor_sock, "info mtree", timeout=10.0)
        if "r100.bar0.ddr" in mtree:
            break
        time.sleep(0.3)

    mtree_log.write_text(mtree + "\n")

    snippet = _extract_mtree_container(mtree, "r100.bar0.ddr")
    if snippet is None:
        raise RuntimeError(
            "SeaBIOS never placed r100.bar0.ddr into the pci address "
            "space within %.1fs; see %s" % (bar_poll_timeout, mtree_log))

    # info mtree prints MemoryRegions by their NAME (the object id for
    # memory-backend-file). The backing path isn't in the mtree output
    # — that we verify separately via /proc/<pid>/maps below.
    if "remushm" not in snippet:
        raise RuntimeError(
            "BAR0 container does not contain the 'remushm' backend at "
            "offset 0; see %s" % mtree_log)

    if not _proc_has_mmap(host_pid, shm_file):
        raise RuntimeError(
            "host QEMU (pid %d) does not have %s mmap'd" %
            (host_pid, shm_file))
    if not _proc_has_mmap(npu_pid, shm_file):
        raise RuntimeError(
            "NPU QEMU (pid %d) does not have %s mmap'd" %
            (npu_pid, shm_file))

    return snippet


def _verify_doorbell_wired(host_monitor_sock, npu_monitor_sock,
                           host_mtree_log, npu_qtree_log,
                           poll_timeout=10.0):
    """Prove the M6 doorbell plumbing is alive on both sides.

    Host side: `info mtree` must list the `r100.bar4.mmio` subregion
    on the r100-npu-pci BAR4 container, which only exists when the
    `doorbell` chardev property was wired.

    NPU side: `info qtree` must list both a `r100-cm7` (conditional
    on our `-machine r100-soc,doorbell=<chardev>` option — the
    machine option kept its historical name; the device was renamed
    from r100-doorbell to r100-cm7 in M8b Stage 3c) and a
    `r100-mailbox` instance (unconditional; the CM7 stub links to it
    to deliver INTGR writes). Presence of both + a realized `chardev`
    property on the CM7 device is the server-coming-up ack. The socket
    connect itself is async (host is server, NPU is reconnect=1
    client), but the device being realized means the machine chose
    to instantiate it from our machine option.

    Raises RuntimeError with an actionable message on failure. Log
    files are always written (empty if the query itself errored out)
    so the caller can diff runs.
    """
    host_mtree = ""
    deadline = time.time() + poll_timeout
    while time.time() < deadline:
        host_mtree = _hmp_query(host_monitor_sock, "info mtree",
                                timeout=10.0)
        if "r100.bar4.mmio" in host_mtree:
            break
        time.sleep(0.3)
    host_mtree_log.write_text(host_mtree + "\n")
    if "r100.bar4.mmio" not in host_mtree:
        raise RuntimeError(
            "host-side BAR4 MMIO overlay missing (r100.bar4.mmio not in "
            "info mtree); see %s" % host_mtree_log)

    snippet = _extract_mtree_container(host_mtree, "r100.bar4.container")
    if snippet is None:
        # Fall back to showing the mmio line only.
        snippet = "\n".join(
            ln for ln in host_mtree.splitlines()
            if "r100.bar4" in ln
        )

    # NPU side: confirm both r100-cm7 (chardev ingress + CM7 stubs)
    # and r100-mailbox (the PCIE_PRIVATE SFR the CM7 now targets)
    # are in the device tree.
    qtree = _hmp_query(npu_monitor_sock, "info qtree", timeout=10.0)
    npu_qtree_log.write_text(qtree + "\n")
    for dev in ("r100-cm7", "r100-mailbox"):
        if dev not in qtree:
            raise RuntimeError(
                "NPU-side %s device not found in info qtree; see %s"
                % (dev, npu_qtree_log))

    return snippet


def _verify_msix_wired(host_monitor_sock, npu_monitor_sock,
                       host_mtree_log, npu_mtree_log,
                       poll_timeout=10.0):
    """Prove the M7 iMSIX plumbing is alive on both sides.

    NPU side: `info mtree` must list the `r100-imsix` MemoryRegion
    (placed in the global sysmem at R100_PCIE_IMSIX_BASE by the
    machine whenever `-machine r100-soc,msix=<chardev>` is set).
    Absence means the machine option didn't take (bad chardev id) or
    the device failed realize.

    Host side: `info mtree` must show BAR5 (`r100.bar5.msix`) placed
    in the pci address space by SeaBIOS, with the `msix-table` /
    `msix-pba` subregions overlayed on it — those are created by the
    `msix_init()` call in r100_npu_pci_realize, which only succeeds
    if the device was wired with room for the 32 vectors. SeaBIOS can
    take ~1-2 s to program the BAR addresses, so we poll.

    Raises RuntimeError with an actionable message on failure. Log
    files are always written (empty if the query itself errored out).
    """
    npu_mtree = ""
    deadline = time.time() + poll_timeout
    while time.time() < deadline:
        npu_mtree = _hmp_query(npu_monitor_sock, "info mtree",
                               timeout=10.0)
        if "r100-imsix" in npu_mtree:
            break
        time.sleep(0.3)
    npu_mtree_log.write_text(npu_mtree + "\n")
    if "r100-imsix" not in npu_mtree:
        raise RuntimeError(
            "NPU-side r100-imsix MMIO region missing from info mtree; "
            "see %s" % npu_mtree_log)

    npu_snippet_lines = [ln for ln in npu_mtree.splitlines()
                         if "r100-imsix" in ln]
    npu_snippet = "\n".join(npu_snippet_lines) or npu_mtree

    host_mtree = ""
    deadline = time.time() + poll_timeout
    while time.time() < deadline:
        host_mtree = _hmp_query(host_monitor_sock, "info mtree",
                                timeout=10.0)
        if "r100.bar5.msix" in host_mtree and "msix-table" in host_mtree:
            break
        time.sleep(0.3)
    host_mtree_log.write_text(host_mtree + "\n")
    if "r100.bar5.msix" not in host_mtree:
        raise RuntimeError(
            "host r100.bar5.msix not present in info mtree (SeaBIOS "
            "BAR5 programming never completed); see %s" % host_mtree_log)
    if "msix-table" not in host_mtree:
        raise RuntimeError(
            "host msix-table overlay missing on BAR5 (msix_init did "
            "not run); see %s" % host_mtree_log)

    host_snippet = _extract_mtree_container(host_mtree, "r100.bar5.msix") \
        or "\n".join(ln for ln in host_mtree.splitlines()
                     if "r100.bar5" in ln or "msix-" in ln)

    return npu_snippet + "\n" + host_snippet


def _verify_issr_wired(host_monitor_sock, npu_monitor_sock,
                       host_qtree_log, npu_qtree_log,
                       poll_timeout=10.0):
    """Prove the M8 ISSR shadow-egress plumbing is alive on both sides.

    Unlike M6/M7, M8 doesn't introduce a new MemoryRegion — the ISSR
    egress is a property on the existing chiplet-0 r100-mailbox, and
    the ingress is a CharBackend on the existing host r100-npu-pci.
    So we inspect `info qtree` on both sides and look for:

    NPU side:  r100-mailbox with a non-null `issr-chardev` property.
               Realization only succeeds when the chardev is found in
               qemu_chr_find(), so presence of a *value* here proves
               both the CLI wiring and the late-binding path.

    Host side: r100-npu-pci with a non-null `issr` property (a
               CharBackend prints as the chardev id, e.g. `issr`).

    Raises RuntimeError with an actionable message on failure.
    """
    qtree_npu = ""
    deadline = time.time() + poll_timeout
    while time.time() < deadline:
        qtree_npu = _hmp_query(npu_monitor_sock, "info qtree", timeout=10.0)
        if "issr-chardev" in qtree_npu and 'issr-chardev ""' not in qtree_npu:
            break
        time.sleep(0.3)
    npu_qtree_log.write_text(qtree_npu + "\n")

    npu_lines = [ln.rstrip() for ln in qtree_npu.splitlines()
                 if "issr-chardev" in ln]
    # Any non-empty chardev value (the chardev id string) proves the
    # CLI-to-machine-to-device wiring took. qdev prints unset
    # CharBackend properties as `prop "" ""` in info qtree.
    if not any('""' not in ln for ln in npu_lines):
        raise RuntimeError(
            "NPU-side r100-mailbox 'issr-chardev' property is unset; "
            "the -machine r100-soc,issr=<id> option didn't latch. "
            "See %s" % npu_qtree_log)
    npu_snippet = "\n".join(npu_lines) or "(no issr-chardev lines)"

    qtree_host = ""
    deadline = time.time() + poll_timeout
    while time.time() < deadline:
        qtree_host = _hmp_query(host_monitor_sock, "info qtree", timeout=10.0)
        # Host property is literally `issr` (not `issr-chardev`); the
        # CharBackend prints on its own line as `  issr = "issr"`.
        for ln in qtree_host.splitlines():
            stripped = ln.strip()
            if stripped.startswith("issr ") and '""' not in stripped:
                break
        else:
            time.sleep(0.3)
            continue
        break
    host_qtree_log.write_text(qtree_host + "\n")

    host_lines = [ln.rstrip() for ln in qtree_host.splitlines()
                  if ln.strip().startswith("issr ")
                  or ln.strip().startswith("issr-debug ")]
    if not any('""' not in ln for ln in host_lines
               if ln.strip().startswith("issr ")):
        raise RuntimeError(
            "host-side r100-npu-pci 'issr' property is unset; "
            "the -device r100-npu-pci,issr=<id> option didn't latch. "
            "See %s" % host_qtree_log)
    host_snippet = "\n".join(host_lines) or "(no issr lines)"

    return npu_snippet + "\n" + host_snippet


def _verify_cfg_hdma_wired(host_monitor_sock, npu_monitor_sock,
                           host_qtree_log, npu_qtree_log,
                           poll_timeout=10.0):
    """Prove the hdma plumbing is alive on both sides.

    P10-fix retired the `cfg` chardev path; cfg-head propagation
    flows through the shared `cfg-shadow` memory-backend-file alias
    which is verified separately by `_verify_npu_cfg_shadow_alias`
    (mtree-based). Only the bidirectional hdma chardev remains as a
    cross-process CharBackend that needs `info qtree` inspection:

    NPU side:
      r100-cm7  : present (no chardev props beyond the doorbell now);
      r100-hdma : `chardev` = "hdma" (non-empty), since M9-1c carved
                  the hdma channel onto its own device for q-cp's
                  dw_hdma_v0 MMIO model.

    Host side (r100-npu-pci):
      `hdma` = "hdma"

    Raises RuntimeError on failure with actionable detail. The caller
    in `run()` downgrades that to a red-text warning so the NPU still
    boots (same policy as the other post-launch verifiers).
    """
    def _has_nonempty(lines, key):
        for ln in lines:
            stripped = ln.strip()
            if not stripped.startswith(key + " "):
                continue
            if '""' in stripped:
                continue
            return True
        return False

    def _has_device_with_nonempty_prop(qtree_text, dev_name, prop):
        """Walk `info qtree` looking for a `dev: <dev_name>` block whose
        body contains `<prop> "<non-empty>"`. info qtree's indentation
        marks block boundaries — the dev's properties are the lines
        more deeply indented than the dev: line itself, until the next
        same-or-shallower indent."""
        target = "dev: " + dev_name + ","
        in_block = False
        block_indent = -1
        for raw in qtree_text.splitlines():
            stripped = raw.lstrip(" ")
            indent = len(raw) - len(stripped)
            if not in_block:
                if target in stripped:
                    in_block = True
                    block_indent = indent
                continue
            if indent <= block_indent and stripped:
                in_block = False
                continue
            if stripped.startswith(prop + " ") and '""' not in stripped:
                return True
        return False

    qtree_npu = ""
    deadline = time.time() + poll_timeout
    while time.time() < deadline:
        qtree_npu = _hmp_query(npu_monitor_sock, "info qtree", timeout=10.0)
        if "r100-hdma" in qtree_npu:
            break
        time.sleep(0.3)
    npu_qtree_log.write_text(qtree_npu + "\n")

    npu_lines = [ln.rstrip() for ln in qtree_npu.splitlines()
                 if "r100-hdma" in ln
                 or "r100-cm7" in ln
                 or ("chardev =" in ln and "hdma" in ln)]
    if "r100-hdma" not in qtree_npu:
        raise RuntimeError(
            "NPU-side r100-hdma device not found in info qtree; "
            "machine wiring missing TYPE_R100_HDMA instance. "
            "See %s" % npu_qtree_log)
    if not _has_device_with_nonempty_prop(qtree_npu, "r100-hdma",
                                          "chardev"):
        raise RuntimeError(
            "NPU-side r100-hdma 'chardev' property is unset; "
            "the -machine r100-soc,hdma=<id> option didn't latch on "
            "the new device. See %s" % npu_qtree_log)
    npu_snippet = "\n".join(npu_lines) or "(no hdma lines)"

    qtree_host = ""
    deadline = time.time() + poll_timeout
    while time.time() < deadline:
        qtree_host = _hmp_query(host_monitor_sock, "info qtree", timeout=10.0)
        lines = qtree_host.splitlines()
        if _has_nonempty(lines, "hdma"):
            break
        time.sleep(0.3)
    host_qtree_log.write_text(qtree_host + "\n")

    host_lines = [ln.rstrip() for ln in qtree_host.splitlines()
                  if ln.strip().startswith("hdma ")
                  or ln.strip().startswith("hdma-debug ")]
    if not _has_nonempty(host_lines, "hdma"):
        raise RuntimeError(
            "host-side r100-npu-pci 'hdma' property is unset; "
            "the -device r100-npu-pci,hdma=<id> option didn't latch. "
            "See %s" % host_qtree_log)
    host_snippet = "\n".join(host_lines) or "(no hdma lines)"

    return npu_snippet + "\n" + host_snippet


def _verify_npu_shared_mapping(npu_monitor_sock, mtree_log):
    """Prove the shared memory-backend-file is spliced over chiplet 0
    DRAM on the NPU side (M5).

    `info mtree` on the NPU QEMU monitor must list the `r100.chiplet0
    .dram` container with the `remushm` backend at offset 0. Unlike the
    host-side BAR0 wiring, chiplet 0 DRAM is mounted at machine init
    time (no BAR programming to wait for), so this is a single-shot
    query with a modest timeout.

    Returns the mtree snippet for the container. Raises RuntimeError if
    the expected layout is missing."""
    mtree = _hmp_query(npu_monitor_sock, "info mtree", timeout=10.0)
    mtree_log.write_text(mtree + "\n")

    snippet = _extract_mtree_container(mtree, "r100.chiplet0.dram")
    if snippet is None:
        raise RuntimeError(
            "NPU `info mtree` does not list r100.chiplet0.dram; see %s"
            % mtree_log)
    if "remushm" not in snippet:
        raise RuntimeError(
            "chiplet 0 DRAM container is not backed by the 'remushm' "
            "backend; see %s" % mtree_log)
    return snippet


@cli.command()
@click.option("--name", "-n", default=None,
              help="Run name. Outputs land in <output-root>/<name>/. "
                   "Default: run-<YYYYmmdd-HHMMSS>.")
@click.option("--output-root", type=click.Path(file_okay=False), default=None,
              help="Parent directory for run outputs. "
                   "Default: <repo>/output/.")
@click.option("--gdb", is_flag=True, help="Wait for GDB connection on :1234.")
@click.option("--trace", is_flag=True,
              help="Enable guest_errors and unimp logging.")
@click.option("--chiplets", type=int, default=4,
              help="Number of chiplets (1-4). Default 4.")
@click.option("--memory", "-m", default="1G",
              help="DRAM size per chiplet (e.g. 1G, 512M).")
@click.option("--host", "with_host", is_flag=True,
              help="Phase 2: also launch the x86_64 host-side QEMU and "
                   "wire a shared-memory bridge between the two.")
@click.option("--host-mem", default=HOST_MEM_DEFAULT, show_default=True,
              help="Guest RAM for the x86 host-side QEMU (only with --host).")
@click.option("--shm-size", type=int, default=SHM_SIZE_DEFAULT,
              help="Bytes for the shared-memory backing file "
                   "(default: 128 MB). M4+ will alias this into BAR0.")
@click.option("--guest-kernel", type=click.Path(dir_okay=False), default=None,
              help="Path to x86 guest bzImage. Default: "
                   "images/x86_guest/bzImage if present.")
@click.option("--guest-initrd", type=click.Path(dir_okay=False), default=None,
              help="Path to x86 guest initramfs. Default: "
                   "images/x86_guest/initramfs.cpio.gz if present.")
@click.option("--guest-share", type=click.Path(file_okay=False), default=None,
              help="Host directory to expose over virtio-9p (tag 'remu'). "
                   "Default: guest/ at the repo root.")
@click.option("--no-guest-boot", is_flag=True,
              help="Force SeaBIOS-only boot on the x86 side even if "
                   "images/x86_guest/{bzImage,initramfs.cpio.gz} exist.")
@click.option("--guest-cmdline-extra", default=None,
              help="Extra tokens to append to the guest kernel cmdline "
                   "(e.g. `debug loglevel=7`).")
def run(name, output_root, gdb, trace, chiplets, memory,
        with_host, host_mem, shm_size,
        guest_kernel, guest_initrd, guest_share, no_guest_boot,
        guest_cmdline_extra):
    """Boot R100 firmware on the emulated SoC.

    Every invocation gets a dedicated output directory under
    <repo>/output/<name>/ (or a timestamped default) containing:

    \b
      uart0.log       — chiplet 0 UART (also muxed to stdio + monitor)
      uart1..3.log    — chiplets 1-3 UARTs
      hils.log        — FreeRTOS HILS ring tail (RLOG_*/FLOG_* from DRAM)
      cmdline.txt     — the full QEMU command used for this run

    With --host (Phase 2), a second QEMU process (x86_64) is launched
    alongside, with the r100-npu-pci endpoint (0x1eff:0x2030) attached.
    The same /dev/shm-backed memory segment is spliced into BAR0 of
    that PCI device (M4) *and* over chiplet 0's DRAM on the NPU side
    (M5), so stores from either side land in the same tmpfs pages.
    SeaBIOS on the x86 side runs through BAR programming and idles at
    "No bootable device"; both QEMU monitors are exposed on unix
    sockets. Additional output files:

    \b
      shm               symlink → /dev/shm/remu-<name>/
      npu/monitor.sock  NPU HMP monitor (unix socket)
      npu/info-mtree.log captured `info mtree` on the NPU (chiplet0.dram splice)
      npu/info-mtree-imsix.log `info mtree` snippet showing r100-imsix (M7)
      host/cmdline.txt  the full x86 QEMU command
      host/qemu.*.log   x86 QEMU stdout / stderr
      host/serial.log   x86 guest serial
      host/monitor.sock host HMP monitor (unix socket)
      host/info-pci.log captured `info pci` HMP output (auto-verified)
      host/info-mtree.log captured `info mtree` on the host (BAR0 splice)
      host/info-mtree-bar5.log `info mtree` snippet showing BAR5 msix-table (M7)
      host/doorbell.sock M6 NPU→host-visible doorbell socket
      host/msix.sock     M7 NPU→host MSI-X reverse-direction socket
      host/issr.sock     M8 NPU→host ISSR shadow-egress socket
      host/hdma.sock     M8b 3b NPU↔host HDMA executor socket
      doorbell.log       ASCII tail of doorbell frames received by NPU (M6)
      msix.log           ASCII tail of MSI-X frames emitted by NPU (M7)
      issr.log           ASCII tail of ISSR frames emitted by NPU (M8)
      hdma.log           ASCII tail of hdma frames (both directions, M8b 3b/3c)
      rbdma.log          ASCII tail of chiplet-0 r100-rbdma kicks / completes
      smmu.log           ASCII tail of chiplet-0 r100-smmu translates / faults
                         (pairs with tests/scripts/{mem_dump,smmu_decode}.py)
      shm/cfg-shadow     P10-fix shared 4 KB cfg-head backend (host BAR2
                         cfg-head trap aliases this; NPU r100-cm7 cfg-mirror
                         trap aliases the same file — kmd writes are visible
                         to q-cp with no chardev round trip)

    `<output-root>/latest` is updated to point at the most recent run.
    """
    _check_qemu_bin("aarch64")
    if with_host:
        _check_qemu_bin("x86_64")

    # Resolve the run name BEFORE creating the output dir so the cleanup
    # sweep narrows to exactly this run's prior state (orphan QEMUs from
    # a SIGKILL'd previous invocation, the stale /dev/shm backing file,
    # and *.sock files that would block a fresh socket bind). Runs with
    # different --name values stay untouched.
    if not name:
        name = "run-" + time.strftime("%Y%m%d-%H%M%S")
    click.echo("Preparing run '%s' (clearing prior state)..." % name)
    _cleanup_run_trash(name, output_root=output_root, verbose=True)

    run_dir, run_name = _make_run_dir(name, output_root)
    click.echo("Run directory: %s" % run_dir)

    host_proc = None
    shm_dir = None
    npu_monitor_sock = None
    doorbell_sock = None
    doorbell_log = None
    msix_sock = None
    msix_log = None
    issr_sock = None
    issr_log = None
    hdma_sock = None
    hdma_log = None
    rbdma_log = None
    smmu_log = None
    if with_host:
        npu_dir = run_dir / "npu"
        npu_dir.mkdir(exist_ok=True)
        npu_monitor_sock = npu_dir / "monitor.sock"
        # M6 doorbell, M7 iMSIX, M8 ISSR shadow, and the M9-1c hdma
        # executor all use the same host-as-server + NPU-as-client
        # pattern; sockets live under host/ so they survive alongside
        # the other host-owned artifacts. ASCII debug tails sit at run
        # root so users can tail them without guessing which side
        # "owns" them. Stale sock paths would make the host bind
        # EADDRINUSE; clean them all before launch. The legacy `cfg`
        # chardev was retired in P10-fix — cfg-head propagation flows
        # through the shared `cfg-shadow` memory-backend-file alias.
        host_dir_early = run_dir / "host"
        host_dir_early.mkdir(exist_ok=True)
        doorbell_sock = host_dir_early / "doorbell.sock"
        msix_sock = host_dir_early / "msix.sock"
        issr_sock = host_dir_early / "issr.sock"
        hdma_sock = host_dir_early / "hdma.sock"
        for p in (doorbell_sock, msix_sock, issr_sock, hdma_sock):
            if p.exists():
                try:
                    p.unlink()
                except OSError:
                    pass
        doorbell_log = run_dir / "doorbell.log"
        msix_log = run_dir / "msix.log"
        issr_log = run_dir / "issr.log"
        hdma_log = run_dir / "hdma.log"
        # rbdma.log is a P10 post-mortem aid (cb lifecycle, MSI-X
        # fan-out checking) so it's gated on --host like the other
        # ASCII tails. Phase 1 NPU-only smoke runs don't need it —
        # qemu_log_mask(LOG_TRACE, ...) lines + --trace cover the
        # standalone-NPU debug surface.
        rbdma_log = run_dir / "rbdma.log"
        # smmu.log is the P11 / P10 SMMU post-mortem aid — every
        # translate / STE decode / PT-walk dispatch / CMDQ op /
        # eventq emit / GERROR raise on chiplet 0 lands here as one
        # ASCII line. Pairs with `tests/scripts/mem_dump.py` +
        # `tests/scripts/smmu_decode.py` for offline decode of the
        # actual STE / PT bytes from the shm splice.
        smmu_log = run_dir / "smmu.log"

    npu_cmd, found = _build_npu_cmd(run_dir, gdb, trace,
                                    with_host=with_host,
                                    npu_monitor_sock=npu_monitor_sock,
                                    doorbell_sock=doorbell_sock,
                                    doorbell_log=doorbell_log,
                                    msix_sock=msix_sock,
                                    msix_log=msix_log,
                                    issr_sock=issr_sock,
                                    issr_log=issr_log,
                                    hdma_sock=hdma_sock,
                                    hdma_log=hdma_log,
                                    rbdma_log=rbdma_log,
                                    smmu_log=smmu_log)

    if found == 0:
        click.secho("Warning: no firmware images in %s/" % IMAGES_DIR,
                     fg="yellow")
        click.echo("Place bl1.bin, bl31_cp0.bin, freertos_cp0.bin, etc.")
        if not click.confirm("Run with empty memory anyway?"):
            return

    if with_host:
        shm_dir, shm_file = _setup_shm(run_name, shm_size)
        shm_link = run_dir / "shm"
        try:
            if shm_link.is_symlink() or shm_link.exists():
                shm_link.unlink()
            shm_link.symlink_to(shm_dir)
        except OSError:
            pass

        # P10-fix: a second shm file backs the entire x86 guest's
        # main RAM. Same /dev/shm directory as remu-shm so cleanup
        # (`clean --name`) sweeps both with one rmtree. Size is derived
        # from `--host-mem` so the QEMU-side `-m N -object
        # memory-backend-file,size=N` invariant holds.
        host_ram_size = _parse_qemu_size(host_mem)
        _, host_ram_file = _setup_shm(run_name, host_ram_size,
                                      fname="host-ram")

        # P10-fix: third shm file backs the 4 KB cfg-shadow shared
        # between the host's BAR2 cfg-head trap and the NPU's
        # cfg-mirror trap. Both QEMUs map this file as their backing
        # store for cfg_mmio_regs[] / cfg_shadow[], so kmd writes to
        # FUNC_SCRATCH / DDH_BASE_LO are immediately visible to q-cp's
        # next read — eliminates the cfg/doorbell ordering race that
        # surfaced once the iATU stopped serialising on `hdma`.
        _, cfg_shadow_file = _setup_shm(run_name, CFG_SHADOW_SIZE,
                                        fname="cfg-shadow")

        # Attach all three backends to the NPU QEMU:
        #  - remushm:    M4/M5 BAR0 ↔ chiplet-0 DRAM splice (128 MB head).
        #  - hostram:    P10-fix outbound iATU alias (the kmd's coherent
        #                DMA pages live here; r100-pcie-outbound aliases
        #                offset 0 of this MR over
        #                R100_PCIE_AXI_SLV_BASE_ADDR so q-cp's PF-window
        #                loads/stores resolve as plain RAM, no chardev).
        #  - cfgshadow: P10-fix cfg-mirror alias (4 KB; both BAR2 cfg-
        #                head trap and r100-cm7 cfg-mirror trap alias
        #                offset 0).
        # The matching `-machine r100-soc,memdev=remushm,
        # host-ram=hostram,cfg-shadow=cfgshadow` flags are emitted by
        # _build_npu_cmd above.
        npu_cmd += [
            "-object",
            "memory-backend-file,id=remushm,mem-path=%s,size=%d,share=on"
            % (shm_file, shm_size),
            "-object",
            "memory-backend-file,id=hostram,mem-path=%s,size=%d,share=on"
            % (host_ram_file, host_ram_size),
            "-object",
            "memory-backend-file,id=cfgshadow,mem-path=%s,size=%d,share=on"
            % (cfg_shadow_file, CFG_SHADOW_SIZE),
        ]
        click.echo("  Shared memory -> %s (%d MB)"
                   % (shm_file, shm_size // (1024 * 1024)))
        click.echo("  Host RAM shm  -> %s (%d MB, x86 guest main memory)"
                   % (host_ram_file, host_ram_size // (1024 * 1024)))
        click.echo("  Cfg shadow shm-> %s (%d B)"
                   % (cfg_shadow_file, CFG_SHADOW_SIZE))
        click.echo("  NPU QEMU    -> monitor: %s" % npu_monitor_sock)

        host_dir = run_dir / "host"
        host_dir.mkdir(exist_ok=True)
        monitor_sock = host_dir / "monitor.sock"

        # Resolve the x86 guest boot artifacts. Explicit CLI paths win;
        # otherwise auto-detect under images/x86_guest/. --no-guest-boot
        # preserves the M3-era SeaBIOS idle mode for regression runs.
        kernel_path = None
        initrd_path = None
        share_path = None
        if not no_guest_boot:
            if guest_kernel is not None:
                kernel_path = Path(guest_kernel).resolve()
            elif X86_GUEST_KERNEL.exists():
                kernel_path = X86_GUEST_KERNEL
            if kernel_path is not None and not kernel_path.exists():
                click.secho(
                    "Warning: guest kernel %s not found — falling back "
                    "to SeaBIOS idle boot" % kernel_path, fg="yellow")
                kernel_path = None
            if guest_initrd is not None:
                initrd_path = Path(guest_initrd).resolve()
            elif kernel_path is not None and X86_GUEST_INITRD.exists():
                initrd_path = X86_GUEST_INITRD
            if guest_share is not None:
                share_path = Path(guest_share).resolve()
            elif kernel_path is not None and X86_GUEST_SHARE.is_dir():
                share_path = X86_GUEST_SHARE
        host_cmd, host_stdout, host_stderr = _build_host_cmd(
            host_dir, shm_file, shm_size, monitor_sock, host_mem,
            doorbell_sock=doorbell_sock,
            msix_sock=msix_sock,
            msix_log=msix_log,
            issr_sock=issr_sock,
            issr_log=issr_log,
            hdma_sock=hdma_sock,
            hdma_log=hdma_log,
            host_ram_file=host_ram_file,
            host_ram_size=host_ram_size,
            cfg_shadow_file=cfg_shadow_file,
            kernel=kernel_path,
            initrd=initrd_path,
            share_dir=share_path,
            cmdline_extra=guest_cmdline_extra)
        (host_dir / "cmdline.txt").write_text(
            " \\\n  ".join(host_cmd) + "\n")
        if kernel_path is not None:
            click.echo("  Host QEMU   -> Linux guest (monitor: %s)" % monitor_sock)
            click.echo("    kernel    -> %s" % kernel_path)
            if initrd_path is not None:
                click.echo("    initrd    -> %s" % initrd_path)
            if share_path is not None:
                click.echo("    9p share  -> %s (tag: remu)" % share_path)
        else:
            click.echo("  Host QEMU   -> SeaBIOS idle (monitor: %s)" % monitor_sock)
        click.echo("  Doorbell    -> %s (debug tail: %s)"
                   % (doorbell_sock, doorbell_log))
        click.echo("  MSI-X       -> %s (debug tail: %s)"
                   % (msix_sock, msix_log))
        click.echo("  ISSR        -> %s (debug tail: %s)"
                   % (issr_sock, issr_log))
        click.echo("  HDMA        -> %s (debug tail: %s)"
                   % (hdma_sock, hdma_log))

    (run_dir / "cmdline.txt").write_text(" \\\n  ".join(npu_cmd) + "\n")

    click.echo()
    click.echo("Starting R100 emulator (%d chiplets)..." % chiplets)
    click.echo("  Machine: r100-soc (4 chiplets, 32 CA73 cores)")
    click.echo("  Press Ctrl-A X to quit")
    click.echo()

    if not with_host:
        # Single-QEMU mode: exec so Python gets out of the way and the
        # NPU QEMU owns stdio directly (unchanged Phase 1 behavior).
        try:
            os.execvp(npu_cmd[0], npu_cmd)
        except OSError as e:
            click.secho("Failed to start QEMU: %s" % e, fg="red")
            raise SystemExit(1)

    # ── Dual-QEMU mode (Phase 2) ───────────────────────────────────────
    # 1. Spawn host QEMU detached (own session, stdio to files).
    # 2. Wait for its HMP monitor to come up, query `info pci`,
    #    auto-verify the r100-npu-pci device is bound.
    # 3. Spawn NPU QEMU in the foreground, inheriting the terminal
    #    (same UX as single-QEMU mode).
    # 4. When NPU exits — cleanly OR via signal OR SIGPIPE from a
    #    piped head/tee — tear down host QEMU and wipe the /dev/shm
    #    backing directory. SIGTERM is converted to SystemExit so
    #    try/finally below fires; atexit is kept as a last resort.
    import atexit

    with open(host_stdout, "wb") as _so, open(host_stderr, "wb") as _se:
        host_proc = subprocess.Popen(
            host_cmd, stdout=_so, stderr=_se, stdin=subprocess.DEVNULL,
            start_new_session=True,
        )

    npu_proc = None

    def _cleanup():
        if npu_proc and npu_proc.poll() is None:
            try:
                npu_proc.terminate()
                npu_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                npu_proc.kill()
        if host_proc and host_proc.poll() is None:
            try:
                host_proc.terminate()
                host_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                host_proc.kill()
        if shm_dir and shm_dir.exists():
            try:
                shutil.rmtree(shm_dir)
            except OSError:
                pass
        if doorbell_sock and doorbell_sock.exists():
            try:
                doorbell_sock.unlink()
            except OSError:
                pass
        if msix_sock and msix_sock.exists():
            try:
                msix_sock.unlink()
            except OSError:
                pass
        if issr_sock and issr_sock.exists():
            try:
                issr_sock.unlink()
            except OSError:
                pass
        if hdma_sock and hdma_sock.exists():
            try:
                hdma_sock.unlink()
            except OSError:
                pass

    atexit.register(_cleanup)
    # Convert signal-driven deaths (timeout, kill, parent exit,
    # broken pipe through tee/head) into SystemExit so the finally
    # below runs normal cleanup instead of leaving orphans.
    def _sig_exit(signum, _frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, _sig_exit)
    signal.signal(signal.SIGHUP, _sig_exit)
    signal.signal(signal.SIGPIPE, _sig_exit)

    try:
        click.echo("Verifying host-side r100-npu-pci...")
        try:
            info = _verify_host_npu_pci(
                host_dir / "monitor.sock", host_dir / "info-pci.log")
        except Exception as e:
            click.secho("Host-side verification failed: %s" % e, fg="red")
            if host_stderr.exists():
                err = host_stderr.read_text()[-2000:]
                if err:
                    click.echo("--- host QEMU stderr (tail) ---")
                    click.echo(err)
            raise SystemExit(1)

        click.secho(
            "  r100-npu-pci bound on host (see %s)" %
            (host_dir / "info-pci.log"), fg="green")
        # Print only the r100-npu-pci block so the common case is a few
        # lines of proof, not a full `info pci` dump. `info pci`
        # groups entries under `  Bus N, device M, function F:`
        # headers with no blank line between groups; split on that.
        import re as _re
        groups = _re.split(r"(?=^  Bus\s)", info, flags=_re.MULTILINE)
        needle = "%s:%s" % (R100_PCI_VENDOR, R100_PCI_DEVICE)
        for grp in groups:
            if needle in grp.lower():
                for ln in grp.rstrip().splitlines():
                    click.echo("    " + ln)
                break
        click.echo()

        npu_stderr_log = npu_dir / "qemu.stderr.log"
        _npu_se = open(npu_stderr_log, "wb")
        npu_proc = subprocess.Popen(npu_cmd, stderr=_npu_se)

        # Wait for both QEMUs to actually mmap the shared file. QEMU
        # does this very early (before CPU start), so a short poll is
        # sufficient; bail noisily if it doesn't happen in a second or
        # two so the failure mode isn't a silent "looks-like-M3-ran".
        click.echo("Verifying shared-memory bridge...")
        ok = False
        for _ in range(30):
            if (_proc_has_mmap(host_proc.pid, shm_file)
                    and _proc_has_mmap(npu_proc.pid, shm_file)):
                ok = True
                break
            if npu_proc.poll() is not None:
                break
            time.sleep(0.1)
        if not ok:
            click.secho(
                "  Shared file %s not mmap'd by both QEMUs yet" % shm_file,
                fg="yellow")

        try:
            snippet = _verify_shared_mapping(
                host_dir / "monitor.sock", shm_file,
                host_proc.pid, npu_proc.pid,
                host_dir / "info-mtree.log")
            click.secho(
                "  memdev aliased over BAR0 + both QEMUs mmap %s"
                % shm_file, fg="green")
            for ln in snippet.splitlines():
                click.echo("    " + ln)
            click.echo()
        except Exception as e:
            click.secho("  %s" % e, fg="red")
            # Don't kill the run — follow-up M-work is orthogonal to
            # this verification. Just surface the failure and keep going.

        # M5: Verify the NPU side also splices remushm over chiplet 0
        # DRAM at offset 0. Unlike BAR0 this is a machine-time mount so
        # it's present immediately; _hmp_query's built-in connect
        # retries handle the small race with NPU-QEMU monitor bring-up.
        try:
            npu_snippet = _verify_npu_shared_mapping(
                npu_monitor_sock, npu_dir / "info-mtree.log")
            click.secho(
                "  memdev aliased over chiplet 0 DRAM (NPU side)",
                fg="green")
            for ln in npu_snippet.splitlines():
                click.echo("    " + ln)
            click.echo()
        except Exception as e:
            click.secho("  %s" % e, fg="red")

        # M6: Verify the doorbell plumbing is wired on both sides.
        try:
            db_snip = _verify_doorbell_wired(
                host_dir / "monitor.sock", npu_monitor_sock,
                host_dir / "info-mtree-bar4.log",
                npu_dir / "info-qtree.log",
            )
            click.secho(
                "  doorbell chardev wired: BAR4 MMIO overlay on host + "
                "r100-cm7 → r100-mailbox on NPU", fg="green")
            for ln in db_snip.splitlines():
                click.echo("    " + ln)
            click.echo()
        except Exception as e:
            click.secho("  %s" % e, fg="red")

        # M7: Verify the MSI-X reverse-direction plumbing.
        try:
            msix_snip = _verify_msix_wired(
                host_dir / "monitor.sock", npu_monitor_sock,
                host_dir / "info-mtree-bar5.log",
                npu_dir / "info-mtree-imsix.log",
            )
            click.secho(
                "  msix chardev wired: r100-imsix on NPU + "
                "msix-table overlay on host BAR5", fg="green")
            for ln in msix_snip.splitlines():
                click.echo("    " + ln)
            click.echo()
        except Exception as e:
            click.secho("  %s" % e, fg="red")

        # M8: Verify the ISSR shadow-egress plumbing is wired on both
        # sides. Failure is non-fatal — the NPU still boots and the
        # other bridges keep working — but surfacing it early saves a
        # debugging round-trip when the chardev id gets mangled.
        try:
            issr_snip = _verify_issr_wired(
                host_dir / "monitor.sock", npu_monitor_sock,
                host_dir / "info-qtree-issr.log",
                npu_dir / "info-qtree-issr.log",
            )
            click.secho(
                "  issr chardev wired: r100-mailbox egress on NPU + "
                "r100-npu-pci ingress on host", fg="green")
            for ln in issr_snip.splitlines():
                click.echo("    " + ln)
            click.echo()
        except Exception as e:
            click.secho("  %s" % e, fg="red")

        # Verify the cfg (host→NPU) + hdma (NPU<->host) pair so q-cp's
        # P1a outbound iATU + P1b cfg-mirror trap have every wire they
        # need. Same non-fatal policy as issr.
        try:
            ch_snip = _verify_cfg_hdma_wired(
                host_dir / "monitor.sock", npu_monitor_sock,
                host_dir / "info-qtree-cfg-hdma.log",
                npu_dir / "info-qtree-cfg-hdma.log",
            )
            click.secho(
                "  cfg+hdma chardevs wired: r100-cm7 cfg / r100-hdma "
                "chardev on NPU + r100-npu-pci cfg/hdma on host",
                fg="green")
            for ln in ch_snip.splitlines():
                click.echo("    " + ln)
            click.echo()
        except Exception as e:
            click.secho("  %s" % e, fg="red")

        try:
            rc = npu_proc.wait()
        except KeyboardInterrupt:
            npu_proc.terminate()
            rc = npu_proc.wait()
        raise SystemExit(rc)
    finally:
        _cleanup()


# ── gdb ──────────────────────────────────────────────────────────────────────

@cli.command()
@click.option("--port", "-p", type=int, default=1234,
              help="GDB server port. Default 1234.")
@click.option("--binary", "-b", type=click.Path(exists=True), default=None,
              help="ELF binary with debug symbols to load.")
def gdb(port, binary):
    """Attach GDB to a running QEMU instance."""
    gdb_cmd = shutil.which("gdb-multiarch") or shutil.which("aarch64-linux-gnu-gdb")
    if not gdb_cmd:
        click.secho("gdb-multiarch not found. Install with: "
                     "sudo apt install gdb-multiarch", fg="red")
        raise SystemExit(1)

    cmd = [gdb_cmd]
    if binary:
        cmd.append(binary)
    cmd += ["-ex", "target remote :%d" % port]

    click.echo("Connecting to QEMU GDB server on :%d..." % port)
    os.execvp(cmd[0], cmd)


# ── status ───────────────────────────────────────────────────────────────────

@cli.command()
def status():
    """Show emulator environment status."""
    click.echo("REMU — R100 NPU System Emulator")
    click.echo()

    # QEMU source
    if QEMU_SRC.is_dir():
        click.secho("  QEMU source:  %s" % QEMU_SRC, fg="green")
    else:
        click.secho("  QEMU source:  NOT FOUND", fg="red")

    # QEMU binaries (aarch64 hosts the NPU; x86_64 hosts the Phase 2 guest)
    if QEMU_BIN.is_file():
        click.secho("  QEMU aarch64: %s" % QEMU_BIN, fg="green")
    else:
        click.secho("  QEMU aarch64: NOT BUILT (run: ./remucli build)",
                    fg="yellow")
    if QEMU_BIN_X86.is_file():
        click.secho("  QEMU x86_64:  %s" % QEMU_BIN_X86, fg="green")
    else:
        click.secho("  QEMU x86_64:  NOT BUILT (run: ./remucli build)",
                    fg="yellow")

    # Firmware images
    click.echo()
    click.echo("  Firmware images (%s/):" % IMAGES_DIR)
    any_found = False
    for fname, addr, desc in FW_IMAGES:
        path = IMAGES_DIR / fname
        if path.is_file():
            size = path.stat().st_size
            click.secho("    [OK] %-22s %8d bytes  @ 0x%010x  %s" %
                         (fname, size, addr, desc), fg="green")
            any_found = True
        else:
            click.secho("    [--] %-22s                @ 0x%010x  %s" %
                         (fname, addr, desc), fg="white", dim=True)

    if not any_found:
        click.echo()
        click.secho("  No firmware images found. Run: "
                     "./remucli fw-build", fg="yellow")

    # Device models
    click.echo()
    click.echo("  Device models (src/machine/):")
    models = sorted(MACHINE_SRC.glob("r100_*.c"))
    for m in models:
        name = m.stem.replace("r100_", "")
        click.echo("    %s" % name)

    # Symlink status
    click.echo()
    r100_link = QEMU_SRC / "hw" / "arm" / "r100"
    if r100_link.exists():
        click.secho("  QEMU integration:  linked", fg="green")
    else:
        click.secho("  QEMU integration:  not linked (run: ./remucli build)",
                     fg="yellow")


# ── images ───────────────────────────────────────────────────────────────────

@cli.command()
@click.option("--check", is_flag=True,
              help="Check which images are present.")
@click.option("--from-dir", type=click.Path(exists=True), default=None,
              help="Copy firmware images from a build output directory.")
def images(check, from_dir):
    """Manage firmware images for the emulator."""
    if from_dir:
        src = Path(from_dir)
        IMAGES_DIR.mkdir(parents=True, exist_ok=True)

        click.echo("Copying firmware images from %s..." % src)
        # Try common FW build output patterns
        patterns = {
            "bl1.bin": ["bl1.bin", "FreeRTOS_CP/bl1.bin"],
            "bl31_cp0.bin": ["bl31.bin", "FreeRTOS_CP/bl31.bin"],
            "freertos_cp0.bin": [
                "FreeRTOS_CP.bin",
                "FreeRTOS_CP/FreeRTOS_CP.bin",
            ],
        }
        for target, candidates in patterns.items():
            for cand in candidates:
                cand_path = src / cand
                if cand_path.is_file():
                    dest = IMAGES_DIR / target
                    shutil.copy2(cand_path, dest)
                    click.secho("  %s -> %s" % (cand_path, dest), fg="green")
                    break

        click.echo("Done. Run './remucli images --check' to verify.")
        return

    # Default: show image status (same as --check)
    IMAGES_DIR.mkdir(parents=True, exist_ok=True)
    click.echo("Firmware images in %s/:" % IMAGES_DIR)
    click.echo()
    for fname, addr, desc in FW_IMAGES:
        path = IMAGES_DIR / fname
        if path.is_file():
            size = path.stat().st_size
            click.secho("  [OK] %-22s %8d bytes  @ 0x%010x" %
                         (fname, size, addr), fg="green")
        else:
            click.secho("  [--] %-22s  missing         @ 0x%010x" %
                         (fname, addr), fg="red")
    click.echo()
    click.echo("Copy images manually or use: ./remucli images --from-dir <path>")


# ── clean ────────────────────────────────────────────────────────────────────

@cli.command()
@click.option("--name", "-n", default=None,
              help="Clean one named run's leftovers (procs, shm, sockets).")
@click.option("--all", "all_", is_flag=True,
              help="Clean every /dev/shm/remu-* directory and any QEMU "
                   "process still mapping one of them.")
@click.option("--output-root", type=click.Path(file_okay=False), default=None,
              help="Parent directory for run outputs. Default: <repo>/output/.")
def clean(name, all_, output_root):
    """Tear down orphan state from a prior run.

    Sweeps three things for the matching run name(s):

    \b
      - QEMU processes mentioning /dev/shm/remu-<name>/ or output/<name>/
        in their cmdline (SIGTERM, then SIGKILL after 3 s).
      - /dev/shm/remu-<name>/ (the tmpfs backing directory).
      - *.sock files under output/<name>/ that would block a re-bind.

    `./remucli run --name <n>` auto-invokes this at start, so manual
    cleanup is only needed after SIGKILL-style crashes or before reusing
    a run name across sessions. `--all` is the "something is broken,
    nuke everything REMU-shaped" escape hatch.
    """
    if not name and not all_:
        raise click.UsageError(
            "Pass --name NAME to clean one run, or --all for a full sweep.")

    root = Path(output_root).resolve() if output_root else OUTPUT_ROOT
    totals = {"procs": 0, "shm": 0, "socks": 0}

    if all_:
        shm_dirs = sorted(SHM_ROOT.glob("remu-*"))
        out_dirs = sorted([p for p in root.glob("*")
                           if p.is_dir() and p.name != "latest"]) \
            if root.exists() else []
        names = set()
        for d in shm_dirs:
            names.add(d.name[len("remu-"):])
        for d in out_dirs:
            names.add(d.name)
        if not names:
            click.echo("Nothing to clean.")
            return
        for n in sorted(names):
            click.echo("• %s" % n)
            counts = _cleanup_run_trash(n, output_root=root, verbose=True)
            for k in totals:
                totals[k] += counts[k]
    else:
        counts = _cleanup_run_trash(name, output_root=root, verbose=True)
        for k in totals:
            totals[k] += counts[k]

    click.secho(
        "Cleanup complete: killed %d proc(s), removed %d shm dir(s), "
        "%d socket(s)." % (totals["procs"], totals["shm"], totals["socks"]),
        fg="green")


# ── test ─────────────────────────────────────────────────────────────────────

# Registry of end-to-end bridge tests. `run_name` must match the RUN_NAME
# the test script hardcodes so pre-run cleanup narrows to the same paths
# the test will touch on launch.
TEST_REGISTRY = {
    "m5": {
        "script": "tests/m5_dataflow_test.py",
        "run_name": "m5-flow",
        "desc": "M5 data-flow: host x86 BAR0 ↔ NPU chiplet-0 DRAM coherency",
        "needs": ("aarch64", "x86_64"),
    },
    "m6": {
        "script": "tests/m6_doorbell_test.py",
        "run_name": "m6-doorbell",
        "desc": "M6 doorbell: guest → NPU INTGR + bogus frame rejection",
        "needs": ("aarch64",),
    },
    "m7": {
        "script": "tests/m7_msix_test.py",
        "run_name": "m7-msix",
        "desc": "M7 iMSIX: NPU → host MSI-X + oor/bad-offset rejection",
        "needs": ("x86_64",),
    },
    "m8": {
        "script": "tests/m8_issr_test.py",
        "run_name": "m8-issr",
        "desc": "M8 ISSR bridge: NPU↔host both-direction shadow",
        "needs": ("aarch64", "x86_64"),
    },
    "p4a": {
        "script": "tests/p4a_rbdma_stub_test.py",
        "run_name": "p4a-rbdma",
        "desc": "P4A r100-rbdma stub: IP_INFO seeds + queue-credit + idle FIFO",
        "needs": ("aarch64", "x86_64"),
    },
    "p4b": {
        "script": "tests/p4b_rbdma_oto_test.py",
        "run_name": "p4b-rbdma",
        "desc": "P4B r100-rbdma OTO: gdbstub-driven 4 KB byte-move via shm splice",
        "needs": ("aarch64", "x86_64"),
    },
    "p5": {
        "script": "tests/p5_hdma_ll_test.py",
        "run_name": "p5-hdma",
        "desc": "P5 r100-hdma LL: gdbstub-driven dw_hdma_v0_lli D2D byte-move",
        "needs": ("aarch64", "x86_64"),
    },
    "p11": {
        "script": "tests/p11_smmu_walk_test.py",
        "run_name": "p11-smmu",
        "desc": ("P11 SMMU stage-2: gdbstub-driven walk + RBDMA OTO via "
                 "IPA → 4 KB byte-move"),
        "needs": ("aarch64", "x86_64"),
    },
    "p11b": {
        "script": "tests/p11b_smmu_evtq_test.py",
        "run_name": "p11b-evtq",
        "desc": ("P11b SMMU eventq + GERROR: deliberate INV_STE fault → "
                 "verify event lands on in-DRAM eventq + PROD bumped"),
        "needs": ("aarch64", "x86_64"),
    },
    "p10": {
        "script": "tests/p10_umd_smoke_test.py",
        "run_name": "p10-umd",
        "desc": ("P10 umd smoke: command_submission integration test "
                 "(rblnSubmitJob → RBDMA OTO → cb_complete → MSI-X) — "
                 "scaffolding only; queue_init handshake debug pending"),
        "needs": ("aarch64", "x86_64"),
        # Excluded from the default `./remucli test` / `./remucli test
        # all` sweep until the queue_init handshake clears (see
        # docs/roadmap.md → P10). Run explicitly with
        # `./remucli test p10` to exercise the build / staging /
        # orchestrator scaffolding once `guest/build-umd.sh` has staged
        # `command_submission`.
        "in_default": False,
    },
}

TEST_KEYS = list(TEST_REGISTRY.keys())
DEFAULT_TEST_KEYS = [k for k, v in TEST_REGISTRY.items()
                     if v.get("in_default", True)]


@cli.command()
@click.argument("tests", nargs=-1,
                type=click.Choice(TEST_KEYS + ["all"]))
@click.option("--skip-clean", is_flag=True,
              help="Don't pre-clean each test's run dir (not recommended).")
@click.option("--stop-on-fail", is_flag=True,
              help="Abort the suite on the first failing test.")
def test(tests, skip_clean, stop_on_fail):
    """Run REMU bridge end-to-end tests with pre-run cleanup.

    By default runs the regression set (m5..m8 + p4a/p4b/p5/p11).  Pass one
    or more test ids to pick specific phases, e.g.:

    \b
        ./remucli test              # full default regression
        ./remucli test m5 m6        # just M5 + M6
        ./remucli test p10          # opt-in test (not in default)
        ./remucli test all p10      # default regression + P10

    Tests flagged `in_default=False` in TEST_REGISTRY (currently just
    P10 — see docs/roadmap.md for the open queue_init handshake) are
    excluded from `all` / bare invocations so a dev `./remucli test`
    still gates regressions cleanly while we land the missing pieces.

    Each test is launched after `./remucli clean --name <its-run>` so
    orphan QEMU processes / stale shm / stale sockets from a prior
    invocation never contaminate the result. Per-test stdout is
    captured under output/test-<id>.log; only a one-line PASS/FAIL
    plus a short tail on failure is printed, so the suite's total
    output stays small enough to paste into a bug report.
    """
    if not tests or "all" in tests:
        # `all` and the bare `./remucli test` form intentionally skip
        # tests flagged `in_default=False` (currently P10 — see
        # TEST_REGISTRY for the rationale). To run an opted-out test
        # name it explicitly: `./remucli test p10`, or
        # `./remucli test all p10` to bolt it onto the regression set.
        selected = list(DEFAULT_TEST_KEYS)
        if "all" in tests:
            for t in tests:
                if t != "all" and t not in selected:
                    selected.append(t)
    else:
        seen = set()
        selected = [t for t in tests if not (t in seen or seen.add(t))]

    needs = set()
    for t in selected:
        needs.update(TEST_REGISTRY[t]["needs"])
    for which in sorted(needs):
        _check_qemu_bin(which)

    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    results = []
    for t in selected:
        info = TEST_REGISTRY[t]
        script = REMU_ROOT / info["script"]
        log = OUTPUT_ROOT / ("test-%s.log" % t)

        click.echo()
        click.secho("[%s] %s" % (t, info["desc"]), fg="cyan")
        if not skip_clean:
            _cleanup_run_trash(info["run_name"], verbose=False)

        if not script.is_file():
            click.secho("  SKIP: %s not found" % script, fg="yellow")
            results.append((t, None))
            continue

        start = time.time()
        with open(log, "wb") as lf:
            rc = subprocess.run(
                [sys.executable, str(script)],
                stdout=lf, stderr=subprocess.STDOUT,
                cwd=REMU_ROOT,
            ).returncode
        dur = time.time() - start

        if rc == 0:
            click.secho("  PASS (%.1fs, log: %s)" % (dur, log), fg="green")
        else:
            click.secho("  FAIL rc=%d (%.1fs, log: %s)" %
                        (rc, dur, log), fg="red")
            tail = log.read_text(errors="replace").splitlines()[-30:]
            for ln in tail:
                click.echo("    " + ln)
        results.append((t, rc))
        if rc and stop_on_fail:
            click.secho("Stopping on first failure (--stop-on-fail).",
                        fg="red")
            break

    click.echo()
    click.echo("Test summary:")
    failed = 0
    skipped = 0
    for t, rc in results:
        if rc is None:
            click.secho("  SKIP  %s" % t, fg="yellow")
            skipped += 1
        elif rc == 0:
            click.secho("  PASS  %s" % t, fg="green")
        else:
            click.secho("  FAIL  %s  (rc=%d)" % (t, rc), fg="red")
            failed += 1
    total = len(results)
    click.echo("  %d/%d passed, %d failed, %d skipped"
               % (total - failed - skipped, total, failed, skipped))
    if failed:
        raise SystemExit(1)


# ── completion ───────────────────────────────────────────────────────────────

@cli.command()
@click.argument("shell", type=click.Choice(["bash", "zsh", "fish"]))
def completion(shell):
    """Print a shell completion script.

    \b
    One-off (current shell):
        eval "$(./remucli completion bash)"

    \b
    Persistent — source from ~/.bashrc (pick one):
        eval "$(/abs/path/to/remucli completion bash)"
        source /abs/path/to/remucli-complete.bash   # if you write it to a file first

    \b
    Zsh and fish use the same pattern:
        eval "$(./remucli completion zsh)"
        ./remucli completion fish | source

    The generated script hooks the command name `remucli`, so either put the
    repo root on PATH or `alias remucli=/abs/path/to/remucli` for bare-name
    invocations to tab-complete.
    """
    from click.shell_completion import get_completion_class
    comp_cls = get_completion_class(shell)
    if comp_cls is None:
        raise click.ClickException("shell '%s' not supported by this click" % shell)
    comp = comp_cls(cli, {}, "remucli", "_REMUCLI_COMPLETE")
    click.echo(comp.source())


# ── entry point ──────────────────────────────────────────────────────────────

def main():
    cli(prog_name="remucli")


if __name__ == "__main__":
    main()
