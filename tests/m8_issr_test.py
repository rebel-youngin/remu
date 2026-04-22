#!/usr/bin/env python3
"""M8 ISSR shadow-bridge end-to-end test.

Exercises the bidirectional ISSR payload bridge the kmd / q-cp will
ride on top of for FW_BOOT_DONE and the TEST_IB ring:

    NPU → host:  FW writes one of the chiplet-0 PCIE r100-mailbox
                 ISSR0..63 scratch registers
                 → src/machine/r100_mailbox.c emits 8-byte
                   (BAR4-offset, value) frame on the new `issr`
                   chardev
                 → src/host/r100_npu_pci.c consumes the frame and
                   write-throughs into bar4_mmio_regs[off/4]
                 → KMD's rebel_mailbox_read(bar4 + MAILBOX_BASE
                   + idx*4) returns `value`

    host → NPU:  KMD writes to BAR4 + MAILBOX_BASE + idx*4
                 → src/host/r100_npu_pci.c's MMIO overlay emits the
                   existing 8-byte (offset, value) doorbell frame
                   (offset now disambiguates INTGR trigger vs. ISSR
                   payload on the NPU side)
                 → src/machine/r100_doorbell.c sees off in
                   [0x80, 0x180) and calls r100_mailbox_set_issr()
                 → FW read of ISSR[idx] returns `value`

Each direction gets its own phase. The test flips the socket
server/client roles for both directions so we can inject frames
directly (same trick as m6_doorbell_test.py and m7_msix_test.py —
production sockets have the host as server, which blocks third-party
injection).

Phase 1 (NPU→host): host-only x86_64 QEMU. Test binds `issr.sock` and
the NPU side of the chardev; QEMU connects as client. Test sends
synthetic ISSR frames and reads BAR4 via HMP `xp` on the host monitor.
The key frame carries `(MAILBOX_BASE + 4*4, 0xFB0D)` — the exact
shape of q-sys's bootdone_service signalling FW_BOOT_DONE.

Phase 2 (host→NPU): NPU-only aarch64 QEMU. Test binds `doorbell.sock`
and the host side; QEMU connects as client. Test sends synthetic
doorbell frames with MAILBOX_BASE-range offsets and reads the
mailbox's ISSR registers via HMP `xp` at R100_PCIE_MAILBOX_BASE +
ISSR0 + idx*4 = 0x1FF8160080 + idx*4. Proves r100-doorbell's new
MAILBOX_BASE branch writes through without raising any SPI.

Success criteria:
  1. Every valid synthetic ISSR frame (Phase 1) updates the host
     BAR4 MMIO register file — `xp` on host returns `value`.
  2. An ISSR frame with an offset outside [0x80, 0x180) gets dropped
     with status=bad-offset in issr.log + GUEST_ERROR.
  3. Every valid synthetic MAILBOX_BASE-range doorbell frame
     (Phase 2) writes through to the NPU mailbox — `xp` at the
     matching ISSR SFR address returns `value`.
  4. The FW_BOOT_DONE roundtrip works: sending `(0x90, 0xFB0D)` in
     Phase 1 makes the BAR4 read return `0xFB0D`, matching the
     KMD's FW_BOOT_DONE magic (rebel_regs.h:FW_BOOT_DONE).
"""

import os
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
QEMU_BIN_X86 = REPO / "build" / "qemu" / "qemu-system-x86_64"
QEMU_BIN_ARM = REPO / "build" / "qemu" / "qemu-system-aarch64"
IMAGES_DIR = REPO / "images"

RUN_NAME = "m8-issr"
RUN_DIR = REPO / "output" / RUN_NAME

# ── Constants — must match src/include/remu_addrmap.h ───────────────────────
MAILBOX_BASE        = 0x00000080   # R100_BAR4_MAILBOX_BASE
MAILBOX_COUNT       = 64           # R100_BAR4_MAILBOX_COUNT
MAILBOX_END         = MAILBOX_BASE + MAILBOX_COUNT * 4
ISSR4_OFFSET        = MAILBOX_BASE + 4 * 4      # FW_BOOT_DONE slot
ISSR7_OFFSET        = MAILBOX_BASE + 7 * 4      # reset-counter slot
BAD_OFFSET          = 0x00000200   # outside MAILBOX_BASE range

