/*
 * R100 integrated MSI-X trigger (M7, commit db3d1df). Reverse of
 * r100_cm7.c's doorbell ingress: snoops FW stores to REBELH_PCIE_MSIX_ADDR
 * (0x1BFFFFFFFC) and emits 8-byte (off, db_data) frame on the host
 * `msix` chardev. Matches pcie_msix_trigger in FreeRTOS/rbln/msix.c.
 * db_data layout: [28:24]PF [23:16]VF [15]VF-Act [14:12]TC [10:0]Vector.
 *
 * Machine-instantiated via `-machine r100-soc,msix=<id>`.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "r100_soc.h"
#include "r100_imsix.h"
#include "remu_frame.h"

DECLARE_INSTANCE_CHECKER(R100IMSIXState, R100_IMSIX, TYPE_R100_IMSIX)

struct R100IMSIXState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharBackend chr;
    CharBackend debug_chr;

    /* Reg-file backing for non-doorbell offsets: silicon discards after
     * address-match, but QEMU needs a read path for save/restore. */
    uint32_t regs[R100_PCIE_IMSIX_SIZE / 4];

    uint64_t frames_sent;
    uint64_t frames_dropped;
    uint32_t last_db_data;
};

static void r100_imsix_emit_debug(R100IMSIXState *s, uint32_t off,
                                  uint32_t db_data)
{
    char line[96];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "imsix off=0x%x db_data=0x%x vector=%u count=%" PRIu64 "\n",
                 off, db_data,
                 db_data & R100_PCIE_IMSIX_VECTOR_MASK,
                 s->frames_sent);
    if (n > 0) {
        /* Best-effort: debug tail must not back-pressure MMIO. */
        qemu_chr_fe_write(&s->debug_chr, (const uint8_t *)line, n);
    }
}

static void r100_imsix_emit(R100IMSIXState *s, uint32_t off, uint32_t db_data)
{
    RemuFrameEmitResult res;

    s->last_db_data = db_data;
    res = remu_frame_emit(&s->chr, "r100-imsix", off, db_data);
    if (res != REMU_FRAME_EMIT_OK) {
        /* Disconnected is expected during socket-client settle; the
         * host doesn't back-pressure, so we just drop-count either way. */
        s->frames_dropped++;
        return;
    }
    s->frames_sent++;
    r100_imsix_emit_debug(s, off, db_data);
}

/*
 * Public: invoked by the r100-cm7 BD-done state machine (M8b 3c) when
 * a command-queue completes. Wraps r100_imsix_emit so the chardev
 * stays single-owner — the wire format the host sees is the same
 * "off=R100_PCIE_IMSIX_DB_OFFSET, db_data=<vector>" an FW store would
 * have produced. Vector goes through R100_PCIE_IMSIX_VECTOR_MASK to
 * stay within the 11-bit field the silicon format allows (CR03 tops
 * out at 32 vectors == 5 bits, but we mask defensively anyway).
 */
void r100_imsix_notify(R100IMSIXState *s, uint32_t vector)
{
    uint32_t db_data = vector & R100_PCIE_IMSIX_VECTOR_MASK;

    r100_imsix_emit(s, R100_PCIE_IMSIX_DB_OFFSET, db_data);
}

static uint64_t r100_imsix_read(void *opaque, hwaddr addr, unsigned size)
{
    R100IMSIXState *s = R100_IMSIX(opaque);
    uint32_t idx = (uint32_t)(addr >> 2);

    if (idx >= ARRAY_SIZE(s->regs)) {
        return 0;
    }
    return s->regs[idx];
}

static void r100_imsix_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    R100IMSIXState *s = R100_IMSIX(opaque);
    uint32_t idx = (uint32_t)(addr >> 2);
    uint32_t v32 = (uint32_t)val;

    if (idx < ARRAY_SIZE(s->regs)) {
        s->regs[idx] = v32;
    }

    /* Only IMSIX_DB_OFFSET triggers; others stash-only (FW doesn't
     * hit off-doorbell in this page; logging would just add noise). */
    if (addr == R100_PCIE_IMSIX_DB_OFFSET) {
        r100_imsix_emit(s, (uint32_t)addr, v32);
    }
}

static const MemoryRegionOps r100_imsix_ops = {
    .read       = r100_imsix_read,
    .write      = r100_imsix_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void r100_imsix_realize(DeviceState *dev, Error **errp)
{
    R100IMSIXState *s = R100_IMSIX(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp,
                   "r100-imsix: 'chardev' property is required");
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_imsix_ops, s,
                          "r100-imsix", R100_PCIE_IMSIX_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void r100_imsix_unrealize(DeviceState *dev)
{
    R100IMSIXState *s = R100_IMSIX(dev);

    qemu_chr_fe_deinit(&s->chr, false);
    qemu_chr_fe_deinit(&s->debug_chr, false);
}

static void r100_imsix_reset(DeviceState *dev)
{
    R100IMSIXState *s = R100_IMSIX(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* Counters kept across reset (matches doorbell). */
}

static const VMStateDescription r100_imsix_vmstate = {
    .name = "r100-imsix",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, R100IMSIXState,
                             R100_PCIE_IMSIX_SIZE / 4),
        VMSTATE_UINT64(frames_sent, R100IMSIXState),
        VMSTATE_UINT64(frames_dropped, R100IMSIXState),
        VMSTATE_UINT32(last_db_data, R100IMSIXState),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_imsix_properties[] = {
    DEFINE_PROP_CHR("chardev", R100IMSIXState, chr),
    DEFINE_PROP_CHR("debug-chardev", R100IMSIXState, debug_chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_imsix_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 integrated MSI-X trigger (FW → host iMSIX-DB)";
    dc->realize = r100_imsix_realize;
    dc->unrealize = r100_imsix_unrealize;
    dc->vmsd = &r100_imsix_vmstate;
    device_class_set_legacy_reset(dc, r100_imsix_reset);
    device_class_set_props(dc, r100_imsix_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    /* Machine-instantiated only (FW-hardcoded PCIE_IMSIX_BASE). */
    dc->user_creatable = false;
}

static const TypeInfo r100_imsix_info = {
    .name          = TYPE_R100_IMSIX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100IMSIXState),
    .class_init    = r100_imsix_class_init,
};

static void r100_imsix_register_types(void)
{
    type_register_static(&r100_imsix_info);
}

type_init(r100_imsix_register_types)
