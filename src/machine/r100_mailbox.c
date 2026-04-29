/*
 * R100 Samsung IPM mailbox SFR (Phase 2, M6 + M8a).
 *
 * Register layout per drivers/mailbox/ipm_samsung.h:
 *   0x00 MCUCTRL (bit 0 = mswrst)
 *   0x08/0x1C INTGR{0,1}  W1S pending
 *   0x0C/0x20 INTCR{0,1}  W1C pending
 *   0x10/0x24 INTMR{0,1}  mask
 *   0x14/0x28 INTSR{0,1}  = pending                  (R)
 *   0x18/0x2C INTMSR{0,1} = pending & ~INTMR         (R)
 *   0x6C MIF_INIT / 0x70 IS_VERSION / 0x80.. ISSR0..63
 *
 * Two qemu_irq outs (`irq[0]`, `irq[1]`) level-high whenever INTMSR is
 * non-zero. Clear via INTCR or MCUCTRL mswrst.
 *
 * M6 (commit 500856b): doorbell → r100_mailbox_raise_intgr() → INTMSR
 * nonzero → SPI asserted. M8a (commit cd24aa9): every MMIO ISSR write
 * re-emits an 8-byte (bar4_off, val) frame on the `issr` chardev so the
 * host BAR4 shadow stays in sync. Host→NPU ISSR (via
 * r100_mailbox_set_issr) does NOT re-emit — would loop the write back.
 *
 * Side bug 2 fix: the issr_store funnel also latches a one-shot
 * `fw_boot_done_seen` flag the first time q-sys writes
 * R100_FW_BOOT_DONE (= 0xFB0D) into ISSR[4] from the NPU_MMIO source.
 * r100-cm7 reads the flag via r100_mailbox_fw_boot_done_seen() to
 * decide whether to synthesise a post-soft-reset re-handshake; until
 * the real cold-boot publish has happened, the synthesis is held so
 * the kmd's cfg writes can't race ahead of q-sys's main.c:250 DCS
 * memset. See `docs/debugging.md` → Side bug 2.
 *
 * P10-fix (CM7 mailbox stub): on chiplet-0 MAILBOX_CP0_M4 the on-die
 * PCIE_CM7 normally services every q-sys ↔ CM7 handshake — most
 * importantly the `notify_dram_init_done` path (q/sys/drivers/smmu/
 * smmu.c:875 → q/sys/drivers/pcie/pcie_mailbox_callback.c:311 →
 * pcie_ep.c:dram_init_done_cb → m7_smmu_enable). Without the ack,
 * `CR0.SMMUEN` stays 0 on chiplet 0, q-cp's HDMA reads of LL chains
 * at IPA `buf_PA + PF_SYSTEM_IPA_BASE` (cb_parse_linked_dma) return
 * zeros instead of the stage-2-translated PA — the P10 hang.
 *
 * **Why only chiplet 0 needs this stub.** Real silicon's PCIE_CM7
 * lives on chiplet 0 — it's the chiplet attached to the PCIe block,
 * so the kmd, the host BAR mappings, and the on-die helper CPU that
 * services the post-DRAM-init PCIe handshake (BAR re-anchoring,
 * iATU reprogramming, AND the SMMU enable that gates host DMA) all
 * sit there. Chiplets 1..3 have no PCIe block and no CM7. q-sys's
 * `smmu_init()` reflects this asymmetry directly:
 *
 *     if (CHIPLET_ID == CHIPLET_ID0)
 *         notify_dram_init_done();   // chiplet 0: wait for CM7 ack
 *     else
 *         smmu_enable();             // chiplets 1..3: just MMIO-write CR0
 *
 * On chiplets 1..3 the FW writes `CR0.SMMUEN = 1` directly to its
 * own chiplet's SMMU MMIO — that store lands on `r100-smmu`'s normal
 * write handler and it works out-of-the-box. No mailbox, no
 * handshake, no stub needed. The pre-fix all-zero HDMA bug was
 * chiplet-0-only because q-cp's HDMA / RBDMA / DNC tasks all run on
 * CP0/CP1 of chiplet 0 (where the umd-submitted CBs land), and only
 * chiplet 0's SMMU was the one stuck off.
 *
 * Pre-this-fix, the address `MAILBOX_CP0_M4` was unmapped lazy RAM,
 * which paradoxically made q-sys boot work: `ipm_samsung_write`
 * stores via the cross-chiplet PA path (`base + target*OFFSET -
 * FREERTOS_VA_OFFSET`), `ipm_samsung_receive` reads via the
 * FREERTOS-VA path (`base` directly). Those land on different PAs
 * in the unmapped region, so every poll loop reads 0 on its first
 * iteration and exits — `notify_dram_init_done`'s loop short-
 * circuits past the in-tree `if (timeout <= 0) smmu_enable();`
 * fallback (CR0.SMMUEN never gets set), but the OTHER CM7 polls
 * (`rbln_pcie_get_ats_enabled` → `rbln_cm7_get_values` over
 * CM7_GET_VALUES_CHANNEL, etc.) exit cleanly with val=0 and the FW
 * boots through to FW_BOOT_DONE.
 *
 * The CM7-stub mode here preserves that read-zero behaviour
 * (ISSR reads always return 0 when `cm7-stub` is set), while adding
 * the side effect we *do* want: when FW raises INTGR1 bit
 * `cm7-dram-init-done-channel` (default 11) with the just-stored
 * value at ISSR[<channel>] == R100_FW_BOOT_DONE, we synchronously
 * RMW `CR0.SMMUEN` at `cm7-smmu-base + 0x20` via the
 * `ldl_le_phys` / `stl_le_phys` pair (so the `CMDQEN` / `EVENTQEN`
 * bits set earlier by `smmu_enable_queues` survive) — the
 * silicon-equivalent observable effect of `m7_smmu_enable`. q-sys's
 * poll exits on the next read (which returns 0 because cm7-stub is
 * on), the boot proceeds normally, and HDMA stage-2 translates work
 * because SMMUEN is now 1. The only divergence from silicon-CM7
 * (other than not actually running CM7 firmware) is that the
 * fallback `if (timeout <= 0) smmu_enable();` path is unreachable
 * in REMU regardless — the stub fires before the timeout would
 * have. See `docs/debugging.md` → "Open issue: P10".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"
#include "r100_soc.h"
#include "r100_mailbox.h"
#include "remu_frame.h"

/* Register offsets — match struct ipm_samsung in
 * external/ssw-bundle/.../drivers/mailbox/ipm_samsung.h. */
