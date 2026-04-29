#!/usr/bin/env python3
"""smmu_decode.py — offline SMMU-v3.2 STE / stage-2 PTE / page-walk decoder.

This is a *pure-Python* decoder that takes raw bytes (file, stdin, or
hex on the command line) and prints the SMMU-v3.2 fields q-cp / q-sys
program. No live SoC needed — pair it with `mem_dump.py` to inspect
what's actually in chiplet-0 DRAM at the addresses the smmu.log trace
points to.

Subcommands:

  ste     Decode one or more 64 B Stream Table Entries.
  pte     Decode 8 B stage-2 page-table entries (table / page / block).
  walk    Walk a 3-level stage-2 mapping: given an L1 base PA + an
          IPA + the granule/tsz/sl0 from a smmu.log `ste` line,
          re-walks the same path the device walker does and prints
          each level. Reads bytes from either a `mem_dump.py --format
          raw --output ...` artifact or directly from a remu-shm path
          via --shm.

Examples:

  # 1. From the `smmu cl=0 ste sid=5 ...` line in smmu.log, get the
  #    s2ttb=0x06000000 PA. Pull the L1 page out of remu-shm and feed
  #    it to walk:
  ./tests/scripts/mem_dump.py -n p11-smmu -o 0x06000000 -s 4096 -f raw \\
      --output /tmp/l1.bin
  # walk does its own dumps internally:
  ./tests/scripts/smmu_decode.py walk --shm /dev/shm/remu-p11-smmu/remu-shm \\
      --vttb 0x06000000 --tsz 25 --sl0 1 --granule 12 --ipa 0x100000000

  # 2. Decode a single STE pulled with `xp /16wx 0x06010000`:
  echo '01 00 00 0f 00 00 00 00 00 00 00 00 00 00 00 00 \\
        00 00 00 00 19 d4 00 00 00 00 00 00 00 00 00 00 \\
        00 00 00 06 00 00 00 00 00 00 00 00 00 00 00 00 \\
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00' \\
    | ./tests/scripts/smmu_decode.py ste --hex-stdin

The output is human-friendly, one line per field, so it's grep-able
and diff-able across runs (e.g. compare a known-good ste between p11
and p10 captures).

References:
  Arm SMMU-v3.2 spec, § 5.2 "Stream Table Entry"
  Arm SMMU-v3.2 spec, § "VMSAv8 stage-2 descriptors"
  external/qemu/hw/arm/smmu-internal.h (PTE_AP / PTE_AF / is_table_pte)
  external/ssw-bundle/products/.../q/sys/.../drivers/smmu/smmu.h
"""
import argparse
import mmap
import os
import struct
import sys
from pathlib import Path


# ──────────────────────────────────────────────────────────────────────
# STE field positions (mirror of src/machine/r100_smmu.c R100_STE0_*).
# ──────────────────────────────────────────────────────────────────────

STE_BYTES = 64

# STE[0]
STE0_VALID = 1 << 0
STE0_CONFIG_S = 1
STE0_CONFIG_M = 0x7 << STE0_CONFIG_S

CONFIG_NAMES = {
    0x0: "ABORT",
    0x4: "BYPASS",
    0x5: "S1_TRANS",
    0x6: "S2_TRANS",
    0x7: "ALL_TRANS",
}

# STE[2] (stage-2 fields)
STE2_S2VMID_S, STE2_S2VMID_M = 0, 0xFFFF
STE2_S2T0SZ_S, STE2_S2T0SZ_M = 32, 0x3F << 32
STE2_S2SL0_S, STE2_S2SL0_M = 38, 0x3 << 38
STE2_S2TG_S, STE2_S2TG_M = 46, 0x3 << 46
STE2_S2PS_S, STE2_S2PS_M = 48, 0x7 << 48
STE2_S2AA64 = 1 << 51
STE2_S2AFFD = 1 << 53
STE2_S2R = 1 << 58

S2TG_NAMES = {0: "4KB", 1: "64KB", 2: "16KB", 3: "?"}
S2PS_BITS = {0: 32, 1: 36, 2: 40, 3: 42, 4: 44, 5: 48, 6: 52, 7: 48}


