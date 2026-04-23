/*
 * R100 NPU-side PCIe doorbell ingress (Phase 2, M6 mailbox-route +
 * M8a ISSR payload-route).
 *
 * This sysbus device terminates the cross-process doorbell channel that
 * originates in the x86 host guest's r100-npu-pci BAR4. On the host side
 * (src/host/r100_npu_pci.c), BAR4 writes into the 4 KB MMIO overlay
 * emit an 8-byte little-endian frame over a shared chardev:
 *
 *   [0..3]  u32 BAR4 offset
 *   [4..7]  u32 value written
 *
 * The offset disambiguates two distinct flows that both cross the
 * same wire:
 *
 *   M6 (INTGR trigger, host → NPU IRQ)
 *   ---------------------------------------------------------------
 *     off = 0x08 (MAILBOX_INTGR0) or 0x1c (MAILBOX_INTGR1)
 *     val = bitmask of db_idx triggered within the INTGR group
 *           (see rebel_doorbell_write() in
 *            external/ssw-bundle/.../kmd/rebellions/rebel/rebel.c:108)
 *     Action: call r100_mailbox_raise_intgr() on the linked mailbox,
 *             which sets INTSR bits and lets INTMSR = INTSR & ~INTMR
 *             drive the chiplet-0 GIC SPI.
 *
 *   M8a (ISSR payload, host → NPU scratch-register write)
 *   ---------------------------------------------------------------
 *     off = 0x80..0x180  (MAILBOX_BASE .. MAILBOX_END, the 64-word
 *                          ISSR0..63 window — R100_BAR4_MAILBOX_*)
 *     val = full 32-bit value to store in ISSRn
 *     Action: call r100_mailbox_set_issr(idx, val) which updates the
 *             NPU-side scratch register *without* asserting any SPI
 *             and *without* looping the value back out on the issr
 *             chardev (the host already has the value — no shadow
 *             write-through needed, and looping would alias the
 *             write onto itself).
 *
 *   Anything else is a protocol violation — silicon would NAK a PCIe
 *   write that fell off the mapped window; we log GUEST_ERROR and
 *   drop the frame so bogus traffic shows up in -D logs.
 *
 * Optional debug tail chardev: if the `debug-chardev` property is set,
 * every successfully-delivered frame (INTGR or ISSR, not the
 * bad-offset ones — those go only to the qemu.log GUEST_ERROR path)
 * is echoed as an ASCII line
 * ("doorbell off=0x%x val=0x%x count=%llu\n") to that chardev. The
 * CLI wires it to output/<run>/doorbell.log so tests and humans can
 * confirm the NPU observed a particular ring or ISSR write without
 * parsing GIC state.
 *
 * The device is instantiated by r100_soc_init when
 * `-machine r100-soc,doorbell=<chardev-id>` is set (see r100_soc.c).
 * That same init path creates the chiplet-0 r100-mailbox instance
 * this device links to. With no chardev the device simply doesn't
 * exist — M1-M5 single-QEMU runs are unaffected.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "r100_soc.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100DoorbellState, R100_DOORBELL)

/* Wire frame: off (u32 LE) + val (u32 LE) = 8 bytes. */
#define R100_DOORBELL_FRAME_SIZE 8

struct R100DoorbellState {
    SysBusDevice parent_obj;

    CharBackend chr;        /* ingress channel from host r100-npu-pci */
    CharBackend debug_chr;  /* optional: human-readable tail chardev */
    R100MailboxState *mailbox; /* link<r100-mailbox>: INTGR write sink */
    /*
     * Optional PF mailbox link for the REMU CM7-relay shortcut. On
     * silicon, the host's INTGR0 SOFT_RESET doorbell is serviced by
     * PCIE_CM7's pcie_soft_reset_handler, which re-emits FW_BOOT_DONE
     * (0xFB0D) into PF.ISSR[4] via MMIO. REMU does not model CM7, so
     * when this link is set we synthesise the same ISSR egress here
     * in the doorbell path — see r100_doorbell_deliver().
     */
    R100MailboxState *pf_mailbox;

    /* Reassembly buffer: frames arrive as one write() on the host
     * side, but char-socket may still split a single frame across
     * multiple _read callbacks, so we accumulate until we have a
     * full R100_DOORBELL_FRAME_SIZE packet. */
    uint8_t rx_buf[R100_DOORBELL_FRAME_SIZE];
    uint32_t rx_len;

    /* Observability for tests / HMP `info qtree` property dump. */
    uint64_t frames_received;   /* total frames successfully parsed */
    uint32_t last_offset;       /* last seen BAR4 offset */
    uint32_t last_value;        /* last seen trigger value */
};