# Matches rebel_regs.h:FW_BOOT_DONE (the magic q-sys writes on boot).
FW_BOOT_DONE        = 0xFB0D

# Chiplet-0 PCIE mailbox physical base (R100_PCIE_MAILBOX_BASE) + the
# ISSR0 offset inside the SFR block (R100_MBX_ISSR0 = 0x80).
NPU_MAILBOX_BASE    = 0x1FF8160000
NPU_MBX_ISSR0       = 0x80


def hmp(sock_path, cmd, timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    for _ in range(50):
        try:
            s.connect(str(sock_path))
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.1)
    else:
        raise RuntimeError("monitor socket %s didn't come up" % sock_path)

    def _drain():
        buf = b""
        while b"(qemu) " not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.decode("utf-8", errors="replace")

    _drain()
    s.sendall((cmd + "\n").encode())
    resp = _drain()
    s.close()
    pat = re.escape(cmd) + r"\r?\n"
    last = None
    for m in re.finditer(pat, resp):
        last = m
    body = resp[last.end():] if last else resp
    body = body.replace("\r", "")
    if body.rstrip().endswith("(qemu)"):
        body = body.rstrip()[:-len("(qemu)")].rstrip()
    return body.strip("\n")


def hmp_readl(sock_path, phys_addr):
    """Read a single 32-bit word via HMP `xp /1wx <phys>`.
    Returns the u32 as an integer."""
    resp = hmp(sock_path, "xp /1wx 0x%x" % phys_addr)
    # Response format: "0000000000001234: 0xdeadbeef"
    m = re.search(r":\s*(0x[0-9a-fA-F]+)\s*$", resp.strip().splitlines()[-1])
    if not m:
        raise RuntimeError("xp readback not parseable: %r" % resp)
    return int(m.group(1), 16)


def frame(offset, value):
    return struct.pack("<II", offset, value)


def wait_for(predicate, timeout, poll=0.1, desc=""):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(poll)
    raise TimeoutError("timed out waiting for %s" % (desc or "predicate"))


def bind_listener(path):
    """Unix-socket server that accepts exactly one client.
    Returns a bound+listening socket. Caller owns close()."""
    if path.exists():
        try:
            path.unlink()
        except OSError:
            pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(str(path))
    srv.listen(1)
    srv.settimeout(15.0)
    return srv


def wipe(paths):
    for p in paths:
        try:
            if p.exists() or p.is_symlink():
                p.unlink()
        except OSError:
            pass


# ─── Phase 1: NPU → host ISSR shadow-egress ─────────────────────────────────

