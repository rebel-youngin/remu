/*
 * REMU - R100 NPU System Emulator
 * Address map constants extracted from q-sys g_sys_addrmap.h and platform_def.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef REMU_ADDRMAP_H
#define REMU_ADDRMAP_H

/* ========================================================================
 * Chiplet topology
 * ======================================================================== */

#define R100_NUM_CHIPLETS           4
#define R100_NUM_CLUSTERS           2       /* CP0, CP1 per chiplet */
#define R100_NUM_CORES_PER_CLUSTER  4
#define R100_NUM_CORES_PER_CHIPLET  (R100_NUM_CLUSTERS * R100_NUM_CORES_PER_CLUSTER)
#define R100_NUM_CORES_TOTAL        (R100_NUM_CHIPLETS * R100_NUM_CORES_PER_CHIPLET)

#define R100_CHIPLET_OFFSET         0x2000000000ULL

/* ========================================================================
 * Per-chiplet physical address map (offsets from chiplet base)
 * Chiplet N base = N * R100_CHIPLET_OFFSET
 * ======================================================================== */

/* DRAM */
#define R100_DRAM_BASE              0x0000000000ULL
#define R100_DRAM_SIZE              0x1000000000ULL  /* 64GB max, 1GB initial */
#define R100_DRAM_INIT_SIZE         0x0040000000ULL  /* 1GB for emulation */
#define R100_DRAM_SECURE_BASE       0x003F000000ULL
#define R100_DRAM_USER_BASE         0x0040000000ULL

/* Boot images in DRAM */
#define R100_CP0_BL31_BASE          0x0000000000ULL
#define R100_CP0_BL31_SIZE          0x0000200000ULL  /* 2MB */
#define R100_CP0_FREERTOS_BASE      0x0000200000ULL
#define R100_CP1_BL31_BASE          0x0014100000ULL
#define R100_CP1_BL31_SIZE          0x0000100000ULL  /* 1MB */
#define R100_CP1_FREERTOS_BASE      0x0014200000ULL

/* iROM / iRAM (chiplet-local alias region) */
#define R100_IROM_BASE              0x1E00000000ULL
#define R100_IROM_SIZE              0x0000010000ULL  /* 64KB */
#define R100_IRAM_BASE              0x1E00010000ULL
#define R100_IRAM_SIZE              0x0000040000ULL  /* 256KB */

/* BL1 load address = iRAM base */
#define R100_BL1_RO_BASE            0x1E00010000ULL

/* Scratchpad and shared memory */
#define R100_SP_MEM_BASE            0x1FE0000000ULL
#define R100_DCL0_SP_MEM_BASE       0x1FE0000000ULL
#define R100_DCL0_SP_MEM_SIZE       0x0002000000ULL  /* 32MB */
#define R100_DCL1_SP_MEM_BASE       0x1FE2000000ULL
#define R100_DCL1_SP_MEM_SIZE       0x0002000000ULL  /* 32MB */
#define R100_SP_MEM_TOTAL_SIZE      0x0004000000ULL  /* 64MB */

#define R100_SH_MEM_BASE            0x1FE4000000ULL
#define R100_DCL0_SH_MEM_BASE       0x1FE4000000ULL
#define R100_DCL0_SH_MEM_SIZE       0x0002000000ULL  /* 32MB */
#define R100_DCL1_SH_MEM_BASE       0x1FE6000000ULL
#define R100_DCL1_SH_MEM_SIZE       0x0002000000ULL  /* 32MB */
#define R100_SH_MEM_TOTAL_SIZE      0x0004000000ULL  /* 64MB */

/* ========================================================================
 * Config space (0x1FF0000000 - 0x1FFB900000)
 * ======================================================================== */

#define R100_CFG_BASE               0x1FF0000000ULL
#define R100_CFG_LIMIT              0x1FFB900000ULL
#define R100_CFG_SIZE               (R100_CFG_LIMIT - R100_CFG_BASE)

/* --- RoT block --- */
#define R100_ROT_CFG_BASE           0x1FF0000000ULL
#define R100_ROT_ROM_BASE           0x1FF0000000ULL
#define R100_ROT_RAM_BASE           0x1FF0010000ULL
#define R100_ROT_CMU_BASE           0x1FF0200000ULL
#define R100_ROT_SYSREG_BASE        0x1FF0210000ULL
#define R100_ROT_SYSREMAP_BASE      0x1FF0220000ULL
#define R100_ROT_PMU_BASE           0x1FF0230000ULL
#define R100_ROT_GPIO_BASE          0x1FF0240000ULL
#define R100_ROT_OTP_BASE           0x1FF0250000ULL
#define R100_ROT_MAILBOX_M0_BASE    0x1FF0280000ULL
#define R100_ROT_MAILBOX_M1_BASE    0x1FF0290000ULL
#define R100_ROT_MAILBOX_M2_BASE    0x1FF02A0000ULL
#define R100_ROT_PVT_CON_BASE       0x1FF0260000ULL

/* Master DWC_SSI QSPI (BL1 qspi_boot_config + FreeRTOS/BL31 NOR SMC).
 * FW reg_base = private alias (rebel_h_baseoffset.h QSPI_ROT_REG_OFFSET
 * + PRIVATE_BASE). ~0x150 B DWC_SSI regs. */
#define R100_QSPI_ROT_BASE          0x1FF0500000ULL
#define R100_QSPI_ROT_PRIVATE_BASE  0x1E00500000ULL
#define R100_QSPI_ROT_REG_SIZE      0x0000010000ULL  /* 64 KB, same as other SFR blocks */

