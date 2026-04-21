/*
 * REMU - R100 NPU System Emulator
 * D-Cluster (DNC/SHM/MGLUE) and RBDMA config-space stubs.
 *
 * q-cp's task init on CP1 touches three sets of registers that BL2/BL31 /
 * FreeRTOS on CP0 never poke. Without stubs they fall through to the
 * chiplet-wide cfg_mr unimpl catch-all and every read returns 0, so
 * `cp_create_tasks_impl` deadlocks on SHM TPG training ~35 s into boot:
 *
 *   1. `rdsn_init()` — external/.../q/cp/src/hal/dnc/rebel/rdsn_if.c —
 *      polls RDSN_HEAD_STATUS0 for `(bits & 0x000F000F) == 0x000F000F`
 *      at DCL{0,1}_MGLUE_CFG_BASE + 0x010 (i.e. DCL offset 0x20010).
 *      Then `rdsn_sanity_check()` fires dtest0 / dtest3 and polls
 *      TE0_RPT0.valid+pass (0x20080) and TE3_RPT0.valid+pass (0x20090).
 *
 *   2. `shm_init()` — external/.../q/cp/src/hal/shm/rebel/shm_if.c —
 *      for each of 16 SHM banks triggers TPG, then polls
 *      `SHM_REG_INTR_VEC.tpg_done` (bit 0 at SHM bank offset 0x030).
 *      Timeout is 1 s of virtual time per bank (SHM_TIMEOUT_US = 1e6),
 *      and failure calls `abort_event(ERR_SHM)` — fatal. First read
 *      of INTR_VEC must already have tpg_done=1.
 *
 *   3. `rbdma_get_ip_info()` — external/.../q/cp/src/hal/rbdma/... —
 *      reads RBDMA_IP_INFO0/1/2/3/4/5 at NBUS_L_RBDMA_CFG_BASE and
 *      logs them. Not a poll, but FW prints `rel_date: 0 rbdma ver: 0`
 *      when all zero. Seed plausible constants for cleaner logs.
 *
 * The two device types here are sparse register files (GHashTable
 * write-back, same pattern as r100_hbm.c) with a small set of
 * read-side overrides per the above. Each DCL instance covers the
 * full 1 MB DCL CFG window so DNC-slot / SHM-bank / MGLUE-register
 * traffic all lands on the same device without per-slot plumbing.
 *
 * Mapping in src/machine/r100_soc.c: two `r100-dnc-cluster` instances
 * per chiplet (DCL0 at 0x1FF2000000, DCL1 at 0x1FF2800000), each as a
 * priority-1 subregion of cfg_mr so it outranks the unimpl catch-all.
 * One `r100-rbdma` per chiplet at 0x1FF3700000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

/* ========================================================================
 * DCL block register layout (offsets from DCL{0,1}_CFG_BASE)
 * ======================================================================== */

/* DNC slots: 8 × 8 KB starting at 0x000000. */
#define DCL_DNC_BASE            R100_DNC_SLOT_BASE
#define DCL_DNC_STRIDE          R100_DNC_SLOT_STRIDE
#define DCL_DNC_COUNT           R100_DNC_SLOT_COUNT
#define DCL_DNC_REGION_END      (DCL_DNC_BASE + DCL_DNC_COUNT * DCL_DNC_STRIDE)

/*
 * Offsets within one DNC slot (g_dnc_memory_map.h):
 *   CONFIG page at slot_off 0x000, STATUS page at slot_off 0x400.
 * IP_INFO registers live in CONFIG; SP_STATUS* lives in STATUS.
 */
#define DNC_CFG_BASE_OFF        0x000
#define DNC_STATUS_BASE_OFF     0x400

#define DNC_IP_INFO0_OFF        (DNC_CFG_BASE_OFF + 0x000)
#define DNC_IP_INFO1_OFF        (DNC_CFG_BASE_OFF + 0x004)
#define DNC_IP_INFO3_OFF        (DNC_CFG_BASE_OFF + 0x00C)
#define DNC_SP_STATUS01_OFF     (DNC_STATUS_BASE_OFF + 0x204)

/*
 * dnc_config_ip_info1: {min_ver:8, maj_ver:8, ip_ver:8, ip_id:8}.
 * dnc_register_ops() accepts ip_ver == DNC_V1_0 (1) or DNC_V1_1 (2) and
 * aborts with ERR_DNC for anything else. The silicon target is EVT1 /
 * REBEL_H, so advertise V1_1. ip_id/maj_ver/min_ver are just logged.
 */
