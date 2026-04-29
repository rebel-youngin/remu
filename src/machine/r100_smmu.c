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
 *             - S1_TRANS → real stage-1 walk (P10):
 *                 * Read STE1.S1DSS to gate stage-1.
 *                   q-sys's `smmu_init_ste` (SIDs 0..4) leaves
 *                   `S1DSS=BYPASS` so stage-1 is skipped and the
 *                   IOVA is passed through unchanged (effective
 *                   identity for these SIDs, because their
 *                   config doesn't include S2_TRANS yet — that
 *                   gets ORed in later by `smmu_s2_enable`).
 *                   `smmu_init_ste_bypass` (SIDs 8..12) sets
 *                   `S1DSS=SUBSTREAM0` so stage-1 walks
 *                   `CD[0]` from `STE0.S1ContextPtr`.
 *                 * Read CD[0] (64 B / 8 dwords; matches QEMU's
 *                   `CD` struct in `smmuv3-internal.h`) and
 *                   decode TT0 / TT1 fields. q-sys's
 *                   `smmu_init_cd_bypass` programs T0SZ=20,
 *                   TG0=4 KB, IPS=40, EPD1=1 (TT1 disabled),
 *                   AA64=1, AFFD=1, R=1, ASET=1, VALID=1 with
 *                   TTB0 = `SMMU_BYPASS_PT + CHIPLET_BASE_ADDR`
 *                   — a stage-1 PT (`smmu_create_bypass_table`)
 *                   whose HTID0 entry identity-maps VA
 *                   `0x40000000..0x8000000000` for the local
 *                   chiplet, HTID1..15 map the same VA window
 *                   to remote chiplets at `c*0x10000000000 +
 *                   0x40000000`. We hand the populated cfg to
 *                   QEMU's `smmu_ptw_64_s1`.
 *               No more "v1 collapses S1_TRANS to identity"
 *               shortcut. SSID != 0 stays unimp (SS-SS lookup is
 *               v2).
 *             - S2_TRANS / ALL_TRANS → build SMMUTransCfg from
 *               STE2/STE3 and dispatch to QEMU's `smmu_ptw()` with
 *               stage=SMMU_STAGE_2. The walker reads PTEs through
 *               `address_space_memory`, so STE3 (S2TTB) is converted
 *               to a global PA (chiplet-local + chiplet_id *
 *               R100_CHIPLET_OFFSET) before we hand it to the cfg.
 *               For ALL_TRANS the FW always pairs S2_TRANS with
 *               STE1.S1DSS=BYPASS (`smmu_init_ste`'s SIDs 0..4),
 *               so stage-1 is skipped and the IOVA == IPA fed to
 *               stage-2 — v1 collapses that to a stage-2-only
 *               walk (no nested decode). v2 honours S1DSS for
 *               ALL_TRANS too.
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
#include "hw/qdev-properties-system.h"  /* DEFINE_PROP_CHR */
#include "chardev/char-fe.h"
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

    /* Translate / CMDQ counters survive reset too. They make a single
     * `info qtree` enough to answer "did the SMMU do anything during
     * this run?" without having to grep the smmu-debug tail. */
    uint64_t translates_total;
    uint64_t translates_bypass; /* CR0.SMMUEN=0 or STE BYPASS / S1_TRANS */
    uint64_t translates_ok;     /* stage-2 walked, no fault */
    uint64_t translates_fault;
    uint64_t cmdq_processed;    /* entries we actually walked past */

    /* Optional debug-tail chardev. Mirrors the rbdma_debug_chr +
     * hdma_debug_chr pattern: when wired, every interesting SMMU
     * event (translate entry/exit, STE decode, PT-walk dispatch, CR0
     * / STRTAB / EVENTQ / CMDQ programming, CMDQ command, eventq
     * emit, GERROR raise) emits a single ASCII line to it. Always-on
     * once the chardev is wired (no -d / --trace dependency); empty
     * when not wired so single-QEMU NPU smoke runs pay nothing. The
     * stream is bounded by the number of translates a workload
     * issues, so a typical P10 cb pair (3-4 LL elements) produces
     * ~30 lines — fine to commit to a file chardev. */
    CharBackend smmu_debug_chr;
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

/* STE0 stage-1 fields (`q-sys/.../smmu.h:321-329`). The CD pointer
 * `S1ContextPtr` is the chiplet-local PA of the CD table; the FW
 * masks it by `CHIPLET_OFFSET - 1` before writing, so we need to
 * add `chiplet_id * R100_CHIPLET_OFFSET` to recover the global PA
 * for `address_space_*`. */
#define R100_STE0_S1CONTEXTPTR_S       6
#define R100_STE0_S1CONTEXTPTR_M       (0x3FFFFFFFFFFFULL << \
                                        R100_STE0_S1CONTEXTPTR_S)

/* STE1.S1DSS — stage-1 default substream behaviour (`q-sys/.../
 * smmu.h:331-335`). FW programs SUBSTREAM0 on the bypass STEs
 * (smmu_init_ste_bypass; we walk CD[0]) and BYPASS on the regular
 * STEs (smmu_init_ste; we skip stage-1 → IOVA pass-through). */