def phase1_npu_to_host():
    """Stand up minimal host x86 QEMU, impersonate the NPU-side
    mailbox's ISSR egress on issr.sock, and assert that every frame
    we send updates the matching BAR4 MMIO offset observable via
    HMP xp on the host monitor.

    Returns 0 on success, non-zero on failure.
    """
    print("\n=== Phase 1: NPU → host ISSR shadow-egress ===")

    host_dir = RUN_DIR / "host-npu2host"
    host_dir.mkdir(parents=True, exist_ok=True)
    issr_sock = host_dir / "issr.sock"
    issr_log = RUN_DIR / "issr-npu2host.log"
    host_mon = host_dir / "monitor.sock"
    host_qemu_log = host_dir / "qemu.log"

    wipe([issr_sock, issr_log, host_mon, host_qemu_log])

    # Bind the issr socket first (role flip: test = NPU).
    srv = bind_listener(issr_sock)

    # Minimal host QEMU. No shared memory — ISSR bridge is orthogonal
    # to the BAR0 splice. We still attach a full r100-npu-pci so
    # realize() installs the issr_chr receive handler.
    cmd = [
        str(QEMU_BIN_X86),
        "-M", "pc",
        "-cpu", "qemu64",
        "-m", "128M",
        "-display", "none",
        "-nographic",
        "-no-reboot",
        "-d", "guest_errors,unimp",
        "-D", str(host_qemu_log),
        "-chardev", "socket,id=issr,path=%s,reconnect=1" % issr_sock,
        "-chardev",
        "file,id=issr_dbg,path=%s,mux=off" % issr_log,
        "-device",
        "r100-npu-pci,issr=issr,issr-debug=issr_dbg",
        "-chardev",
        "socket,id=mon,path=%s,server=on,wait=off" % host_mon,
        "-mon", "chardev=mon,mode=readline",
        "-serial", "file:%s" % (host_dir / "serial.log"),
    ]
    (host_dir / "cmdline.txt").write_text(" \\\n  ".join(cmd) + "\n")

    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL, start_new_session=True)

    try:
        # Wait for the host to connect to our issr socket as a
        # reconnect=1 client.
        cli, _ = srv.accept()
        cli.settimeout(5.0)

        # Give SeaBIOS a moment to enumerate the PCI device and
        # program BAR addresses. Without this, HMP xp on BAR4 would
        # hit an unassigned address.
        wait_for(lambda: host_mon.exists(), 10.0, desc="host monitor socket")
        # Poll for BAR programming via info pci: the actual BAR4
        # address varies by seabios settings; we use `info mtree`
        # and look for r100.bar4.container, which only appears after
        # the BAR is placed in pci address space.
        wait_for(
            lambda: "r100.bar4.container" in hmp(host_mon, "info mtree"),
            15.0, desc="BAR4 programmed")

        # Pull the BAR4 base from info mtree. Format per QEMU:
        #   00000000febf0000-00000000fec07fff (prio 0, i/o): r100.bar4.container
        mtree = hmp(host_mon, "info mtree")
        bar4_re = re.compile(
            r"^\s*([0-9a-fA-F]+)-[0-9a-fA-F]+\s.*r100\.bar4\.container",
            re.MULTILINE)
        m = bar4_re.search(mtree)
        if m is None:
            raise RuntimeError(
                "could not locate r100.bar4.container in info mtree")
        bar4_base = int(m.group(1), 16)
        print("  BAR4 base: 0x%x" % bar4_base)

        # ── Test cases ──────────────────────────────────────────────
        # Each tuple: (bar4_off, value, expect_update, status_tag).
        cases = [
            (ISSR4_OFFSET, FW_BOOT_DONE, True, "ok"),     # FW_BOOT_DONE
            (ISSR7_OFFSET, 0x1234, True, "ok"),            # reset counter
            (MAILBOX_BASE + 63 * 4, 0xCAFEBABE, True, "ok"),  # edge: ISSR63
            (BAD_OFFSET, 0xDEADBEEF, False, "bad-offset"), # outside range
        ]

        failures = []
        for off, val, expect_update, tag in cases:
            cli.sendall(frame(off, val))
            # Give the host QEMU a moment to ingest the frame before
            # we read BAR4. The chardev is polled in QEMU's main loop
            # so a short sleep is enough.
            time.sleep(0.15)
            actual = hmp_readl(host_mon, bar4_base + off)
            ok = (actual == val) if expect_update else True
            print("    send off=0x%x val=0x%x -> BAR4[off]=0x%x (%s, tag=%s)"
                  % (off, val, actual,
                     "PASS" if ok else "FAIL", tag))
            if not ok:
                failures.append(
                    "off=0x%x expected BAR4 write-through val=0x%x but got 0x%x"
                    % (off, val, actual))

        # ── Check debug tail has one ASCII line per frame ───────────
        # The file chardev is line-buffered on QEMU's side; the tail
        # flushes after each write. Give it a beat in case.
        time.sleep(0.2)
        tail = issr_log.read_text()
        print("  issr debug tail:")
        for ln in tail.splitlines():
            print("    %s" % ln)

        # Expect one "status=ok" per accepted frame + one
        # "status=bad-offset" per rejected frame.
        ok_count = sum(1 for l in tail.splitlines()
                       if "status=ok" in l)
        bad_count = sum(1 for l in tail.splitlines()
                        if "status=bad-offset" in l)
        expected_ok = sum(1 for (_, _, e, _) in cases if e)
        expected_bad = sum(1 for (_, _, e, _) in cases if not e)
        if ok_count != expected_ok:
            failures.append(
                "expected %d 'status=ok' lines in issr.log, got %d"
                % (expected_ok, ok_count))
        if bad_count != expected_bad:
            failures.append(
                "expected %d 'status=bad-offset' lines in issr.log, got %d"
                % (expected_bad, bad_count))

        # ── Check GUEST_ERROR log has the expected rejection ────────
        if host_qemu_log.exists():
            qlog = host_qemu_log.read_text()
            if "out of MAILBOX_BASE range" not in qlog:
                failures.append(
                    "qemu.log missing GUEST_ERROR for bad-offset frame")

        if failures:
            print("FAIL:")
            for f in failures:
                print("  %s" % f)
            return 1

        print("PASS: phase 1 (NPU → host ISSR egress)")
        return 0

    finally:
        try:
            srv.close()
        except OSError:
            pass
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        wipe([issr_sock, host_mon])


