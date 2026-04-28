/*
 * REMU - R100 NPU System Emulator
 * r100-rbdma — RBDMA (Rebel Block DMA) reg block + OTO byte-mover.
 *
 * Two-layer device:
 *   - P4A: register block + kick → FNSH FIFO → done-IRQ ack path.
 *     q-cp's RMW init sequences round-trip; the kickoff trigger
 *     completes synchronously with no real data movement.
 *   - P4B: layered on top of the same RUN_CONF1 trigger,
 *     `r100_rbdma_do_oto` performs an actual byte-for-byte copy from
 *     SAR → DAR via address_space_{read,write} for `task_type=OTO`
 *     (the only flavour umd's `simple_copy` emits today). Other
 *     task_types still ack the kick via LOG_UNIMP so q-cp's done
 *     loop unwinds.
 *
 * Maps the 1 MB RBDMA configuration window (NBUS_L_RBDMA_CFG @
 * 0x1FF3700000) per chiplet. Implements:
 *
 *   * Sparse register file (hashtable write-back) so q-cp's RMW init
 *     sequences (rbdma_clear_cdma, autofetch enable, log-mgr, run-conf
 *     defaults, etc.) round-trip normally.
 *
 *   * Synthetic IP_INFO seeds. q-cp's `rbdma_get_ip_info` reads info0..5
 *     to learn version + queue depth + executor count; with the original
 *     passive stub all six were zero, so `num_of_executer = 0` and
 *     `num_of_fnsh_fifo = 0`, which made `rbdma_update_credit` always
 *     return zero credit (see HAL `rbdma_if.c`). q-cp would never push a
 *     task. We seed plausible silicon-shaped values: 8 TEs, 32-deep TQ /
 *     UTQ / PTQ / TEQ / FNSH FIFO, all under R100_RBDMA_FIFO_DEPTH.
 *
 *   * "All free" credit reporting on TQ / UTQ / PTQ / TEQ status reads:
 *     `rbdma_update_credit` reads NORMALTQUEUE_STATUS,
 *     NORMALTQUEUE_EXT_STATUS, PTQUEUE_STATUS as the *free* count.
 *     We respond with the configured queue depth so q-cp always thinks
 *     there's room. Because we drain every task to the FNSH FIFO from
 *     the bottom-half scheduled below, the live state is in fact
 *     "all free between kicks", so this matches the model.
 *
 *   * Kick → done IRQ. q-cp's `rbdma_send_task` does eight 32-bit
 *     writes into the CDMA_TASK page (offset RBDMA_CDMA_TASK_BASE_OFF =
 *     0x200 within the RBDMA window). Final write is RUN_CONF1 (offset
 *     +0x1C = 0x21C); that's the silicon kickoff register and we use it
 *     as the trigger. On RUN_CONF1 store, we capture PTID_INIT (offset
 *     0x200, written first in the descriptor) which carries the
 *     `rbdma_global_fnsh_intr_fifo` payload q-cp needs back, push it
 *     onto a small FIFO, and schedule a BH that pulses INT_ID_RBDMA1
 *     (978). q-cp's `rbdma_done_handler` then drains FNSH_INTR_FIFO via
 *     `rbdma_polling`, which we serve from the same FIFO.
 *
 *     Honour the silicon `intr_disable` semantics: when RUN_CONF1
 *     bit 0 is set (used by q-cp's dump_shm / shm_clear paths) the FIFO
 *     entry is still pushed (so a polling consumer can observe it) but
 *     no GIC SPI is pulsed.
 *
 *   * GIC SPI line for INT_ID_RBDMA1 wired in src/machine/r100_soc.c.
 *     INT_ID_RBDMA0_ERR (977) is reserved with a sysbus_init_irq slot
 *     for symmetry — REMU never synthesises errors today.
 *
 * Per-chiplet instantiation: q-cp's `rbdma_init(cl_id)` runs on every
 * CA73 CP0; each chiplet's worker writes its own RBDMA at chiplet_base
 * + NBUS_L_RBDMA_CFG_BASE. Mirroring r100-dnc-cluster, we instantiate
 * one r100-rbdma per chiplet inside r100_chiplet_init.
 *
 * Historical note: the passive variant of this device used to live as
 * a small section in r100_dnc.c (alongside the DCL CFG stub it shares
 * the regstore pattern with). P4A carved it out into its own pair of
 * files so the active task-completion path doesn't bloat r100_dnc.c
 * and so future RBDMA-only features (TE register banks, autofetch
 * mirroring, etc.) have a clear home.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "r100_soc.h"

/* ========================================================================
 * Register layout (offsets from NBUS_L_RBDMA_CFG_BASE)
 *
 * Sourced from the q-cp HAL autogen headers:
 *   external/.../q/cp/src/hal/autogen/rebel/g_rbdma_memory_map.h
 *   external/.../q/cp/src/hal/autogen/rebel/g_cdma_global_registers.h
 *   external/.../q/cp/src/hal/autogen/rebel/g_cdma_task_registers.h
 * ======================================================================== */

