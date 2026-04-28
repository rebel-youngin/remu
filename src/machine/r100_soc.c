/*
 * REMU - R100 NPU machine (r100-soc). 4 chiplets × 8 CA73 + per-chiplet
 * memory / peripheral / GICv3 / 16550 UART. See docs/architecture.md
 * for the full component list.
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
#include "hw/arm/bsa.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/char/pl011.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "hw/qdev-properties-system.h"
#include "hw/loader.h"
#include "sysemu/sysemu.h"
#include "sysemu/hostmem.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "exec/address-spaces.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "target/arm/gtimer.h"
#include "qapi/qmp/qlist.h"
#include "r100_soc.h"

static HostMemoryBackend *r100_soc_resolve_memdev(R100SoCMachineState *r100m);
static Chardev *r100_soc_resolve_chr(const char *id, const char *role);

/*
 * Samsung IMPDEF cache-flush encoding used by TF-A/BL31 cache-maintenance
 * and the SMMU driver: MSR S1_1_C15_C14_0, Xt. QEMU's decoder UNDEFs on it,
 * so BL31 faults right after enable_mmu_direct_el3_bl31. Modelled as WO-NOP
 * (cache flush is a no-op without a cache model anyway).
 *   tf-a/lib/xlat_tables_v2/aarch64/enable_mmu.S
 *   tf-a/drivers/smmu/smmu.c (smmu_flush_all_cache / smmu_flush_l2_cache)
 */
static const ARMCPRegInfo r100_samsung_impdef_regs[] = {
    { .name = "S1_1_C15_C14_0_CACHE_INV",
      .state = ARM_CP_STATE_AA64,
      .opc0 = 1, .opc1 = 1, .crn = 15, .crm = 14, .opc2 = 0,
      .access = PL1_W, .type = ARM_CP_NOP },
};

/*
 * Cortex-A73 IMPDEF sysregs at op1=0/CRn=15/CRm=0 that cortex-a72 lacks
 * (TRM §4.5). Needed by TF-A CA73 CVE-2018-3639 WA (erratum_cortex_a73_3639_wa
 * unconditionally sets CPUACTLR_EL1 bit 3) and cpu_get_rev_var helpers.
 * Modelled RAZ/WI — behaviourally correct without a cache model. See commit
 * 7a2e232 for the full encoding / rationale; extend this table if a new
 * FW release probes a new IMPDEF encoding.
 */
static const ARMCPRegInfo r100_cortex_a73_impdef_regs[] = {
    { .name = "A73_CPUACTLR_EL1",  .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 0, .opc2 = 0,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "A73_CPUECTLR_EL1",  .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 0, .opc2 = 1,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "A73_CPUMERRSR_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 0, .opc2 = 2,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
};

/* Per-chiplet CMU block bases (64 KB each). FW inits PLLs and polls lock. */
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
 * PMU stub: dual-mapped at R100_ROT_PMU_BASE (cfg space, for BL2/BL31 QSPI
 * cross-chiplet access) and R100_CPMU_PRIVATE_BASE (private alias, for BL1
 * boot-mode/log/flag).
 */
static void r100_create_pmu(MemoryRegion *cfg_mr, MemoryRegion *sysmem,
                            int chiplet_id, int secondary_count,
                            uint64_t chiplet_base)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    MemoryRegion *iomem;
    MemoryRegion *alias;
    uint64_t cfg_offset = R100_ROT_PMU_BASE - R100_CFG_BASE;
    char name[64];

    dev = qdev_new(TYPE_R100_PMU);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    qdev_prop_set_uint32(dev, "secondary-chiplet-count", secondary_count);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);

    iomem = sysbus_mmio_get_region(sbd, 0);
    memory_region_add_subregion(cfg_mr, cfg_offset, iomem);

    /* Private alias at 0x1E00230000 + chiplet_base, priority 0 overlap to
     * beat the lower-priority private-window catch-all. */
    alias = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.pmu_priv", chiplet_id);
    memory_region_init_alias(alias, NULL, name, iomem, 0, R100_SFR_SIZE);
    memory_region_add_subregion_overlap(sysmem,
                                        chiplet_base + R100_CPMU_PRIVATE_BASE,
                                        alias, 0);
}

/* SYSREG/SYSREMAP for chiplet-ID detection. Same dual-mapping as PMU:
 * private alias for BL1's own-chiplet read, cfg space for QSPI cross-chiplet. */
static void r100_create_sysreg(MemoryRegion *cfg_mr, MemoryRegion *sysmem,
                               int chiplet_id, uint64_t chiplet_base)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    MemoryRegion *iomem;
    MemoryRegion *alias;
    uint64_t cfg_offset = R100_ROT_SYSREMAP_BASE - R100_CFG_BASE;
    char name[64];

    dev = qdev_new(TYPE_R100_SYSREG);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);

    iomem = sysbus_mmio_get_region(sbd, 0);
    memory_region_add_subregion(cfg_mr, cfg_offset, iomem);

    alias = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.sysremap_priv", chiplet_id);
    memory_region_init_alias(alias, NULL, name, iomem, 0,
                             R100_SFR_SIZE);
    memory_region_add_subregion_overlap(sysmem,
                                        chiplet_base + R100_SYSREMAP_PRIVATE_BASE,
                                        alias, 0);
}

/*
 * PCIe sub-controller stub RAM. pmu_release_cm7() only writes registers;
 * BL1 then polls four PHY{0..3}_SRAM_INIT_DONE bit [0] (cm7_pcie_common.c).
 * REMU doesn't run CM7 FW, so we seed bit [0] at reset.
 */
static void r100_create_pcie_subctrl(MemoryRegion *cfg_mr, int chiplet_id)
{
    /* PHY{0..3}_SRAM_INIT_DONE offsets — cm7_pcie_common.h PCIE_PHY{0..3}_CFG_REG. */
    static const uint32_t phy_sram_init_done_off[] = {
        0x215C, /* PHY0 */
        0x112C, /* PHY1 */
        0x1130, /* PHY2 */
        0x1134, /* PHY3 */
    };
    MemoryRegion *mr = g_new(MemoryRegion, 1);
    uint64_t offset = R100_PCIE_SUBCTRL_BASE - R100_CFG_BASE;
    char name[64];
    uint32_t *ram;

    snprintf(name, sizeof(name), "r100.chiplet%d.pcie_subctrl", chiplet_id);
    memory_region_init_ram(mr, NULL, name, R100_PCIE_SUBCTRL_SIZE,
                           &error_fatal);
    memory_region_add_subregion(cfg_mr, offset, mr);

    ram = memory_region_get_ram_ptr(mr);
    for (size_t i = 0; i < ARRAY_SIZE(phy_sram_init_done_off); i++) {
        ram[phy_sram_init_done_off[i] / sizeof(uint32_t)] = 0x00000001;
    }
}

/* PL330 DMA controller stub. BL1 _bl1_init_blk_rbc() fake-loads UCIe PHY FW;
 * RBC stub reports link-up without PHY microcode. */
static void r100_create_dma_pl330(MemoryRegion *cfg_mr, int chiplet_id)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    uint64_t offset = R100_DMA_PL330_BASE - R100_CFG_BASE;

    dev = qdev_new(TYPE_R100_DMA_PL330);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(cfg_mr, offset, sysbus_mmio_get_region(sbd, 0));
}

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

/* PVT monitor bases, 5 per chiplet (pvt_base_address[] in pvt_con.c). */
static const struct {
    uint64_t base;
    const char *name;
} r100_pvt_blocks[] = {
    { R100_ROT_PVT_CON_BASE,   "ROT" },
    { R100_DCL0_PVT_CON0_BASE, "DCL0_0" },
    { R100_DCL0_PVT_CON1_BASE, "DCL0_1" },
    { R100_DCL1_PVT_CON0_BASE, "DCL1_0" },
    { R100_DCL1_PVT_CON1_BASE, "DCL1_1" },
};

#define R100_NUM_PVT_BLOCKS ARRAY_SIZE(r100_pvt_blocks)

/* PVT monitor stubs. FreeRTOS pvt_init() polls PVT_CON_STATUS+0x1C idle
 * bits and per-sensor validity — FW hangs in PVT_ENABLE_PROC_CONTROLLER
 * without. Defaults set in r100_pvt.c. */
static void r100_create_pvt_blocks(MemoryRegion *cfg_mr, int chiplet_id)
{
    int i;

    for (i = 0; i < R100_NUM_PVT_BLOCKS; i++) {
        DeviceState *dev;
        SysBusDevice *sbd;
        uint64_t offset = r100_pvt_blocks[i].base - R100_CFG_BASE;

        dev = qdev_new(TYPE_R100_PVT);
        qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
        qdev_prop_set_string(dev, "name", r100_pvt_blocks[i].name);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        memory_region_add_subregion(cfg_mr, offset,
                                    sysbus_mmio_get_region(sbd, 0));
    }
}