#define DNC_IP_INFO1_SEED       ((0U << 24) | (2U << 16) | (0U << 8) | 0U)
/* dnc_config_ip_info3: {regmap_min_ver:8, regmap_maj_ver:8, rsvd:16}. */
#define DNC_IP_INFO3_SEED       ((1U << 8) | 0U)
/*
 * dnc_status_sp_status01: {test_done:1, rsvd:15, test_cnt_mismatch:16}.
 * dnc_run_sp_test(dnc_id, pattern=0x{0,2}) polls this after writing
 * CONFIG.SP_TEST.trig=1 and aborts after 1 ms without test_done. With
 * no real DNC behind the stub we permanently advertise test_done=1 so
 * both pattern passes in dnc_init_workload_path() exit immediately.
 */
#define DNC_SP_STATUS01_TEST_DONE   0x1U

/* SHM banks: 16 × 1 KB starting at 0x10000. */
#define DCL_SHM_BASE            R100_SHM_BANK_BASE
#define DCL_SHM_STRIDE          R100_SHM_BANK_STRIDE
#define DCL_SHM_COUNT           R100_SHM_BANK_COUNT
#define DCL_SHM_REGION_END      (DCL_SHM_BASE + DCL_SHM_COUNT * DCL_SHM_STRIDE)

/* Offsets within one SHM bank (g_shm_reg.h). */
#define SHM_IP_INFO0_OFF        0x000
#define SHM_IP_INFO1_OFF        0x004
#define SHM_IP_INFO2_OFF        0x008
#define SHM_IP_INFO3_OFF        0x00C
#define SHM_INTR_VEC_OFF        0x030
#define SHM_INTR_VEC_TPG_DONE   (1U << 0)

/*
 * MGLUE / RDSN_HEAD at DCL offset 0x20000. g_rdsn_head.h shows a 0x400
 * spread of registers; we model a single-instance region.
 */
#define DCL_MGLUE_BASE          R100_MGLUE_BASE
#define DCL_MGLUE_SIZE          0x10000
#define DCL_MGLUE_END           (DCL_MGLUE_BASE + DCL_MGLUE_SIZE)

/* Offsets within MGLUE (g_rdsn_head.h). */
#define RDSN_STATUS0_OFF        0x010
#define RDSN_TE0_RPT0_OFF       0x080
#define RDSN_TE3_RPT0_OFF       0x090

/*
 * rdsn_head_status0 reports per-row init/config readiness. The FW mask
 * is RDSN_STATUS0_ALL_PREPARED = 0x000F000F — init{0..3} (bits 0..3) +
 * config{0..3} (bits 16..19). With all bits set the status read passes
 * rdsn_is_prepared() and rdsn_set_ids() on the first iteration.
 */
#define RDSN_STATUS0_ALL_PREPARED   0x000F000F

/* rdsn_head_te{0,3}_rpt0: {timeout:1, valid:1, pass:1, …}. valid+pass =
 * bits 1 and 2 set. rdsn_sanity_check() waits for both. */
#define RDSN_RPT0_VALID_PASS        0x00000006

/* ========================================================================
 * RBDMA register layout (offsets from NBUS_L_RBDMA_CFG_BASE)
 * ======================================================================== */

/* g_cdma_global_registers.h */
#define RBDMA_IP_INFO0_OFF      0x000
#define RBDMA_IP_INFO1_OFF      0x004
#define RBDMA_IP_INFO2_OFF      0x008
#define RBDMA_IP_INFO3_OFF      0x00C
#define RBDMA_IP_INFO4_OFF      0x010
#define RBDMA_IP_INFO5_OFF      0x014

/*
 * Seed values. rel_date is a plausible hex-encoded date (0x20260101).
 * ip_info1: {min_ver=0, maj_ver=1, rbdma_ver=1, ip_id=1}.
 * ip_info2: FW writes chiplet_id here — leave accept-and-remember.
 */
#define RBDMA_INFO0_REL_DATE    0x20260101U
#define RBDMA_INFO1_SEED        ((1U << 24) | (1U << 16) | (1U << 8) | 0U)

/* ========================================================================
 * Shared sparse register file (like r100_hbm.c)
 * ======================================================================== */

typedef struct R100RegStore {
    GHashTable *regs;   /* hwaddr -> uint32_t */
} R100RegStore;

