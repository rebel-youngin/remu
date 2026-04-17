/*
 * REMU - R100 NPU System Emulator
 * RBC (Rebel Bus Controller) / UCIe link stub
 *
 * The R100 SoC has 6 RBC blocks per chiplet (V00/V01/V10/V11/H00/H01),
 * each managing a UCIe die-to-die link between chiplets. During BL1/BL2,
 * the FW:
 *   1. Initializes CMU clocks for each RBC block (handled by CMU stubs)
 *   2. Loads UCIe firmware to each block's SRAM
 *   3. Enables the UCIe core and waits for link-up (BL2)
 *
 * This stub models the RBC SYSREG and UCIe PHY registers. It returns
 * "link up" status for all blocks, allowing the FW to proceed past
 * UCIe link training.
 *
 * Each RBC block has:
 *   - CMU at offset 0x000000 (from block base)
 *   - SYSREG at offset 0x010000
 *   - UCIe subsystem (SS) at offset 0x020000
 *   - RBCM (manager) at offset 0x070000
 *
 * The block spans 512KB (0x80000) in the address map.
 *
 * UCIe link-up handshake
 * ----------------------
 * BL2's wait_ucie_link_up_for_CP() polls check_link_up() until success. The
 * ZEBU-build FW (what `remu build` produces via `-p zebu`) checks:
 *
 *   target_ss->global_reg_cmn_mcu_scratch_reg1 == 0xFFFFFFFF
 *
 * i.e. UCIe SS offset 0x2e038 (block offset 0x4e038). See
 *   external/ssw-bundle/.../tf-a/drivers/aw/ucie.c::check_link_up()
 *
 * The non-ZEBU path instead checks
 *   target_ss->dvsec1_reg_reg_global_dvsec1_ucie_link_status.lstatus_link_status
 * at UCIe SS offset 0x20014 bit[15]. We seed both so either build works.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

/* R100_RBC_BLOCK_SIZE defined in r100_soc.h (shared with r100_soc.c). */
#define R100_RBC_REG_COUNT      (R100_RBC_BLOCK_SIZE / 4)

/*
 * UCIe SS occupies [0x20000, 0x70000) within the RBC block.
 * RBCM starts at 0x70000.
 */
#define R100_RBC_UCIE_SS_BASE   0x20000
#define R100_RBC_UCIE_SS_END    0x70000

/*
 * UCIe SS register offsets (from UCIe SS base, i.e. block offset - 0x20000).
 *
 * LINK_STATUS: dvsec1_reg_reg_global_dvsec1_ucie_link_status (offset 0x20014).
 *   bit[15] = lstatus_link_status; used by non-ZEBU check_link_up().
 *
 * SCRATCH_REG1: global_reg_cmn_mcu_scratch_reg1 (offset 0x2e038).
 *   Read as 0xFFFFFFFF to signal link-up in ZEBU builds.
 */
#define UCIE_REG_LINK_STATUS    0x20014
#define UCIE_REG_SCRATCH_REG1   0x2e038
#define UCIE_LINK_STATUS_UP     (1u << 15)
#define UCIE_SCRATCH_LINKUP     0xFFFFFFFFu

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
     * Within the UCIe subsystem [0x20000, 0x70000), spoof link-up for the
     * registers BL2 polls in check_link_up().
     */
    if (addr >= R100_RBC_UCIE_SS_BASE && addr < R100_RBC_UCIE_SS_END) {
        hwaddr ucie_off = addr - R100_RBC_UCIE_SS_BASE;

        switch (ucie_off) {
        case UCIE_REG_SCRATCH_REG1:
            qemu_log_mask(LOG_UNIMP,
                          "r100-rbc: cl%u blk%u SCRATCH_REG1 read "
                          "(off=0x%"HWADDR_PRIx") -> 0x%x\n",
                          s->chiplet_id, s->block_id, addr,
                          UCIE_SCRATCH_LINKUP);
            return UCIE_SCRATCH_LINKUP;
        case UCIE_REG_LINK_STATUS:
            qemu_log_mask(LOG_UNIMP,
                          "r100-rbc: cl%u blk%u LINK_STATUS read "
                          "(off=0x%"HWADDR_PRIx") -> 0x%x\n",
                          s->chiplet_id, s->block_id, addr,
                          s->regs[reg_idx] | UCIE_LINK_STATUS_UP);
            return s->regs[reg_idx] | UCIE_LINK_STATUS_UP;
        default:
            return s->regs[reg_idx];
        }
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
    device_class_set_legacy_reset(dc, r100_rbc_reset);
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
