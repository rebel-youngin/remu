/*
 * REMU - R100 NPU System Emulator
 * D-Cluster (DNC/SHM/MGLUE) config-space stub.
 *
 * q-cp's task init on CP1 touches two sets of registers that BL2/BL31 /
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
 * The device type here is a sparse register file (GHashTable
 * write-back, same pattern as r100_hbm.c / r100_rbdma.c) with a
 * small set of read-side overrides per the above. Each DCL instance
 * covers the full 1 MB DCL CFG window so DNC-slot / SHM-bank /
 * MGLUE-register traffic all lands on the same device without per-
 * slot plumbing.
 *
 * Mapping in src/machine/r100_soc.c: two `r100-dnc-cluster` instances
 * per chiplet (DCL0 at 0x1FF2000000, DCL1 at 0x1FF2800000), each as a
 * priority-1 subregion of cfg_mr so it outranks the unimpl catch-all.
 *
 * RBDMA used to share this file as a small passive sibling section
 * (see commit 4a76ce6); P4A carved it out into r100_rbdma.{c,h} when
 * it grew an active kick → done IRQ path.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
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

/* M9-1c — TASK_32B passage page (slot+0x800..+0x81F) holds the 32 B
 * dnc_reg_task_passage q-cp populates per dnc_send_task. The struct
 * is `__attribute__((packed, aligned(4)))` so AArch64 emits ascending
 * 4 B stores — the final word lands at TASK_DESC_CFG1 (+0x01C inside
 * TASK_32B = slot+0x81C) with desc_cfg1.itdone=1. We use that store
 * as the kickoff trigger. The earlier `DNC_TASK_DESC_CONFIG_WRITE_NORECORD`
 * turq-prologue calls also touch the same address as a 64-bit writeq
 * (CFG0+CFG1 pair) carrying itdone=1 — they're distinguished by
 * access_size 8 vs 4. */
#define DNC_TASK_32B_BASE_OFF       0x800
#define DNC_TASK_32B_END_OFF        0x820     /* exclusive */
#define DNC_TASK_DESC_ID_OFF        (DNC_TASK_32B_BASE_OFF + 0x000)
#define DNC_TASK_DESC_CFG0_OFF      (DNC_TASK_32B_BASE_OFF + 0x018)
#define DNC_TASK_DESC_CFG1_OFF      (DNC_TASK_32B_BASE_OFF + 0x01C)

/* desc_cfg1 layout (g_dnc_task_32b.h union dnc_task_desc_cfg1):
 *     data:12 addr:13 auto_fetch_pfix:1 mode:2 queue:2 sole:1 itdone:1
 * itdone is bit 31. queue is bits 28..29 (cmd_type from
 * cp1/dnc_if.c:dnc_send_task copies COMMON_CMD_TYPE_* there). */
#define DNC_DESC_CFG1_ITDONE_BIT    (1U << 31)
#define DNC_DESC_CFG1_QUEUE_SHIFT   28
#define DNC_DESC_CFG1_QUEUE_MASK    0x3U
#define DNC_DESC_CFG1_MODE_SHIFT    26
#define DNC_DESC_CFG1_MODE_MASK     0x3U
#define DNC_DESC_CFG1_SOLE_BIT      (1U << 30)

/* TASK_DONE page (slot+0xA00..+0xBFF). q-cp's dnc_X_done_handler reads
 * a 64-bit readq at slot+0xA00 = (done_rpt0 [bits 0..31] | done_rpt1
 * [bits 32..63]). r100-dnc latches the synthesised 8 bytes here so
 * the read returns a valid completion record on the IRQ-handler's
 * first poll — see r100_dnc_complete_done_passage(). */
#define DNC_TASK_DONE_BASE_OFF      0xA00
#define DNC_TASK_DONE_RPT0_OFF      (DNC_TASK_DONE_BASE_OFF + 0x000)
#define DNC_TASK_DONE_RPT1_OFF      (DNC_TASK_DONE_BASE_OFF + 0x004)

/* dnc_task_done_done_rpt1 (evt1/g_dnc_task_done.h) bitfield positions:
 *     dnc_id:4 chiplet_id:2 rsvd6:2 sole_cnt:8 local_tstamp:8
 *     discard_rpt:1 event_type:3 cmd_type:3 sender:1
 * event_type=2 is DONE; cmd_type 0..3 mirrors COMMON_CMD_TYPE_*. */
