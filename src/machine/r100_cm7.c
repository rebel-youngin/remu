/*
 * R100 NPU-side PCIe CM7 stub — host<->NPU bridge terminator.
 *
 * Silicon: the PCIE_CM7 subcontroller lives inside the PCIe IP and
 * runs its own FW that (a) mediates SOFT_RESET, (b) ingests BAR2
 * cfg-head stores, (c) responds to the QUEUE_INIT doorbell by DMA'ing
 * the kmd's rbln_device_desc and writing back init_done=1, (d)
 * watches command-queue doorbells, walks BDs, executes the PACKET
 * stream and marks BDs done + rings MSI-X. REMU models none of that
 * FW; instead one QEMU device terminates every cross-process channel
 * from the host QEMU and synthesises just enough of the same effects
 * that the stock kmd thinks it's talking to real CM7 FW.
 *
 * Name history: this file was `r100_doorbell.c` through M8b Stage 3b —
 * "doorbell" was the first responsibility (M6 INTGR ingress). By
 * Stage 3c it owns four CM7 behaviours (SOFT_RESET, cfg-shadow, QINIT,
 * BD-done) and only one of them is a doorbell in the MMIO-register
 * sense. The device was renamed r100-cm7 to match what it actually
 * emulates; the top-level `-machine r100-soc,doorbell=<id>` knob
 * keeps the old name for backward compat.
 *
 * Streams terminated here (see BAR4/BAR2 tables in CLAUDE.md):
 *
 *   doorbell chardev  (host→NPU, 8-byte (off, val) frames — remu_frame.h)
 *     0x08 INTGR0   — M8b 3a CM7 stub: SOFT_RESET bit 0 synthesises
 *                     FW_BOOT_DONE into PF.ISSR[4]; other bits relay
 *                     to VF0.INTGR1 for parity with silicon.
 *     0x1c INTGR1   — M6 host→NPU IRQ (r100_mailbox_raise_intgr → SPI).
 *                     Plus CM7-special bits:
 *                       bit 0..(R100_CM7_MAX_QUEUES-1) — cmd-queue
 *                         doorbell: drives the BD-done state machine
 *                         (M8b 3c, this file).
 *                       bit 7 — REBEL_DOORBELL_QUEUE_INIT: QINIT stub
 *                         (M8b 3b).
 *     0x80..0x180   — M8a ISSR payload (r100_mailbox_set_issr).
 *
 *   cfg chardev      (host→NPU, 8-byte (cfg_off, val) frames)
 *                   BAR2 cfg-head writes land in cfg_shadow[].
 *                   DDH_BASE_{LO,HI} at +0xC0/+0xC4 anchor every
 *                   HDMA transfer below.
 *
 *   hdma chardev     (NPU<->host, variable-length frames — remu_hdma_proto.h)
 *                   Owned by `r100-hdma` since M9-1c; r100-cm7 reaches
 *                   it via a QOM link and the public API in
 *                   src/machine/r100_hdma.h. r100-hdma routes incoming
 *                   OP_READ_RESPs by req_id partition: cm7's BD-done
 *                   responses (req_id 1..R100_CM7_MAX_QUEUES) come back
 *                   through r100_hdma_set_cm7_callback(). cm7 still
 *                   drives Egress for QINIT (untagged req_id=0),
 *                   BD-done OP_WRITE / OP_READ_REQ / OP_CFG_WRITE
 *                   (req_id = qid + 1) — the helpers just call into
 *                   r100-hdma instead of touching the chardev directly.
 *
 * Each stream has an optional debug-chardev tail; additionally the
 * BD-done state machine gets its own cm7-debug-chardev for one ASCII
 * line per phase transition. The hdma stream's debug tail moved to
 * r100-hdma along with its CharBackend. All machine-instantiated via
 * `-machine r100-soc,doorbell=<id>,cfg=<id>,hdma=<id>,cm7-debug=<id>`.
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
#include "r100_imsix.h"
#include "r100_hdma.h"
#include "remu_frame.h"
#include "remu_doorbell_proto.h"
#include "remu_hdma_proto.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100Cm7State, R100_CM7)

/* Number of u32 slots in the BAR2 cfg-head shadow (4 KB / 4). */
#define R100_CFG_SHADOW_COUNT (REMU_BAR2_CFG_HEAD_SIZE / 4u)

/* Command-queue count (matches NUMBER_OF_CMD_QUEUES in kmd/rebel.h and
 * QUEUE_DESC_COUNT — the ring queue is qid 1 but only the primary
 * cmd queue needs BD-done emulation for rbln_queue_test). Keep at 2
 * so bit 1 still decodes as a queue doorbell if a future test exercises
 * it. */
#define R100_CM7_MAX_QUEUES 2u

/* BD-job state — one per command queue. Driven by INTGR1 bit `qid`
 * on the host side; the pipeline issues HDMA READ_REQs and waits for
 * the matching OP_READ_RESP frames to come back on the `hdma` chardev. */
typedef enum {
    CM7_BD_IDLE = 0,
    CM7_BD_WAIT_QDESC,   /* awaiting queue_desc[qid] read-back */
    CM7_BD_WAIT_BD,      /* awaiting bd[wrap(ci)] read-back */
    CM7_BD_WAIT_PKT,     /* awaiting packet_write_data read-back */
} R100Cm7BdPhase;

typedef struct R100Cm7BdJob {
    R100Cm7BdPhase phase;
    uint32_t qid;
    uint32_t pi_snap;    /* producer index captured at doorbell fire */
    uint32_t ci;         /* local consumer index — advances per BD */
    /* BD ring DMA (host-view, HOST_PHYS_BASE already stripped). */
    uint64_t ring_dma;
    /* log2(ring entries) from queue_desc.size (= RBLN_QUEUE_ENTRY_CNT_OFFSET). */
    uint32_t ring_log2;
    /* Staging buffers for the three read-back stages. `bd_buf` holds
     * the raw 24-byte rbln_bd for the current iteration; `pkt_buf`
     * holds the 16-byte packet_write_data (we only need header, value,
     * addr — but allocating the full frame matches the HDMA max). */
    uint8_t bd_buf[REMU_BD_STRIDE];
    uint8_t pkt_buf[16];           /* sizeof(packet_write_data) */
} R100Cm7BdJob;

