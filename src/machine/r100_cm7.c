/*
 * R100 NPU-side PCIe CM7 stub — host<->NPU bridge terminator.
 *
 * Silicon: the PCIE_CM7 subcontroller lives inside the PCIe IP and
 * runs its own FW that mediates SOFT_RESET, ingests BAR2 cfg-head
 * stores, and forwards host doorbells. REMU has no Cortex-M7 vCPU,
 * so this device terminates the matching cross-process chardev
 * channels from the host QEMU and synthesises just enough of the
 * same effects that the stock kmd thinks it's talking to real CM7
 * FW. BD walk, CB dispatch, BD.DONE writeback, and MSI-X live in
 * q-cp on CA73 CP0 — this device does not touch them.
 *
 * Streams terminated here (see BAR4/BAR2 tables in CLAUDE.md):
 *
 *   doorbell chardev  (host→NPU, 8-byte (off, val) frames — remu_frame.h)
 *     0x08 INTGR0   — M8b 3a CM7 stub: SOFT_RESET bit 0 synthesises
 *                     FW_BOOT_DONE into PF.ISSR[4] for kmd's
 *                     post-probe re-handshake (cold boot is real,
 *                     see docs/debugging.md). Other bits relay to
 *                     VF0.INTGR1 for parity with silicon.
 *     0x1c INTGR1   — M6 host→NPU IRQ (r100_mailbox_raise_intgr →
 *                     SPI 185). Raised verbatim — q-cp on CA73 CP0
 *                     wakes hq_task on the queue-doorbell bits and
 *                     handles QUEUE_INIT itself. No CM7-side
 *                     special-case dispatch.
 *     0x80..0x180   — M8a ISSR payload (r100_mailbox_set_issr).
 *
 *   cfg chardev      (host→NPU, 8-byte (cfg_off, val) frames)
 *                   BAR2 cfg-head writes ingress here and update
 *                   cfg_shadow[]. The 4 KB MMIO trap installed at
 *                   R100_DEVICE_COMM_SPACE_BASE serves NPU reads
 *                   from the same shadow (P1b inbound iATU model)
 *                   and forwards NPU writes back upstream over the
 *                   hdma chardev as OP_CFG_WRITE so the host's
 *                   r100-npu-pci cfg_mmio_regs[] stays in sync.
 *
 * The hdma chardev itself is owned by `r100-hdma` since M9-1c
 * (CharBackend is single-frontend). cm7 reaches it through a QOM
 * link and uses the public emit API in src/machine/r100_hdma.h
 * for the OP_CFG_WRITE upstream forward; everything else goes
 * through r100-pcie-outbound (P1a) or q-cp directly.
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
#include "exec/address-spaces.h"
#include "migration/vmstate.h"
#include "r100_soc.h"
#include "r100_hdma.h"
#include "remu_frame.h"
#include "remu_doorbell_proto.h"
#include "remu_hdma_proto.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100Cm7State, R100_CM7)

/* Number of u32 slots in the BAR2 cfg-head shadow (4 KB / 4). */
#define R100_CFG_SHADOW_COUNT (REMU_BAR2_CFG_HEAD_SIZE / 4u)

/* FW_BOOT_DONE value written into PF.ISSR[4] by the CM7 stub on
 * SOFT_RESET (M8b 3a re-handshake; cold boot travels the real path
 * via q-sys' bootdone_task). */
#define REMU_FW_BOOT_DONE 0xFB0D

struct R100Cm7State {
    SysBusDevice parent_obj;

    CharBackend chr;              /* doorbell ingress (host→NPU) */
    CharBackend debug_chr;        /* doorbell ASCII tail */
    CharBackend cfg_chr;          /* cfg ingress (host→NPU) */
    CharBackend cfg_debug_chr;    /* cfg ASCII tail */

    /*
     * P1b reverse-mirror trap: 4 KB MMIO overlay on chiplet-0 DRAM at
     * R100_DEVICE_COMM_SPACE_BASE (0x10200000). Models silicon's
     * inbound iATU that maps host BAR2 cfg-head onto NPU local memory
     * — host writes (cfg chardev) and NPU writes (q-cp / q-sys MMIO)
     * both terminate in cfg_shadow, and NPU writes additionally push
     * an OP_CFG_WRITE upstream so the kmd's BAR2 cfg-head shadow stays
     * in sync (FUNC_SCRATCH completion path: q-cp's
     * `cb_complete → writel(scratchpad)` round-trips back into
     * `rebel_cfg_read(FUNC_SCRATCH)` on the host without an explicit
     * DMA transaction).
     */
    MemoryRegion cfg_mirror_mmio;

