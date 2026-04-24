/*
 * R100 NPU-side PCIe doorbell ingress (M6 INTGR + M8a ISSR + M8b Stage 3a CM7 stub
 * + M8b Stage 3b QINIT CM7/HDMA stub).
 *
 * Terminates the cross-process channels from the host QEMU. Three
 * inbound streams + one outbound stream arrive/leave through this
 * device:
 *
 *   doorbell chardev  (host→NPU, 8-byte (off, val) frames — remu_frame.h)
 *     0x08 INTGR0   — M8b Stage 3a CM7 stub (SOFT_RESET bit 0 synthesises
 *                     FW_BOOT_DONE into PF.ISSR[4]; other bits → VF0.INTGR1)
 *     0x1c INTGR1   — M6 host→NPU IRQ (r100_mailbox_raise_intgr → SPI)
 *     0x80..0x180   — M8a ISSR payload (r100_mailbox_set_issr: scratch
 *                     write-through, no SPI, no chardev loopback)
 *                   Additionally: INTGR1 bit 7 (REBEL_DOORBELL_QUEUE_INIT)
 *                   is the CM7-stub trigger for M8b Stage 3b — see
 *                   r100_doorbell_qinit_stub() below.
 *
 *   cfg chardev      (host→NPU, 8-byte (cfg_off, val) frames — remu_frame.h)
 *                   BAR2 cfg-head writes from the x86 guest kmd land here
 *                   and populate cfg_shadow[], so the CM7 stub can read
 *                   DDH_BASE_{LO,HI} without a round-trip.
 *
 *   hdma chardev     (NPU→host, variable-length frames — remu_hdma_proto.h)
 *                   HDMA_OP_WRITE frames carry (dst_addr, payload); the
 *                   host r100-npu-pci executes them as pci_dma_write into
 *                   the kmd's dma_alloc_coherent pages. Used today only by
 *                   the QINIT stub; M9 will reuse it for BD-done writes.
 *
 * Each stream has an optional debug-chardev tail: one ASCII line per
 * delivered / emitted frame to output/<run>/<stream>.log. All chardevs
 * are machine-instantiated via
 * `-machine r100-soc,doorbell=<id>,cfg=<id>,hdma=<id>` (see r100_soc.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "r100_soc.h"
#include "remu_frame.h"
#include "remu_doorbell_proto.h"
#include "remu_hdma_proto.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100DoorbellState, R100_DOORBELL)

/* Number of u32 slots in the BAR2 cfg-head shadow (4 KB / 4). */
#define R100_CFG_SHADOW_COUNT (REMU_BAR2_CFG_HEAD_SIZE / 4u)

struct R100DoorbellState {
    SysBusDevice parent_obj;

    CharBackend chr;              /* doorbell ingress (host→NPU) */
    CharBackend debug_chr;        /* doorbell ASCII tail */
    CharBackend cfg_chr;          /* cfg ingress (host→NPU) */
    CharBackend cfg_debug_chr;    /* cfg ASCII tail */
    CharBackend hdma_chr;         /* hdma egress (NPU→host) */
    CharBackend hdma_debug_chr;   /* hdma ASCII tail */

    R100MailboxState *mailbox;      /* VF0: M6 INTGR sink + M8a ISSR sink */
    R100MailboxState *pf_mailbox;   /* PF: CM7-stub FW_BOOT_DONE target (M8b 3a) */

    /* Chardev byte stream may split a frame across callbacks. */
    RemuFrameRx rx;
    RemuFrameRx cfg_rx;

    /* Doorbell-stream counters. */
    uint64_t frames_received;
    uint32_t last_offset;
    uint32_t last_value;

    /* Cfg-shadow mirror of host BAR2 cfg head (4 KB worth of u32s).
     * Host-initiated writes land here via the cfg chardev; NPU code
     * reads DDH_BASE_{LO,HI} (+0xC0/0xC4) when synthesising HDMA. */
    uint32_t cfg_shadow[R100_CFG_SHADOW_COUNT];
    uint64_t cfg_frames_received;
    uint64_t cfg_frames_dropped;
    uint32_t cfg_last_offset;
    uint32_t cfg_last_value;

    /* HDMA-egress counters (CM7-stub productions, plus future M9 uses). */
    uint64_t hdma_frames_sent;
    uint64_t hdma_frames_dropped;
    /* QINIT stub self-diagnostics — bumped on each of the conditions
     * that short-circuit the stub so tests can assert the no-op path. */
    uint64_t qinit_stubs_fired;
    uint64_t qinit_stubs_dropped;
};