/* --- CP0 block --- */
#define R100_CP0_CFG_BASE           0x1FF1000000ULL
#define R100_CP0_CMU_BASE           0x1FF1000000ULL
#define R100_CP0_SYSREG_BASE        0x1FF1010000ULL
#define R100_CP0_MAILBOX_M3_BASE    0x1FF1040000ULL
#define R100_CP0_MAILBOX_M4_BASE    0x1FF1050000ULL

/* --- CP1 block --- */
#define R100_CP1_CFG_BASE           0x1FF1800000ULL
#define R100_CP1_CMU_BASE           0x1FF1800000ULL
#define R100_CP1_SYSREG_BASE        0x1FF1810000ULL
#define R100_CP1_MAILBOX_M3_BASE    0x1FF1840000ULL
#define R100_CP1_MAILBOX_M4_BASE    0x1FF1850000ULL

/* --- DCL0 block (D-Cluster 0) --- */
#define R100_DCL0_CFG_BASE          0x1FF2000000ULL
#define R100_DCL0_DNC0_CFG_BASE     0x1FF2000000ULL
#define R100_DCL0_CMU_BASE          0x1FF2100000ULL
#define R100_DCL0_SYSREG_BASE       0x1FF2110000ULL
#define R100_DCL0_PVT_CON0_BASE     0x1FF2120000ULL
#define R100_DCL0_PVT_CON1_BASE     0x1FF2130000ULL

/* --- DCL1 block (D-Cluster 1) --- */
#define R100_DCL1_CFG_BASE          0x1FF2800000ULL
#define R100_DCL1_DNC0_CFG_BASE     0x1FF2800000ULL
#define R100_DCL1_CMU_BASE          0x1FF2900000ULL
#define R100_DCL1_SYSREG_BASE       0x1FF2910000ULL
#define R100_DCL1_PVT_CON0_BASE     0x1FF2920000ULL
#define R100_DCL1_PVT_CON1_BASE     0x1FF2930000ULL

/* DCLx CFG: 1 MB. Packs 8 DNC slots (stride 0x2000), 16 SHM banks
 * (stride 0x400), MGLUE/RDSN-head at 0x020000. q-cp shm_init/rdsn_init
 * poll status from CP1 — see r100_dnc.c. */
#define R100_DCL_CFG_SIZE           0x100000ULL

/* Within a DCL block (offsets from R100_DCLx_CFG_BASE) */
#define R100_DNC_SLOT_BASE          0x000000ULL
#define R100_DNC_SLOT_STRIDE        0x002000ULL
#define R100_DNC_SLOT_COUNT         8
#define R100_SHM_BANK_BASE          0x010000ULL
#define R100_SHM_BANK_STRIDE        0x000400ULL
#define R100_SHM_BANK_COUNT         16
#define R100_MGLUE_BASE             0x020000ULL

/* --- NBUS blocks --- */
#define R100_NBUS_U_CMU_BASE        0x1FF3000000ULL
#define R100_NBUS_D_CMU_BASE        0x1FF3300000ULL
#define R100_NBUS_L_CMU_BASE        0x1FF3500000ULL
#define R100_NBUS_L_RBDMA_CFG_BASE  0x1FF3700000ULL
#define R100_NBUS_L_RBDMA_CFG_SIZE  0x100000ULL    /* 0x1FF3700000 .. 0x1FF3800000 */

/* --- PL330 DMA controller (used by BL1 for UCIe firmware load) --- */
#define R100_DMA_PL330_BASE         0x1FF02C0000ULL
#define R100_DMA_PL330_SIZE         0x0000001000ULL  /* 4KB: struct pl330 */

/* --- GIC600 --- */
#define R100_GIC_DIST_BASE          0x1FF3800000ULL
#define R100_GIC_REDIST_CP0_BASE    0x1FF3840000ULL
#define R100_GIC_REDIST_CP1_BASE    0x1FF38C0000ULL
#define R100_GIC_SIZE               0x0004000000ULL

/* --- SBUS blocks --- */
#define R100_SBUS_U_CMU_BASE        0x1FF3A00000ULL
#define R100_SBUS_D_CMU_BASE        0x1FF3C00000ULL
#define R100_SBUS_D_SYSREG_BASE     0x1FF3C10000ULL  /* chiplet ID in SYSREMAP */
#define R100_SBUS_L_CMU_BASE        0x1FF3E00000ULL

/* --- WBUS blocks --- */
#define R100_WBUS_U_CMU_BASE        0x1FF4000000ULL
#define R100_WBUS_D_CMU_BASE        0x1FF4600000ULL

/* --- SMMU-600 TCU (Translation Control Unit) --- */
#define R100_SMMU_TCU_BASE          0x1FF4200000ULL  /* TCU_OFFSET from rebel_h_baseoffset.h */

/* --- EBUS blocks --- */
#define R100_EBUS_U_CMU_BASE        0x1FF4800000ULL
#define R100_EBUS_D_CMU_BASE        0x1FF4A00000ULL
#define R100_EBUS_R_CMU_BASE        0x1FF4C00000ULL

/* --- RBC blocks (UCIe) --- */
#define R100_RBC_V00_CMU_BASE       0x1FF5000000ULL
#define R100_RBC_V01_CMU_BASE       0x1FF5400000ULL
#define R100_RBC_V10_CMU_BASE       0x1FF5800000ULL
#define R100_RBC_V11_CMU_BASE       0x1FF5C00000ULL
#define R100_RBC_H00_CMU_BASE       0x1FF6000000ULL
#define R100_RBC_H01_CMU_BASE       0x1FF6400000ULL

/* --- DRAM controller --- */
#define R100_DRAM_CMU_BASE          0x1FF7000000ULL
#define R100_DRAM_SYSREG_BASE       0x1FF7010000ULL
#define R100_DRAM_CNTL_BASE         0x1FF7400000ULL