#define R100_STE1_S1DSS_S              0
#define R100_STE1_S1DSS_M              (0x3ULL << R100_STE1_S1DSS_S)
#define R100_STE1_S1DSS_TERMINATE      (0x0ULL << R100_STE1_S1DSS_S)
#define R100_STE1_S1DSS_BYPASS         (0x1ULL << R100_STE1_S1DSS_S)
#define R100_STE1_S1DSS_SUBSTREAM0     (0x2ULL << R100_STE1_S1DSS_S)

/* CD field decode. SMMUv3 CD is 64 B / 8 dwords / 16 words; the FW
 * uses `struct context_desc { uint64_t val[6]; uint64_t rsvd[2]; }`
 * with the same layout. r100_smmu reads the 64 bytes into a `union`
 * with both `uint32_t word[16]` (matches QEMU's `CD` struct in
 * `smmuv3-internal.h`) and `uint64_t val[8]` (matches FW's struct).
 *
 * Layout (per Arm SMMUv3 spec § 5.3 + cross-checked vs q-sys's
 * `CD0_*` macros and QEMU's `CD_*` macros):
 *
 *   word[0]  bits[5:0]   T0SZ
 *            bits[7:6]   TG0
 *            bit[14]     EPD0
 *            bit[15]     ENDI
 *            bits[21:16] T1SZ
 *            bits[23:22] TG1
 *            bit[30]     EPD1
 *            bit[31]     VALID
 *   word[1]  bits[2:0]   IPS
 *            bit[3]      AFFD
 *            bits[7:6]   TBI
 *            bit[9]      AARCH64
 *            bit[10]     HD (must be 0)
 *            bit[11]     HA (must be 0)
 *            bit[12]     S
 *            bit[13]     R
 *            bit[14]     A
 *            bits[31:16] ASID
 *   word[2]  TTB0[31:4] (bits[3:0] are MIR/HAD/CnP)
 *   word[3]  bits[18:0]  TTB0[51:32]
 *   word[4]  TTB1[31:4]
 *   word[5]  bits[18:0]  TTB1[51:32]
 *   word[6]  MAIR
 *   word[7]  reserved / etc
 *
 * Macros mirror QEMU's `CD_*` style so the decoder reads naturally
 * if a future contributor cross-references `smmuv3-internal.h`. */
#define R100_CD_VALID(w)        (((w)[0] >> 31) & 0x1U)
#define R100_CD_AARCH64(w)      (((w)[1] >>  9) & 0x1U)
#define R100_CD_S(w)            (((w)[1] >> 12) & 0x1U)
#define R100_CD_HA(w)           (((w)[1] >> 11) & 0x1U)
#define R100_CD_HD(w)           (((w)[1] >> 10) & 0x1U)
#define R100_CD_A(w)            (((w)[1] >> 14) & 0x1U)
#define R100_CD_R(w)            (((w)[1] >> 13) & 0x1U)
#define R100_CD_AFFD(w)         (((w)[1] >>  3) & 0x1U)
#define R100_CD_TBI(w)          (((w)[1] >>  6) & 0x3U)
#define R100_CD_IPS(w)          (((w)[1] >>  0) & 0x7U)
#define R100_CD_ASID(w)         (((w)[1] >> 16) & 0xFFFFU)
#define R100_CD_ENDI(w)         (((w)[0] >> 15) & 0x1U)
#define R100_CD_TSZ(w, sel)     (((w)[0] >> (16 * (sel) +  0)) & 0x3FU)
#define R100_CD_TG(w, sel)      (((w)[0] >> (16 * (sel) +  6)) & 0x3U)
#define R100_CD_EPD(w, sel)     (((w)[0] >> (16 * (sel) + 14)) & 0x1U)
#define R100_CD_TTB(w, sel)                                       \
    ((((uint64_t)((w)[(sel) * 2 + 3] & 0x7FFFFU)) << 32) |        \
     ((uint64_t)((w)[(sel) * 2 + 2] & ~0xFU)))

/* CD.IPS → effective output address bits. Same table as
 * `smmu_ptw_64_s2`'s S2PS field (and oas2bits in
 * smmuv3-internal.h). */
static inline uint8_t r100_cd_ips_to_bits(uint8_t ips)
{
    static const uint8_t tbl[] = { 32, 36, 40, 42, 44, 48, 48, 48 };

    return tbl[ips & 0x7];
}

/* CD.TGx → granule shift (matches QEMU's `tg2granule`). The FW
 * uses 4 KB on both TT0 (CD0_TG0_4KB) and TT1; non-4 KB granules
 * happen on TF-A's RoT init paths only and aren't on the engine
 * translate path. */
