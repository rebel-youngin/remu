/*
 * REMU - R100 NPU System Emulator
 * Machine type and device model declarations
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef R100_SOC_H
#define R100_SOC_H

#include "hw/sysbus.h"
#include "hw/arm/boot.h"
#include "qom/object.h"
#include "remu_addrmap.h"

/* ========================================================================
 * QOM type names
 * ======================================================================== */

#define TYPE_R100_SOC_MACHINE   "r100-soc-machine"
#define TYPE_R100_CMU           "r100-cmu"
#define TYPE_R100_PMU           "r100-pmu"
#define TYPE_R100_SYSREG        "r100-sysreg"
#define TYPE_R100_HBM           "r100-hbm"
#define TYPE_R100_QSPI_BRIDGE   "r100-qspi-bridge"
#define TYPE_R100_RBC           "r100-rbc"
#define TYPE_R100_DMA_PL330     "r100-dma-pl330"
#define TYPE_R100_SMMU          "r100-smmu"
#define TYPE_R100_PVT           "r100-pvt"
#define TYPE_R100_LOGBUF        "r100-logbuf-tail"
#define TYPE_R100_UNIMPL        "r100-unimpl"

#define R100_RBC_BLOCK_SIZE     0x80000ULL  /* 512KB per RBC block */

/* ========================================================================
 * CMU (Clock Management Unit) device
 * ======================================================================== */

#define R100_CMU_REG_COUNT      (R100_CMU_BLOCK_SIZE / 4)

struct R100CMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_CMU_REG_COUNT];
    uint32_t chiplet_id;
    char *name;  /* e.g., "CP0", "ROT", "DCL0" for debug */
};

typedef struct R100CMUState R100CMUState;

DECLARE_INSTANCE_CHECKER(R100CMUState, R100_CMU, TYPE_R100_CMU)

/* ========================================================================
 * PMU (Power Management Unit) device
 * ======================================================================== */

#define R100_PMU_REG_SIZE       0x10000
#define R100_PMU_REG_COUNT      (R100_PMU_REG_SIZE / 4)

struct R100PMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_PMU_REG_COUNT];
    uint32_t chiplet_id;
    uint32_t secondary_chiplet_count;
};

/*
 * Pull in the ARM power-control helpers (arm_set_cpu_on / off) so the
 * PMU can release vCPUs from reset in response to CPU_CONFIGURATION
 * writes. Matches what TF-A's `plat_pmu_cpu_on()` does on silicon —
 * writing LOCAL_PWR_ON to CPU_CONFIGURATION is the reset-release signal.
 */

typedef struct R100PMUState R100PMUState;

DECLARE_INSTANCE_CHECKER(R100PMUState, R100_PMU, TYPE_R100_PMU)

/* ========================================================================
 * SYSREG (System Register / Chiplet ID) device
 * ======================================================================== */

#define R100_SYSREG_REG_SIZE    0x10000
#define R100_SYSREG_REG_COUNT   (R100_SYSREG_REG_SIZE / 4)

struct R100SysregState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_SYSREG_REG_COUNT];
    uint32_t chiplet_id;
};

typedef struct R100SysregState R100SysregState;

DECLARE_INSTANCE_CHECKER(R100SysregState, R100_SYSREG, TYPE_R100_SYSREG)

/* ========================================================================
 * HBM3 controller stub
 * ======================================================================== */

/*
 * HBM3 controller covers 16 memory channels at 0x40000 stride plus
 * 16 PHY blocks at 0x10000 stride plus the ICON block, all in a
 * contiguous 6MB window from 0x1FF7400000. See hbm3.h for the layout.
 * The stub returns 0xFFFFFFFF for unwritten offsets (which satisfies
 * dfi_init_complete and other "training ready" polls) and remembers
 * individual writes in a sparse hash so ioctl-style RMW patterns work.
 */
#define R100_HBM_REG_SIZE       0x600000

struct R100HBMState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    GHashTable *regs;     /* hwaddr -> uint32_t (sparse write-back store) */
    uint32_t chiplet_id;
};

