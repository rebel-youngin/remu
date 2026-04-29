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
 *                     see docs/debugging.md). Gated on the
 *                     `fw_boot_done_seen` latch on PF (set by q-sys's
 *                     own `bootdone_task` write); pre-cold-boot
 *                     SOFT_RESETs are dropped so the kmd parks on
 *                     its FW_BOOT_DONE poll until q-sys's main.c:250
 *                     DCS memset has run — Side bug 2 fix. Other
 *                     bits relay to VF0.INTGR1 for parity with silicon.
 *     0x1c INTGR1   — M6 host→NPU IRQ (r100_mailbox_raise_intgr →
 *                     SPI 185). Raised verbatim — q-cp on CA73 CP0
 *                     wakes hq_task on the queue-doorbell bits and
 *                     handles QUEUE_INIT itself. No CM7-side
 *                     special-case dispatch.
 *     0x80..0x180   — M8a ISSR payload (r100_mailbox_set_issr).
 *
 * BAR2 cfg-head propagation lives off-device entirely: when the
 * `cfg-shadow` link is wired (i.e. dual-QEMU `--host` mode), the
 * 4 KB window at R100_DEVICE_COMM_SPACE_BASE is realised as a
 * memory_region_init_alias over a shared `cfg-shadow`
 * memory-backend-file (P10-fix). The host's r100-npu-pci aliases the
 * same backend over its BAR2 cfg-head subregion, so kmd writes to
 * FUNC_SCRATCH / DDH_BASE_LO are visible on q-cp's next read with no
 * chardev queue and no OP_CFG_WRITE round trip. NPU-only tests that
 * don't drive the cfg path (m6 / m8 doorbell+ISSR bridges) leave the
 * link unset; the alias is skipped and chiplet-0 lazy RAM services
 * any incidental access at that window — same as pre-P1b silicon.
 * The pre-P10-fix `cfg` chardev path
 * (8-byte frames + R100_CFG_SHADOW_COUNT u32 array + io-ops trap +
 * OP_CFG_WRITE reverse-emit on hdma) was retired alongside the
 * outbound iATU chardev fallback because the alias has been the only
 * path used by the regression suite for several builds; check
 * `git log src/machine/r100_cm7.c` for the historical implementation
 * if a chardev-style reproducer is needed.
 *
 * The hdma chardev itself is owned by `r100-hdma` since M9-1c
 * (CharBackend is single-frontend). cm7 no longer reaches it after
 * the cfg reverse-path retirement; the link prop is gone and the
 * UMQ multi-queue scaffolding can reclaim req_id 0x00.
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
#include "remu_frame.h"
#include "remu_doorbell_proto.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100Cm7State, R100_CM7)

/* FW_BOOT_DONE magic now lives in r100_mailbox.h (R100_FW_BOOT_DONE) —
 * shared with the mailbox's `fw_boot_done_seen` latch. */

struct R100Cm7State {
    SysBusDevice parent_obj;

    CharBackend chr;              /* doorbell ingress (host→NPU) */
    CharBackend debug_chr;        /* doorbell ASCII tail */

    /*
     * P10-fix: 4 KB MMIO alias overlay on chiplet-0 DRAM at
     * R100_DEVICE_COMM_SPACE_BASE (0x10200000) over the shared
     * `cfg-shadow` memory-backend-file. The host's r100-npu-pci
     * aliases the same backend over its BAR2 cfg-head subregion,
     * so kmd writes to FUNC_SCRATCH / DDH_BASE_LO are visible on
     * q-cp's next read with no chardev queue. Replaces the prior
     * io-ops trap + cfg-chardev RX + OP_CFG_WRITE reverse-emit.
     */
    MemoryRegion cfg_mirror_mmio;
    MemoryRegion *cfg_shadow_mr;

    R100MailboxState *mailbox;      /* VF0: M6 INTGR sink + M8a ISSR sink */
    R100MailboxState *pf_mailbox;   /* PF: CM7-stub FW_BOOT_DONE source */

    /* Chardev byte stream may split a frame across callbacks. */
    RemuFrameRx rx;