static inline uint8_t r100_cd_tg_to_granule_sz(uint8_t tg, int sel)
{
    switch (tg) {
    case 0:  return sel ? 0  : 12;
    case 1:  return sel ? 14 : 16;
    case 2:  return sel ? 12 : 14;
    case 3:  return sel ? 16 :  0;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Debug-tail helper                                                   */
/* ------------------------------------------------------------------ */

/*
 * One ASCII line per significant SMMU event when the `debug-chardev`
 * property is wired. Same shape as rbdma's `r100_rbdma_emit_debug` /
 * hdma's `r100_hdma_emit_trace` — bounded throughput (one line per
 * translate / STE decode / PT-walk / queue op / fault), no `-d` /
 * `--trace` dependency, silent when the backend isn't connected.
 *
 * The traced events are deliberately the same set the existing
 * `qemu_log_mask(LOG_TRACE, ...)` paths cover, so users who already
 * have a `--trace`-style workflow get nothing new but a *separate*
 * stream pointed at one device. Anything that LOG_GUEST_ERRORs (fault
 * fallthrough, eventq overflow drop) ALSO emits here so the debug
 * tail is self-contained.
 *
 * Format:
 *   smmu cl=<id> <event> <key=value>...
 * matching the rbdma / hdma tails so a single grep across all
 * `*.log`s under `output/<run>/` finds related events on the same
 * BD lifecycle.
 */
static void r100_smmu_emit_debug(R100SMMUState *s, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

static void r100_smmu_emit_debug(R100SMMUState *s, const char *fmt, ...)
{
    char line[256];
    int n;
    va_list ap;

    if (!qemu_chr_fe_backend_connected(&s->smmu_debug_chr)) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        qemu_chr_fe_write(&s->smmu_debug_chr, (const uint8_t *)line,
                          MIN((size_t)n, sizeof(line) - 1));
    }
}

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
    r100_smmu_emit_debug(s,
                         "smmu cl=%u CMDQ_BASE base_pa=0x%" PRIx64
                         " log2size=%u\n",
                         s->chiplet_id, s->cmdq_base_pa,
                         s->cmdq_log2size);
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
    r100_smmu_emit_debug(s,
                         "smmu cl=%u EVENTQ_BASE base_pa=0x%" PRIx64
                         " log2size=%u\n",
                         s->chiplet_id, s->eventq_base_pa,
                         s->eventq_log2size);
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
    r100_smmu_emit_debug(s,
                         "smmu cl=%u GERROR raise bits=0x%x"
                         " gerror=0x%x gerrorn=0x%x irqen=%d total=%"
                         PRIu64 "\n",
                         s->chiplet_id, bits,
                         s->regs[SMMU_GERROR >> 2], ackn,
                         !!(s->regs[SMMU_IRQ_CTRL >> 2] &
                            SMMU_IRQ_CTRL_GERROR_IRQEN),
                         s->gerror_raised);

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
        r100_smmu_emit_debug(s,
                             "smmu cl=%u eventq_drop reason=disabled"
                             " type=0x%02x sid=%u input_addr=0x%"
                             PRIx64 " dropped_total=%" PRIu64 "\n",
                             s->chiplet_id, type, sid,
                             (uint64_t)input_addr, s->events_dropped);
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
        r100_smmu_emit_debug(s,
                             "smmu cl=%u eventq_drop reason=full"
                             " type=0x%02x sid=%u input_addr=0x%"
                             PRIx64 " prod=0x%x cons=0x%x"
                             " dropped_total=%" PRIu64 "\n",
                             s->chiplet_id, type, sid,
                             (uint64_t)input_addr, prod, cons,
                             s->events_dropped);
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
    r100_smmu_emit_debug(s,
                         "smmu cl=%u eventq_emit type=0x%02x sid=%u"
                         " input_addr=0x%" PRIx64 " idx=%u"
                         " entry_pa=0x%" PRIx64 " prod=0x%x→0x%x"
                         " cons=0x%x irqen=%d emitted_total=%" PRIu64
                         "\n",
                         s->chiplet_id, type, sid,
                         (uint64_t)input_addr, idx,
                         (uint64_t)entry_pa, prod, next_prod, cons,
                         !!(s->regs[SMMU_IRQ_CTRL >> 2] &
                            SMMU_IRQ_CTRL_EVENTQ_IRQEN),
                         s->events_emitted);

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
    r100_smmu_emit_debug(s,
                         "smmu cl=%u STRTAB_BASE base_pa=0x%" PRIx64
                         " log2size=%u fmt=%s n_sids=%u\n",
                         s->chiplet_id, s->strtab_base_pa,
                         s->strtab_log2size,
                         s->strtab_fmt == SMMU_STRTAB_FMT_LINEAR
                            ? "LINEAR" : "2LVL",
                         s->strtab_log2size > 0
                            ? (1u << s->strtab_log2size) : 0u);
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
            r100_smmu_emit_debug(s,
                                 "smmu cl=%u cmdq idx=%u op=0x%02x %s"
                                 " cmd[0]=0x%" PRIx64 " cmd[1]=0x%"
                                 PRIx64 "\n",
                                 s->chiplet_id, idx, (unsigned)opcode,
                                 r100_smmu_cmd_str(opcode), cmd[0],
                                 cmd[1]);

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
        s->cmdq_processed++;
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
 * internals. The `S2_*` enum names date from v1's stage-2-only
 * walker; the same enum doubles for stage-1 faults post-P10
 * (no point in proliferating S1_TRANSLATION / S1_PERMISSION / …
 * variants that the FW eventq consumer can't tell apart anyway —
 * SMMUv3 events report the fault type through `event_id`, which
 * is identical for S1 and S2 translation faults). */
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

/*
 * Read the 64-byte CD at @cd_chiplet_local_pa (the chiplet-local
 * S1ContextPtr extracted from STE0). We add `chiplet_id *
 * R100_CHIPLET_OFFSET` here so the walker reads the right CD on
 * non-zero chiplets. `cd_words` is a 16-element u32 array matching
 * QEMU's `CD` struct layout (and the FW's `struct context_desc`
 * binary layout) — see banner comment on R100_CD_* macros above.
 */
static bool r100_smmu_read_cd(R100SMMUState *s, hwaddr cd_chiplet_local_pa,
                              uint32_t cd_words[16])
{
    hwaddr cd_pa;
    MemTxResult mr;

    cd_pa = cd_chiplet_local_pa +
            (uint64_t)s->chiplet_id * R100_CHIPLET_OFFSET;
    mr = address_space_read(&address_space_memory, cd_pa,
                            MEMTXATTRS_UNSPECIFIED, cd_words, 64);
    if (mr != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-smmu cl=%u: CD fetch failed cd_pa=0x%"
                      PRIx64 " mr=%d\n",
                      s->chiplet_id, (uint64_t)cd_pa, (int)mr);
        return false;
    }
    return true;
}

/*
 * Build a stage-1 SMMUTransCfg from a freshly-read CD. Returns
 * false on any "C_BAD_CD" condition QEMU's `decode_cd` rejects —
 * matches Arm SMMUv3 spec § 5.3 (V=1 / AA64=1 / A=1 / S=0 / HA=0
 * / HD=0 / per-TT TSZ in [16,39] / per-TT granule ∈ {12,14,16}).
 *
 * Both TT0 and TT1 are populated; `select_tt(cfg, iova)` in
 * `smmu_ptw_64_s1` picks the right one based on the high bits of
 * the input IOVA. Disabled TTs (CD.EPD{0,1}=1) are flagged so
 * `select_tt` skips them — q-sys's `smmu_init_cd_bypass` sets
 * EPD1=1 because the bypass PT only covers the low VA half (TT0
 * range = `0..2^(64-T0SZ)`).
 *
 * On success, `cfg->tt[i].ttb` holds a *global* PA — the FW writes
 * a chiplet-local PA into the CD (`SMMU_BYPASS_PT +
 * CHIPLET_BASE_ADDR`, which is already global, but smmu_init_cd
 * stores 0 for TT0 in the PF user-CD path), so we add `chiplet_id
 * * CHIPLET_OFFSET` only when TTB looks chiplet-local (top 12
 * bits zero). The FW's bypass init writes a global TTB so this is
 * a no-op in the umd path; the conversion is here for forward
 * compatibility with `smmu_activate_ctx` (which writes a different
 * pt_base for q-cp's per-context address spaces).
 */
static bool r100_smmu_build_s1cfg(R100SMMUState *s,
                                  const uint32_t cd[16],
                                  SMMUTransCfg *cfg)
{
    uint8_t  oas;
    int      i;

    /* Sanity-check the CD. We deliberately *don't* require CD.A=1
     * (TERM_MODEL=1 enforcement in QEMU's strict path) because q-sys's
     * `smmu_init_cd_bypass` leaves CD_A=0 (the line `val |= CD0_A;`
     * is commented out in `drivers/smmu/smmu.c`); real SMMU-600
     * silicon doesn't fault that bypass CD, and we'd defeat the
     * whole "make the FW SMMU init's impact real" point if we
     * rejected the FW's own programming. CD.S / CD.HA / CD.HD must
     * still be 0 — those gate features (stalls, hardware AF/DBM
     * updates) the v1 walker doesn't model, and the FW programs them
     * 0 anyway. */
    if (!R100_CD_VALID(cd) || !R100_CD_AARCH64(cd) ||
        R100_CD_S(cd) || R100_CD_HA(cd) || R100_CD_HD(cd)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-smmu cl=%u: bad CD val[0]=0x%x val[1]=0x%x"
                      " (V=%u AA64=%u S=%u HA=%u HD=%u)\n",
                      s->chiplet_id, cd[0], cd[1],
                      R100_CD_VALID(cd), R100_CD_AARCH64(cd),
                      R100_CD_S(cd), R100_CD_HA(cd), R100_CD_HD(cd));
        return false;
    }

    oas = r100_cd_ips_to_bits(R100_CD_IPS(cd));

    memset(cfg, 0, sizeof(*cfg));
    cfg->stage         = SMMU_STAGE_1;
    cfg->aa64          = true;
    cfg->oas           = oas;
    cfg->tbi           = R100_CD_TBI(cd);
    cfg->asid          = R100_CD_ASID(cd);
    cfg->affd          = R100_CD_AFFD(cd);
    cfg->record_faults = R100_CD_R(cd);
    cfg->s2cfg.vmid    = -1;

    for (i = 0; i < 2; i++) {
        SMMUTransTableInfo *tt = &cfg->tt[i];
        uint8_t tsz = R100_CD_TSZ(cd, i);
        uint8_t tg  = R100_CD_TG(cd, i);
        uint8_t gran;
        uint64_t ttb;

        tt->disabled = R100_CD_EPD(cd, i);
        if (tt->disabled) {
            continue;
        }

        if (tsz < 16 || tsz > 39 || R100_CD_ENDI(cd)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-smmu cl=%u: bad CD TT%d tsz=%u endi=%u\n",
                          s->chiplet_id, i, tsz, R100_CD_ENDI(cd));
            return false;
        }
        gran = r100_cd_tg_to_granule_sz(tg, i);
        if (gran != 12 && gran != 14 && gran != 16) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-smmu cl=%u: bad CD TT%d granule"
                          " (tg=%u → gran=%u)\n",
                          s->chiplet_id, i, tg, gran);
            return false;
        }
        if (gran != 16) {
            cfg->oas = MIN(cfg->oas, 48);
        }
        tt->granule_sz = gran;
        tt->tsz        = tsz;
        ttb            = R100_CD_TTB(cd, i);
        if (ttb && (ttb >> 36) == 0) {
            ttb += (uint64_t)s->chiplet_id * R100_CHIPLET_OFFSET;
        }
        tt->ttb        = ttb;
        tt->had        = false;
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

