#!/usr/bin/env python3
"""mem_dump.py — generic shm-backed memory dumper for REMU runs.

Both QEMUs share three memory-backend-files under /dev/shm/remu-<name>/
(see CLAUDE.md → "shm" splice points):

  - remu-shm    chiplet-0 DRAM head + host BAR0 (M4/M5)
  - host-ram    x86 guest main RAM, also aliased over the chiplet-0
                4 GB PCIe-outbound iATU window (P10-fix)
  - cfg-shadow  4 KB BAR2 cfg-head ↔ NPU cfg-mirror (P10-fix)

This tool mmap's any of those backends read-only and dumps a window
to stdout (or to a file). It's region-agnostic: anything the device
models route through chiplet-0 DRAM ends up here, including BD
descriptors, queue_descs, command buffers, RBDMA OTO source/destination
buffers, q-cp's stack/heap when the kmd has staged something there,
SMMU stream-tables, stage-2 page tables, etc.

Output formats (all little-endian; that's what the CA73 / x86 pair
agree on):

  --format hex     `xxd`-style hex + ASCII gutter (default)
  --format u64     one little-endian 64-bit word per line, decimal addr
  --format u32     one 32-bit word per line
  --format raw     binary stdout (or write to --output file)

Examples:

  # Dump 256 B from chiplet-0 DRAM @ 0x07000000 (P10 src buffer):
  ./tests/scripts/mem_dump.py --name p11-smmu --offset 0x07000000 --size 256

  # Pull the STE entry q-cp wrote and pipe to the SMMU decoder:
  ./tests/scripts/mem_dump.py --name p11-smmu --offset 0x06010000 \
      --size 64 --format raw --output /tmp/ste0.bin
  ./tests/scripts/smmu_decode.py ste --input /tmp/ste0.bin

  # Dump 64 KB starting from a PA the smmu.log line printed:
  ./tests/scripts/mem_dump.py -n p10 -o 0x06002000 -s 0x10000 -f u64 \
      --output /tmp/l3.txt

The script is process-isolated from the QEMUs: the shm is mmap'd
PROT_READ, no monitor / gdbstub round trip, no race against the live
guest beyond ordinary tearing for in-flight stores. Pass `--snapshot`
to copy the bytes into Python before formatting if the tearing risk
matters for your post-mortem (default: False; the dump itself reads
straight from the mapping).
"""
import argparse
import os
import mmap
import sys
from pathlib import Path


SHM_ROOT = Path("/dev/shm")

# Map of human region names → backend filename in /dev/shm/remu-<name>/.
# Order matters for `--list`; keep most-used first.
REGIONS = {
    "remu-shm":   "remu-shm",
    "host-ram":   "host-ram",
    "cfg-shadow": "cfg-shadow",
}


def parse_int(s):
    """Accept 0x-prefixed hex, 0b binary, or plain decimal. argparse's
    `type=int` doesn't take 0x natively."""
    return int(s, 0)


def resolve_region(name, region):
    """Return Path to the shm file for (run name, region tag).
    Raises FileNotFoundError if the run hasn't been started or the
    region tag is unknown."""
    if region not in REGIONS:
        raise ValueError(
            "unknown region %r; valid: %s"
            % (region, ", ".join(REGIONS.keys())))
    p = SHM_ROOT / ("remu-" + name) / REGIONS[region]
    if not p.exists():
        raise FileNotFoundError(
            "no shm file at %s — is the run '%s' active and started "
            "with --host?" % (p, name))
    return p


