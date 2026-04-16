/*
 * REMU - R100 NPU System Emulator
 * QEMU machine type: r100-soc
 *
 * Models a 4-chiplet R100 SoC with:
 *   - 32 CA73 vCPUs (4 chiplets x 2 clusters x 4 cores)
 *   - Per-chiplet memory regions (DRAM, iROM, iRAM, SP, SHM)
 *   - Per-chiplet peripheral stubs (CMU, PMU, SYSREG, HBM3)
 *   - GICv3 interrupt controller
 *   - PL011 UART for console output
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/arm/armv7m.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/char/pl011.h"
#include "hw/loader.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "target/arm/cpu.h"
#include "r100_soc.h"

/*
 * Per-chiplet CMU block base addresses.
 * Each CMU block is 64KB. The FW initializes PLLs in these blocks
 * during BL1/BL2 boot and polls for lock status.
 */
static const uint64_t r100_cmu_bases[] = {
    /* Name             Base address */
    0x1FF0200000ULL,  /* ROT CMU */
    0x1FF1000000ULL,  /* CP0 CMU */
    0x1FF1800000ULL,  /* CP1 CMU */
    0x1FF2100000ULL,  /* DCL0 CMU */
    0x1FF2900000ULL,  /* DCL1 CMU */
    0x1FF3000000ULL,  /* NBUS_U CMU */
    0x1FF3300000ULL,  /* NBUS_D CMU */
    0x1FF3500000ULL,  /* NBUS_L CMU */
    0x1FF3A00000ULL,  /* SBUS_U CMU */
    0x1FF3C00000ULL,  /* SBUS_D CMU */
    0x1FF3E00000ULL,  /* SBUS_L CMU */
    0x1FF4000000ULL,  /* WBUS_U CMU */
    0x1FF4600000ULL,  /* WBUS_D CMU */
    0x1FF4800000ULL,  /* EBUS_U CMU */
    0x1FF4A00000ULL,  /* EBUS_D CMU */
    0x1FF4C00000ULL,  /* EBUS_R CMU */
    /* RBC CMU blocks omitted — covered by RBC device models */
    0x1FF7000000ULL,  /* DRAM CMU */
    0x1FF8100000ULL,  /* PCIE CMU */
    0x1FF9000000ULL,  /* PERI0 CMU */
    0x1FF9800000ULL,  /* PERI1 CMU */
};

static const char *r100_cmu_names[] = {
    "ROT", "CP0", "CP1", "DCL0", "DCL1",
    "NBUS_U", "NBUS_D", "NBUS_L",
    "SBUS_U", "SBUS_D", "SBUS_L",
    "WBUS_U", "WBUS_D",
    "EBUS_U", "EBUS_D", "EBUS_R",
    /* RBC CMU names omitted — covered by RBC device models */
    "DRAM", "PCIE",
    "PERI0", "PERI1",
};

#define R100_NUM_CMU_BLOCKS ARRAY_SIZE(r100_cmu_bases)

/*
 * Create and map a single CMU stub device at the given address
 * within a chiplet's address space.
 */
static void r100_create_cmu(MemoryRegion *cfg_mr, int chiplet_id,
                            uint64_t base, const char *name)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    uint64_t offset = base - R100_CFG_BASE;

    dev = qdev_new(TYPE_R100_CMU);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    qdev_prop_set_string(dev, "block-name", name);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(cfg_mr, offset, sysbus_mmio_get_region(sbd, 0));
}

/*
 * Create and map a PMU stub device for a chiplet.
 */
static void r100_create_pmu(MemoryRegion *cfg_mr, int chiplet_id,
                            int secondary_count)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    uint64_t offset = R100_ROT_PMU_BASE - R100_CFG_BASE;

    dev = qdev_new(TYPE_R100_PMU);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    qdev_prop_set_uint32(dev, "secondary-chiplet-count", secondary_count);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(cfg_mr, offset, sysbus_mmio_get_region(sbd, 0));
}

/*
 * Create and map a SYSREG/SYSREMAP stub device for chiplet ID detection.
 */
static void r100_create_sysreg(MemoryRegion *cfg_mr, int chiplet_id)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    uint64_t offset = R100_ROT_SYSREMAP_BASE - R100_CFG_BASE;

    dev = qdev_new(TYPE_R100_SYSREG);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(cfg_mr, offset, sysbus_mmio_get_region(sbd, 0));
}

/*
 * Create and map an HBM3 controller stub.
 */
static void r100_create_hbm(MemoryRegion *cfg_mr, int chiplet_id)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    uint64_t offset = R100_DRAM_CNTL_BASE - R100_CFG_BASE;

    dev = qdev_new(TYPE_R100_HBM);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(cfg_mr, offset, sysbus_mmio_get_region(sbd, 0));
}

/*
 * RBC block base addresses within config space (6 blocks per chiplet).
 */