static int r100_doorbell_can_receive(void *opaque)
{
    R100DoorbellState *s = opaque;
    return remu_frame_rx_headroom(&s->rx);
}

static int r100_doorbell_cfg_can_receive(void *opaque)
{
    R100DoorbellState *s = opaque;
    return remu_frame_rx_headroom(&s->cfg_rx);
}

static void r100_doorbell_emit_debug(R100DoorbellState *s,
                                     uint32_t off, uint32_t val)
{
    char line[80];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "doorbell off=0x%x val=0x%x count=%" PRIu64 "\n",
                 off, val, s->frames_received);
    if (n > 0) {
        /* Best-effort: debug tail must not back-pressure ingress. */
        qemu_chr_fe_write(&s->debug_chr, (const uint8_t *)line, n);
    }
}

static void r100_doorbell_cfg_emit_debug(R100DoorbellState *s,
                                         uint32_t off, uint32_t val,
                                         const char *status)
{
    char line[96];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->cfg_debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "cfg off=0x%x val=0x%x status=%s count=%" PRIu64 "\n",
                 off, val, status, s->cfg_frames_received);
    if (n > 0) {
        qemu_chr_fe_write(&s->cfg_debug_chr, (const uint8_t *)line, n);
    }
}

static void r100_doorbell_hdma_emit_debug(R100DoorbellState *s,
                                          uint64_t dst, uint32_t len,
                                          const char *tag)
{
    char line[128];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->hdma_debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "hdma tag=%s dst=0x%" PRIx64 " len=%u sent=%" PRIu64 "\n",
                 tag, dst, len, s->hdma_frames_sent);
    if (n > 0) {
        qemu_chr_fe_write(&s->hdma_debug_chr, (const uint8_t *)line, n);
    }
}

/* FW_BOOT_DONE value written into PF.ISSR[4] by the CM7 stub on SOFT_RESET. */
#define REMU_FW_BOOT_DONE 0xFB0D

/* Best-effort HDMA write helper. Bumps sent/dropped counters so test
 * code (and future M9 BD completions) gets the same accounting. */
static bool r100_doorbell_hdma_write(R100DoorbellState *s, uint64_t dst,
                                     const void *payload, uint32_t len,
                                     const char *tag)
{
    RemuHdmaEmitResult rc = remu_hdma_emit_write(&s->hdma_chr,
                                                  "r100-doorbell hdma",
                                                  dst, payload, len);
    if (rc == REMU_HDMA_EMIT_OK) {
        s->hdma_frames_sent++;
        r100_doorbell_hdma_emit_debug(s, dst, len, tag);
        return true;
    }
    s->hdma_frames_dropped++;
    qemu_log_mask(LOG_UNIMP,
                  "r100-doorbell: hdma %s dropped dst=0x%" PRIx64
                  " len=%u rc=%d\n", tag, dst, len, (int)rc);
    return false;
}

/*
 * M8b Stage 3b — QINIT CM7 stub (NPU side of the split refactor).
 *
 * Silicon flow: kmd's rebel_queue_init() writes desc into host RAM,
 * programs DDH_BASE_{LO,HI} into BAR2 cfg head, then rings INTGR1
 * bit 7 (REBEL_DOORBELL_QUEUE_INIT). PCIe CM7 ISRs, reads desc via
 * HDMA, writes desc->fw_version + desc->init_done = 1 back.
 *
 * REMU: doorbell frame for INTGR1 arrives here; we recognise bit 7 and
 * synthesise the same two writes — but we can't read host RAM from the
 * NPU QEMU (iATU not modelled), so we hardcode fw_version to a string
 * whose major-version prefix matches whatever the current kmd stamps
 * in driver_version (rbln_device_version_check compares strncmp up to
 * the first '.'; today's drivers ship 3.x, so "3.remu-stub" passes).
 *
 * When HDMA grows an HDMA_OP_READ (+ async response), this helper can
 * be upgraded to an actual driver→fw mirror and the hardcoded major
 * drops.  Until then: update the literal below when drivers bump
 * majors.
 */
#define REMU_FW_VERSION_STR "3.remu-stub"

