/*
 * REMU - R100 NPU System Emulator
 * QEMU machine type: r100-soc
 *
 * Models a 4-chiplet R100 SoC with:
 *   - 32 CA73 vCPUs (4 chiplets x 2 clusters x 4 cores)
 *   - Per-chiplet memory regions (DRAM, iROM, iRAM, SP, SHM)
 *   - Per-chiplet peripheral stubs (CMU, PMU, SYSREG, HBM3)
 *   - Per-chiplet GICv3 (one arm-gicv3 instance per chiplet, num-cpu=8)
 *   - Per-chiplet 16550 UART for console output (chardev-demuxed, IRQ=SPI 33
 *     on its own chiplet's GIC)
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
#include "qapi/qmp/qlist.h"
#include "r100_soc.h"

/* Forward declaration — definition at the bottom next to the rest of
 * the machine-level QOM glue. Needed here because r100_soc_init (which
 * calls it) is compiled before the definition. */
static HostMemoryBackend *r100_soc_resolve_memdev(R100SoCMachineState *r100m);

/*
 * Samsung IMPDEF EL1/EL3 system instructions used by the R100 FW.
 *
 * The R100 CP is silicon-equivalent to Samsung Cortex-A73 (Exynos M-family
 * parts). The cache-maintenance helpers in TF-A/BL31 and the SMMU driver
 * flush L1/L2 via an IMPDEF SYS instruction encoded as
 *   MSR S1_1_C15_C14_0, Xt        (op0=1, op1=1, CRn=15, CRm=14, op2=0)
 * Refs:
 *   external/.../tf-a/lib/xlat_tables_v2/aarch64/enable_mmu.S:156,161
 *     (enable_mmu_direct_el3_bl31 cache invalidate pass after turning MMU on)
 *   external/.../tf-a/drivers/smmu/smmu.c:834,838,845
 *     (smmu_flush_all_cache / smmu_flush_l2_cache)
 *
 * QEMU's generic AArch64 decoder does not model this encoding, so the
 * instruction UNDEFs at EL3. Without a handler, BL31 faults the first time
 * it tries to invalidate caches right after enable_mmu_direct_el3_bl31 has
 * turned on the EL3 MMU, and the CPU ends up trapped in the EL3 sync
 * exception vector (a nested fault then manifests when the panic handler
 * dereferences a zero SP_EL3).
 *
 * We model the register as a write-only NOP: the FW only ever writes to
 * it, and the effect (cache flush) is a no-op on an emulator that does
 * not model caches anyway.
 */
static const ARMCPRegInfo r100_samsung_impdef_regs[] = {
    { .name = "S1_1_C15_C14_0_CACHE_INV",
      .state = ARM_CP_STATE_AA64,
      .opc0 = 1, .opc1 = 1, .crn = 15, .crm = 14, .opc2 = 0,
      .access = PL1_W, .type = ARM_CP_NOP },
};

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
 *
 * Maps the device at two locations:
 *   - config-space view: R100_ROT_PMU_BASE (within cfg_mr)
 *   - private-alias view: R100_CPMU_PRIVATE_BASE in sysmem (per-chiplet)
 *
 * BL1 reads boot-mode/log/flag registers via the private alias
 * (CPMU_PRIVATE in rebel_h_bootmgr.h), and BL2/BL31 use the config-space
 * view via QSPI bridge for cross-chiplet access.
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

    /* Private alias: 0x1E00230000 + chiplet_base, points at the same iomem.
     * Use overlap with priority 0 so it takes precedence over the
     * lower-priority private-window RAM catch-all. */
    alias = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "r100.chiplet%d.pmu_priv", chiplet_id);
    memory_region_init_alias(alias, NULL, name, iomem, 0, R100_PMU_REG_SIZE);
    memory_region_add_subregion_overlap(sysmem,
                                        chiplet_base + R100_CPMU_PRIVATE_BASE,
                                        alias, 0);
}

/*
 * Create and map a SYSREG/SYSREMAP stub device for chiplet ID detection.
 *
 * Same dual-mapping pattern as PMU: config-space view + private alias.
 * BL1 reads SYSREG_SYSREMAP_PRIVATE for the local chiplet ID, while
 * the QSPI bridge accesses other chiplets via the config-space view.
 */
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
                             R100_SYSREG_REG_SIZE);
    memory_region_add_subregion_overlap(sysmem,
                                        chiplet_base + R100_SYSREMAP_PRIVATE_BASE,
                                        alias, 0);
}

/*
 * Create a stub RAM region for the PCIe sub-controller block.
 *
 * pmu_release_cm7() writes PCIE_GLOBAL_MASK, PCIE_GLOBAL_PEND,
 * PCIE_SFR_APP_CTRL_SIGNALS, and PCIE_SFR_PHY_RESET_OVRD during BL1 —
 * pure writes with no read-back check, so RAM semantics suffice.
 *
 * On -p silicon BL1 then calls cm7_wait_phy_sram_init_done(), which polls
 * four PHY{0..3}_SRAM_INIT_DONE registers for bit [0] to be set by the CM7
 * PCIe sub-controller firmware (common/headers/fw/pcie/cm7_pcie_common.c).
 * Remu doesn't run CM7 FW, so we seed bit [0] at reset; the poll exits on
 * iteration 0. The cm7_notify_load_done path uses separate SFR_PHY_CFG_*
 * registers that are RMW'd then checked via cm7_check_load_done() — plain
 * RAM already gives correct semantics there.
 */
