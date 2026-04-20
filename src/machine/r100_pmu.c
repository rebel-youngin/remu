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
 *   - CP{0,1}_NONCPU_STATUS (0x2404 / 0x2444): cluster power status
 *     (FW computes as CP0_NONCPU_STATUS + PERNONCPU_OFFSET * cluster)
 *   - CP{0,1}_L2_STATUS (0x2604 / 0x2624): L2 cache power status
 *   - DCL0_STATUS (0x4284): D-Cluster power status
 *
 * Writes to CPU_CONFIGURATION release (or halt) the target AP core; we
 * translate them into QEMU arm_set_cpu_on()/arm_set_cpu_off() calls so
 * BL1's reset_release_secondary_cp0() actually starts the secondary
 * chiplets' CP0.cpu0 at the RVBAR that was previously staged via the
 * QSPI bridge. See external/.../rebel_h_plat.c:plat_pmu_cpu_on().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "target/arm/arm-powerctl.h"
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

    /*
     * Cluster power ON for both CP0 and CP1. BL2's plat_pmu_cl_on(cluster)
     * polls CP0_NONCPU_STATUS + PERNONCPU_OFFSET*cluster & 0xF == 0xF
     * before proceeding to plat_pmu_cpu_on() for the CP1.cpu0 release.
     * Without the CP1 seed, that poll spins forever on all four chiplets.
     */
    s->regs[R100_PMU_CP0_NONCPU_STATUS >> 2] = R100_PMU_CL_STATUS_ON;
    s->regs[R100_PMU_CP1_NONCPU_STATUS >> 2] = R100_PMU_CL_STATUS_ON;

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

/*
 * Read back the 64-bit RVBAR previously written to this chiplet's
 * SYSREG_CP{0,1} by the FW. Returns 0 if the backing RAM region is
 * unmapped or access fails — callers treat 0 as "no override" and
 * skip the release.
 *
 * Three release paths converge here, all routed through the same
 * backing RAM via dual-mount aliases:
 *   - BL1 cold-boot:   plat_set_cpu_rvbar() via QSPI bridge → writes to
 *                      the target chiplet's SYSREG_CP0 private alias.
 *   - BL2 CP1 release: plat_set_cpu_rvbar(CLUSTER_CP1, ...) direct MMIO
 *                      → writes to SYSREG_CP1 config-space address,
 *                      overlaid onto the private-alias RAM by the
 *                      per-chiplet CPU view.
 *   - BL31 PSCI CPU_ON: rebel_h_pm.c:set_rvbar() direct MMIO → same
 *                       dual-mount config-space address.
 *
 * SYSREG_CP1 sits at +R100_PER_SYSREG_CP from SYSREG_CP0 on silicon
 * (see platform_def.h). Both blocks are 64 KB and share the
 * RVBARADDR0_{LOW,HIGH} register layout.
 */
