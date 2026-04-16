/*
 * REMU - R100 NPU System Emulator
 * HBM3 memory controller stub device
 *
 * The real HBM3 controller performs PHY training and calibration during
 * BL2 boot. This stub returns "training complete / ready" status immediately,
 * allowing the FW to proceed past memory initialization.
 *
 * Mapped at DRAM_CNTL_BASE (0x1FF7400000) per chiplet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

/*
 * HBM3 status register offsets (from DRAM_CNTL_BASE).
 * The exact register layout depends on the memory controller IP.
 * We return "ready" for all reads — the FW checks specific bits
 * to confirm training completion.
 */
#define HBM_STATUS_READY    0xFFFFFFFFU

static uint64_t r100_hbm_read(void *opaque, hwaddr addr, unsigned size)
{
    R100HBMState *s = R100_HBM(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_HBM_REG_COUNT) {
        return 0;
    }

    /*
     * Return stored value if written, otherwise return "all bits set"
     * which satisfies most ready/complete status checks.
     */
    if (s->regs[reg_idx] != 0) {
        return s->regs[reg_idx];
    }
    return HBM_STATUS_READY;
}

static void r100_hbm_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100HBMState *s = R100_HBM(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_HBM_REG_COUNT) {
        return;
    }

    s->regs[reg_idx] = (uint32_t)val;
}

static const MemoryRegionOps r100_hbm_ops = {
    .read = r100_hbm_read,
    .write = r100_hbm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_hbm_realize(DeviceState *dev, Error **errp)
{
    R100HBMState *s = R100_HBM(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-hbm.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_hbm_ops, s,
                          name, R100_HBM_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_hbm_reset(DeviceState *dev)
{
    R100HBMState *s = R100_HBM(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static Property r100_hbm_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100HBMState, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_hbm_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_hbm_realize;
    device_class_set_legacy_reset(dc, r100_hbm_reset);
    device_class_set_props(dc, r100_hbm_properties);
}

static const TypeInfo r100_hbm_info = {
    .name = TYPE_R100_HBM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100HBMState),
    .class_init = r100_hbm_class_init,
};

static void r100_hbm_register_types(void)
{
    type_register_static(&r100_hbm_info);
}

type_init(r100_hbm_register_types)