    R100MailboxState *mailbox;      /* VF0: M6 INTGR sink + M8a ISSR sink */
    R100MailboxState *pf_mailbox;   /* PF: CM7-stub FW_BOOT_DONE source */
    R100HDMAState    *hdma;         /* M9-1c: shared chardev owner */

    /* Chardev byte stream may split a frame across callbacks. */
    RemuFrameRx rx;
    RemuFrameRx cfg_rx;

    /* Doorbell-stream counters. */
    uint64_t frames_received;
    uint32_t last_offset;
    uint32_t last_value;

    /* Cfg-shadow mirror of host BAR2 cfg head (4 KB worth of u32s).
     * Host-initiated writes land here via the cfg chardev; NPU writes
     * (P1b) update the same shadow and forward as OP_CFG_WRITE. */
    uint32_t cfg_shadow[R100_CFG_SHADOW_COUNT];
    uint64_t cfg_frames_received;
    uint64_t cfg_frames_dropped;
    uint32_t cfg_last_offset;
    uint32_t cfg_last_value;

    /* HDMA egress counters for the cfg-mirror reverse path. */
    uint64_t hdma_frames_sent;
    uint64_t hdma_frames_dropped;
};

/* ------------------------------------------------------------------ */
/* Debug-tail helpers                                                  */
/* ------------------------------------------------------------------ */

static void r100_cm7_emit_debug(R100Cm7State *s,
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

static void r100_cm7_cfg_emit_debug(R100Cm7State *s,
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

/* ------------------------------------------------------------------ */
/* Doorbell (host→NPU) frame ingress                                   */
/* ------------------------------------------------------------------ */

static int r100_cm7_can_receive(void *opaque)
{
    R100Cm7State *s = opaque;
    return remu_frame_rx_headroom(&s->rx);
}

static int r100_cm7_cfg_can_receive(void *opaque)
{
    R100Cm7State *s = opaque;
    return remu_frame_rx_headroom(&s->cfg_rx);
}

static void r100_cm7_deliver(R100Cm7State *s, uint32_t off, uint32_t val)
{
    uint32_t issr_idx = 0;
    RemuDoorbellKind kind = remu_doorbell_classify(off, &issr_idx);

    s->frames_received++;
    s->last_offset = off;
    s->last_value = val;

    qemu_log_mask(LOG_TRACE,
                  "r100-cm7: deliver off=0x%x val=0x%x count=%" PRIu64 "\n",
                  off, val, s->frames_received);

    switch (kind) {
    case REMU_DB_KIND_INTGR0:
        /*
         * CA73 soft-reset stub — narrowly scoped to INTGR0 bit 0.
         *
         * Cold boot is real (PF.ISSR[4]=0xFB0D flows through q-sys'
         * bootdone_task → r100-mailbox → `issr` chardev → host BAR4
         * shadow). What this stubs is the *post-soft-reset re-handshake*:
         * kmd's rebel_hw_init(RBLN_RESET_FIRST) rings INTGR0 bit 0 and
         * polls ISSR[4] for a fresh 0xFB0D. Silicon reboots the CA73s
         * through PCIE_CM7 → pcie_soft_reset_handler; REMU doesn't
         * model that (see docs/roadmap.md → P8), so we synthesise only
         * the observable endpoint. Running firmware is undisturbed.
         *
         * Other INTGR0 bits fall through to VF0.INTGR1 so q-sys'
         * IDX_MAILBOX_PCIE_VF0 default_cb sees them (no-op today,
         * kept for parity + future subscribers).
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
        /*
         * Plain SPI 185 raise. q-cp on CA73 CP0 owns every interesting
         * bit on this register: queue-doorbell bits wake hq_task →
         * cb_task → cb_complete (full BD lifecycle), and bit 7
         * (REBEL_DOORBELL_QUEUE_INIT) similarly wakes the
         * hq_init / rl_cq_init path that publishes the kmd's QINIT
         * descriptor and writes back init_done=1 over the P1a outbound
         * iATU. No CM7-side special-case dispatch.
         */
        r100_mailbox_raise_intgr(s->mailbox, 1, val);
        break;
    case REMU_DB_KIND_ISSR:
        /* P1b: kmd's BAR4 ISSR-payload writes (MAILBOX_BASE + idx*4)
         * land on PF.ISSR on silicon, and q-cp's
         * `rl_get_mailbox(PCIE_PF=0, idx)` reads from PF (because
         * `inst[PCIE_PF] = IDX_MAILBOX_PCIE_PF` in q-cp's mailbox
         * driver). Routing the relay to VF0 left URG/CQ/MQ payloads
         * stranded — the FW saw 0 and aborted on a "force abort"
         * URG_EVENT_FORCE_ABORT (event=0). PF is the authoritative
         * backing store; the IRQ side stays on VF0 (INTGR1 → SPI
         * 185) because that's what fires on CA73, exactly mirroring
         * silicon's CM7→VF0 IRQ-only relay.
         *
         * Mirror to VF0 too so any pre-existing reader of VF0.ISSR
         * (M8a tests, parity) keeps working — the egress chardev is
         * deliberately PF-only so the host-side BAR4 shadow doesn't
         * see two writes per kmd store. */
        if (s->pf_mailbox) {
            r100_mailbox_set_issr(s->pf_mailbox, issr_idx, val);
        }
        r100_mailbox_set_issr(s->mailbox, issr_idx, val);
        break;
    case REMU_DB_KIND_OTHER:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cm7: unexpected frame off=0x%x val=0x%x\n",
                      off, val);
        return;
    }

    r100_cm7_emit_debug(s, off, val);
}

static void r100_cm7_receive(void *opaque, const uint8_t *buf, int size)
{
    R100Cm7State *s = opaque;
    uint32_t off, val;

    while (size > 0) {
        if (remu_frame_rx_feed(&s->rx, &buf, &size, &off, &val)) {
            r100_cm7_deliver(s, off, val);
        }
    }
}

/* ------------------------------------------------------------------ */
/* cfg chardev ingress (host→NPU BAR2 cfg-head shadow)                 */
/* ------------------------------------------------------------------ */

static void r100_cm7_cfg_deliver(R100Cm7State *s, uint32_t off, uint32_t val)
{
    s->cfg_last_offset = off;
    s->cfg_last_value = val;

    /* Reject anything outside the 4 KB cfg head window or unaligned —
     * host side only forwards the head, this is a safety net. */
    if (off >= REMU_BAR2_CFG_HEAD_SIZE || (off & 0x3u)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cm7: cfg frame off=0x%x out of cfg head / "
                      "unaligned\n", off);
        s->cfg_frames_dropped++;
        r100_cm7_cfg_emit_debug(s, off, val, "bad-offset");
        return;
    }
    /*
     * Single source of truth: cfg_shadow. The 4 KB MMIO trap installed
     * at R100_DEVICE_COMM_SPACE_BASE (see r100_cm7_realize) overlays
     * chiplet-0 DRAM, so NPU reads of e.g. DDH_BASE_{LO,HI} land in
     * cfg_mirror_read which returns the slot below — there is no
     * separate DRAM mirror to keep in sync. NPU-side writes round-trip
     * back to the host's cfg_mmio_regs through the same trap
     * (OP_CFG_WRITE), closing the loop without a feedback path here.
     */
    s->cfg_shadow[off >> 2] = val;
    s->cfg_frames_received++;
    r100_cm7_cfg_emit_debug(s, off, val, "ok");
    qemu_log_mask(LOG_TRACE,
                  "r100-cm7: cfg deliver off=0x%x val=0x%x received=%"
                  PRIu64 "\n", off, val, s->cfg_frames_received);
}