/* SMMU-600 TCU stub. BL2 smmu_early_init polls CR0ACK&EVENTQEN after
 * programming CR0, and GBPA UPDATE self-clear. Details in r100_smmu.c. */
static void r100_create_smmu(MemoryRegion *cfg_mr, int chiplet_id)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    uint64_t offset = R100_SMMU_TCU_BASE - R100_CFG_BASE;

    dev = qdev_new(TYPE_R100_SMMU);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(cfg_mr, offset, sysbus_mmio_get_region(sbd, 0));
}

/* RBC block bases in cfg space (6 per chiplet). */
static const uint64_t r100_rbc_bases[] = {
    0x1FF5000000ULL,  /* RBC_V00 */
    0x1FF5400000ULL,  /* RBC_V01 */
    0x1FF5800000ULL,  /* RBC_V10 */
    0x1FF5C00000ULL,  /* RBC_V11 */
    0x1FF6000000ULL,  /* RBC_H00 */
    0x1FF6400000ULL,  /* RBC_H01 */
};

#define R100_NUM_RBC_BLOCKS ARRAY_SIZE(r100_rbc_bases)

/* QSPI bridge: inter-chiplet register access. Primary drives, but every
 * chiplet has the HW block at PERI1_SPI2_CFG_BASE (ch 2). */
static void r100_create_qspi_bridge(MemoryRegion *cfg_mr, int chiplet_id)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    uint64_t offset = 0x1FF9B20000ULL - R100_CFG_BASE;  /* PERI1_SPI2_CFG */

    dev = qdev_new(TYPE_R100_QSPI_BRIDGE);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(cfg_mr, offset, sysbus_mmio_get_region(sbd, 0));
}

/* DWC_SSI master QSPI (QSPI_ROT). Serves BL1 qspi_boot_config /
 * bl1_load_fip_image and BL31 NOR_FLASH_SVC_* SMCs (qspi_boot.c reg_base
 * = QSPI_ROT_PRIVATE 0x1E00500000). Dual-mapped same as PMU/SYSREG. */
static void r100_create_qspi_boot(MemoryRegion *cfg_mr, MemoryRegion *sysmem,
                                  int chiplet_id, uint64_t chiplet_base)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    MemoryRegion *iomem;
    MemoryRegion *alias;
    uint64_t cfg_offset = R100_QSPI_ROT_BASE - R100_CFG_BASE;
    char name[64];

    dev = qdev_new(TYPE_R100_QSPI_BOOT);
    qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);

    iomem = sysbus_mmio_get_region(sbd, 0);
    memory_region_add_subregion(cfg_mr, cfg_offset, iomem);

    alias = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.qspi_boot_priv", chiplet_id);
    memory_region_init_alias(alias, NULL, name, iomem, 0,
                             R100_QSPI_ROT_REG_SIZE);
    memory_region_add_subregion_overlap(sysmem,
                                        chiplet_base + R100_QSPI_ROT_PRIVATE_BASE,
                                        alias, 1);
}

/*
 * RBC (UCIe link) stubs. Dual-mapped cfg_space (0x1FF5.../0x1FF6... for
 * QSPI cross-chiplet) + private alias (0x1E05.../0x1E06... for own-chiplet
 * BL2 wait_ucie_link_up_for_CP). Offsets from their respective bases
 * match, so private_base = PRIVATE_BASE + (cfg_base - CFG_BASE).
 */
static void r100_create_rbc_blocks(MemoryRegion *cfg_mr, MemoryRegion *sysmem,
                                   int chiplet_id, uint64_t chiplet_base)
{
    int i;

    for (i = 0; i < R100_NUM_RBC_BLOCKS; i++) {
        DeviceState *dev;
        SysBusDevice *sbd;
        MemoryRegion *iomem;
        MemoryRegion *alias;
        uint64_t cfg_offset = r100_rbc_bases[i] - R100_CFG_BASE;
        uint64_t priv_base = R100_PRIVATE_BASE + cfg_offset;
        char name[64];

        dev = qdev_new(TYPE_R100_RBC);
        qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
        qdev_prop_set_uint32(dev, "block-id", i);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);

        iomem = sysbus_mmio_get_region(sbd, 0);
        memory_region_add_subregion_overlap(cfg_mr, cfg_offset, iomem, 1);

        /* Private-alias mount: prio 1 beats the private-window unimpl catch-all (prio 0). */
        alias = g_new(MemoryRegion, 1);
        snprintf(name, sizeof(name), "r100.chiplet%d.rbc%d_priv",
                 chiplet_id, i);
        memory_region_init_alias(alias, NULL, name, iomem, 0,
                                 R100_RBC_BLOCK_SIZE);
        memory_region_add_subregion_overlap(sysmem,
                                            chiplet_base + priv_base,
                                            alias, 1);
    }
}

/*
 * QSPI NOR flash staging region. Single board-level flash aliased into each
 * chiplet's local flash window (FLASH_BASE_ADDR = 0x1F80000000). FW's
 * flash_nor_read() memcpy's directly; BL1 print_ucie_link_speed reads a
 * HW-CFG struct at +0x5E000. Zero-fill lets BL1 fall through to
 * "UCIe speed: default"; preload a real image with
 * `-device loader,file=...,addr=0x1F80000000` if needed.
 */
static void r100_create_flash(MemoryRegion *sysmem)
{
    MemoryRegion *flash = g_new(MemoryRegion, 1);
    int chiplet_id;

    memory_region_init_ram(flash, NULL, "r100.flash", R100_FLASH_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, R100_FLASH_BASE, flash);

    /* Secondary chiplets alias the same backing store. */
    for (chiplet_id = 1; chiplet_id < R100_NUM_CHIPLETS; chiplet_id++) {
        MemoryRegion *alias = g_new(MemoryRegion, 1);
        char name[64];
        uint64_t base = (uint64_t)chiplet_id * R100_CHIPLET_OFFSET +
                        R100_FLASH_BASE;

        snprintf(name, sizeof(name), "r100.chiplet%d.flash_alias", chiplet_id);
        memory_region_init_alias(alias, NULL, name, flash, 0, R100_FLASH_SIZE);
        memory_region_add_subregion(sysmem, base, alias);
    }
}

/*
 * Init a chiplet: memory regions + peripheral stubs. `memdev` (chiplet 0
 * only) splices a shared backend over DRAM head for host/NPU sharing —
 * M5, commit 72c98f0. `gic_dev` is the chiplet's own GICv3 — already
 * realized by r100_soc_init, used here to wire per-DCL DNC completion
 * SPIs (M9-1c) into q-cp's CP1.cpu0.
 */