struct R100Cm7State {
    SysBusDevice parent_obj;

    CharBackend chr;              /* doorbell ingress (host→NPU) */
    CharBackend debug_chr;        /* doorbell ASCII tail */
    CharBackend cfg_chr;          /* cfg ingress (host→NPU) */
    CharBackend cfg_debug_chr;    /* cfg ASCII tail */
    CharBackend cm7_debug_chr;    /* BD-done state-machine tail (3c) */

    /*
     * P1c reverse-mirror trap: 4 KB MMIO overlay on chiplet-0 DRAM at
     * R100_DEVICE_COMM_SPACE_BASE (0x10200000). Models silicon's
     * inbound iATU that maps host BAR2 cfg-head onto NPU local memory
     * — host writes (cfg chardev) and NPU writes (q-cp / q-sys MMIO)
     * both terminate in cfg_shadow, and NPU writes additionally push
     * an OP_CFG_WRITE upstream so the kmd's BAR2 cfg-head shadow stays
     * in sync (FUNC_SCRATCH completion path: q-cp's
     * `cb_complete → writel(scratchpad)` round-trips back into
     * `rebel_cfg_read(FUNC_SCRATCH)` on the host without an explicit
     * DMA transaction). Reads always come from cfg_shadow, so the
     * NPU sees its own stores plus everything the host has published.
     */
    MemoryRegion cfg_mirror_mmio;

    R100MailboxState *mailbox;      /* VF0: M6 INTGR sink + M8a ISSR sink */
    R100MailboxState *pf_mailbox;   /* PF: CM7-stub FW_BOOT_DONE + BD-done pi source */
    R100IMSIXState   *imsix;        /* M7 link, dead since P2 — kept for P7 cleanup */
    R100MailboxState *mbtq_mailbox; /* PERI0_MAILBOX_M9_CPU1 — q-cp DNC task queue */
    R100HDMAState    *hdma;         /* M9-1c: shared chardev owner */

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

    /* HDMA counters (both directions). */
    uint64_t hdma_frames_sent;
    uint64_t hdma_frames_dropped;
    uint64_t hdma_frames_received;

    /* QINIT stub self-diagnostics (unchanged from 3b). */
    uint64_t qinit_stubs_fired;
    uint64_t qinit_stubs_dropped;

    /* BD-done counters. */
    uint64_t bd_doorbells;         /* INTGR1 bit <qid> fires accepted */
    uint64_t bd_doorbells_dropped; /* fires rejected (job busy, out-of-range) */
    uint64_t bds_completed;        /* BDs that reached the FSM's IDLE complete */

    /* dnc_one_task entries pushed to the q-cp task-queue mailbox.
     * pi_next is the local cache of the producer index we publish
     * into MBTQ_PI_IDX (ISSR[0]) so we ring-wrap correctly. */
    uint64_t mbtq_pushes;
    uint64_t mbtq_pushes_dropped;
    uint32_t mbtq_pi_next;

    /*
     * P1c kill-switches. Stage 3c shipped the CM7-side BD-done state
     * machine and DNC mbtq push as stop-gaps for q-cp running blind to
     * host RAM. P1a's r100-pcie-outbound iATU model gives q-cp on CP0
     * direct host-RAM access via HDMA, so q-cp now owns the full BD
     * lifecycle (cb_proc_pkt_loop → cb_complete → pcie_msix_trigger).
     * Both stubs default off; keep the helpers wired so a regression
     * can flip a -global back on without re-plumbing INTGR1.
     */
    bool bd_done_stub;
    bool mbtq_stub;
    /*
     * The QINIT stub races q-cp on the init_done write: cm7 fires
     * synchronously from the INTGR1 ISR (~µs after the doorbell), q-cp
     * runs hq_init / rl_cq_init in the hq_task FreeRTOS context (~ms
     * later because the path includes 4 KB outbound iATU reads of
     * dev_desc + queue_desc). When cm7 wins, the kmd's `init_done`
     * poll (rebel_queue_init) resolves before q-cp's `cq.initialized`
     * goes true; the kmd's BD-doorbell (DB_IDX_CQ) then arrives at
     * q-cp's doorbell ISR, which calls hq_set_cq_req_funcs and logs
     *   "[hq] cq is not initialized"
     * — leaving the BD permanently un-serviced. Default qinit-stub
     * off so the kmd only sees init_done after q-cp has actually
     * finished init (the natural serialisation point).
     */
    bool qinit_stub;

    R100Cm7BdJob jobs[R100_CM7_MAX_QUEUES];
};

/*
 * kmd inconsistency helper: rbln_dma_host_convert() adds
 * REMU_HOST_PHYS_BASE to every DMA address published through
 * rbln_device_desc + rbln_queue_desc.addr_{lo,hi}, but
 * rbln_queue_test writes bd->addr as a raw dma_addr_t (the value
 * returned by dma_alloc_coherent). The NPU stub has to cope with
 * both conventions because it reads both kinds of pointers. Strip
 * HOST_PHYS_BASE when present; otherwise assume raw.
 */
static inline uint64_t cm7_host_dma(uint64_t kmd_addr)
{
    return kmd_addr >= REMU_HOST_PHYS_BASE
         ? kmd_addr - REMU_HOST_PHYS_BASE
         : kmd_addr;
}

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

static void r100_cm7_bd_emit_debug(R100Cm7State *s, uint32_t qid,
                                   const char *transition, uint32_t ci,
                                   uint32_t pi, const char *detail)
{
    char line[160];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->cm7_debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "bd-done qid=%u %s ci=%u pi=%u %s completed=%" PRIu64 "\n",
                 qid, transition, ci, pi,
                 detail ? detail : "", s->bds_completed);
    if (n > 0) {
        qemu_chr_fe_write(&s->cm7_debug_chr, (const uint8_t *)line, n);
    }
}