/* CDMA sub-block bases (g_rbdma_memory_map.h). */
#define RBDMA_CDMA_GLOBAL_BASE_OFF      0x000
#define RBDMA_CDMA_TASK_BASE_OFF        0x200

/* CDMA_GLOBAL — only the offsets we actively interpret. Everything
 * else hits the regstore directly. */
#define RBDMA_GLOBAL_IP_INFO0_OFF       (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x000)
#define RBDMA_GLOBAL_IP_INFO1_OFF       (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x004)
#define RBDMA_GLOBAL_IP_INFO2_OFF       (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x008)
#define RBDMA_GLOBAL_IP_INFO3_OFF       (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x00C)
#define RBDMA_GLOBAL_IP_INFO4_OFF       (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x010)
#define RBDMA_GLOBAL_IP_INFO5_OFF       (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x014)
#define RBDMA_GLOBAL_INTR_FIFO_NUM_OFF  (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x120)
#define RBDMA_GLOBAL_FNSH_INTR_FIFO_OFF (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x128)
#define RBDMA_GLOBAL_NORMALTQ_STATUS_OFF \
    (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x180)
#define RBDMA_GLOBAL_NORMALTQ_EXT_STATUS_OFF \
    (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x184)
#define RBDMA_GLOBAL_URGENTTQ_STATUS_OFF \
    (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x188)
#define RBDMA_GLOBAL_URGENTTQ_EXT_STATUS_OFF \
    (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x18C)
#define RBDMA_GLOBAL_PTQ_STATUS_OFF     (RBDMA_CDMA_GLOBAL_BASE_OFF + 0x190)

/* CDMA_TASK descriptor layout (g_cdma_task_registers.h). q-cp writes
 * eight 32-bit words ascending from PTID_INIT (0x200) to RUN_CONF1
 * (0x21C). The latter store is the kickoff trigger. */
#define RBDMA_TASK_PTID_INIT_OFF        (RBDMA_CDMA_TASK_BASE_OFF + 0x000)
#define RBDMA_TASK_SRCADDRESS_OFF       (RBDMA_CDMA_TASK_BASE_OFF + 0x004)
#define RBDMA_TASK_DESTADDRESS_OFF      (RBDMA_CDMA_TASK_BASE_OFF + 0x008)
#define RBDMA_TASK_SIZEOF128BLK_OFF     (RBDMA_CDMA_TASK_BASE_OFF + 0x00C)
#define RBDMA_TASK_RUN_CONF0_OFF        (RBDMA_CDMA_TASK_BASE_OFF + 0x018)
#define RBDMA_TASK_RUN_CONF1_OFF        (RBDMA_CDMA_TASK_BASE_OFF + 0x01C)
#define RBDMA_TASK_RUN_CONF1_INTR_DISABLE_BIT   (1U << 0)

/* RUN_CONF0 bit layout (union rbdma_td_run_conf0,
 * g_cdma_task_registers.h):
 *   task_type:4 | split_granule_l2:4 | ext_num_of_chunk:12 |
 *   src_addr_msb:2 | dst_addr_msb:2  | tsync_dnc_code:4    |
 *   ext_dnc_mask:3 | fid_max:1
 *
 * P4B's OTO byte-mover only needs task_type and the two
 * {src,dst}_addr_msb fields. The other bits are honoured by silicon's
 * full RBDMA but are functionally inert at a byte-copy level — q-cp's
 * existing test framework hits OTO with most of these zero. */