static void r100_chiplet_init(MachineState *machine, int chiplet_id,
                              MemoryRegion *sysmem,
                              HostMemoryBackend *memdev,
                              DeviceState *gic_dev)
{
    uint64_t chiplet_base = (uint64_t)chiplet_id * R100_CHIPLET_OFFSET;
    char name[64];
    MemoryRegion *dram, *irom, *iram, *sp_mem, *sh_mem, *cfg_mr;
    int i;

    /* DRAM: chiplet 0+memdev → container (shared @ 0, lazy tail); else plain RAM. */
    dram = g_new(MemoryRegion, 1);
    if (chiplet_id == 0 && memdev != NULL) {
        MemoryRegion *shared = host_memory_backend_get_memory(memdev);
        uint64_t shared_size = memory_region_size(shared);

        if (shared_size == 0 || shared_size > R100_DRAM_INIT_SIZE) {
            error_report("r100-soc: memdev size 0x%" PRIx64
                         " is out of range for chiplet 0 DRAM (max 0x%"
                         PRIx64 ")",
                         shared_size, (uint64_t)R100_DRAM_INIT_SIZE);
            exit(1);
        }

        snprintf(name, sizeof(name), "r100.chiplet0.dram");
        memory_region_init(dram, NULL, name, R100_DRAM_INIT_SIZE);
        memory_region_add_subregion(dram, 0, shared);
        host_memory_backend_set_mapped(memdev, true);
        vmstate_register_ram(shared, NULL);

        if (shared_size < R100_DRAM_INIT_SIZE) {
            MemoryRegion *tail = g_new(MemoryRegion, 1);
            snprintf(name, sizeof(name), "r100.chiplet0.dram_tail");
            memory_region_init_ram(tail, NULL, name,
                                   R100_DRAM_INIT_SIZE - shared_size,
                                   &error_fatal);
            memory_region_add_subregion(dram, shared_size, tail);
        }
    } else {
        snprintf(name, sizeof(name), "r100.chiplet%d.dram", chiplet_id);
        memory_region_init_ram(dram, NULL, name, R100_DRAM_INIT_SIZE,
                               &error_fatal);
    }
    memory_region_add_subregion(sysmem, chiplet_base + R100_DRAM_BASE, dram);

    /* iROM 64 KB */
    irom = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.irom", chiplet_id);
    memory_region_init_ram(irom, NULL, name, R100_IROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, chiplet_base + R100_IROM_BASE, irom);

    /* iRAM 256 KB */
    iram = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.iram", chiplet_id);
    memory_region_init_ram(iram, NULL, name, R100_IRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, chiplet_base + R100_IRAM_BASE, iram);

    /* Scratchpad 64 MB */
    sp_mem = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.sp_mem", chiplet_id);
    memory_region_init_ram(sp_mem, NULL, name, R100_SP_MEM_TOTAL_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, chiplet_base + R100_SP_MEM_BASE,
                                sp_mem);

    /* Shared memory 64 MB */
    sh_mem = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.sh_mem", chiplet_id);
    memory_region_init_ram(sh_mem, NULL, name, R100_SH_MEM_TOTAL_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, chiplet_base + R100_SH_MEM_BASE,
                                sh_mem);

    /* Config-space container + chiplet-wide unimpl catch-all underneath so
     * un-stubbed offsets (NBUS/SBUS/EBUS SYSREGs) return 0+LOG_UNIMP. */
    cfg_mr = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.cfg", chiplet_id);
    memory_region_init(cfg_mr, NULL, name, R100_CFG_SIZE);
    memory_region_add_subregion_overlap(sysmem,
                                        chiplet_base + R100_CFG_BASE,
                                        cfg_mr, 0);
    snprintf(name, sizeof(name), "r100.chiplet%d.cfg-unimpl", chiplet_id);
    create_unimplemented_device(g_strdup(name),
                                chiplet_base + R100_CFG_BASE,
                                R100_CFG_SIZE);

    /* CMU stubs. */
    for (i = 0; i < R100_NUM_CMU_BLOCKS; i++) {
        r100_create_cmu(cfg_mr, chiplet_id, r100_cmu_bases[i],
                        r100_cmu_names[i]);
    }

    /* Private 256 MB unimpl catch-all at PRIVATE_BASE (SYSREG_ROT_PRIVATE,
     * SYSREMAP, PMU, OTP, GPIO, RBC, CMU_RBC_*). Specific device aliases
     * layer on top via higher-priority overlap; unimplemented_device is
     * lazy-allocated so zero memory cost. */
    {
        char nm[64];
        snprintf(nm, sizeof(nm), "r100.chiplet%d.priv_win-unimpl", chiplet_id);
        create_unimplemented_device(g_strdup(nm),
                                    chiplet_base + R100_PRIVATE_WIN_BASE,
                                    R100_PRIVATE_WIN_SIZE);
    }

    r100_create_pmu(cfg_mr, sysmem, chiplet_id,
                    chiplet_id == 0 ? (R100_NUM_CHIPLETS - 1) : 0,
                    chiplet_base);
    r100_create_sysreg(cfg_mr, sysmem, chiplet_id, chiplet_base);

    /*
     * SYSREG_CP{0,1} backing RAM — triple-mounted (one per cluster per
     * chiplet) to cover the three RVBAR write paths:
     *   1. private alias @ sysmem  — BL1 QSPI-bridge writes
     *   2. cfg_mr @ SYSREG offset  — BL2 absolute cross-chiplet form
     *   3. CPU view (handled in r100_build_chiplet_view) — BL31 PSCI local form
     * PMU read_rvbar reads back from the private-alias backing on CPU_CONFIGURATION.
     * Prio 1 in cfg_mr beats the chiplet-wide unimpl catch-all (prio 0).
     */
    {
        static const struct {
            const char *tag;
            uint64_t priv_base;
            uint64_t cfg_base;
        } sysreg_cp[] = {
            { "sysreg_cp0",
              R100_SYSREG_CP0_PRIVATE_BASE, R100_CP0_SYSREG_BASE },
            { "sysreg_cp1",
              R100_SYSREG_CP1_PRIVATE_BASE, R100_CP1_SYSREG_BASE },
        };
        size_t k;

        for (k = 0; k < ARRAY_SIZE(sysreg_cp); k++) {
            MemoryRegion *mr = g_new(MemoryRegion, 1);
            MemoryRegion *cfg_alias = g_new(MemoryRegion, 1);
            char nm[64];

            snprintf(nm, sizeof(nm), "r100.chiplet%d.%s",
                     chiplet_id, sysreg_cp[k].tag);
            memory_region_init_ram(mr, NULL, nm, R100_SYSREG_CP0_SIZE,
                                   &error_fatal);
            memory_region_add_subregion_overlap(sysmem,
                                                chiplet_base +
                                                    sysreg_cp[k].priv_base,
                                                mr, 0);

            snprintf(nm, sizeof(nm), "r100.chiplet%d.%s_cfg",
                     chiplet_id, sysreg_cp[k].tag);
            memory_region_init_alias(cfg_alias, NULL, nm, mr, 0,
                                     R100_SYSREG_CP0_SIZE);
            memory_region_add_subregion_overlap(
                cfg_mr,
                sysreg_cp[k].cfg_base - R100_CFG_BASE,
                cfg_alias, 1);
        }
    }

    r100_create_hbm(cfg_mr, chiplet_id);
    r100_create_smmu(cfg_mr, chiplet_id);
    r100_create_pvt_blocks(cfg_mr, chiplet_id);
    r100_create_dma_pl330(cfg_mr, chiplet_id);
    r100_create_qspi_bridge(cfg_mr, chiplet_id);
    r100_create_qspi_boot(cfg_mr, sysmem, chiplet_id, chiplet_base);
    r100_create_rbc_blocks(cfg_mr, sysmem, chiplet_id, chiplet_base);

    /* DCL cluster (DNC/SHM/MGLUE) + RBDMA cfg-window stubs. q-cp CP1's
     * cp_create_tasks_impl polls SHM INTR_VEC.tpg_done / RDSN MGLUE STATUS0
     * / RBDMA IP_INFO — see r100_dnc.c. Prio 1 beats the cfg_mr unimpl. */
    {
        static const struct {
            const char *tag;
            uint64_t base;
            uint32_t dcl_id;
        } dcls[] = {
            { "dcl0", R100_DCL0_CFG_BASE, 0 },
            { "dcl1", R100_DCL1_CFG_BASE, 1 },
        };
        size_t k;

        for (k = 0; k < ARRAY_SIZE(dcls); k++) {
            DeviceState *dev = qdev_new(TYPE_R100_DNC_CLUSTER);
            SysBusDevice *sbd;

            qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
            qdev_prop_set_uint32(dev, "dcl-id", dcls[k].dcl_id);
            sbd = SYS_BUS_DEVICE(dev);
            sysbus_realize_and_unref(sbd, &error_fatal);
            memory_region_add_subregion_overlap(
                cfg_mr, dcls[k].base - R100_CFG_BASE,
                sysbus_mmio_get_region(sbd, 0), 1);

            /* M9-1c: wire each (slot, cmd_type) GPIO output to the
             * matching DNC SPI on this chiplet's GIC. q-cp on CP1 binds
             * dnc_X_done_handler to all six DNC INTIDs per slot via
             * gic_irq_connect; we only synthesise the four
             * non-EXCEPTION lines (COMPUTE / UDMA / UDMA_LP / UDMA_ST,
             * cmd_type 0..3). The done-passage discriminator
             * cfg1.queue is 2 bits so cmd_type 4 (TASK16B) is never
             * actually triggered from put_urq_task — leave its line
             * unwired (sysbus_init_irq still allocates the slot). */
            for (uint32_t slot = 0; slot < R100_DNC_SLOT_COUNT; slot++) {
                for (uint32_t ct = 0; ct < R100_HW_SPEC_DNC_CMD_TYPE_NUM;
                     ct++) {
                    uint32_t global_dnc_id =
                        dcls[k].dcl_id * R100_DNC_SLOT_COUNT + slot;
                    uint32_t intid = r100_dnc_intid(global_dnc_id, ct);
                    int gpio_idx =
                        slot * R100_HW_SPEC_DNC_CMD_TYPE_NUM + ct;

                    if (intid == 0) {
                        continue;
                    }
                    sysbus_connect_irq(sbd, gpio_idx,
                        qdev_get_gpio_in(gic_dev,
                            R100_INTID_TO_GIC_SPI_GPIO(intid)));
                }
            }
            (void)dcls[k].tag;
        }
    }

    /* P4A — r100-rbdma functional stub. Per-chiplet instantiation
     * because q-cp's rbdma_init(cl_id) runs on every CA73 CP0 (and
     * binds rbdma_done_handler to INT_ID_RBDMA1 on its local GIC). The
     * device exposes two GPIO-out lines: idx 0 = INT_ID_RBDMA0_ERR
     * (reserved, never pulsed) and idx 1 = INT_ID_RBDMA1 (fnsh-FIFO
     * completion line driven by the kick-BH). Prio 1 in cfg_mr beats
     * the chiplet-wide unimpl catch-all. */
    {
        DeviceState *dev = qdev_new(TYPE_R100_RBDMA);
        SysBusDevice *sbd;

        qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        memory_region_add_subregion_overlap(
            cfg_mr, R100_NBUS_L_RBDMA_CFG_BASE - R100_CFG_BASE,
            sysbus_mmio_get_region(sbd, 0), 1);

        sysbus_connect_irq(sbd, 0,
            qdev_get_gpio_in(gic_dev,
                R100_INTID_TO_GIC_SPI_GPIO(R100_INT_ID_RBDMA0_ERR)));
        sysbus_connect_irq(sbd, 1,
            qdev_get_gpio_in(gic_dev,
                R100_INTID_TO_GIC_SPI_GPIO(R100_INT_ID_RBDMA1)));
    }

    r100_create_pcie_subctrl(cfg_mr, chiplet_id);

    /* CSS600 generic counter (4 KB RAM). generic_delay_timer_init writes to
     * CNTCR/CNTCVL/CNTCVU; QEMU CPU has its own gtimer, so RAM accepts the
     * stores and that's enough. */
    {
        MemoryRegion *cntgen = g_new(MemoryRegion, 1);
        char nm[64];
        snprintf(nm, sizeof(nm), "r100.chiplet%d.css600_cntgen", chiplet_id);
        memory_region_init_ram(cntgen, NULL, nm,
                               R100_CSS600_CNTGEN_SIZE, &error_fatal);
        memory_region_add_subregion(cfg_mr,
                                    R100_CSS600_CNTGEN_BASE - R100_CFG_BASE,
                                    cntgen);
    }

    /* TODO Phase 2: DNC, HDMA */
}