#define R100_MBX_MCUCTRL        0x00
#define R100_MBX_INTGR0         0x08
#define R100_MBX_INTCR0         0x0C
#define R100_MBX_INTMR0         0x10
#define R100_MBX_INTSR0         0x14
#define R100_MBX_INTMSR0        0x18
#define R100_MBX_INTGR1         0x1C
#define R100_MBX_INTCR1         0x20
#define R100_MBX_INTMR1         0x24
#define R100_MBX_INTSR1         0x28
#define R100_MBX_INTMSR1        0x2C
#define R100_MBX_MIF_INIT       0x6C
#define R100_MBX_IS_VERSION     0x70
#define R100_MBX_ISSR0          0x80

#define R100_MBX_MCUCTRL_MSWRST BIT(0)

/*
 * MMIO-region size and backing storage sizes for the mailbox SFR.
 * One 4 KB SFR at a fixed chiplet-relative base; see the top-of-file
 * comment for register layout. 64 ISSR scratch registers follow
 * ipm_samsung.h.
 */
#define R100_MBX_SFR_SIZE       0x1000
#define R100_MBX_ISSR_COUNT     64

/* SMMU-600 register offsets the CM7 dram-init-done stub touches.
 * Match q/sys/.../drivers/smmu/smmu.h: SMMU_CR0 / SMMU_CR0ACK at
 * 0x020 / 0x024, CR0_SMMUEN = BIT(0). Keep local — pulling in the
 * device's own header would couple the mailbox to r100-smmu's
 * internals, and we only need the two offsets + the bit. */