static void r100_cm7_mbtq_emit_debug(R100Cm7State *s, uint32_t qid,
                                     uint32_t slot, uint32_t pi,
                                     uint64_t cmd_descr_addr,
                                     const char *status)
{
    char line[160];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->cm7_debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "mbtq qid=%u slot=%u pi=%u cmd_descr=0x%" PRIx64
                 " status=%s pushes=%" PRIu64 " dropped=%" PRIu64 "\n",
                 qid, slot, pi, cmd_descr_addr, status, s->mbtq_pushes,
                 s->mbtq_pushes_dropped);
    if (n > 0) {
        qemu_chr_fe_write(&s->cm7_debug_chr, (const uint8_t *)line, n);
    }
}

/* ------------------------------------------------------------------ */
/* HDMA egress wrappers — delegate to r100-hdma                        */
/* ------------------------------------------------------------------ */

/* FW_BOOT_DONE value written into PF.ISSR[4] by the CM7 stub on SOFT_RESET. */
#define REMU_FW_BOOT_DONE 0xFB0D

/* As of M9-1c the `hdma` chardev is owned by r100-hdma (single-frontend
 * CharBackend constraint); cm7 reaches it through a QOM link. The
 * three thin wrappers below preserve cm7's existing call sites and
 * accumulate the per-cm7 success/drop counters so the QINIT and
 * BD-done observability paths look unchanged from outside. */

static bool r100_cm7_hdma_write_tagged(R100Cm7State *s, uint32_t req_id,
                                       uint64_t dst, const void *payload,
                                       uint32_t len, const char *tag)
{
    if (!s->hdma) {
        s->hdma_frames_dropped++;
        qemu_log_mask(LOG_UNIMP,
                      "r100-cm7: hdma WRITE %s dropped (link unbound) "
                      "dst=0x%" PRIx64 " len=%u req_id=%u\n",
                      tag, dst, len, req_id);
        return false;
    }
    if (!r100_hdma_emit_write_tagged(s->hdma, req_id, dst, payload, len,
                                     tag)) {
        s->hdma_frames_dropped++;
        return false;
    }
    s->hdma_frames_sent++;
    return true;
}

static bool r100_cm7_hdma_read_req(R100Cm7State *s, uint32_t req_id,
                                   uint64_t src, uint32_t read_len,
                                   const char *tag)
{
    if (!s->hdma) {
        s->hdma_frames_dropped++;
        qemu_log_mask(LOG_UNIMP,
                      "r100-cm7: hdma READ_REQ %s dropped (link unbound) "
                      "src=0x%" PRIx64 " len=%u req_id=%u\n",
                      tag, src, read_len, req_id);
        return false;
    }
    if (!r100_hdma_emit_read_req(s->hdma, req_id, src, read_len, tag)) {
        s->hdma_frames_dropped++;
        return false;
    }
    s->hdma_frames_sent++;
    return true;
}

static bool r100_cm7_hdma_cfg_write(R100Cm7State *s, uint32_t req_id,
                                    uint32_t cfg_off, uint32_t val,
                                    const char *tag)
{
    if (!s->hdma) {
        s->hdma_frames_dropped++;
        qemu_log_mask(LOG_UNIMP,
                      "r100-cm7: hdma CFG_WRITE %s dropped (link "
                      "unbound) off=0x%x val=0x%x req_id=%u\n",
                      tag, cfg_off, val, req_id);
        return false;
    }
    if (!r100_hdma_emit_cfg_write(s->hdma, req_id, cfg_off, val, tag)) {
        s->hdma_frames_dropped++;
        return false;
    }
    s->hdma_frames_sent++;
    return true;
}

/* ------------------------------------------------------------------ */
/* M8b Stage 3b — QINIT CM7 stub (untagged, req_id=0)                  */
/* ------------------------------------------------------------------ */

/*
 * Silicon flow: kmd's rebel_queue_init() writes desc into host RAM,
 * programs DDH_BASE_{LO,HI} into BAR2 cfg head, then rings INTGR1
 * bit 7 (REBEL_DOORBELL_QUEUE_INIT). PCIe CM7 responds by DMA'ing
 * the descriptor, writing desc->fw_version + desc->init_done = 1.
 *
 * REMU can't read host RAM from the NPU-side QEMU (iATU not modelled
 * before 3c), so we hardcode fw_version to a string whose major-version
 * prefix matches the kmd's strncmp check in rbln_device_version_check
 * — today's drivers ship 3.x, so "3.remu-stub" passes.
 */
#define REMU_FW_VERSION_STR "3.remu-stub"

static uint64_t r100_cm7_ddh_desc_dma(R100Cm7State *s)
{
    uint32_t lo = s->cfg_shadow[REMU_CFG_DDH_BASE_LO / 4];
    uint32_t hi = s->cfg_shadow[REMU_CFG_DDH_BASE_HI / 4];
    uint64_t ddh_host_addr = ((uint64_t)hi << 32) | lo;

    if (ddh_host_addr == 0) {
        return 0;
    }
    return cm7_host_dma(ddh_host_addr);
}