static void r100_doorbell_qinit_stub(R100DoorbellState *s)
{
    uint32_t lo = s->cfg_shadow[REMU_CFG_DDH_BASE_LO / 4];
    uint32_t hi = s->cfg_shadow[REMU_CFG_DDH_BASE_HI / 4];
    uint64_t ddh_host_addr = ((uint64_t)hi << 32) | lo;
    uint64_t desc_dma;
    uint8_t fw_version[REMU_DDH_VERSION_MAX];
    uint32_t init_done = 1;

    if (!qemu_chr_fe_backend_connected(&s->hdma_chr)) {
        /* Single-QEMU runs / tests don't wire hdma; treat as no-op. */
        qemu_log_mask(LOG_UNIMP,
                      "r100-doorbell: QINIT stub: hdma chardev not wired, "
                      "skipping (lo=0x%x hi=0x%x)\n", lo, hi);
        s->qinit_stubs_dropped++;
        return;
    }
    /* kmd writes desc_device + HOST_PHYS_BASE (rbln_dma_host_convert).
     * If DDH_BASE < HOST_PHYS_BASE, cfg head hasn't been programmed yet —
     * refuse rather than emit a garbage DMA. */
    if (ddh_host_addr < REMU_HOST_PHYS_BASE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-doorbell: QINIT stub: DDH_BASE=0x%" PRIx64
                      " < HOST_PHYS_BASE=0x%llx (cfg not programmed)\n",
                      ddh_host_addr,
                      (unsigned long long)REMU_HOST_PHYS_BASE);
        s->qinit_stubs_dropped++;
        return;
    }
    desc_dma = ddh_host_addr - REMU_HOST_PHYS_BASE;

    /* fw_version first so kmd's readl_poll_timeout_atomic never sees
     * init_done=1 with a still-empty fw_version. Buffer is
     * zero-filled so the remainder of the 52-byte field is NUL
     * (matches strncpy semantics in the kmd). */
    memset(fw_version, 0, sizeof(fw_version));
    strncpy((char *)fw_version, REMU_FW_VERSION_STR,
            sizeof(fw_version) - 1);
    if (!r100_doorbell_hdma_write(s, desc_dma + REMU_DDH_FW_VERSION_OFF,
                                  fw_version, sizeof(fw_version),
                                  "qinit/fw_version")) {
        s->qinit_stubs_dropped++;
        return;
    }
    if (!r100_doorbell_hdma_write(s, desc_dma + REMU_DDH_INIT_DONE_OFF,
                                  &init_done, sizeof(init_done),
                                  "qinit/init_done")) {
        s->qinit_stubs_dropped++;
        return;
    }
    s->qinit_stubs_fired++;
    qemu_log_mask(LOG_TRACE,
                  "r100-doorbell: QINIT stub: desc=0x%" PRIx64
                  " init_done=1 fw_version='%s' fired=%" PRIu64 "\n",
                  desc_dma, REMU_FW_VERSION_STR, s->qinit_stubs_fired);
}