#define R100_MBX_CM7_SMMU_CR0_OFFSET    0x20
#define R100_MBX_CM7_SMMU_CR0_SMMUEN    (1u << 0)

struct R100MailboxState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq[2];            /* [0] = INTMSR0, [1] = INTMSR1 */
    char *name;                 /* e.g. "pcie.chiplet0" for debug */

    /* M8 NPU→host ISSR shadow-egress. When `issr_chr` is connected,
     * every FW-initiated ISSR write (MMIO path) is also emitted as
     * an 8-byte (BAR4-offset, value) frame on this chardev so the
     * host-side r100-npu-pci can mirror the write into its BAR4
     * MMIO register file. Writes driven by r100_mailbox_set_issr()
     * (the host→NPU ingress path via r100-cm7) deliberately
     * skip the emit to avoid echoing frames back at the host. */
    CharBackend issr_chr;
    CharBackend issr_debug_chr;

    uint32_t mcuctrl;
    /* Combined INTGR/INTSR storage: INTGR is W1S into `pending`, INTSR
     * and INTGR reads both return it. */
    uint32_t pending[2];
    uint32_t intmr[2];
    uint32_t mif_init;
    uint32_t is_version;
    uint32_t issr[R100_MBX_ISSR_COUNT];

    /* Observability counters (survive reset, inspectable via HMP). */
    uint64_t intgr_writes[2];
    uint64_t issr_egress_frames;    /* emitted to host (MMIO writes) */
    uint64_t issr_egress_dropped;   /* short write / backend gone */
    uint64_t issr_ingress_writes;   /* set via r100_mailbox_set_issr */

    /* One-shot latch: set when an NPU_MMIO write of R100_FW_BOOT_DONE
     * lands on ISSR[4]. r100-cm7 polls this to gate its post-soft-
     * reset FW_BOOT_DONE re-synthesis (Side bug 2 fix). Stays set for
     * the life of the boot — kmd's HOST_RELAY clears of ISSR[4] do
     * not flip it back. Reset to false on machine cold reset; carried
     * across vmstate snapshots. */
    bool fw_boot_done_seen;

    /* P10-fix: optional on-die-CM7 mailbox stub. When `cm7_stub` is
     * set, this mailbox terminates the q-sys ↔ PCIE_CM7 channel —
     * see top-of-file banner. ISSR reads always return 0 to match
     * the lazy-RAM-via-FREERTOS-VA shape every CM7 poll loop relies
     * on for early exit; writes to INTGR1 bit
     * `cm7_dram_init_done_channel` with the just-stored ISSR slot
     * holding R100_FW_BOOT_DONE poke `CR0.SMMUEN` at
     * `cm7_smmu_base + 0x20` (silicon-equivalent
     * `dram_init_done_cb → m7_smmu_enable`). `cm7_smmu_base` = 0 is
     * a soft no-op so pre-SoC-realize tests can flip the flag
     * without crashing. */
    bool cm7_stub;
    uint32_t cm7_dram_init_done_channel;
    uint64_t cm7_smmu_base;
    uint64_t cm7_dram_init_done_acks;
};

DECLARE_INSTANCE_CHECKER(R100MailboxState, R100_MAILBOX, TYPE_R100_MAILBOX)

static void r100_mailbox_cm7_dram_init_done_check(R100MailboxState *s,
                                                  uint32_t intgr1_val);

static inline uint32_t r100_mailbox_masked(const R100MailboxState *s, int g)
{
    return s->pending[g] & ~s->intmr[g];
}

static void r100_mailbox_update_irq(R100MailboxState *s, int g)
{
    int level = r100_mailbox_masked(s, g) != 0;
    qemu_set_irq(s->irq[g], level);
}

void r100_mailbox_raise_intgr(R100MailboxState *s, int group, uint32_t val)
{
    if (!s || (group != 0 && group != 1)) {
        return;
    }
    s->pending[group] |= val;
    s->intgr_writes[group]++;
    r100_mailbox_update_irq(s, group);
    if (group == 1) {
        r100_mailbox_cm7_dram_init_done_check(s, val);
    }
}

