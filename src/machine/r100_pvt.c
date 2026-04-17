/*
 * REMU - R100 NPU System Emulator
 * PVT (Process-Voltage-Temperature) controller stub
 *
 * Samsung PVT monitor block. Each chiplet has 5 instances (ROT, DCL0_PVT0,
 * DCL0_PVT1, DCL1_PVT0, DCL1_PVT1) at fixed 64 KB config-space windows —
 * see external/ssw-bundle/products/rebel/q/sys/drivers/pvt_con/pvt_con.c
 * for the base-address table and register-layout headers.
 *
 * FreeRTOS's pvt_init() (driver init level 5) does three things that hit
 * this stub:
 *
 *   1. PVT_ENABLE_{PROC,VOLT,TEMP}_CONTROLLER macros disable the selected
 *      controller and then *unconditionally* spin on the matching idle bit:
 *
 *          while (!PVT_IS_PROC_CON_IDLE(_pvt_regs))
 *              ;
 *
 *      PROC/VOLT/TEMP idle all share PVT_CON_STATUS (offset 0x1C):
 *        bit 0 = ts_con_idle  (shared by VOLT and TEMP)
 *        bit 1 = ps_con_idle  (PROC)
 *      Hardware reset value is 0x3 (both idle). Returning 0x3 unconditionally
 *      satisfies every path.
 *
 *   2. PVT_WAIT_UNTIL_VALID(type, regs, sensor_id, timeout=10000) polls
 *      the per-sensor status register:
 *        VOLT: 0x400 + r*0x40 + 0x0C, bit 0 = vs_valid
 *        TEMP: 0x800 + r*0x80 + 0x40, bit 0 = ts_valid
 *        PROC: 0x2800 + r*0x40 + 0x0C, bit 0 = ps_valid
 *      The loop is timeout-bounded so it wouldn't hang, but without a
 *      stub each sensor burns ~10k MMIO reads (up to ~2.5M total) which
 *      is slow in TCG. Returning 0x1 on these offsets exits on the first
 *      iteration.
 *
 *   3. All the config writes (SAMPLING_INTERVAL, SETUP_TIME_CNT, interrupt
 *      enables, thresholds, ...) are plain read/write-back — FW later reads
 *      back some of these values.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

/* Top-level PVT registers (see struct pvt_regs in pvt_con_reg.h). */
#define PVT_CON_STATUS              0x1C
#define PVT_CON_STATUS_RESET        0x3u  /* ts_con_idle | ps_con_idle */

/* Per-sensor status register windows. Layouts come directly from
 * struct pvt_voltage_regs / pvt_temperature_regs / pvt_process_regs. */
#define PVT_VOLT_BASE               0x400
#define PVT_VOLT_STRIDE             0x40
#define PVT_VOLT_STATUS_OFF         0x0C
#define PVT_VOLT_COUNT              16

#define PVT_TEMP_BASE               0x800
#define PVT_TEMP_STRIDE             0x80
#define PVT_TEMP_STATUS_OFF         0x40
#define PVT_TEMP_COUNT              16

#define PVT_PROC_BASE               0x2800
#define PVT_PROC_STRIDE             0x40
#define PVT_PROC_STATUS_OFF         0x0C
#define PVT_PROC_COUNT              32

static bool pvt_is_sensor_status(hwaddr addr, hwaddr base, hwaddr stride,
                                 hwaddr status_off, unsigned count)
{
    if (addr < base) {
        return false;
    }
    hwaddr rel = addr - base;
    unsigned idx = rel / stride;
    if (idx >= count) {
        return false;
    }
    return (rel % stride) == status_off;
}

static uint64_t r100_pvt_read(void *opaque, hwaddr addr, unsigned size)
{
    R100PVTState *s = R100_PVT(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_PVT_REG_COUNT) {
        return 0;
    }

    if (addr == PVT_CON_STATUS) {
        return PVT_CON_STATUS_RESET;
    }

    /* Per-sensor validity polls short-circuit to "valid". */
    if (pvt_is_sensor_status(addr, PVT_VOLT_BASE, PVT_VOLT_STRIDE,
                             PVT_VOLT_STATUS_OFF, PVT_VOLT_COUNT) ||
        pvt_is_sensor_status(addr, PVT_TEMP_BASE, PVT_TEMP_STRIDE,
                             PVT_TEMP_STATUS_OFF, PVT_TEMP_COUNT) ||
        pvt_is_sensor_status(addr, PVT_PROC_BASE, PVT_PROC_STRIDE,
                             PVT_PROC_STATUS_OFF, PVT_PROC_COUNT)) {
        return 0x1;
    }

    return s->regs[reg_idx];
}

static void r100_pvt_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100PVTState *s = R100_PVT(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_PVT_REG_COUNT) {
        return;
    }

    s->regs[reg_idx] = (uint32_t)val;
}

static const MemoryRegionOps r100_pvt_ops = {
    .read = r100_pvt_read,
    .write = r100_pvt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void r100_pvt_realize(DeviceState *dev, Error **errp)
{
    R100PVTState *s = R100_PVT(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-pvt.cl%u.%s",
             s->chiplet_id, s->name ? s->name : "inst");
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_pvt_ops, s, name,
                          R100_PVT_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_pvt_reset(DeviceState *dev)
{
    R100PVTState *s = R100_PVT(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static Property r100_pvt_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100PVTState, chiplet_id, 0),
    DEFINE_PROP_STRING("name", R100PVTState, name),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_pvt_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_pvt_realize;
    device_class_set_legacy_reset(dc, r100_pvt_reset);
    device_class_set_props(dc, r100_pvt_properties);
}

static const TypeInfo r100_pvt_info = {
    .name = TYPE_R100_PVT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100PVTState),
    .class_init = r100_pvt_class_init,
};

static void r100_pvt_register_types(void)
{
    type_register_static(&r100_pvt_info);
}

type_init(r100_pvt_register_types)