/*
 * Per-chiplet "CPU view" MemoryRegion. Silicon's 256 MB PRIVATE_BASE
 * window (0x1E00000000) is chiplet-local (FW reads own CHIPLET_ID via
 * this alias). REMU stores each chiplet's aliases at
 * `chiplet_base + 0x1E00000000` in sysmem and adds per-chiplet container:
 *   1. sysmem alias @0 (shared peripherals, DRAM, cross-chiplet cfg).
 *   2. prio-10 overlay at PRIVATE_WIN_BASE → chiplet's own slice.
 *   3. prio-10 overlay at SYSREG_CP{0,1}_BASE for BL31 PSCI set_rvbar
 *      (local cfg-space form) — same backing RAM as private-alias,
 *      coherent with PMU read_rvbar.
 *   4. prio-10 GIC dist/redist aliases → chiplet's own GIC (mounted in
 *      sysmem at chiplet-absolute address with prio 1).
 * CPU "memory" link points here; TLB walks hit the chiplet overlay.
 * Same idiom as hw/arm/xlnx-versal.c APU/RPU.
 */
static MemoryRegion *r100_build_chiplet_view(MemoryRegion *sysmem,
                                             int chiplet_id)
{
    uint64_t chiplet_base = (uint64_t)chiplet_id * R100_CHIPLET_OFFSET;
    MemoryRegion *view = g_new(MemoryRegion, 1);
    MemoryRegion *sysmem_alias = g_new(MemoryRegion, 1);
    MemoryRegion *priv_alias = g_new(MemoryRegion, 1);
    MemoryRegion *cp0_cfg_alias = g_new(MemoryRegion, 1);
    MemoryRegion *cp1_cfg_alias = g_new(MemoryRegion, 1);
    char name[64];

    snprintf(name, sizeof(name), "r100.chiplet%d.cpu_view", chiplet_id);
    memory_region_init(view, NULL, name, UINT64_MAX);

    snprintf(name, sizeof(name), "r100.chiplet%d.sysmem_alias", chiplet_id);
    memory_region_init_alias(sysmem_alias, NULL, name, sysmem, 0, UINT64_MAX);
    memory_region_add_subregion_overlap(view, 0, sysmem_alias, 0);

    snprintf(name, sizeof(name), "r100.chiplet%d.priv_view", chiplet_id);
    memory_region_init_alias(priv_alias, NULL, name, sysmem,
                             chiplet_base + R100_PRIVATE_WIN_BASE,
                             R100_PRIVATE_WIN_SIZE);
    memory_region_add_subregion_overlap(view, R100_PRIVATE_WIN_BASE,
                                        priv_alias, 10);

    /* Chiplet-local SYSREG_CP0 view @ cfg-space addr. Target RAM (created
     * by r100_chiplet_init at chiplet_base + SYSREG_CP0_PRIVATE_BASE)
     * resolved lazily. */
    snprintf(name, sizeof(name), "r100.chiplet%d.sysreg_cp0_cfg_view",
             chiplet_id);
    memory_region_init_alias(cp0_cfg_alias, NULL, name, sysmem,
                             chiplet_base + R100_SYSREG_CP0_PRIVATE_BASE,
                             R100_SYSREG_CP0_SIZE);
    memory_region_add_subregion_overlap(view, R100_CP0_SYSREG_BASE,
                                        cp0_cfg_alias, 10);

    /* Same for SYSREG_CP1 (BL2 plat_set_cpu_rvbar(CLUSTER_CP1)). */
    snprintf(name, sizeof(name), "r100.chiplet%d.sysreg_cp1_cfg_view",
             chiplet_id);
    memory_region_init_alias(cp1_cfg_alias, NULL, name, sysmem,
                             chiplet_base + R100_SYSREG_CP1_PRIVATE_BASE,
                             R100_SYSREG_CP0_SIZE);
    memory_region_add_subregion_overlap(view, R100_CP1_SYSREG_BASE,
                                        cp1_cfg_alias, 10);

    /* Chiplet-local GIC aliases. arm-gicv3 mounted in sysmem at
     * chiplet_base + GIC_{DIST,REDIST}_BASE; alias them back to the
     * FW-hardcoded addresses at prio 10 so CPU reads don't fall through
     * to chiplet 0's cfg_mr. 8-frame 1 MB redist (CP0+CP1 back-to-back). */
    {
        MemoryRegion *dist_alias = g_new(MemoryRegion, 1);
        MemoryRegion *redist_alias = g_new(MemoryRegion, 1);

        snprintf(name, sizeof(name), "r100.chiplet%d.gic_dist_view",
                 chiplet_id);
        memory_region_init_alias(dist_alias, NULL, name, sysmem,
                                 chiplet_base + R100_GIC_DIST_BASE,
                                 0x10000);
        memory_region_add_subregion_overlap(view, R100_GIC_DIST_BASE,
                                            dist_alias, 10);

        snprintf(name, sizeof(name), "r100.chiplet%d.gic_redist_view",
                 chiplet_id);
        memory_region_init_alias(redist_alias, NULL, name, sysmem,
                                 chiplet_base + R100_GIC_REDIST_CP0_BASE,
                                 0x100000);
        memory_region_add_subregion_overlap(view, R100_GIC_REDIST_CP0_BASE,
                                            redist_alias, 10);
    }

    return view;
}