#define DNC_RPT1_DNC_ID_SHIFT           0
#define DNC_RPT1_CHIPLET_ID_SHIFT       4
#define DNC_RPT1_DISCARD_RPT_SHIFT      24
#define DNC_RPT1_EVENT_TYPE_SHIFT       25
#define DNC_RPT1_EVENT_TYPE_DONE        2u
#define DNC_RPT1_CMD_TYPE_SHIFT         28
#define DNC_RPT1_CMD_TYPE_MASK          0x7u

/* Cap on per-cluster outstanding completions awaiting BH delivery. q-cp
 * dispatches one task per DNC at a time, so 32 (= 8 slots × 4 cmd_types
 * worst-case) is generous head-room — we drop with LOG_GUEST_ERROR
 * before silently coalescing. */
#define DNC_DONE_FIFO_DEPTH         32u

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
 * Sparse register file (like r100_hbm.c)
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

/* M9-1c FIFO entry: one pending DNC kickoff awaiting BH-delivered
 * completion IRQ. */
typedef struct R100DNCDoneEntry {
    uint32_t slot_id;       /* 0..7 — DNC index within this DCL */
    uint32_t cmd_type;      /* COMMON_CMD_TYPE_* (0..3) */
    uint32_t desc_id_bits;  /* desc_cfg1 / desc_id captured for done_rpt0 */
} R100DNCDoneEntry;

struct R100DNCClusterState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    R100RegStore store;
    uint32_t chiplet_id;
    uint32_t dcl_id;        /* 0 = DCL0, 1 = DCL1 — used for log prefix */

    /* M9-1c active path. One GPIO out per (slot, cmd_type) — q-cp
     * binds dnc_X_done_handler to all six DNC SPIs but the hardware
     * pulses the one matching cfg1.queue, so we mirror that. The
     * EXCEPTION line (cmd_type slot 0 in interrupt.h) stays unwired:
     * REMU never synthesises faults, only completions. */
    qemu_irq slot_done_irq[DCL_DNC_COUNT][R100_HW_SPEC_DNC_CMD_TYPE_NUM];

    QEMUBH *done_bh;
    R100DNCDoneEntry done_fifo[DNC_DONE_FIFO_DEPTH];
    uint32_t done_fifo_head;       /* next slot to read */
    uint32_t done_fifo_tail;       /* next slot to write */
    uint64_t done_pulses_fired;    /* observability */
    uint64_t done_drops_fifo;      /* FIFO full */
    uint64_t done_drops_intid;     /* r100_dnc_intid returned 0 */
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

/* ========================================================================
 * M9-1c — active task-completion path
 *
 * Detect q-cp's `dnc_send_task -> put_urq_task` final passage write at
 * slot+0x81C (DESC_CFG1) with itdone=1 and access_size 4, latch a
 * synthetic dnc_reg_done_passage at slot+0xA00..+0xA04 so a 64-bit
 * readq(TASK_DONE) returns a coherent completion record, and pulse the
 * matching DNC GIC SPI from a bottom-half so q-cp's
 * dnc_X_done_handler runs on the next CPU schedule.
 *
 * The earlier turq prologue calls (DNC_TASK_DESC_CONFIG_WRITE_NORECORD,
 * 64-bit writeq landing at 0x818) also carry itdone=1, so we strictly
 * gate on access_size 4 to avoid spurious IRQs per turq iteration. The
 * tlb-invalidate path (sole=1, mode=2) writes CFG1 alone but with
 * itdone=1 too — q-cp serialises it before dnc_send_task's actual
 * passage, so a stray pulse is harmless (the worker already runs the
 * handler). Keep the gate permissive for now; revisit if we observe
 * IRQ-storm symptoms during bring-up.
 * ======================================================================== */

static uint32_t r100_dnc_synth_done_rpt1(uint32_t global_dnc_id,
                                         uint32_t chiplet_id,
                                         uint32_t cmd_type)
{
    uint32_t rpt1 = 0;

    rpt1 |= (global_dnc_id & 0xFu) << DNC_RPT1_DNC_ID_SHIFT;
    rpt1 |= (chiplet_id & 0x3u)    << DNC_RPT1_CHIPLET_ID_SHIFT;
    /* discard_rpt = 0 so DNC_IS_AUTO_FETCH_DONE_RPT(...) returns false
     * and the handler proceeds into cm_handle_irq instead of early-
     * return. event_type = DONE so q-cp's get_irq_status decodes the
     * completion as a real task-done (not enqueue/checkin). */
    rpt1 |= (DNC_RPT1_EVENT_TYPE_DONE & 0x7u) << DNC_RPT1_EVENT_TYPE_SHIFT;
    rpt1 |= (cmd_type & DNC_RPT1_CMD_TYPE_MASK) << DNC_RPT1_CMD_TYPE_SHIFT;
    return rpt1;
}