def decode_ste(blob, sid=None):
    """Pretty-print one 64 B STE. `blob` is bytes, length 64. Returns
    a (str report, dict fields) tuple so callers (walk) can reuse the
    decoded stage-2 fields."""
    if len(blob) < STE_BYTES:
        raise ValueError("STE blob too short: %d B (need %d)" %
                         (len(blob), STE_BYTES))
    qw = struct.unpack("<8Q", blob[:STE_BYTES])
    ste0, ste1, ste2, ste3 = qw[0], qw[1], qw[2], qw[3]

    valid = bool(ste0 & STE0_VALID)
    cfg = (ste0 & STE0_CONFIG_M) >> STE0_CONFIG_S
    cfg_name = CONFIG_NAMES.get(cfg, "?")

    fields = {
        "ste0": ste0, "ste1": ste1, "ste2": ste2, "ste3": ste3,
        "valid": valid,
        "config": cfg_name,
        "config_raw": cfg,
        "vmid":   (ste2 & STE2_S2VMID_M) >> STE2_S2VMID_S,
        "s2t0sz": (ste2 & STE2_S2T0SZ_M) >> STE2_S2T0SZ_S,
        "s2sl0":  (ste2 & STE2_S2SL0_M) >> STE2_S2SL0_S,
        "s2tg":   (ste2 & STE2_S2TG_M) >> STE2_S2TG_S,
        "s2ps":   (ste2 & STE2_S2PS_M) >> STE2_S2PS_S,
        "s2aa64": bool(ste2 & STE2_S2AA64),
        "s2affd": bool(ste2 & STE2_S2AFFD),
        "s2r":    bool(ste2 & STE2_S2R),
        "s2ttb":  ste3 & ~0xFFF,
    }

    label = "STE" if sid is None else "STE sid=%u" % sid
    s2tg = fields["s2tg"]
    s2ps = fields["s2ps"]
    granule = {0: 12, 1: 16, 2: 14}.get(s2tg, 12)
    input_bits = 64 - fields["s2t0sz"] if fields["s2t0sz"] else 0
    out_bits = S2PS_BITS.get(s2ps, 48)

    report = []
    report.append("%s  raw=[%016x %016x %016x %016x %016x %016x %016x %016x]"
                  % (label, *qw))
    report.append("  STE[0]:  V=%d  config=%s (0x%x)" %
                  (valid, cfg_name, cfg))
    report.append("  STE[1]:  0x%016x" % ste1)
    if cfg in (0x6, 0x7):
        # Stage-2-relevant decode
        report.append(
            "  STE[2]:  S2T0SZ=%u (input %u bits) S2SL0=%u "
            "S2TG=%u (%s, granule %u-bit) S2PS=%u (out %u bits) "
            "S2AA64=%d S2AFFD=%d S2R=%d S2VMID=%u" %
            (fields["s2t0sz"], input_bits, fields["s2sl0"],
             s2tg, S2TG_NAMES.get(s2tg, "?"), granule,
             s2ps, out_bits,
             fields["s2aa64"], fields["s2affd"], fields["s2r"],
             fields["vmid"]))
        report.append("  STE[3]:  S2TTB=0x%016x  (page-aligned)"
                      % fields["s2ttb"])
    else:
        report.append("  STE[2]:  0x%016x  (stage-2 fields N/A for %s)"
                      % (ste2, cfg_name))
        report.append("  STE[3]:  0x%016x" % ste3)
    if any(qw[4:]):
        report.append("  STE[4..7]: %s" %
                      " ".join("0x%016x" % q for q in qw[4:]))
    return "\n".join(report), fields


# ──────────────────────────────────────────────────────────────────────
# Stage-2 PTE decoding.
# ──────────────────────────────────────────────────────────────────────

