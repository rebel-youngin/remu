/*
 * REMU - R100 NPU System Emulator
 * SMMU-600 TCU device — register block + stage-2 page-table walker +
 * eventq / GERROR fault delivery.
 *
 * MMIO region at TCU_OFFSET (0x1FF4200000, per
 * external/.../rebel_h_baseoffset.h), instantiated per chiplet from
 * `r100_soc.c:r100_create_smmu`. Three roles:
 *
 *   1. **MMIO surface for FW init paths** (BL2 / FreeRTOS / q-cp):
 *      - BL2 `smmu_early_init` (EL3, one-shot at boot): programs
 *        EVENTQ base, STRTAB base, GBPA fence, then enables event
 *        queues via CR0 and spins on:
 *          a) `while (!(cr0ack & EVENTQEN))` — we mirror writes to
 *             SMMU_CR0 into SMMU_CR0ACK (masked to EN bits).
 *          b) `while (gbpa & UPDATE)` — we always clear the UPDATE
 *             bit on GBPA writes.
 *      - FreeRTOS EL1 `smmu_cmdq_enqueue_cmd`: writes a command to
 *        the next slot in the in-DRAM CMDQ (base = SMMU_CMDQ_BASE
 *        PA, size = 1<<log2), increments PROD, polls CONS for space.
 *      - `smmu_sync()`: posts a CMD_SYNC with CS=SIG_IRQ and msiaddr
 *        set back to the sync slot itself, then spins on the first
 *        u32 of that slot turning 0 (the SMMU's MSI write data,
 *        reset value 0, overwrites cmd[0] on completion). We hook
 *        the PROD write: walk from old-CONS to new-PROD, write a
 *        u32 0 to the MSI address of every CMD_SYNC entry, advance
 *        CONS to match PROD so `smmu_q_has_space` immediately
 *        reports space.
 *      Without CMDQ processing the poll times out 1000 times a
 *      second and the HILS log fills with `[smmu] Failed to sync`.
 *      - FreeRTOS `smmu_init` → `smmu_enable_queues`: writes
 *        `SMMU_EVENTQ_BASE / PROD / CONS` (PROD/CONS at 0x100A8 /
 *        0x100AC, in MMIO page 1 above the standard CR0 / CMDQ
 *        page) + `CR0.EVENTQEN`. We cache the eventq geometry
 *        (`eventq_base_pa`, `eventq_log2size`) so emit doesn't
 *        re-decode on every fault, and the CR0ACK mirror covers
 *        EVENTQEN so the FW spin exits.
 *      - `smmu_enable_interrupt`: writes
 *        `IRQ_CTRL = GERROR_IRQEN | EVENTQ_IRQEN` and polls
 *        `IRQ_CTRLACK`. Both gates are honoured by the
 *        `r100_smmu_emit_event` / `r100_smmu_raise_gerror` helpers
 *        before they pulse the matching sysbus IRQ output.
 *
 *   2. **Public translate API for engines that consume DVAs** (P11):
 *      `r100_smmu_translate(s, sid, ssid, dva, access, *out)` —
 *      see `r100_smmu.h` for the contract. Internally:
 *        a) Read the chiplet's CR0 latch — if !SMMUEN, identity.
 *        b) Read the STE for `sid` from the in-DRAM stream table at
 *           STRTAB_BASE_PA + sid * 64 (SMMU-600 STEs are 64 B / 8
 *           dwords; q-sys's `st[sid].ste[0..7]` matches).
 *        c) Decode STE0.{V, config}:
 *             - !V or ABORT → fault.
 *             - BYPASS → identity.
 *             - S1_TRANS → v1 stage-1 not implemented, identity +
 *               LOG_UNIMP. q-cp's `smmu_init_ste` sets STE1.S1DSS=BYPASS
 *               for the 4 SIDs it leaves at S1_TRANS, so the effective
 *               behaviour is identity anyway — v1 collapses that to
 *               "identity + log once" and skips the CD walk. v2 will
 *               walk CD per SSID.
 *             - S2_TRANS / ALL_TRANS → build SMMUTransCfg from
 *               STE2/STE3 and dispatch to QEMU's `smmu_ptw()` with
 *               stage=SMMU_STAGE_2. The walker reads PTEs through
 *               `address_space_memory`, so STE3 (S2TTB) is converted
 *               to a global PA (chiplet-local + chiplet_id *
 *               R100_CHIPLET_OFFSET) before we hand it to the cfg.
 *      Faults route through `r100_smmu_emit_event` (below) so FW's
 *      `smmu_event_intr` handler sees a real entry on the eventq.
 *
 *   3. **Eventq / GERROR fault delivery** (v2):
 *      - `r100_smmu_emit_event(s, type, sid, input_addr)`: builds a
 *        32-byte SMMUv3 event record, writes it to `eventq_base_pa +
 *        prod_idx * 32`, advances PROD with wrap, and pulses
 *        `evt_irq` (SPI 762) if `IRQ_CTRL.EVENTQ_IRQEN=1`.
 *        Encoding matches FW's `smmu_print_event` reader:
 *          evt[0] bits[7:0] = event_id (0x03=F_STE_FETCH, 0x04=C_BAD_STE,
 *            0x10=F_TRANSLATION, 0x11=F_ADDR_SIZE, 0x12=F_ACCESS,
 *            0x13=F_PERMISSION, 0x0b=F_WALK_EABT — see
 *            `q/sys/drivers/smmu/smmu.c:165` for the full table).
 *          evt[1]            = sid
 *          evt[4..5]         = input_addr (low/high)
 *        Eventq overflow (PROD == CONS but wrap differs) triggers
 *        `GERROR_EVTQ_ABT_ERR` and the event is dropped.
 *      - `r100_smmu_raise_gerror(s, bit)`: toggles `bit` in
 *        `SMMU_GERROR`, which makes `GERROR ^ GERRORN` non-zero so
 *        FW's `smmu_gerr_intr` sees an active error. Pulses
 *        `gerr_irq` (SPI 765) if `IRQ_CTRL.GERROR_IRQEN=1`. FW
 *        ack: write the post-toggle GERROR value to GERRORN; the
 *        `^=` semantics naturally clear once both registers match.
 *      - GERROR sources today: `GERROR_EVTQ_ABT_ERR` on eventq
 *        overflow, `GERROR_CMDQ_ERR` on illegal CMDQ opcode (Phase
 *        deferred — current CMDQ walker logs unknown opcodes but
 *        doesn't escalate; the recognised opcode set covers
 *        everything FW emits today).
 *
 * STRTAB_BASE / STRTAB_BASE_CFG cache: writes to those offsets update
 * private fields (`strtab_base_pa`, `strtab_log2size`) so the lookup
 * doesn't re-decode on every translate. v1 only supports format=LINEAR
 * (q-sys uses LINEAR for ≤ 32 SIDs; 2LVL is v2). Out-of-range `sid`
 * (≥ 1<<log2size) returns INV_STE.
 *
 * v1 deliberately omits:
 *   - IOTLB cache (LL chains are 3-4 elements; cost is in the chain
 *     reads, not the walk).
 *   - CMD_TLBI_* / CMD_CFGI_* honest invalidation (today's CMDQ
 *     processor still drains every entry and answers SYNC, which is
 *     what FW polls — no FW-visible regression). v1 of the walker
 *     re-reads STE / S2TTB on every translate, so stale STEs aren't
 *     possible. v2 adds an STE cache + invalidation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "exec/cpu-common.h"
#include "exec/address-spaces.h"
#include "hw/arm/smmu-common.h"

#include "r100_soc.h"
#include "r100_smmu.h"
#include "r100/remu_addrmap.h"

/* SMMU-600 has two MMIO pages: page 0 carries CR0 / IRQ_CTRL / STRTAB /
 * CMDQ / EVENTQ_BASE etc (offset 0x0..0xFFF and the wider 0x0..0xFFFF
 * window most FW touches); page 1 starts at 0x10000 and carries the
 * EVENTQ_PROD / CONS / PRIQ_PROD / CONS pairs (so a hypervisor can
 * page-protect the queue pointer access independently). q-sys writes
 * SMMU_EVENTQ_PROD = 0x100A8 in `smmu_enable_queues`, so REMU's MMIO
 * region must extend across both pages. 128 KB is the smallest power
 * of two that covers the highest offset (0x100CC = SMMU_PRIQ_CONS). */