/* Pretty-name STE0.config for the debug tail. */
static const char *r100_smmu_ste_config_str(uint64_t cfg_field)
{
    switch (cfg_field) {
    case R100_STE0_CONFIG_ABORT:     return "ABORT";
    case R100_STE0_CONFIG_BYPASS:    return "BYPASS";
    case R100_STE0_CONFIG_S1_TRANS:  return "S1_TRANS";
    case R100_STE0_CONFIG_S2_TRANS:  return "S2_TRANS";
    case R100_STE0_CONFIG_ALL_TRANS: return "ALL_TRANS";
    default:                         return "?";
    }
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
    const char *acc_str = (access == R100_SMMU_ACCESS_WRITE) ? "wr" : "rd";

    out->sid = sid;
    out->fault_addr = dva;
    s->translates_total++;

    r100_smmu_emit_debug(s,
                         "smmu cl=%u xlate_in sid=%u ssid=%u dva=0x%"
                         PRIx64 " %s cr0_smmuen=%d strtab_base_pa=0x%"
                         PRIx64 "\n",
                         s->chiplet_id, sid, ssid, (uint64_t)dva,
                         acc_str,
                         !!(s->regs[SMMU_CR0 >> 2] & SMMU_CR0_SMMUEN),
                         s->strtab_base_pa);

    /* CR0.SMMUEN gate. Pre-enable: identity. This lets early-boot
     * engine accesses (before BL2 has even programmed STRTAB_BASE)
     * see DVAs as PAs, matching FW's regime where the SMMU is in
     * GBPA-bypass until enable. */
    if (!(s->regs[SMMU_CR0 >> 2] & SMMU_CR0_SMMUEN)) {
        out->ok = true;
        out->pa = dva;
        s->translates_bypass++;
        r100_smmu_emit_debug(s,
                             "smmu cl=%u xlate_out sid=%u dva=0x%" PRIx64
                             " bypass=identity_smmuen0 pa=0x%" PRIx64 "\n",
                             s->chiplet_id, sid, (uint64_t)dva,
                             (uint64_t)dva);
        return;
    }

    if (!r100_smmu_read_ste(s, sid, ste)) {
        out->ok = false;
        out->fault = R100_SMMU_FAULT_STE_FETCH;
        s->translates_fault++;
        r100_smmu_emit_debug(s,
                             "smmu cl=%u xlate_out sid=%u dva=0x%" PRIx64
                             " FAULT ste_fetch strtab_base_pa=0x%" PRIx64
                             " ste_pa=0x%" PRIx64 "\n",
                             s->chiplet_id, sid, (uint64_t)dva,
                             s->strtab_base_pa,
                             s->strtab_base_pa +
                                (uint64_t)sid * R100_SMMU_STE_BYTES);
        r100_smmu_emit_event(s,
            r100_smmu_fault_to_event_id(out->fault), sid, dva);
        return;
    }

    ste0 = ste[0];
    cfg_field = ste0 & R100_STE0_CONFIG_M;

    /* One line for the decoded STE — captures every interesting
     * stage-2 input (V, config, S2T0SZ, S2SL0, S2TG, S2PS, S2AA64,
     * S2AFFD, S2R, S2VMID, S2TTB) so the smmu-debug tail has the
     * decode every translate would otherwise bury in another tool. */
    {
        uint64_t ste2 = ste[2];
        uint64_t ste3 = ste[3];
        uint8_t  tsz = (ste2 & R100_STE2_S2T0SZ_M) >> R100_STE2_S2T0SZ_S;
        uint8_t  sl0 = (ste2 & R100_STE2_S2SL0_M) >> R100_STE2_S2SL0_S;
        uint8_t  tg  = (ste2 & R100_STE2_S2TG_M)  >> R100_STE2_S2TG_S;
        uint8_t  ps  = (ste2 & R100_STE2_S2PS_M)  >> R100_STE2_S2PS_S;
        int      vmid = (int)((ste2 & R100_STE2_S2VMID_M) >>
                              R100_STE2_S2VMID_S);

        r100_smmu_emit_debug(s,
                             "smmu cl=%u ste sid=%u v=%d cfg=%s"
                             " s2t0sz=%u s2sl0=%u s2tg=%u s2ps=%u"
                             " s2aa64=%d s2affd=%d s2r=%d vmid=%d"
                             " s2ttb=0x%" PRIx64 "\n",
                             s->chiplet_id, sid,
                             !!(ste0 & R100_STE0_VALID),
                             r100_smmu_ste_config_str(cfg_field),
                             tsz, sl0, tg, ps,
                             !!(ste2 & R100_STE2_S2AA64),
                             !!(ste2 & R100_STE2_S2AFFD),
                             !!(ste2 & R100_STE2_S2R),
                             vmid, (uint64_t)(ste3 & ~0xFFFULL));
    }

    if (!(ste0 & R100_STE0_VALID)) {
        out->ok = false;
        out->fault = R100_SMMU_FAULT_INV_STE;
        s->translates_fault++;
        r100_smmu_emit_debug(s,
                             "smmu cl=%u xlate_out sid=%u dva=0x%" PRIx64
                             " FAULT inv_ste reason=v0\n",
                             s->chiplet_id, sid, (uint64_t)dva);
        r100_smmu_emit_event(s,
            r100_smmu_fault_to_event_id(out->fault), sid, dva);
        return;
    }

    if (cfg_field == R100_STE0_CONFIG_ABORT) {
        out->ok = false;
        out->fault = R100_SMMU_FAULT_INV_STE;
        s->translates_fault++;
        r100_smmu_emit_debug(s,
                             "smmu cl=%u xlate_out sid=%u dva=0x%" PRIx64
                             " FAULT inv_ste reason=abort\n",
                             s->chiplet_id, sid, (uint64_t)dva);
        r100_smmu_emit_event(s,
            r100_smmu_fault_to_event_id(out->fault), sid, dva);
        return;
    }
    if (cfg_field == R100_STE0_CONFIG_BYPASS) {
        out->ok = true;
        out->pa = dva;
        s->translates_bypass++;
        r100_smmu_emit_debug(s,
                             "smmu cl=%u xlate_out sid=%u dva=0x%" PRIx64
                             " bypass=ste_bypass pa=0x%" PRIx64 "\n",
                             s->chiplet_id, sid, (uint64_t)dva,
                             (uint64_t)dva);
        return;
    }
    if (cfg_field == R100_STE0_CONFIG_S1_TRANS) {
        /* P10: real stage-1 walk. Dispatch by STE1.S1DSS:
         *
         *   - SUBSTREAM0: walk CD[0] from STE0.S1ContextPtr. The
         *     FW's `smmu_init_ste_bypass` programs this for the
         *     bypass SIDs (8..12), with a CD pointing at
         *     SMMU_BYPASS_PT.
         *   - BYPASS: stage-1 disabled → IOVA pass-through. The
         *     FW's `smmu_init_ste` programs this for the regular
         *     SIDs (0..4) so engines see identity until
         *     `smmu_s2_enable` ORs in S2_TRANS.
         *   - TERMINATE: any non-substream0 transaction faults.
         *     We don't see this on a translate (engines don't
         *     issue substream IDs to v1 r100_hdma / r100_rbdma),
         *     so log + identity for safety. v2 will fault.
         *
         * SSID != 0 stays unimp (substream-aware lookup). The
         * `r100_hdma_translate` caller passes ssid=0 today; if a
         * future caller passes a real SSID we LOG_UNIMP and walk
         * CD[0] anyway (the bypass PT is the same for all
         * substreams). */
        uint64_t ste1 = ste[1];
        uint64_t s1dss = ste1 & R100_STE1_S1DSS_M;
        uint64_t s1ctxptr = (ste0 & R100_STE0_S1CONTEXTPTR_M);
        uint32_t cd[16];
        const char *dss_str =
            (s1dss == R100_STE1_S1DSS_TERMINATE) ? "TERMINATE" :
            (s1dss == R100_STE1_S1DSS_BYPASS)    ? "BYPASS" :
            (s1dss == R100_STE1_S1DSS_SUBSTREAM0)? "SUBSTREAM0" :
                                                   "RES";

        r100_smmu_emit_debug(s,
                             "smmu cl=%u s1_dispatch sid=%u s1dss=%s"
                             " s1ctxptr=0x%" PRIx64 " ssid=%u\n",
                             s->chiplet_id, sid, dss_str,
                             (uint64_t)s1ctxptr, ssid);

        if (s1dss == R100_STE1_S1DSS_BYPASS) {
            out->ok = true;
            out->pa = dva;
            s->translates_bypass++;
            r100_smmu_emit_debug(s,
                                 "smmu cl=%u xlate_out sid=%u dva=0x%"
                                 PRIx64 " bypass=s1dss_bypass pa=0x%"
                                 PRIx64 "\n",
                                 s->chiplet_id, sid, (uint64_t)dva,
                                 (uint64_t)dva);
            return;
        }
        if (s1dss == R100_STE1_S1DSS_TERMINATE) {
            qemu_log_mask(LOG_UNIMP,
                          "r100-smmu cl=%u: S1DSS=TERMINATE sid=%u "
                          "ssid=%u — v1 falls through to identity\n",
                          s->chiplet_id, sid, ssid);
            out->ok = true;
            out->pa = dva;
            s->translates_bypass++;
            r100_smmu_emit_debug(s,
                                 "smmu cl=%u xlate_out sid=%u dva=0x%"
                                 PRIx64 " bypass=s1dss_term_v1 pa=0x%"
                                 PRIx64 "\n",
                                 s->chiplet_id, sid, (uint64_t)dva,
                                 (uint64_t)dva);
            return;
        }
        if (ssid != 0) {
            qemu_log_mask(LOG_UNIMP,
                          "r100-smmu cl=%u: substream lookup sid=%u "
                          "ssid=%u — v1 walks CD[0]\n",
                          s->chiplet_id, sid, ssid);
        }

        if (!r100_smmu_read_cd(s, s1ctxptr, cd)) {
            out->ok = false;
            out->fault = R100_SMMU_FAULT_STE_FETCH;
            s->translates_fault++;
            r100_smmu_emit_debug(s,
                                 "smmu cl=%u xlate_out sid=%u dva=0x%"
                                 PRIx64 " FAULT cd_fetch s1ctxptr=0x%"
                                 PRIx64 "\n",
                                 s->chiplet_id, sid, (uint64_t)dva,
                                 (uint64_t)s1ctxptr);
            r100_smmu_emit_event(s,
                r100_smmu_fault_to_event_id(out->fault), sid, dva);
            return;
        }

        if (!r100_smmu_build_s1cfg(s, cd, &cfg)) {
            out->ok = false;
            out->fault = R100_SMMU_FAULT_INV_STE;
            s->translates_fault++;
            r100_smmu_emit_debug(s,
                                 "smmu cl=%u xlate_out sid=%u dva=0x%"
                                 PRIx64 " FAULT inv_ste reason=bad_cd"
                                 " cd[0]=0x%x cd[1]=0x%x\n",
                                 s->chiplet_id, sid, (uint64_t)dva,
                                 cd[0], cd[1]);
            r100_smmu_emit_event(s,
                r100_smmu_fault_to_event_id(out->fault), sid, dva);
            return;
        }

        /* One line per CD decode for the debug tail — TT0 is the
         * "interesting" half (TT1 is disabled by FW's bypass CD
         * via EPD1=1). */
        r100_smmu_emit_debug(s,
                             "smmu cl=%u cd sid=%u v=1 aa64=%d ips=%u"
                             " oas=%u tbi=%u asid=%u affd=%d r=%d"
                             " tt0_tsz=%u tt0_gran=%u tt0_ttb=0x%"
                             PRIx64 " tt0_dis=%d tt1_dis=%d\n",
                             s->chiplet_id, sid,
                             cfg.aa64, R100_CD_IPS(cd), cfg.oas,
                             cfg.tbi, cfg.asid, cfg.affd,
                             cfg.record_faults,
                             cfg.tt[0].tsz, cfg.tt[0].granule_sz,
                             (uint64_t)cfg.tt[0].ttb,
                             cfg.tt[0].disabled, cfg.tt[1].disabled);
        r100_smmu_emit_debug(s,
                             "smmu cl=%u ptw_s1 sid=%u dva=0x%" PRIx64
                             " ttb=0x%" PRIx64 " tsz=%u gran=%u"
                             " perm=%s\n",
                             s->chiplet_id, sid, (uint64_t)dva,
                             (uint64_t)cfg.tt[0].ttb,
                             cfg.tt[0].tsz, cfg.tt[0].granule_sz,
                             acc_str);

        rc = smmu_ptw(NULL, &cfg, dva, perm, &tlbe, &info);
        if (rc != 0) {
            out->ok = false;
            out->fault = r100_smmu_map_ptw_err(info.type);
            out->fault_addr = info.addr ? info.addr : dva;
            s->translates_fault++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-smmu cl=%u TRANSLATE FAULT (s1) sid=%u"
                          " dva=0x%" PRIx64 " perm=%d → %s @ 0x%"
                          PRIx64 " (ttb=0x%" PRIx64 " tsz=%u gran=%u)\n",
                          s->chiplet_id, sid, dva, (int)perm,
                          r100_smmu_fault_str(out->fault),
                          out->fault_addr, cfg.tt[0].ttb,
                          cfg.tt[0].tsz, cfg.tt[0].granule_sz);
            r100_smmu_emit_debug(s,
                                 "smmu cl=%u xlate_out sid=%u dva=0x%"
                                 PRIx64 " FAULT(s1) %s @ 0x%" PRIx64
                                 " ttb=0x%" PRIx64 " tsz=%u gran=%u\n",
                                 s->chiplet_id, sid, (uint64_t)dva,
                                 r100_smmu_fault_str(out->fault),
                                 (uint64_t)out->fault_addr,
                                 (uint64_t)cfg.tt[0].ttb,
                                 cfg.tt[0].tsz, cfg.tt[0].granule_sz);
            r100_smmu_emit_event(s,
                r100_smmu_fault_to_event_id(out->fault), sid,
                out->fault_addr);
            return;
        }

        out->ok = true;
        out->pa = tlbe.entry.translated_addr +
                  (dva & tlbe.entry.addr_mask);
        s->translates_ok++;
        r100_smmu_emit_debug(s,
                             "smmu cl=%u xlate_out sid=%u dva=0x%" PRIx64
                             " ok(s1) pa=0x%" PRIx64 " page_base=0x%"
                             PRIx64 " mask=0x%" PRIx64 "\n",
                             s->chiplet_id, sid, (uint64_t)dva,
                             (uint64_t)out->pa,
                             (uint64_t)tlbe.entry.translated_addr,
                             (uint64_t)tlbe.entry.addr_mask);
        return;
    }
    /* S2_TRANS or ALL_TRANS — both consume STE2/STE3 stage-2 fields.
     * For ALL_TRANS, q-sys's `smmu_init_ste` sets STE1.S1DSS=BYPASS
     * (stage-1 skipped), so the IPA fed to stage-2 == the IOVA fed
     * by the master. v1 collapses ALL_TRANS to a stage-2-only walk
     * (no nested decode). v2 honours S1DSS for ALL_TRANS and walks
     * CD when S1DSS=SUBSTREAM0. */

    if (!r100_smmu_build_s2cfg(s, ste, &cfg)) {
        out->ok = false;
        out->fault = R100_SMMU_FAULT_INV_STE;
        s->translates_fault++;
        r100_smmu_emit_debug(s,
                             "smmu cl=%u xlate_out sid=%u dva=0x%" PRIx64
                             " FAULT inv_ste reason=bad_s2cfg\n",
                             s->chiplet_id, sid, (uint64_t)dva);
        r100_smmu_emit_event(s,
            r100_smmu_fault_to_event_id(out->fault), sid, dva);
        return;
    }

    r100_smmu_emit_debug(s,
                         "smmu cl=%u ptw sid=%u dva=0x%" PRIx64
                         " vttb=0x%" PRIx64 " tsz=%u sl0=%u gran=%u"
                         " eff_ps=%u perm=%s\n",
                         s->chiplet_id, sid, (uint64_t)dva,
                         cfg.s2cfg.vttb, cfg.s2cfg.tsz,
                         cfg.s2cfg.sl0, cfg.s2cfg.granule_sz,
                         cfg.s2cfg.eff_ps, acc_str);

    rc = smmu_ptw(NULL, &cfg, dva, perm, &tlbe, &info);
    if (rc != 0) {
        out->ok = false;
        out->fault = r100_smmu_map_ptw_err(info.type);
        out->fault_addr = info.addr ? info.addr : dva;
        s->translates_fault++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-smmu cl=%u TRANSLATE FAULT sid=%u dva=0x%"
                      PRIx64 " perm=%d → %s @ 0x%" PRIx64
                      " (vttb=0x%" PRIx64 " tsz=%u sl0=%u gran=%u)\n",
                      s->chiplet_id, sid, dva, (int)perm,
                      r100_smmu_fault_str(out->fault),
                      out->fault_addr, cfg.s2cfg.vttb,
                      cfg.s2cfg.tsz, cfg.s2cfg.sl0,
                      cfg.s2cfg.granule_sz);
        r100_smmu_emit_debug(s,
                             "smmu cl=%u xlate_out sid=%u dva=0x%" PRIx64
                             " FAULT %s @ 0x%" PRIx64 " vttb=0x%" PRIx64
                             " tsz=%u sl0=%u gran=%u\n",
                             s->chiplet_id, sid, (uint64_t)dva,
                             r100_smmu_fault_str(out->fault),
                             (uint64_t)out->fault_addr, cfg.s2cfg.vttb,
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
    s->translates_ok++;
    r100_smmu_emit_debug(s,
                         "smmu cl=%u xlate_out sid=%u dva=0x%" PRIx64
                         " ok pa=0x%" PRIx64 " page_base=0x%" PRIx64
                         " mask=0x%" PRIx64 "\n",
                         s->chiplet_id, sid, (uint64_t)dva,
                         (uint64_t)out->pa,
                         (uint64_t)tlbe.entry.translated_addr,
                         (uint64_t)tlbe.entry.addr_mask);
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
    case SMMU_CR0: {
        uint32_t old = s->regs[reg_idx];
        s->regs[reg_idx] = (uint32_t)val;
        if (((old ^ (uint32_t)val) & SMMU_CR0_ACK_MASK) != 0) {
            r100_smmu_emit_debug(s,
                "smmu cl=%u CR0 0x%x→0x%x"
                " smmuen=%d→%d eventqen=%d→%d cmdqen=%d→%d\n",
                s->chiplet_id, old, (uint32_t)val,
                !!(old & SMMU_CR0_SMMUEN),
                !!(val & SMMU_CR0_SMMUEN),
                !!(old & SMMU_CR0_EVENTQEN),
                !!(val & SMMU_CR0_EVENTQEN),
                !!(old & SMMU_CR0_CMDQEN),
                !!(val & SMMU_CR0_CMDQEN));
        }
        break;
    }
    case SMMU_IRQ_CTRL: {
        uint32_t old = s->regs[reg_idx];
        s->regs[reg_idx] = (uint32_t)val;
        if (((old ^ (uint32_t)val) & SMMU_IRQ_CTRL_ACK_MASK) != 0) {
            r100_smmu_emit_debug(s,
                "smmu cl=%u IRQ_CTRL 0x%x→0x%x"
                " gerror_irqen=%d→%d eventq_irqen=%d→%d\n",
                s->chiplet_id, old, (uint32_t)val,
                !!(old & SMMU_IRQ_CTRL_GERROR_IRQEN),
                !!(val & SMMU_IRQ_CTRL_GERROR_IRQEN),
                !!(old & SMMU_IRQ_CTRL_EVENTQ_IRQEN),
                !!(val & SMMU_IRQ_CTRL_EVENTQ_IRQEN));
        }
        break;
    }
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
     * via HMP `info qtree` / qmp `qom-get` after a regression.
     * Same convention for the new translates_* / cmdq_processed
     * counters added with the smmu-debug chardev. */
}

static void r100_smmu_unrealize(DeviceState *dev)
{
    R100SMMUState *s = R100_SMMU(dev);

    qemu_chr_fe_deinit(&s->smmu_debug_chr, false);
}

static Property r100_smmu_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100SMMUState, chiplet_id, 0),
    DEFINE_PROP_CHR("debug-chardev", R100SMMUState, smmu_debug_chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_smmu_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_smmu_realize;
    dc->unrealize = r100_smmu_unrealize;
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