/* Machine init: 4 chiplets + 32 CPUs + 4 per-chiplet GICs + 4 UARTs. */
static void r100_soc_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *chiplet_views[R100_NUM_CHIPLETS];
    DeviceState *gic_dev[R100_NUM_CHIPLETS];
    Object *cpuobj;
    int chiplet, cluster, core, cpu_idx;
    int num_cpus = R100_NUM_CORES_TOTAL;
    const int cpus_per_gic = R100_NUM_CORES_PER_CHIPLET; /* 8 */

    if (machine->smp.cpus != num_cpus && machine->smp.cpus != 0) {
        error_report("R100 SoC requires exactly %d CPUs (got %d)",
                     num_cpus, machine->smp.cpus);
        exit(1);
    }

    /* Build CPU views first — CPUs need to set "memory" link at realize.
     * Aliasing sysmem means later subregions propagate automatically. */
    for (chiplet = 0; chiplet < R100_NUM_CHIPLETS; chiplet++) {
        chiplet_views[chiplet] = r100_build_chiplet_view(sysmem, chiplet);
    }

    /* Create CPUs. */
    for (cpu_idx = 0; cpu_idx < num_cpus; cpu_idx++) {
        chiplet = cpu_idx / R100_NUM_CORES_PER_CHIPLET;
        cluster = (cpu_idx % R100_NUM_CORES_PER_CHIPLET) /
                  R100_NUM_CORES_PER_CLUSTER;
        core = cpu_idx % R100_NUM_CORES_PER_CLUSTER;

        cpuobj = object_new(machine->cpu_type);

        /* MPIDR: chiplet in bits [24:25] (flags band, FW masks off in
         * plat_core_pos_by_mpidr), Aff1=cluster, Aff0=core. Per-cluster
         * bl31_cp{0,1}.bin means core_pos only needs cluster-local uniqueness. */
        object_property_set_int(cpuobj, "mp-affinity",
                                ((uint64_t)chiplet << 24) |
                                    ((uint64_t)cluster << 8) | core,
                                &error_fatal);

        /* MIDR = CA73 r1p1 (0x411FD091). TF-A asserts on missing CA73 cpu_ops;
         * r1p1 is past all runtime-gated CA73 errata revision windows. Commit 7a2e232. */
        object_property_set_int(cpuobj, "midr", 0x411FD091, &error_fatal);

        object_property_set_int(cpuobj, "rvbar",
                                (uint64_t)chiplet * R100_CHIPLET_OFFSET +
                                R100_BL1_RO_BASE,
                                &error_fatal);

        object_property_set_int(cpuobj, "cntfrq", R100_CORE_TIMER_FREQ,
                                &error_fatal);

        /* Route through chiplet view so PRIVATE_BASE reads hit own-chiplet
         * devices. Secure AS inherits when "secure-memory" is unset. */
        object_property_set_link(cpuobj, "memory",
                                 OBJECT(chiplet_views[chiplet]),
                                 &error_fatal);

        /* Only chiplet 0 / CP0 / core 0 starts powered on. */
        if (cpu_idx != 0) {
            object_property_set_bool(cpuobj, "start-powered-off", true,
                                     &error_fatal);
        }

        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        define_arm_cp_regs(ARM_CPU(cpuobj), r100_samsung_impdef_regs);
        define_arm_cp_regs(ARM_CPU(cpuobj), r100_cortex_a73_impdef_regs);
    }

    /*
     * Per-chiplet GICv3. One arm-gicv3 per chiplet (num-cpu=8). Redist
     * 8 frames contiguous: CP0 at +0..3, CP1 at +4..7 (= REDIST_CP1_BASE
     * = CP0_BASE + 4*0x20000). Mounted at chiplet-absolute addr prio 1
     * (beats cfg_mr prio 0); CPU views add prio-10 aliases at the
     * FW-local addresses. `first-cpu-index` is our QEMU-patch property
     * (see cli/qemu-patches/0001-*.patch) that lets GIC N bind global
     * CPUs [8*N..8*N+7] without ICC_* cpreg collision.
     */
    for (chiplet = 0; chiplet < R100_NUM_CHIPLETS; chiplet++) {
        SysBusDevice *gbd;
        uint64_t chiplet_base = (uint64_t)chiplet * R100_CHIPLET_OFFSET;

        gic_dev[chiplet] = qdev_new("arm-gicv3");
        qdev_prop_set_uint32(gic_dev[chiplet], "num-cpu", cpus_per_gic);
        qdev_prop_set_uint32(gic_dev[chiplet], "first-cpu-index",
                             (uint32_t)chiplet * cpus_per_gic);
        /* num-irq covers up to INTID num-irq-1. q-cp's interrupt.h
         * enum reaches INT_ID_RBDMA0_ERR = 977; M9-1c only wires
         * DNC SPIs (max INTID 617 = DNC15_TASK16B) but a future
         * round of HAL wiring may need any of the higher IDs, so
         * size for the full distributor. */
        qdev_prop_set_uint32(gic_dev[chiplet], "num-irq", 992);
        qdev_prop_set_uint32(gic_dev[chiplet], "revision", 3);
        {
            QList *redist = qlist_new();
            qlist_append_int(redist, cpus_per_gic);
            qdev_prop_set_array(gic_dev[chiplet], "redist-region-count",
                                redist);
        }
        gbd = SYS_BUS_DEVICE(gic_dev[chiplet]);
        sysbus_realize_and_unref(gbd, &error_fatal);

        sysbus_mmio_map_overlap(gbd, 0,
                                chiplet_base + R100_GIC_DIST_BASE, 1);
        sysbus_mmio_map_overlap(gbd, 1,
                                chiplet_base + R100_GIC_REDIST_CP0_BASE, 1);

        for (int local = 0; local < cpus_per_gic; local++) {
            int gi = chiplet * cpus_per_gic + local;
            DeviceState *cpudev = DEVICE(qemu_get_cpu(gi));

            sysbus_connect_irq(gbd, local,
                               qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
            sysbus_connect_irq(gbd, local + cpus_per_gic,
                               qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
            sysbus_connect_irq(gbd, local + 2 * cpus_per_gic,
                               qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
            sysbus_connect_irq(gbd, local + 3 * cpus_per_gic,
                               qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));

            /* Wire gtimer outputs → GIC PPI inputs (layout mirrors
             * hw/arm/virt.c). FreeRTOS tick is CNTVIRQ (PPI 27); without
             * this wiring vTaskDelay never fires — commit 680f964.
             * GICv3 PPI index = (num_irq - GIC_INTERNAL) + i*32 + ppi.
             * Keep in sync with the num-irq above (992 → SPI count
             * 960 → PPI region starts at gpio_in[960]). */
            {
                static const int gt_ppi[NUM_GTIMERS] = {
                    [GTIMER_PHYS]    = ARCH_TIMER_NS_EL1_IRQ,      /* 30 */
                    [GTIMER_VIRT]    = ARCH_TIMER_VIRT_IRQ,        /* 27 */
                    [GTIMER_HYP]     = ARCH_TIMER_NS_EL2_IRQ,      /* 26 */
                    [GTIMER_SEC]     = ARCH_TIMER_S_EL1_IRQ,       /* 29 */
                    [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ, /* 28 */
                };
                const int intidbase = (992 - GIC_INTERNAL) + local * 32;

                for (unsigned t = 0; t < NUM_GTIMERS; t++) {
                    qdev_connect_gpio_out(
                        cpudev, t,
                        qdev_get_gpio_in(gic_dev[chiplet],
                                         intidbase + gt_ppi[t]));
                }
            }
        }
    }

    /* Shared QSPI NOR flash — single chip, aliased per-chiplet inside
     * r100_create_flash. */
    r100_create_flash(sysmem);

    /* Init all 4 chiplets. */
    {
        R100SoCMachineState *r100m = R100_SOC_MACHINE(machine);
        HostMemoryBackend *memdev = r100_soc_resolve_memdev(r100m);
        for (chiplet = 0; chiplet < R100_NUM_CHIPLETS; chiplet++) {
            r100_chiplet_init(machine, chiplet, sysmem,
                              chiplet == 0 ? memdev : NULL,
                              gic_dev[chiplet]);
        }
    }

    /*
     * Per-chiplet 16550 UART (serial-mm, regshift=2). FW uses TI 16550
     * driver (uart_helper.h), not PL011. Each chiplet N → gpio_in[33]
     * of its own GIC (= INTID 65 in QEMU's numbering; see
     * R100_INTID_TO_GIC_SPI_GPIO), bound to serial_hd(N) so streams
     * demux. console_uart_init() in q-sys main() doesn't register an
     * RX ISR and TX is polled, so the exact INTID here is dead code
     * today — revisit if terminal_task ever grows an IRQ-driven RX
     * path (would need the same INTID↔gpio_in conversion as the
     * mailbox above, lest it repeat the PERI0_M7 collision).
     */
    for (chiplet = 0; chiplet < R100_NUM_CHIPLETS; chiplet++) {
        SerialMM *smm = SERIAL_MM(qdev_new(TYPE_SERIAL_MM));
        MemoryRegion *uart_mr;
        Chardev *chr = serial_hd(chiplet);

        qdev_prop_set_uint8(DEVICE(smm), "regshift", 2);
        qdev_prop_set_uint32(DEVICE(smm), "baudbase",
                             (uint32_t)(R100_UART_CLOCK / 16));
        if (chr) {
            qdev_prop_set_chr(DEVICE(smm), "chardev", chr);
        }
        qdev_prop_set_uint8(DEVICE(smm), "endianness", DEVICE_LITTLE_ENDIAN);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(smm), &error_fatal);
        sysbus_connect_irq(SYS_BUS_DEVICE(smm), 0,
                           qdev_get_gpio_in(gic_dev[chiplet], 33));

        uart_mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(smm), 0);

        /* Prio 10 beats cfg_mr unimpl + CMU stub at chiplet N's cfg +0x9040000. */
        memory_region_add_subregion_overlap(
            sysmem,
            (uint64_t)chiplet * R100_CHIPLET_OFFSET + R100_PERI0_UART0_BASE,
            uart_mr, 10);
    }

    /* HILS ring-buffer tail drain (chiplet 0). 2 MB .logbuf at DRAM
     * 0x10000000; poll-and-emit independent of in-FW terminal_task.
     * Wired to serial_hd(N_CHIPLETS) = 5th -serial slot → hils.log. */
    {
        DeviceState *dev = qdev_new(TYPE_R100_LOGBUF);
        Chardev *chr = serial_hd(R100_NUM_CHIPLETS);

        qdev_prop_set_uint64(dev, "base", 0x10000000ULL);
        qdev_prop_set_uint64(dev, "size", 0x00200000ULL);
        if (chr) {
            qdev_prop_set_chr(dev, "chardev", chr);
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    }

    /*
     * Chiplet-0 PCIE mailbox cluster + optional r100-cm7 ingress +
     * optional r100-imsix egress.
     *
     * Silicon: 17 Samsung IPM SFRs at MAILBOX_PCIE (VF0..VF15 @ 0x1000
     * stride + PF at 0x1FF8170000). VF0 serves q-sys CA73 (INTID 185
     * on __TARGET_CP==0; see mailbox_data[IDX_MAILBOX_PCIE_VF0]); PF
     * serves PCIE_CM7 and hosts FW_BOOT_DONE / reset-counter ISSR
     * egress. KMD BAR4 INTGR writes physically land on PF, PCIE_CM7
     * then relays to VF0.
     *
     * REMU models two blocks (VF0, PF); share the `issr` chardev (host
     * keys off BAR4 offset, not origin). INTMSR IRQs only wired for VF0
     * (CM7 not modelled, so PF.INTGR is latched-but-unrouted). The
     * doorbell ingress is shortcut-wired to VF0 so KMD INTGR writes
     * directly fire the IRQ q-sys services — see r100_mailbox.c /
     * r100_cm7.c, commit cd24aa9 (M8a) and a01d2b5 (CM7 stub).
     *
     * r100-imsix is the q-cp `cb_complete → pcie_msix_trigger` MMIO
     * sink (single source of truth for MSI-X). Both devices stay
     * optional (gated on their respective chardevs); single-QEMU
     * Phase 1 runs don't wire any of the cross-process chardevs and
     * see none of the devices.
     */
    {
        R100SoCMachineState *r100m = R100_SOC_MACHINE(machine);
        DeviceState *mbx_vf0_dev;
        DeviceState *mbx_pf_dev;
        /* P3: q-cp's `_inst[HW_SPEC_DNC_QUEUE_NUM=4]` task-queue
         * mailboxes — chiplet 0 only (q-cp/CP1 runs there). Indexed by
         * cmd_type: [0]=COMPUTE, [1]=UDMA, [2]=UDMA_LP, [3]=UDMA_ST.
         * Instantiated as real Samsung-IPM SFRs so q-cp/CP0's per-
         * cmd_type push (the honest path) lands on a proper ring;
         * mtq_init's MBTQ_PI_IDX/CI_IDX writes round-trip without
         * aliasing other lazy-RAM placeholders. */
        DeviceState *mbx_mbtq_dev[4];
        Chardev *issr_chr = NULL;
        Chardev *issr_dbg = NULL;

        /* Resolve issr/debug chardevs once; both mailbox blocks latch
         * them. Unset in single-QEMU runs — egress short-circuits on
         * qemu_chr_fe_backend_connected. */
        issr_chr = r100_soc_resolve_chr(r100m->issr_chardev_id, "issr");
        if (issr_chr) {
            issr_dbg = r100_soc_resolve_chr(r100m->issr_debug_chardev_id,
                                            "issr debug");
        }

        /* VF0 — q-sys CA73 INTID 185 sink. No issr-chardev here: QEMU
         * CharBackend is single-frontend, so only PF (the auth egress
         * source for bootdone/reset-counter) owns the issr chardev. */
        mbx_vf0_dev = qdev_new(TYPE_R100_MAILBOX);
        qdev_prop_set_string(mbx_vf0_dev, "name", "pcie.vf0.chiplet0");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(mbx_vf0_dev), &error_fatal);
        /* Prio 10 beats chiplet-0 cfg_mr catch-all. */
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(mbx_vf0_dev), 0,
                                R100_PCIE_MAILBOX_BASE, 10);
        /* gpio_in index = INTID - GIC_INTERNAL; see remu_addrmap.h. */
        sysbus_connect_irq(SYS_BUS_DEVICE(mbx_vf0_dev), 0,
                           qdev_get_gpio_in(gic_dev[0],
                                            R100_INTID_TO_GIC_SPI_GPIO(
                                                R100_PCIE_MBX_GROUP0_INTID)));
        sysbus_connect_irq(SYS_BUS_DEVICE(mbx_vf0_dev), 1,
                           qdev_get_gpio_in(gic_dev[0],
                                            R100_INTID_TO_GIC_SPI_GPIO(
                                                R100_PCIE_MBX_GROUP1_INTID)));

        /* PF — bootdone_notify_to_host(PCIE_PF) target; on silicon also
         * where KMD BAR4 TLPs decode. No SPI wire (PF's IRQ subscriber
         * is PCIE_CM7, not modelled). */
        mbx_pf_dev = qdev_new(TYPE_R100_MAILBOX);
        qdev_prop_set_string(mbx_pf_dev, "name", "pcie.pf.chiplet0");
        if (issr_chr) {
            qdev_prop_set_chr(mbx_pf_dev, "issr-chardev", issr_chr);
        }
        if (issr_dbg) {
            qdev_prop_set_chr(mbx_pf_dev, "issr-debug-chardev", issr_dbg);
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(mbx_pf_dev), &error_fatal);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(mbx_pf_dev), 0,
                                R100_PCIE_MAILBOX_PF_BASE, 10);

        /* P3: task-queue mailboxes at PERI{0,1}_MAILBOX_M{9,10}_CPU1
         * (chiplet 0). q-cp's CP1.cpu0 polls MBTQ_PI_IDX (ISSR[0])
         * across all four from taskmgr_fetch_dnc_task_master_cp1, one
         * per cmd_type (COMPUTE/UDMA/UDMA_LP/UDMA_ST). Poll-based (no
         * IRQ wiring); each mailbox is just the shared scratch ring
         * for its cmd_type. Replaces the lazy-RAM placeholders for
         * these four slots — the chiplet/mailbox loop below skips
         * (cl=0, j=0..3). */
        {
            static const struct {
                const char *name;
                uint64_t base;
            } mbtq_slots[ARRAY_SIZE(mbx_mbtq_dev)] = {
                { "peri0_m9_cpu1.chiplet0",
                  R100_PERI0_MAILBOX_M9_BASE },   /* COMPUTE */
                { "peri0_m10_cpu1.chiplet0",
                  R100_PERI0_MAILBOX_M10_BASE },  /* UDMA */
                { "peri1_m9_cpu1.chiplet0",
                  R100_PERI1_MAILBOX_M9_BASE },   /* UDMA_LP */
                { "peri1_m10_cpu1.chiplet0",
                  R100_PERI1_MAILBOX_M10_BASE },  /* UDMA_ST */
            };

            for (size_t i = 0; i < ARRAY_SIZE(mbtq_slots); i++) {
                mbx_mbtq_dev[i] = qdev_new(TYPE_R100_MAILBOX);
                qdev_prop_set_string(mbx_mbtq_dev[i], "name",
                                     mbtq_slots[i].name);
                sysbus_realize_and_unref(SYS_BUS_DEVICE(mbx_mbtq_dev[i]),
                                         &error_fatal);
                sysbus_mmio_map_overlap(SYS_BUS_DEVICE(mbx_mbtq_dev[i]), 0,
                                        mbtq_slots[i].base, 10);
            }
        }

        /*
         * Integrated MSI-X trigger (M7, commit db3d1df). Snoops FW
         * stores at REBELH_PCIE_MSIX_ADDR (0x1BFFFFFFFC), forwards
         * (offset, db_data) frames on `msix` chardev → host-side
         * msix_notify(). Driven exclusively by q-cp's `pcie_msix_trigger`
         * on CA73 CP0 (`q/sys/osl/FreeRTOS/.../msix.c`) — `cb_complete`
         * naturally stores into this trap. Optional: only when
         * -machine r100-soc,msix=<id>.
         */
        {
            Chardev *chr = r100_soc_resolve_chr(r100m->msix_chardev_id,
                                                "msix");

            if (chr) {
                Chardev *dbg = r100_soc_resolve_chr(
                                   r100m->msix_debug_chardev_id,
                                   "msix debug");
                DeviceState *imsix_dev = qdev_new(TYPE_R100_IMSIX);

                qdev_prop_set_chr(imsix_dev, "chardev", chr);
                if (dbg) {
                    qdev_prop_set_chr(imsix_dev, "debug-chardev", dbg);
                }
                sysbus_realize_and_unref(SYS_BUS_DEVICE(imsix_dev),
                                         &error_fatal);
                sysbus_mmio_map_overlap(SYS_BUS_DEVICE(imsix_dev), 0,
                                        R100_PCIE_IMSIX_BASE, 10);
            }
        }

        {
            Chardev *chr = r100_soc_resolve_chr(r100m->doorbell_chardev_id,
                                                "doorbell");
            if (chr) {
                Chardev *dbg = r100_soc_resolve_chr(
                                   r100m->doorbell_debug_chardev_id,
                                   "doorbell debug");
                /* M8b 3b: cfg (host→NPU) + hdma (NPU<->host). All
                 * optional — missing any demotes the corresponding
                 * CM7 behaviour to a no-op but doesn't break the
                 * M6/M8a paths. */
                Chardev *cfg_chr = r100_soc_resolve_chr(
                                       r100m->cfg_chardev_id, "cfg");
                Chardev *cfg_dbg = NULL;
                Chardev *hdma_chr = r100_soc_resolve_chr(
                                       r100m->hdma_chardev_id, "hdma");
                Chardev *hdma_dbg = NULL;
                DeviceState *cm7 = qdev_new(TYPE_R100_CM7);
                DeviceState *hdma_dev = NULL;

                if (cfg_chr) {
                    cfg_dbg = r100_soc_resolve_chr(
                                  r100m->cfg_debug_chardev_id,
                                  "cfg debug");
                }
                if (hdma_chr) {
                    hdma_dbg = r100_soc_resolve_chr(
                                  r100m->hdma_debug_chardev_id,
                                  "hdma debug");
                }

                /* M9-1c: r100-hdma owns the `hdma` chardev (single-
                 * frontend constraint) and exposes the dw_hdma_v0
                 * MMIO window q-cp programs. Created before cm7 so
                 * cm7's link can resolve. SPI 186 single line shared
                 * across all 32 channels; per-channel pending lives
                 * in SUBCTRL_EDMA_INT_CA73 (plain RAM in pcie-subctrl). */
                hdma_dev = qdev_new(TYPE_R100_HDMA);
                qdev_prop_set_uint32(hdma_dev, "chiplet-id", 0);
                if (hdma_chr) {
                    qdev_prop_set_chr(hdma_dev, "chardev", hdma_chr);
                }
                if (hdma_dbg) {
                    qdev_prop_set_chr(hdma_dev, "debug-chardev", hdma_dbg);
                }
                sysbus_realize_and_unref(SYS_BUS_DEVICE(hdma_dev),
                                         &error_fatal);
                sysbus_mmio_map(SYS_BUS_DEVICE(hdma_dev), 0,
                                R100_HDMA_BASE);
                sysbus_connect_irq(SYS_BUS_DEVICE(hdma_dev), 0,
                                   qdev_get_gpio_in(gic_dev[0],
                                       R100_INTID_TO_GIC_SPI_GPIO(
                                           R100_INT_ID_HDMA)));

                /* P1: PCIe outbound iATU stub. Only meaningful when
                 * the host chardev is present (otherwise there's
                 * nowhere to send OP_READ_REQ frames); single-QEMU
                 * runs leave the 4 GB AXI window unmapped just like
                 * before, so any q-cp BD-read attempt still surfaces
                 * as an unassigned-region access in the log. */
                if (hdma_chr) {
                    DeviceState *outbound = qdev_new(TYPE_R100_PCIE_OUTBOUND);

                    qdev_prop_set_uint32(outbound, "chiplet-id", 0);
                    object_property_set_link(OBJECT(outbound), "hdma",
                                             OBJECT(hdma_dev),
                                             &error_fatal);
                    sysbus_realize_and_unref(SYS_BUS_DEVICE(outbound),
                                             &error_fatal);
                    sysbus_mmio_map(SYS_BUS_DEVICE(outbound), 0,
                                    R100_PCIE_AXI_SLV_BASE_ADDR);
                }

                qdev_prop_set_chr(cm7, "chardev", chr);
                if (dbg) {
                    qdev_prop_set_chr(cm7, "debug-chardev", dbg);
                }
                if (cfg_chr) {
                    qdev_prop_set_chr(cm7, "cfg-chardev", cfg_chr);
                }
                if (cfg_dbg) {
                    qdev_prop_set_chr(cm7, "cfg-debug-chardev", cfg_dbg);
                }
                /* mailbox=VF0 (shortcut PCIE_CM7 relay — INTGR bits
                 * raise INTID 185 q-sys services). pf-mailbox=PF so
                 * SOFT_RESET can re-synthesise FW_BOOT_DONE→PF.ISSR[4]
                 * (CM7 stub, a01d2b5; narrowed post GIC wiring fix —
                 * cold boot is real, see docs/debugging.md
                 * Post-mortems). hdma is the cfg-mirror reverse-path
                 * sink for OP_CFG_WRITE so q-cp's NPU-side
                 * FUNC_SCRATCH writes round-trip back to the kmd. */
                object_property_set_link(OBJECT(cm7), "mailbox",
                                         OBJECT(mbx_vf0_dev),
                                         &error_fatal);
                object_property_set_link(OBJECT(cm7), "pf-mailbox",
                                         OBJECT(mbx_pf_dev),
                                         &error_fatal);
                object_property_set_link(OBJECT(cm7), "hdma",
                                         OBJECT(hdma_dev),
                                         &error_fatal);
                sysbus_realize_and_unref(SYS_BUS_DEVICE(cm7),
                                         &error_fatal);
                (void)mbx_mbtq_dev;
            }
        }
    }

    /*
     * Shared inter-chiplet mailbox RAM. TF-A mailbox_data[] dereferences
     * absolute bases directly (drivers/mailbox/mailbox.c); every chiplet
     * view needs plain RAM backing so the ISSR handshake is visible
     * across chiplets:
     *   Inbound (→CL0 BL2):  0x1FF9120000 M7 CL1, 0x1FF9130000 M8 CL2, 0x1FF9930000 M8 CL3
     *   Outbound (CL0→CLN):  CL0-base + N * CHIPLET_OFFSET
     * Plus M8b Stage 1 (commit 1ef7208): PERI{0,1}_MAILBOX_M{9,10} per
     * chiplet — q-cp CP1 workers spin on MBTQ_PI_IDX (ISSR[0]); plain
     * RAM keeps pi==ci==0 so the loop doesn't starve CP0.
     */
    {
        static const uint64_t r100_per_chiplet_mbox_offsets[] = {
            0x1FF9140000ULL, /* PERI0_MAILBOX_M9 */
            0x1FF9150000ULL, /* PERI0_MAILBOX_M10 */
            0x1FF9940000ULL, /* PERI1_MAILBOX_M9 */
            0x1FF9950000ULL, /* PERI1_MAILBOX_M10 */
        };
        static const char *r100_per_chiplet_mbox_tags[] = {
            "peri0_m9", "peri0_m10", "peri1_m9", "peri1_m10",
        };
        static const struct {
            uint64_t base;
            const char *name;
        } r100_mbox_slots[] = {
            { 0x1FF9120000ULL, "r100.mbox_cl0_peri0_m7" },
            { 0x1FF9130000ULL, "r100.mbox_cl0_peri0_m8" },
            { 0x1FF9930000ULL, "r100.mbox_cl0_peri1_m8" },
            { 0x3FF9120000ULL, "r100.mbox_cl1_peri0_m7" },
            { 0x5FF9120000ULL, "r100.mbox_cl2_peri0_m7" },
            { 0x7FF9120000ULL, "r100.mbox_cl3_peri0_m7" },
        };

        for (size_t i = 0; i < ARRAY_SIZE(r100_mbox_slots); i++) {
            MemoryRegion *mr = g_new(MemoryRegion, 1);

            memory_region_init_ram(mr, NULL, r100_mbox_slots[i].name,
                                   0x10000, &error_fatal);
            memory_region_add_subregion_overlap(sysmem,
                                                r100_mbox_slots[i].base,
                                                mr, 10);
        }

        for (int cl = 0; cl < R100_NUM_CHIPLETS; cl++) {
            for (size_t j = 0;
                 j < ARRAY_SIZE(r100_per_chiplet_mbox_offsets);
                 j++) {
                MemoryRegion *mr;
                char nm[64];

                /* P3: all four task-queue slots on chiplet 0 are real
                 * r100-mailbox blocks (q-cp DNC compute / UDMA /
                 * UDMA_LP / UDMA_ST queues) — skip the lazy-RAM
                 * placeholder so the two don't overlap. Other chiplets
                 * keep lazy-RAM (q-cp CP1 only runs on chiplet 0). */
                if (cl == 0) {
                    continue;
                }
                mr = g_new(MemoryRegion, 1);
                snprintf(nm, sizeof(nm), "r100.mbox_cl%d_%s",
                         cl, r100_per_chiplet_mbox_tags[j]);
                memory_region_init_ram(mr, NULL, nm, 0x10000,
                                       &error_fatal);
                memory_region_add_subregion_overlap(
                    sysmem,
                    (uint64_t)cl * R100_CHIPLET_OFFSET +
                        r100_per_chiplet_mbox_offsets[j],
                    mr, 10);
            }
        }
    }
}

