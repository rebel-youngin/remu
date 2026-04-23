/*
 * REMU - R100 NPU System Emulator
 * SYSREG / SYSREMAP stub device
 *
 * The FW reads the chiplet ID from SYSREMAP registers during early boot
 * to determine which chiplet it is running on (0=primary, 1-3=secondary).
 *
 * Key registers at ROT_SYSREMAP_BASE (0x1FF0220000):
 *   - 0x0400 (REMAP_NOC): NOC remap value
 *   - 0x0404 (REMAP_NIC): NIC remap value
 *   - 0x0444 (CHIPLET_ID): chiplet ID [1:0]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

#define R100_SYSREG_REG_SIZE    0x10000
#define R100_SYSREG_REG_COUNT   (R100_SYSREG_REG_SIZE / 4)

struct R100SysregState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_SYSREG_REG_COUNT];
    uint32_t chiplet_id;
};

typedef struct R100SysregState R100SysregState;

DECLARE_INSTANCE_CHECKER(R100SysregState, R100_SYSREG, TYPE_R100_SYSREG)

static uint64_t r100_sysreg_read(void *opaque, hwaddr addr, unsigned size)
{
    R100SysregState *s = R100_SYSREG(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_SYSREG_REG_COUNT) {
        return 0;
    }

    switch (addr) {
    case R100_SYSREMAP_CHIPLET_ID:
        return s->chiplet_id;
    case R100_SYSREMAP_REMAP_NOC:
    case R100_SYSREMAP_REMAP_NIC:
        return s->chiplet_id;
    default:
        return s->regs[reg_idx];
    }
}

static void r100_sysreg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    R100SysregState *s = R100_SYSREG(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_SYSREG_REG_COUNT) {
        return;
    }

    s->regs[reg_idx] = (uint32_t)val;
}

static const MemoryRegionOps r100_sysreg_ops = {
    .read = r100_sysreg_read,
    .write = r100_sysreg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_sysreg_realize(DeviceState *dev, Error **errp)
{
    R100SysregState *s = R100_SYSREG(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-sysreg.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_sysreg_ops, s,
                          name, R100_SYSREG_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_sysreg_reset(DeviceState *dev)
{
    R100SysregState *s = R100_SYSREG(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static Property r100_sysreg_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100SysregState, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_sysreg_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_sysreg_realize;
    device_class_set_legacy_reset(dc, r100_sysreg_reset);
    device_class_set_props(dc, r100_sysreg_properties);
}

static const TypeInfo r100_sysreg_info = {
    .name = TYPE_R100_SYSREG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100SysregState),
    .class_init = r100_sysreg_class_init,
};

static void r100_sysreg_register_types(void)
{
    type_register_static(&r100_sysreg_info);
}

type_init(r100_sysreg_register_types)