#define RBDMA_RUN_CONF0_TASK_TYPE_SHIFT   0
#define RBDMA_RUN_CONF0_TASK_TYPE_MASK    0xFu
#define RBDMA_RUN_CONF0_SRC_ADDR_MSB_SHIFT  20
#define RBDMA_RUN_CONF0_SRC_ADDR_MSB_MASK   0x3u
#define RBDMA_RUN_CONF0_DST_ADDR_MSB_SHIFT  22
#define RBDMA_RUN_CONF0_DST_ADDR_MSB_MASK   0x3u

/* enum rbdma_task_type (g_cmd_descr_rbdma_rebel.h). Only OTO is
 * behaviourally implemented today; other task_types fall through to
 * the kick-acknowledge stub so q-cp's done loop doesn't deadlock
 * (LOG_UNIMP fires once per kick to surface the gap). */
#define RBDMA_TASK_TYPE_OTO     0u
#define RBDMA_TASK_TYPE_CST     1u
#define RBDMA_TASK_TYPE_GTH     2u
#define RBDMA_TASK_TYPE_SCT     3u
#define RBDMA_TASK_TYPE_OTM     4u
#define RBDMA_TASK_TYPE_GTHR    6u
#define RBDMA_TASK_TYPE_SCTR    7u
#define RBDMA_TASK_TYPE_PTL     8u
#define RBDMA_TASK_TYPE_IVL     9u
#define RBDMA_TASK_TYPE_VCM    10u
#define RBDMA_TASK_TYPE_DUM    11u
#define RBDMA_TASK_TYPE_DAS    12u

/* Address encoding. Both src and dst are stored as 128 B-units split
 * across two registers: low-32 bits in SRCADDRESS_OR_CONST /
 * DESTADDRESS, high 2 bits in RUN_CONF0.{src,dst}_addr_msb. The
 * device's outgoing AXI traffic shifts left by 7 to produce a 41-bit
 * byte address. */
#define RBDMA_ADDR_GRAIN_SHIFT  7      /* 128 B blocks */

/* Outer cap on a single OTO byte-move. Mirrors umd cmdgen's
 * `args->size > SZ_32M` guard. Anything larger is q-cp scatter (CST /
 * GTH / SCTR / ...) which we defer to a later P4B-extension. */
#define RBDMA_OTO_MAX_BYTES     (32u * 1024u * 1024u)

/* Synthetic IP_INFO seeds. q-cp logs ip_info0/1 verbatim, decodes
 * info3.num_of_executer for TE iteration, and decodes info4/info5 for
 * queue-credit caps. Choose values plausible for a "8 TE / 32-deep
 * everywhere" silicon flavour. */
#define R100_RBDMA_NUM_TE                   8u
#define R100_RBDMA_NUM_TQ                  32u
#define R100_RBDMA_NUM_UTQ                 32u
#define R100_RBDMA_NUM_PTQ                 32u
#define R100_RBDMA_NUM_TEQ                 32u
#define R100_RBDMA_NUM_FNSH_FIFO           32u
#define R100_RBDMA_NUM_ERR_FIFO             8u

/* FIFO depth for our completion ring. Must accommodate the deepest
 * possible q-cp burst before the BH drains it. Sized to match
 * R100_RBDMA_NUM_FNSH_FIFO so we never silently coalesce while q-cp
 * still has FNSH_FIFO credit. */
#define R100_RBDMA_FIFO_DEPTH               R100_RBDMA_NUM_FNSH_FIFO

#define RBDMA_INFO0_REL_DATE                0x20260101U
#define RBDMA_INFO1_SEED \
    ((1U << 24) | (1U << 16) | (1U << 8) | 0U)        /* {1,1,1,0} */
#define RBDMA_INFO3_SEED \
    ((R100_RBDMA_NUM_TE & 0xFFu) << 8 | 0u)
    /* info3: {num_of_totalutlb:8, num_of_executer:8, num_of_max_sgr:12,
     *         num_of_max_sgi:4} — only num_of_executer matters today;
     * the SGR/SGI fields are unused by q-cp's task path. utlb count
     * stays 0 (q-cp's utlb credit poll degenerates to "all free"). */
#define RBDMA_INFO4_SEED \
    (((R100_RBDMA_NUM_PTQ & 0xFFu) << 16) | \
     ((R100_RBDMA_NUM_UTQ & 0xFFu) <<  8) | \
     (R100_RBDMA_NUM_TQ & 0xFFu))
    /* info4: {num_of_tqueue:8, num_of_utqueue:8, num_of_ptq:8, rsvd:8}. */
