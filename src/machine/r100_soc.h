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
#define TYPE_R100_UNIMPL        "r100-unimpl"

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

#define R100_HBM_REG_SIZE       0x10000
#define R100_HBM_REG_COUNT      (R100_HBM_REG_SIZE / 4)

struct R100HBMState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_HBM_REG_COUNT];
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