#define R100_SMMU_REG_SIZE      0x20000
#define R100_SMMU_REG_COUNT     (R100_SMMU_REG_SIZE / 4)

/* SMMU-600 STE is 8 × 64-bit dwords = 64 bytes (Arm SMMU v3.2 spec,
 * § 5.2 "Stream Table Entry"). q-sys lays out `st[sid].ste[0..7]`
 * the same way (drivers/smmu/smmu.c). */
#define R100_SMMU_STE_BYTES     64u

/* SMMU-600 event queue entry is 4 × 64-bit dwords = 32 bytes. q-sys's
 * `EVTQ_ENTRY_DWORDS = 4` matches; `smmu_evtq_dequeue` reads 4 dwords
 * out of the queue per entry, then `smmu_print_event` decodes
 * evt[0..7] (8 × uint32_t = 32 bytes) for type / SID / input_addr. */
#define R100_SMMU_EVTQ_BYTES    32u

struct R100SMMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_SMMU_REG_COUNT];
    uint32_t chiplet_id;

    /* Sysbus IRQ outputs — wired in r100_soc.c r100_create_smmu to the
     * chiplet's GIC at SPI 762 (NS event queue) and SPI 765 (GERROR).
     * Edge-triggered per q-sys's `connect_interrupt_with_handler(...,
     * IRQ_TYPE_EDGE, ...)`, so emit + raise paths use `qemu_irq_pulse`
     * (raise + lower in the same BQL window) rather than level. */
    qemu_irq evt_irq;
    qemu_irq gerr_irq;

    /* Cached CMDQ geometry from the last SMMU_CMDQ_BASE write. */
    uint64_t cmdq_base_pa;      /* PA of first entry in guest memory */
    uint32_t cmdq_log2size;     /* log2(#entries); valid range 0..19 */

    /* Cached EVENTQ geometry from the last SMMU_EVENTQ_BASE write.
     * Like cmdq, FW writes a global PA (q-sys's `smmu_init_queue`
     * folds CHIPLET_BASE_ADDR into the BASE write before it hits the
     * MMIO), so the cached value is ready for `cpu_physical_memory_*`
     * without further chiplet-base arithmetic on emit. */
    uint64_t eventq_base_pa;    /* PA of first event entry */
    uint32_t eventq_log2size;   /* log2(#entries); EVTQ_LOG2 = 10 today */

    /* Cached STRTAB geometry from the last SMMU_STRTAB_BASE /
     * SMMU_STRTAB_BASE_CFG write. PA is global (chiplet-base
     * adjusted at write time) so the translate fast path doesn't
     * re-add it on every call. */
    uint64_t strtab_base_pa;    /* global PA of first STE */
    uint32_t strtab_log2size;   /* log2(num SIDs); STE table is linear */
    uint32_t strtab_fmt;        /* 0=LINEAR, 1=2LVL (v1 supports LINEAR) */

    /* Stats — survive reset; useful for diagnosing eventq / GERROR
     * pressure when FW logs an `Event ...` line. */
    uint64_t events_emitted;
    uint64_t events_dropped;    /* eventq full or disabled */
    uint64_t gerror_raised;
};

DECLARE_INSTANCE_CHECKER(R100SMMUState, R100_SMMU, TYPE_R100_SMMU)

/* SMMU-600 register offsets used by TF-A (see struct smmu600_regs +
 * `external/.../q/sys/common/headers/fw/drivers/smmu/smmu.h`). */
#define SMMU_CR0                0x20
#define SMMU_CR0ACK             0x24
#define SMMU_GBPA               0x44
#define SMMU_IRQ_CTRL           0x50
#define SMMU_IRQ_CTRLACK        0x54
#define SMMU_GERROR             0x60
#define SMMU_GERRORN            0x64
#define SMMU_GERROR_IRQ_CFG0    0x68    /* 64-bit */
#define SMMU_GERROR_IRQ_CFG0_HI 0x6C
#define SMMU_GERROR_IRQ_CFG1    0x70
#define SMMU_GERROR_IRQ_CFG2    0x74
#define SMMU_STRTAB_BASE        0x80    /* 64-bit */
#define SMMU_STRTAB_BASE_HI     0x84
#define SMMU_STRTAB_BASE_CFG    0x88
#define SMMU_CMDQ_BASE          0x90    /* 64-bit */
#define SMMU_CMDQ_BASE_HI       0x94
#define SMMU_CMDQ_PROD          0x98
#define SMMU_CMDQ_CONS          0x9C
#define SMMU_EVENTQ_BASE        0xA0    /* 64-bit */
#define SMMU_EVENTQ_BASE_HI     0xA4
#define SMMU_EVENTQ_IRQ_CFG0    0xB0    /* 64-bit */
#define SMMU_EVENTQ_IRQ_CFG0_HI 0xB4
#define SMMU_EVENTQ_IRQ_CFG1    0xB8
#define SMMU_EVENTQ_IRQ_CFG2    0xBC
/* EVENTQ + PRIQ PROD/CONS live in MMIO page 1 (offset 0x10000+). q-sys
 * writes SMMU_EVENTQ_PROD = 0x100A8 in `smmu_enable_queues`. */
#define SMMU_EVENTQ_PROD        0x100A8
#define SMMU_EVENTQ_CONS        0x100AC

/* IRQ_CTRL / IRQ_CTRLACK bits (q-sys `smmu.h:168-170`). FW writes
 * EVENTQ_IRQEN | GERROR_IRQEN in `smmu_enable_interrupt` and polls
 * IRQ_CTRLACK to converge. Both gate r100-smmu's edge-pulses on the
 * matching wired SPI. */
#define SMMU_IRQ_CTRL_GERROR_IRQEN  (1u << 0)
#define SMMU_IRQ_CTRL_PRIQ_IRQEN    (1u << 1)
#define SMMU_IRQ_CTRL_EVENTQ_IRQEN  (1u << 2)
#define SMMU_IRQ_CTRL_ACK_MASK      (SMMU_IRQ_CTRL_GERROR_IRQEN | \
                                     SMMU_IRQ_CTRL_PRIQ_IRQEN |   \
                                     SMMU_IRQ_CTRL_EVENTQ_IRQEN)