static void r100_cm7_qinit_stub(R100Cm7State *s)
{
    uint64_t desc_dma;
    uint8_t fw_version[REMU_DDH_VERSION_MAX];
    uint32_t init_done = 1;

    /* P1c: q-cp on CP0 owns the init_done write (see struct comment). */
    if (!s->qinit_stub) {
        return;
    }
    if (!s->hdma) {
        /* Single-QEMU runs / tests don't wire hdma; treat as no-op. */
        qemu_log_mask(LOG_UNIMP,
                      "r100-cm7: QINIT stub: hdma link unbound, "
                      "skipping\n");
        s->qinit_stubs_dropped++;
        return;
    }
    desc_dma = r100_cm7_ddh_desc_dma(s);
    if (desc_dma == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cm7: QINIT stub: DDH_BASE=0 (cfg not "
                      "programmed)\n");
        s->qinit_stubs_dropped++;
        return;
    }

    /* fw_version first so kmd's readl_poll_timeout_atomic never sees
     * init_done=1 with a still-empty fw_version. Buffer is zero-filled
     * so the remainder of the 52-byte field is NUL (strncpy semantics
     * match the kmd). */
    memset(fw_version, 0, sizeof(fw_version));
    strncpy((char *)fw_version, REMU_FW_VERSION_STR,
            sizeof(fw_version) - 1);
    if (!r100_cm7_hdma_write_tagged(s, 0u,
                                    desc_dma + REMU_DDH_FW_VERSION_OFF,
                                    fw_version, sizeof(fw_version),
                                    "qinit/fw_version")) {
        s->qinit_stubs_dropped++;
        return;
    }
    if (!r100_cm7_hdma_write_tagged(s, 0u,
                                    desc_dma + REMU_DDH_INIT_DONE_OFF,
                                    &init_done, sizeof(init_done),
                                    "qinit/init_done")) {
        s->qinit_stubs_dropped++;
        return;
    }
    s->qinit_stubs_fired++;
    qemu_log_mask(LOG_TRACE,
                  "r100-cm7: QINIT stub: desc=0x%" PRIx64
                  " init_done=1 fw_version='%s' fired=%" PRIu64 "\n",
                  desc_dma, REMU_FW_VERSION_STR, s->qinit_stubs_fired);
}

/* ------------------------------------------------------------------ */
/* M8b Stage 3c — BD-done state machine                                */
/* ------------------------------------------------------------------ */

static inline uint32_t r100_cm7_wrap_idx(R100Cm7BdJob *j, uint32_t i)
{
    return i & ((1u << j->ring_log2) - 1u);
}

static uint64_t r100_cm7_bd_dma(R100Cm7BdJob *j, uint32_t idx)
{
    return j->ring_dma + (uint64_t)r100_cm7_wrap_idx(j, idx) * REMU_BD_STRIDE;
}

static uint64_t r100_cm7_qdesc_dma(R100Cm7State *s, uint32_t qid)
{
    uint64_t desc_dma = r100_cm7_ddh_desc_dma(s);
    if (desc_dma == 0) {
        return 0;
    }
    return desc_dma + REMU_DDH_QUEUE_DESC_OFF +
           (uint64_t)qid * REMU_QDESC_STRIDE;
}

static bool r100_cm7_bd_kick_read_bd(R100Cm7State *s, R100Cm7BdJob *j)
{
    uint64_t bd_dma = r100_cm7_bd_dma(j, j->ci);
    uint32_t req_id = j->qid + 1u;

    j->phase = CM7_BD_WAIT_BD;
    r100_cm7_bd_emit_debug(s, j->qid, "WAIT_BD", j->ci, j->pi_snap,
                           "READ_REQ bd");
    return r100_cm7_hdma_read_req(s, req_id, bd_dma, REMU_BD_STRIDE,
                                  "bd-done/bd");
}

static void r100_cm7_bd_fail(R100Cm7State *s, R100Cm7BdJob *j,
                             const char *why)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "r100-cm7: BD-done qid=%u ci=%u pi=%u fail: %s\n",
                  j->qid, j->ci, j->pi_snap, why);
    r100_cm7_bd_emit_debug(s, j->qid, "IDLE", j->ci, j->pi_snap, why);
    j->phase = CM7_BD_IDLE;
    s->bd_doorbells_dropped++;
}

static void r100_cm7_bd_start(R100Cm7State *s, uint32_t qid)
{
    R100Cm7BdJob *j;
    uint32_t pi;
    uint64_t qdesc_dma;
    uint32_t req_id = qid + 1u;

    /* P1c: q-cp on CP0 owns the BD lifecycle (see struct comment). */
    if (!s->bd_done_stub) {
        return;
    }
    if (qid >= R100_CM7_MAX_QUEUES) {
        s->bd_doorbells_dropped++;
        return;
    }
    j = &s->jobs[qid];
    if (j->phase != CM7_BD_IDLE) {
        /* In-flight — the kmd may ring twice before the first walk
         * finishes, but rbln_queue_test only submits a single BD
         * before waiting. The new pi will be picked up when we
         * re-read the ISSR on the next idle transition; for now just
         * count the drop. */
        s->bd_doorbells_dropped++;
        qemu_log_mask(LOG_UNIMP,
                      "r100-cm7: BD-done qid=%u doorbell while busy "
                      "(phase=%d)\n", qid, (int)j->phase);
        return;
    }
    if (!s->hdma) {
        s->bd_doorbells_dropped++;
        qemu_log_mask(LOG_UNIMP,
                      "r100-cm7: BD-done qid=%u doorbell but hdma "
                      "link unbound\n", qid);
        return;
    }
    /* kmd publishes `pi` via rebel_mailbox_write(rdev, qid, pi), which
     * on BAR4 lands at MAILBOX_BASE + qid*4. On silicon that landing
     * pad is PF.ISSR[qid] — PCIE_CM7 reads PF and (for VF event
     * delivery) forwards into VF0. q-cp/q-sys reads the value back
     * via `inst[PCIE_PF=0] = IDX_MAILBOX_PCIE_PF` from PF.ISSR[qid].
     * REMU's host→NPU relay therefore deposits the kmd payload into
     * `pf_mailbox` (see KIND_ISSR in r100_cm7_deliver), and the
     * BD-done state machine here reads it from the same backing
     * store. P1b post-mortem in docs/debugging.md captures the
     * earlier VF0-only routing that produced "undefined tdr doorbell
     * event 0" because q-cp's URG handler reads PF, not VF0. */
    pi = s->pf_mailbox ? r100_mailbox_get_issr(s->pf_mailbox, qid) : 0;
    if (pi == j->ci) {
        /* Spurious doorbell / already caught up. */
        s->bd_doorbells_dropped++;
        return;
    }
    qdesc_dma = r100_cm7_qdesc_dma(s, qid);
    if (qdesc_dma == 0) {
        r100_cm7_bd_fail(s, j, "DDH_BASE=0");
        return;
    }
    j->qid = qid;
    j->pi_snap = pi;
    s->bd_doorbells++;
    r100_cm7_bd_emit_debug(s, qid, "WAIT_QDESC", j->ci, pi,
                           "READ_REQ queue_desc");
    j->phase = CM7_BD_WAIT_QDESC;
    if (!r100_cm7_hdma_read_req(s, req_id, qdesc_dma, REMU_QDESC_STRIDE,
                                "bd-done/qdesc")) {
        r100_cm7_bd_fail(s, j, "READ_REQ qdesc emit");
    }
}