static const uint64_t r100_rbc_bases[] = {
    0x1FF5000000ULL,  /* RBC_V00 */
    0x1FF5400000ULL,  /* RBC_V01 */
    0x1FF5800000ULL,  /* RBC_V10 */
    0x1FF5C00000ULL,  /* RBC_V11 */
    0x1FF6000000ULL,  /* RBC_H00 */
    0x1FF6400000ULL,  /* RBC_H01 */
};

#define R100_NUM_RBC_BLOCKS ARRAY_SIZE(r100_rbc_bases)

/*
 * Create and map a QSPI bridge device for inter-chiplet communication.
 * Only chiplet 0 (primary) actively uses the bridge, but all chiplets
 * have the QSPI hardware at the PERI1 SPI config addresses.
 */
static void r100_create_qspi_bridge(MemoryRegion *cfg_mr, int chiplet_id)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    /*
     * QSPI bridge channel 2 is at PERI1_SPI2_CFG_BASE (0x1FF9B20000).
     * Map it within the chiplet's config space.
     */
    uint64_t offset = 0x1FF9B20000ULL - R100_CFG_BASE;

    dev = qdev_new(TYPE_R100_QSPI_BRIDGE);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(cfg_mr, offset, sysbus_mmio_get_region(sbd, 0));
}

/*
 * Create and map RBC (UCIe link) stub devices for a chiplet.
 */
static void r100_create_rbc_blocks(MemoryRegion *cfg_mr, int chiplet_id)
{
    int i;

    for (i = 0; i < R100_NUM_RBC_BLOCKS; i++) {
        DeviceState *dev;
        SysBusDevice *sbd;
        uint64_t offset = r100_rbc_bases[i] - R100_CFG_BASE;

        dev = qdev_new(TYPE_R100_RBC);
        qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
        qdev_prop_set_uint32(dev, "block-id", i);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        memory_region_add_subregion_overlap(cfg_mr, offset,
                                            sysbus_mmio_get_region(sbd, 0), 1);
    }
}

/*
 * Initialize a single chiplet: memory regions, CPUs, and peripheral stubs.
 */
static void r100_chiplet_init(MachineState *machine, int chiplet_id,
                              MemoryRegion *sysmem)
{
    uint64_t chiplet_base = (uint64_t)chiplet_id * R100_CHIPLET_OFFSET;
    char name[64];
    MemoryRegion *dram, *irom, *iram, *sp_mem, *sh_mem, *cfg_mr;
    int i;

    /* --- DRAM --- */
    dram = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.dram", chiplet_id);
    memory_region_init_ram(dram, NULL, name, R100_DRAM_INIT_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, chiplet_base + R100_DRAM_BASE, dram);

    /* --- iROM (64KB) --- */
    irom = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.irom", chiplet_id);
    memory_region_init_ram(irom, NULL, name, R100_IROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, chiplet_base + R100_IROM_BASE, irom);

    /* --- iRAM (256KB) --- */
    iram = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.iram", chiplet_id);
    memory_region_init_ram(iram, NULL, name, R100_IRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, chiplet_base + R100_IRAM_BASE, iram);

    /* --- Scratchpad memory (64MB) --- */
    sp_mem = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.sp_mem", chiplet_id);
    memory_region_init_ram(sp_mem, NULL, name, R100_SP_MEM_TOTAL_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, chiplet_base + R100_SP_MEM_BASE,
                                sp_mem);

    /* --- Shared memory (64MB) --- */
    sh_mem = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.sh_mem", chiplet_id);
    memory_region_init_ram(sh_mem, NULL, name, R100_SH_MEM_TOTAL_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, chiplet_base + R100_SH_MEM_BASE,
                                sh_mem);

    /* --- Config space container (covers 0x1FF0000000 - 0x1FFB900000) --- */
    cfg_mr = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.cfg", chiplet_id);
    memory_region_init(cfg_mr, NULL, name, R100_CFG_SIZE);
    memory_region_add_subregion(sysmem, chiplet_base + R100_CFG_BASE, cfg_mr);

    /* --- CMU stubs (one per block) --- */
    for (i = 0; i < R100_NUM_CMU_BLOCKS; i++) {
        r100_create_cmu(cfg_mr, chiplet_id, r100_cmu_bases[i],
                        r100_cmu_names[i]);
    }

    /* --- PMU stub --- */
    r100_create_pmu(cfg_mr, chiplet_id,
                    chiplet_id == 0 ? (R100_NUM_CHIPLETS - 1) : 0);

    /* --- SYSREG (chiplet ID) --- */
    r100_create_sysreg(cfg_mr, chiplet_id);

    /* --- HBM3 controller stub --- */
    r100_create_hbm(cfg_mr, chiplet_id);

    /* --- QSPI bridge (inter-chiplet register access) --- */
    r100_create_qspi_bridge(cfg_mr, chiplet_id);

    /* --- RBC stubs (UCIe link status) --- */
    r100_create_rbc_blocks(cfg_mr, chiplet_id);

    /*
     * TODO Phase 2: Add DNC, HDMA, doorbell/mailbox stubs
     */
}