/* SMMU_GERROR / GERRORN: FW reads both, takes XOR to find active
 * errors. Raising = toggle bit in GERROR; FW ack = write the same
 * value to GERRORN. Bit layout from q-sys `smmu.c:49-57` — keep
 * the order identical so FW's `smmu_gerr_intr` decode lines match
 * the bits we set. */
#define SMMU_GERROR_CMDQ_ERR              (1u << 0)
#define SMMU_GERROR_EVTQ_ABT_ERR          (1u << 2)
#define SMMU_GERROR_PRIQ_ABT_ERR          (1u << 3)
#define SMMU_GERROR_MSI_CMDQ_ABT_ERR      (1u << 4)
#define SMMU_GERROR_MSI_EVTQ_ABT_ERR      (1u << 5)
#define SMMU_GERROR_MSI_PRIQ_ABT_ERR      (1u << 6)
#define SMMU_GERROR_MSI_GERROR_ABT_ERR    (1u << 7)
#define SMMU_GERROR_SFM_ERR               (1u << 8)

/* Bit masks matched against firmware-side SMMU_{EVENTQEN,SMMUEN,CMDQEN}. */
#define SMMU_CR0_SMMUEN         (1u << 0)
#define SMMU_CR0_EVENTQEN       (1u << 2)
#define SMMU_CR0_CMDQEN         (1u << 3)
#define SMMU_CR0_ACK_MASK       (SMMU_CR0_SMMUEN | SMMU_CR0_EVENTQEN | \
                                 SMMU_CR0_CMDQEN)

#define SMMU_GBPA_UPDATE        (1u << 31)

/* SMMU_CMDQ_BASE / STRTAB_BASE / EVENTQ_BASE: PA in bits[51:5]; CMDQ +
 * EVENTQ also pack log2size in bits[4:0]; STRTAB carries log2size
 * separately in STRTAB_BASE_CFG. */
#define SMMU_Q_BASE_ADDR_MASK   (0x7fffffffffffULL << 5)
#define SMMU_Q_LOG2SIZE_MASK    0x1fULL
#define SMMU_Q_LOG2SIZE_CAP     19  /* >1M entries is absurd; bail out */

/* STRTAB_BASE_CFG bits (q-sys `smmu.h:178-192`). */
#define SMMU_STRTAB_FMT_S       16
#define SMMU_STRTAB_FMT_M       (0x3u << SMMU_STRTAB_FMT_S)
#define SMMU_STRTAB_FMT_LINEAR  0
#define SMMU_STRTAB_FMT_2LVL    1
#define SMMU_STRTAB_LOG2SIZE_M  0x3fu
#define SMMU_STRTAB_LOG2SIZE_CAP 16  /* Stream table > 64K SIDs is absurd */

/* CMDQ entry is 2 dwords (16 bytes) per the SMMU-600 spec + FW. */
#define SMMU_CMDQ_ENTRY_BYTES   16u

/* Command opcode lives in cmd[0] bits[7:0]. Subset we recognise — the
 * invalidate / config-cache commands are no-ops in v1 because
 * `r100_smmu_translate` doesn't cache STE / CD / IOTLB entries (it
 * re-reads the STE from in-DRAM stream table on every call and
 * relies on QEMU's `smmu_ptw_64_s2` to read PTEs on demand). v2 adds
 * an STE / IOTLB cache and these commands gain teeth then. */
#define SMMU_CMD_OPCODE_MASK        0xFFULL
#define SMMU_CMD_PREFETCH_CONFIG    0x01ULL
#define SMMU_CMD_PREFETCH_ADDR      0x02ULL
#define SMMU_CMD_CFGI_STE           0x03ULL
#define SMMU_CMD_CFGI_STE_RANGE     0x04ULL
#define SMMU_CMD_CFGI_CD            0x05ULL
#define SMMU_CMD_CFGI_CD_ALL        0x06ULL
#define SMMU_CMD_TLBI_NH_ALL        0x10ULL
#define SMMU_CMD_TLBI_NH_ASID       0x11ULL
#define SMMU_CMD_TLBI_NH_VA         0x12ULL
#define SMMU_CMD_TLBI_NH_VAA        0x13ULL
#define SMMU_CMD_TLBI_EL2_ALL       0x20ULL
#define SMMU_CMD_TLBI_S12_VMALL     0x28ULL
#define SMMU_CMD_TLBI_S2_IPA        0x2AULL
#define SMMU_CMD_TLBI_NSNH_ALL      0x30ULL
#define SMMU_CMD_SYNC               0x46ULL

/* CMD_SYNC fields (see SYNC_0_* / SYNC_1_* in drivers/smmu/smmu.h). */
#define SMMU_SYNC_CS_MASK       (0x3ULL << 12)
#define SMMU_SYNC_CS_SIG_IRQ    (0x1ULL << 12)
#define SMMU_SYNC_MSIADDR_MASK  (0x3ffffffffffffULL << 2)

/* STE0 decode (`q-sys/common/headers/fw/drivers/smmu/smmu.h:313-326`). */
#define R100_STE0_VALID                BIT_ULL(0)
#define R100_STE0_CONFIG_S             1
#define R100_STE0_CONFIG_M             (0x7ULL << R100_STE0_CONFIG_S)
#define R100_STE0_CONFIG_ABORT         (0x0ULL << R100_STE0_CONFIG_S)
#define R100_STE0_CONFIG_BYPASS        (0x4ULL << R100_STE0_CONFIG_S)
#define R100_STE0_CONFIG_S1_TRANS      (0x5ULL << R100_STE0_CONFIG_S)
#define R100_STE0_CONFIG_S2_TRANS      (0x6ULL << R100_STE0_CONFIG_S)
#define R100_STE0_CONFIG_ALL_TRANS     (0x7ULL << R100_STE0_CONFIG_S)

/* STE2 decode (`q-sys/.../smmu.h:394-432`). */
#define R100_STE2_S2VMID_S             0
#define R100_STE2_S2VMID_M             (0xFFFFULL << R100_STE2_S2VMID_S)
#define R100_STE2_S2T0SZ_S             32
#define R100_STE2_S2T0SZ_M             (0x3FULL << R100_STE2_S2T0SZ_S)
#define R100_STE2_S2SL0_S              38
#define R100_STE2_S2SL0_M              (0x3ULL << R100_STE2_S2SL0_S)
#define R100_STE2_S2TG_S               46
#define R100_STE2_S2TG_M               (0x3ULL << R100_STE2_S2TG_S)
#define R100_STE2_S2TG_4KB             (0x0ULL)
#define R100_STE2_S2TG_64KB            (0x1ULL)
#define R100_STE2_S2TG_16KB            (0x2ULL)
#define R100_STE2_S2PS_S               48
#define R100_STE2_S2PS_M               (0x7ULL << R100_STE2_S2PS_S)
#define R100_STE2_S2AA64               BIT_ULL(51)
#define R100_STE2_S2AFFD               BIT_ULL(53)
#define R100_STE2_S2R                  BIT_ULL(58)

/* ------------------------------------------------------------------ */
/* MMIO write back-ends                                                */
/* ------------------------------------------------------------------ */