static void r100_create_pcie_subctrl(MemoryRegion *cfg_mr, int chiplet_id)
{
    /* PHY{0..3}_SRAM_INIT_DONE offsets within PCIE_SUBCTRL (see
     * cm7_pcie_common.h: PCIE_PHY{0..3}_CFG_REG). */
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

/*
 * Create and map a PL330 DMA controller stub.
 *
 * BL1 uses this during _bl1_init_blk_rbc() to "load" UCIe PHY firmware
 * into each RBC's SRAM. The stub fakes completion without transferring
 * data (the RBC stub reports link-up without running PHY microcode).
 */
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
 * PVT monitor block bases (offsets from R100_CFG_BASE) and debug names.
 * 5 instances per chiplet — see pvt_base_address[] in
 * external/ssw-bundle/.../drivers/pvt_con/pvt_con.c.
 */
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

/*
 * Create and map the PVT (Process-Voltage-Temperature) monitor stubs.
 *
 * FreeRTOS's pvt_init() spins on PVT_CON_STATUS (+0x1C) waiting for the
 * idle bits, and bounded-waits on per-sensor validity bits. Without this
 * stub chiplet 0's primary CPU hangs in PVT_ENABLE_PROC_CONTROLLER's
 * "while (!ps_con_idle)" loop as soon as FreeRTOS takes over from BL31.
 */
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

/*
 * Create and map the SMMU-600 TCU register block stub.
 *
 * BL2's smmu_early_init() programs the event-queue / strtab registers
 * and then enables event queues via CR0, spinning on `CR0ACK & EVENTQEN`
 * until the write is acknowledged. It also fences GBPA by setting the
 * UPDATE bit and polling for it to clear. Without a device the polls
 * run forever, so the primary chiplet never reaches load_cl0_cp_images
 * and secondaries never see notify_cl0_cp_images_load_done.
 */
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
 * Create and map the DWC_SSI master-QSPI controller (QSPI_ROT).
 *
 * This is the controller the FW talks to for on-board NOR flash access —
 * BL1's qspi_boot_config()/bl1_load_fip_image() and the BL31 std-svc
 * NOR_FLASH_SVC_{READ_DATA,ERASE_4K,...} handlers both route through
 * external/.../q/sys/drivers/qspi_boot/qspi_boot.c, which hard-codes its
 * reg_base to QSPI_ROT_PRIVATE (0x1E00500000).
 *
 * Same dual-mapping pattern as PMU / SYSREG: the device's iomem sits in
 * the chiplet's config-space container at R100_QSPI_ROT_BASE, and an
 * alias of the same iomem is layered at chiplet_base + QSPI_ROT_PRIVATE
 * in the global sysmem. That way reads/writes from both the FW driver
 * (private alias) and any possible cross-chiplet config-space access
 * land on the same sparse register file — keeping drx writes coherent
 * with the status-byte model on DRX reads.
 *
 * Without this device FreeRTOS's postproc_cm7_logger_init() spins in
 * tx_available() polling DW_SSI_SR for 2 000 000 us after the shell
 * prompt and prints `spi tx available timeout error` to UART0.
 */
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
 * Create and map RBC (UCIe link) stub devices for a chiplet.
 *
 * Each RBC block is dual-mapped:
 *   - config-space view: r100_rbc_bases[i] (0x1FF5...) inside cfg_mr
 *   - private-alias view: 0x1E05... / 0x1E06... in sysmem (per-chiplet)
 *
 * BL2's wait_ucie_link_up_for_CP() reads its *own* chiplet's UCIe link
 * status via the private alias (BLK_RBC_Vxx_BASE = 0x1E05xxxxxxxx in
 * external/.../include/drivers/aw/ucie.h). Cross-chiplet reads go
 * through the QSPI bridge to the config-space view. Both need to route
 * to the same device instance so a linkup write stays coherent.
 *
 * The RBC block private-alias addresses differ from the config-space
 * addresses only in the top byte (0x1E vs 0x1FF0): offsets from their
 * respective window bases (PRIVATE_BASE / CFG_BASE) match. We compute
 * the private base by substituting PRIVATE_BASE for CFG_BASE.
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

        /* Private-alias mount: overlap priority 1 so it outranks the
         * chiplet-private-window unimplemented-device catch-all (prio 0). */
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
 * Create the QSPI NOR flash staging region.
 *
 * The R100 board has a single serial-NOR flash that every chiplet's QSPI
 * controller sees at the same local address (FLASH_BASE_ADDR = 0x1F80000000).
 * The FW's flash_nor_read() driver does a plain memcpy from that address
 * (see external/ssw-bundle/products/rebel/q/sys/drivers/qspi_boot/rl_serial_flash.c:flash_nor_read),
 * and BL1's print_ucie_link_speed() reads the HW-CFG struct at offset 0x5E000
 * right after the UCIe images are DMA-staged.
 *
 * Without this region the first CPU-direct flash read faults into the
 * unimplemented_device catch-all and stalls BL1. We model it as a single
 * RAM backing store, aliased into each chiplet's local flash window so
 * secondary chiplets see a coherent view.
 *
 * Zero-filled content is sufficient to progress past BL1: the magic-code
 * check at offset 0x5E000 misses and print_ucie_link_speed() falls through
 * to "UCIe speed: default". A real flash image can be preloaded by the CLI
 * with a -device loader,file=...,addr=0x1F80000000.
 */
static void r100_create_flash(MemoryRegion *sysmem)
{
    MemoryRegion *flash = g_new(MemoryRegion, 1);
    int chiplet_id;

    memory_region_init_ram(flash, NULL, "r100.flash", R100_FLASH_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, R100_FLASH_BASE, flash);

    /* Alias the same flash at each secondary chiplet's local address so
     * per-chiplet flash reads land on the same backing store. Chiplet 0 is
     * already covered by the primary mapping above. */
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
 * Initialize a single chiplet: memory regions, CPUs, and peripheral stubs.
 *
 * `memdev` (optional, chiplet 0 only): when non-NULL, the memory-backend
 * is spliced over the head of chiplet 0's DRAM at offset 0. The tail
 * (R100_DRAM_INIT_SIZE - memdev_size) stays plain lazy RAM. This gives
 * the x86 host guest's BAR0 and the NPU CA73 cores a coherent view of
 * the same bytes — the Phase 2 cross-process DRAM sharing path. The
 * memdev is silently ignored on secondary chiplets since silicon
 * exposes only chiplet 0's DRAM via PCIe BAR0. See docs/roadmap.md
 * Phase 2 / M5.
 */
static void r100_chiplet_init(MachineState *machine, int chiplet_id,
                              MemoryRegion *sysmem,
                              HostMemoryBackend *memdev)
{
    uint64_t chiplet_base = (uint64_t)chiplet_id * R100_CHIPLET_OFFSET;
    char name[64];
    MemoryRegion *dram, *irom, *iram, *sp_mem, *sh_mem, *cfg_mr;
    int i;

    /* --- DRAM ---
     * Chiplet 0 with memdev: container with shared backend at offset 0
     *                        and plain RAM covering the tail.
     * Every other case:      single plain RAM region. */
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

    /* --- Config space container (covers 0x1FF0000000 - 0x1FFB900000) ---
     * Also drop a chiplet-wide unimplemented_device underneath so that
     * any MMIO offset we haven't explicitly stubbed (NBUS/SBUS/EBUS
     * SYSREGs, etc.) returns 0 + LOG_UNIMP instead of "rejected" faults.
     */
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

    /* --- CMU stubs (one per block) --- */
    for (i = 0; i < R100_NUM_CMU_BLOCKS; i++) {
        r100_create_cmu(cfg_mr, chiplet_id, r100_cmu_bases[i],
                        r100_cmu_names[i]);
    }

    /*
     * --- Private alias window (unimplemented-device catch-all) ---
     * Covers the full chiplet-private 256 MB window at PRIVATE_BASE
     * (0x1E00000000). SYSREG_ROT_PRIVATE, SYSREMAP, PMU, OTP, GPIO, RBC,
     * CMU_RBC_* etc. all live here. Specific device aliases (PMU,
     * SYSREMAP) layer on top via overlap with higher priority.
     * unimplemented_device is lazy-allocated unlike RAM, so the 256 MB
     * region has no memory cost.
     */
    {
        char nm[64];
        snprintf(nm, sizeof(nm), "r100.chiplet%d.priv_win-unimpl", chiplet_id);
        create_unimplemented_device(g_strdup(nm),
                                    chiplet_base + R100_PRIVATE_WIN_BASE,
                                    R100_PRIVATE_WIN_SIZE);
    }

    /* --- PMU stub (config space + private alias) --- */
    r100_create_pmu(cfg_mr, sysmem, chiplet_id,
                    chiplet_id == 0 ? (R100_NUM_CHIPLETS - 1) : 0,
                    chiplet_base);

    /* --- SYSREG (chiplet ID) — config space + private alias --- */
    r100_create_sysreg(cfg_mr, sysmem, chiplet_id, chiplet_base);

    /*
     * --- SYSREG_CP{0,1} backing RAM ---
     *
     * BL1's plat_set_cpu_rvbar() writes each secondary CP0.cpu0's reset
     * vector to RVBARADDR0_LOW/HIGH inside SYSREG_CP0, via the QSPI
     * bridge (which targets the private alias). BL2's direct-MMIO path
     * for CP1 release writes to the absolute cross-chiplet-form config
     * address `CHIPLET_OFFSET * chiplet + SYSREG_CP0_OFFSET + PER_SYSREG_CP
     * * cluster`, and BL31's PSCI warm-boot path writes to the chiplet-
     * local config address. The PMU device then reads RVBAR back from
     * the matching per-cluster backing when a CPU_CONFIGURATION write
     * releases that core.
     *
     * Three separate access paths, one backing RAM per cluster per
     * chiplet. We triple-mount each RAM:
     *
     *   1. At the private-alias sysmem address (for QSPI bridge writes
     *      and any private-alias direct access).
     *   2. Inside the chiplet's cfg_mr at the SYSREG config-space offset
     *      (so absolute `chiplet_N * OFFSET + SYSREG_CP{0,1}_OFFSET`
     *      writes from BL2 running on chiplet N land on chiplet N's own
     *      RAM, not the unimpl catch-all).
     *   3. In the per-chiplet CPU view at the chiplet-local SYSREG
     *      config-space address (r100_build_chiplet_view handles this —
     *      needed for BL31 PSCI set_rvbar which uses the local form).
     *
     * Overlap priority 1 inside cfg_mr outranks the chiplet-wide
     * unimpl catch-all at priority 0.
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

    /* --- HBM3 controller stub --- */
    r100_create_hbm(cfg_mr, chiplet_id);

    /* --- SMMU-600 TCU stub (cr0/cr0ack mirror, gbpa update auto-clear) --- */
    r100_create_smmu(cfg_mr, chiplet_id);

    /* --- PVT monitor stubs (5 per chiplet; idle/valid bits pre-asserted) --- */
    r100_create_pvt_blocks(cfg_mr, chiplet_id);

    /* --- PL330 DMA controller stub (used by BL1 UCIe FW load) --- */
    r100_create_dma_pl330(cfg_mr, chiplet_id);

    /* --- QSPI bridge (inter-chiplet register access) --- */
    r100_create_qspi_bridge(cfg_mr, chiplet_id);

    /* --- QSPI_ROT master controller (DWC_SSI) — config space + private alias
     *     Required for FreeRTOS postproc_cm7_logger / BL31 NOR_FLASH SMCs
     *     (erase/write/read-status) — without it tx_available() times out
     *     after the FreeRTOS shell prompt. */
    r100_create_qspi_boot(cfg_mr, sysmem, chiplet_id, chiplet_base);

    /* --- RBC stubs (UCIe link status) — config space + private alias --- */
    r100_create_rbc_blocks(cfg_mr, sysmem, chiplet_id, chiplet_base);

    /*
     * --- D-cluster (DNC/SHM/MGLUE) and RBDMA cfg-window stubs ---
     *
     * q-cp on CP1 polls SHM INTR_VEC.tpg_done, RDSN MGLUE STATUS0 and
     * reads RBDMA IP_INFO{0..5} during `cp_create_tasks_impl`. Without
     * these stubs the reads fall to the chiplet-wide cfg_mr unimpl
     * catch-all (returns 0), SHM TPG training times out after ~1 s of
     * virtual time, and `abort_event(ERR_SHM)` fires. See src/machine/
     * r100_dnc.c for the override table.
     *
     * Priority 1 overlays on cfg_mr (same pattern as SYSREG_CP*, RBC)
     * so they outrank the unimpl catch-all at priority 0.
     */
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
            (void)dcls[k].tag;
        }
    }

    {
        DeviceState *dev = qdev_new(TYPE_R100_RBDMA);
        SysBusDevice *sbd;

        qdev_prop_set_uint32(dev, "chiplet-id", chiplet_id);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        memory_region_add_subregion_overlap(
            cfg_mr, R100_NBUS_L_RBDMA_CFG_BASE - R100_CFG_BASE,
            sysbus_mmio_get_region(sbd, 0), 1);
    }

    /* --- PCIe sub-controller catch-all RAM (for pmu_release_cm7) --- */
    r100_create_pcie_subctrl(cfg_mr, chiplet_id);

    /*
     * --- CSS600 generic counter region (4KB RAM) ---
     * generic_delay_timer_init/reset_counter writes to CNTCR/CNTCVL/CNTCVU.
     * QEMU CPU has its own generic timer; this stub just accepts the writes.
     */
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

    /*
     * TODO Phase 2: Add DNC, HDMA, doorbell/mailbox stubs
     */
}