/*
 * Machine initialization: creates 4 chiplets, 32 CPUs, GIC, UART.
 */
static void r100_soc_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;
    DeviceState *gicdev;
    SysBusDevice *gicbusdev;
    int chiplet, cluster, core, cpu_idx;
    int num_cpus = R100_NUM_CORES_TOTAL;
    char name[64];

    /* Validate CPU count */
    if (machine->smp.cpus != num_cpus && machine->smp.cpus != 0) {
        error_report("R100 SoC requires exactly %d CPUs (got %d)",
                     num_cpus, machine->smp.cpus);
        exit(1);
    }

    /* --- Create CPUs --- */
    for (cpu_idx = 0; cpu_idx < num_cpus; cpu_idx++) {
        chiplet = cpu_idx / R100_NUM_CORES_PER_CHIPLET;
        cluster = (cpu_idx % R100_NUM_CORES_PER_CHIPLET) /
                  R100_NUM_CORES_PER_CLUSTER;
        core = cpu_idx % R100_NUM_CORES_PER_CLUSTER;

        cpuobj = object_new(machine->cpu_type);

        /*
         * Set MPIDR: Aff2=chiplet, Aff1=cluster, Aff0=core
         * This allows the FW to identify which chiplet/cluster/core it is.
         */
        object_property_set_int(cpuobj, "mp-affinity",
                                (chiplet << 16) | (cluster << 8) | core,
                                &error_fatal);

        /* Set reset vector to BL1 base within this chiplet's iRAM */
        object_property_set_int(cpuobj, "rvbar",
                                (uint64_t)chiplet * R100_CHIPLET_OFFSET +
                                R100_BL1_RO_BASE,
                                &error_fatal);

        /* Set timer frequency */
        object_property_set_int(cpuobj, "cntfrq", R100_CORE_TIMER_FREQ,
                                &error_fatal);

        /*
         * Only chiplet 0 / CP0 / core 0 starts powered on.
         * All other cores wait for PMU release (PSCI or direct wakeup).
         */
        if (cpu_idx != 0) {
            object_property_set_bool(cpuobj, "start-powered-off", true,
                                     &error_fatal);
        }

        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    }

    /*
     * --- GICv3 ---
     * We create one GIC for chiplet 0. In a full multi-chiplet model,
     * each chiplet would have its own GIC instance. For Phase 1, we start
     * with one GIC serving all CPUs (simplified).
     */
    gicdev = qdev_new("arm-gicv3");
    qdev_prop_set_uint32(gicdev, "num-cpu", num_cpus);
    qdev_prop_set_uint32(gicdev, "num-irq", 256);
    qdev_prop_set_uint32(gicdev, "revision", 3);
    gicbusdev = SYS_BUS_DEVICE(gicdev);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);

    /* Map GIC distributor at chiplet 0's GIC address */
    sysbus_mmio_map(gicbusdev, 0, R100_GIC_DIST_BASE);
    /* Map GIC redistributor */
    sysbus_mmio_map(gicbusdev, 1, R100_GIC_REDIST_CP0_BASE);

    /* Wire GIC IRQs to CPUs */
    for (cpu_idx = 0; cpu_idx < num_cpus; cpu_idx++) {
        DeviceState *cpu = DEVICE(qemu_get_cpu(cpu_idx));
        int ppibase = num_cpus * 2 + cpu_idx * 6;

        /* IRQ, FIQ, Virtual IRQ, Virtual FIQ, HYP IRQ, HYP FIQ */
        sysbus_connect_irq(gicbusdev, ppibase + 0,
                           qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, ppibase + 1,
                           qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, ppibase + 2,
                           qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, ppibase + 3,
                           qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
    }

    /*
     * --- PL011 UART ---
     * Map at chiplet 0's PERI0_UART0 address for console output.
     */
    pl011_create(R100_PERI0_UART0_BASE,
                 qdev_get_gpio_in(gicdev, 33),  /* SPI 33 */
                 serial_hd(0));

    /* --- Initialize all 4 chiplets --- */
    for (chiplet = 0; chiplet < R100_NUM_CHIPLETS; chiplet++) {
        r100_chiplet_init(machine, chiplet, sysmem);
    }
}

static void r100_soc_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "R100 NPU System Emulator (4 chiplets, 32 CA73 cores)";
    mc->init = r100_soc_init;
    mc->default_cpus = R100_NUM_CORES_TOTAL;
    mc->min_cpus = R100_NUM_CORES_TOTAL;
    mc->max_cpus = R100_NUM_CORES_TOTAL;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a72");
    mc->no_cdrom = true;
    mc->no_floppy = true;
    mc->no_parallel = true;
}

static const TypeInfo r100_soc_machine_info = {
    .name = TYPE_R100_SOC_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = r100_soc_machine_class_init,
};

static void r100_soc_machine_register_types(void)
{
    type_register_static(&r100_soc_machine_info);
}

type_init(r100_soc_machine_register_types)