/*
 * `memdev` and chardev machine props are strings (not links): QEMU
 * parses -machine BEFORE creating memory-backend-* / chardevs, so link
 * properties would fail with "not found". Resolved lazily at machine-init.
 *
 * All 7 props are plain `char *` fields on R100SoCMachineState that get
 * strdup'd on set, freed on finalize, and dup'd on get. The macro below
 * emits one getter/setter pair per field; the description table drives
 * instance_init's single registration loop and the offset list drives
 * finalize.
 */
#define R100_SOC_DEF_STRPROP(_sym, _field)                                  \
    static char *r100_soc_get_##_sym(Object *o, Error **errp)               \
    {                                                                       \
        (void)errp;                                                         \
        return g_strdup(R100_SOC_MACHINE(o)->_field);                       \
    }                                                                       \
    static void r100_soc_set_##_sym(Object *o, const char *v, Error **errp) \
    {                                                                       \
        (void)errp;                                                         \
        R100SoCMachineState *m = R100_SOC_MACHINE(o);                       \
        g_free(m->_field);                                                  \
        m->_field = g_strdup(v);                                            \
    }

R100_SOC_DEF_STRPROP(memdev,         memdev_id)
R100_SOC_DEF_STRPROP(doorbell,       doorbell_chardev_id)
R100_SOC_DEF_STRPROP(doorbell_debug, doorbell_debug_chardev_id)
R100_SOC_DEF_STRPROP(msix,           msix_chardev_id)
R100_SOC_DEF_STRPROP(msix_debug,     msix_debug_chardev_id)
R100_SOC_DEF_STRPROP(issr,           issr_chardev_id)
R100_SOC_DEF_STRPROP(issr_debug,     issr_debug_chardev_id)
R100_SOC_DEF_STRPROP(cfg,            cfg_chardev_id)
R100_SOC_DEF_STRPROP(cfg_debug,      cfg_debug_chardev_id)
R100_SOC_DEF_STRPROP(hdma,           hdma_chardev_id)
R100_SOC_DEF_STRPROP(hdma_debug,     hdma_debug_chardev_id)