typedef struct R100HBMState R100HBMState;

DECLARE_INSTANCE_CHECKER(R100HBMState, R100_HBM, TYPE_R100_HBM)

/* ========================================================================
 * PL330 DMA controller stub
 *
 * Fake-completion stub: BL1's dma_load_image() polls ch_stat[0].csr and
 * dbgcmd for completion. Returning zero on those reads satisfies the poll.
 * Destination contents are never consumed (RBC stubs report UCIe link-up
 * without running PHY microcode), so no real memcpy is performed.
 * ======================================================================== */

#define R100_DMA_REG_COUNT      (R100_DMA_PL330_SIZE / 4)

struct R100DMAPl330State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_DMA_REG_COUNT];
    uint32_t chiplet_id;
};

typedef struct R100DMAPl330State R100DMAPl330State;

DECLARE_INSTANCE_CHECKER(R100DMAPl330State, R100_DMA_PL330, TYPE_R100_DMA_PL330)

/* ========================================================================
 * SMMU-600 TCU stub (Arm SMMU-v3 register block at TCU_OFFSET = 0x1FF4200000)
 *
 * BL2's smmu_early_init() programs the event queue / strtab / global
 * bypass and enables event queues via CR0. Without an ack-mirror device
 * it hangs on `while (!(cr0ack & EVENTQEN_MASK))`. We implement a tiny
 * register file that mirrors CR0 into CR0ACK and auto-clears the GBPA
 * UPDATE bit so the complete-poll exits.
 * ======================================================================== */

#define R100_SMMU_REG_SIZE      0x10000
#define R100_SMMU_REG_COUNT     (R100_SMMU_REG_SIZE / 4)

struct R100SMMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_SMMU_REG_COUNT];
    uint32_t chiplet_id;
};

typedef struct R100SMMUState R100SMMUState;

DECLARE_INSTANCE_CHECKER(R100SMMUState, R100_SMMU, TYPE_R100_SMMU)

/* ========================================================================
 * PVT controller stub (Samsung Process-Voltage-Temperature monitor)
 *
 * FreeRTOS's pvt_init() (external/.../drivers/pvt_con/pvt_con.c) spins on
 * PVT_CON_STATUS.ps_con_idle / ts_con_idle inside PVT_ENABLE_*_CONTROLLER
 * macros, and bounded-waits on per-sensor ps_valid/vs_valid/ts_valid bits.
 * Without a device stub the status register is unmodelled and the unbounded
 * idle-poll hangs the primary CPU just after FreeRTOS enters.
 *
 * The stub is a 64 KB register file that:
 *   - returns 0x3 on reads of PVT_CON_STATUS (+0x1C) so the ps/ts idle
 *     polls (bits 0 & 1) exit immediately;
 *   - returns 0x1 on per-sensor *_status reads so PVT_WAIT_UNTIL_VALID
 *     exits on the first iteration instead of burning its 10 000-count
 *     timeout per sensor;
 *   - reads/write-backs everything else.
 * ======================================================================== */

#define R100_PVT_REG_SIZE       0x10000
#define R100_PVT_REG_COUNT      (R100_PVT_REG_SIZE / 4)

struct R100PVTState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_PVT_REG_COUNT];
    uint32_t chiplet_id;
    char *name;  /* e.g. "ROT", "DCL0_0", "DCL0_1" for debug */
};

typedef struct R100PVTState R100PVTState;

DECLARE_INSTANCE_CHECKER(R100PVTState, R100_PVT, TYPE_R100_PVT)

/* ========================================================================
 * Unimplemented region (catch-all for unmapped config space reads)
 * Returns 0 on read, ignores writes, logs access for debugging
 * ======================================================================== */

struct R100UnimplState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    char *name;
    uint64_t size;
};

typedef struct R100UnimplState R100UnimplState;

DECLARE_INSTANCE_CHECKER(R100UnimplState, R100_UNIMPL, TYPE_R100_UNIMPL)

#endif /* R100_SOC_H */