/* Board QSPI NOR flash (CPU-visible via DWC direct-read window, same
 * local addr on each chiplet). FW flash_nor_read memcpy's (FLASH_BASE +
 * off). Layout (rebel_h_img_info.h):
 *   0x00000..0x7FFFF : part 0 HW-CFG/SW-CFG; 0x5E000 = NVMEM_HW_CFG
 *   0x80000+         : GPT part 1 (tboot_*)
 * Blank flash is enough for BL1 (hw-cfg magic-miss → default UCIe speed). */
#define R100_FLASH_BASE             0x1F80000000ULL
#define R100_FLASH_SIZE             0x0004000000ULL  /* 64 MB */

/* --- PCIe block --- */
#define R100_PCIE_INTMEM_BASE       0x1FF8000000ULL
#define R100_PCIE_CMU_BASE          0x1FF8100000ULL
#define R100_PCIE_SYSREG_BASE       0x1FF8110000ULL
/* PCIe Samsung-IPM mailbox cluster: VF0..VF15 + PF, 4 KB stride. Base =
 * VF0 (q-sys CA73 listener on __TARGET_CP==0). PF hosts the KMD BAR4
 * TLP landing and bootdone_notify_to_host(PCIE_PF). See r100_soc.c. */
#define R100_PCIE_MAILBOX_BASE      0x1FF8160000ULL  /* cluster base = VF0 */
#define R100_PCIE_MAILBOX_SFR_STRIDE 0x1000ULL       /* per-function SFR */
#define R100_PCIE_MAILBOX_PF_BASE   (R100_PCIE_MAILBOX_BASE + \
                                     16 * R100_PCIE_MAILBOX_SFR_STRIDE)

/* --- PERI0 block --- */
#define R100_PERI0_CMU_BASE         0x1FF9000000ULL
#define R100_PERI0_SYSREG_BASE      0x1FF9010000ULL
#define R100_PERI0_UART0_BASE       0x1FF9040000ULL
#define R100_PERI0_UART1_BASE       0x1FF9050000ULL
#define R100_PERI0_GPIO_BASE        0x1FF90B0000ULL
/* PERI0_MAILBOX_M{9,10} / PERI1_MAILBOX_M{9,10} — q-cp DNC
 * task-queue Samsung-IPM SFRs. q-cp CP1.cpu0 sits in
 * taskmgr_fetch_dnc_task_master_cp1 polling MBTQ_PI_IDX (ISSR[0])
 * across all four (poll-based, no IRQ). q-cp's
 * `_inst[HW_SPEC_DNC_QUEUE_NUM=4]` table
 * (`q/cp/src/task_mgr/cp1/mb_task_queue.c:44`) maps cmd_types ->
 * mailbox indices:
 *   [0] COMPUTE -> PERI0_MAILBOX_M9_CPU1
 *   [1] UDMA    -> PERI0_MAILBOX_M10_CPU1
 *   [2] UDMA_LP -> PERI1_MAILBOX_M9_CPU1
 *   [3] UDMA_ST -> PERI1_MAILBOX_M10_CPU1
 * (DDMA / DDMA_HIGHP exist as common_cmd_type enum values but flow
 * through the auto-fetch DDMA_AF path, not these mailboxes.)
 *
 * On silicon PCIE_CM7 firmware writes 24 B dnc_one_task entries +
 * bumps PI to dispatch work; q-cp on CP0 owns this push natively
 * post-P1c via mtq_push_task. All four mailboxes are nevertheless
 * instantiated as real r100-mailbox blocks on chiplet 0 (P3) so
 * mtq_init's writes to MBTQ_PI_IDX/CI_IDX persist correctly across
 * cmd_types. */
#define R100_PERI0_MAILBOX_M9_BASE  0x1FF9140000ULL
#define R100_PERI0_MAILBOX_M10_BASE 0x1FF9150000ULL
#define R100_PERI1_MAILBOX_M9_BASE  0x1FF9940000ULL
#define R100_PERI1_MAILBOX_M10_BASE 0x1FF9950000ULL

/* --- PERI1 block --- */
#define R100_PERI1_CMU_BASE         0x1FF9800000ULL
#define R100_PERI1_SYSREG_BASE      0x1FF9810000ULL
#define R100_PERI1_UART0_BASE       0x1FF9840000ULL

/* mb_task_queue ring layout in ISSR (q-cp mb_task_queue.c:33-42).
 * 64 ISSR slots: [0]=PI, [1]=CI, [2..3]=reserved, [4..51]=8 entries. */
#define R100_MBTQ_PI_IDX            0u
#define R100_MBTQ_CI_IDX            1u
#define R100_MBTQ_QUEUE_IDX         4u
#define R100_MBTQ_ENTRY_SIZE_WORD   6u    /* 24 B per dnc_one_task */
#define R100_MBTQ_ENTRY_MAX         8u    /* power-of-2 */

/* ========================================================================
 * CPU / Timer constants
 * ======================================================================== */

#define R100_CORE_TIMER_FREQ        500000000ULL  /* 500MHz */
#define R100_UART_CLOCK             250000000ULL  /* 250MHz */

/* ========================================================================
 * SFR (Special Function Register) common size
 * ======================================================================== */

#define R100_SFR_SIZE               0x10000ULL    /* 64KB per SFR block */
#define R100_CMU_BLOCK_SIZE         0x10000ULL    /* 64KB per CMU block */

/* ========================================================================
 * Per-chiplet "private alias" window (PRIVATE_BASE in q-sys headers).
 * Re-maps the same SYSREMAP and PMU device instances at chiplet-local
 * addresses just above iRAM. Used by FW via macros like CPMU_PRIVATE
 * and SYSREG_SYSREMAP_PRIVATE.
 * ======================================================================== */