/*
 * Build a per-chiplet "CPU view" MemoryRegion.
 *
 * On silicon, the 256 MB PRIVATE_BASE window (0x1E00000000) is a
 * chiplet-local address-decoder alias — each chiplet's CPUs see their
 * own private registers at that address. The FW reads CHIPLET_ID from
 * SYSREG_SYSREMAP_PRIVATE+0x444 and SECOND_CHIPLET_CNT from
 * CPMU_PRIVATE+0x840 via absolute PRIVATE_BASE addresses, relying on
 * this chiplet-local routing to know which chiplet it runs on.
 *
 * In REMU every chiplet's per-chiplet device aliases live in the
 * shared system_memory at `chiplet_base + 0x1E00000000`. Without
 * per-CPU views, every CPU's read of 0x1E00220444 lands on chiplet 0's
 * SYSREG instance, so BL2 on chiplets 1-3 reads CHIPLET_ID=0 and takes
 * the IS_PRIMARY_CHIPLET path incorrectly.
 *
 * This builds a container MR for each chiplet that:
 *   1. Aliases all of system_memory at offset 0 (shared peripherals,
 *      DRAM, config-space views of other chiplets — all still work).
 *   2. Overlays a 256 MB window at 0x1E00000000 that points to the
 *      chiplet's own slice of system_memory at chiplet_base+0x1E00000000.
 *      Overlap priority 10 makes this take precedence over the base
 *      sysmem alias.
 *   3. Overlays a chiplet-local view of SYSREG_CP0 at the config-space
 *      address 0x1FF1010000. BL31's rebel_h_pm.c:set_rvbar() writes the
 *      warm-boot entry for a PSCI CPU_ON target via this config-space
 *      address (not via the private alias that BL1's QSPI path uses),
 *      so without a chiplet-local route those writes land in the
 *      unimplemented-device catch-all. The r100-pmu device reads the
 *      RVBAR back from the private-alias copy when a CPU_CONFIGURATION
 *      write releases the core; aliasing both addresses to the same
 *      backing RAM keeps the two paths coherent and matches silicon,
 *      where each chiplet's SYSREG_CP0 is decoded locally at the global
 *      config address.
 *
 * The CPU "memory" link is set to this view, so its TLB walks hit the
 * chiplet-local overlay. Same pattern as hw/arm/xlnx-versal.c giving
 * APU/RPU clusters distinct views.
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

    /*
     * Chiplet-local alias of SYSREG_CP0 at its config-space address.
     * The target RAM is created by r100_chiplet_init() at the chiplet's
     * private-alias base (chiplet_base + R100_SYSREG_CP0_PRIVATE_BASE);
     * memory_region_init_alias resolves the sysmem view lazily so
     * order-of-init doesn't matter.
     */
    snprintf(name, sizeof(name), "r100.chiplet%d.sysreg_cp0_cfg_view",
             chiplet_id);
    memory_region_init_alias(cp0_cfg_alias, NULL, name, sysmem,
                             chiplet_base + R100_SYSREG_CP0_PRIVATE_BASE,
                             R100_SYSREG_CP0_SIZE);
    memory_region_add_subregion_overlap(view, R100_CP0_SYSREG_BASE,
                                        cp0_cfg_alias, 10);

    /*
     * Chiplet-local alias of SYSREG_CP1 at its config-space address.
     * BL2's plat_set_cpu_rvbar(CLUSTER_CP1, ...) writes the CP1 warm-boot
     * entry through this address; the PMU device reads it back from the
     * matching private-alias RAM on the CP1 CPU_CONFIGURATION release.
     */
    snprintf(name, sizeof(name), "r100.chiplet%d.sysreg_cp1_cfg_view",
             chiplet_id);
    memory_region_init_alias(cp1_cfg_alias, NULL, name, sysmem,
                             chiplet_base + R100_SYSREG_CP1_PRIVATE_BASE,
                             R100_SYSREG_CP0_SIZE);
    memory_region_add_subregion_overlap(view, R100_CP1_SYSREG_BASE,
                                        cp1_cfg_alias, 10);

    /*
     * Chiplet-local aliases for this chiplet's own GIC.
     *
     * r100_soc_init mounts each chiplet's arm-gicv3 in sysmem at
     * chiplet_base + R100_GIC_DIST_BASE / R100_GIC_REDIST_CP0_BASE (priority 1
     * to beat cfg_mr). Here we alias the FW-hardcoded chiplet-local addresses
     * back to that chiplet-absolute sysmem region, at priority 10 so the CPU
     * resolves its GIC reads here instead of falling through sysmem_alias to
     * chiplet 0's cfg_mr. The redist alias covers the full 8-frame 1 MB
     * window (CP0 frames at +0..3, CP1 frames at +4..7 = +0x80000 =
     * R100_GIC_REDIST_CP1_BASE). Aliases resolve lazily, so this is fine
     * even though the GIC is realized later.
     */
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