static void r100_dnc_complete_done_passage(R100DNCClusterState *s,
                                           uint32_t slot_id,
                                           uint32_t cmd_type,
                                           uint32_t desc_id_bits)
{
    hwaddr slot_base = (hwaddr)slot_id * DCL_DNC_STRIDE;
    uint32_t global_dnc_id = s->dcl_id * DCL_DNC_COUNT + slot_id;
    uint32_t rpt1 = r100_dnc_synth_done_rpt1(global_dnc_id, s->chiplet_id,
                                             cmd_type);

    /* Latch done_rpt0 + done_rpt1 in regstore so the handler's readq
     * sees a coherent payload regardless of whether QEMU splits the
     * 8-byte access into two 4-byte loads. */
    regstore_store(&s->store, slot_base + DNC_TASK_DONE_RPT0_OFF,
                   desc_id_bits);
    regstore_store(&s->store, slot_base + DNC_TASK_DONE_RPT1_OFF, rpt1);
}

static void r100_dnc_done_bh(void *opaque)
{
    R100DNCClusterState *s = opaque;

    while (s->done_fifo_head != s->done_fifo_tail) {
        R100DNCDoneEntry *e = &s->done_fifo[s->done_fifo_head];
        uint32_t global_dnc_id;
        uint32_t intid;
        qemu_irq irq;

        s->done_fifo_head =
            (s->done_fifo_head + 1) % DNC_DONE_FIFO_DEPTH;

        if (e->cmd_type >= R100_HW_SPEC_DNC_CMD_TYPE_NUM) {
            s->done_drops_intid++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-dnc cl=%u dcl=%u slot=%u: cmd_type=%u "
                          "out of range — drop\n",
                          s->chiplet_id, s->dcl_id, e->slot_id,
                          e->cmd_type);
            continue;
        }
        global_dnc_id = s->dcl_id * DCL_DNC_COUNT + e->slot_id;
        intid = r100_dnc_intid(global_dnc_id, e->cmd_type);
        if (intid == 0) {
            s->done_drops_intid++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-dnc cl=%u dcl=%u slot=%u cmd_type=%u: "
                          "no INTID — drop\n",
                          s->chiplet_id, s->dcl_id, e->slot_id,
                          e->cmd_type);
            continue;
        }

        r100_dnc_complete_done_passage(s, e->slot_id, e->cmd_type,
                                       e->desc_id_bits);

        irq = s->slot_done_irq[e->slot_id][e->cmd_type];
        if (irq) {
            qemu_irq_pulse(irq);
            s->done_pulses_fired++;
            qemu_log_mask(LOG_TRACE,
                          "r100-dnc cl=%u dcl=%u slot=%u kickoff "
                          "dnc_id=%u cmd_type=%u desc_id=0x%x → "
                          "intid=%u spi=%u fired=%" PRIu64 "\n",
                          s->chiplet_id, s->dcl_id, e->slot_id,
                          global_dnc_id, e->cmd_type, e->desc_id_bits,
                          intid, intid - R100_GIC_INTERNAL,
                          s->done_pulses_fired);
        } else {
            s->done_drops_intid++;
            qemu_log_mask(LOG_UNIMP,
                          "r100-dnc cl=%u dcl=%u slot=%u cmd_type=%u: "
                          "GPIO out unwired — drop (machine wiring "
                          "missing?)\n",
                          s->chiplet_id, s->dcl_id, e->slot_id,
                          e->cmd_type);
        }
    }
}