#define R100_PRIVATE_BASE               0x1E00000000ULL
#define R100_SYSREMAP_PRIVATE_BASE      0x1E00220000ULL  /* SYSREG_SYSREMAP private alias */
#define R100_CPMU_PRIVATE_BASE          0x1E00230000ULL  /* PMU private alias */
#define R100_SYSREG_CP0_PRIVATE_BASE    0x1E01010000ULL  /* SYSREG_CP0 private alias (RVBAR etc.) */
#define R100_SYSREG_CP0_SIZE            0x10000ULL       /* 64KB */
/* SYSREG_CP1 mirrors CP0 at +0x800000 (cfg + private). BL2
 * set_rvbar writes CP0+PER_SYSREG_CP*1; PMU reads back from
 * matching private alias on CPU_CONFIGURATION release. */
#define R100_PER_SYSREG_CP              0x00800000ULL
#define R100_SYSREG_CP1_PRIVATE_BASE    (R100_SYSREG_CP0_PRIVATE_BASE + \
                                         R100_PER_SYSREG_CP)

/* Private-alias catch-all (SYSREG_ROT/SYSREMAP/CPMU/OTP/GPIO PRIVATE).
 * PMU/SYSREMAP device aliases layer on top via overlap. */
#define R100_PRIVATE_WIN_BASE           0x1E00000000ULL
#define R100_PRIVATE_WIN_SIZE           0x0010000000ULL  /* 256 MB */

/* ========================================================================
 * PCIe sub-controller block (catch-all for pmu_release_cm7 writes)
 * ======================================================================== */

#define R100_PCIE_SUBCTRL_BASE          0x1FF8180000ULL
#define R100_PCIE_SUBCTRL_SIZE          0x0000010000ULL  /* 64KB */

/* ARM CSS600 generic counter generator (CSS600_CNTGEN). FW writes CNTCR,
 * CNTCVL, CNTCVU to enable/reset the system counter. Reads/writes only. */
#define R100_CSS600_CNTGEN_BASE         0x1FFB809000ULL
#define R100_CSS600_CNTGEN_SIZE         0x0000001000ULL

/* ========================================================================
 * PMU register offsets (relative to PMU base)
 * ======================================================================== */

#define R100_PMU_OM_STAT                0x0000  /* boot-mode select; bits[2:0] */
#define R100_PMU_OM_MASK                0x7
#define R100_PMU_BOOT_MODE_NORMAL       0x5     /* NORMAL_BOOT in rebel_h_bootmgr.h */

#define R100_PMU_RST_STAT               0x0404
#define R100_PMU_RESET_MASK             0x040C
#define R100_PMU_INFORM4                0x0840
#define R100_PMU_INFORM7                0x084C
#define R100_PMU_CPU0_CONFIGURATION     0x2000
#define R100_PMU_CPU0_STATUS            0x2004
#define R100_PMU_CPU0_OPTION            0x2008
#define R100_PMU_PERCPU_OFFSET          0x0080
#define R100_PMU_PERCLUSTER_OFFSET      0x0200
/* CPU_CONFIGURATION bits (platform_def.h): AUTO_WAKEUP=BIT(31),
 * INITIATE_WAKEUP=0xF<<16, LOCAL_PWR_ON=0xF. */
#define R100_PMU_CPU_CFG_LOCAL_PWR_MASK 0xFU
/* SYSREG_CP* per-CPU RVBAR (platform_def.h). PMU reads back on release. */
#define R100_SYSREG_RVBARADDR0_LOW      0x354
#define R100_SYSREG_RVBARADDR0_HIGH     0x358
#define R100_SYSREG_PERCPU_RVBAR_OFF    0x8
#define R100_PMU_CP0_NONCPU_CONFIG      0x2400
#define R100_PMU_CP0_NONCPU_STATUS      0x2404
/* Per-cluster NONCPU = CP0_NONCPU + PERNONCPU_OFFSET*cluster.
 * BL2 plat_pmu_cl_on(CP1) polls CP1_NONCPU_STATUS (0x2444) & 0xF == 0xF. */
#define R100_PMU_PERNONCPU_OFFSET       0x0040
#define R100_PMU_CP1_NONCPU_CONFIG      (R100_PMU_CP0_NONCPU_CONFIG + \
                                         R100_PMU_PERNONCPU_OFFSET)
#define R100_PMU_CP1_NONCPU_STATUS      (R100_PMU_CP0_NONCPU_STATUS + \
                                         R100_PMU_PERNONCPU_OFFSET)
#define R100_PMU_CP0_L2_CONFIG          0x2600
#define R100_PMU_CP0_L2_STATUS          0x2604
#define R100_PMU_CP1_L2_CONFIG          0x2620
#define R100_PMU_CP1_L2_STATUS          0x2624
#define R100_PMU_DCL0_CONFIG            0x4280
#define R100_PMU_DCL0_STATUS            0x4284
#define R100_PMU_DCL1_CONFIG            0x42A0
#define R100_PMU_DCL1_STATUS            0x42A4

/* RBC (UCIe) per-block CONFIG/STATUS within PMU. STATUS=CONFIG+4. */
#define R100_PMU_RBCH00_CONFIG          0x42C0
#define R100_PMU_RBCH00_STATUS          0x42C4
#define R100_PMU_RBCH01_CONFIG          0x42E0
#define R100_PMU_RBCH01_STATUS          0x42E4
#define R100_PMU_RBCV00_CONFIG          0x4300
#define R100_PMU_RBCV00_STATUS          0x4304
#define R100_PMU_RBCV01_CONFIG          0x4320
#define R100_PMU_RBCV01_STATUS          0x4324
#define R100_PMU_RBCV10_CONFIG          0x4340
#define R100_PMU_RBCV10_STATUS          0x4344
#define R100_PMU_RBCV11_CONFIG          0x4360
#define R100_PMU_RBCV11_STATUS          0x4364