static void r100_cm7_cfg_receive(void *opaque, const uint8_t *buf, int size)
{
    R100Cm7State *s = opaque;
    uint32_t off, val;

    while (size > 0) {
        if (remu_frame_rx_feed(&s->cfg_rx, &buf, &size, &off, &val)) {
            r100_cm7_cfg_deliver(s, off, val);
        }
    }
}

/* ------------------------------------------------------------------ */
/* P1b — DEVICE_COMMUNICATION_SPACE reverse-mirror trap                */
/*                                                                     */
/* Overlay at NPU PA R100_DEVICE_COMM_SPACE_BASE (0x10200000, 4 KB)    */
/* installed in r100_cm7_realize at priority 10 over chiplet-0 DRAM    */
/* (priority 0). Reads from any NPU CPU return the cfg_shadow slot;    */
/* writes update cfg_shadow + emit an OP_CFG_WRITE on the hdma         */
/* chardev so the host's r100-npu-pci cfg-head shadow stays in sync.   */
/* ------------------------------------------------------------------ */

/*
 * The 4 KB cfg head sees a much wider mix of access widths than the
 * 8-byte (cfg_off, val) wire frame suggests:
 *
 *   - q-sys main() on CP0 does
 *       memset(DEVICE_COMMUNICATION_SPACE_BASE, 0, CP1_LOGBUF_MAGIC)
 *     on cold boot (osl/FreeRTOS/Source/main.c:250). The compiler
 *     lowers this to a mix of `dc zva` cache-line zeros and 1/8-byte
 *     stores depending on the MMU attributes the FW has installed
 *     for this region.
 *   - hil_init_descs reads DDH_BASE_LO/HI as a 64-bit FUNC_READQ.
 *   - cb_complete writes FUNC_SCRATCH as a 32-bit `writel`.
 *
 * Set `impl` to the 32-bit slot stride so the read/write helpers
 * stay simple, and let QEMU's accepts_io_with_size() machinery pack /
 * unpack wider or narrower accesses on top. `valid.unaligned = true`
 * isn't strictly needed — every q-sys / q-cp access is naturally
 * aligned — but it costs nothing and matches the silicon semantics
 * (the inbound iATU has no alignment trap).
 */