static void r100_dnc_kickoff(R100DNCClusterState *s, hwaddr slot_off,
                             uint32_t cfg1_val)
{
    uint32_t slot_id = (uint32_t)(slot_off / DCL_DNC_STRIDE);
    uint32_t cmd_type = (cfg1_val >> DNC_DESC_CFG1_QUEUE_SHIFT) &
                        DNC_DESC_CFG1_QUEUE_MASK;
    uint32_t desc_id_bits;
    uint32_t next_tail;

    if (slot_id >= DCL_DNC_COUNT) {
        return;
    }
    /* desc_id was the first word of the passage — q-cp wrote it before
     * desc_mode/prog0/.../cfg1, so regstore has the latest value. */
    {
        hwaddr slot_base = (hwaddr)slot_id * DCL_DNC_STRIDE;
        if (!regstore_lookup(&s->store, slot_base + DNC_TASK_DESC_ID_OFF,
                             &desc_id_bits)) {
            desc_id_bits = 0;
        }
    }

    next_tail = (s->done_fifo_tail + 1) % DNC_DONE_FIFO_DEPTH;
    if (next_tail == s->done_fifo_head) {
        s->done_drops_fifo++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-dnc cl=%u dcl=%u slot=%u: completion FIFO "
                      "full (%u entries) — drop\n",
                      s->chiplet_id, s->dcl_id, slot_id,
                      DNC_DONE_FIFO_DEPTH);
        return;
    }
    s->done_fifo[s->done_fifo_tail] = (R100DNCDoneEntry){
        .slot_id = slot_id,
        .cmd_type = cmd_type,
        .desc_id_bits = desc_id_bits,
    };
    s->done_fifo_tail = next_tail;

    if (s->done_bh) {
        qemu_bh_schedule(s->done_bh);
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

    /* M9-1c kickoff gate. Inside a DNC slot, a 4-byte write to
     * TASK_DESC_CFG1 with itdone=1 marks the end of the
     * dnc_reg_task_passage struct copy — that's the trigger. The
     * 8-byte writeq variant from DNC_TASK_DESC_CONFIG_WRITE_NORECORD
     * (turq prologue) lands at +0x818 = CFG0+CFG1 pair; gating on
     * size==4 strictly disambiguates them. */
    if (addr < DCL_DNC_REGION_END) {
        hwaddr slot_off = addr % DCL_DNC_STRIDE;

        if (slot_off == DNC_TASK_DESC_CFG1_OFF && size == 4 &&
            (val & DNC_DESC_CFG1_ITDONE_BIT)) {
            r100_dnc_kickoff(s, addr, (uint32_t)val);
        }
    }
}

static const MemoryRegionOps r100_dnc_ops = {
    .read = r100_dnc_read,
    .write = r100_dnc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    /* Allow 8-byte readq for q-cp's DNC_TASK_DONE_READQ. QEMU splits
     * the access into 32-bit halves through r100_dnc_read; the
     * regstore was just written by the BH so both halves see the
     * latched done_rpt0/1 atomically (BHs run between MMIO ops on the
     * same CPU). 8-byte writeq is also seen via
     * DNC_TASK_DESC_CONFIG_WRITE_NORECORD; the kickoff gate filters
     * it out by size == 4. */
    .impl.max_access_size = 8,
};

static void r100_dnc_realize(DeviceState *dev, Error **errp)
{
    R100DNCClusterState *s = R100_DNC_CLUSTER(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-dnc-cluster.cl%u.dcl%u",
             s->chiplet_id, s->dcl_id);
    regstore_init(&s->store);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_dnc_ops, s,
                          name, R100_DCL_CFG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    /* M9-1c — one GPIO out per (slot, cmd_type). Total =
     * DCL_DNC_COUNT * R100_HW_SPEC_DNC_CMD_TYPE_NUM = 8 × 4 = 32 lines
     * per DCL. r100_soc.c wires these to the matching GIC SPIs from
     * r100_dnc_intid(). */
    for (uint32_t slot = 0; slot < DCL_DNC_COUNT; slot++) {
        for (uint32_t ct = 0; ct < R100_HW_SPEC_DNC_CMD_TYPE_NUM; ct++) {
            sysbus_init_irq(sbd, &s->slot_done_irq[slot][ct]);
        }
    }

    s->done_bh = qemu_bh_new(r100_dnc_done_bh, s);
    s->done_fifo_head = 0;
    s->done_fifo_tail = 0;
    s->done_pulses_fired = 0;
    s->done_drops_fifo = 0;
    s->done_drops_intid = 0;
}

static void r100_dnc_reset(DeviceState *dev)
{
    R100DNCClusterState *s = R100_DNC_CLUSTER(dev);
    regstore_reset(&s->store);
    s->done_fifo_head = 0;
    s->done_fifo_tail = 0;
    if (s->done_bh) {
        qemu_bh_cancel(s->done_bh);
    }
    /* Counters survive reset so tests can assert cumulative behaviour
     * (matches r100-cm7's mbtq counter convention). */
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

/* ======================================================================== */

static void r100_dnc_register_types(void)
{
    type_register_static(&r100_dnc_info);
}

type_init(r100_dnc_register_types)