#define RBDMA_INFO5_SEED \
    (((R100_RBDMA_NUM_ERR_FIFO  & 0xFFu) << 24) | \
     ((R100_RBDMA_NUM_FNSH_FIFO & 0xFFu) << 16) | \
     ((R100_RBDMA_NUM_TEQ       & 0xFFu) <<  8) | \
     (R100_RBDMA_NUM_TEQ        & 0xFFu))
    /* info5: {num_of_tequeue:8, num_of_uetqueue:8, num_of_fnsh_fifo:8,
     *         num_of_err_fifo:8} — uetqueue alongside tequeue
     * symmetric to TQ/UTQ. */

/* ========================================================================
 * Sparse register file. Same shape as r100_dnc.c / r100_hbm.c — a
 * GHashTable from offset → uint32_t. Kept inline here rather than
 * shared because the three users have diverged on default-policy:
 * RBDMA's reads also synthesise live FIFO state (depth, pop), which
 * a generic helper would have to bypass anyway.
 * ======================================================================== */

typedef struct R100RBDMARegStore {
    GHashTable *regs;
} R100RBDMARegStore;

static void rbdma_regstore_init(R100RBDMARegStore *rs)
{
    rs->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static void rbdma_regstore_reset(R100RBDMARegStore *rs)
{
    if (rs->regs) {
        g_hash_table_remove_all(rs->regs);
    }
}

static bool rbdma_regstore_lookup(R100RBDMARegStore *rs, hwaddr addr,
                                  uint32_t *out)
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

static void rbdma_regstore_store(R100RBDMARegStore *rs, hwaddr addr,
                                 uint32_t val)
{
    g_hash_table_insert(rs->regs, GUINT_TO_POINTER((guint)addr),
                        GUINT_TO_POINTER((guint)val));
}

/* ========================================================================
 * Device state
 * ======================================================================== */

/* One pending RBDMA task awaiting BH-delivered completion. ptid_init
 * is the captured task->id_info.bits — q-cp's done_handler reads
 * FNSH_INTR_FIFO and matches its low 28 bits against the pending
 * cmd's id_info to find the right cb to mark complete. */
typedef struct R100RBDMAFnshEntry {
    uint32_t ptid_init;
    bool intr_enabled;
} R100RBDMAFnshEntry;

struct R100RBDMAState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    R100RBDMARegStore store;
    uint32_t chiplet_id;

    /* P4A — done IRQ + a small ring of pending FNSH entries. */
    qemu_irq err_irq;       /* INT_ID_RBDMA0_ERR — reserved, never pulsed */
    qemu_irq done_irq;      /* INT_ID_RBDMA1     — fnsh-IRQ trigger */
    QEMUBH *done_bh;

    R100RBDMAFnshEntry fnsh_fifo[R100_RBDMA_FIFO_DEPTH];
    uint32_t fnsh_head;     /* next slot to read (consumer side) */
    uint32_t fnsh_tail;     /* next slot to write (producer side) */

    /* Observability counters survive reset (matches r100-dnc-cluster
     * convention so cumulative tests can assert across boots). */
    uint64_t kicks;
    uint64_t completes;
    uint64_t pulses_fired;
    uint64_t fifo_overflows;

    /* P4B — RBDMA OTO byte-mover stats. `oto_bytes` accumulates the
     * total bytes physically moved between SAR/DAR via
     * address_space_{read,write}; useful in tests + post-mortem. */
    uint64_t oto_kicks;
    uint64_t oto_bytes;
    uint64_t oto_dma_errors;
    uint64_t unimp_task_kicks;
};
typedef struct R100RBDMAState R100RBDMAState;

DECLARE_INSTANCE_CHECKER(R100RBDMAState, R100_RBDMA, TYPE_R100_RBDMA)

/* ========================================================================
 * Synthetic defaults — first-read values for registers q-cp polls
 * before storing anything. After a write, the regstore wins (see read
 * dispatch below).
 * ======================================================================== */

