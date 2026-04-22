/*
 * R100 NPU-side integrated MSI-X trigger (Phase 2, M7).
 *
 * This sysbus device is the reverse-direction counterpart to
 * src/machine/r100_doorbell.c. Silicon's DW PCIe controller snoops
 * writes to a hard-coded physical address (the
 * pf0_port_logic_msix_address_match_* register programmed by
 * pcie_msix_setup in external/ssw-bundle/.../drivers/pcie/
 * pcie_dw_msix.c); every matching 4-byte store is turned into a real
 * MSI-X TLP whose vector number is encoded in the low 11 bits of the
 * written value. See pcie_msix_trigger() in
 *   external/ssw-bundle/.../FreeRTOS/Source/rbln/msix.c
 *
 * FW-visible address on each chiplet (see REBELH_PCIE_MSIX_ADDR in
 *   external/ssw-bundle/.../drivers/pcie/pcie_rebelh.h):
 *
 *   REBELH_PCIE_MSIX_ADDR = 0x1B_FFFF_F000 (PCIE_IMSIX_BASE)
 *                         + 0x        FFC (PCIE_IMSIX_OFFSET)
 *                         = 0x1B_FFFF_FFFC
 *
 * The 32-bit "db_data" written at that offset encodes:
 *
 *   | 31:29 Rsvd | 28:24 PF | 23:16 VF | 15 VF-Act | 14:12 TC |
 *   | 11 Rsvd    | 10:0 Vector |
 *
 * On REMU we install one instance of this device on the chiplet-0
 * CPU view at PCIE_IMSIX_BASE with a 4 KB MMIO window. Offsets other
 * than PCIE_IMSIX_OFFSET are plain register-backed so FW readbacks /
 * near-miss stores don't raise GUEST_ERROR entries. A 4-byte store
 * to PCIE_IMSIX_OFFSET emits an 8-byte little-endian frame to the
 * configured chardev:
 *
 *   [0..3]  u32 offset   (always PCIE_IMSIX_OFFSET today; reserved
 *                         for future PF/VF channel selection)
 *   [4..7]  u32 db_data  (the raw encoded word from the FW store)
 *
 * That matches the M6 doorbell wire format byte-for-byte, just
 * flowing in the opposite direction. The host-side r100-npu-pci
 * decodes the frame, masks in R100_PCIE_IMSIX_VECTOR_MASK, and calls
 * msix_notify() on the PCI device (see src/host/r100_npu_pci.c).
 *
 * Optional debug tail chardev: if `debug-chardev` is set, every
 * emitted frame is echoed as an ASCII line
 *   imsix off=0x%x db_data=0x%x vector=%u count=%llu
 * to that chardev. Used by the M7 test and for humans.
 *
 * The device is instantiated by r100_soc_init when
 * `-machine r100-soc,msix=<chardev-id>` is set (see r100_soc.c).
 * With no chardev the device simply doesn't exist — M1-M6 single-
 * QEMU runs are unaffected.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "r100_soc.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100IMSIXState, R100_IMSIX)

/* Wire frame to host: off (u32 LE) + db_data (u32 LE) = 8 bytes.
 * Matches src/machine/r100_doorbell.c's frame layout so both
 * directions share the same parser in src/host/r100_npu_pci.c. */
#define R100_IMSIX_FRAME_SIZE   8

struct R100IMSIXState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharBackend chr;        /* egress channel to host r100-npu-pci */
    CharBackend debug_chr;  /* optional: human-readable tail chardev */

    /* Register-file backing for non-doorbell offsets. FW never reads
     * these back on silicon (the DW controller discards the TLP
     * after the address-match fires), but QEMU's MMIO ops need a
     * coherent read path for debugger inspection / save-restore. */
    uint32_t regs[R100_PCIE_IMSIX_SIZE / 4];

    /* Observability counters (preserved across reset; inspectable
     * via `info qtree` on the NPU HMP monitor and via the debug
     * tail chardev). */
    uint64_t frames_sent;
    uint64_t frames_dropped;  /* chardev write short / backend gone */
    uint32_t last_db_data;    /* last encoded word written by FW */
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
        /* Best-effort non-blocking write: debug tail is advisory and
         * must not back-pressure the MMIO path. */
        qemu_chr_fe_write(&s->debug_chr, (const uint8_t *)line, n);
    }
}

static void r100_imsix_emit(R100IMSIXState *s, uint32_t off, uint32_t db_data)
{
    uint8_t frame[R100_IMSIX_FRAME_SIZE];
    int rc;

    s->last_db_data = db_data;

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        /* No host connected — this is expected during early machine
         * init before the socket-client settles; count and drop. */
        s->frames_dropped++;
        return;
    }

    stl_le_p(&frame[0], off);
    stl_le_p(&frame[4], db_data);

    rc = qemu_chr_fe_write(&s->chr, frame, sizeof(frame));
    if (rc != sizeof(frame)) {
        qemu_log_mask(LOG_UNIMP,
                      "r100-imsix: frame off=0x%x db_data=0x%x "
                      "dropped (rc=%d)\n", off, db_data, rc);
        s->frames_dropped++;
        return;
    }
    s->frames_sent++;
    r100_imsix_emit_debug(s, off, db_data);
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

    /* The only trigger offset inside this page. Everything else is
     * stash-only: the FW never writes off-doorbell inside this
     * window, but qemu_log_mask noise would be unhelpful for the
     * rare alignment-probe case. */
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
    /* frames_sent / frames_dropped / last_db_data are kept across
     * reset so tests can inspect cumulative activity after a guest-
     * initiated soft reset (matches r100-doorbell semantics). */
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
    /* Instantiation is the machine's job — placement at the FW-
     * hardcoded PCIE_IMSIX_BASE is topology, not user-configurable. */
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