static void r100_cm7_bd_on_qdesc(R100Cm7State *s, R100Cm7BdJob *j,
                                 const uint8_t *payload, uint32_t len)
{
    uint32_t ring_lo, ring_hi, ring_log2;
    uint64_t ring_kmd;

    if (len != REMU_QDESC_STRIDE) {
        r100_cm7_bd_fail(s, j, "qdesc resp len");
        return;
    }
    ring_log2 = ldl_le_p(&payload[REMU_QDESC_SIZE_OFF]);
    ring_lo   = ldl_le_p(&payload[REMU_QDESC_ADDR_LO_OFF]);
    ring_hi   = ldl_le_p(&payload[REMU_QDESC_ADDR_HI_OFF]);
    if (ring_log2 == 0 || ring_log2 > 16) {
        r100_cm7_bd_fail(s, j, "qdesc ring_log2 range");
        return;
    }
    ring_kmd  = ((uint64_t)ring_hi << 32) | ring_lo;
    j->ring_dma = cm7_host_dma(ring_kmd);
    j->ring_log2 = ring_log2;

    if (!r100_cm7_bd_kick_read_bd(s, j)) {
        r100_cm7_bd_fail(s, j, "READ_REQ bd emit");
    }
}

static void r100_cm7_bd_on_bd(R100Cm7State *s, R100Cm7BdJob *j,
                              const uint8_t *payload, uint32_t len)
{
    uint64_t pkt_kmd, pkt_dma;
    uint32_t pkt_size;
    uint32_t req_id = j->qid + 1u;

    if (len != REMU_BD_STRIDE) {
        r100_cm7_bd_fail(s, j, "bd resp len");
        return;
    }
    /* Cache the whole BD so we can emit the DONE write-back later
     * without a second round-trip. */
    memcpy(j->bd_buf, payload, REMU_BD_STRIDE);
    pkt_size = ldl_le_p(&payload[REMU_BD_SIZE_OFF]);
    pkt_kmd  = ldq_le_p(&payload[REMU_BD_ADDR_OFF]);
    pkt_dma  = cm7_host_dma(pkt_kmd);
    if (pkt_size == 0 || pkt_size > sizeof(j->pkt_buf)) {
        /* Real silicon would tolerate larger payloads; rbln_queue_test
         * stages exactly sizeof(packet_write_data) = 16 bytes. Clamp
         * + warn rather than refusing — we still need to advance ci
         * so the kmd's dma_fence_wait_timeout eventually returns.
         * Treat as "we only care about the first 16 bytes". */
        qemu_log_mask(LOG_UNIMP,
                      "r100-cm7: BD-done qid=%u bd.size=%u clamped to %zu\n",
                      j->qid, pkt_size, sizeof(j->pkt_buf));
        pkt_size = sizeof(j->pkt_buf);
    }
    r100_cm7_bd_emit_debug(s, j->qid, "WAIT_PKT", j->ci, j->pi_snap,
                           "READ_REQ pkt");
    j->phase = CM7_BD_WAIT_PKT;
    if (!r100_cm7_hdma_read_req(s, req_id, pkt_dma, pkt_size,
                                "bd-done/pkt")) {
        r100_cm7_bd_fail(s, j, "READ_REQ pkt emit");
    }
}

static void r100_cm7_bd_on_pkt(R100Cm7State *s, R100Cm7BdJob *j,
                               const uint8_t *payload, uint32_t len)
{
    uint32_t req_id = j->qid + 1u;
    uint32_t pkt_value;
    uint32_t bd_header;
    uint64_t bd_dma;
    uint64_t qdesc_ci_dma;

    if (len < 8) {
        r100_cm7_bd_fail(s, j, "pkt resp len");
        return;
    }
    /* packet_write_data layout: { u32 header, u32 value, u64 addr } —
     * `value` is what PACKET_WRITE_DATA publishes into `addr`. The kmd
     * test sets addr = scratch_addr (FUNC_SCRATCH in cfg region) and
     * reads it back via rebel_cfg_read(FUNC_SCRATCH). We mirror that
     * by pushing `value` into the host's cfg-head shadow at
     * FUNC_SCRATCH (OP_CFG_WRITE). Real silicon also writes a copy
     * into NPU-local DRAM at dram_cfg_base + FUNC_SCRATCH; the kmd
     * doesn't read that path on x86, so skip it for now (tracked as
     * M9 follow-up). */
    pkt_value = ldl_le_p(&payload[4]);
    if (!r100_cm7_hdma_cfg_write(s, req_id, REMU_CFG_FUNC_SCRATCH_OFF,
                                 pkt_value, "bd-done/scratch")) {
        r100_cm7_bd_fail(s, j, "CFG_WRITE emit");
        return;
    }
    /* Mark BD done + advance local ci + publish ci to queue_desc. */
    bd_header  = ldl_le_p(&j->bd_buf[REMU_BD_HEADER_OFF]);
    bd_header |= REMU_BD_FLAGS_DONE_MASK;
    {
        uint8_t done_buf[4];
        stl_le_p(done_buf, bd_header);
        bd_dma = r100_cm7_bd_dma(j, j->ci);
        if (!r100_cm7_hdma_write_tagged(s, req_id,
                                        bd_dma + REMU_BD_HEADER_OFF,
                                        done_buf, sizeof(done_buf),
                                        "bd-done/bd_header")) {
            r100_cm7_bd_fail(s, j, "WRITE bd_header emit");
            return;
        }
    }
    j->ci++;
    {
        uint8_t ci_buf[4];
        stl_le_p(ci_buf, j->ci);
        qdesc_ci_dma = r100_cm7_qdesc_dma(s, j->qid);
        if (qdesc_ci_dma == 0 ||
            !r100_cm7_hdma_write_tagged(s, req_id,
                                        qdesc_ci_dma + REMU_QDESC_CI_OFF,
                                        ci_buf, sizeof(ci_buf),
                                        "bd-done/qdesc_ci")) {
            r100_cm7_bd_fail(s, j, "WRITE qdesc.ci emit");
            return;
        }
    }
    s->bds_completed++;

    if (j->ci != j->pi_snap) {
        /* More BDs staged in this fire — loop back without re-reading
         * queue_desc (pi is a snapshot; ring_dma doesn't move). */
        r100_cm7_bd_emit_debug(s, j->qid, "LOOP", j->ci, j->pi_snap,
                               "READ_REQ next bd");
        if (!r100_cm7_bd_kick_read_bd(s, j)) {
            r100_cm7_bd_fail(s, j, "READ_REQ next bd");
        }
        return;
    }

    /*
     * P2: this used to call r100_imsix_notify(s->imsix, j->qid) to fire
     * MSI-X straight from the FSM. After P1c the FSM is gated off by
     * default (bd-done-stub=false) and q-cp on CP0's
     * `cb_complete → pcie_msix_trigger` is the natural MSI-X source via
     * the r100-imsix MMIO trap. With bd-done-stub=on (used for
     * bisecting q-cp regressions) the FSM still walks the BD and writes
     * DONE / advances ci, but it must NOT also fire MSI-X — duplicate
     * fires desync `host/msix.log` and make bisect logs ambiguous.
     * Single source of truth: cb_complete. The `imsix` QOM link + state
     * field stay wired so P7 can delete them alongside the rest of the
     * Stage 3c FSM in one cleanup pass.
     */
    r100_cm7_bd_emit_debug(s, j->qid, "IDLE", j->ci, j->pi_snap,
                           "complete");
    j->phase = CM7_BD_IDLE;
}

