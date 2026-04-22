/*
 * R100 NPU-side PCIe doorbell ingress (Phase 2, M6 + M7 mailbox refactor).
 *
 * This sysbus device terminates the cross-process doorbell channel that
 * originates in the x86 host guest's r100-npu-pci BAR4. On the host side
 * (src/host/r100_npu_pci.c), writes to MAILBOX_INTGR0 / MAILBOX_INTGR1
 * within BAR4 emit an 8-byte little-endian frame over a shared chardev:
 *
 *   [0..3]  u32 BAR4 offset (0x8 = MAILBOX_INTGR0, 0x1c = MAILBOX_INTGR1)
 *   [4..7]  u32 value written (bitmask of db_idx triggered within the
 *           INTGR group; see rebel_doorbell_write() in
 *           external/ssw-bundle/.../kmd/rebellions/rebel/rebel.c:108)
 *
 * On receipt of a complete frame we inject the written value into the
 * linked `r100-mailbox` instance's INTGR register (group 0 for offset
 * 0x08, group 1 for offset 0x1c). The mailbox's standard group→SPI
 * assertion then fires the chiplet-0 GIC SPI that FW listens on,
 * exactly as it would if the FW had issued a cfg-space store to
 * MAILBOX_PCIE_PRIVATE + INTGR{0,1} from inside the NPU. Before this
 * refactor the doorbell pulsed a placeholder SPI directly and
 * bypassed the mailbox entirely — useful as an M6 bring-up hack but
 * it hid the real FW register surface from the emulator and meant
 * any MSI-X reply path (M7) would need a second ad-hoc mechanism.
 *
 * Optional debug tail chardev: if the `debug-chardev` property is set,
 * every received frame is echoed as an ASCII line ("doorbell off=0x%x
 * val=0x%x count=%llu\n") to that chardev. The CLI wires it to
 * output/<run>/doorbell.log so tests and humans can confirm the NPU
 * observed a particular ring without parsing GIC state.
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
    int group;

    s->frames_received++;
    s->last_offset = off;
    s->last_value = val;

    switch (off) {
    case R100_BAR4_MAILBOX_INTGR0:
        group = 0;
        break;
    case R100_BAR4_MAILBOX_INTGR1:
        group = 1;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-doorbell: unexpected frame off=0x%x val=0x%x\n",
                      off, val);
        return;
    }

    /* The mailbox is the single ground-truth for host→FW signalling:
     * it latches pending bits in INTGR/INTSR, masks via INTMR, and
     * asserts its per-group SPI (wired by r100_soc_init). A later
     * INTCR write by the ISR clears the pending bit and deasserts
     * the line — no need for the ad-hoc pulse we used in pre-M7 M6. */
    r100_mailbox_raise_intgr(s->mailbox, group, val);

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