static uint64_t r100_cm7_cfg_mirror_read(void *opaque, hwaddr off,
                                         unsigned size)
{
    R100Cm7State *s = opaque;
    uint32_t v32;

    if (off + size > REMU_BAR2_CFG_HEAD_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cm7: cfg-mirror read off=0x%" HWADDR_PRIx
                      " size=%u out of bounds (0..0x%x)\n",
                      off, size, REMU_BAR2_CFG_HEAD_SIZE);
        return 0;
    }
    v32 = s->cfg_shadow[off >> 2];
    qemu_log_mask(LOG_TRACE,
                  "r100-cm7: cfg-mirror read off=0x%" HWADDR_PRIx
                  " size=%u val=0x%x\n", off, size, v32);
    return v32;
}

static void r100_cm7_cfg_mirror_write(void *opaque, hwaddr off,
                                      uint64_t val, unsigned size)
{
    R100Cm7State *s = opaque;
    uint32_t v32;

    if (off + size > REMU_BAR2_CFG_HEAD_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cm7: cfg-mirror write off=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64
                      " out of bounds (0..0x%x)\n",
                      off, size, val, REMU_BAR2_CFG_HEAD_SIZE);
        return;
    }
    v32 = (uint32_t)val;
    s->cfg_shadow[off >> 2] = v32;
    qemu_log_mask(LOG_TRACE,
                  "r100-cm7: cfg-mirror write off=0x%" HWADDR_PRIx
                  " size=%u val=0x%x\n", off, size, v32);
    /*
     * Forward to host BAR2 cfg-head shadow over the hdma chardev.
     * The kmd reads FUNC_SCRATCH (and other DDH fields) via
     * `rebel_cfg_read`, which dereferences `cfg_mmio_regs[idx]` on
     * the host side. The OP_CFG_WRITE handler in r100-npu-pci
     * stores the value there with no further egress, so the loop
     * terminates: NPU-write → cfg_shadow + hdma → host cfg_mmio_regs.
     *
     * req_id is unused on the host side for OP_CFG_WRITE (the dst
     * field carries the cfg offset), so use 0 — we don't need to
     * disambiguate concurrent in-flight requests.
     */
    if (s->hdma) {
        if (r100_hdma_emit_cfg_write(s->hdma, 0u, (uint32_t)(off & ~0x3u),
                                     v32, "cfg-mirror")) {
            s->hdma_frames_sent++;
        } else {
            s->hdma_frames_dropped++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-cm7: cfg-mirror write off=0x%" HWADDR_PRIx
                          " HDMA emit failed\n", off);
        }
    }
}