/* ------------------------------------------------------------------ */
/* MBTQ — push a dnc_one_task into q-cp's task-queue mailbox           */
/* ------------------------------------------------------------------ */

/*
 * q-cp's CP1.cpu0 sits in taskmgr_fetch_dnc_task_master_cp1
 * (cp/.../fetch_task.c) polling MBTQ_PI_IDX (ISSR[0]) on
 * PERI0_MAILBOX_M9_CPU1 (q-cp side mb_task_queue.c:33-42). On
 * silicon the PCIE_CM7 firmware writes 24 B dnc_one_task entries
 * + bumps PI to dispatch DNC work; REMU has no Cortex-M7 vCPU,
 * so r100-cm7 takes that role.
 *
 * Wire format (mb_task_queue.c slot layout):
 *   ISSR[QUEUE_IDX + (pi%MAX)*ENTRY_SIZE_WORD .. +ENTRY_SIZE_WORD-1] = entry
 *   ISSR[PI_IDX] = pi+1
 *
 * The mailbox itself is the dumb scratch-store, so
 * r100_mailbox_set_issr_words bypasses the issr_store funnel
 * (no chardev egress, no host-relay accounting). Per-instance
 * counters live on R100Cm7State.
 *
 * M9-1c: each push synthesises a minimal `struct cmd_descr` in a
 * pre-reserved chiplet-0 private-DRAM slot and points the mailbox
 * entry's cmd_descr field at it. Without this q-cp's worker
 * derefs NULL at fetch_task.c:173 (`one_task->cmd_descr->...`)
 * and faults before reaching the new active DNC stub. The synth
 * sets:
 *   cmd_descr_common.cmd_type = COMMON_CMD_TYPE_COMPUTE (0)
 *   cmd_descr_common.descr_type = COMMON_DESCR_TYPE_DEFAULT (0)
 *   cmd_descr_dnc.dnc_task_conf.core_affinity = BIT(0)
 * everything else zero. The worker reads `core_affinity & BIT(i)`
 * to decide which DNC slot to dispatch to (fetch_task.c:181); 0x1
 * routes to DNC0 inside the worker's dnc range, which is what our
 * r100-dnc-cluster active path is wired to fire on.
 */
static void r100_cm7_synth_cmd_descr(R100Cm7State *s, uint32_t pi_index,
                                     uint64_t *out_npu_addr)
{
    /* dnc_task_conf.core_affinity is a uint64_t bitfield at offset 20
     * of cmd_descr (after the 20 B cmd_descr_common). Layout chosen
     * to match g_cmd_descr_common_rebel.h + g_cmd_descr_dnc_rebel.h
     * exactly — bytes 0..19 zero, bytes 20..27 little-endian 0x1. */
    enum {
        DNC_TASK_CONF_OFF      = 20u, /* sizeof(cmd_descr_common) */
        DESCR_BUF_LEN          = 32u, /* covers task_conf in full */
    };
    uint8_t buf[DESCR_BUF_LEN];
    uint64_t addr = R100_CMD_DESCR_SYNTH_BASE +
                    (uint64_t)(pi_index % R100_CMD_DESCR_SYNTH_COUNT) *
                    R100_CMD_DESCR_SYNTH_STRIDE;
    MemTxResult mr;

    memset(buf, 0, sizeof(buf));
    /* core_affinity = 1 (bit 0 → DNC0). Low byte of the 8-byte field. */
    buf[DNC_TASK_CONF_OFF] = 0x01u;

    mr = address_space_write(&address_space_memory, addr,
                             MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));
    if (mr != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cm7: cmd_descr synth write failed addr=0x%"
                      PRIx64 "\n", addr);
        *out_npu_addr = 0;
        return;
    }
    *out_npu_addr = addr;
}