/* PMU status values. RST_STAT is a bitmask of reset reasons; FW's
 * FSB() spins until any bit is set. PINRESET = POR/cold (rebel_h_pmu.h). */
#define R100_PMU_COLD_RESET             (1U << 16)  /* PINRESET */
#define R100_PMU_CHIPLET_RESET          0x1
#define R100_PMU_CPU_STATUS_ON          0xF
#define R100_PMU_CL_STATUS_ON           0xF
#define R100_PMU_L2_STATUS_ON           0x1
#define R100_PMU_DCL_STATUS_ON          0xF
#define R100_PMU_LOCAL_PWR_ON           0xF

/* ========================================================================
 * SYSREMAP register offsets (chiplet ID detection)
 * ======================================================================== */

#define R100_SYSREMAP_REMAP_NOC     0x0400
#define R100_SYSREMAP_REMAP_NIC     0x0404
#define R100_SYSREMAP_CHIPLET_ID    0x0444

/* ========================================================================
 * PCIe / Host interface (for Phase 2)
 * ======================================================================== */

#define R100_PCI_VENDOR_ID          0x1eff
#define R100_PCI_DEVICE_ID_CR03     0x2030  /* CR03 quad, ASIC_REBEL_QUAD */
#define R100_MSIX_ENTRIES           32

/* BAR IDs */
#define R100_DDR_BAR_ID             0
#define R100_ACP_BAR_ID             2
#define R100_DOORBELL_BAR_ID        4
#define R100_MSI_BAR_ID             5

/* BAR4 doorbell: INTGR triggers + MAILBOX_BASE shadow of NPU
 * ISSR0..63 (rebel_regs.h MAILBOX_BASE/SIZE; ipm_samsung +0x80).
 * M6/M8a chardev bridge details in src/machine/r100_cm7.c. */
#define R100_BAR4_MAILBOX_INTGR0    0x00000008u  /* db_idx >= 32 */
#define R100_BAR4_MAILBOX_INTGR1    0x0000001cu  /* db_idx <  32 */
#define R100_BAR4_MAILBOX_BASE      0x00000080u
#define R100_BAR4_MAILBOX_COUNT     64u          /* 64 u32 = 256 B */
#define R100_BAR4_MAILBOX_END       \
    (R100_BAR4_MAILBOX_BASE + R100_BAR4_MAILBOX_COUNT * 4u)
/* Host-side MMIO-trapped head of BAR4 (INTGR + MAILBOX_BASE body). */
#define R100_BAR4_MMIO_SIZE         0x1000u

/* Chiplet-0 PCIe mailbox (r100-mailbox) IRQ wiring.
 *
 * These are raw GICv3 INTIDs, matching FW mailbox_data[].irq_num_low
 * (drivers/mailbox/mailbox.c) which is the value passed to
 * gic_irq_enable()/ISENABLER. INTID 185 (G1/INTMSR1) is the VF0
 * mailbox IRQ that ipm_samsung_isr handles as IDX_MAILBOX_PCIE_VF0
 * on __TARGET_CP==0 (KMD INTGR1 path). INTID 184 (G0/INTMSR0) is a
 * placeholder for the unmodelled PCIE_CM7 subscriber; no FW ISR
 * binds to it, but we wire it for completeness.
 *
 * IMPORTANT: QEMU's arm_gicv3 exposes incoming SPI lines as
 * gpio_in[0..num_spi-1], where gpio_in[N] maps to INTID (N +
 * GIC_INTERNAL) with GIC_INTERNAL=32. So to wire an SPI to the GIC
 * at a given INTID, the gpio_in index is (INTID - GIC_INTERNAL).
 * Silicon-side docs and FW both speak "INTID"; use the macro below
 * at call sites. An off-by-32 here silently routes mailbox IRQs to
 * a *different* INTID — on REMU that collided with the PERI0_M7
 * mailbox's ISR slot, caused an IRQ storm on CA73 CPU0, and froze
 * bootdone_task — see docs/debugging.md for the full post-mortem. */
#define R100_GIC_INTERNAL           32
#define R100_INTID_TO_GIC_SPI_GPIO(intid)  ((intid) - R100_GIC_INTERNAL)

#define R100_PCIE_MBX_GROUP0_INTID  184
#define R100_PCIE_MBX_GROUP1_INTID  185

/* Integrated MSI-X trigger (M7, FW→host). DW PCIe on silicon snoops
 * REBELH_PCIE_MSIX_ADDR = 0x1BFFFFFFFC; matching writes become MSI-X
 * TLPs. db_data: [28:24]PF [23:16]VF [15]VF-Act [14:12]TC [10:0]Vec.
 * REMU: r100-imsix MMIO overlay → (off, db_data) chardev frame →
 * host r100-npu-pci msix_notify(). PF/VF/TC ignored (single PF today). */
#define R100_PCIE_IMSIX_BASE        0x1BFFFFF000ULL
#define R100_PCIE_IMSIX_SIZE        0x0000001000ULL   /* 4 KB page */
#define R100_PCIE_IMSIX_DB_OFFSET   0x00000FFCU
#define R100_PCIE_IMSIX_VECTOR_MASK 0x000007FFU       /* db_data[10:0] */

/* ========================================================================
 * M9-1c — DNC completion + HDMA register block + cmd_descr synth
 * ======================================================================== */

/* DNC completion INTIDs (q/cp/include/hal/interrupt.h, transcribed
 * verbatim — values are NON-CONTIGUOUS so don't compute by addition).
 *
 * Each DNC asserts six SPIs starting at the EXCEPTION base in order:
 *   {EXCEPTION, COMPUTE, UDMA, UDMA_LP, UDMA_ST, TASK16B}
 * q-cp's dnc_X_done_handler binds all six via gic_irq_connect; the
 * handler demuxes on done_passage.done_rpt1.cmd_type, so REMU only
 * needs to deliver the INTID matching the kicked cmd_type. r100_dnc.c
 * uses r100_dnc_intid(dnc_id, cmd_type) below to look up the right
 * one before pulsing the GIC SPI line. The dual-DCL layout maps
 * DCL0 → DNC0..7, DCL1 → DNC8..15 (g_sys_addrmap.h). */