def fmt_hex(addr, buf, group=4, width=16):
    """xxd-style hex + ASCII. addr is the *logical* address of buf[0]
    (caller-supplied; we don't assume it equals the offset within the
    shm file — useful when dumping page-table fragments and you want
    to see the IPA, not the chiplet-PA). group=4 keeps little-endian
    words visually intact."""
    out = []
    for i in range(0, len(buf), width):
        chunk = buf[i:i + width]
        hex_parts = []
        for j in range(0, len(chunk), group):
            hex_parts.append(chunk[j:j + group].hex())
        hex_col = " ".join(hex_parts).ljust(
            (2 * group * (width // group)) + (width // group - 1) + 2)
        ascii_col = "".join(
            chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
        out.append("%016x  %s  |%s|" % (addr + i, hex_col, ascii_col))
    return "\n".join(out)


def fmt_words(addr, buf, word_size):
    """One LE word per line, with logical addr in the left column."""
    out = []
    for i in range(0, len(buf), word_size):
        chunk = buf[i:i + word_size]
        if len(chunk) < word_size:
            # Trailing partial word — pad with zeros so int.from_bytes
            # doesn't error. The hex column makes the truncation
            # obvious to the reader.
            chunk = chunk + b"\x00" * (word_size - len(chunk))
        v = int.from_bytes(chunk, "little")
        if word_size == 8:
            out.append("%016x  0x%016x  %d" % (addr + i, v, v))
        else:
            out.append("%016x  0x%08x  %d" % (addr + i, v, v))
    return "\n".join(out)


def dump(buf, addr, fmt):
    if fmt == "hex":
        return fmt_hex(addr, buf)
    if fmt == "u64":
        return fmt_words(addr, buf, 8)
    if fmt == "u32":
        return fmt_words(addr, buf, 4)
    raise ValueError("unknown format %r" % fmt)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "-n", "--name", required=False, default=None,
        help="REMU run name (matches `./remucli run --name <NAME>`). "
             "Resolves to /dev/shm/remu-<NAME>/<region>. Required "
             "unless --file is given.")
    ap.add_argument(
        "-r", "--region", default="remu-shm", choices=list(REGIONS.keys()),
        help="Which shm-backed region to read. Default: remu-shm "
             "(chiplet-0 DRAM head + host BAR0 splice).")
    ap.add_argument(
        "--file", default=None, type=Path,
        help="Bypass --name/--region and read the given file directly. "
             "Useful when the shm has been copied or you want to dump "
             "a saved snapshot.")
    ap.add_argument(
        "-o", "--offset", type=parse_int, default=0,
        help="Starting byte offset into the region (decimal or 0x... hex).")
    ap.add_argument(
        "-s", "--size", type=parse_int, default=0x100,
        help="Number of bytes to read (default: 256).")
    ap.add_argument(
        "-a", "--address-base", type=parse_int, default=None,
        help="Logical address printed in the left column. Defaults to "
             "--offset (so the column matches the chiplet-PA you typed). "
             "Pass an IPA / DVA when dumping a chunk you've already "
             "translated through SMMU and want to see in the source-of-"
             "truth coordinates.")
    ap.add_argument(
        "-f", "--format", default="hex",
        choices=("hex", "u64", "u32", "raw"),
        help="Output format (default: hex).")
    ap.add_argument(
        "--output", "-O", type=Path, default=None,
        help="Write to file instead of stdout (binary for --format raw, "
             "text otherwise). Path is created/truncated.")
    ap.add_argument(
        "--snapshot", action="store_true",
        help="Copy bytes into Python before formatting (defaults to "
             "False — we read straight from the mapping). Use this if "
             "you're chasing a tearing race against in-flight stores; "
             "the live mapping can change under you mid-dump.")
    ap.add_argument(
        "--list", action="store_true",
        help="List available regions for the run name and exit.")
    args = ap.parse_args()

    if args.list:
        if not args.name:
            ap.error("--list requires --name")
        run_dir = SHM_ROOT / ("remu-" + args.name)
        if not run_dir.is_dir():
            print("no run dir at %s" % run_dir, file=sys.stderr)
            return 1
        for tag, fname in REGIONS.items():
            p = run_dir / fname
            if p.exists():
                print("%-12s  %s  (%d bytes)" %
                      (tag, p, p.stat().st_size))
            else:
                print("%-12s  %s  (missing)" % (tag, p))
        return 0

    if args.file is not None:
        path = args.file
        if not path.exists():
            print("file not found: %s" % path, file=sys.stderr)
            return 1
    else:
        if not args.name:
            ap.error("--name is required (or use --file)")
        try:
            path = resolve_region(args.name, args.region)
        except (FileNotFoundError, ValueError) as e:
            print(str(e), file=sys.stderr)
            return 1

    file_size = path.stat().st_size
    if args.offset < 0 or args.offset >= file_size:
        print("offset 0x%x is out of range (file size 0x%x)"
              % (args.offset, file_size), file=sys.stderr)
        return 1
    avail = file_size - args.offset
    size = args.size if args.size <= avail else avail
    if size != args.size:
        print("note: clamped size to 0x%x (file ends at 0x%x)"
              % (size, file_size), file=sys.stderr)

    addr_base = args.address_base
    if addr_base is None:
        addr_base = args.offset

    fd = os.open(path, os.O_RDONLY)
    try:
        # Page-aligned mmap origin (Linux mmap requires offset be a
        # multiple of PAGE_SIZE), then index into the result.
        page = mmap.PAGESIZE
        map_origin = (args.offset // page) * page
        map_size = args.offset + size - map_origin
        mm = mmap.mmap(fd, map_size, prot=mmap.PROT_READ,
                       offset=map_origin)
        try:
            slice_start = args.offset - map_origin
            buf = mm[slice_start:slice_start + size]
            if args.snapshot:
                buf = bytes(buf)
        finally:
            mm.close()
    finally:
        os.close(fd)

    if args.format == "raw":
        if args.output is None:
            sys.stdout.buffer.write(buf)
        else:
            args.output.write_bytes(bytes(buf))
        return 0

    text = dump(buf, addr_base, args.format) + "\n"
    if args.output is None:
        sys.stdout.write(text)
    else:
        args.output.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