#undef R100_SOC_DEF_STRPROP

typedef struct R100SoCStrProp {
    const char *name;                                  /* -machine ...,<name>= */
    char *(*get)(Object *, Error **);
    void  (*set)(Object *, const char *, Error **);
    size_t field_off;                                  /* for finalize */
    const char *description;
} R100SoCStrProp;

#define R100_SOC_STR_PROP(_name, _sym, _field, _desc) { \
    .name        = (_name),                             \
    .get         = r100_soc_get_##_sym,                 \
    .set         = r100_soc_set_##_sym,                 \
    .field_off   = offsetof(R100SoCMachineState, _field),\
    .description = (_desc),                             \
}

static const R100SoCStrProp r100_soc_str_props[] = {
    R100_SOC_STR_PROP("memdev", memdev, memdev_id,
        "memory-backend-* id spliced over chiplet-0 DRAM head "
        "(Phase 2 M5)."),
    R100_SOC_STR_PROP("doorbell", doorbell, doorbell_chardev_id,
        "chardev id: host→NPU 8-byte (BAR4 off, val) frames into "
        "INTGR/ISSR (M6/M8a)."),
    R100_SOC_STR_PROP("doorbell-debug", doorbell_debug,
                      doorbell_debug_chardev_id,
        "chardev id for an ASCII trace of every doorbell frame accepted."),
    R100_SOC_STR_PROP("msix", msix, msix_chardev_id,
        "chardev id: NPU→host MSI-X frames from REBELH_PCIE_MSIX_ADDR (M7)."),
    R100_SOC_STR_PROP("msix-debug", msix_debug, msix_debug_chardev_id,
        "chardev id for an ASCII trace of every iMSIX doorbell frame."),
    R100_SOC_STR_PROP("issr", issr, issr_chardev_id,
        "chardev id: NPU→host 8-byte (BAR4 off, val) ISSR-shadow frames "
        "(M8a)."),
    R100_SOC_STR_PROP("issr-debug", issr_debug, issr_debug_chardev_id,
        "chardev id for an ASCII trace of every ISSR frame emitted."),
    R100_SOC_STR_PROP("cfg", cfg, cfg_chardev_id,
        "chardev id: host→NPU 8-byte (BAR2 cfg-head off, val) frames "
        "that populate cfg_shadow[] for the CM7 QINIT stub (M8b 3b)."),
    R100_SOC_STR_PROP("cfg-debug", cfg_debug, cfg_debug_chardev_id,
        "chardev id for an ASCII trace of every cfg frame received."),
    R100_SOC_STR_PROP("hdma", hdma, hdma_chardev_id,
        "chardev id: NPU→host variable-length HDMA_OP_WRITE frames "
        "that the host executes as pci_dma_write (M8b 3b, M9 BD-done)."),
    R100_SOC_STR_PROP("hdma-debug", hdma_debug, hdma_debug_chardev_id,
        "chardev id for an ASCII trace of every hdma frame emitted."),
};