static void r100_smmu_update_cmdq_base(R100SMMUState *s)
{
    uint64_t lo = s->regs[SMMU_CMDQ_BASE >> 2];
    uint64_t hi = s->regs[SMMU_CMDQ_BASE_HI >> 2];
    uint64_t val = lo | (hi << 32);

    s->cmdq_base_pa = val & SMMU_Q_BASE_ADDR_MASK;
    s->cmdq_log2size = val & SMMU_Q_LOG2SIZE_MASK;
}

/*
 * EVENTQ_BASE recompute. Mirrors `r100_smmu_update_cmdq_base`: FW
 * writes a global PA in bits[51:5] (already chiplet-base-folded
 * in q-sys's `smmu_init_queue`) plus log2size in bits[4:0]. The
 * cached `eventq_base_pa` is then ready for `cpu_physical_memory_*`
 * directly when emit lands an event.
 *
 * Why this matches cmdq_base (no chiplet_id*OFFSET add) but
 * `r100_smmu_update_strtab` does add it: the strtab path
 * pre-dates the eventq plumbing and is consistent with q-sys's
 * STRTAB_BASE write on chiplet 0 only (where the offset is 0).
 * `docs/rbdma-smmu-review.md` flags the multi-chiplet STRTAB
 * fix as a follow-on; eventq is new code so it tracks the FW
 * write directly.
 */
static void r100_smmu_update_eventq_base(R100SMMUState *s)
{
    uint64_t lo = s->regs[SMMU_EVENTQ_BASE >> 2];
    uint64_t hi = s->regs[SMMU_EVENTQ_BASE_HI >> 2];
    uint64_t val = lo | (hi << 32);

    s->eventq_base_pa = val & SMMU_Q_BASE_ADDR_MASK;
    s->eventq_log2size = val & SMMU_Q_LOG2SIZE_MASK;

    qemu_log_mask(LOG_TRACE,
                  "r100-smmu cl=%u EVENTQ updated base_pa=0x%" PRIx64
                  " log2size=%u\n",
                  s->chiplet_id, s->eventq_base_pa, s->eventq_log2size);
}

/* ------------------------------------------------------------------ */
/* Eventq + GERROR — fault delivery to FW                              */
/* ------------------------------------------------------------------ */

/*
 * Raise (toggle) one or more bits in SMMU_GERROR. FW's
 * `smmu_gerr_intr` reads `gerror ^ gerrorn` to find active errors,
 * services each, then writes gerror back to gerrorn to ack — which
 * makes them equal again so the XOR clears. Toggling on raise lines
 * up with the FW model: a fresh raise after FW ack flips the bit
 * back to "active".
 *
 * We can call this from the BQL-holding side (translate fault path,
 * cmdq walker, eventq overflow). Edge-pulse the gerr_irq SPI if
 * `IRQ_CTRL.GERROR_IRQEN=1` — FW's handler binds it as
 * `IRQ_TYPE_EDGE`.
 */
static void r100_smmu_raise_gerror(R100SMMUState *s, uint32_t bits)
{
    uint32_t old = s->regs[SMMU_GERROR >> 2];
    uint32_t ackn = s->regs[SMMU_GERRORN >> 2];

    /* Only toggle bits that are currently inactive (gerror == gerrorn
     * for that bit). Re-raising an already-active bit before FW acks
     * would flip it back to inactive — silently dropping the error. */
    bits &= ~(old ^ ackn);
    if (!bits) {
        qemu_log_mask(LOG_TRACE,
                      "r100-smmu cl=%u GERROR raise dropped — "
                      "bits already active gerror=0x%x gerrorn=0x%x\n",
                      s->chiplet_id, old, ackn);
        return;
    }

    s->regs[SMMU_GERROR >> 2] = old ^ bits;
    s->gerror_raised++;

    qemu_log_mask(LOG_TRACE,
                  "r100-smmu cl=%u GERROR raise bits=0x%x → "
                  "gerror=0x%x gerrorn=0x%x irqen=%d\n",
                  s->chiplet_id, bits, s->regs[SMMU_GERROR >> 2], ackn,
                  !!(s->regs[SMMU_IRQ_CTRL >> 2] &
                     SMMU_IRQ_CTRL_GERROR_IRQEN));

    if ((s->regs[SMMU_IRQ_CTRL >> 2] & SMMU_IRQ_CTRL_GERROR_IRQEN) &&
        s->gerr_irq) {
        qemu_irq_pulse(s->gerr_irq);
    }
}

/*
 * Build a 32-byte SMMUv3 event record and push it onto the in-DRAM
 * eventq, advance PROD with wrap encoding, and pulse evt_irq if
 * `IRQ_CTRL.EVENTQ_IRQEN=1`. Field positions track the FW reader in
 * `q/sys/drivers/smmu/smmu.c:smmu_print_event`:
 *   evt[0] bits[7:0]  = event type
 *   evt[1]            = SID
 *   evt[4..5]         = input_addr (low/high 32-bit halves)
 * The remaining dwords are zeroed; FW only inspects type / sid /
 * input_addr today.
 *
 * Drop scenarios:
 *   - eventq disabled (CR0.EVENTQEN=0) or geometry unprogrammed
 *     (eventq_base_pa == 0): drop, log, no GERROR (FW can't read
 *     the queue anyway). Increments `events_dropped`.
 *   - eventq full: drop, raise EVTQ_ABT_ERR (`smmu_gerr_intr`
 *     surfaces the abort), increment `events_dropped`. FW's spec
 *     is "events lost on a full queue".
 */
