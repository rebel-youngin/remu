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
     * (the host→NPU ingress path via r100-doorbell) deliberately
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
};

DECLARE_INSTANCE_CHECKER(R100MailboxState, R100_MAILBOX, TYPE_R100_MAILBOX)

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
    if (r100_mailbox_issr_src_emits(src)) {
        r100_mailbox_issr_emit(s, idx, val);
    }
}

void r100_mailbox_set_issr(R100MailboxState *s, uint32_t idx, uint32_t val)
{
    r100_mailbox_issr_store(s, idx, val, MBX_ISSR_SRC_HOST_RELAY);
}

/*
 * CM7-stub egress: mimic an ISSR write that would normally originate
 * from the PCIE_CM7 subcontroller's FW. Used by r100-doorbell to
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
    /* intgr_writes / issr counters kept across reset for test inspection. */
    r100_mailbox_update_irq(s, 0);
    r100_mailbox_update_irq(s, 1);
}

static const VMStateDescription r100_mailbox_vmstate = {
    .name = "r100-mailbox",
    .version_id = 2,
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
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_mailbox_properties[] = {
    DEFINE_PROP_STRING("name", R100MailboxState, name),
    DEFINE_PROP_CHR("issr-chardev", R100MailboxState, issr_chr),
    DEFINE_PROP_CHR("issr-debug-chardev", R100MailboxState, issr_debug_chr),
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
