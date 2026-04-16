/*
 * REMU - R100 NPU System Emulator
 * RBC (Rebel Bus Controller) / UCIe link stub
 *
 * The R100 SoC has 6 RBC blocks per chiplet (V00/V01/V10/V11/H00/H01),
 * each managing a UCIe die-to-die link between chiplets. During BL1,
 * the FW:
 *   1. Initializes CMU clocks for each RBC block (handled by CMU stubs)
 *   2. Loads UCIe firmware to each block's SRAM
 *   3. Enables the UCIe core and waits for link-up
 *
 * This stub models the RBC SYSREG and UCIe PHY registers. It returns
 * "link up" status for all blocks, allowing the FW to proceed past
 * UCIe link training.
 *
 * Each RBC block has:
 *   - SYSREG at offset 0x010000 (from block base)
 *   - UCIe subsystem at offset 0x020000
 *   - MGR (manager) at offset 0x070000
 *
 * The block spans ~512KB (0x80000) in the address map.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

#define R100_RBC_BLOCK_SIZE     0x80000     /* 512KB per RBC block */
#define R100_RBC_REG_COUNT      (R100_RBC_BLOCK_SIZE / 4)

/*
 * UCIe PHY D2D error log register offsets (from UCIe SS base at +0x20000).
 * These are checked by wait_ucie_link_up() in rebel_h_rbc.c.
 *
 * m0_err_log0 contains the LTSM state in bits [4:0]:
 *   0x10 = ACTIVE (link is up and running)
 */
#define UCIE_PHY_D2D_M0_ERR_LOG0_OFFSET    0x20000  /* approximate */
#define UCIE_LTSM_STATE_ACTIVE              0x10

typedef struct R100RBCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_RBC_REG_COUNT];
    uint32_t chiplet_id;
    uint32_t block_id;      /* 0-5: H00,H01,V00,V01,V10,V11 */
} R100RBCState;

DECLARE_INSTANCE_CHECKER(R100RBCState, R100_RBC, TYPE_R100_RBC)

static uint64_t r100_rbc_read(void *opaque, hwaddr addr, unsigned size)
{
    R100RBCState *s = R100_RBC(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_RBC_REG_COUNT) {
        return 0;
    }

    /*
     * For addresses within the UCIe subsystem range (0x20000+),
     * return "link active" state in the LTSM state field.
     * This satisfies check_link_up() in the FW.
     */
    if (addr >= 0x20000 && addr < 0x40000) {
        uint32_t val = s->regs[reg_idx];
        /* Set LTSM state to ACTIVE in the lower bits */
        val |= UCIE_LTSM_STATE_ACTIVE;
        return val;
    }

    return s->regs[reg_idx];
}

static void r100_rbc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100RBCState *s = R100_RBC(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_RBC_REG_COUNT) {
        return;
    }

    s->regs[reg_idx] = (uint32_t)val;
}

static const MemoryRegionOps r100_rbc_ops = {
    .read = r100_rbc_read,
    .write = r100_rbc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_rbc_realize(DeviceState *dev, Error **errp)
{
    R100RBCState *s = R100_RBC(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-rbc.cl%u.blk%u",
             s->chiplet_id, s->block_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_rbc_ops, s,
                          name, R100_RBC_BLOCK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_rbc_reset(DeviceState *dev)
{
    R100RBCState *s = R100_RBC(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static Property r100_rbc_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100RBCState, chiplet_id, 0),
    DEFINE_PROP_UINT32("block-id", R100RBCState, block_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_rbc_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_rbc_realize;
    dc->reset = r100_rbc_reset;
    device_class_set_props(dc, r100_rbc_properties);
}

static const TypeInfo r100_rbc_info = {
    .name = TYPE_R100_RBC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100RBCState),
    .class_init = r100_rbc_class_init,
};

static void r100_rbc_register_types(void)
{
    type_register_static(&r100_rbc_info);
}

type_init(r100_rbc_register_types)
