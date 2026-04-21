#!/usr/bin/env python3
"""
REMU CLI — R100 NPU System Emulator management tool.

Invoked via the ./remucli wrapper at the repo root (no install step).
Dependency: click (`pip install --user click`).

Usage:
    ./remucli build [--clean] [--jobs N]
    ./remucli fw-build [-p silicon] [-c tf-a -c cp0 -c cp1] [--clean]
    ./remucli run [--name NAME] [--gdb] [--trace] [--chiplets N]
    ./remucli gdb [--port PORT] [-b ELF]
    ./remucli status
    ./remucli images [--check | --from-dir PATH]
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
MACHINE_SRC = REMU_ROOT / "src" / "machine"
INCLUDE_SRC = REMU_ROOT / "src" / "include"
IMAGES_DIR = REMU_ROOT / "images"
OUTPUT_ROOT = REMU_ROOT / "output"

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

# R100 chiplet offset (must match R100_CHIPLET_OFFSET in remu_addrmap.h).
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


def _check_qemu_bin():
    if not QEMU_BIN.is_file():
        click.secho("QEMU binary not found. Run './remucli build' first.", fg="red")
        raise SystemExit(1)


def _link_sources():
    """Symlink remu device models into the QEMU source tree."""
    r100_dir = QEMU_SRC / "hw" / "arm" / "r100"
    if not r100_dir.exists():
        r100_dir.symlink_to(MACHINE_SRC)
        click.echo("  Linked %s -> %s" % (r100_dir, MACHINE_SRC))

    include_link = QEMU_SRC / "include" / "hw" / "arm" / "remu_addrmap.h"
    if not include_link.exists():
        include_link.symlink_to(INCLUDE_SRC / "remu_addrmap.h")
        click.echo("  Linked %s" % include_link)

    r100_soc_h_link = QEMU_SRC / "include" / "hw" / "arm" / "r100_soc.h"
    if not r100_soc_h_link.exists():
        r100_soc_h_link.symlink_to(MACHINE_SRC / "r100_soc.h")
        click.echo("  Linked %s" % r100_soc_h_link)


def _patch_meson():
    """Ensure QEMU's hw/arm/meson.build includes our subdir."""
    meson_path = QEMU_SRC / "hw" / "arm" / "meson.build"
    marker = "subdir('r100')"

    content = meson_path.read_text()
    if marker not in content:
        content += "\n%s\n" % marker
        meson_path.write_text(content)
        click.echo("  Patched %s" % meson_path)
        return True
    return False


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
    """Build QEMU with R100 device models."""
    _check_qemu_src()

    if clean:
        click.echo("Cleaning...")
        if QEMU_BUILD.exists():
            shutil.rmtree(QEMU_BUILD)
        r100_link = QEMU_SRC / "hw" / "arm" / "r100"
        if r100_link.is_symlink():
            r100_link.unlink()
        click.secho("Clean complete.", fg="green")
        if not click.confirm("Continue with build?"):
            return

    click.echo("Linking remu sources into QEMU tree...")
    _link_sources()

    click.echo("Patching meson.build...")
    _patch_meson()

    QEMU_BUILD.mkdir(parents=True, exist_ok=True)

    # Configure
    if not (QEMU_BUILD / "build.ninja").exists():
        click.echo("Configuring QEMU (aarch64-softmmu)...")
        _run([
            str(QEMU_SRC / "configure"),
            "--target-list=aarch64-softmmu",
            "--prefix=%s" % (REMU_ROOT / "install"),
            "--enable-debug",
            "--disable-werror",
            "--disable-docs",
            "--disable-guest-agent",
            "--extra-cflags=-I%s" % INCLUDE_SRC,
        ], cwd=QEMU_BUILD)

    # Build
    nj = jobs if jobs > 0 else os.cpu_count()
    click.echo("Building with %d jobs..." % nj)
    _run(["ninja", "-j", str(nj)], cwd=QEMU_BUILD)

    click.secho("\nBuild complete: %s" % QEMU_BIN, fg="green")


# ── fw-build ─────────────────────────────────────────────────────────────────