static int r100_doorbell_can_receive(void *opaque)
{
    R100DoorbellState *s = opaque;
    return R100_DOORBELL_FRAME_SIZE - s->rx_len;
}

static void r100_doorbell_emit_debug(R100DoorbellState *s,
                                     uint32_t off, uint32_t val)
{
    char line[80];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "doorbell off=0x%x val=0x%x count=%" PRIu64 "\n",
                 off, val, s->frames_received);
    if (n > 0) {
        /* Best-effort non-blocking write: debug tail is advisory and
         * must not back-pressure the ingress path. */
        qemu_chr_fe_write(&s->debug_chr, (const uint8_t *)line, n);
    }
}

static void r100_doorbell_deliver(R100DoorbellState *s,
                                  uint32_t off, uint32_t val)
{
    s->frames_received++;
    s->last_offset = off;
    s->last_value = val;

    /* REMU-TRACE: visible in NPU qemu.stderr.log */
    fprintf(stderr, "REMU-TRACE: doorbell_deliver off=0x%x val=0x%x count=%" PRIu64 "\n",
            off, val, s->frames_received);
    fflush(stderr);

    /*
     * BAR4 frames fall into three classes distinguished by offset:
     *
     *  0x08 / 0x1c  INTGR0 / INTGR1 trigger  → raise mailbox INTGR
     *               and let the per-group SPI fire via INTMSR.
     *               This is the M6 doorbell path.
     *
     *  0x80..0x180  MAILBOX_BASE payload     → write through to
     *               the linked mailbox's ISSR0..63 scratch store.
     *               No interrupt asserted; the KMD / FW reads these
     *               back after they've already taken the matching
     *               INTGR SPI (see ipm_samsung_{read,write} +
     *               mb_read/mb_write in q-sys). This is the M8
     *               host→NPU ISSR ingress path.
     *
     *  anything else                         → protocol violation;
     *               log and drop. Silicon would just NAK a PCIe
     *               write that fell off the mapped window — we
     *               treat it as a GUEST_ERROR so bogus frames are
     *               traceable in the -D log.
     */
    if (off == R100_BAR4_MAILBOX_INTGR0) {
        /*
         * REMU CM7-relay shortcut.
         *
         * Silicon path: host BAR4 INTGR0 write → PCIE_CM7 fires on SPI
         * 184 → ipm_samsung_isr → pcie_host2cm7_callback →
         * pcie_doorbell_cb_table[channel]. Channel 0 (SOFT_RESET) runs
         * pcie_soft_reset_handler, which tears clusters down + back up
         * via PMU and eventually re-emits FW_BOOT_DONE to PF.ISSR[4].
         *
         * REMU path: we model neither CM7 nor the PMU reset sequence,
         * and the CA73 q-sys ISR table binds IDX_MAILBOX_PCIE_VF0 to
         * `default_cb` (the PCIE-specific handlers in
         * external/.../drivers/pcie/pcie_mailbox_callback.c are
         * compiled only for __TARGET_PCIE / CM7 — see that file's
         * CMakeLists.txt). So we short-circuit CM7 right here in QEMU:
         *
         *   - SOFT_RESET (bit 0): write 0xFB0D (FW_BOOT_DONE) into
         *     PF.ISSR[4] via the CM7 stub helper and emit the ISSR
         *     egress frame, so the host BAR4 shadow at +0x90 converges
         *     and `rebel_reset_done` observes the expected value.
         *
         *   - Other bits (VF2PF_RESET, HEARTBEAT, SOFT_RESET_CTRL,
         *     ...): relay onto VF0.INTGR1 so the CA73 CP-side ISR at
         *     least gets a visible IRQ + `REMU-TRACE: ipm_isr` entry
         *     in uart0.log. default_cb won't do anything useful, but
         *     we prefer a visible no-op over a silent drop.
         *
         * VF0.INTSR0/INTGR0 is intentionally left untouched — q-sys
         * doesn't subscribe to SPI 184, and latching pending bits
         * there would just noise up HMP dumps without any observer.
         */
        if ((val & 0x1) && s->pf_mailbox) {
            /* PCIE_DOORBELL_SOFT_RESET == channel 0 (bit 0). */
            #define REMU_FW_BOOT_DONE 0xFB0D
            r100_mailbox_cm7_stub_write_issr(s->pf_mailbox, 4,
                                             REMU_FW_BOOT_DONE);
            #undef REMU_FW_BOOT_DONE
        }
        if (val & ~0x1U) {
            /* Leftover non-SOFT_RESET bits (if any): raise on INTGR1. */
            r100_mailbox_raise_intgr(s->mailbox, 1, val & ~0x1U);
        }
    } else if (off == R100_BAR4_MAILBOX_INTGR1) {
        r100_mailbox_raise_intgr(s->mailbox, 1, val);
    } else if (off >= R100_BAR4_MAILBOX_BASE &&
               off < R100_BAR4_MAILBOX_END) {
        uint32_t idx = (off - R100_BAR4_MAILBOX_BASE) >> 2;
        r100_mailbox_set_issr(s->mailbox, idx, val);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-doorbell: unexpected frame off=0x%x val=0x%x\n",
                      off, val);
        return;
    }

    r100_doorbell_emit_debug(s, off, val);
}