/*
 * Machine initialization: creates 4 chiplets, 32 CPUs, 4 per-chiplet GICs, UART.
 */
static void r100_soc_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *chiplet_views[R100_NUM_CHIPLETS];
    DeviceState *gic_dev[R100_NUM_CHIPLETS];
    Object *cpuobj;
    int chiplet, cluster, core, cpu_idx;
    int num_cpus = R100_NUM_CORES_TOTAL;
    const int cpus_per_gic = R100_NUM_CORES_PER_CHIPLET; /* 8 */

    /* Validate CPU count */
    if (machine->smp.cpus != num_cpus && machine->smp.cpus != 0) {
        error_report("R100 SoC requires exactly %d CPUs (got %d)",
                     num_cpus, machine->smp.cpus);
        exit(1);
    }

    /*
     * Build per-chiplet CPU memory views before CPU realization so that
     * each cpuobj can set its "memory" link. The views alias system_memory,
     * so mappings added later by r100_chiplet_init() become visible
     * automatically through the sysmem alias.
     */
    for (chiplet = 0; chiplet < R100_NUM_CHIPLETS; chiplet++) {
        chiplet_views[chiplet] = r100_build_chiplet_view(sysmem, chiplet);
    }

    /* --- Create CPUs --- */
    for (cpu_idx = 0; cpu_idx < num_cpus; cpu_idx++) {
        chiplet = cpu_idx / R100_NUM_CORES_PER_CHIPLET;
        cluster = (cpu_idx % R100_NUM_CORES_PER_CHIPLET) /
                  R100_NUM_CORES_PER_CLUSTER;
        core = cpu_idx % R100_NUM_CORES_PER_CLUSTER;

        cpuobj = object_new(machine->cpu_type);

        /*
         * MPIDR encoding: chiplet in bits [24:25], Aff1=cluster, Aff0=core.
         *
         * The R100 TF-A learns its chiplet identity from SYSREG
         * (SYSREMAP_CHIPLET at 0x1E00220444), not from MPIDR. Its
         * plat_core_pos_by_mpidr() masks out bits 24-31 and then requires
         * the remaining value to be <= 0xFFFF (i.e. Aff2 = Aff3 = 0),
         * returning Aff0 as the core position inside the chiplet. The
         * binary is built per-cluster (bl31_cp0.bin / bl31_cp1.bin), so
         * core positions only need to be unique within a cluster.
         *
         * Placing the chiplet id in the architectural "flags" band
         * (bits 24-31, which the FW masks off) keeps mp_affinity unique
         * across all 32 vCPUs for arm_set_cpu_on()/GIC routing while
         * letting the FW see Aff2 == 0 on every chiplet.
         */
        object_property_set_int(cpuobj, "mp-affinity",
                                ((uint64_t)chiplet << 24) |
                                    ((uint64_t)cluster << 8) | core,
                                &error_fatal);

        /*
         * Override MIDR to report Cortex-A73 r1p1 (0x411FD091). The R100 FW's
         * TF-A is built with cortex_a73 cpu_ops only, and asserts at
         * reset_handler if get_cpu_ops_ptr returns NULL (MIDR mismatch).
         * QEMU has no cortex-a73 model — cortex-a72 is the closest match.
         *
         * r1p1 (variant=1, revision=1) is past the range of all CA73 errata
         * workarounds. Otherwise the workarounds (e.g. 852427) try to access
         * implementation-defined system registers like S3_0_C15_C0_1 that
         * cortex-a72 doesn't model, triggering an UNDEF exception.
         */
        object_property_set_int(cpuobj, "midr", 0x411FD091, &error_fatal);

        /* Set reset vector to BL1 base within this chiplet's iRAM */
        object_property_set_int(cpuobj, "rvbar",
                                (uint64_t)chiplet * R100_CHIPLET_OFFSET +
                                R100_BL1_RO_BASE,
                                &error_fatal);

        /* Set timer frequency */
        object_property_set_int(cpuobj, "cntfrq", R100_CORE_TIMER_FREQ,
                                &error_fatal);

        /*
         * Route this CPU's memory accesses through its chiplet's view so
         * the chiplet-local PRIVATE_BASE window (CHIPLET_ID, SECOND_CHIPLET_CNT)
         * resolves to this chiplet's own device instances. The secure
         * address space automatically inherits from "memory" when
         * "secure-memory" is unset (see target/arm/cpu.c realize).
         */
        object_property_set_link(cpuobj, "memory",
                                 OBJECT(chiplet_views[chiplet]),
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

        /* Install Samsung IMPDEF system-reg stubs (cache flush IMPDEF). */
        define_arm_cp_regs(ARM_CPU(cpuobj), r100_samsung_impdef_regs);
    }

    /*
     * --- Per-chiplet GICv3 ---
     * Silicon has one GIC600 per chiplet. Each is modelled here as an
     * independent arm-gicv3 instance with num-cpu=8 (2 clusters * 4 cores)
     * and a single 8-frame redistributor region:
     *   GICD: 0x10000, 64 KB
     *   GICR: 0x100000, 1 MB, 8 frames contiguous
     *     frames 0..3 -> CP0.cpu0..3   (= R100_GIC_REDIST_CP0_BASE)
     *     frames 4..7 -> CP1.cpu0..3   (= R100_GIC_REDIST_CP1_BASE,
     *                                   i.e. CP0_BASE + 4 * 0x20000)
     *
     * Mounting strategy (same pattern as PMU, SYSREG, CMU, HBM, etc.):
     *   - Each chiplet's GIC MMIO is mounted in sysmem at the chiplet-absolute
     *     address `chiplet_base + R100_GIC_*_BASE`, using sysbus_mmio_map_overlap
     *     with priority 1 — so it outranks that chiplet's cfg_mr container
     *     (185 MB at chiplet_base + R100_CFG_BASE, priority 0) covering the
     *     same range.
     *   - r100_build_chiplet_view installs priority-10 aliases at the
     *     FW-local R100_GIC_*_BASE that point back at the chiplet-absolute
     *     sysmem regions. Aliases are lazy and propagate correctly even
     *     though the GIC is realized after the CPU views are built.
     *
     * Upstream arm-gicv3 always binds to global CPUs 0..num_cpu-1 via
     * qemu_get_cpu(i), so without extension multiple instances collide on
     * ICC_* cpreg registration. external/qemu carries a local patch adding
     * a first-cpu-index property (hw/intc/arm_gicv3_common.{c,h},
     * arm_gicv3_cpuif.c, arm_gicv3_kvm.c); we set it to chiplet*8 so GIC N
     * binds to global CPUs [8*N .. 8*N+7].
     */
    for (chiplet = 0; chiplet < R100_NUM_CHIPLETS; chiplet++) {
        SysBusDevice *gbd;
        uint64_t chiplet_base = (uint64_t)chiplet * R100_CHIPLET_OFFSET;

        gic_dev[chiplet] = qdev_new("arm-gicv3");
        qdev_prop_set_uint32(gic_dev[chiplet], "num-cpu", cpus_per_gic);
        qdev_prop_set_uint32(gic_dev[chiplet], "first-cpu-index",
                             (uint32_t)chiplet * cpus_per_gic);
        qdev_prop_set_uint32(gic_dev[chiplet], "num-irq", 256);
        qdev_prop_set_uint32(gic_dev[chiplet], "revision", 3);
        {
            QList *redist = qlist_new();
            qlist_append_int(redist, cpus_per_gic);
            qdev_prop_set_array(gic_dev[chiplet], "redist-region-count",
                                redist);
        }
        gbd = SYS_BUS_DEVICE(gic_dev[chiplet]);
        sysbus_realize_and_unref(gbd, &error_fatal);

        /* Priority 1 beats each chiplet's cfg_mr (priority 0). */
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
        }
    }

    /* --- Shared QSPI NOR flash staging region ---
     * Created once (not per-chiplet) since the physical flash is a single
     * chip shared by every chiplet's QSPI controller. Aliased into each
     * chiplet's local flash address inside r100_create_flash().
     */
    r100_create_flash(sysmem);

    /* --- Initialize all 4 chiplets --- */
    {
        R100SoCMachineState *r100m = R100_SOC_MACHINE(machine);
        HostMemoryBackend *memdev = r100_soc_resolve_memdev(r100m);
        for (chiplet = 0; chiplet < R100_NUM_CHIPLETS; chiplet++) {
            r100_chiplet_init(machine, chiplet, sysmem,
                              chiplet == 0 ? memdev : NULL);
        }
    }

    /*
     * --- 16550 UARTs (serial-mm), one per chiplet ---
     * The R100 FW uses a TI 16550 driver (see uart_helper.h), not PL011.
     * Reads LSR (offset 0x14 = reg 5 << 2) — 32-bit-stride MMIO layout.
     *
     * PLAT_UART0_PERI0 expands to CHIPLET_ADDR(UART0_PERI0_OFFSET) so each
     * chiplet N writes to N*CHIPLET_OFFSET + 0x1FF9040000. Give each
     * chiplet its own SerialMM instance bound to a distinct chardev
     * (serial_hd(N)) so the four chiplets' output is demuxed into
     * separate streams. If the user didn't supply four -serial/-chardev
     * options, the extra chiplets fall back to null. Each chiplet's UART IRQ
     * wires to its own chiplet's GICv3 (SPI 33).
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

        /* Priority 10 to outrank the cfg_mr catch-all + CMU stubs at
         * chiplet N's cfg offset 0x9040000. */
        memory_region_add_subregion_overlap(
            sysmem,
            (uint64_t)chiplet * R100_CHIPLET_OFFSET + R100_PERI0_UART0_BASE,
            uart_mr, 10);
    }

    /*
     * --- HILS log-buffer tail (chiplet 0's FreeRTOS .logbuf drain) ---
     *
     * FreeRTOS's RLOG_x / FLOG_x macros accumulate into a 2 MB ring at
     * physical 0x10000000 (see FreeRTOS.ld `.logbuf` at virt
     * 0x10010000000, with the kernel's +0x10000000000 virtual offset).
     * The in-FW drain path (terminal_task -> uart_putc) requires the
     * scheduler to be running, which is blocked today by the open PSCI
     * CPU_ON warm-boot item. r100-logbuf-tail bypasses the FW drain by
     * polling the ring from the emulator side and writing formatted
     * entries to its own chardev, so HILS logs become visible even
     * while init_smp() is still busy-waiting on secondary cores.
     *
     * We bind it to serial_hd(R100_NUM_CHIPLETS) -- the 5th `-serial`
     * slot. The CLI wires that to /tmp/remu_hils.log by default.
     */
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
     * --- PCIe doorbell ingress (Phase 2, M6) ---
     *
     * Instantiated only when `-machine r100-soc,doorbell=<chardev-id>`
     * is set. The chardev is a client Unix socket (see cli/remu_cli.py
     * _build_npu_cmd) wired to the x86 host-side r100-npu-pci's
     * matching server socket, so 8-byte (offset, value) frames emitted
     * on BAR4 MAILBOX_INTGR writes arrive here and pulse
     * R100_PCIE_DOORBELL_SPI on chiplet 0's GIC. See
     * src/machine/r100_doorbell.c for the wire format.
     */
    {
        R100SoCMachineState *r100m = R100_SOC_MACHINE(machine);

        if (r100m->doorbell_chardev_id != NULL &&
            *r100m->doorbell_chardev_id != '\0') {
            Chardev *chr = qemu_chr_find(r100m->doorbell_chardev_id);
            Chardev *dbg = NULL;
            DeviceState *db;

            if (chr == NULL) {
                error_report("r100-soc: doorbell chardev '%s' not found",
                             r100m->doorbell_chardev_id);
                exit(1);
            }
            if (r100m->doorbell_debug_chardev_id != NULL &&
                *r100m->doorbell_debug_chardev_id != '\0') {
                dbg = qemu_chr_find(r100m->doorbell_debug_chardev_id);
                if (dbg == NULL) {
                    error_report("r100-soc: doorbell debug chardev "
                                 "'%s' not found",
                                 r100m->doorbell_debug_chardev_id);
                    exit(1);
                }
            }

            db = qdev_new(TYPE_R100_DOORBELL);
            qdev_prop_set_chr(db, "chardev", chr);
            if (dbg) {
                qdev_prop_set_chr(db, "debug-chardev", dbg);
            }
            sysbus_realize_and_unref(SYS_BUS_DEVICE(db), &error_fatal);
            sysbus_connect_irq(SYS_BUS_DEVICE(db), 0,
                               qdev_get_gpio_in(gic_dev[0],
                                                R100_PCIE_DOORBELL_SPI));
        }
    }

    /*
     * --- Shared inter-chiplet mailbox RAM ---
     * TF-A's `mailbox_data[]` table pairs each IDX_MAILBOX_* instance
     * with an absolute base. ipm_samsung_write/receive dereference
     * `mailbox_data[inst].base` directly, so every chiplet view's
     * sysmem_alias needs a plain RAM backing at the same physical
     * addresses to make the ISSR handshake visible across chiplets:
     *
     *   Inbound (CL1/2/3 -> CL0), polled by CL0's BL2:
     *     0x1FF9120000  CL0 PERI0 M7 (CL1)
     *     0x1FF9130000  CL0 PERI0 M8 (CL2)
     *     0x1FF9930000  CL0 PERI1 M8 (CL3)
     *
     *   Outbound (CL0 -> CL1/2/3), polled by secondary BL2:
     *     0x3FF9120000  CL1 PERI0 M7 (= CL0 base + CHIPLET_OFFSET * 1)
     *     0x5FF9120000  CL2 PERI0 M7 (= CL0 base + CHIPLET_OFFSET * 2)
     *     0x7FF9120000  CL3 PERI0 M7 (= CL0 base + CHIPLET_OFFSET * 3)
     *
     * See external/.../tf-a/drivers/mailbox/mailbox.c:mailbox_data[].
     * Priority 10 outranks each chiplet's cfg_mr catch-all.
     */
    {
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
    }
}