#define R100_HW_SPEC_DNC_COUNT          16u
#define R100_HW_SPEC_DNC_IRQ_NUM        6u    /* {EXC,COMP,UDMA,LP,ST,T16B} */
#define R100_HW_SPEC_DNC_CMD_TYPE_NUM   5u    /* without EXCEPTION */

static const uint32_t r100_dnc_exc_intid_table[R100_HW_SPEC_DNC_COUNT] = {
    [0]  = 410, [1]  = 422, [2]  = 416, [3]  = 428,
    [4]  = 434, [5]  = 446, [6]  = 440, [7]  = 452,
    [8]  = 570, [9]  = 582, [10] = 576, [11] = 588,
    [12] = 594, [13] = 606, [14] = 600, [15] = 612,
};

/* Spot-checks — if Samsung re-shuffles a future header release these
 * trip a compile error rather than silently shipping a bad LUT. */
_Static_assert(R100_HW_SPEC_DNC_COUNT == 16,
               "R100 DNC count must match interrupt.h");
/* Compile-time array-length assertions hit a -Wpedantic edge under
 * static const uint32_t[]; runtime cardinality is enforced by
 * r100_dnc_intid()'s bounds check below. */

/* Map (dnc_id, cmd_type) → SPI INTID of the matching DNC done-handler
 * line. cmd_type uses the COMMON_CMD_TYPE_* enum (COMPUTE=0..UDMA_ST=3),
 * with the extra DNC-private TASK16B reachable via cmd_type=4. Returns
 * 0 (an invalid SPI) on out-of-range input — the caller drops the
 * trigger and emits LOG_GUEST_ERROR rather than randomly pulsing an
 * unrelated SPI. */
static inline uint32_t r100_dnc_intid(uint32_t dnc_id, uint32_t cmd_type)
{
    if (dnc_id >= R100_HW_SPEC_DNC_COUNT ||
        cmd_type >= R100_HW_SPEC_DNC_CMD_TYPE_NUM) {
        return 0;
    }
    return r100_dnc_exc_intid_table[dnc_id] + 1u + cmd_type;
}

/* HDMA single-line completion INTID (q/cp/include/hal/interrupt.h:267).
 * All 32 channels (16 WR + 16 RD) share this SPI; per-channel "which
 * one finished" lives in SUBCTRL_EDMA_INT_CA73 (offset below into the
 * existing pcie-subctrl plain-RAM block). The handler reads the
 * pending bitmap, services each set bit, and write-1-clears. */
#define R100_INT_ID_HDMA                186u
#define R100_PCIE_SUBCTRL_EDMA_INT_CA73_OFF 0x4368u   /* in subctrl 64 KB */

/* RBDMA INTIDs (q/cp/include/hal/interrupt.h:69-70).
 *   INT_ID_RBDMA0_ERR = 977 — error / abort path (REMU never synthesises).
 *   INT_ID_RBDMA1     = 978 — finish-FIFO completion line driven by the
 *                            r100-rbdma kick-BH (P4A; see r100_rbdma.c).
 * Both are within `num-irq = 992` configured on the per-chiplet GICv3
 * in r100_soc.c. Per-chiplet wiring: q-cp's `rbdma_init(cl_id)` runs on
 * every CA73 CP0 and binds rbdma_done_handler to INT_ID_RBDMA1 on its
 * local GIC, so each chiplet's r100-rbdma must connect its done line to
 * the matching gic_dev[chiplet]. */
#define R100_INT_ID_RBDMA0_ERR          977u
#define R100_INT_ID_RBDMA1              978u

/* SMMU-600 TCU non-secure wired SPIs (q/sys/drivers/smmu/smmu.c:31-67).
 *   SMMU_EVT_IRQ  = 762 — non-secure event queue overflow / new entry.
 *                          q-sys connects smmu_event_intr() here, which
 *                          drains EVENTQ via SMMU_EVENTQ_PROD/CONS and
 *                          calls smmu_print_event() for every entry.
 *   SMMU_GERR_IRQ = 765 — global error (CMDQ_ERR / EVTQ_ABT / SFM_ERR /
 *                          MSI_*_ABT_ERR). q-sys's smmu_gerr_intr() reads
 *                          GERROR ^ GERRORN, services each active bit,
 *                          and write-1-clears via GERRORN.
 * (763 = TCU_CMD_SYNC_NS_ID — referenced by FW source as a label but
 * the CMD_SYNC completion path uses the in-DRAM MSI write-back trick,
 * not a wired SPI; we leave 763 unused.)
 *
 * IRQ_CTRL gates both lines: q-sys writes IRQ_CTRL =
 * IRQ_CTRL_GERROR_IRQEN | IRQ_CTRL_EVENTQ_IRQEN, so r100-smmu must
 * respect both bits before pulsing. Wiring lives in r100_soc.c
 * r100_create_smmu — both lines on the same chiplet's GIC
 * (gic_dev[chiplet_id]). The `num-irq = 992` cap fits both. */
#define R100_INT_ID_SMMU_EVT            762u
#define R100_INT_ID_SMMU_GERR           765u