static uint32_t rbdma_default(R100RBDMAState *s, hwaddr addr)
{
    switch (addr) {
    case RBDMA_GLOBAL_IP_INFO0_OFF:
        return RBDMA_INFO0_REL_DATE;
    case RBDMA_GLOBAL_IP_INFO1_OFF:
        return RBDMA_INFO1_SEED;
    case RBDMA_GLOBAL_IP_INFO2_OFF:
        /* {chiplet_id:8, reserved:24}. q-cp's `rbdma_get_ip_info`
         * overwrites this with the explicit `info2.chiplet_id =
         * cl_id` store; this default just covers the first read. */
        return s->chiplet_id & 0xFFU;
    case RBDMA_GLOBAL_IP_INFO3_OFF:
        return RBDMA_INFO3_SEED;
    case RBDMA_GLOBAL_IP_INFO4_OFF:
        return RBDMA_INFO4_SEED;
    case RBDMA_GLOBAL_IP_INFO5_OFF:
        return RBDMA_INFO5_SEED;
    default:
        return 0;
    }
}

/* ========================================================================
 * Active path — kick → BH → FNSH push → done IRQ
 * ======================================================================== */

static uint32_t r100_rbdma_fnsh_depth(const R100RBDMAState *s)
{
    if (s->fnsh_tail >= s->fnsh_head) {
        return s->fnsh_tail - s->fnsh_head;
    }
    return R100_RBDMA_FIFO_DEPTH - (s->fnsh_head - s->fnsh_tail);
}

static bool r100_rbdma_fnsh_push(R100RBDMAState *s,
                                 const R100RBDMAFnshEntry *e)
{
    uint32_t next_tail = (s->fnsh_tail + 1) % R100_RBDMA_FIFO_DEPTH;

    if (next_tail == s->fnsh_head) {
        s->fifo_overflows++;
        return false;
    }
    s->fnsh_fifo[s->fnsh_tail] = *e;
    s->fnsh_tail = next_tail;
    return true;
}

static bool r100_rbdma_fnsh_pop(R100RBDMAState *s, R100RBDMAFnshEntry *out)
{
    if (s->fnsh_head == s->fnsh_tail) {
        return false;
    }
    *out = s->fnsh_fifo[s->fnsh_head];
    s->fnsh_head = (s->fnsh_head + 1) % R100_RBDMA_FIFO_DEPTH;
    return true;
}

/* BH: one pulse per pushed entry whose intr_disable bit was clear,
 * mirroring silicon (each task-done emits one fnsh-fifo entry, the
 * IRQ-fanned-out handler then drains the FIFO via successive
 * FNSH_INTR_FIFO reads). qemu_irq_pulse coalescing is benign because
 * the handler's drain loop walks until INTR_FIFO_READABLE_NUM = 0.
 *
 * We don't pop here: the regstore-side FNSH_INTR_FIFO read is the
 * consumer. The BH only walks the ring forward to count + log the
 * pulses; the entries stay in the FIFO until q-cp reads them. */
static void r100_rbdma_done_bh(void *opaque)
{
    R100RBDMAState *s = opaque;
    uint32_t walk = s->fnsh_head;
    uint32_t tail = s->fnsh_tail;

    /* Re-snapshot at most R100_RBDMA_FIFO_DEPTH entries so a wrap
     * doesn't loop us indefinitely if a kick fires concurrently. */
    while (walk != tail) {
        const R100RBDMAFnshEntry *e = &s->fnsh_fifo[walk];

        if (e->intr_enabled && s->done_irq) {
            qemu_irq_pulse(s->done_irq);
            s->pulses_fired++;
            qemu_log_mask(LOG_TRACE,
                          "r100-rbdma cl=%u kick → fnsh ptid=0x%08x "
                          "fired=%" PRIu64 " depth=%u\n",
                          s->chiplet_id, e->ptid_init,
                          s->pulses_fired, r100_rbdma_fnsh_depth(s));
        }
        walk = (walk + 1) % R100_RBDMA_FIFO_DEPTH;
        s->completes++;
    }

    /* `kicks` and `completes` may diverge if pulses run ahead of
     * regstore reads — that's expected, the consumer-side
     * FNSH_INTR_FIFO read is what actually drains the queue. The BH
     * runs at most once per kick; subsequent re-runs are idempotent
     * because we recompute walk from the current head each time. */
}