static void r100_cm7_mbtq_push(R100Cm7State *s, uint32_t qid)
{
    uint32_t entry[R100_MBTQ_ENTRY_SIZE_WORD];
    uint32_t slot;
    uint32_t pi_after;
    uint64_t cmd_descr_addr = 0;

    /* P1c: stub is off by default (see struct comment); q-cp on CP0
     * builds the real cmd_descr from CB content and pushes the
     * dnc_one_task itself once cb_proc_pkt_loop materialises. */
    if (!s->mbtq_stub) {
        return;
    }
    if (!s->mbtq_mailbox) {
        s->mbtq_pushes_dropped++;
        r100_cm7_mbtq_emit_debug(s, qid, 0, 0, 0, "no-mailbox");
        return;
    }

    /* Stage cmd_descr first so the entry's pointer is valid the moment
     * q-cp pops it. */
    r100_cm7_synth_cmd_descr(s, s->mbtq_pi_next, &cmd_descr_addr);

    /*
     * dnc_one_task on-wire layout (task_mgr/task.h):
     *   word 0..0  union exec_id    id_info  (32 bits)
     *   word 1..2  struct cmd_descr *cmd_descr (64 bits, NPU phys)
     *   word 3..4  uint64_t         jurq_used (64 bits, 0)
     *   word 5     padding to MTQ_ENTRY_SIZE_WORD = 6
     */
    memset(entry, 0, sizeof(entry));
    entry[0] = qid + 1u;
    entry[1] = (uint32_t)cmd_descr_addr;
    entry[2] = (uint32_t)(cmd_descr_addr >> 32);
    /* entry[3..5] left zero — jurq_used + tail padding. */

    slot = R100_MBTQ_QUEUE_IDX +
           (s->mbtq_pi_next & (R100_MBTQ_ENTRY_MAX - 1u)) *
            R100_MBTQ_ENTRY_SIZE_WORD;
    r100_mailbox_set_issr_words(s->mbtq_mailbox, slot, entry,
                                R100_MBTQ_ENTRY_SIZE_WORD);
    pi_after = ++s->mbtq_pi_next;
    r100_mailbox_set_issr_words(s->mbtq_mailbox, R100_MBTQ_PI_IDX,
                                &pi_after, 1);
    s->mbtq_pushes++;

    r100_cm7_mbtq_emit_debug(s, qid, slot, pi_after, cmd_descr_addr,
                             cmd_descr_addr ? "ok" : "ok-null-descr");
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
         * model that (see docs/roadmap.md Phase 2 "real CA73 soft-
         * reset"), so we synthesise only the observable endpoint.
         * Running firmware is undisturbed.
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
        r100_mailbox_raise_intgr(s->mailbox, 1, val);
        /*
         * M8b Stage 3b CM7 stub: INTGR1 bit 7 (REBEL_DOORBELL_QUEUE_INIT)
         * triggers the QINIT write-back. The SPI raise above is kept
         * for parity with silicon's path (default_cb is a no-op on
         * CA73; the IRQ just gets logged).
         */
        if (val & (1u << REMU_DB_QUEUE_INIT_INTGR1_BIT)) {
            r100_cm7_qinit_stub(s);
        }
        /*
         * P1c (Phase 1c): the CM7-side BD-done state machine and DNC
         * mbtq push were Stage 3c stop-gaps used while q-cp couldn't
         * reach host RAM. With P1a's `r100-pcie-outbound` modelling
         * the chiplet-0 outbound iATU, q-cp on CP0 now reads BDs /
         * packets out of host RAM via HDMA and owns the full BD
         * lifecycle:
         *
         *   queue doorbell INTGR1 bit N
         *      → r100_mailbox_raise_intgr (above) → SPI 185
         *      → q-cp hq_task wakes
         *      → rl_cq_proc → rl_cq_bd_proc → cb_proc_pkt_loop
         *      → cb_complete: BD.DONE | ci++ | FUNC_SCRATCH write
         *      → pcie_msix_trigger → r100-imsix → host MSI-X
         *
         * Synthesising the same effects in CM7 raced with q-cp: the
         * stub completed first, the kmd's fence resolved, the test
         * buffer was freed, and q-cp then read garbage and aborted on
         * "Current packet(0x1) is not supported." We keep the helper
         * functions compiled in (call sites below) so a regression
         * can flip a runtime switch back on, but the helpers
         * themselves now early-return when `s->bd_done_stub` /
         * `s->mbtq_stub` are off (default off post-P1c).
         */
        for (uint32_t qid = 0; qid < R100_CM7_MAX_QUEUES; qid++) {
            if (val & (1u << qid)) {
                r100_cm7_bd_start(s, qid);
                r100_cm7_mbtq_push(s, qid);
            }
        }
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
     * Single source of truth: cfg_shadow. P1c installs an MMIO trap
     * at R100_DEVICE_COMM_SPACE_BASE (see r100_cm7_realize), so NPU
     * reads of e.g. DDH_BASE_{LO,HI} land in cfg_mirror_read which
     * returns the slot below — there is no separate DRAM mirror to
     * keep in sync. NPU-side writes round-trip back to the host's
     * cfg_mmio_regs through the same trap (OP_CFG_WRITE), closing
     * the loop without a feedback path through this function.
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
/* HDMA RX callback (host→NPU OP_READ_RESP responses — M8b 3c)         */
/*                                                                     */
/* r100-hdma owns the chardev backend and demuxes by req_id partition: */
/* it calls this back for the cm7 range (1..R100_CM7_MAX_QUEUES). The  */
/* old in-cm7 RemuHdmaRx accumulator is gone — the device-level RX     */
/* state now lives on R100HDMAState.                                   */
/* ------------------------------------------------------------------ */

static void r100_cm7_hdma_dispatch(void *opaque,
                                   const RemuHdmaHeader *hdr,
                                   const uint8_t *payload)
{
    R100Cm7State *s = opaque;
    R100Cm7BdJob *j;
    uint32_t slot;

    s->hdma_frames_received++;

    /* op == OP_READ_RESP enforced by r100-hdma; assert defensively. */
    if (hdr->op != REMU_HDMA_OP_READ_RESP) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cm7: hdma RX unexpected op=%s req_id=%u "
                      "dst=0x%" PRIx64 " len=%u\n",
                      remu_hdma_op_str(hdr->op), hdr->req_id, hdr->dst,
                      hdr->len);
        return;
    }
    /* req_id = qid + 1; 0 would be an untagged write reply, which
     * the NPU never expects. */
    if (hdr->req_id == 0 ||
        hdr->req_id > R100_CM7_MAX_QUEUES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cm7: hdma RX bad req_id=%u (expected 1..%u)\n",
                      hdr->req_id, R100_CM7_MAX_QUEUES);
        return;
    }
    slot = hdr->req_id - 1u;
    j = &s->jobs[slot];
    switch (j->phase) {
    case CM7_BD_WAIT_QDESC:
        r100_cm7_bd_on_qdesc(s, j, payload, hdr->len);
        break;
    case CM7_BD_WAIT_BD:
        r100_cm7_bd_on_bd(s, j, payload, hdr->len);
        break;
    case CM7_BD_WAIT_PKT:
        r100_cm7_bd_on_pkt(s, j, payload, hdr->len);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-cm7: hdma RX resp qid=%u phase=IDLE "
                      "(stale/dup) req_id=%u len=%u\n",
                      slot, hdr->req_id, hdr->len);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* P1c — DEVICE_COMMUNICATION_SPACE reverse-mirror trap                */
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
        if (!r100_cm7_hdma_cfg_write(s, 0u, (uint32_t)(off & ~0x3u),
                                     v32, "cfg-mirror")) {
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
    /* cfg is optional — when unset we still accept doorbell frames but
     * the CM7 QINIT stub will short-circuit on a zeroed DDH_BASE. */
    if (qemu_chr_fe_backend_connected(&s->cfg_chr)) {
        qemu_chr_fe_set_handlers(&s->cfg_chr,
                                 r100_cm7_cfg_can_receive,
                                 r100_cm7_cfg_receive,
                                 NULL, NULL, s, NULL, true);
    }
    /* hdma chardev now owned by r100-hdma; cm7 plugs into its RX
     * demux through the cm7-callback hook. Optional — when the link
     * is unbound (single-QEMU / M6 test paths) the BD-done state
     * machine silently fails its READ_REQ emits and the job FSM
     * never leaves IDLE. */
    if (s->hdma) {
        r100_hdma_set_cm7_callback(s->hdma, r100_cm7_hdma_dispatch, s);
    }

    /*
     * P1c reverse-mirror trap: install the 4 KB cfg-shadow MMIO over
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
    qemu_chr_fe_deinit(&s->cm7_debug_chr, false);
}

static void r100_cm7_reset(DeviceState *dev)
{
    R100Cm7State *s = R100_CM7(dev);

    remu_frame_rx_reset(&s->rx);
    remu_frame_rx_reset(&s->cfg_rx);
    /* Drop any in-flight BD jobs — a reset between runs invalidates
     * the host-view DMA addresses. Counters survive reset so tests
     * can assert cumulative behaviour. */
    for (size_t i = 0; i < ARRAY_SIZE(s->jobs); i++) {
        s->jobs[i].phase = CM7_BD_IDLE;
        s->jobs[i].ci = 0;
    }
    /* MBTQ producer index is a local cache; reset with the device so
     * a fresh boot starts the ring at 0. The mailbox itself holds the
     * authoritative PI/CI; on a reset both sides are 0. */
    s->mbtq_pi_next = 0;
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
        VMSTATE_UINT64(hdma_frames_received, R100Cm7State),
        VMSTATE_UINT64(qinit_stubs_fired, R100Cm7State),
        VMSTATE_UINT64(qinit_stubs_dropped, R100Cm7State),
        VMSTATE_UINT64(bd_doorbells, R100Cm7State),
        VMSTATE_UINT64(bd_doorbells_dropped, R100Cm7State),
        VMSTATE_UINT64(bds_completed, R100Cm7State),
        VMSTATE_UINT64(mbtq_pushes, R100Cm7State),
        VMSTATE_UINT64(mbtq_pushes_dropped, R100Cm7State),
        VMSTATE_UINT32(mbtq_pi_next, R100Cm7State),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_cm7_properties[] = {
    DEFINE_PROP_CHR("chardev", R100Cm7State, chr),
    DEFINE_PROP_CHR("debug-chardev", R100Cm7State, debug_chr),
    DEFINE_PROP_CHR("cfg-chardev", R100Cm7State, cfg_chr),
    DEFINE_PROP_CHR("cfg-debug-chardev", R100Cm7State, cfg_debug_chr),
    DEFINE_PROP_CHR("cm7-debug-chardev", R100Cm7State, cm7_debug_chr),
    DEFINE_PROP_LINK("mailbox", R100Cm7State, mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_LINK("pf-mailbox", R100Cm7State, pf_mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_LINK("imsix", R100Cm7State, imsix,
                     TYPE_R100_IMSIX, R100IMSIXState *),
    DEFINE_PROP_LINK("mbtq-mailbox", R100Cm7State, mbtq_mailbox,
                     TYPE_R100_MAILBOX, R100MailboxState *),
    DEFINE_PROP_LINK("hdma", R100Cm7State, hdma,
                     TYPE_R100_HDMA, R100HDMAState *),
    /* P1c kill-switches; default false. -global r100-cm7.bd-done-stub=on
     * (or `mbtq-stub=on` / `qinit-stub=on`) re-enables the Stage 3c
     * paths for bisecting a q-cp regression. */
    DEFINE_PROP_BOOL("bd-done-stub", R100Cm7State, bd_done_stub, false),
    DEFINE_PROP_BOOL("mbtq-stub", R100Cm7State, mbtq_stub, false),
    DEFINE_PROP_BOOL("qinit-stub", R100Cm7State, qinit_stub, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_cm7_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 PCIe CM7 stub (doorbell / cfg-shadow / QINIT / BD-done)";
    dc->realize = r100_cm7_realize;
    dc->unrealize = r100_cm7_unrealize;
    dc->vmsd = &r100_cm7_vmstate;
    device_class_set_legacy_reset(dc, r100_cm7_reset);
    device_class_set_props(dc, r100_cm7_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    /* Machine-instantiated only (mailbox / imsix links are topology). */
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