/* HDMA register block (q/cp/src/hal/hdma/hdma_if.c hdma_init_dev).
 *   hdma_base = cl_id * CHIPLET_INTERVAL + U_PCIE_CORE_OFFSET +
 *               PCIE_HDMA_OFFSET
 * U_PCIE_CORE_OFFSET = 0x1C00000000 (rebel_h_baseoffset.h:384).
 * PCIE_HDMA_OFFSET   = 0x180380000  (hdma_if.c:82).
 * Per-channel stride from HDMA_REG_{WR,RD}_CH_OFFSET with ch_sep=3:
 *   stride = 2 << (3+7) = 0x800; WR ch_n at 2*ch*stride, RD at
 *   (2*ch+1)*stride. 16 ch each → 32 stride slots × 0x800 = 0x10000.
 * Round up to 0x20000 to give head-room for vsec common regs. */
#define R100_U_PCIE_CORE_OFFSET     0x1C00000000ULL
#define R100_PCIE_HDMA_OFFSET       0x180380000ULL
#define R100_HDMA_BASE              (R100_U_PCIE_CORE_OFFSET + \
                                     R100_PCIE_HDMA_OFFSET)
#define R100_HDMA_SIZE              0x20000ULL
#define R100_HDMA_CH_STRIDE         0x800u
#define R100_HDMA_CH_COUNT          16u    /* per direction (WR or RD) */

/* DesignWare dw_hdma_v0 per-channel register offsets (hdma_regs.h —
 * struct hdma_ch_regs layout, offsets are per-element byte offsets
 * from the WR/RD slot base computed by R100_HDMA_CH_STRIDE). */
#define R100_HDMA_CH_REG_ENABLE         0x00u    /* RW0 = idle */
#define R100_HDMA_CH_REG_DOORBELL       0x04u    /* HDMA_DB_START / STOP */
#define R100_HDMA_CH_REG_ELEM_PF        0x08u    /* LL prefetch hint (RAZ/WI) */
#define R100_HDMA_CH_REG_HANDSHAKE      0x0Cu    /* HW handshake (RAZ/WI) */
#define R100_HDMA_CH_REG_LLP_LO         0x10u    /* LL head address low */
#define R100_HDMA_CH_REG_LLP_HI         0x14u    /* LL head address high */
#define R100_HDMA_CH_REG_CYCLE          0x18u    /* cycle.state / cycle.bit */
#define R100_HDMA_CH_REG_XFER_SIZE      0x1Cu
#define R100_HDMA_CH_REG_SAR_LO         0x20u
#define R100_HDMA_CH_REG_SAR_HI         0x24u
#define R100_HDMA_CH_REG_DAR_LO         0x28u
#define R100_HDMA_CH_REG_DAR_HI         0x2Cu
#define R100_HDMA_CH_REG_WATERMARK      0x30u    /* watermark IRQ enable */
#define R100_HDMA_CH_REG_CTRL1          0x34u    /* LLEN / TYPE / RO / ... */
#define R100_HDMA_CH_REG_FUNC_NUM       0x38u    /* PF/VF select (RAZ/WI) */
#define R100_HDMA_CH_REG_QOS            0x3Cu    /* weight / pf-depth / TC */
#define R100_HDMA_CH_REG_STATUS         0x80u    /* enum hdma_status */
#define R100_HDMA_CH_REG_INT_STATUS     0x84u
#define R100_HDMA_CH_REG_INT_SETUP      0x88u    /* RAIE/LAIE/RSIE/LSIE/... */
#define R100_HDMA_CH_REG_INT_CLEAR      0x8Cu
#define R100_HDMA_CH_REG_MSI_STOP_LO    0x90u
#define R100_HDMA_CH_REG_MSI_STOP_HI    0x94u
#define R100_HDMA_CH_REG_MSI_WATERMARK_LO 0x98u
#define R100_HDMA_CH_REG_MSI_WATERMARK_HI 0x9Cu
#define R100_HDMA_CH_REG_MSI_ABORT_LO   0xA0u
#define R100_HDMA_CH_REG_MSI_ABORT_HI   0xA4u
#define R100_HDMA_CH_REG_MSI_MSGD       0xA8u    /* MSI message data */

#define R100_HDMA_ENABLE_BIT            (1u << 0)
#define R100_HDMA_DB_START_BIT          (1u << 0)
#define R100_HDMA_DB_STOP_BIT           (1u << 1)

/* CTRL1 bit definitions (hdma_regs.h HDMA_CTRL1_*). Only LLEN is
 * load-bearing for the P5 LL walker — the rest are kept here as
 * documentation so the regstore round-trips them faithfully on the
 * test path q-cp's dump_regs uses. */
#define R100_HDMA_CTRL1_LLEN_BIT        (1u << 0)
#define R100_HDMA_CTRL1_TYPE_BIT        (1u << 1)
#define R100_HDMA_CTRL1_RO_BIT          (1u << 4)

/* hdma_status enum (q/cp/.../hdma_regs.h). q-cp polls for STOPPED. */
#define R100_HDMA_STATUS_RUNNING        0u
#define R100_HDMA_STATUS_STOPPED        1u
#define R100_HDMA_STATUS_ABORTED        2u

/* int_status / int_clear bits. */
#define R100_HDMA_INT_STOP_BIT          (1u << 0)
#define R100_HDMA_INT_ABORT_BIT         (1u << 1)
#define R100_HDMA_INT_WATERMARK_BIT     (1u << 2)

/* P5: dw_hdma_v0 linked-list element shapes — sourced from
 * external/.../q/cp/common/headers/public/qman_if_common.h. Each LL
 * chain is a sequence of `dw_hdma_v0_lli` (24 B: ctrl, transfer_size,
 * sar.reg, dar.reg) with optional `dw_hdma_v0_llp` (16 B: ctrl,
 * reserved, llp.reg) jumps when q-cp's record_desc transitions to a
 * new desc-buf chunk. The `control` field is shared across both shapes
 * so the walker can pre-read the first 4 bytes to discriminate.
 *
 * CB     = element valid (cycle bit; q-cp always sets to 1)
 * LLP    = element is a jump (LLP shape, follow llp.reg)
 * LIE    = local-IRQ-enable on this LLI's completion (chain end marker
 *          for our walker — q-cp's record_lli sets this on the last
 *          data element, and a trailing record_llp(0,0) terminator
 *          follows but the walker stops on LIE before reaching it). */