static const MemoryRegionOps r100_cm7_cfg_mirror_ops = {
    .read       = r100_cm7_cfg_mirror_read,
    .write      = r100_cm7_cfg_mirror_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid      = { .min_access_size = 1, .max_access_size = 8,
                    .unaligned = true },
    .impl       = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Realize / reset / vmstate / properties                              */
/* ------------------------------------------------------------------ */

static void r100_cm7_realize(DeviceState *dev, Error **errp)
{
    R100Cm7State *s = R100_CM7(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp,
                   "r100-cm7: 'chardev' property is required");
        return;
    }
    if (s->mailbox == NULL) {
        error_setg(errp,
                   "r100-cm7: 'mailbox' link property is required");
        return;
    }

    qemu_chr_fe_set_handlers(&s->chr,
                             r100_cm7_can_receive,
                             r100_cm7_receive,
                             NULL, NULL, s, NULL, true);
    /* cfg is optional — when unset the cfg-mirror trap still installs
     * but reads always return zero (no kmd has published DDH_BASE) and
     * NPU writes simply update the local shadow. The OP_CFG_WRITE
     * upstream is similarly a no-op when the hdma link is unbound. */
    if (qemu_chr_fe_backend_connected(&s->cfg_chr)) {
        qemu_chr_fe_set_handlers(&s->cfg_chr,
                                 r100_cm7_cfg_can_receive,
                                 r100_cm7_cfg_receive,
                                 NULL, NULL, s, NULL, true);
    }

    /*
     * P1b reverse-mirror trap: install the 4 KB cfg-shadow MMIO over
     * chiplet-0 DRAM at R100_DEVICE_COMM_SPACE_BASE. Priority 10 is
     * higher than the bare DRAM (priority 0), so every NPU access in
     * this window goes through cfg_mirror_ops instead of leaking into
     * the underlying RAM region. Always installed when cm7 is
     * created (i.e. dual-QEMU --host runs); single-QEMU configs don't
     * instantiate cm7 at all so the trap is implicitly absent there.
     */
    memory_region_init_io(&s->cfg_mirror_mmio, OBJECT(dev),
                          &r100_cm7_cfg_mirror_ops, s,
                          "r100.cm7.cfg-mirror",
                          R100_DEVICE_COMM_SPACE_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        R100_DEVICE_COMM_SPACE_BASE,
                                        &s->cfg_mirror_mmio, 10);
}

static void r100_cm7_unrealize(DeviceState *dev)
{
    R100Cm7State *s = R100_CM7(dev);

    qemu_chr_fe_deinit(&s->chr, false);
    qemu_chr_fe_deinit(&s->debug_chr, false);
    qemu_chr_fe_deinit(&s->cfg_chr, false);
    qemu_chr_fe_deinit(&s->cfg_debug_chr, false);
}

static void r100_cm7_reset(DeviceState *dev)
{
    R100Cm7State *s = R100_CM7(dev);

    remu_frame_rx_reset(&s->rx);
    remu_frame_rx_reset(&s->cfg_rx);
}

static const VMStateDescription r100_cm7_vmstate = {
    .name = "r100-cm7",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(rx.len, R100Cm7State),
        VMSTATE_UINT8_ARRAY(rx.buf, R100Cm7State, REMU_FRAME_SIZE),
        VMSTATE_UINT32(cfg_rx.len, R100Cm7State),
        VMSTATE_UINT8_ARRAY(cfg_rx.buf, R100Cm7State, REMU_FRAME_SIZE),
        VMSTATE_UINT64(frames_received, R100Cm7State),
        VMSTATE_UINT32(last_offset, R100Cm7State),
        VMSTATE_UINT32(last_value, R100Cm7State),
        VMSTATE_UINT32_ARRAY(cfg_shadow, R100Cm7State,
                             R100_CFG_SHADOW_COUNT),
        VMSTATE_UINT64(cfg_frames_received, R100Cm7State),
        VMSTATE_UINT64(cfg_frames_dropped, R100Cm7State),
        VMSTATE_UINT32(cfg_last_offset, R100Cm7State),
        VMSTATE_UINT32(cfg_last_value, R100Cm7State),
        VMSTATE_UINT64(hdma_frames_sent, R100Cm7State),
        VMSTATE_UINT64(hdma_frames_dropped, R100Cm7State),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_cm7_properties[] = {
    DEFINE_PROP_CHR("chardev", R100Cm7State, chr),
    DEFINE_PROP_CHR("debug-chardev", R100Cm7State, debug_chr),
    DEFINE_PROP_CHR("cfg-chardev", R100Cm7State, cfg_chr),
    DEFINE_PROP_CHR("cfg-debug-chardev", R100Cm7State, cfg_debug_chr),
    DEFINE_PROP_LINK("mailbox", R100Cm7State, mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_LINK("pf-mailbox", R100Cm7State, pf_mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_LINK("hdma", R100Cm7State, hdma,
                     TYPE_R100_HDMA, R100HDMAState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_cm7_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 PCIe CM7 stub (doorbell ingress + cfg-mirror trap)";
    dc->realize = r100_cm7_realize;
    dc->unrealize = r100_cm7_unrealize;
    dc->vmsd = &r100_cm7_vmstate;
    device_class_set_legacy_reset(dc, r100_cm7_reset);
    device_class_set_props(dc, r100_cm7_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    /* Machine-instantiated only (mailbox links are topology). */
    dc->user_creatable = false;
}

static const TypeInfo r100_cm7_info = {
    .name          = TYPE_R100_CM7,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100Cm7State),
    .class_init    = r100_cm7_class_init,
};

static void r100_cm7_register_types(void)
{
    type_register_static(&r100_cm7_info);
}

type_init(r100_cm7_register_types)