/*
 * Where an ISSR write originated — determines whether we emit a
 * (bar4_off, val) frame on the NPU→host egress chardev.
 *
 *   MBX_ISSR_SRC_NPU_MMIO   FW store to ISSR{N} MMIO → host shadow
 *                            must converge: emit.
 *   MBX_ISSR_SRC_CM7_STUB   REMU shortcut for the PCIE_CM7 FW that
 *                            isn't modelled (only entry: the
 *                            SOFT_RESET doorbell → FW_BOOT_DONE
 *                            path). Same observable effect as an
 *                            MMIO store from FW: emit.
 *   MBX_ISSR_SRC_HOST_RELAY Doorbell-routed BAR4 payload write
 *                            arriving from the other QEMU. Host
 *                            already holds this value in its BAR4
 *                            shadow; echoing would loop across the
 *                            two processes: do NOT emit.
 *
 * Every r100_mailbox ISSR-write entry point funnels through
 * r100_mailbox_issr_store() below so the emit rule is one switch, not
 * three implicit behaviours baked into three separate functions.
 */
typedef enum {
    MBX_ISSR_SRC_NPU_MMIO = 0,
    MBX_ISSR_SRC_CM7_STUB,
    MBX_ISSR_SRC_HOST_RELAY,
} MbxIssrSrc;

static inline bool r100_mailbox_issr_src_emits(MbxIssrSrc src)
{
    return src == MBX_ISSR_SRC_NPU_MMIO || src == MBX_ISSR_SRC_CM7_STUB;
}

/*
 * M8a NPU→host ISSR egress. Frame's bar4_off = MAILBOX_BASE + idx*4
 * (host-side offset, not NPU SFR offset — keeps host parser trivial).
 * Non-blocking; silicon doesn't back-pressure either.
 */
static void r100_mailbox_issr_emit_debug(R100MailboxState *s, uint32_t idx,
                                         uint32_t val)
{
    char line[96];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->issr_debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "issr[%s] idx=%u val=0x%x egress=%" PRIu64 "\n",
                 s->name ? s->name : "?", idx, val, s->issr_egress_frames);
    if (n > 0) {
        qemu_chr_fe_write(&s->issr_debug_chr, (const uint8_t *)line, n);
    }
}

static void r100_mailbox_issr_emit(R100MailboxState *s, uint32_t idx,
                                   uint32_t val)
{
    uint32_t bar4_off = R100_BAR4_MAILBOX_BASE + idx * 4U;
    RemuFrameEmitResult res =
        remu_frame_emit(&s->issr_chr, "r100-mailbox issr", bar4_off, val);

    switch (res) {
    case REMU_FRAME_EMIT_OK:
        s->issr_egress_frames++;
        r100_mailbox_issr_emit_debug(s, idx, val);
        break;
    case REMU_FRAME_EMIT_DISCONNECTED:
        /* Single-QEMU run or host not attached yet — silently skip,
         * matches prior behaviour. */
        break;
    case REMU_FRAME_EMIT_SHORT_WRITE:
        s->issr_egress_dropped++;
        break;
    }
}

/*
 * Single funnel for every ISSR scratch-register write. Updates the
 * backing store, bumps the matching observability counter, and emits
 * on the egress chardev iff the source says the host needs to see it.
 * Out-of-range idx is a no-op (matches silicon RAZ/WI for reserved).
 *
 * Side bug 2 latch: if this is q-sys's `bootdone_task` writing
 * R100_FW_BOOT_DONE into ISSR[4] (NPU_MMIO source — the only origin
 * for that store on a real cold boot; CM7_STUB is REMU's
 * post-soft-reset shortcut and doesn't count), set the one-shot
 * `fw_boot_done_seen` flag. r100-cm7 reads it to decide whether to
 * answer SOFT_RESET with a synthetic handshake (see
 * r100_mailbox_fw_boot_done_seen + r100_cm7.c).
 */
