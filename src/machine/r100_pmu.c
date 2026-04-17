/*
 * REMU - R100 NPU System Emulator
 * PMU (Power Management Unit) stub device
 *
 * The PMU tracks boot status, manages power domains, and provides
 * chiplet reset control. During boot, the FW reads:
 *
 *   - RST_STAT (0x0404): reset type (cold vs chiplet reset)
 *   - INFORM4 (0x0840): secondary chiplet count (on primary) / boot config
 *   - CPU0_STATUS (0x2004): CPU power status (must return ON = 0xF)
 *   - CP0_NONCPU_STATUS (0x2404): cluster power status
 *   - CP0_L2_STATUS (0x2604): L2 cache power status
 *   - DCL0_STATUS (0x4284): D-Cluster power status
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

static void r100_pmu_set_defaults(R100PMUState *s)
{
    /*
     * Boot-mode register at PMU offset 0. FW reads (val & OM_MASK) and
     * compares to NORMAL_BOOT (0x5) — see rebel_h_bootmgr.h:BOOT_MODE_VAL.
     */
    s->regs[R100_PMU_OM_STAT >> 2] = R100_PMU_BOOT_MODE_NORMAL;

    /* Cold reset */
    s->regs[R100_PMU_RST_STAT >> 2] = R100_PMU_COLD_RESET;

    /*
     * INFORM4: on chiplet 0, this holds the number of secondary chiplets.
     * On secondary chiplets, this can hold the chiplet's own ID.
     */
    if (s->chiplet_id == 0) {
        s->regs[R100_PMU_INFORM4 >> 2] = s->secondary_chiplet_count;
    } else {
        s->regs[R100_PMU_INFORM4 >> 2] = s->chiplet_id;
    }

    /* All CPU cores powered on */
    for (int i = 0; i < R100_NUM_CORES_PER_CLUSTER; i++) {
        uint32_t off = R100_PMU_CPU0_STATUS + i * R100_PMU_PERCPU_OFFSET;
        if ((off >> 2) < R100_PMU_REG_COUNT) {
            s->regs[off >> 2] = R100_PMU_CPU_STATUS_ON;
        }
    }

    /* Cluster power on */
    s->regs[R100_PMU_CP0_NONCPU_STATUS >> 2] = R100_PMU_CL_STATUS_ON;

    /* L2 cache on for both CP0 and CP1 */
    s->regs[R100_PMU_CP0_L2_STATUS >> 2] = R100_PMU_L2_STATUS_ON;
    s->regs[R100_PMU_CP1_L2_STATUS >> 2] = R100_PMU_L2_STATUS_ON;

    /* D-Clusters on */
    s->regs[R100_PMU_DCL0_STATUS >> 2] = R100_PMU_DCL_STATUS_ON;
    s->regs[R100_PMU_DCL1_STATUS >> 2] = R100_PMU_DCL_STATUS_ON;

    /* All RBC blocks powered on (FW polls these in pmu_enable_blk_rbc). */
    s->regs[R100_PMU_RBCH00_STATUS >> 2] = R100_PMU_LOCAL_PWR_ON;
    s->regs[R100_PMU_RBCH01_STATUS >> 2] = R100_PMU_LOCAL_PWR_ON;
    s->regs[R100_PMU_RBCV00_STATUS >> 2] = R100_PMU_LOCAL_PWR_ON;
    s->regs[R100_PMU_RBCV01_STATUS >> 2] = R100_PMU_LOCAL_PWR_ON;
    s->regs[R100_PMU_RBCV10_STATUS >> 2] = R100_PMU_LOCAL_PWR_ON;
    s->regs[R100_PMU_RBCV11_STATUS >> 2] = R100_PMU_LOCAL_PWR_ON;

    /* CPU configurations: set to powered on with automatic wakeup */
    for (int i = 0; i < R100_NUM_CORES_PER_CLUSTER; i++) {
        uint32_t off = R100_PMU_CPU0_CONFIGURATION + i * R100_PMU_PERCPU_OFFSET;
        if ((off >> 2) < R100_PMU_REG_COUNT) {
            s->regs[off >> 2] = R100_PMU_LOCAL_PWR_ON | (1U << 31);
        }
    }

    /* Cluster configuration: powered on */
    s->regs[R100_PMU_CP0_NONCPU_CONFIG >> 2] = R100_PMU_LOCAL_PWR_ON;
    s->regs[R100_PMU_DCL0_CONFIG >> 2] = R100_PMU_LOCAL_PWR_ON;
    s->regs[R100_PMU_DCL1_CONFIG >> 2] = R100_PMU_LOCAL_PWR_ON;
}

static uint64_t r100_pmu_read(void *opaque, hwaddr addr, unsigned size)
{
    R100PMUState *s = R100_PMU(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_PMU_REG_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pmu[cl%u]: read out of range 0x%" HWADDR_PRIx "\n",
                      s->chiplet_id, addr);
        return 0;
    }

    return s->regs[reg_idx];
}

static void r100_pmu_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100PMUState *s = R100_PMU(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_PMU_REG_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pmu[cl%u]: write out of range 0x%" HWADDR_PRIx "\n",
                      s->chiplet_id, addr);
        return;
    }

    s->regs[reg_idx] = (uint32_t)val;

    /*
     * When a CPU_CONFIGURATION register is written with a power-on value,
     * the corresponding CPU should be released from reset. This is how
     * BL31 brings up secondary cores.
     *
     * TODO: Implement PSCI / secondary core release via CPU status writes.
     */
}

static const MemoryRegionOps r100_pmu_ops = {
    .read = r100_pmu_read,
    .write = r100_pmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_pmu_realize(DeviceState *dev, Error **errp)
{
    R100PMUState *s = R100_PMU(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-pmu.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_pmu_ops, s,
                          name, R100_PMU_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_pmu_reset(DeviceState *dev)
{
    R100PMUState *s = R100_PMU(dev);
    memset(s->regs, 0, sizeof(s->regs));
    r100_pmu_set_defaults(s);
}

static Property r100_pmu_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100PMUState, chiplet_id, 0),
    DEFINE_PROP_UINT32("secondary-chiplet-count", R100PMUState,
                       secondary_chiplet_count, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_pmu_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_pmu_realize;
    device_class_set_legacy_reset(dc, r100_pmu_reset);
    device_class_set_props(dc, r100_pmu_properties);
}

static const TypeInfo r100_pmu_info = {
    .name = TYPE_R100_PMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100PMUState),
    .class_init = r100_pmu_class_init,
};

static void r100_pmu_register_types(void)
{
    type_register_static(&r100_pmu_info);
}

type_init(r100_pmu_register_types)