# Flags from external/qemu/hw/arm/smmu-internal.h.
PTE_VALID = 1 << 0          # bit[0]: descriptor valid
PTE_TYPE = 1 << 1           # bit[1]: 1 = table/page, 0 = block at L1/L2
PTE_S2AP_S = 6
PTE_S2AP_M = 0x3 << PTE_S2AP_S
PTE_AF = 1 << 10
PTE_NS = 1 << 5
PTE_SH_S = 8
PTE_SH_M = 0x3 << PTE_SH_S
PTE_OUTPUT_M = ((1 << 48) - 1) & ~0xFFF  # bits[47:12]; ignore upper-bits

S2AP_RW = {0: "no_access", 1: "RO", 2: "WO", 3: "RW"}
SH_NAMES = {0: "non_share", 1: "RES", 2: "outer", 3: "inner"}


def decode_pte(pte, level):
    """Decode one 64-bit stage-2 PTE at a given walk level (1/2/3).
    Returns (str report, kind, output_pa). `kind` ∈ {invalid, block,
    table, page}. output_pa is the PA bits[47:12] — caller adds the
    level-appropriate offset for block descriptors at L1/L2."""
    val = pte
    valid = bool(val & PTE_VALID)
    type_bit = bool(val & PTE_TYPE)
    if not valid:
        return ("PTE[L%d]  raw=0x%016x  INVALID" % (level, val),
                "invalid", 0)
    if level < 3:
        if type_bit:
            kind = "table"
        else:
            kind = "block"
    else:
        kind = "page" if type_bit else "invalid"  # L3 must have bit1=1
    out_pa = val & PTE_OUTPUT_M

    s2ap = (val & PTE_S2AP_M) >> PTE_S2AP_S
    af = bool(val & PTE_AF)
    sh = (val & PTE_SH_M) >> PTE_SH_S
    ns = bool(val & PTE_NS)

    out = ["PTE[L%d]  raw=0x%016x  %s  out=0x%016x" %
           (level, val, kind, out_pa)]
    if kind in ("page", "block"):
        out.append("           AP=%s (0x%x) AF=%d SH=%s NS=%d" %
                   (S2AP_RW.get(s2ap, "?"), s2ap, af,
                    SH_NAMES.get(sh, "?"), ns))
    return "\n".join(out), kind, out_pa


# ──────────────────────────────────────────────────────────────────────
# 3-level stage-2 walker.
# ──────────────────────────────────────────────────────────────────────

def index_for_level(ipa, level, granule_bits, sl0):
    """Compute the table-index bits at a given walk level for an Arm
    VMSAv8 stage-2 walk. Mirrors `external/qemu/hw/arm/smmu-common.c`'s
    walk arithmetic for the granule/sl0 combinations REMU exercises.

    For granule=12, sl0=1 (P11 default), the start level is L1 and:
      L1[idx] from bits[38:30]  (9 bits)
      L2[idx] from bits[29:21]  (9 bits)
      L3[idx] from bits[20:12]  (9 bits)
    Each entry is 8 B → table_offset = idx * 8."""
    if granule_bits == 12:
        bits_per_lvl = 9
    elif granule_bits == 14:
        bits_per_lvl = 11
    else:
        bits_per_lvl = 13  # 64 KB granule
    # Highest input bit is determined by tsz; we just expose the
    # straight layout here. The caller validates IPA range.
    idx_shift = granule_bits + bits_per_lvl * (3 - level)
    idx = (ipa >> idx_shift) & ((1 << bits_per_lvl) - 1)
    return idx