static void r100_doorbell_receive(void *opaque, const uint8_t *buf, int size)
{
    R100DoorbellState *s = opaque;

    while (size > 0) {
        uint32_t want = R100_DOORBELL_FRAME_SIZE - s->rx_len;
        uint32_t take = size < (int)want ? (uint32_t)size : want;

        memcpy(s->rx_buf + s->rx_len, buf, take);
        s->rx_len += take;
        buf += take;
        size -= take;

        if (s->rx_len == R100_DOORBELL_FRAME_SIZE) {
            uint32_t off = ldl_le_p(s->rx_buf);
            uint32_t val = ldl_le_p(s->rx_buf + 4);
            s->rx_len = 0;
            r100_doorbell_deliver(s, off, val);
        }
    }
}

static void r100_doorbell_realize(DeviceState *dev, Error **errp)
{
    R100DoorbellState *s = R100_DOORBELL(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp,
                   "r100-doorbell: 'chardev' property is required");
        return;
    }
    if (s->mailbox == NULL) {
        error_setg(errp,
                   "r100-doorbell: 'mailbox' link property is required");
        return;
    }

    qemu_chr_fe_set_handlers(&s->chr,
                             r100_doorbell_can_receive,
                             r100_doorbell_receive,
                             NULL,   /* event */
                             NULL,   /* be_change */
                             s,
                             NULL,   /* context */
                             true);  /* set_open */
}

static void r100_doorbell_unrealize(DeviceState *dev)
{
    R100DoorbellState *s = R100_DOORBELL(dev);

    qemu_chr_fe_deinit(&s->chr, false);
    qemu_chr_fe_deinit(&s->debug_chr, false);
}

static void r100_doorbell_reset(DeviceState *dev)
{
    R100DoorbellState *s = R100_DOORBELL(dev);

    s->rx_len = 0;
    /* frames_received / last_* are observability counters that we
     * intentionally keep across reset so tests can inspect the
     * cumulative total after a guest-initiated soft reset. */
}

static const VMStateDescription r100_doorbell_vmstate = {
    .name = "r100-doorbell",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(rx_len, R100DoorbellState),
        VMSTATE_UINT8_ARRAY(rx_buf, R100DoorbellState,
                            R100_DOORBELL_FRAME_SIZE),
        VMSTATE_UINT64(frames_received, R100DoorbellState),
        VMSTATE_UINT32(last_offset, R100DoorbellState),
        VMSTATE_UINT32(last_value, R100DoorbellState),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_doorbell_properties[] = {
    DEFINE_PROP_CHR("chardev", R100DoorbellState, chr),
    DEFINE_PROP_CHR("debug-chardev", R100DoorbellState, debug_chr),
    DEFINE_PROP_LINK("mailbox", R100DoorbellState, mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_LINK("pf-mailbox", R100DoorbellState, pf_mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_doorbell_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 PCIe doorbell ingress (host BAR4 → mailbox INTGR)";
    dc->realize = r100_doorbell_realize;
    dc->unrealize = r100_doorbell_unrealize;
    dc->vmsd = &r100_doorbell_vmstate;
    device_class_set_legacy_reset(dc, r100_doorbell_reset);
    device_class_set_props(dc, r100_doorbell_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    /* Must be instantiated by the machine (see r100_soc.c); no
     * `-device r100-doorbell` from the cmdline, since the mailbox
     * link is part of the SoC topology. */
    dc->user_creatable = false;
}

static const TypeInfo r100_doorbell_info = {
    .name          = TYPE_R100_DOORBELL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100DoorbellState),
    .class_init    = r100_doorbell_class_init,
};

static void r100_doorbell_register_types(void)
{
    type_register_static(&r100_doorbell_info);
}

type_init(r100_doorbell_register_types)