#define R100_HDMA_LL_CTRL_CB            (1u << 0)
#define R100_HDMA_LL_CTRL_TCB           (1u << 1)
#define R100_HDMA_LL_CTRL_LLP           (1u << 2)
#define R100_HDMA_LL_CTRL_LIE           (1u << 3)
#define R100_HDMA_LL_CTRL_RIE           (1u << 4)

#define R100_HDMA_LLI_SIZE              24u    /* sizeof(dw_hdma_v0_lli) */
#define R100_HDMA_LLP_SIZE              16u    /* sizeof(dw_hdma_v0_llp) */

/* Safety cap for the synchronous LL walker. Real chains are typically
 * a handful of LLIs (one per contiguous PA range, q-cp splits at 2 MB
 * page boundaries). 1024 elements covers a 2 GB chain without any
 * runtime overhead, while still bailing out on a malformed cycle-bit
 * loop instead of spinning the vCPU forever. */
#define R100_HDMA_LL_MAX_ELEMS          1024u

/* Per-chunk size for D2D in-NPU copies inside one LLI. The LLI's
 * transfer_size can be up to 2 MB (q-cp's contiguous_size cap); we
 * loop in 64 KB strides to keep the on-stack scratch reasonable. */
#define R100_HDMA_D2D_CHUNK             (64u * 1024u)

/* req_id partitioning on the `hdma` chardev (remu_hdma_proto.h).
 *   0x00..0x7F   — reserved. The legacy cm7 BD-done partition lived
 *                  at 0x01..0x0F until P7 retired the FSM, and the
 *                  P1b r100-cm7 OP_CFG_WRITE reverse-emit at 0x00
 *                  was retired with the shm-backed `cfg-shadow`
 *                  alias (host x86 QEMU and NPU QEMU now share a
 *                  4 KB memory-backend-file over BAR2 cfg-head /
 *                  cfg-mirror traps; no chardev round trip). UMQ
 *                  multi-queue may reclaim this range.
 *   0x80..0xBF   — r100-hdma MMIO-driven channel ops (encoded as
 *                  0x80 | (type<<5) | ch with type=0 WR, type=1 RD,
 *                  ch in 0..15).
 *   0xC0..0xFF   — r100-pcie-outbound synchronous PF-window reads
 *                  (cookie = req_id & 0x3F, rotates on each request,
 *                  only one in flight at a time). */
#define R100_HDMA_REQ_ID_CH_MASK_BASE   0x80u
#define R100_HDMA_REQ_ID_CH_DIR_RD      0x20u    /* 1<<5 */
#define R100_HDMA_REQ_ID_CH_NUM_MASK    0x1Fu    /* fits 0..15 */

#define R100_PCIE_OUTBOUND_REQ_ID_BASE  0xC0u
#define R100_PCIE_OUTBOUND_REQ_ID_MASK  0x3Fu    /* cookie space */

/* ========================================================================
 * P1 — PCIe outbound iATU window + DEVICE_COMMUNICATION_SPACE mirror
 * ========================================================================
 *
 * Real silicon: q-cp/CP0 dereferences host-RAM bus addresses (BD ring,
 * rbln_device_desc, queue_desc, packet stream, ...) by issuing AXI
 * loads/stores in the chiplet-0 iATU outbound window. The DW iATU
 * translates AXI[cpu_addr ± offset] → PCIe TLP[pci_addr ± offset];
 * pcie_ep.c:418 programs PF cpu_addr=0x8000000000, pci_addr=0x0,
 * size=4 GB. Per-VF outbound windows start at +(vf+1)*4GB but are
 * not exercised today (PF-only).
 *
 * REMU has neither the DW PCIe IP nor a real iATU. r100-pcie-outbound
 * (src/machine/r100_pcie_outbound.c) installs a 4 GB MMIO trap at
 * R100_PCIE_AXI_SLV_BASE_ADDR; reads block on a HDMA OP_READ_REQ /
 * OP_READ_RESP round-trip via the existing `hdma` chardev (req_id
 * partition 0xC0..0xFF, see above), writes are fire-and-forget OP_WRITE.
 * The MMIO `addr` parameter is already the PCIe bus address — the iATU
 * mapping is identity after the AXI base is subtracted by QEMU's
 * MemoryRegion bookkeeping.
 *
 * BAR2 cfg-head mirror: real silicon's inbound iATU maps host BAR2
 * onto NPU local memory at the FW_LOGBUF_SIZE offset, so kmd's BAR2
 * stores at FW_LOGBUF_SIZE + 0xC0/0xC4 (DDH_BASE_LO/HI) end up readable
 * by q-cp through `hil_reg_base[0] + DDH_BASE_LO` (= NPU DRAM
 * 0x102000C0). REMU's BAR2 lives on the host side (lazy RAM + cfg-head
 * trap forwarding to the NPU via `cfg` chardev). r100-cm7's cfg
 * delivery now write-throughs to NPU DRAM at this address so q-cp's
 * `hil_init_descs` / FUNC_READQ-style accesses find the value the kmd
 * wrote.
 */
#define R100_PCIE_AXI_SLV_BASE_ADDR     0x8000000000ULL
#define R100_PCIE_OUTBOUND_PF_SIZE      0x100000000ULL   /* 4 GB */

#define R100_DEVICE_COMM_SPACE_BASE     0x10200000ULL    /* NPU DRAM */
#define R100_DEVICE_COMM_SPACE_SIZE     0x1000ULL        /* 4 KB */

#endif /* REMU_ADDRMAP_H */