static uint64_t r100_pmu_read_rvbar(R100PMUState *s, uint32_t cluster,
                                    uint32_t cpu)
{
    uint64_t base = (uint64_t)s->chiplet_id * R100_CHIPLET_OFFSET +
                    R100_SYSREG_CP0_PRIVATE_BASE +
                    (uint64_t)cluster * R100_PER_SYSREG_CP;
    uint32_t lo = 0, hi = 0;
    MemTxResult r;

    r = address_space_read(&address_space_memory,
                           base + R100_SYSREG_RVBARADDR0_LOW +
                               R100_SYSREG_PERCPU_RVBAR_OFF * cpu,
                           MEMTXATTRS_UNSPECIFIED, &lo, sizeof(lo));
    if (r != MEMTX_OK) {
        return 0;
    }
    r = address_space_read(&address_space_memory,
                           base + R100_SYSREG_RVBARADDR0_HIGH +
                               R100_SYSREG_PERCPU_RVBAR_OFF * cpu,
                           MEMTXATTRS_UNSPECIFIED, &hi, sizeof(hi));
    if (r != MEMTX_OK) {
        return 0;
    }
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Handle a write to a CPU_CONFIGURATION register. Decode the
 * cluster/core index from the offset, update the mirrored CPU_STATUS
 * so the FW's post-release poll completes, and kick the corresponding
 * vCPU on or off via the ARM power-control helpers.
 *
 * cfg_off is the register offset within the PMU block (0x2000 for
 * CP0.CPU0, +PERCPU_OFFSET per core, +PERCLUSTER_OFFSET per cluster).
 */
static void r100_pmu_handle_cpu_config(R100PMUState *s, uint32_t cfg_off,
                                       uint32_t val)
{
    uint32_t cluster_off = cfg_off - R100_PMU_CPU0_CONFIGURATION;
    uint32_t cluster = cluster_off / R100_PMU_PERCLUSTER_OFFSET;
    uint32_t cpu_in_cluster = (cluster_off % R100_PMU_PERCLUSTER_OFFSET)
                              / R100_PMU_PERCPU_OFFSET;
    uint32_t status_off;
    bool power_on;
    uint64_t mpidr, entry;
    int ret;

    if (cluster >= R100_NUM_CLUSTERS ||
        cpu_in_cluster >= R100_NUM_CORES_PER_CLUSTER) {
        return;
    }

    power_on = (val & R100_PMU_CPU_CFG_LOCAL_PWR_MASK) == R100_PMU_LOCAL_PWR_ON;

    status_off = R100_PMU_CPU0_STATUS +
                 cluster * R100_PMU_PERCLUSTER_OFFSET +
                 cpu_in_cluster * R100_PMU_PERCPU_OFFSET;
    s->regs[status_off >> 2] = power_on ? R100_PMU_CPU_STATUS_ON : 0;

    /*
     * Must match the mp-affinity encoding in r100_soc.c: chiplet in
     * bits 24-25, Aff1=cluster, Aff0=core. See the long comment there
     * for why the chiplet id is not placed in Aff2.
     */
    mpidr = ((uint64_t)s->chiplet_id << 24) | ((uint64_t)cluster << 8)
            | cpu_in_cluster;

    /*
     * Chiplet 0 / CP0 / cpu0 is the BSP — it was already started at
     * machine reset via the rvbar property, so CPU_CONFIGURATION writes
     * touching it are a no-op on the QEMU side.
     */
    if (mpidr == 0) {
        return;
    }

    if (!power_on) {
        /*
         * plat_pmu_cpu_off() is called as a precondition of cpu_on in
         * BL1's reset-release sequence; for already-off secondaries
         * arm_set_cpu_off returns IS_OFF which is harmless.
         */
        arm_set_cpu_off(mpidr);
        return;
    }

    entry = r100_pmu_read_rvbar(s, cluster, cpu_in_cluster);
    if (!entry) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pmu[cl%u]: cpu_on without RVBAR (cluster=%u "
                      "cpu=%u) — skipping release\n",
                      s->chiplet_id, cluster, cpu_in_cluster);
        return;
    }

    /*
     * BL1 writes RVBAR as a chiplet-local PRIVATE_BASE address
     * (`0x1E00028000` for BL2). Each chiplet's CPUs use a dedicated
     * memory view (built by r100_build_chiplet_view) where the 256 MB
     * window at PRIVATE_WIN_BASE is aliased to that chiplet's own slice
     * of sysmem. Starting the secondary at the unmodified `entry`
     * therefore lands on the correct chiplet's iRAM backing via the
     * private-window alias — matching silicon, where PC stays in the
     * PRIVATE_BASE range so PC-relative ADRP symbol resolution works.
     *
     * Do NOT add `chiplet_id * CHIPLET_OFFSET` here. That would start
     * the CPU at the absolute (cross-chiplet) address and shift every
     * linker symbol by CHIPLET_OFFSET via ADRP, corrupting expressions
     * like BL_CODE_END - BL2_BASE in bl2_el3_plat_arch_setup.
     */

    ret = arm_set_cpu_on(mpidr, entry, 0, 3, true);
    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS &&
        ret != QEMU_ARM_POWERCTL_ALREADY_ON) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pmu[cl%u]: arm_set_cpu_on(mpidr=0x%" PRIx64
                      ", entry=0x%" PRIx64 ") failed: %d\n",
                      s->chiplet_id, mpidr, entry, ret);
    }
}

static void r100_pmu_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100PMUState *s = R100_PMU(opaque);
    uint32_t reg_idx = addr >> 2;
    uint32_t off = (uint32_t)addr;

    if (reg_idx >= R100_PMU_REG_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pmu[cl%u]: write out of range 0x%" HWADDR_PRIx "\n",
                      s->chiplet_id, addr);
        return;
    }

    s->regs[reg_idx] = (uint32_t)val;

    /*
     * BL2's pmu_reset_dcluster() toggles each D-Cluster OFF then ON by
     * writing DCL0/1_CONFIGURATION and spinning on DCL0/1_STATUS &
     * DCL_STATUS_MASK until the low 4 bits reflect the new power state.
     * On real hardware the CPMU sequencer mirrors the LOCAL_PWR bits
     * into the status register; emulate that synchronously so the poll
     * completes. See rebel_h_pmu.c:pmu_reset_dcluster().
     */
    switch (off) {
    case R100_PMU_DCL0_CONFIG:
        s->regs[R100_PMU_DCL0_STATUS >> 2] =
            (uint32_t)val & R100_PMU_CPU_CFG_LOCAL_PWR_MASK;
        return;
    case R100_PMU_DCL1_CONFIG:
        s->regs[R100_PMU_DCL1_STATUS >> 2] =
            (uint32_t)val & R100_PMU_CPU_CFG_LOCAL_PWR_MASK;
        return;
    default:
        break;
    }

    /*
     * CPU_CONFIGURATION registers live at PMU offsets
     * 0x2000 + cluster*PERCLUSTER_OFFSET + cpu*PERCPU_OFFSET for
     * cluster in {CP0, CP1} and cpu in 0..3. Fan out to the release
     * handler for any matching offset.
     */
    if (off >= R100_PMU_CPU0_CONFIGURATION) {
        uint32_t cluster_off = off - R100_PMU_CPU0_CONFIGURATION;
        uint32_t cluster = cluster_off / R100_PMU_PERCLUSTER_OFFSET;
        uint32_t lane = cluster_off % R100_PMU_PERCLUSTER_OFFSET;

        if (cluster < R100_NUM_CLUSTERS &&
            lane < R100_NUM_CORES_PER_CLUSTER * R100_PMU_PERCPU_OFFSET &&
            (lane % R100_PMU_PERCPU_OFFSET) == 0) {
            r100_pmu_handle_cpu_config(s, off, (uint32_t)val);
        }
    }
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