#undef R100_SOC_STR_PROP

static void r100_soc_machine_instance_init(Object *obj)
{
    for (size_t i = 0; i < ARRAY_SIZE(r100_soc_str_props); i++) {
        const R100SoCStrProp *p = &r100_soc_str_props[i];
        object_property_add_str(obj, p->name, p->get, p->set);
        object_property_set_description(obj, p->name, p->description);
    }
}

static void r100_soc_machine_instance_finalize(Object *obj)
{
    R100SoCMachineState *r100m = R100_SOC_MACHINE(obj);
    for (size_t i = 0; i < ARRAY_SIZE(r100_soc_str_props); i++) {
        char **slot = (char **)((char *)r100m +
                                r100_soc_str_props[i].field_off);
        g_free(*slot);
        *slot = NULL;
    }
}

/* Resolve memdev=<id> to HostMemoryBackend; NULL if unset; exit on bad id. */
static HostMemoryBackend *r100_soc_resolve_memdev(R100SoCMachineState *r100m)
{
    Object *obj;

    if (r100m->memdev_id == NULL || *r100m->memdev_id == '\0') {
        return NULL;
    }
    obj = object_resolve_path_component(object_get_objects_root(),
                                        r100m->memdev_id);
    if (obj == NULL) {
        error_report("r100-soc: memdev '%s' not found under /objects",
                     r100m->memdev_id);
        exit(1);
    }
    if (!object_dynamic_cast(obj, TYPE_MEMORY_BACKEND)) {
        error_report("r100-soc: object '%s' is not a memory-backend",
                     r100m->memdev_id);
        exit(1);
    }
    return MEMORY_BACKEND(obj);
}

/*
 * Resolve one -machine r100-soc,<role>=<id> chardev. Unset (NULL or "")
 * returns NULL so the caller can skip the matching device instantiation
 * in single-QEMU runs. A non-empty id that doesn't name a known chardev
 * is a configuration error and aborts the boot.
 */
static Chardev *r100_soc_resolve_chr(const char *id, const char *role)
{
    Chardev *chr;

    if (id == NULL || *id == '\0') {
        return NULL;
    }
    chr = qemu_chr_find(id);
    if (chr == NULL) {
        error_report("r100-soc: %s chardev '%s' not found", role, id);
        exit(1);
    }
    return chr;
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
    .instance_size = sizeof(R100SoCMachineState),
    .instance_init = r100_soc_machine_instance_init,
    .instance_finalize = r100_soc_machine_instance_finalize,
    .class_init = r100_soc_machine_class_init,
};

static void r100_soc_machine_register_types(void)
{
    type_register_static(&r100_soc_machine_info);
}

type_init(r100_soc_machine_register_types)