static void regstore_init(R100RegStore *rs)
{
    rs->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static void regstore_reset(R100RegStore *rs)
{
    if (rs->regs) {
        g_hash_table_remove_all(rs->regs);
    }
}

static bool regstore_lookup(R100RegStore *rs, hwaddr addr, uint32_t *out)
{
    gpointer val;

    if (!rs->regs) {
        return false;
    }
    if (!g_hash_table_lookup_extended(rs->regs,
                                      GUINT_TO_POINTER((guint)addr),
                                      NULL, &val)) {
        return false;
    }
    *out = (uint32_t)GPOINTER_TO_UINT(val);
    return true;
}

static void regstore_store(R100RegStore *rs, hwaddr addr, uint32_t val)
{
    g_hash_table_insert(rs->regs, GUINT_TO_POINTER((guint)addr),
                        GUINT_TO_POINTER((guint)val));
}

/* ========================================================================
 * r100-dnc-cluster device (DCL0 / DCL1 CFG window, 1 MB)
 * ======================================================================== */

struct R100DNCClusterState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    R100RegStore store;
    uint32_t chiplet_id;
    uint32_t dcl_id;        /* 0 = DCL0, 1 = DCL1 — used for log prefix */
};
typedef struct R100DNCClusterState R100DNCClusterState;

DECLARE_INSTANCE_CHECKER(R100DNCClusterState, R100_DNC_CLUSTER,
                         TYPE_R100_DNC_CLUSTER)

/*
 * Seed IP_INFO{0..3} so that shm_get_ip_info() prints non-zero values.
 * SHM bank id encoded in ip_id / shm_unit_id / rdsn_rtid for uniqueness.
 */
static uint32_t dnc_shm_default(hwaddr bank_off, uint32_t bank_id)
{
    switch (bank_off) {
    case SHM_IP_INFO0_OFF:
        return 0x20260101U;             /* rel_date */
    case SHM_IP_INFO1_OFF:
        /* ip_id | ip_ver | max_ver | min_ver */
        return ((bank_id & 0xFFU) << 24) | (1U << 16) | (0U << 8) | 0U;
    case SHM_IP_INFO2_OFF:
        /* wr_lat | rd_lat | rdsn_rtid | shm_unit_id */
        return (10U << 24) | (10U << 16) |
               ((bank_id & 0xFFU) << 8) | (bank_id & 0xFFU);
    case SHM_IP_INFO3_OFF:
        /* capa_exp | capa_man | etc | n_bank
         * n_bank=16 (plausible), capa_man=1, capa_exp=28 (≈256 MB). */
        return (28U << 24) | (1U << 16) | (0U << 8) | 16U;
    case SHM_INTR_VEC_OFF:
        /*
         * Bit 0 (tpg_done) is set so shm_init's post-TPG poll exits on
         * iteration 0. intr_malf/derr (bits 16/17) stay clear so the
         * FW's error branches don't fire. */
        return SHM_INTR_VEC_TPG_DONE;
    default:
        return 0;
    }
}

static uint32_t dnc_mglue_default(hwaddr mglue_off)
{
    switch (mglue_off) {
    case RDSN_STATUS0_OFF:
        return RDSN_STATUS0_ALL_PREPARED;
    case RDSN_TE0_RPT0_OFF:
    case RDSN_TE3_RPT0_OFF:
        return RDSN_RPT0_VALID_PASS;
    default:
        return 0;
    }
}

static uint32_t dnc_slot_default(hwaddr slot_off, uint32_t slot_id)
{
    (void)slot_id;
    switch (slot_off) {
    case DNC_IP_INFO0_OFF:
        return 0x20260101U;             /* rel_date */
    case DNC_IP_INFO1_OFF:
        return DNC_IP_INFO1_SEED;
    case DNC_IP_INFO3_OFF:
        return DNC_IP_INFO3_SEED;
    case DNC_SP_STATUS01_OFF:
        return DNC_SP_STATUS01_TEST_DONE;
    default:
        return 0;
    }
}

static uint64_t r100_dnc_read(void *opaque, hwaddr addr, unsigned size)
{
    R100DNCClusterState *s = R100_DNC_CLUSTER(opaque);
    uint32_t stored;

    /*
     * Honour any value the FW has written: RMW sequences against
     * ctrl0/ctrl1 and TRIG registers must round-trip normally. Only
     * fall back to the synthesised default if the register has never
     * been touched.
     */
    if (regstore_lookup(&s->store, addr, &stored)) {
        return stored;
    }

    /* SHM bank region: decode (bank_id, bank_offset). */
    if (addr >= DCL_SHM_BASE && addr < DCL_SHM_REGION_END) {
        hwaddr rel = addr - DCL_SHM_BASE;
        uint32_t bank_id = (uint32_t)(rel / DCL_SHM_STRIDE);
        hwaddr bank_off = rel % DCL_SHM_STRIDE;
        return dnc_shm_default(bank_off, bank_id);
    }

    /* MGLUE / RDSN head region. */
    if (addr >= DCL_MGLUE_BASE && addr < DCL_MGLUE_END) {
        return dnc_mglue_default(addr - DCL_MGLUE_BASE);
    }

    /* DNC slots (8 × 8 KB, stride 0x2000). */
    if (addr < DCL_DNC_REGION_END) {
        uint32_t slot_id = (uint32_t)(addr / DCL_DNC_STRIDE);
        hwaddr slot_off = addr % DCL_DNC_STRIDE;
        return dnc_slot_default(slot_off, slot_id);
    }

    return 0;
}