# ─── Phase 2: host → NPU ISSR ingress via doorbell ──────────────────────────

def phase2_host_to_npu():
    """Stand up minimal NPU aarch64 QEMU, impersonate the host-side
    BAR4 MAILBOX_BASE writes on doorbell.sock, and assert that
    frames with MAILBOX_BASE-range offsets update the NPU mailbox's
    ISSR registers (observable via HMP xp on the mailbox SFR base).

    Returns 0 on success, non-zero on failure.
    """
    print("\n=== Phase 2: host → NPU ISSR ingress via doorbell ===")

    npu_dir = RUN_DIR / "npu-host2npu"
    npu_dir.mkdir(parents=True, exist_ok=True)
    db_sock = npu_dir / "doorbell.sock"
    db_log = RUN_DIR / "doorbell-host2npu.log"
    npu_mon = npu_dir / "monitor.sock"
    npu_qemu_log = npu_dir / "qemu.log"

    wipe([db_sock, db_log, npu_mon, npu_qemu_log])

    srv = bind_listener(db_sock)

    # Minimal NPU QEMU with the doorbell chardev; still loads the
    # usual FW images if present so info qtree / log filters look
    # like a normal run. HMP readback needs a CPU to actually
    # service the MMIO transaction, so we let the NPU boot normally
    # (it'll hit FreeRTOS or loop in BL1, either is fine here).
    cmd = [
        str(QEMU_BIN_ARM),
        "-M", "r100-soc,doorbell=doorbell,doorbell-debug=doorbell_dbg",
        "-display", "none",
        "-nographic",
        "-d", "guest_errors,unimp",
        "-D", str(npu_qemu_log),
        "-chardev", "socket,id=doorbell,path=%s,reconnect=1" % db_sock,
        "-chardev", "file,id=doorbell_dbg,path=%s,mux=off" % db_log,
        "-chardev", "file,id=uart0,path=%s,mux=off" % (npu_dir / "uart0.log"),
        "-serial", "chardev:uart0",
    ]
    for n in range(1, 4):
        cmd += [
            "-chardev",
            "file,id=uart%d,path=%s,mux=off"
            % (n, npu_dir / ("uart%d.log" % n)),
            "-serial", "chardev:uart%d" % n,
        ]
    cmd += [
        "-chardev", "file,id=hils,path=%s,mux=off"
        % (npu_dir / "hils.log"),
        "-serial", "chardev:hils",
        "-monitor", "unix:%s,server=on,wait=off" % npu_mon,
    ]

    # FW images are optional for this test — the mailbox is mounted
    # at machine init regardless of CPU activity. Skip loading them
    # to keep the test's startup cheap.
    (npu_dir / "cmdline.txt").write_text(" \\\n  ".join(cmd) + "\n")

    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL, start_new_session=True)

    try:
        cli, _ = srv.accept()
        cli.settimeout(5.0)

        wait_for(lambda: npu_mon.exists(), 10.0, desc="NPU monitor socket")
        # The mailbox is mounted by r100_soc_init before main_loop
        # starts, so `info qtree` listing r100-mailbox is the "ready"
        # signal we can use.
        wait_for(
            lambda: "r100-mailbox" in hmp(npu_mon, "info qtree"),
            15.0, desc="r100-mailbox realized")

        # ── Test cases ──────────────────────────────────────────────
        cases = [
            (MAILBOX_BASE + 4 * 4,  0xF00DCAFE),     # ISSR[4]
            (MAILBOX_BASE + 7 * 4,  0x00000001),     # ISSR[7] reset counter
            (MAILBOX_BASE + 63 * 4, 0x13371337),    # edge: ISSR[63]
        ]

        failures = []
        for off, val in cases:
            idx = (off - MAILBOX_BASE) // 4
            # Before: ISSR[idx] should be 0 (mailbox reset).
            npu_addr = NPU_MAILBOX_BASE + NPU_MBX_ISSR0 + idx * 4
            before = hmp_readl(npu_mon, npu_addr)
            cli.sendall(frame(off, val))
            time.sleep(0.15)
            after = hmp_readl(npu_mon, npu_addr)
            ok = (after == val)
            print("    send off=0x%x val=0x%x -> ISSR[%d] 0x%x -> 0x%x (%s)"
                  % (off, val, idx, before, after,
                     "PASS" if ok else "FAIL"))
            if not ok:
                failures.append(
                    "off=0x%x expected ISSR[%d]=0x%x but got 0x%x"
                    % (off, idx, val, after))

        # ── Also confirm no SPI was asserted: INTGR pending should
        #    still be 0 after all those payload writes (only INTGR
        #    trigger offsets 0x08/0x1c should raise interrupts). ─────
        pending0 = hmp_readl(npu_mon, NPU_MAILBOX_BASE + 0x14)   # INTSR0
        pending1 = hmp_readl(npu_mon, NPU_MAILBOX_BASE + 0x28)   # INTSR1
        if pending0 != 0 or pending1 != 0:
            failures.append(
                "MAILBOX_BASE payload writes leaked into INTSR: "
                "INTSR0=0x%x INTSR1=0x%x (expected 0)"
                % (pending0, pending1))
        else:
            print("    INTSR0=0x0 INTSR1=0x0 (no spurious SPI)")

        # ── Debug tail check ────────────────────────────────────────
        time.sleep(0.2)
        tail = db_log.read_text()
        db_lines = [l for l in tail.splitlines()
                    if any("off=0x%x" % off in l for off, _ in cases)]
        if len(db_lines) != len(cases):
            failures.append(
                "expected %d payload frames in doorbell.log, got %d"
                % (len(cases), len(db_lines)))

        if failures:
            print("FAIL:")
            for f in failures:
                print("  %s" % f)
            return 1

        print("PASS: phase 2 (host → NPU ISSR ingress)")
        return 0

    finally:
        try:
            srv.close()
        except OSError:
            pass
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        wipe([db_sock, npu_mon])


def main():
    if not QEMU_BIN_X86.is_file():
        print("%s not built; run ./remucli build first" % QEMU_BIN_X86)
        return 1
    if not QEMU_BIN_ARM.is_file():
        print("%s not built; run ./remucli build first" % QEMU_BIN_ARM)
        return 1

    RUN_DIR.mkdir(parents=True, exist_ok=True)

    rc = phase1_npu_to_host()
    if rc != 0:
        return rc
    rc = phase2_host_to_npu()
    if rc != 0:
        return rc

    print("\nPASS: M8 ISSR bridge plumbing end-to-end")
    print("  run dir: %s" % RUN_DIR)
    return 0


if __name__ == "__main__":
    sys.exit(main())
