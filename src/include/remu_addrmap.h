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

/* --- DCL1 block (D-Cluster 1) --- */
#define R100_DCL1_CFG_BASE          0x1FF2800000ULL
#define R100_DCL1_DNC0_CFG_BASE     0x1FF2800000ULL
#define R100_DCL1_CMU_BASE          0x1FF2900000ULL
#define R100_DCL1_SYSREG_BASE       0x1FF2910000ULL

/* --- NBUS blocks --- */
#define R100_NBUS_U_CMU_BASE        0x1FF3000000ULL
#define R100_NBUS_D_CMU_BASE        0x1FF3300000ULL
#define R100_NBUS_L_CMU_BASE        0x1FF3500000ULL
#define R100_NBUS_L_RBDMA_CFG_BASE  0x1FF3700000ULL

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

/* --- QSPI NOR flash staging region ---
 *
 * The R100 board has a single serial-NOR flash connected via QSPI. The FW's
 * flash_nor_read() driver memcpy's directly from (FLASH_BASE_ADDR + offset),
 * so the flash must be visible to the CPU as plain memory. In silicon this
 * is the DWC QSPI controller's memory-mapped "direct read" window; on each
 * chiplet the same physical flash is visible at the same local address.
 *
 * Layout (see external/ssw-bundle/.../rebel_h_img_info.h):
 *   0x00000 - 0x7FFFF : board/binning/HW-CFG/SW-CFG area (partition 0)
 *   0x5E000           : NVMEM_FLASH_HW_CFG_ADDR (read by print_ucie_link_speed)
 *   0x80000+          : GPT partition 1 (tboot_s, tboot_p0/p1, tboot_u/n, ...)
 *
 * A blank (zero-filled) flash is enough to unblock BL1: the hw-cfg magic-code
 * check misses and the FW falls back to default UCIe speed.
 */
#define R100_FLASH_BASE             0x1F80000000ULL
#define R100_FLASH_SIZE             0x0004000000ULL  /* 64 MB */

/* --- PCIe block --- */
#define R100_PCIE_INTMEM_BASE       0x1FF8000000ULL
#define R100_PCIE_CMU_BASE          0x1FF8100000ULL
#define R100_PCIE_SYSREG_BASE       0x1FF8110000ULL
#define R100_PCIE_MAILBOX_BASE      0x1FF8160000ULL

/* --- PERI0 block --- */
#define R100_PERI0_CMU_BASE         0x1FF9000000ULL
#define R100_PERI0_SYSREG_BASE      0x1FF9010000ULL
#define R100_PERI0_UART0_BASE       0x1FF9040000ULL
#define R100_PERI0_UART1_BASE       0x1FF9050000ULL
#define R100_PERI0_GPIO_BASE        0x1FF90B0000ULL

/* --- PERI1 block --- */
#define R100_PERI1_CMU_BASE         0x1FF9800000ULL
#define R100_PERI1_SYSREG_BASE      0x1FF9810000ULL
#define R100_PERI1_UART0_BASE       0x1FF9840000ULL

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

/*
 * Catch-all RAM window for the per-chiplet "private" alias region.
 * Covers SYSREG_ROT_PRIVATE, SYSREG_SYSREMAP_PRIVATE, CPMU_PRIVATE,
 * OTP_CON_PRIVATE, OTP_CON_CPU_PRIVATE, GPIO_ROT_PRIVATE, etc. The specific
 * device aliases (PMU, SYSREMAP) are layered on top via overlap regions.
 */
#define R100_PRIVATE_WIN_BASE           0x1E00000000ULL
#define R100_PRIVATE_WIN_SIZE           0x0010000000ULL  /* 256 MB (full chiplet private alias) */

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
#define R100_PMU_CP0_NONCPU_CONFIG      0x2400
#define R100_PMU_CP0_NONCPU_STATUS      0x2404
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

/* PMU status values */
/*
 * RST_STAT is a bitmask of reset reasons. The FW's FSB() macro spins
 * until at least one bit is set. PINRESET (bit 16) indicates POR/cold
 * boot — see rebel_h_pmu.h.
 */
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

#endif /* REMU_ADDRMAP_H */