# q-sys build.sh targets that have real build_* functions. Note: the `rtos`
# alias advertised in build.sh --help has no corresponding build_rtos in
# q-sys scripts/, so we don't expose it here.
FW_COMPONENTS = ["all", "tf-a", "cp0", "cp1", "cm", "boot", "bootrom", "cmrt"]
FW_PLATFORMS = ["silicon", "zebu", "zebu_ci", "zebu_vdk"]
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
              help="Target platform. Default 'silicon'.")
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

    click.echo("q-sys fw-build: targets=%s platform=%s mode=%s chiplets=%d" %
               (",".join(targets), platform, mode, chiplets))
    click.echo("  ARM64: %s" % env["COMPILER_PATH_ARM64"])
    click.echo("  ARM32: %s" % env["COMPILER_PATH_ARM32"])

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
    refresh the output/latest -> <name> convenience symlink."""
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
    return run_dir


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
def run(name, output_root, gdb, trace, chiplets, memory):
    """Boot R100 firmware on the emulated SoC.

    Every invocation gets a dedicated output directory under
    <repo>/output/<name>/ (or a timestamped default) containing:

    \b
      uart0.log       — chiplet 0 UART (also muxed to stdio + monitor)
      uart1..3.log    — chiplets 1-3 UARTs
      hils.log        — FreeRTOS HILS ring tail (RLOG_*/FLOG_* from DRAM)
      cmdline.txt     — the full QEMU command used for this run

    `<output-root>/latest` is updated to point at the most recent run.
    """
    _check_qemu_bin()

    run_dir = _make_run_dir(name, output_root)
    click.echo("Run directory: %s" % run_dir)

    uart0_log = run_dir / "uart0.log"
    hils_log = run_dir / "hils.log"

    # Chiplet 0 UART drives stdio (muxed with the QEMU monitor — Ctrl-A C
    # toggles) and tee's into uart0.log via chardev logfile=. Chiplets
    # 1-3 each get a dedicated file chardev so per-chiplet boot output
    # doesn't interleave. All four -serial bindings are explicit because
    # QEMU's implicit `-serial mon:stdio` is suppressed once any other
    # -serial option is present.
    cmd = [
        str(QEMU_BIN),
        "-M", "r100-soc",
        "-display", "none",
        "-chardev",
        "stdio,id=uart0,mux=on,signal=off,logfile=%s,logappend=off" % uart0_log,
        "-mon", "chardev=uart0,mode=readline",
        "-serial", "chardev:uart0",
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

    # 5th `-serial` slot (serial_hd(4)) is consumed by the in-machine
    # r100-logbuf-tail device. It polls chiplet 0's DRAM .logbuf ring at
    # 0x10000000 and drains RLOG_*/FLOG_* entries out of band so they
    # surface even before FreeRTOS's own terminal_task starts draining.
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

    # CP0 + CP1 images also need to land in each secondary chiplet's DRAM
    # so chiplet N's CP0/CP1.cpu0 (released by BL2) finds its reset vector
    # images. Chiplet 0 is already covered by FW_IMAGES; replicate at
    # chiplet_id * CHIPLET_OFFSET + base for chiplets 1..N-1.
    for chiplet_id in range(1, R100_NUM_CHIPLETS):
        for fname, base, _desc in FW_PER_CHIPLET_IMAGES:
            path = IMAGES_DIR / fname
            if path.is_file():
                addr = chiplet_id * R100_CHIPLET_OFFSET + base
                cmd += ["-device",
                        "loader,file=%s,addr=0x%x" % (path, addr)]
                found += 1

    if found == 0:
        click.secho("Warning: no firmware images in %s/" % IMAGES_DIR,
                     fg="yellow")
        click.echo("Place bl1.bin, bl31_cp0.bin, freertos_cp0.bin, etc.")
        if not click.confirm("Run with empty memory anyway?"):
            return

    (run_dir / "cmdline.txt").write_text(" \\\n  ".join(cmd) + "\n")

    click.echo()
    click.echo("Starting R100 emulator (%d chiplets)..." % chiplets)
    click.echo("  Machine: r100-soc (4 chiplets, 32 CA73 cores)")
    click.echo("  Press Ctrl-A X to quit")
    click.echo()

    try:
        os.execvp(cmd[0], cmd)
    except OSError as e:
        click.secho("Failed to start QEMU: %s" % e, fg="red")
        raise SystemExit(1)


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

    # QEMU binary
    if QEMU_BIN.is_file():
        click.secho("  QEMU binary:  %s" % QEMU_BIN, fg="green")
    else:
        click.secho("  QEMU binary:  NOT BUILT (run: ./remucli build)", fg="yellow")

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
                     "./remucli fw-build -p silicon", fg="yellow")

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