static void r100_mailbox_issr_store(R100MailboxState *s, uint32_t idx,
                                    uint32_t val, MbxIssrSrc src)
{
    if (!s || idx >= R100_MBX_ISSR_COUNT) {
        return;
    }
    s->issr[idx] = val;
    if (src == MBX_ISSR_SRC_HOST_RELAY) {
        s->issr_ingress_writes++;
    }
    if (src == MBX_ISSR_SRC_NPU_MMIO && idx == 4 && val == R100_FW_BOOT_DONE) {
        s->fw_boot_done_seen = true;
    }
    if (r100_mailbox_issr_src_emits(src)) {
        r100_mailbox_issr_emit(s, idx, val);
    }
}

void r100_mailbox_set_issr(R100MailboxState *s, uint32_t idx, uint32_t val)
{
    r100_mailbox_issr_store(s, idx, val, MBX_ISSR_SRC_HOST_RELAY);
}

/*
 * Peek at ISSR[idx] without the MMIO traversal (and without any of
 * the egress-emit plumbing). Strictly read-only so the three-way
 * source bookkeeping in r100_mailbox_issr_store() stays
 * authoritative.
 */
uint32_t r100_mailbox_get_issr(R100MailboxState *s, uint32_t idx)
{
    if (!s || idx >= R100_MBX_ISSR_COUNT) {
        return 0;
    }
    return s->issr[idx];
}

/*
 * One-shot latch set by issr_store on the first NPU_MMIO write of
 * R100_FW_BOOT_DONE to ISSR[4] (q-sys's `bootdone_task` publish).
 * NULL-safe so callers can wire `pf-mailbox` lazily without a
 * separate guard. See header comment + Side bug 2 in docs/debugging.md.
 */
bool r100_mailbox_fw_boot_done_seen(R100MailboxState *s)
{
    return s != NULL && s->fw_boot_done_seen;
}

/*
 * In-process multi-slot ISSR write. Bypasses the issr_store() funnel
 * because this path has no host-visible side effects — the targets
 * are NPU-internal mailboxes (PERI0_M9_CPU1 etc.) that q-cp polls
 * directly from CA73, so there is no egress chardev to emit on and
 * the host-relay/MMIO/CM7-stub bookkeeping doesn't apply.
 */
void r100_mailbox_set_issr_words(R100MailboxState *s, uint32_t idx,
                                 const uint32_t *vals, uint32_t count)
{
    if (!s || !vals) {
        return;
    }
    for (uint32_t i = 0; i < count && idx + i < R100_MBX_ISSR_COUNT; i++) {
        s->issr[idx + i] = vals[i];
    }
}

/*
 * CM7-stub egress: mimic an ISSR write that would normally originate
 * from the PCIE_CM7 subcontroller's FW. Used by r100-cm7 to
 * implement the REMU CM7-relay shortcut — on silicon, a host SOFT_RESET
 * doorbell ends in pcie_soft_reset_handler on CM7, which writes
 * FW_BOOT_DONE back into PF.ISSR[4] via MMIO; that MMIO write is what
 * we're simulating here. Updates the backing store *and* emits the
 * egress frame so the host BAR4 shadow converges, exactly as if CM7
 * had done `sfr->issr4 = 0xFB0D` on the real chip.
 */
void r100_mailbox_cm7_stub_write_issr(R100MailboxState *s, uint32_t idx,
                                      uint32_t val)
{
    r100_mailbox_issr_store(s, idx, val, MBX_ISSR_SRC_CM7_STUB);
}

/*
 * P10-fix: PCIE_CM7 dram-init-done stub. Mirrors the silicon path in
 * q/sys/drivers/pcie/pcie_mailbox_callback.c:311 →
 * pcie_ep.c:dram_init_done_cb → m7_smmu_enable() — set CR0.SMMUEN on
 * this chiplet's SMMU and clear the handshake ISSR slot so q-sys's
 * poll loop in notify_dram_init_done() observes val == 0 and exits.
 * Called from the INTGR1 write path when the matching channel bit is
 * raised AND ISSR[channel] reads 0xFB0D — same trigger a real CM7
 * sees on its mailbox IRQ. NULL-base is a soft no-op (test runs may
 * enable the stub before SoC realize wires the SMMU MMIO).
 */