/* P4B — OTO byte mover. Runs inline on RUN_CONF1 store, before the
 * FNSH push, so q-cp's done-handler always observes the data already
 * landed at DAR by the time it walks the cb completion logic.
 *
 * Address handling — read this carefully, the silicon path differs:
 *
 *   On real silicon, SAR / DAR carry **device virtual addresses (DVAs)**
 *   programmed by q-cp from kmd-allocated cb_descr fields. The RBDMA
 *   engine emits AXI bursts that traverse the per-chiplet SMMU-600
 *   (`r100_smmu.c`'s real counterpart), which performs the S1 + S2
 *   page-table walk to translate DVA → output PA before the DDR
 *   controller / iATU sees the request.
 *
 *   REMU does NOT model SMMU translation. `r100_smmu.c` is a
 *   register-only stub — it acks `CR0→CR0ACK`, auto-advances
 *   `CMDQ_CONS=PROD`, and never walks the STE / CD / page tables FW
 *   sets up in DRAM. The SMMU's effective transform in REMU is
 *   therefore `S1 ∘ S2 = identity`. This matches how `r100-hdma`,
 *   `r100-dnc-cluster`, and the rest of the engine fleet already
 *   handle DVAs (see `r100_hdma.c:r100_hdma_kick_wr` — same shape).
 *   Honouring real FW page tables is tracked separately as a
 *   long-term follow-on (`docs/roadmap.md` → "SMMU honour FW page
 *   tables"); the natural plug point would be a translation hook
 *   here just before the address_space_{read,write} call.
 *
 *   The `chiplet_base += chiplet_id * R100_CHIPLET_OFFSET` step below
 *   is **not** a substitute for SMMU translation. It's REMU's flat
 *   global-vs-chiplet-local plumbing: every chiplet's DRAM is mounted
 *   at its own offset in `&address_space_memory`, and engines on
 *   chiplet N see chiplet-local addresses on their NoC. Without the
 *   add, a SAR like 0x100600000 from chiplet-2 RBDMA would land in
 *   chiplet 0's DRAM instead of chiplet 2's. r100-hdma uses the same
 *   convention.
 *
 * Returns true on success. On address_space failure we log GUEST_ERROR
 * but still let the caller push the FNSH entry so q-cp doesn't
 * deadlock waiting for a completion that will never arrive — the
 * post-mortem log + the stale dst memory together signal the error. */
static bool r100_rbdma_do_oto(R100RBDMAState *s)
{
    uint32_t src_lo = 0;
    uint32_t dst_lo = 0;
    uint32_t blk128 = 0;
    uint32_t run_conf0 = 0;
    uint64_t src, dst, size;
    uint64_t chiplet_base;
    uint8_t *buf;
    MemTxResult mr;

    (void)rbdma_regstore_lookup(&s->store,
                                RBDMA_TASK_SRCADDRESS_OFF, &src_lo);
    (void)rbdma_regstore_lookup(&s->store,
                                RBDMA_TASK_DESTADDRESS_OFF, &dst_lo);
    (void)rbdma_regstore_lookup(&s->store,
                                RBDMA_TASK_SIZEOF128BLK_OFF, &blk128);
    (void)rbdma_regstore_lookup(&s->store,
                                RBDMA_TASK_RUN_CONF0_OFF, &run_conf0);

    {
        uint32_t src_msb = (run_conf0 >> RBDMA_RUN_CONF0_SRC_ADDR_MSB_SHIFT)
                           & RBDMA_RUN_CONF0_SRC_ADDR_MSB_MASK;
        uint32_t dst_msb = (run_conf0 >> RBDMA_RUN_CONF0_DST_ADDR_MSB_SHIFT)
                           & RBDMA_RUN_CONF0_DST_ADDR_MSB_MASK;

        src = ((((uint64_t)src_msb) << 32) | src_lo)
              << RBDMA_ADDR_GRAIN_SHIFT;
        dst = ((((uint64_t)dst_msb) << 32) | dst_lo)
              << RBDMA_ADDR_GRAIN_SHIFT;
    }
    size = (uint64_t)blk128 << RBDMA_ADDR_GRAIN_SHIFT;

    if (size == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-rbdma cl=%u OTO: zero-sized kick "
                      "(src=0x%" PRIx64 " dst=0x%" PRIx64 ")\n",
                      s->chiplet_id, src, dst);
        return false;
    }
    if (size > RBDMA_OTO_MAX_BYTES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-rbdma cl=%u OTO: size 0x%" PRIx64
                      " > cap 0x%x — clamping\n",
                      s->chiplet_id, size, RBDMA_OTO_MAX_BYTES);
        size = RBDMA_OTO_MAX_BYTES;
    }

    chiplet_base = (uint64_t)s->chiplet_id * R100_CHIPLET_OFFSET;
    src += chiplet_base;
    dst += chiplet_base;

    buf = g_malloc(size);
    mr = address_space_read(&address_space_memory, src,
                            MEMTXATTRS_UNSPECIFIED, buf, size);
    if (mr != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-rbdma cl=%u OTO: src read failed @ 0x%"
                      PRIx64 " size=0x%" PRIx64 "\n",
                      s->chiplet_id, src, size);
        s->oto_dma_errors++;
        g_free(buf);
        return false;
    }
    mr = address_space_write(&address_space_memory, dst,
                             MEMTXATTRS_UNSPECIFIED, buf, size);
    if (mr != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-rbdma cl=%u OTO: dst write failed @ 0x%"
                      PRIx64 " size=0x%" PRIx64 "\n",
                      s->chiplet_id, dst, size);
        s->oto_dma_errors++;
        g_free(buf);
        return false;
    }
    g_free(buf);

    s->oto_kicks++;
    s->oto_bytes += size;
    qemu_log_mask(LOG_TRACE,
                  "r100-rbdma cl=%u OTO: %" PRIu64 " B "
                  "src=0x%" PRIx64 " → dst=0x%" PRIx64
                  " (oto_kicks=%" PRIu64 ")\n",
                  s->chiplet_id, size, src, dst, s->oto_kicks);
    return true;
}