static void r100_doorbell_deliver(R100DoorbellState *s,
                                  uint32_t off, uint32_t val)
{
    uint32_t issr_idx = 0;
    RemuDoorbellKind kind = remu_doorbell_classify(off, &issr_idx);

    s->frames_received++;
    s->last_offset = off;
    s->last_value = val;

    qemu_log_mask(LOG_TRACE,
                  "r100-doorbell: deliver off=0x%x val=0x%x count=%" PRIu64 "\n",
                  off, val, s->frames_received);

    switch (kind) {
    case REMU_DB_KIND_INTGR0:
        /*
         * CA73 soft-reset stub — narrowly scoped to INTGR0 bit 0.
         *
         * Cold boot is now real: with the PCIe mailbox INTID wiring
         * corrected (remu_addrmap.h post-mortem + docs/debugging.md),
         * q-sys' bootdone_task completes and emits PF.ISSR[4] =
         * 0xFB0D via r100-mailbox → `issr` chardev → host BAR4+0x90
         * shadow end-to-end. So this handler is NOT a cold-boot fake.
         *
         * What it still stubs is the *post-soft-reset re-handshake*:
         * kmd's rebel_hw_init(RBLN_RESET_FIRST) unconditionally clears
         * ISSR[4] and rings REBEL_DOORBELL_SOFT_RESET (INTGR0 bit 0),
         * then polls ISSR[4] for a fresh 0xFB0D. On silicon,
         * INTGR0 bit 0 is routed through PCIE_CM7 →
         * pcie_soft_reset_handler which *physically resets* the CA73s;
         * firmware reboots from BL1 and re-runs bootdone_task, which
         * naturally re-emits 0xFB0D. REMU does not model that reset
         * (see docs/roadmap.md Phase 2: "real CA73 soft-reset" —
         * future milestone), so we synthesise only the observable
         * endpoint: PF.ISSR[4] = 0xFB0D. The running firmware is
         * undisturbed.
         *
         * Other bits of INTGR0 are relayed into VF0.INTGR1 so that
         * q-sys' IDX_MAILBOX_PCIE_VF0 default_cb sees them (no-op on
         * CA73 today, kept for parity with silicon's path and to
         * surface any future subscribers via info-qtree/TRACE).
         *
         * VF0.INTGR0 itself is left untouched — q-sys has no VF0
         * INTGR0 subscriber, so forwarding there would just spam
         * default_cb dumps.
         */
        if ((val & 0x1U) && s->pf_mailbox) {
            r100_mailbox_cm7_stub_write_issr(s->pf_mailbox, 4,
                                             REMU_FW_BOOT_DONE);
        }
        if (val & ~0x1U) {
            r100_mailbox_raise_intgr(s->mailbox, 1, val & ~0x1U);
        }
        break;
    case REMU_DB_KIND_INTGR1:
        r100_mailbox_raise_intgr(s->mailbox, 1, val);
        /*
         * M8b Stage 3b CM7 stub: INTGR1 bit 7 (REBEL_DOORBELL_QUEUE_INIT).
         * Silicon flows host→CM7→HDMA; REMU flows host→doorbell-chardev
         * →this handler→hdma-chardev→host pci_dma_write. The SPI raise
         * above is kept for parity with the real FW path (default_cb is
         * a no-op on CA73; the IRQ just gets logged). qinit_stub checks
         * hdma_chr connectivity and no-ops on tests that don't wire it.
         */
        if (val & (1u << REMU_DB_QUEUE_INIT_INTGR1_BIT)) {
            r100_doorbell_qinit_stub(s);
        }
        break;
    case REMU_DB_KIND_ISSR:
        r100_mailbox_set_issr(s->mailbox, issr_idx, val);
        break;
    case REMU_DB_KIND_OTHER:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-doorbell: unexpected frame off=0x%x val=0x%x\n",
                      off, val);
        return;
    }

    r100_doorbell_emit_debug(s, off, val);
}

static void r100_doorbell_receive(void *opaque, const uint8_t *buf, int size)
{
    R100DoorbellState *s = opaque;
    uint32_t off, val;

    while (size > 0) {
        if (remu_frame_rx_feed(&s->rx, &buf, &size, &off, &val)) {
            r100_doorbell_deliver(s, off, val);
        }
    }
}

static void r100_doorbell_cfg_deliver(R100DoorbellState *s,
                                      uint32_t off, uint32_t val)
{
    s->cfg_last_offset = off;
    s->cfg_last_value = val;

    /* Reject anything outside the 4 KB cfg head window or unaligned —
     * host side only forwards the head, this is a safety net. */
    if (off >= REMU_BAR2_CFG_HEAD_SIZE || (off & 0x3u)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-doorbell: cfg frame off=0x%x out of cfg head / "
                      "unaligned\n", off);
        s->cfg_frames_dropped++;
        r100_doorbell_cfg_emit_debug(s, off, val, "bad-offset");
        return;
    }
    s->cfg_shadow[off >> 2] = val;
    s->cfg_frames_received++;
    r100_doorbell_cfg_emit_debug(s, off, val, "ok");
    qemu_log_mask(LOG_TRACE,
                  "r100-doorbell: cfg deliver off=0x%x val=0x%x "
                  "received=%" PRIu64 "\n",
                  off, val, s->cfg_frames_received);
}

static void r100_doorbell_cfg_receive(void *opaque, const uint8_t *buf,
                                      int size)
{
    R100DoorbellState *s = opaque;
    uint32_t off, val;

    while (size > 0) {
        if (remu_frame_rx_feed(&s->cfg_rx, &buf, &size, &off, &val)) {
            r100_doorbell_cfg_deliver(s, off, val);
        }
    }
}

static void r100_doorbell_realize(DeviceState *dev, Error **errp)
{
    R100DoorbellState *s = R100_DOORBELL(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp,
                   "r100-doorbell: 'chardev' property is required");
        return;
    }
    if (s->mailbox == NULL) {
        error_setg(errp,
                   "r100-doorbell: 'mailbox' link property is required");
        return;
    }

    qemu_chr_fe_set_handlers(&s->chr,
                             r100_doorbell_can_receive,
                             r100_doorbell_receive,
                             NULL, NULL, s, NULL, true);
    /* cfg is optional — when unset we still accept doorbell frames but
     * the CM7 QINIT stub will short-circuit on a zeroed DDH_BASE. */
    if (qemu_chr_fe_backend_connected(&s->cfg_chr)) {
        qemu_chr_fe_set_handlers(&s->cfg_chr,
                                 r100_doorbell_cfg_can_receive,
                                 r100_doorbell_cfg_receive,
                                 NULL, NULL, s, NULL, true);
    }
}

