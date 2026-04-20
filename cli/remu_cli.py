#!/usr/bin/env python3
"""
REMU CLI — R100 NPU System Emulator management tool.

Usage:
    remu build [--clean] [--jobs N]
    remu run [--gdb] [--trace] [--chiplets N]
    remu gdb [--port PORT]
    remu status
    remu images [--list | --check]
"""

import os
import sys
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

# R100 firmware image definitions: (filename, load_address, description)
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


def _check_qemu_src():
    if not QEMU_SRC.is_dir():
        click.secho("QEMU source not found at: %s" % QEMU_SRC, fg="red")
        click.echo("Run:  git clone --depth 1 --branch v9.2.0 "
                    "https://gitlab.com/qemu-project/qemu.git %s" % QEMU_SRC)
        raise SystemExit(1)


def _check_qemu_bin():
    if not QEMU_BIN.is_file():
        click.secho("QEMU binary not found. Run 'remu build' first.", fg="red")
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

@click.group()
@click.version_option("0.1.0", prog_name="remu")
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
# scripts/, so we don't expose it here.
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

@cli.command()
@click.option("--gdb", is_flag=True, help="Wait for GDB connection on :1234.")
@click.option("--trace", is_flag=True,
              help="Enable guest_errors and unimp logging.")
@click.option("--chiplets", type=int, default=4,
              help="Number of chiplets (1-4). Default 4.")
@click.option("--memory", "-m", default="1G",
              help="DRAM size per chiplet (e.g. 1G, 512M).")
@click.option("--uart-log-dir", type=click.Path(file_okay=False),
              default="/tmp",
              help="Directory for per-chiplet UART log files. "
                   "Chiplet 0 uses stdio; 1-3 write to "
                   "remu_uart{1,2,3}.log. Default: /tmp.")
@click.option("--hils-log", type=click.Path(dir_okay=False), default=None,
              help="File to receive the FreeRTOS HILS ring tail "
                   "(RLOG_*/FLOG_* messages from DRAM 0x10000000). "
                   "Default: <uart-log-dir>/remu_hils.log. Pass 'stderr' "
                   "to mux onto the controlling terminal instead.")
def run(gdb, trace, chiplets, memory, uart_log_dir, hils_log):
    """Boot R100 firmware on the emulated SoC."""
    _check_qemu_bin()

    # Per-chiplet UART routing: chiplet 0 goes to stdio so boot output
    # is live; chiplets 1-3 each get a dedicated file chardev to keep
    # logs from interleaving with each other.
    uart_log_dir = Path(uart_log_dir)
    uart_log_dir.mkdir(parents=True, exist_ok=True)

    # Drive chiplet 0's UART onto stdio (muxed with monitor) and the
    # other three onto dedicated file chardevs. We manage all four
    # serial bindings explicitly because QEMU's `-nographic` implicit
    # `-serial mon:stdio` is suppressed once any other `-serial`
    # option is present, which otherwise leaves chiplet 3 unbound.
    cmd = [
        str(QEMU_BIN),
        "-M", "r100-soc",
        "-display", "none",
        "-serial", "mon:stdio",
    ]
    for n in range(1, 4):
        log_path = uart_log_dir / ("remu_uart%d.log" % n)
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
    if hils_log == "stderr":
        cmd += ["-chardev", "stdio,id=hils,mux=off,signal=off",
                "-serial", "chardev:hils"]
        click.echo("  HILS ring tail -> stderr (chardev=stdio)")
    else:
        hils_path = Path(hils_log) if hils_log else (uart_log_dir / "remu_hils.log")
        hils_path.parent.mkdir(parents=True, exist_ok=True)
        cmd += [
            "-chardev",
            "file,id=hils,path=%s,mux=off" % hils_path,
            "-serial", "chardev:hils",
        ]
        click.echo("  HILS ring tail -> %s" % hils_path)

    if gdb:
        cmd += ["-s", "-S"]
        click.secho("Waiting for GDB on localhost:1234...", fg="yellow")

    if trace:
        cmd += ["-d", "guest_errors,unimp"]

    # Load firmware images
    found = 0
    for fname, addr, desc in FW_IMAGES:
        path = IMAGES_DIR / fname
        if path.is_file():
            cmd += ["-device", "loader,file=%s,addr=0x%x" % (path, addr)]
            found += 1

    if found == 0:
        click.secho("Warning: no firmware images in %s/" % IMAGES_DIR,
                     fg="yellow")
        click.echo("Place bl1.bin, bl31_cp0.bin, freertos_cp0.bin, etc.")
        if not click.confirm("Run with empty memory anyway?"):
            return

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
        click.secho("  QEMU binary:  NOT BUILT (run: remu build)", fg="yellow")

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
                     "remu fw-build -p silicon", fg="yellow")

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
        click.secho("  QEMU integration:  not linked (run: remu build)",
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

        click.echo("Done. Run 'remu images --check' to verify.")
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
    click.echo("Copy images manually or use: remu images --from-dir <path>")


# ── entry point ──────────────────────────────────────────────────────────────

def main():
    cli()


if __name__ == "__main__":
    main()