static void r100_rbdma_kickoff(R100RBDMAState *s, uint32_t run_conf1)
{
    R100RBDMAFnshEntry e;
    uint32_t ptid_init = 0;
    uint32_t run_conf0 = 0;
    uint32_t task_type;

    /* PTID_INIT (offset 0x200) was the first descriptor word q-cp
     * wrote — regstore has it. Missing entry means a stray RUN_CONF1
     * write with no preceding PTID_INIT; ptid_init=0 is harmless
     * (q-cp's done_handler will fail to match it against any
     * outstanding cmd and log GUEST_ERROR — that's the right
     * behaviour, the FW bug is upstream). */
    (void)rbdma_regstore_lookup(&s->store, RBDMA_TASK_PTID_INIT_OFF,
                                &ptid_init);

    /* P4B — dispatch on task_type. OTO does the real byte move; every
     * other task_type falls through to the kick-acknowledge stub so
     * q-cp's done loop unwinds (umd's `simple_copy` only emits OTO,
     * but q-sys unit_test/rbdma_test.c walks the full enum). */
    (void)rbdma_regstore_lookup(&s->store, RBDMA_TASK_RUN_CONF0_OFF,
                                &run_conf0);
    task_type = (run_conf0 >> RBDMA_RUN_CONF0_TASK_TYPE_SHIFT)
                & RBDMA_RUN_CONF0_TASK_TYPE_MASK;

    switch (task_type) {
    case RBDMA_TASK_TYPE_OTO:
        (void)r100_rbdma_do_oto(s);
        break;
    default:
        s->unimp_task_kicks++;
        qemu_log_mask(LOG_UNIMP,
                      "r100-rbdma cl=%u: task_type=%u not yet "
                      "behavioural — completing kick without byte "
                      "move (ptid=0x%08x)\n",
                      s->chiplet_id, task_type, ptid_init);
        break;
    }

    e.ptid_init = ptid_init;
    e.intr_enabled =
        !(run_conf1 & RBDMA_TASK_RUN_CONF1_INTR_DISABLE_BIT);

    if (!r100_rbdma_fnsh_push(s, &e)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-rbdma cl=%u: FNSH FIFO full (%u entries) "
                      "— drop kick ptid=0x%08x\n",
                      s->chiplet_id, R100_RBDMA_FIFO_DEPTH, ptid_init);
        return;
    }
    s->kicks++;

    if (s->done_bh) {
        qemu_bh_schedule(s->done_bh);
    }
}

/* ========================================================================
 * MMIO read/write
 * ======================================================================== */