static void r100_mailbox_cm7_dram_init_done_check(R100MailboxState *s,
                                                  uint32_t intgr1_val)
{
    uint32_t channel;
    hwaddr cr0_pa;
    uint32_t cr0;

    if (!s->cm7_stub) {
        return;
    }
    channel = s->cm7_dram_init_done_channel;
    if (!(intgr1_val & (1u << channel))) {
        return;
    }
    if (channel >= R100_MBX_ISSR_COUNT ||
        s->issr[channel] != R100_FW_BOOT_DONE) {
        return;
    }
    if (s->cm7_smmu_base == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-mailbox[%s]: CM7 dram-init-done fired but "
                      "cm7-smmu-base unset; skipping SMMUEN poke\n",
                      s->name ? s->name : "?");
        s->issr[channel] = 0;
        return;
    }

    /* Pair little-endian load + store so we don't depend on host
     * endianness when targeting r100-smmu's DEVICE_LITTLE_ENDIAN
     * MMIO. RMW because CR0's other bits (CMDQEN / EVENTQEN) were
     * set earlier in `smmu_init_queues` and must survive. */
    cr0_pa = s->cm7_smmu_base + R100_MBX_CM7_SMMU_CR0_OFFSET;
    cr0 = ldl_le_phys(&address_space_memory, cr0_pa);
    cr0 |= R100_MBX_CM7_SMMU_CR0_SMMUEN;
    stl_le_phys(&address_space_memory, cr0_pa, cr0);
    s->issr[channel] = 0;
    s->cm7_dram_init_done_acks++;
    qemu_log_mask(LOG_TRACE,
                  "r100-mailbox[%s]: CM7 dram-init-done ack — "
                  "SMMUEN set on 0x%" PRIx64 ", ISSR[%u] cleared "
                  "(acks=%" PRIu64 ")\n",
                  s->name ? s->name : "?", s->cm7_smmu_base, channel,
                  s->cm7_dram_init_done_acks);
}

static uint64_t r100_mailbox_read(void *opaque, hwaddr addr, unsigned size)
{
    R100MailboxState *s = R100_MAILBOX(opaque);

    switch (addr) {
    case R100_MBX_MCUCTRL:
        return s->mcuctrl;
    /* INTGR reads = pending (same storage as INTSR on silicon). */
    case R100_MBX_INTGR0:
    case R100_MBX_INTSR0:
        return s->pending[0];
    case R100_MBX_INTGR1:
    case R100_MBX_INTSR1:
        return s->pending[1];
    case R100_MBX_INTCR0:
    case R100_MBX_INTCR1:
        return 0; /* write-only */
    case R100_MBX_INTMR0:
        return s->intmr[0];
    case R100_MBX_INTMR1:
        return s->intmr[1];
    case R100_MBX_INTMSR0:
        return r100_mailbox_masked(s, 0);
    case R100_MBX_INTMSR1:
        return r100_mailbox_masked(s, 1);
    case R100_MBX_MIF_INIT:
        return s->mif_init;
    case R100_MBX_IS_VERSION:
        return s->is_version;
    default:
        if (addr >= R100_MBX_ISSR0 &&
            addr < R100_MBX_ISSR0 + R100_MBX_ISSR_COUNT * 4) {
            /* CM7-stub mode: every CM7 channel poll loop in q-sys
             * (notify_dram_init_done, rbln_cm7_get_values,
             * rbln_pcie_request_ats_iatu) waits for "CM7" to clear
             * the ISSR slot it just wrote to. With no real CM7,
             * always returning 0 lets every poll exit on its first
             * iteration — matching the pre-fix lazy-RAM shape where
             * read (FREERTOS-VA path) hit a different PA than write
             * (cross-chiplet path) and so always read 0. The
             * dram-init-done side effect (setting CR0.SMMUEN) still
             * fires from the INTGR1 write handler before the FW
             * polls. See top-of-file banner. */
            if (s->cm7_stub) {
                return 0;
            }
            return s->issr[(addr - R100_MBX_ISSR0) >> 2];
        }
        qemu_log_mask(LOG_UNIMP,
                      "r100-mailbox[%s]: unimplemented read @ 0x%" HWADDR_PRIx "\n",
                      s->name ? s->name : "?", addr);
        return 0;
    }
}

