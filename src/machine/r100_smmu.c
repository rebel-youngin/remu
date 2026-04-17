/*
 * REMU - R100 NPU System Emulator
 * SMMU-600 TCU stub device
 *
 * Minimal stub for the Arm SMMU-600 register block at TCU_OFFSET
 * (0x1FF4200000, per external/.../rebel_h_baseoffset.h). BL2's
 * smmu_early_init() programs the EVENTQ base, the strtab base, the
 * GBPA fence and finally enables event queues via CR0. It then polls
 * two ack/ready paths:
 *
 *   1. CR0 -> CR0ACK : writing EVENTQEN / SMMUEN must be reflected in
 *      cr0ack so the `while (!(cr0ack & MASK))` loop exits.
 *   2. GBPA[31] (UPDATE) must clear to 0 when the SMMU has latched the
 *      new global bypass attribute (see SMMU-600 TRM). Firmware writes
 *      SMMU_GBPA_COMPLETE|SHCFG_USEINCOMING and polls the same bit.
 *
 * Everything else is plain read/write-back. The STE / CD / event queue
 * structures themselves live in DRAM (SMMU_STE_BASE_ADDR = 0x14000000
 * etc., see platform_def.h) and are already covered by the chiplet
 * DRAM backing — no MMIO routing needed for them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

/* SMMU-600 register offsets used by TF-A (see struct smmu600_regs). */
#define SMMU_CR0            0x20
#define SMMU_CR0ACK         0x24
#define SMMU_GBPA           0x44
#define SMMU_IRQ_CTRL       0x50
#define SMMU_IRQ_CTRLACK    0x54

/* Bit masks matched against firmware-side SMMU_{EVENTQEN,SMMUEN,CMDQEN}. */
#define SMMU_CR0_SMMUEN     (1u << 0)
#define SMMU_CR0_EVENTQEN   (1u << 2)
#define SMMU_CR0_CMDQEN     (1u << 3)
#define SMMU_CR0_ACK_MASK   (SMMU_CR0_SMMUEN | SMMU_CR0_EVENTQEN | \
                             SMMU_CR0_CMDQEN)

#define SMMU_GBPA_UPDATE    (1u << 31)

static uint64_t r100_smmu_read(void *opaque, hwaddr addr, unsigned size)
{
    R100SMMUState *s = R100_SMMU(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_SMMU_REG_COUNT) {
        return 0;
    }

    switch (addr) {
    case SMMU_CR0ACK:
        return s->regs[SMMU_CR0 >> 2] & SMMU_CR0_ACK_MASK;
    case SMMU_IRQ_CTRLACK:
        return s->regs[SMMU_IRQ_CTRL >> 2];
    default:
        return s->regs[reg_idx];
    }
}

static void r100_smmu_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    R100SMMUState *s = R100_SMMU(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_SMMU_REG_COUNT) {
        return;
    }

    switch (addr) {
    case SMMU_GBPA:
        /*
         * Firmware sets GBPA_UPDATE|SHCFG and then polls GBPA_UPDATE to
         * clear. Latch the UPDATE bit as always-clear so the poll exits
         * immediately.
         */
        s->regs[reg_idx] = (uint32_t)val & ~SMMU_GBPA_UPDATE;
        break;
    default:
        s->regs[reg_idx] = (uint32_t)val;
        break;
    }
}

static const MemoryRegionOps r100_smmu_ops = {
    .read = r100_smmu_read,
    .write = r100_smmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
};

static void r100_smmu_realize(DeviceState *dev, Error **errp)
{
    R100SMMUState *s = R100_SMMU(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-smmu.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_smmu_ops, s, name,
                          R100_SMMU_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_smmu_reset(DeviceState *dev)
{
    R100SMMUState *s = R100_SMMU(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static Property r100_smmu_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100SMMUState, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_smmu_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_smmu_realize;
    device_class_set_legacy_reset(dc, r100_smmu_reset);
    device_class_set_props(dc, r100_smmu_properties);
}

static const TypeInfo r100_smmu_info = {
    .name = TYPE_R100_SMMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100SMMUState),
    .class_init = r100_smmu_class_init,
};

static void r100_smmu_register_types(void)
{
    type_register_static(&r100_smmu_info);
}

type_init(r100_smmu_register_types)