static void r100_smmu_emit_event(R100SMMUState *s, uint8_t type,
                                 uint32_t sid, hwaddr input_addr)
{
    uint32_t log2size = s->eventq_log2size;
    uint32_t idx_mask;
    uint32_t wrp_mask;
    uint32_t prod, cons, idx;
    hwaddr   entry_pa;
    uint32_t evt[8] = { 0 };
    uint32_t next_prod;

    if (!(s->regs[SMMU_CR0 >> 2] & SMMU_CR0_EVENTQEN) ||
        s->eventq_base_pa == 0 || log2size == 0 ||
        log2size > SMMU_Q_LOG2SIZE_CAP) {
        s->events_dropped++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-smmu cl=%u event dropped (eventq disabled)"
                      " type=0x%02x sid=%u input_addr=0x%" PRIx64 "\n",
                      s->chiplet_id, type, sid, (uint64_t)input_addr);
        return;
    }

    idx_mask = (1u << log2size) - 1;
    wrp_mask = 1u << log2size;
    prod = s->regs[SMMU_EVENTQ_PROD >> 2];
    cons = s->regs[SMMU_EVENTQ_CONS >> 2];

    /* Full: PROD index == CONS index but wrap differs. */
    if ((prod & idx_mask) == (cons & idx_mask) &&
        (prod & wrp_mask) != (cons & wrp_mask)) {
        s->events_dropped++;
        r100_smmu_raise_gerror(s, SMMU_GERROR_EVTQ_ABT_ERR);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-smmu cl=%u event dropped (eventq full)"
                      " type=0x%02x sid=%u input_addr=0x%" PRIx64 "\n",
                      s->chiplet_id, type, sid, (uint64_t)input_addr);
        return;
    }

    idx = prod & idx_mask;
    entry_pa = s->eventq_base_pa + (hwaddr)idx * R100_SMMU_EVTQ_BYTES;

    evt[0] = (uint32_t)type;
    evt[1] = sid;
    evt[4] = (uint32_t)(input_addr & 0xffffffffu);
    evt[5] = (uint32_t)(input_addr >> 32);

    cpu_physical_memory_write(entry_pa, evt, sizeof(evt));

    /* Advance PROD with wrap (matches q-sys's `smmu_q_inc_prod` —
     * carry from idx mask flips the wrap bit, otherwise straight
     * increment within the same wrap). */
    if (((prod & idx_mask) + 1u) & wrp_mask) {
        next_prod = (prod & ~idx_mask) ^ wrp_mask;
    } else {
        next_prod = (prod & ~idx_mask) | ((prod & idx_mask) + 1u);
    }
    s->regs[SMMU_EVENTQ_PROD >> 2] = next_prod;
    s->events_emitted++;

    qemu_log_mask(LOG_TRACE,
                  "r100-smmu cl=%u eventq emit type=0x%02x sid=%u "
                  "input_addr=0x%" PRIx64 " idx=%u entry_pa=0x%" PRIx64
                  " prod=0x%x→0x%x cons=0x%x irqen=%d\n",
                  s->chiplet_id, type, sid, (uint64_t)input_addr, idx,
                  (uint64_t)entry_pa, prod, next_prod, cons,
                  !!(s->regs[SMMU_IRQ_CTRL >> 2] &
                     SMMU_IRQ_CTRL_EVENTQ_IRQEN));

    if ((s->regs[SMMU_IRQ_CTRL >> 2] & SMMU_IRQ_CTRL_EVENTQ_IRQEN) &&
        s->evt_irq) {
        qemu_irq_pulse(s->evt_irq);
    }
}

/*
 * R100SMMUFault → FW event_id mapping. Codes match q-sys's `events[]`
 * table in `q/sys/drivers/smmu/smmu.c:165-209`, so FW's
 * `smmu_print_event` decode lines pretty-print our faults with the
 * silicon-spec name.
 */
static uint8_t r100_smmu_fault_to_event_id(R100SMMUFault fault)
{
    switch (fault) {
    case R100_SMMU_FAULT_INV_STE:        return 0x04; /* C_BAD_STE */
    case R100_SMMU_FAULT_STE_FETCH:      return 0x03; /* F_STE_FETCH */
    case R100_SMMU_FAULT_S1_NOT_IMPL:    return 0x10; /* F_TRANSLATION (best fit) */
    case R100_SMMU_FAULT_S2_TRANSLATION: return 0x10; /* F_TRANSLATION */
    case R100_SMMU_FAULT_S2_PERMISSION:  return 0x13; /* F_PERMISSION */
    case R100_SMMU_FAULT_S2_ACCESS:      return 0x12; /* F_ACCESS */
    case R100_SMMU_FAULT_S2_ADDR_SIZE:   return 0x11; /* F_ADDR_SIZE */
    case R100_SMMU_FAULT_WALK_EABT:      return 0x0b; /* F_WALK_EABT */
    case R100_SMMU_OK:
    default:                             return 0x10; /* F_TRANSLATION */
    }
}

/*
 * Recompute the cached stream-table geometry from the latched
 * STRTAB_BASE / STRTAB_BASE_CFG registers. STRTAB_BASE carries the
 * 47-bit PA in bits[51:5] (no log2size — that lives in CFG). FW
 * writes a chiplet-local PA; we add the chiplet base so the cached
 * value is a QEMU global PA ready for `address_space_*`.
 */
static void r100_smmu_update_strtab(R100SMMUState *s)
{
    uint64_t lo = s->regs[SMMU_STRTAB_BASE >> 2];
    uint64_t hi = s->regs[SMMU_STRTAB_BASE_HI >> 2];
    uint64_t base = (lo | (hi << 32)) & SMMU_Q_BASE_ADDR_MASK;
    uint32_t cfg = s->regs[SMMU_STRTAB_BASE_CFG >> 2];
    uint32_t log2size = cfg & SMMU_STRTAB_LOG2SIZE_M;
    uint32_t fmt = (cfg & SMMU_STRTAB_FMT_M) >> SMMU_STRTAB_FMT_S;

    s->strtab_base_pa  = base + (uint64_t)s->chiplet_id * R100_CHIPLET_OFFSET;
    s->strtab_log2size = MIN(log2size, SMMU_STRTAB_LOG2SIZE_CAP);
    s->strtab_fmt      = fmt;

    qemu_log_mask(LOG_TRACE,
                  "r100-smmu cl=%u STRTAB updated base_pa=0x%" PRIx64
                  " log2size=%u fmt=%u\n",
                  s->chiplet_id, s->strtab_base_pa,
                  s->strtab_log2size, s->strtab_fmt);
}

/*
 * Walk CMDQ entries in (old_cons, new_prod] and satisfy every CMD_SYNC
 * by writing the (default-zero) MSI data to the msiaddr encoded in
 * cmd[1]. Non-SYNC entries (CMD_TLBI_* / CMD_CFGI_* / PREFETCH_*) are
 * no-ops for v1, *which is correct*: `r100_smmu_translate` re-reads
 * the STE on every call and lets `smmu_ptw_64_s2` read PTEs on demand
 * — there is literally no cached state to invalidate. The FW polls
 * `CONS` to reclaim space, and we publish the new CONS at the end
 * regardless of opcode. v2 will add an STE / IOTLB cache; the
 * invalidates gain teeth there.
 *
 * `cons` / `prod` are the composite PROD/CONS registers: bits
 * [log2size-1:0] are the index, bit [log2size] is the wrap flag, and
 * higher bits are reserved. Equality of both fields means the queue
 * is empty; inequality in the wrap bit alone means the queue is full.
 * We iterate one entry at a time and stop when cons == prod.
 */
