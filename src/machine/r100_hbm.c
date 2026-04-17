/*
 * REMU - R100 NPU System Emulator
 * HBM3 memory controller stub device
 *
 * Covers the full HBM3 controller window (16x CON, 16x PHY, ICON),
 * returning 0xFFFFFFFF for any unwritten offset so the FW's DFI/PHY
 * training "complete" polls succeed on every channel. Written values
 * are kept in a sparse hash so read-modify-write sequences work.
 *
 * A few registers need custom behaviour:
 *   - ICON test_instruction_req0 (0x5F0008): default 0 (no outstanding
 *     request) so the FW's `req0 |= (1<<bit)` / `req0 &= ~(1<<bit)`
 *     RMW sequence cleanly transitions between non-zero and zero.
 *   - ICON test_instruction_req1.test_done (bit 5 of 0x5F000C) is
 *     synthesised from req0: non-zero req0 → test_done=1, req0==0 →
 *     test_done=0. See drivers/hbm3/icon.c:icon_test_instruction_req()
 *     for the exact FW pattern.
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

#define HBM_STATUS_READY            0xFFFFFFFFU

/* Offsets relative to the HBM stub base (0x1FF7400000). */
#define HBM_ICON_OFFSET             0x5F0000U     /* 0x1FF79F0000 */
#define HBM_ICON_CTRL_CON0_OFFSET   (HBM_ICON_OFFSET + 0x0U)
#define HBM_ICON_REQ0_OFFSET        (HBM_ICON_OFFSET + 0x8U)
#define HBM_ICON_REQ1_OFFSET        (HBM_ICON_OFFSET + 0xCU)
#define HBM_ICON_REQ1_TEST_DONE     (1U << 5)
#define HBM_ICON_CTRL_SW_MODE_REQ   (1U << 29)

static bool r100_hbm_lookup(R100HBMState *s, hwaddr addr, uint32_t *out)
{
    gpointer val;

    if (!s->regs) {
        return false;
    }
    if (!g_hash_table_lookup_extended(s->regs,
                                      GUINT_TO_POINTER((guint)addr),
                                      NULL, &val)) {
        return false;
    }
    *out = (uint32_t)GPOINTER_TO_UINT(val);
    return true;
}

static uint64_t r100_hbm_read(void *opaque, hwaddr addr, unsigned size)
{
    R100HBMState *s = R100_HBM(opaque);
    uint32_t stored;
    uint64_t ret;

    /*
     * ICON icon_ctrl_con0 and test_instruction_req0 default to 0 so
     * the FW's RMW `|=` / `&=~` cycle transitions cleanly between
     * non-zero and zero values. Any explicit write is preserved.
     */
    if (addr == HBM_ICON_CTRL_CON0_OFFSET ||
        addr == HBM_ICON_REQ0_OFFSET) {
        ret = r100_hbm_lookup(s, addr, &stored) ? stored : 0;
        return ret;
    }

    /*
     * Synthesise test_done (bit 5 of test_instruction_req1) based on
     * whether any request is active: either a bit in req0 is set, or
     * icon_ctrl_con0.sw_mode_req (bit 29) is set. Firmware uses both
     * paths — see drivers/hbm3/icon.c icon_test_instruction_req() /
     * icon_sw_instruction_req(). Other bits of req1 (ch_select, etc.)
     * keep whatever was last written.
     */
    if (addr == HBM_ICON_REQ1_OFFSET) {
        uint32_t req0 = 0;
        uint32_t ctrl = 0;
        uint32_t req1 = HBM_STATUS_READY;
        bool active;

        (void)r100_hbm_lookup(s, HBM_ICON_REQ0_OFFSET, &req0);
        (void)r100_hbm_lookup(s, HBM_ICON_CTRL_CON0_OFFSET, &ctrl);
        (void)r100_hbm_lookup(s, HBM_ICON_REQ1_OFFSET, &req1);
        active = (req0 != 0) || ((ctrl & HBM_ICON_CTRL_SW_MODE_REQ) != 0);
        if (active) {
            req1 |= HBM_ICON_REQ1_TEST_DONE;
        } else {
            req1 &= ~HBM_ICON_REQ1_TEST_DONE;
        }
        qemu_log_mask(LOG_UNIMP,
                      "r100-hbm.cl%u: ICON req1 read (req0=0x%x ctrl=0x%x)"
                      " -> 0x%x\n",
                      s->chiplet_id, req0, ctrl, req1);
        return req1;
    }

    if (r100_hbm_lookup(s, addr, &stored)) {
        return stored;
    }
    return HBM_STATUS_READY;
}

static void r100_hbm_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100HBMState *s = R100_HBM(opaque);

    if (addr == HBM_ICON_CTRL_CON0_OFFSET ||
        addr == HBM_ICON_REQ0_OFFSET ||
        addr == HBM_ICON_REQ1_OFFSET) {
        const char *name = addr == HBM_ICON_CTRL_CON0_OFFSET ? "ctrl_con0"
                         : addr == HBM_ICON_REQ0_OFFSET      ? "req0"
                                                             : "req1";
        qemu_log_mask(LOG_UNIMP,
                      "r100-hbm.cl%u: ICON %s write 0x%"PRIx64"\n",
                      s->chiplet_id, name, val);
    }
    g_hash_table_insert(s->regs, GUINT_TO_POINTER((guint)addr),
                        GUINT_TO_POINTER((guint)(uint32_t)val));
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

    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
    snprintf(name, sizeof(name), "r100-hbm.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_hbm_ops, s,
                          name, R100_HBM_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_hbm_reset(DeviceState *dev)
{
    R100HBMState *s = R100_HBM(dev);

    if (s->regs) {
        g_hash_table_remove_all(s->regs);
    }
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
