/*
 * R100 NPU-side PCIe doorbell ingress (M6 INTGR + M8a ISSR + M8b Stage 3a CM7 stub).
 *
 * Terminates the cross-process channel from host BAR4 MMIO writes.
 * Wire frame: u32 bar4_off LE + u32 val LE = 8 bytes.
 *
 *   0x08 INTGR0 — M8b Stage 3a CM7 stub path (SOFT_RESET bit 0 →
 *                 synthesise FW_BOOT_DONE into PF.ISSR[4]; other bits
 *                 relayed to VF0.INTGR1)
 *   0x1c INTGR1 — M6 host→NPU IRQ (r100_mailbox_raise_intgr → SPI)
 *   0x80..0x180 MAILBOX_BASE — M8a ISSR payload (r100_mailbox_set_issr:
 *                 scratch-only, no SPI, no chardev loopback)
 *   other        — GUEST_ERROR (silicon would NAK)
 *
 * Optional debug-chardev tail: one line per delivered frame to
 * output/<run>/doorbell.log. Machine-instantiated via
 * `-machine r100-soc,doorbell=<id>` (see r100_soc.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
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
#include "remu_frame.h"
#include "remu_doorbell_proto.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100DoorbellState, R100_DOORBELL)

struct R100DoorbellState {
    SysBusDevice parent_obj;

    CharBackend chr;
    CharBackend debug_chr;
    R100MailboxState *mailbox;      /* VF0: M6 INTGR sink + M8a ISSR sink */
    R100MailboxState *pf_mailbox;   /* PF: CM7-stub FW_BOOT_DONE target (M8b 3a) */

    /* Chardev byte stream may split a frame across callbacks. */
    RemuFrameRx rx;

    uint64_t frames_received;
    uint32_t last_offset;
    uint32_t last_value;
};

static int r100_doorbell_can_receive(void *opaque)
{
    R100DoorbellState *s = opaque;
    return remu_frame_rx_headroom(&s->rx);
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
        /* Best-effort: debug tail must not back-pressure ingress. */
        qemu_chr_fe_write(&s->debug_chr, (const uint8_t *)line, n);
    }
}

/* FW_BOOT_DONE value written into PF.ISSR[4] by the CM7 stub on SOFT_RESET. */
#define REMU_FW_BOOT_DONE 0xFB0D

static void r100_doorbell_deliver(R100DoorbellState *s,
                                  uint32_t off, uint32_t val)
{
    uint32_t issr_idx = 0;
    RemuDoorbellKind kind = remu_doorbell_classify(off, &issr_idx);

    s->frames_received++;
    s->last_offset = off;
    s->last_value = val;

    qemu_log_mask(LOG_TRACE,
                  "r100-doorbell: deliver off=0x%x val=0x%x count=%" PRIu64 "\n",
                  off, val, s->frames_received);

    switch (kind) {
    case REMU_DB_KIND_INTGR0:
        /*
         * REMU CM7-relay shortcut (M8b Stage 3a, commit a01d2b5).
         * Silicon routes host INTGR0 through PCIE_CM7 →
         * pcie_soft_reset_handler which re-emits FW_BOOT_DONE into
         * PF.ISSR[4]. REMU doesn't model CM7 and q-sys IDX_MAILBOX_PCIE_VF0
         * binds `default_cb` on CA73, so synthesise that effect here:
         *   - bit 0 (SOFT_RESET): PF.ISSR[4] = 0xFB0D → egress → host BAR4+0x90
         *   - other bits: relay to VF0.INTGR1 (visible IRQ; default_cb no-op)
         * VF0.INTGR0 untouched — no q-sys subscriber, would just noise dumps.
         */
        if ((val & 0x1U) && s->pf_mailbox) {
            r100_mailbox_cm7_stub_write_issr(s->pf_mailbox, 4,
                                             REMU_FW_BOOT_DONE);
        }
        if (val & ~0x1U) {
            r100_mailbox_raise_intgr(s->mailbox, 1, val & ~0x1U);
        }
        break;
    case REMU_DB_KIND_INTGR1:
        r100_mailbox_raise_intgr(s->mailbox, 1, val);
        break;
    case REMU_DB_KIND_ISSR:
        r100_mailbox_set_issr(s->mailbox, issr_idx, val);
        break;
    case REMU_DB_KIND_OTHER:
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
    uint32_t off, val;

    while (size > 0) {
        if (remu_frame_rx_feed(&s->rx, &buf, &size, &off, &val)) {
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
                             NULL, NULL, s, NULL, true);
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

    remu_frame_rx_reset(&s->rx);
    /* frames_received / last_* kept across reset for test inspection. */
}

static const VMStateDescription r100_doorbell_vmstate = {
    .name = "r100-doorbell",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(rx.len, R100DoorbellState),
        VMSTATE_UINT8_ARRAY(rx.buf, R100DoorbellState, REMU_FRAME_SIZE),
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
    /* Machine-instantiated only (mailbox link is topology). */
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