static const char *r100_smmu_cmd_str(uint64_t opcode)
{
    switch (opcode) {
    case SMMU_CMD_PREFETCH_CONFIG: return "PREFETCH_CONFIG";
    case SMMU_CMD_PREFETCH_ADDR:   return "PREFETCH_ADDR";
    case SMMU_CMD_CFGI_STE:        return "CFGI_STE";
    case SMMU_CMD_CFGI_STE_RANGE:  return "CFGI_STE_RANGE";
    case SMMU_CMD_CFGI_CD:         return "CFGI_CD";
    case SMMU_CMD_CFGI_CD_ALL:     return "CFGI_CD_ALL";
    case SMMU_CMD_TLBI_NH_ALL:     return "TLBI_NH_ALL";
    case SMMU_CMD_TLBI_NH_ASID:    return "TLBI_NH_ASID";
    case SMMU_CMD_TLBI_NH_VA:      return "TLBI_NH_VA";
    case SMMU_CMD_TLBI_NH_VAA:     return "TLBI_NH_VAA";
    case SMMU_CMD_TLBI_EL2_ALL:    return "TLBI_EL2_ALL";
    case SMMU_CMD_TLBI_S12_VMALL:  return "TLBI_S12_VMALL";
    case SMMU_CMD_TLBI_S2_IPA:     return "TLBI_S2_IPA";
    case SMMU_CMD_TLBI_NSNH_ALL:   return "TLBI_NSNH_ALL";
    case SMMU_CMD_SYNC:            return "SYNC";
    default:                       return "?";
    }
}
static void r100_smmu_process_cmdq(R100SMMUState *s, uint32_t old_cons,
                                   uint32_t new_prod)
{
    uint32_t log2size = s->cmdq_log2size;
    uint32_t idx_mask;
    uint32_t wrp_mask;
    uint32_t cons = old_cons;

    if (s->cmdq_base_pa == 0 || log2size == 0 ||
        log2size > SMMU_Q_LOG2SIZE_CAP) {
        return;
    }

    idx_mask = (1u << log2size) - 1;
    wrp_mask = 1u << log2size;

    while ((cons & (idx_mask | wrp_mask)) !=
           (new_prod & (idx_mask | wrp_mask))) {
        uint32_t idx = cons & idx_mask;
        hwaddr entry_pa = s->cmdq_base_pa +
                          (hwaddr)idx * SMMU_CMDQ_ENTRY_BYTES;
        uint64_t cmd[2];

        cpu_physical_memory_read(entry_pa, cmd, sizeof(cmd));

        {
            uint64_t opcode = cmd[0] & SMMU_CMD_OPCODE_MASK;

            qemu_log_mask(LOG_TRACE,
                          "r100-smmu cl=%u CMDQ idx=%u op=0x%02x %s "
                          "cmd[1]=0x%" PRIx64 "\n",
                          s->chiplet_id, idx, (unsigned)opcode,
                          r100_smmu_cmd_str(opcode), cmd[1]);

            if (opcode == SMMU_CMD_SYNC &&
                (cmd[0] & SMMU_SYNC_CS_MASK) == SMMU_SYNC_CS_SIG_IRQ) {
                hwaddr msi_pa = cmd[1] & SMMU_SYNC_MSIADDR_MASK;
                uint32_t zero = 0;

                cpu_physical_memory_write(msi_pa, &zero, sizeof(zero));
            }
            /* CFGI_* / TLBI_* / PREFETCH_* fall through as no-ops —
             * the v1 walker has no cache to invalidate. */
        }

        idx = (cons & idx_mask) + 1;
        if (idx & wrp_mask) {
            cons = (cons & ~idx_mask) ^ wrp_mask;
        } else {
            cons = (cons & ~idx_mask) | idx;
        }
    }

    s->regs[SMMU_CMDQ_CONS >> 2] = cons;
}

/* ------------------------------------------------------------------ */
/* Stage-2 walker — STE decode + smmu_ptw() dispatch                   */
/* ------------------------------------------------------------------ */

/*
 * S2PS encoding → effective output address bits. Arm SMMU spec
 * § "Stage 2 Configuration", Table:
 *   0b000 = 32, 0b001 = 36, 0b010 = 40, 0b011 = 42,
 *   0b100 = 44, 0b101 = 48, 0b110 = 52.
 * Anything outside 0..6 clamps to 48 (matches q-sys's `0x5 <<
 * STE2_S2PS_S = 48` programming).
 */
static uint8_t r100_smmu_s2ps_to_bits(uint64_t s2ps)
{
    static const uint8_t tbl[] = { 32, 36, 40, 42, 44, 48, 52, 48 };

    return tbl[s2ps & 0x7];
}

static uint8_t r100_smmu_s2tg_to_granule_sz(uint64_t s2tg)
{
    switch (s2tg) {
    case R100_STE2_S2TG_4KB:  return 12;
    case R100_STE2_S2TG_16KB: return 14;
    case R100_STE2_S2TG_64KB: return 16;
    default:                  return 12;
    }
}

/* Translate an `SMMUPTWErrorType` from `smmu-common.h` into the
 * R100-specific fault enum. The cardinality is 1:1 today; this
 * indirection keeps the public header decoupled from QEMU's SMMU
 * internals. */
static R100SMMUFault r100_smmu_map_ptw_err(SMMUPTWEventType type)
{
    switch (type) {
    case SMMU_PTW_ERR_WALK_EABT:   return R100_SMMU_FAULT_WALK_EABT;
    case SMMU_PTW_ERR_TRANSLATION: return R100_SMMU_FAULT_S2_TRANSLATION;
    case SMMU_PTW_ERR_ADDR_SIZE:   return R100_SMMU_FAULT_S2_ADDR_SIZE;
    case SMMU_PTW_ERR_ACCESS:      return R100_SMMU_FAULT_S2_ACCESS;
    case SMMU_PTW_ERR_PERMISSION:  return R100_SMMU_FAULT_S2_PERMISSION;
    case SMMU_PTW_ERR_NONE:
    default:                       return R100_SMMU_FAULT_S2_TRANSLATION;
    }
}

/* Read the 64-byte STE for `sid` from the in-DRAM stream table.
 * Returns false on a cache miss (sid out of range) or DMA failure.
 * On success, `ste[0..7]` mirrors q-sys's `st[sid].ste[0..7]`. */
static bool r100_smmu_read_ste(R100SMMUState *s, uint32_t sid,
                               uint64_t ste[8])
{
    hwaddr ste_pa;
    MemTxResult mr;

    if (s->strtab_base_pa == 0) {
        return false;
    }
    if (s->strtab_fmt != SMMU_STRTAB_FMT_LINEAR) {
        /* v1: 2LVL stream tables not supported. q-sys uses LINEAR
         * for the ≤32-SID R100 SoC, so this is observability-only
         * today. */
        qemu_log_mask(LOG_UNIMP,
                      "r100-smmu cl=%u: 2LVL stream table not "
                      "implemented (sid=%u)\n", s->chiplet_id, sid);
        return false;
    }
    if (s->strtab_log2size > 0 && sid >= (1u << s->strtab_log2size)) {
        return false;
    }

    ste_pa = s->strtab_base_pa + (hwaddr)sid * R100_SMMU_STE_BYTES;
    mr = address_space_read(&address_space_memory, ste_pa,
                            MEMTXATTRS_UNSPECIFIED, ste,
                            R100_SMMU_STE_BYTES);
    if (mr != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-smmu cl=%u: STE fetch failed sid=%u "
                      "ste_pa=0x%" PRIx64 " mr=%d\n",
                      s->chiplet_id, sid, (uint64_t)ste_pa, (int)mr);
        return false;
    }
    return true;
}

/* Build a stage-2 SMMUTransCfg from the cached STE words. Returns
 * false if the stage-2 fields look obviously broken (granule_sz +
 * tsz combination QEMU's `get_start_level` can't handle). */