static void r100_mailbox_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    R100MailboxState *s = R100_MAILBOX(opaque);
    uint32_t v = (uint32_t)val;

    switch (addr) {
    case R100_MBX_MCUCTRL:
        s->mcuctrl = v & R100_MBX_MCUCTRL_MSWRST;
        if (v & R100_MBX_MCUCTRL_MSWRST) {
            /* Soft-reset: clear pending + mask. */
            s->pending[0] = s->pending[1] = 0;
            s->intmr[0] = s->intmr[1] = 0;
            r100_mailbox_update_irq(s, 0);
            r100_mailbox_update_irq(s, 1);
        }
        break;
    case R100_MBX_INTGR0:
        s->pending[0] |= v;
        s->intgr_writes[0]++;
        r100_mailbox_update_irq(s, 0);
        break;
    case R100_MBX_INTGR1:
        s->pending[1] |= v;
        s->intgr_writes[1]++;
        r100_mailbox_update_irq(s, 1);
        r100_mailbox_cm7_dram_init_done_check(s, v);
        break;
    case R100_MBX_INTCR0:
        s->pending[0] &= ~v;
        r100_mailbox_update_irq(s, 0);
        break;
    case R100_MBX_INTCR1:
        s->pending[1] &= ~v;
        r100_mailbox_update_irq(s, 1);
        break;
    case R100_MBX_INTMR0:
        s->intmr[0] = v;
        r100_mailbox_update_irq(s, 0);
        break;
    case R100_MBX_INTMR1:
        s->intmr[1] = v;
        r100_mailbox_update_irq(s, 1);
        break;
    case R100_MBX_INTSR0:
    case R100_MBX_INTSR1:
    case R100_MBX_INTMSR0:
    case R100_MBX_INTMSR1:
        /* RO status mirrors; writes ignored on silicon. */
        break;
    case R100_MBX_MIF_INIT:
        s->mif_init = v;
        break;
    case R100_MBX_IS_VERSION:
        s->is_version = v;
        break;
    default:
        if (addr >= R100_MBX_ISSR0 &&
            addr < R100_MBX_ISSR0 + R100_MBX_ISSR_COUNT * 4) {
            uint32_t idx = (addr - R100_MBX_ISSR0) >> 2;
            /* MMIO path = NPU→host egress via the issr_store funnel.
             * Host→NPU path takes r100_mailbox_set_issr (HOST_RELAY
             * source), which skips the emit to avoid an echo loop. */
            r100_mailbox_issr_store(s, idx, v, MBX_ISSR_SRC_NPU_MMIO);
            return;
        }
        qemu_log_mask(LOG_UNIMP,
                      "r100-mailbox[%s]: unimplemented write @ 0x%" HWADDR_PRIx
                      " = 0x%x\n", s->name ? s->name : "?", addr, v);
        break;
    }
}