static void r100_doorbell_unrealize(DeviceState *dev)
{
    R100DoorbellState *s = R100_DOORBELL(dev);

    qemu_chr_fe_deinit(&s->chr, false);
    qemu_chr_fe_deinit(&s->debug_chr, false);
    qemu_chr_fe_deinit(&s->cfg_chr, false);
    qemu_chr_fe_deinit(&s->cfg_debug_chr, false);
    qemu_chr_fe_deinit(&s->hdma_chr, false);
    qemu_chr_fe_deinit(&s->hdma_debug_chr, false);
}

static void r100_doorbell_reset(DeviceState *dev)
{
    R100DoorbellState *s = R100_DOORBELL(dev);

    remu_frame_rx_reset(&s->rx);
    remu_frame_rx_reset(&s->cfg_rx);
    /* frames_received / last_* / cfg_shadow kept across reset so tests
     * can assert prior-run state. */
}

static const VMStateDescription r100_doorbell_vmstate = {
    .name = "r100-doorbell",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(rx.len, R100DoorbellState),
        VMSTATE_UINT8_ARRAY(rx.buf, R100DoorbellState, REMU_FRAME_SIZE),
        VMSTATE_UINT32(cfg_rx.len, R100DoorbellState),
        VMSTATE_UINT8_ARRAY(cfg_rx.buf, R100DoorbellState, REMU_FRAME_SIZE),
        VMSTATE_UINT64(frames_received, R100DoorbellState),
        VMSTATE_UINT32(last_offset, R100DoorbellState),
        VMSTATE_UINT32(last_value, R100DoorbellState),
        VMSTATE_UINT32_ARRAY(cfg_shadow, R100DoorbellState,
                             R100_CFG_SHADOW_COUNT),
        VMSTATE_UINT64(cfg_frames_received, R100DoorbellState),
        VMSTATE_UINT64(cfg_frames_dropped, R100DoorbellState),
        VMSTATE_UINT32(cfg_last_offset, R100DoorbellState),
        VMSTATE_UINT32(cfg_last_value, R100DoorbellState),
        VMSTATE_UINT64(hdma_frames_sent, R100DoorbellState),
        VMSTATE_UINT64(hdma_frames_dropped, R100DoorbellState),
        VMSTATE_UINT64(qinit_stubs_fired, R100DoorbellState),
        VMSTATE_UINT64(qinit_stubs_dropped, R100DoorbellState),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_doorbell_properties[] = {
    DEFINE_PROP_CHR("chardev", R100DoorbellState, chr),
    DEFINE_PROP_CHR("debug-chardev", R100DoorbellState, debug_chr),
    DEFINE_PROP_CHR("cfg-chardev", R100DoorbellState, cfg_chr),
    DEFINE_PROP_CHR("cfg-debug-chardev", R100DoorbellState, cfg_debug_chr),
    DEFINE_PROP_CHR("hdma-chardev", R100DoorbellState, hdma_chr),
    DEFINE_PROP_CHR("hdma-debug-chardev", R100DoorbellState, hdma_debug_chr),
    DEFINE_PROP_LINK("mailbox", R100DoorbellState, mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_LINK("pf-mailbox", R100DoorbellState, pf_mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_doorbell_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 PCIe doorbell ingress (host BAR4 → mailbox INTGR)";
    dc->realize = r100_doorbell_realize;
    dc->unrealize = r100_doorbell_unrealize;
    dc->vmsd = &r100_doorbell_vmstate;
    device_class_set_legacy_reset(dc, r100_doorbell_reset);
    device_class_set_props(dc, r100_doorbell_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    /* Machine-instantiated only (mailbox link is topology). */
    dc->user_creatable = false;
}

static const TypeInfo r100_doorbell_info = {
    .name          = TYPE_R100_DOORBELL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100DoorbellState),
    .class_init    = r100_doorbell_class_init,
};

static void r100_doorbell_register_types(void)
{
    type_register_static(&r100_doorbell_info);
}

type_init(r100_doorbell_register_types)