/*
 * Accessors for the optional `memdev` string property on the machine.
 *
 * QEMU parses `-machine ...` options BEFORE it creates memory-backend-*
 * objects (system/vl.c: object_create_early() returns false for
 * "memory-backend-*"), so a link-style property would always fail with
 * "Device 'remushm' not found". We stash the id as a plain string and
 * resolve it to a HostMemoryBackend at machine-init time instead.
 */
static char *r100_soc_get_memdev(Object *obj, Error **errp)
{
    R100SoCMachineState *r100m = R100_SOC_MACHINE(obj);
    return g_strdup(r100m->memdev_id);
}

static void r100_soc_set_memdev(Object *obj, const char *value, Error **errp)
{
    R100SoCMachineState *r100m = R100_SOC_MACHINE(obj);
    g_free(r100m->memdev_id);
    r100m->memdev_id = g_strdup(value);
}

static char *r100_soc_get_doorbell(Object *obj, Error **errp)
{
    R100SoCMachineState *r100m = R100_SOC_MACHINE(obj);
    return g_strdup(r100m->doorbell_chardev_id);
}

static void r100_soc_set_doorbell(Object *obj, const char *value, Error **errp)
{
    R100SoCMachineState *r100m = R100_SOC_MACHINE(obj);
    g_free(r100m->doorbell_chardev_id);
    r100m->doorbell_chardev_id = g_strdup(value);
}

