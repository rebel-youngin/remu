/*
 * REMU - R100 NPU System Emulator
 * PL330 DMA controller stub
 *
 * During BL1, _bl1_init_blk_rbc() calls load_ucie_image() → dma_load_image()
 * to copy UCIe PHY firmware from flash into each RBC's SRAM. The driver
 * (external/ssw-bundle/products/rebel/q/sys/bootloader/cp/tf-a/drivers/arm/pl330/pl330.c) then polls:
 *
 *   - ch_stat[0].csr (offset 0x100): lower 4 bits must be zero → channel idle
 *   - dbgstatus (offset 0xD00): bit 0 zero → debugger idle
 *   - dbgcmd (offset 0xD04): read-back zero after write → command accepted
 *
 * Returning zero on those reads satisfies the polls and lets BL1 proceed.
 * No real data transfer is performed — the RBC stub reports UCIe LTSM=ACTIVE
 * without running PHY microcode, so the DMA destination contents are never
 * consumed.
 *
 * Mapped at R100_DMA_PL330_BASE (0x1FF02C0000) per chiplet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

/* Completion-poll register offsets (from struct pl330 in pl330.h) */
#define PL330_CH0_CSR       0x100   /* ch_stat[0].csr */
#define PL330_DBGSTATUS     0xD00
#define PL330_DBGCMD        0xD04

static uint64_t r100_dma_read(void *opaque, hwaddr addr, unsigned size)
{
    R100DMAPl330State *s = R100_DMA_PL330(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_DMA_REG_COUNT) {
        return 0;
    }

    switch (addr) {
    case PL330_CH0_CSR:
    case PL330_DBGSTATUS:
    case PL330_DBGCMD:
        return 0;
    default:
        return s->regs[reg_idx];
    }
}

static void r100_dma_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100DMAPl330State *s = R100_DMA_PL330(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_DMA_REG_COUNT) {
        return;
    }

    s->regs[reg_idx] = (uint32_t)val;
}

static const MemoryRegionOps r100_dma_ops = {
    .read = r100_dma_read,
    .write = r100_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_dma_realize(DeviceState *dev, Error **errp)
{
    R100DMAPl330State *s = R100_DMA_PL330(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-dma-pl330.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_dma_ops, s,
                          name, R100_DMA_PL330_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_dma_reset(DeviceState *dev)
{
    R100DMAPl330State *s = R100_DMA_PL330(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static Property r100_dma_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100DMAPl330State, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_dma_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_dma_realize;
    device_class_set_legacy_reset(dc, r100_dma_reset);
    device_class_set_props(dc, r100_dma_properties);
}

static const TypeInfo r100_dma_info = {
    .name = TYPE_R100_DMA_PL330,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100DMAPl330State),
    .class_init = r100_dma_class_init,
};

static void r100_dma_register_types(void)
{
    type_register_static(&r100_dma_info);
}

type_init(r100_dma_register_types)