static bool r100_smmu_build_s2cfg(R100SMMUState *s, const uint64_t ste[8],
                                  SMMUTransCfg *cfg)
{
    uint64_t ste2 = ste[2];
    uint64_t ste3 = ste[3];
    uint8_t  granule_sz =
        r100_smmu_s2tg_to_granule_sz((ste2 & R100_STE2_S2TG_M) >>
                                     R100_STE2_S2TG_S);
    uint8_t  eff_ps =
        r100_smmu_s2ps_to_bits((ste2 & R100_STE2_S2PS_M) >>
                               R100_STE2_S2PS_S);
    uint8_t  tsz = (ste2 & R100_STE2_S2T0SZ_M) >> R100_STE2_S2T0SZ_S;
    uint8_t  sl0 = (ste2 & R100_STE2_S2SL0_M) >> R100_STE2_S2SL0_S;
    int      vmid = (int)((ste2 & R100_STE2_S2VMID_M) >>
                          R100_STE2_S2VMID_S);
    bool     affd = !!(ste2 & R100_STE2_S2AFFD);
    bool     record_faults = !!(ste2 & R100_STE2_S2R);
    /* STE3 carries the S2TTB directly: q-sys writes
     * `st[func_id].ste[3] = pt_base` in `smmu_s2_enable`, where
     * pt_base is a chiplet-local PA naturally page-aligned. We
     * mask off the bottom 12 bits as belt-and-braces (page granule
     * — smmu_ptw_64_s2's `extract64(vttb, 0, eff_ps)` would tolerate
     * stragglers but it's cleaner here) and add the chiplet base so
     * the cached vttb is a QEMU global PA ready for the walker's
     * `address_space_memory` reads. */
    uint64_t vttb = (ste3 & ~0xFFFULL) +
                    (uint64_t)s->chiplet_id * R100_CHIPLET_OFFSET;

    if (tsz < 16 || tsz > 39) {
        /* Arm SMMU input range is 64 - tsz bits, must be 25..48. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-smmu cl=%u: bad S2T0SZ=%u\n",
                      s->chiplet_id, tsz);
        return false;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->stage = SMMU_STAGE_2;
    cfg->oas   = eff_ps;
    cfg->aa64  = true;
    cfg->s2cfg.tsz            = tsz;
    cfg->s2cfg.sl0            = sl0;
    cfg->s2cfg.affd           = affd;
    cfg->s2cfg.record_faults  = record_faults;
    cfg->s2cfg.granule_sz     = granule_sz;
    cfg->s2cfg.eff_ps         = eff_ps;
    cfg->s2cfg.vmid           = vmid;
    cfg->s2cfg.vttb           = vttb;
    return true;
}

const char *r100_smmu_fault_str(R100SMMUFault f)
{
    switch (f) {
    case R100_SMMU_OK:                    return "ok";
    case R100_SMMU_FAULT_INV_STE:         return "inv_ste";
    case R100_SMMU_FAULT_STE_FETCH:       return "ste_fetch";
    case R100_SMMU_FAULT_S1_NOT_IMPL:     return "s1_not_impl";
    case R100_SMMU_FAULT_S2_TRANSLATION:  return "s2_translation";
    case R100_SMMU_FAULT_S2_PERMISSION:   return "s2_permission";
    case R100_SMMU_FAULT_S2_ACCESS:       return "s2_access";
    case R100_SMMU_FAULT_S2_ADDR_SIZE:    return "s2_addr_size";
    case R100_SMMU_FAULT_WALK_EABT:       return "walk_eabt";
    }
    return "?";
}

void r100_smmu_translate(R100SMMUState *s, uint32_t sid, uint32_t ssid,
                         hwaddr dva, int access,
                         R100SMMUTranslateResult *out)
{
    uint64_t ste[8];
    uint64_t ste0;
    uint64_t cfg_field;
    SMMUTransCfg cfg;
    SMMUTLBEntry tlbe;
    SMMUPTWEventInfo info = { 0 };
    IOMMUAccessFlags perm = (access == R100_SMMU_ACCESS_WRITE)
                            ? IOMMU_WO : IOMMU_RO;
    int rc;

    out->sid = sid;
    out->fault_addr = dva;

    /* CR0.SMMUEN gate. Pre-enable: identity. This lets early-boot
     * engine accesses (before BL2 has even programmed STRTAB_BASE)
     * see DVAs as PAs, matching FW's regime where the SMMU is in
     * GBPA-bypass until enable. */
    if (!(s->regs[SMMU_CR0 >> 2] & SMMU_CR0_SMMUEN)) {
        out->ok = true;
        out->pa = dva;
        return;
    }

    if (!r100_smmu_read_ste(s, sid, ste)) {
        out->ok = false;
        out->fault = R100_SMMU_FAULT_STE_FETCH;
        r100_smmu_emit_event(s,
            r100_smmu_fault_to_event_id(out->fault), sid, dva);
        return;
    }

    ste0 = ste[0];
    if (!(ste0 & R100_STE0_VALID)) {
        out->ok = false;
        out->fault = R100_SMMU_FAULT_INV_STE;
        r100_smmu_emit_event(s,
            r100_smmu_fault_to_event_id(out->fault), sid, dva);
        return;
    }

    cfg_field = ste0 & R100_STE0_CONFIG_M;
    if (cfg_field == R100_STE0_CONFIG_ABORT) {
        out->ok = false;
        out->fault = R100_SMMU_FAULT_INV_STE;
        r100_smmu_emit_event(s,
            r100_smmu_fault_to_event_id(out->fault), sid, dva);
        return;
    }
    if (cfg_field == R100_STE0_CONFIG_BYPASS) {
        out->ok = true;
        out->pa = dva;
        return;
    }
    if (cfg_field == R100_STE0_CONFIG_S1_TRANS) {
        /* v1: stage-1 walker not implemented. q-sys leaves SIDs
         * 0..4 at S1_TRANS with `STE1.S1DSS=BYPASS`, so the
         * effective behaviour is identity anyway — but log
         * once-per-sid so v2 work has a paper trail. No event
         * emit: this is a "missing feature" path, not a fault. */
        qemu_log_mask(LOG_UNIMP,
                      "r100-smmu cl=%u: S1_TRANS sid=%u — stage-1 "
                      "walk not implemented in v1, identity\n",
                      s->chiplet_id, sid);
        out->ok = true;
        out->pa = dva;
        return;
    }
    /* S2_TRANS or ALL_TRANS — both consume STE2/STE3 stage-2 fields.
     * For ALL_TRANS, q-sys's `smmu_init_ste_bypass` sets the CD's
     * stage-1 to bypass so the IPA fed to stage-2 == the IOVA fed
     * by the master. v1 collapses both to a stage-2-only walk. */

    if (!r100_smmu_build_s2cfg(s, ste, &cfg)) {
        out->ok = false;
        out->fault = R100_SMMU_FAULT_INV_STE;
        r100_smmu_emit_event(s,
            r100_smmu_fault_to_event_id(out->fault), sid, dva);
        return;
    }

    rc = smmu_ptw(NULL, &cfg, dva, perm, &tlbe, &info);
    if (rc != 0) {
        out->ok = false;
        out->fault = r100_smmu_map_ptw_err(info.type);
        out->fault_addr = info.addr ? info.addr : dva;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-smmu cl=%u TRANSLATE FAULT sid=%u dva=0x%"
                      PRIx64 " perm=%d → %s @ 0x%" PRIx64
                      " (vttb=0x%" PRIx64 " tsz=%u sl0=%u gran=%u)\n",
                      s->chiplet_id, sid, dva, (int)perm,
                      r100_smmu_fault_str(out->fault),
                      out->fault_addr, cfg.s2cfg.vttb,
                      cfg.s2cfg.tsz, cfg.s2cfg.sl0,
                      cfg.s2cfg.granule_sz);
        r100_smmu_emit_event(s,
            r100_smmu_fault_to_event_id(out->fault), sid,
            out->fault_addr);
        return;
    }

    /* Combine the IPA's offset-within-page with the translated page
     * base. CACHED_ENTRY_TO_ADDR macro from smmu-common.h does this
     * for S1+S2 nested walks; we reproduce it here for stage-2-only. */
    out->ok = true;
    out->pa = tlbe.entry.translated_addr +
              (dva & tlbe.entry.addr_mask);
}