def read_qword(shm_path, pa):
    """Read one little-endian 64-bit word at byte offset `pa` from
    `shm_path`. PA is treated as a chiplet-0 offset into the shm —
    the same coordinate the SMMU walker uses internally on a
    chiplet-0 walk. Any out-of-range PA is reported as fatal."""
    sz = shm_path.stat().st_size
    if pa < 0 or pa + 8 > sz:
        raise ValueError("PA 0x%x out of range (file size 0x%x)"
                         % (pa, sz))
    fd = os.open(shm_path, os.O_RDONLY)
    try:
        page = mmap.PAGESIZE
        origin = (pa // page) * page
        size = pa + 8 - origin
        mm = mmap.mmap(fd, size, prot=mmap.PROT_READ, offset=origin)
        try:
            return struct.unpack("<Q", mm[pa - origin:pa - origin + 8])[0]
        finally:
            mm.close()
    finally:
        os.close(fd)


def walk(shm_path, vttb_pa, ipa, granule_bits, sl0, tsz):
    """Replay a 3-level stage-2 walk against a live shm file. Returns
    a list of (level, table_pa, idx, pte_pa, pte_val, decoded_str)."""
    # Determine starting level. Arm SMMU v3 sl0 encoding for 4 KB
    # granule: 0 = L2, 1 = L1, 2 = L3 (depending on input size).
    # For our P11 cases (input 39-bit, sl0=1) start at L1.
    # The general (granule, sl0) → start_level mapping is large; we
    # pin to the P11 path and refuse anything else with a clear error.
    if granule_bits != 12:
        raise NotImplementedError(
            "non-4KB granule walks not implemented yet (granule=%u). "
            "Fix me when q-cp programs 16/64 KB granules." % granule_bits)
    if sl0 == 1:
        start_lvl = 1
    elif sl0 == 0:
        start_lvl = 2
    elif sl0 == 2:
        start_lvl = 3
    else:
        raise ValueError("bad sl0=%u" % sl0)

    input_bits = 64 - tsz
    if ipa >> input_bits:
        raise ValueError(
            "IPA 0x%x is wider than s2t0sz allows (tsz=%u → %u bits)"
            % (ipa, tsz, input_bits))

    walk_log = []
    table_pa = vttb_pa
    cur_lvl = start_lvl
    while cur_lvl <= 3:
        idx = index_for_level(ipa, cur_lvl, granule_bits, sl0)
        pte_pa = table_pa + idx * 8
        pte = read_qword(shm_path, pte_pa)
        decoded, kind, next_out = decode_pte(pte, cur_lvl)
        walk_log.append((cur_lvl, table_pa, idx, pte_pa, pte, decoded, kind))
        if kind in ("invalid",):
            break
        if kind == "block":
            break  # block descriptor terminates the walk early
        if kind == "page":
            break
        # Table — descend.
        table_pa = next_out
        cur_lvl += 1
    return walk_log


def report_walk(ipa, walk_log, granule_bits=12):
    """Pretty-print a walk_log from walk(). Computes the final PA at
    the end if the walk produced a page/block."""
    out = ["Walk IPA=0x%x  (granule=%u-bit)" % (ipa, granule_bits)]
    final_pa = None
    for entry in walk_log:
        lvl, table_pa, idx, pte_pa, pte, decoded, kind = entry
        out.append("  L%d  table=0x%016x  idx=%4u  pte_pa=0x%016x"
                   % (lvl, table_pa, idx, pte_pa))
        for line in decoded.splitlines():
            out.append("    " + line)
        if kind in ("page", "block"):
            output_pa = pte & PTE_OUTPUT_M
            offset_in_page = ipa & ((1 << granule_bits) - 1)
            final_pa = output_pa | offset_in_page
        if kind == "invalid":
            out.append("    (walk halted: invalid PTE)")
    if final_pa is not None:
        out.append("Final PA = 0x%016x" % final_pa)
    else:
        out.append("Final PA = (none, walk did not reach a leaf)")
    return "\n".join(out)


# ──────────────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────────────

def parse_int(s):
    return int(s, 0)


def read_blob(args):
    """Read the bytes for a `ste` / `pte` invocation from the various
    sources the CLI supports."""
    if args.input is not None:
        return Path(args.input).read_bytes()
    if args.hex_stdin or args.hex:
        text = args.hex if args.hex else sys.stdin.read()
        # Accept whitespace-separated hex and 0x-prefixed runs.
        cleaned = text.replace(",", " ").replace("0x", "").split()
        joined = "".join(cleaned)
        return bytes.fromhex(joined)
    raise SystemExit("provide bytes via --input <FILE>, --hex-stdin, "
                     "or --hex 'aa bb cc...'")


def cmd_ste(args):
    blob = read_blob(args)
    n = max(1, len(blob) // STE_BYTES)
    if len(blob) % STE_BYTES != 0:
        print("warning: input length %d isn't a multiple of %d, "
              "decoding the leading %d STE(s)" %
              (len(blob), STE_BYTES, n), file=sys.stderr)
    for i in range(n):
        report, _ = decode_ste(blob[i * STE_BYTES:(i + 1) * STE_BYTES],
                               sid=i if n > 1 else args.sid)
        print(report)
        if i + 1 < n:
            print()
    return 0


def cmd_pte(args):
    blob = read_blob(args)
    n = len(blob) // 8
    if len(blob) % 8 != 0:
        print("warning: input length %d isn't a multiple of 8, "
              "trailing bytes ignored" % len(blob), file=sys.stderr)
    base = args.address_base
    for i in range(n):
        pte = struct.unpack_from("<Q", blob, i * 8)[0]
        decoded, _, _ = decode_pte(pte, args.level)
        if base is not None:
            # Prefix every continuation line with the same address so
            # `grep`s that filter on the PA see all the fields. The
            # first line gets the prefix once, follow-on lines get a
            # blank-of-equal-width so columns line up.
            prefix = "0x%016x  " % (base + i * 8)
            blank = " " * len(prefix)
            lines = decoded.splitlines()
            print(prefix + lines[0])
            for ln in lines[1:]:
                print(blank + ln)
        else:
            print(decoded)
    return 0


def cmd_walk(args):
    if args.shm is None:
        raise SystemExit("walk requires --shm <PATH-TO-remu-shm>")
    shm = Path(args.shm)
    if not shm.exists():
        raise SystemExit("shm file not found: %s" % shm)
    log = walk(shm, args.vttb, args.ipa,
               granule_bits=args.granule, sl0=args.sl0, tsz=args.tsz)
    print(report_walk(args.ipa, log, granule_bits=args.granule))
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    ap_ste = sub.add_parser("ste", help="Decode SMMU-v3 STE entries.")
    ap_ste.add_argument("--input", "-i", default=None,
                        help="Path to a binary blob.")
    ap_ste.add_argument("--hex", default=None,
                        help="Inline hex string.")
    ap_ste.add_argument("--hex-stdin", action="store_true",
                        help="Read hex from stdin.")
    ap_ste.add_argument("--sid", type=parse_int, default=None,
                        help="SID label for the report header (single STE).")
    ap_ste.set_defaults(func=cmd_ste)

    ap_pte = sub.add_parser("pte", help="Decode stage-2 PTE entries.")
    ap_pte.add_argument("--input", "-i", default=None)
    ap_pte.add_argument("--hex", default=None)
    ap_pte.add_argument("--hex-stdin", action="store_true")
    ap_pte.add_argument(
        "--level", "-l", type=int, default=3, choices=(1, 2, 3),
        help="Walk level the input came from. Affects table-vs-page "
             "interpretation (default: 3).")
    ap_pte.add_argument(
        "--address-base", type=parse_int, default=None,
        help="Print each PTE prefixed with its (synthesised) address; "
             "useful when dumping a whole 4 KB page.")
    ap_pte.set_defaults(func=cmd_pte)

    ap_walk = sub.add_parser(
        "walk", help="Replay a 3-level stage-2 walk against a live shm.")
    ap_walk.add_argument("--shm", required=True,
                         help="Path to remu-shm (e.g. "
                              "/dev/shm/remu-<run>/remu-shm).")
    ap_walk.add_argument("--vttb", type=parse_int, required=True,
                         help="L1 base PA (== STE3.S2TTB from smmu.log).")
    ap_walk.add_argument("--ipa", type=parse_int, required=True,
                         help="IPA / DVA to translate.")
    ap_walk.add_argument("--tsz", type=parse_int, default=25,
                         help="S2T0SZ (default 25 → 39-bit input range).")
    ap_walk.add_argument("--sl0", type=parse_int, default=1,
                         help="S2SL0 start-level encoding (default 1 = L1).")
    ap_walk.add_argument("--granule", type=parse_int, default=12,
                         help="Granule bits — 12=4KB (default), 14=16KB, "
                              "16=64KB. 16/64 KB walks are not "
                              "implemented yet.")
    ap_walk.set_defaults(func=cmd_walk)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