    /* Doorbell-stream counters. */
    uint64_t frames_received;
    uint32_t last_offset;
    uint32_t last_value;
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

/* ------------------------------------------------------------------ */
/* Doorbell (host→NPU) frame ingress                                   */
/* ------------------------------------------------------------------ */

static int r100_cm7_can_receive(void *opaque)
{
    R100Cm7State *s = opaque;
    return remu_frame_rx_headroom(&s->rx);
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
         * Side bug 2 fix: the synthesis is gated on PF's
         * `fw_boot_done_seen` latch (set the first time q-sys's
         * `bootdone_task` writes 0xFB0D into PF.ISSR[4] from the
         * NPU-MMIO source). Without the gate, kmd's RBLN_RESET_FIRST
         * SOFT_RESET on early probe can race ahead of q-sys's
         * `main.c:250` DCS memset — kmd sees the synthetic 0xFB0D,
         * writes DDH_BASE_LO into the shared cfg-shadow shm, and q-sys
         * later zeroes offsets 0..0x103F (incl. DDH_BASE_LO at 0xC0),
         * causing q-cp's `hil_init_descs` to NULL-deref ~30% of runs.
         * With the gate, a pre-cold-boot SOFT_RESET is silently
         * dropped; the kmd parks on its own FW_BOOT_DONE poll
         * (`readx_poll_timeout` for `val != 0`) until q-sys publishes
         * naturally over the existing `issr` chardev egress, which is
         * provably after the memset (bootdone_task only runs after
         * `cp_create_tasks()` returns inside main()). All subsequent
         * SOFT_RESETs find the latch already set and synthesise as
         * before.
         *
         * Other INTGR0 bits fall through to VF0.INTGR1 so q-sys'
         * IDX_MAILBOX_PCIE_VF0 default_cb sees them (no-op today,
         * kept for parity + future subscribers).
         */
        if (val & 0x1U) {
            if (r100_mailbox_fw_boot_done_seen(s->pf_mailbox)) {
                r100_mailbox_cm7_stub_write_issr(s->pf_mailbox, 4,
                                                 R100_FW_BOOT_DONE);
            } else {
                qemu_log_mask(LOG_TRACE,
                              "r100-cm7: SOFT_RESET dropped — q-sys "
                              "cold-boot FW_BOOT_DONE not yet observed; "
                              "kmd will park on PF.ISSR[4] poll until "
                              "the natural publish arrives\n");
            }
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

    /*
     * Install a 4 KB MemoryRegion alias over the shared `cfg-shadow`
     * backend at R100_DEVICE_COMM_SPACE_BASE (priority 10 over the
     * priority-0 chiplet-0 DRAM, so NPU CPU accesses in this window
     * land here instead of leaking into bare RAM). The host x86
     * QEMU's r100-npu-pci aliases the same backend over its BAR2
     * cfg-head subregion (REMU_BAR2_CFG_HEAD_OFF), so kmd writes to
     * FUNC_SCRATCH / DDH_BASE_LO are observable on q-cp's next read
     * with no chardev queue. Optional — single-QEMU NPU-only tests
     * (m6 / m8) skip the alias and let chiplet-0 lazy RAM service
     * any incidental access at the window, matching pre-P1b silicon.
     */
    if (s->cfg_shadow_mr) {
        uint64_t alias_size =
            MIN((uint64_t)memory_region_size(s->cfg_shadow_mr),
                (uint64_t)R100_DEVICE_COMM_SPACE_SIZE);
        memory_region_init_alias(&s->cfg_mirror_mmio, OBJECT(dev),
                                 "r100.cm7.cfg-mirror.alias",
                                 s->cfg_shadow_mr, 0, alias_size);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            R100_DEVICE_COMM_SPACE_BASE,
                                            &s->cfg_mirror_mmio, 10);
    }
}

static void r100_cm7_unrealize(DeviceState *dev)
{
    R100Cm7State *s = R100_CM7(dev);

    qemu_chr_fe_deinit(&s->chr, false);
    qemu_chr_fe_deinit(&s->debug_chr, false);
}

static void r100_cm7_reset(DeviceState *dev)
{
    R100Cm7State *s = R100_CM7(dev);

    remu_frame_rx_reset(&s->rx);
}

static const VMStateDescription r100_cm7_vmstate = {
    .name = "r100-cm7",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(rx.len, R100Cm7State),
        VMSTATE_UINT8_ARRAY(rx.buf, R100Cm7State, REMU_FRAME_SIZE),
        VMSTATE_UINT64(frames_received, R100Cm7State),
        VMSTATE_UINT32(last_offset, R100Cm7State),
        VMSTATE_UINT32(last_value, R100Cm7State),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_cm7_properties[] = {
    DEFINE_PROP_CHR("chardev", R100Cm7State, chr),
    DEFINE_PROP_CHR("debug-chardev", R100Cm7State, debug_chr),
    DEFINE_PROP_LINK("mailbox", R100Cm7State, mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_LINK("pf-mailbox", R100Cm7State, pf_mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_LINK("cfg-shadow", R100Cm7State, cfg_shadow_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
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