/* ------------------------------------------------------------------ */
/* MMIO ops                                                            */
/* ------------------------------------------------------------------ */

static uint64_t r100_smmu_read(void *opaque, hwaddr addr, unsigned size)
{
    R100SMMUState *s = R100_SMMU(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_SMMU_REG_COUNT) {
        return 0;
    }

    switch (addr) {
    case SMMU_CR0ACK:
        return s->regs[SMMU_CR0 >> 2] & SMMU_CR0_ACK_MASK;
    case SMMU_IRQ_CTRLACK:
        return s->regs[SMMU_IRQ_CTRL >> 2] & SMMU_IRQ_CTRL_ACK_MASK;
    case SMMU_STRTAB_BASE:
    case SMMU_CMDQ_BASE:
    case SMMU_EVENTQ_BASE:
        if (size == 8) {
            return (uint64_t)s->regs[reg_idx] |
                   ((uint64_t)s->regs[reg_idx + 1] << 32);
        }
        return s->regs[reg_idx];
    default:
        return s->regs[reg_idx];
    }
}

static void r100_smmu_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    R100SMMUState *s = R100_SMMU(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_SMMU_REG_COUNT) {
        return;
    }

    switch (addr) {
    case SMMU_GBPA:
        /* FW sets GBPA_UPDATE|SHCFG and polls UPDATE clear. Latch
         * UPDATE off so the poll exits immediately. */
        s->regs[reg_idx] = (uint32_t)val & ~SMMU_GBPA_UPDATE;
        break;
    case SMMU_GERRORN:
        /* FW ack: write the post-toggle GERROR value back into
         * GERRORN. After the store, gerror == gerrorn so the
         * `^=` semantics in r100_smmu_raise_gerror correctly
         * mark "no active error" until the next raise. */
        s->regs[reg_idx] = (uint32_t)val;
        qemu_log_mask(LOG_TRACE,
                      "r100-smmu cl=%u GERRORN ack=0x%x → "
                      "gerror=0x%x active=0x%x\n",
                      s->chiplet_id, (uint32_t)val,
                      s->regs[SMMU_GERROR >> 2],
                      s->regs[SMMU_GERROR >> 2] ^ (uint32_t)val);
        break;
    case SMMU_STRTAB_BASE:
        if (size == 8) {
            s->regs[SMMU_STRTAB_BASE >> 2]    = (uint32_t)val;
            s->regs[SMMU_STRTAB_BASE_HI >> 2] = (uint32_t)(val >> 32);
        } else {
            s->regs[reg_idx] = (uint32_t)val;
        }
        r100_smmu_update_strtab(s);
        break;
    case SMMU_STRTAB_BASE_HI:
        s->regs[reg_idx] = (uint32_t)val;
        r100_smmu_update_strtab(s);
        break;
    case SMMU_STRTAB_BASE_CFG:
        s->regs[reg_idx] = (uint32_t)val;
        r100_smmu_update_strtab(s);
        break;
    case SMMU_CMDQ_BASE:
        if (size == 8) {
            s->regs[SMMU_CMDQ_BASE >> 2]    = (uint32_t)val;
            s->regs[SMMU_CMDQ_BASE_HI >> 2] = (uint32_t)(val >> 32);
        } else {
            s->regs[reg_idx] = (uint32_t)val;
        }
        r100_smmu_update_cmdq_base(s);
        break;
    case SMMU_CMDQ_BASE_HI:
        s->regs[reg_idx] = (uint32_t)val;
        r100_smmu_update_cmdq_base(s);
        break;
    case SMMU_CMDQ_PROD: {
        uint32_t old_cons = s->regs[SMMU_CMDQ_CONS >> 2];
        uint32_t new_prod = (uint32_t)val;

        s->regs[SMMU_CMDQ_PROD >> 2] = new_prod;
        r100_smmu_process_cmdq(s, old_cons, new_prod);
        break;
    }
    case SMMU_EVENTQ_BASE:
        if (size == 8) {
            s->regs[SMMU_EVENTQ_BASE >> 2]    = (uint32_t)val;
            s->regs[SMMU_EVENTQ_BASE_HI >> 2] = (uint32_t)(val >> 32);
        } else {
            s->regs[reg_idx] = (uint32_t)val;
        }
        r100_smmu_update_eventq_base(s);
        break;
    case SMMU_EVENTQ_BASE_HI:
        s->regs[reg_idx] = (uint32_t)val;
        r100_smmu_update_eventq_base(s);
        break;
    default:
        s->regs[reg_idx] = (uint32_t)val;
        break;
    }
}

static const MemoryRegionOps r100_smmu_ops = {
    .read = r100_smmu_read,
    .write = r100_smmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
};

/* ------------------------------------------------------------------ */
/* Realize / reset / properties                                        */
/* ------------------------------------------------------------------ */

static void r100_smmu_realize(DeviceState *dev, Error **errp)
{
    R100SMMUState *s = R100_SMMU(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-smmu.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_smmu_ops, s, name,
                          R100_SMMU_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    /* Two IRQ outputs in the order r100_soc.c connects them:
     *   IRQ 0 → GIC SPI 762 (SMMU_EVT_IRQ, q-sys's smmu_event_intr).
     *   IRQ 1 → GIC SPI 765 (SMMU_GERR_IRQ, q-sys's smmu_gerr_intr).
     * Both edge-triggered per FW's `IRQ_TYPE_EDGE` connect call,
     * so emit / raise paths use `qemu_irq_pulse` (raise+lower in one
     * BQL window). */
    sysbus_init_irq(sbd, &s->evt_irq);
    sysbus_init_irq(sbd, &s->gerr_irq);
}

static void r100_smmu_reset(DeviceState *dev)
{
    R100SMMUState *s = R100_SMMU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->cmdq_base_pa = 0;
    s->cmdq_log2size = 0;
    s->eventq_base_pa = 0;
    s->eventq_log2size = 0;
    s->strtab_base_pa = 0;
    s->strtab_log2size = 0;
    s->strtab_fmt = SMMU_STRTAB_FMT_LINEAR;
    /* events_emitted / events_dropped / gerror_raised survive reset
     * — handy for "did anything happen during this run?" inspection
     * via HMP `info qtree` / qmp `qom-get` after a regression. */
}

static Property r100_smmu_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100SMMUState, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_smmu_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_smmu_realize;
    device_class_set_legacy_reset(dc, r100_smmu_reset);
    device_class_set_props(dc, r100_smmu_properties);
}

static const TypeInfo r100_smmu_info = {
    .name = TYPE_R100_SMMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100SMMUState),
    .class_init = r100_smmu_class_init,
};

static void r100_smmu_register_types(void)
{
    type_register_static(&r100_smmu_info);
}

type_init(r100_smmu_register_types)
