/*
 * REMU - R100 NPU System Emulator
 * CMU (Clock Management Unit) stub device
 *
 * The real CMU has 14+ blocks per chiplet, each managing PLLs and clock
 * dividers. During boot, the FW (cmu.c) polls these registers:
 *
 *   - pll_con0.stable  (PLL locked) — we always return 1
 *   - pll_con0.mux_busy — we always return 0
 *   - mux_stat.busy — we always return 0
 *
 * All other registers use store-on-write, return-on-read semantics.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "r100_soc.h"

/*
 * CMU PLL register bit positions.
 * These match the Samsung CMU IP used in the R100 SoC.
 *
 * pll_con0 layout (32-bit):
 *   [31]    = mux_sel (0=OSC, 1=PLL)
 *   [30]    = mux_busy (1=mux switching in progress)
 *   [29]    = stable (1=PLL locked)
 *   [25:16] = m_div (multiplier)
 *   [13:8]  = p_div (pre-divider)
 *   [2:0]   = s_div (post-divider)
 */
#define PLL_CON0_MUX_SEL_BIT   (1U << 31)
#define PLL_CON0_MUX_BUSY_BIT  (1U << 30)
#define PLL_CON0_STABLE_BIT    (1U << 29)

/*
 * Heuristic: PLL_CON0 registers are typically at offset 0x100, 0x110, 0x120
 * within a CMU block. However, the exact offsets vary by block. Rather than
 * hard-coding every register, we use a simple rule:
 *
 *   - For ANY read, force the stable bit set and mux_busy/busy clear.
 *
 * This is safe because the FW only cares about these bits when polling,
 * and setting them unconditionally just means "PLLs lock instantly."
 */

static uint64_t r100_cmu_read(void *opaque, hwaddr addr, unsigned size)
{
    R100CMUState *s = R100_CMU(opaque);
    uint32_t reg_idx = addr >> 2;
    uint32_t val;

    if (reg_idx >= R100_CMU_REG_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cmu[%s]: read out of range at 0x%" HWADDR_PRIx "\n",
                      s->name, addr);
        return 0;
    }

    val = s->regs[reg_idx];

    /*
     * Force PLL status bits for all registers:
     *   - Set stable bit (PLL is locked)
     *   - Clear mux_busy bit (mux switch is complete)
     *
     * This makes IS_PLL_LOCKED() and WAIT_MUX_BUSY() in cmu.c
     * return immediately without spinning.
     */
    val |= PLL_CON0_STABLE_BIT;
    val &= ~PLL_CON0_MUX_BUSY_BIT;

    return val;
}

static void r100_cmu_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100CMUState *s = R100_CMU(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_CMU_REG_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cmu[%s]: write out of range at 0x%" HWADDR_PRIx "\n",
                      s->name, addr);
        return;
    }

    /* Store value for read-back consistency */
    s->regs[reg_idx] = (uint32_t)val;
}

static const MemoryRegionOps r100_cmu_ops = {
    .read = r100_cmu_read,
    .write = r100_cmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_cmu_realize(DeviceState *dev, Error **errp)
{
    R100CMUState *s = R100_CMU(dev);
    char region_name[64];

    snprintf(region_name, sizeof(region_name),
             "r100-cmu.cl%u.%s", s->chiplet_id, s->name);

    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_cmu_ops, s,
                          region_name, R100_CMU_BLOCK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_cmu_reset(DeviceState *dev)
{
    R100CMUState *s = R100_CMU(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static Property r100_cmu_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100CMUState, chiplet_id, 0),
    DEFINE_PROP_STRING("block-name", R100CMUState, name),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_cmu_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_cmu_realize;
    device_class_set_legacy_reset(dc, r100_cmu_reset);
    device_class_set_props(dc, r100_cmu_properties);
}

static const TypeInfo r100_cmu_info = {
    .name = TYPE_R100_CMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100CMUState),
    .class_init = r100_cmu_class_init,
};

static void r100_cmu_register_types(void)
{
    type_register_static(&r100_cmu_info);
}

type_init(r100_cmu_register_types)