static uint64_t r100_rbdma_read(void *opaque, hwaddr addr, unsigned size)
{
    R100RBDMAState *s = R100_RBDMA(opaque);
    uint32_t stored;
    R100RBDMAFnshEntry e;

    /* Live-state taps that sit ABOVE the regstore — these mustn't
     * round-trip a stale store value once the device's runtime state
     * has moved on. */
    switch (addr) {
    case RBDMA_GLOBAL_INTR_FIFO_NUM_OFF:
        /* {fnsh:8, err:8, reserved:16}. q-cp's done_handler loops
         * while `fnsh > 0`. err stays 0 (no synth errors). */
        return r100_rbdma_fnsh_depth(s) & 0xFFu;

    case RBDMA_GLOBAL_FNSH_INTR_FIFO_OFF:
        /* Pop one entry — silicon: reading this register returns the
         * head of the fnsh FIFO and decrements the readable count. */
        if (r100_rbdma_fnsh_pop(s, &e)) {
            return e.ptid_init;
        }
        return 0;

    case RBDMA_GLOBAL_NORMALTQ_STATUS_OFF:
        return R100_RBDMA_NUM_TQ;
    case RBDMA_GLOBAL_NORMALTQ_EXT_STATUS_OFF:
        return R100_RBDMA_NUM_TEQ;
    case RBDMA_GLOBAL_URGENTTQ_STATUS_OFF:
        return R100_RBDMA_NUM_UTQ;
    case RBDMA_GLOBAL_URGENTTQ_EXT_STATUS_OFF:
        return R100_RBDMA_NUM_TEQ;
    case RBDMA_GLOBAL_PTQ_STATUS_OFF:
        return R100_RBDMA_NUM_PTQ;
    default:
        break;
    }

    /* Honour FW writes for everything else (RMW init sequences, task
     * descriptor read-back during error handling). */
    if (rbdma_regstore_lookup(&s->store, addr, &stored)) {
        return stored;
    }
    return rbdma_default(s, addr);
}

static void r100_rbdma_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    R100RBDMAState *s = R100_RBDMA(opaque);

    rbdma_regstore_store(&s->store, addr, (uint32_t)val);

    /* Kick on RUN_CONF1 store. q-cp's `rbdma_send_task` walks the
     * descriptor in ascending order, so RUN_CONF1 is always the last
     * write of the burst — captured PTID_INIT is fresh. */
    if (addr == RBDMA_TASK_RUN_CONF1_OFF && size == 4) {
        r100_rbdma_kickoff(s, (uint32_t)val);
    }
}

static const MemoryRegionOps r100_rbdma_ops = {
    .read = r100_rbdma_read,
    .write = r100_rbdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* ========================================================================
 * QOM glue
 * ======================================================================== */

static void r100_rbdma_realize(DeviceState *dev, Error **errp)
{
    R100RBDMAState *s = R100_RBDMA(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-rbdma.cl%u", s->chiplet_id);
    rbdma_regstore_init(&s->store);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_rbdma_ops, s,
                          name, R100_NBUS_L_RBDMA_CFG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    /* Two SPI lines: ERR (idx 0) + DONE (idx 1). r100_soc.c connects
     * both to the matching INT_ID_RBDMA{0_ERR,1} GIC inputs on this
     * chiplet's GIC. ERR slot is reserved for symmetry / future
     * synthesised faults; never pulsed today. */
    sysbus_init_irq(sbd, &s->err_irq);
    sysbus_init_irq(sbd, &s->done_irq);

    s->done_bh = qemu_bh_new(r100_rbdma_done_bh, s);
    s->fnsh_head = 0;
    s->fnsh_tail = 0;
    s->kicks = 0;
    s->completes = 0;
    s->pulses_fired = 0;
    s->fifo_overflows = 0;
    s->oto_kicks = 0;
    s->oto_bytes = 0;
    s->oto_dma_errors = 0;
    s->unimp_task_kicks = 0;
}

static void r100_rbdma_reset(DeviceState *dev)
{
    R100RBDMAState *s = R100_RBDMA(dev);
    rbdma_regstore_reset(&s->store);
    s->fnsh_head = 0;
    s->fnsh_tail = 0;
    if (s->done_bh) {
        qemu_bh_cancel(s->done_bh);
    }
    /* Counters intentionally survive reset. */
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

static void r100_rbdma_register_types(void)
{
    type_register_static(&r100_rbdma_info);
}

type_init(r100_rbdma_register_types)