static void r100_dnc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100DNCClusterState *s = R100_DNC_CLUSTER(opaque);

    regstore_store(&s->store, addr, (uint32_t)val);
}

static const MemoryRegionOps r100_dnc_ops = {
    .read = r100_dnc_read,
    .write = r100_dnc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_dnc_realize(DeviceState *dev, Error **errp)
{
    R100DNCClusterState *s = R100_DNC_CLUSTER(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-dnc-cluster.cl%u.dcl%u",
             s->chiplet_id, s->dcl_id);
    regstore_init(&s->store);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_dnc_ops, s,
                          name, R100_DCL_CFG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_dnc_reset(DeviceState *dev)
{
    R100DNCClusterState *s = R100_DNC_CLUSTER(dev);
    regstore_reset(&s->store);
}

static Property r100_dnc_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100DNCClusterState, chiplet_id, 0),
    DEFINE_PROP_UINT32("dcl-id", R100DNCClusterState, dcl_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_dnc_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_dnc_realize;
    device_class_set_legacy_reset(dc, r100_dnc_reset);
    device_class_set_props(dc, r100_dnc_properties);
}

static const TypeInfo r100_dnc_info = {
    .name = TYPE_R100_DNC_CLUSTER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100DNCClusterState),
    .class_init = r100_dnc_class_init,
};

/* ========================================================================
 * r100-rbdma device (NBUS_L_RBDMA CFG window, 1 MB)
 * ======================================================================== */

struct R100RBDMAState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    R100RegStore store;
    uint32_t chiplet_id;
};
typedef struct R100RBDMAState R100RBDMAState;

DECLARE_INSTANCE_CHECKER(R100RBDMAState, R100_RBDMA, TYPE_R100_RBDMA)

static uint32_t rbdma_default(hwaddr addr, uint32_t chiplet_id)
{
    switch (addr) {
    case RBDMA_IP_INFO0_OFF:
        return RBDMA_INFO0_REL_DATE;
    case RBDMA_IP_INFO1_OFF:
        return RBDMA_INFO1_SEED;
    case RBDMA_IP_INFO2_OFF:
        /*
         * {chiplet_id:8, reserved:24}. FW writes this during bring-up
         * so this default only covers the first read; it's overwritten
         * by rbdma_get_ip_info()'s `info2.chiplet_id = cl_id` store.
         */
        return chiplet_id & 0xFFU;
    default:
        return 0;
    }
}

static uint64_t r100_rbdma_read(void *opaque, hwaddr addr, unsigned size)
{
    R100RBDMAState *s = R100_RBDMA(opaque);
    uint32_t stored;

    if (regstore_lookup(&s->store, addr, &stored)) {
        return stored;
    }
    return rbdma_default(addr, s->chiplet_id);
}

static void r100_rbdma_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    R100RBDMAState *s = R100_RBDMA(opaque);

    regstore_store(&s->store, addr, (uint32_t)val);
}

static const MemoryRegionOps r100_rbdma_ops = {
    .read = r100_rbdma_read,
    .write = r100_rbdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_rbdma_realize(DeviceState *dev, Error **errp)
{
    R100RBDMAState *s = R100_RBDMA(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-rbdma.cl%u", s->chiplet_id);
    regstore_init(&s->store);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_rbdma_ops, s,
                          name, R100_NBUS_L_RBDMA_CFG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_rbdma_reset(DeviceState *dev)
{
    R100RBDMAState *s = R100_RBDMA(dev);
    regstore_reset(&s->store);
}

static Property r100_rbdma_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100RBDMAState, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_rbdma_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_rbdma_realize;
    device_class_set_legacy_reset(dc, r100_rbdma_reset);
    device_class_set_props(dc, r100_rbdma_properties);
}

static const TypeInfo r100_rbdma_info = {
    .name = TYPE_R100_RBDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100RBDMAState),
    .class_init = r100_rbdma_class_init,
};

/* ======================================================================== */

static void r100_dnc_register_types(void)
{
    type_register_static(&r100_dnc_info);
    type_register_static(&r100_rbdma_info);
}

type_init(r100_dnc_register_types)