static char *r100_soc_get_doorbell_debug(Object *obj, Error **errp)
{
    R100SoCMachineState *r100m = R100_SOC_MACHINE(obj);
    return g_strdup(r100m->doorbell_debug_chardev_id);
}

static void r100_soc_set_doorbell_debug(Object *obj, const char *value,
                                        Error **errp)
{
    R100SoCMachineState *r100m = R100_SOC_MACHINE(obj);
    g_free(r100m->doorbell_debug_chardev_id);
    r100m->doorbell_debug_chardev_id = g_strdup(value);
}

static void r100_soc_machine_instance_init(Object *obj)
{
    object_property_add_str(obj, "memdev",
                            r100_soc_get_memdev, r100_soc_set_memdev);
    object_property_set_description(obj, "memdev",
        "Optional memory-backend-* id spliced over chiplet 0 DRAM head; "
        "shared with the Phase-2 x86 host QEMU so BAR0 and the CA73 "
        "cores see the same bytes.");

    object_property_add_str(obj, "doorbell",
                            r100_soc_get_doorbell, r100_soc_set_doorbell);
    object_property_set_description(obj, "doorbell",
        "Optional chardev id that receives 8-byte (BAR4 offset, value) "
        "frames from the Phase-2 x86 host QEMU's r100-npu-pci BAR4 and "
        "injects them as GIC SPI " G_STRINGIFY(R100_PCIE_DOORBELL_SPI)
        " on chiplet 0.");

    object_property_add_str(obj, "doorbell-debug",
                            r100_soc_get_doorbell_debug,
                            r100_soc_set_doorbell_debug);
    object_property_set_description(obj, "doorbell-debug",
        "Optional chardev id that receives an ASCII trace of every "
        "doorbell frame observed by the NPU (one line per frame).");
}

static void r100_soc_machine_instance_finalize(Object *obj)
{
    R100SoCMachineState *r100m = R100_SOC_MACHINE(obj);
    g_free(r100m->memdev_id);
    g_free(r100m->doorbell_chardev_id);
    g_free(r100m->doorbell_debug_chardev_id);
}

/*
 * Resolve `-machine memdev=<id>` to the HostMemoryBackend instance
 * created by the late-backend phase. Returns NULL if no id was given.
 * Any other failure (id set but object missing / wrong type) exits.
 */
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