static const MemoryRegionOps r100_mailbox_ops = {
    .read = r100_mailbox_read,
    .write = r100_mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void r100_mailbox_realize(DeviceState *dev, Error **errp)
{
    R100MailboxState *s = R100_MAILBOX(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char region_name[64];

    snprintf(region_name, sizeof(region_name),
             "r100-mailbox.%s", s->name ? s->name : "anon");
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_mailbox_ops, s,
                          region_name, R100_MBX_SFR_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq[0]);
    sysbus_init_irq(sbd, &s->irq[1]);
    /* ISSR chardev is write-only egress; no receive handler. Optional. */
}

static void r100_mailbox_unrealize(DeviceState *dev)
{
    R100MailboxState *s = R100_MAILBOX(dev);

    qemu_chr_fe_deinit(&s->issr_chr, false);
    qemu_chr_fe_deinit(&s->issr_debug_chr, false);
}

static void r100_mailbox_reset(DeviceState *dev)
{
    R100MailboxState *s = R100_MAILBOX(dev);

    s->mcuctrl = 0;
    s->pending[0] = s->pending[1] = 0;
    s->intmr[0] = s->intmr[1] = 0;
    s->mif_init = 0;
    s->is_version = 0;
    memset(s->issr, 0, sizeof(s->issr));
    /* intgr_writes / issr / cm7_dram_init_done_acks counters kept across
     * reset for test inspection. */
    /* Side bug 2 latch: clear so a fresh cold boot has to re-publish. */
    s->fw_boot_done_seen = false;
    r100_mailbox_update_irq(s, 0);
    r100_mailbox_update_irq(s, 1);
}

static const VMStateDescription r100_mailbox_vmstate = {
    .name = "r100-mailbox",
    .version_id = 4,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(mcuctrl, R100MailboxState),
        VMSTATE_UINT32_ARRAY(pending, R100MailboxState, 2),
        VMSTATE_UINT32_ARRAY(intmr, R100MailboxState, 2),
        VMSTATE_UINT32(mif_init, R100MailboxState),
        VMSTATE_UINT32(is_version, R100MailboxState),
        VMSTATE_UINT32_ARRAY(issr, R100MailboxState, R100_MBX_ISSR_COUNT),
        VMSTATE_UINT64_ARRAY(intgr_writes, R100MailboxState, 2),
        VMSTATE_UINT64_V(issr_egress_frames, R100MailboxState, 2),
        VMSTATE_UINT64_V(issr_egress_dropped, R100MailboxState, 2),
        VMSTATE_UINT64_V(issr_ingress_writes, R100MailboxState, 2),
        VMSTATE_BOOL_V(fw_boot_done_seen, R100MailboxState, 3),
        VMSTATE_UINT64_V(cm7_dram_init_done_acks, R100MailboxState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_mailbox_properties[] = {
    DEFINE_PROP_STRING("name", R100MailboxState, name),
    DEFINE_PROP_CHR("issr-chardev", R100MailboxState, issr_chr),
    DEFINE_PROP_CHR("issr-debug-chardev", R100MailboxState, issr_debug_chr),
    /* P10-fix: CM7 mailbox stub. See top-of-file banner +
     * r100_mailbox_cm7_dram_init_done_check(). Only the chiplet-0
     * MAILBOX_CP0_M4 instance opts in. ISSR reads always return 0
     * when cm7-stub is true (matches the lazy-RAM-via-FREERTOS-VA
     * shape every CM7 poll relies on); the SMMU-enable side effect
     * lives behind the INTGR1[<channel>] trigger, fed by
     * `cm7-smmu-base`. */
    DEFINE_PROP_BOOL("cm7-stub", R100MailboxState, cm7_stub, false),
    DEFINE_PROP_UINT32("cm7-dram-init-done-channel", R100MailboxState,
                       cm7_dram_init_done_channel, 11),
    DEFINE_PROP_UINT64("cm7-smmu-base", R100MailboxState,
                       cm7_smmu_base, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_mailbox_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 Samsung IPM mailbox SFR block";
    dc->realize = r100_mailbox_realize;
    dc->unrealize = r100_mailbox_unrealize;
    dc->vmsd = &r100_mailbox_vmstate;
    device_class_set_legacy_reset(dc, r100_mailbox_reset);
    device_class_set_props(dc, r100_mailbox_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    /* Machine-instantiated (SPI wiring is topology). */
    dc->user_creatable = false;
}

static const TypeInfo r100_mailbox_info = {
    .name          = TYPE_R100_MAILBOX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100MailboxState),
    .class_init    = r100_mailbox_class_init,
};

static void r100_mailbox_register_types(void)
{
    type_register_static(&r100_mailbox_info);
}

type_init(r100_mailbox_register_types)
