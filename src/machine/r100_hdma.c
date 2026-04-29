/*
 * REMU - R100 NPU System Emulator
 * DesignWare dw_hdma_v0 register-block model (M9-1c + P5).
 *
 * On silicon, q-cp's `hdma_if.c` programs DMA channels in a PCIe-
 * attached DesignWare HDMA controller at
 *
 *     hdma_base = cl_id * CHIPLET_INTERVAL
 *               + U_PCIE_CORE_OFFSET (0x1C00000000)
 *               + PCIE_HDMA_OFFSET   (0x180380000)
 *
 * 16 WR + 16 RD channels each with a small register window
 * (struct hdma_ch_regs in q/cp/.../hdma_regs.h: enable, doorbell,
 * elem_pf, handshake, llp, cycle, xfer_size, sar, dar, watermark,
 * ctrl1, func_num, qos, status, int_status, int_setup, int_clear,
 * msi_{stop,watermark,abort}, msi_msgd). q-cp's hdma_ch_trigger
 * sequence is:
 *
 *     ctrl1   |= LLEN
 *     func_num = pf/vf select
 *     llp      = chain head (chan->desc)
 *     enable   = 1
 *     doorbell = HDMA_DB_START
 *
 * The completion path is either an MSI to the host (D2D channels —
 * not exercised here, msi_stop fan-out deferred) or a local SPI on
 * INT_ID_HDMA (= 186; single line shared across all 32 channels, with
 * per-channel pending bitmap in SUBCTRL_EDMA_INT_CA73 at 0x1FF8184368).
 * q-cp's hdma_forward_irq reads SUBCTRL_EDMA_INT_CA73 to find which
 * channel finished and dispatches into hdma_irq_handler.
 *
 * REMU has neither the DW HDMA IP nor the actual PCIe RC, so this
 * device emulates the *peripheral effect* over the existing `hdma`
 * chardev (remu_hdma_proto.h). Two paths share the kick handler:
 *
 *   1. Non-LL doorbell (M9-1c hand-driven test path) — single-shot
 *      using the SAR/DAR/XFER_SIZE fields with no chain. WR is
 *      fire-and-forget (status flips to STOPPED before the doorbell
 *      write returns); RD parks `pending_req_id` and completes
 *      asynchronously when the matching OP_READ_RESP arrives.
 *
 *   2. LL doorbell (P5; q-cp's `cmd_descr_hdma` / `hdma_ll_trigger`
 *      production path) — when CTRL1.LLEN=1, the kick walks the
 *      `dw_hdma_v0_lli`/`dw_hdma_v0_llp` chain rooted at LLP_LO|HI:
 *
 *        - read 24 B element header at the cursor (control,
 *          transfer_size, sar, dar). The `control` byte's LLP bit
 *          discriminates `dw_hdma_v0_llp` (16 B; jump to next chunk)
 *          from `dw_hdma_v0_lli` (24 B; data transfer).
 *        - for an LLI, route by SAR/DAR vs REMU_HOST_PHYS_BASE:
 *            * dir=WR && DAR≥host: NPU→host. address_space_read at
 *              SAR, emit OP_WRITE with stripped DAR. Chunked into
 *              REMU_HDMA_MAX_PAYLOAD-sized frames.
 *            * dir=RD && SAR≥host: host→NPU. Per chunk, emit
 *              OP_READ_REQ with stripped SAR + park on the
 *              channel's resp_cond via qemu_cond_wait_bql() (BQL
 *              released so the chardev iothread can deliver
 *              OP_READ_RESP). On wake, address_space_write the
 *              payload at DAR.
 *            * neither addr is host: D2D / NPU-internal copy. Direct
 *              address_space_read → address_space_write loop.
 *        - LIE bit on the LLI signals end-of-chain; on hit we exit
 *          the loop, flip status to STOPPED, set INT_STOP, write the
 *          per-channel SUBCTRL pending bit, and pulse SPI 186.
 *
 *      Sequential chunking + per-channel cond means each RD channel
 *      can have at most one OP_READ_REQ in flight; the WR direction
 *      is fire-and-forget. With only 16 RD channels and a single
 *      `pending_req_id` slot per channel, the existing 0x80..0xBF
 *      req_id partition is unchanged.
 *
 * The chardev backend is single-frontend (CharBackend), so r100-hdma
 * is the sole host-side counterpart and r100-cm7 reaches it through
 * a QOM link + the public helpers in r100_hdma.h. Disjoint req_id
 * partitions keep r100-pcie-outbound's PF-window reads (0xC0..0xFF)
 * from colliding with this device's channel-driven 0x80..0xBF range.
 *
 * SMMU note (P11): this engine's NPU-side SAR/DAR are DVAs that
 * traverse the chiplet's SMMU-600 on real silicon. Per Notion REBELQ
 * SMMU Design § 1, HDMA-IPA uses SID 0 on PF (LUT entries with
 * SID=17 / SSID.V=0 select the bypass region — that's a kmd/q-cp
 * configuration choice, not a property of REMU). r100_hdma_lli_{wr,
 * rd,d2d} now translate via `r100_smmu_translate` before each
 * `address_space_*` call. When CR0.SMMUEN=0 (early boot, single-
 * QEMU `--name` runs, p5 test) or the engine has no smmu link, the
 * translate falls through to identity — matches Arm-SMMU pre-enable
 * bypass and keeps existing tests working. The walker handles
 * stage-2 only in v1 (q-sys flips STE0.config to ALL_TRANS with
 * S1_DSS=BYPASS); stage-1 + multi-VF + IOTLB cache are v2.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
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
#include "r100_smmu.h"
#include "remu_doorbell_proto.h"  /* REMU_HOST_PHYS_BASE */
#include "remu_hdma_proto.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100HDMAState, R100_HDMA)

/* dw_hdma_v0 ch_sep=3 (bundle vsec_data) → per-channel stride =
 * 2 << (3 + 7) = 0x800. Mirror exposes a stride pair per channel
 * (WR slot followed by RD slot), so the full 16-channel × 2-direction
 * window is 16 × 2 × 0x800 = 0x10000. R100_HDMA_SIZE = 0x20000 leaves
 * head-room for a future common-vsec block. */

typedef enum {
    R100_HDMA_DIR_WR = 0,    /* NPU → host */
    R100_HDMA_DIR_RD = 1,    /* host → NPU */
} R100HdmaDir;

/*
 * Per-channel walker state for LL mode (P5).
 *
 * Owned + accessed under BQL. The cond is ONLY used in the RD walk
 * path so it can drop BQL while the chardev iothread delivers the
 * matching OP_READ_RESP. WR walks are fire-and-forget per chunk and
 * never touch the cond. We init/destroy on realize/unrealize (NOT in
 * reset; cond_destroy on a parked waiter would deadlock the cluster
 * reset path).
 *
 * Buffer is sized to one OP_READ_RESP frame (REMU_HDMA_MAX_PAYLOAD).
 * Larger LLIs are chunked into back-to-back round trips, each
 * consuming one resp slot — `pending_req_id` matches every chunk
 * since only one OP_READ_REQ is in flight per channel at a time.
 */
typedef struct R100HdmaWalk {
    QemuCond  resp_cond;
    bool      ll_active;     /* LL walk in progress on this channel */
    bool      resp_ready;    /* most recent RD chunk delivered */
    uint32_t  resp_len;
    uint8_t   resp_buf[REMU_HDMA_MAX_PAYLOAD];
} R100HdmaWalk;

typedef struct R100HdmaChan {
    /* Programming registers (RW from q-cp's HAL). The set of fields
     * here matches struct hdma_ch_regs in the firmware tree
     * 1:1 except for padding_1[16] which we treat as RAZ/WI. */
    uint32_t enable;
    uint32_t doorbell;
    uint32_t elem_pf;
    uint32_t handshake;
    uint64_t llp;            /* LL chain head; only meaningful with LLEN */
    uint32_t cycle;
    uint32_t xfer_size;
    uint64_t sar;            /* WR: NPU phys; RD: host phys */
    uint64_t dar;            /* WR: host phys; RD: NPU phys */
    uint32_t watermark;
    uint32_t ctrl1;          /* LLEN bit gates LL-walk path */
    uint32_t func_num;
    uint32_t qos;
    uint32_t status;         /* RUNNING / STOPPED / ABORTED */
    uint32_t int_status;     /* STOP / ABORT / WATERMARK bits */
    uint32_t int_setup;
    uint64_t msi_stop;
    uint64_t msi_watermark;
    uint64_t msi_abort;
    uint32_t msi_msgd;

    uint32_t pending_req_id; /* req_id assigned to RUNNING RD frame */

    R100HdmaWalk walk;
} R100HdmaChan;

struct R100HDMAState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharBackend hdma_chr;
    CharBackend hdma_debug_chr;
    qemu_irq hdma_irq;

    uint32_t chiplet_id;

    R100HdmaChan ch[2 /* WR/RD */][R100_HDMA_CH_COUNT];

    RemuHdmaRx rx;

    /* Counters. */
    uint64_t hdma_frames_sent;
    uint64_t hdma_frames_dropped;
    uint64_t hdma_frames_received;

    uint64_t doorbells_started;
    uint64_t doorbells_dropped;
    uint64_t channel_completions;

    /* P11: chiplet's r100-smmu for stage-2 translation of LL element
     * cursor + LLI SAR/DAR (NPU-side). May be NULL (test harnesses
     * without SMMU wiring, e.g. p5 which mmaps raw chiplet-0 DRAM
     * offsets and never enables the TCU); the translate helper falls
     * through to identity in that case. */
    R100SMMUState *smmu;
};

/* ------------------------------------------------------------------ */
/* Debug tail                                                          */
/* ------------------------------------------------------------------ */

static void r100_hdma_emit_debug(R100HDMAState *s, const char *dir,
                                 uint32_t op, uint32_t req_id,
                                 uint64_t dst, uint32_t len,
                                 const char *tag)
{
    char line[160];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->hdma_debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "hdma cl=%u %s op=%s req_id=0x%x dst=0x%" PRIx64
                 " len=%u tag=%s sent=%" PRIu64 " recv=%" PRIu64 "\n",
                 s->chiplet_id, dir, remu_hdma_op_str(op), req_id, dst,
                 len, tag, s->hdma_frames_sent,
                 s->hdma_frames_received);
    if (n > 0) {
        qemu_chr_fe_write(&s->hdma_debug_chr, (const uint8_t *)line, n);
    }
}

/* Structural trace tail (doorbell / LL walk start / completion). One
 * line per significant event, lands in the same hdma-debug chardev as
 * the OP_WRITE/READ_REQ frame trace. Always-on, bounded to a few
 * lines per kicked LL chain — no --trace overhead. */
static void r100_hdma_emit_trace(R100HDMAState *s, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

static void r100_hdma_emit_trace(R100HDMAState *s, const char *fmt, ...)
{
    char line[256];
    int n;
    va_list ap;

    if (!qemu_chr_fe_backend_connected(&s->hdma_debug_chr)) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        qemu_chr_fe_write(&s->hdma_debug_chr, (const uint8_t *)line,
                          MIN((size_t)n, sizeof(line) - 1));
    }
}

/* ------------------------------------------------------------------ */
/* Public emit API (shared with r100-cm7)                              */
/* ------------------------------------------------------------------ */

static inline uint64_t r100_hdma_strip_host_phys(uint64_t kmd_addr)
{
    return kmd_addr >= REMU_HOST_PHYS_BASE
         ? kmd_addr - REMU_HOST_PHYS_BASE
         : kmd_addr;
}

bool r100_hdma_emit_write_tagged(R100HDMAState *s, uint32_t req_id,
                                 uint64_t dst, const void *payload,
                                 uint32_t len, const char *tag)
{
    RemuHdmaEmitResult rc;

    rc = remu_hdma_emit_write_tagged(&s->hdma_chr, "r100-hdma", req_id,
                                     dst, payload, len);
    if (rc == REMU_HDMA_EMIT_OK) {
        s->hdma_frames_sent++;
        r100_hdma_emit_debug(s, "tx", REMU_HDMA_OP_WRITE, req_id, dst,
                             len, tag);
        return true;
    }
    s->hdma_frames_dropped++;
    qemu_log_mask(LOG_UNIMP,
                  "r100-hdma: WRITE %s dropped dst=0x%" PRIx64
                  " len=%u req_id=0x%x rc=%d\n", tag, dst, len, req_id,
                  (int)rc);
    return false;
}

bool r100_hdma_emit_read_req(R100HDMAState *s, uint32_t req_id,
                             uint64_t src, uint32_t read_len,
                             const char *tag)
{
    RemuHdmaEmitResult rc;

    rc = remu_hdma_emit_read_req(&s->hdma_chr, "r100-hdma", req_id, src,
                                 read_len);
    if (rc == REMU_HDMA_EMIT_OK) {
        s->hdma_frames_sent++;
        r100_hdma_emit_debug(s, "tx", REMU_HDMA_OP_READ_REQ, req_id,
                             src, read_len, tag);
        return true;
    }
    s->hdma_frames_dropped++;
    qemu_log_mask(LOG_UNIMP,
                  "r100-hdma: READ_REQ %s dropped src=0x%" PRIx64
                  " read_len=%u req_id=0x%x rc=%d\n", tag, src,
                  read_len, req_id, (int)rc);
    return false;
}

/* ------------------------------------------------------------------ */
/* P11 — SMMU stage-2 translate hook                                   */
/* ------------------------------------------------------------------ */

/* SID for HDMA-IPA path (Notion REBELQ SMMU Design § 1: "0–4:
 * DNC(dma) / RBDMA / HDMA-IPA, PF + VF0–3"). v1 hardcodes PF = SID 0;
 * the bypass-LUT "Device HDMA (PA mode)" path lives at SID 16, which
 * STE0.config=BYPASS gives identity for free — we don't need to dispatch
 * by SID for v1. Multi-VF (SID 1..4) is gated on workload + P11 v2. */
#define R100_HDMA_SMMU_SID      0

/*
 * Translate a chiplet-local DVA through the chiplet's r100-smmu.
 * Same shape as `r100_rbdma_translate`: identity passthrough when
 * the SMMU isn't wired or `CR0.SMMUEN=0` (early boot, p5 test
 * harness), else stage-2 walk.
 *
 * `out_pa` is chiplet-local on success. The HDMA caller is
 * responsible for adding `chiplet_id * R100_CHIPLET_OFFSET` if the
 * resulting access goes through `&address_space_memory` (the d2d
 * loop and the LL chain reads do; the host-leg paths chunk through
 * `hdma` chardev frames where the address is interpreted on the
 * remote side and never touches our flat global).
 */
static bool r100_hdma_translate(R100HDMAState *s, hwaddr dva, int access,
                                const char *what, hwaddr *out_pa)
{
    R100SMMUTranslateResult tr = { 0 };

    if (!s->smmu) {
        *out_pa = dva;
        return true;
    }
    r100_smmu_translate(s->smmu, R100_HDMA_SMMU_SID, 0, dva, access, &tr);
    if (!tr.ok) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma cl=%u: SMMU %s on %s dva=0x%" PRIx64
                      " (sid=%u, fault_addr=0x%" PRIx64 ")\n",
                      s->chiplet_id,
                      r100_smmu_fault_str(tr.fault),
                      what, dva, tr.sid, tr.fault_addr);
        r100_hdma_emit_trace(s,
                             "hdma cl=%u smmu_fault %s dva=0x%"
                             PRIx64 " fault=%s\n",
                             s->chiplet_id, what, dva,
                             r100_smmu_fault_str(tr.fault));
        return false;
    }
    *out_pa = tr.pa;
    return true;
}

/* ------------------------------------------------------------------ */
/* SUBCTRL_EDMA_INT_CA73 pending-bit + SPI pulse                       */
/* ------------------------------------------------------------------ */

/*
 * The chiplet-relative SUBCTRL register sits at
 *   chiplet_base + R100_PCIE_SUBCTRL_BASE + R100_PCIE_SUBCTRL_EDMA_INT_CA73_OFF
 * which on chiplet 0 = 0x1FF8184368. r100_create_pcie_subctrl mounts
 * a plain-RAM region there, so we go through the system address
 * space to avoid threading an extra MR pointer onto this device.
 */
static void r100_hdma_signal_completion(R100HDMAState *s,
                                        R100HdmaDir dir, uint32_t ch)
{
    hwaddr subctrl_addr = (hwaddr)s->chiplet_id * R100_CHIPLET_OFFSET +
                          R100_PCIE_SUBCTRL_BASE +
                          R100_PCIE_SUBCTRL_EDMA_INT_CA73_OFF;
    uint32_t bit = 1u << (dir * R100_HDMA_CH_COUNT + ch);
    uint32_t cur;
    MemTxResult mr;

    mr = address_space_read(&address_space_memory, subctrl_addr,
                            MEMTXATTRS_UNSPECIFIED, &cur, sizeof(cur));
    if (mr != MEMTX_OK) {
        cur = 0;
    }
    cur |= bit;
    mr = address_space_write(&address_space_memory, subctrl_addr,
                             MEMTXATTRS_UNSPECIFIED, &cur, sizeof(cur));
    if (mr != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: SUBCTRL_EDMA pending bit write "
                      "failed addr=0x%" PRIx64 " val=0x%x\n",
                      (uint64_t)subctrl_addr, cur);
    }
    if (s->hdma_irq) {
        qemu_irq_pulse(s->hdma_irq);
    }
    s->channel_completions++;
    qemu_log_mask(LOG_TRACE,
                  "r100-hdma cl=%u signal_completion dir=%s ch=%u "
                  "pending_mask=0x%x completions=%" PRIu64 "\n",
                  s->chiplet_id,
                  dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                  ch, cur, s->channel_completions);
    r100_hdma_emit_trace(s,
                         "hdma cl=%u signal_completion dir=%s ch=%u "
                         "pending_mask=0x%x completions=%" PRIu64 "\n",
                         s->chiplet_id,
                         dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                         ch, cur, s->channel_completions);
}

/* ------------------------------------------------------------------ */
/* MMIO ops                                                            */
/* ------------------------------------------------------------------ */

/* Decode an absolute offset into the 128 KB device window into
 * (direction, channel, register-offset). The dw_hdma_v0 layout is:
 *   slot N (0..31) = N * 0x800
 *   slot 2*ch     = WR ch
 *   slot 2*ch+1   = RD ch
 * Returns false for offsets above the channel area (vsec common —
 * not modelled, treated as RAZ/WI). */
static bool r100_hdma_decode(hwaddr addr, R100HdmaDir *out_dir,
                             uint32_t *out_ch, uint32_t *out_reg)
{
    uint32_t slot;

    if (addr >= 32u * R100_HDMA_CH_STRIDE) {
        return false;
    }
    slot = (uint32_t)(addr / R100_HDMA_CH_STRIDE);
    *out_dir = (slot & 1u) ? R100_HDMA_DIR_RD : R100_HDMA_DIR_WR;
    *out_ch  = slot >> 1;
    *out_reg = (uint32_t)(addr % R100_HDMA_CH_STRIDE);
    return true;
}

static uint32_t r100_hdma_make_req_id(R100HdmaDir dir, uint32_t ch)
{
    return R100_HDMA_REQ_ID_CH_MASK_BASE |
           ((dir == R100_HDMA_DIR_RD) ? R100_HDMA_REQ_ID_CH_DIR_RD : 0u) |
           (ch & R100_HDMA_REQ_ID_CH_NUM_MASK);
}

static bool r100_hdma_decode_req_id(uint32_t req_id, R100HdmaDir *out_dir,
                                    uint32_t *out_ch)
{
    if ((req_id & 0xC0u) != R100_HDMA_REQ_ID_CH_MASK_BASE) {
        return false;
    }
    *out_dir = (req_id & R100_HDMA_REQ_ID_CH_DIR_RD) ? R100_HDMA_DIR_RD
                                                     : R100_HDMA_DIR_WR;
    *out_ch  = req_id & R100_HDMA_REQ_ID_CH_NUM_MASK;
    return *out_ch < R100_HDMA_CH_COUNT;
}

/* WR doorbell: pull payload from chiplet-local sysmem and emit
 * OP_WRITE. Mark STOPPED + signal completion synchronously — q-cp's
 * driver is happy with a fire-and-forget hand-off when the IRQ
 * arrives "soon" after the doorbell. */
static void r100_hdma_kick_wr(R100HDMAState *s, uint32_t ch)
{
    R100HdmaChan *c = &s->ch[R100_HDMA_DIR_WR][ch];
    uint32_t req_id = r100_hdma_make_req_id(R100_HDMA_DIR_WR, ch);
    uint64_t npu_phys = (uint64_t)s->chiplet_id * R100_CHIPLET_OFFSET +
                        c->sar;
    uint64_t host_dst = r100_hdma_strip_host_phys(c->dar);
    uint8_t *buf;
    MemTxResult mr;

    if (c->xfer_size == 0 || c->xfer_size > REMU_HDMA_MAX_PAYLOAD) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: WR ch=%u bad xfer_size=%u\n", ch,
                      c->xfer_size);
        s->doorbells_dropped++;
        c->status = R100_HDMA_STATUS_ABORTED;
        c->int_status |= R100_HDMA_INT_ABORT_BIT;
        r100_hdma_signal_completion(s, R100_HDMA_DIR_WR, ch);
        return;
    }
    buf = g_malloc(c->xfer_size);
    mr = address_space_read(&address_space_memory, npu_phys,
                            MEMTXATTRS_UNSPECIFIED, buf, c->xfer_size);
    if (mr != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: WR ch=%u sar=0x%" PRIx64
                      " read failed\n", ch, npu_phys);
        c->status = R100_HDMA_STATUS_ABORTED;
        c->int_status |= R100_HDMA_INT_ABORT_BIT;
        s->doorbells_dropped++;
        g_free(buf);
        r100_hdma_signal_completion(s, R100_HDMA_DIR_WR, ch);
        return;
    }
    if (!r100_hdma_emit_write_tagged(s, req_id, host_dst, buf,
                                     c->xfer_size, "ch-wr")) {
        c->status = R100_HDMA_STATUS_ABORTED;
        c->int_status |= R100_HDMA_INT_ABORT_BIT;
        s->doorbells_dropped++;
        g_free(buf);
        r100_hdma_signal_completion(s, R100_HDMA_DIR_WR, ch);
        return;
    }
    g_free(buf);
    c->status = R100_HDMA_STATUS_STOPPED;
    c->int_status |= R100_HDMA_INT_STOP_BIT;
    s->doorbells_started++;
    r100_hdma_signal_completion(s, R100_HDMA_DIR_WR, ch);
}

/* RD doorbell: emit OP_READ_REQ; status stays RUNNING until the
 * matching OP_READ_RESP arrives in r100_hdma_dispatch_resp. */
static void r100_hdma_kick_rd(R100HDMAState *s, uint32_t ch)
{
    R100HdmaChan *c = &s->ch[R100_HDMA_DIR_RD][ch];
    uint32_t req_id = r100_hdma_make_req_id(R100_HDMA_DIR_RD, ch);
    uint64_t host_src = r100_hdma_strip_host_phys(c->sar);

    if (c->xfer_size == 0 || c->xfer_size > REMU_HDMA_MAX_PAYLOAD) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: RD ch=%u bad xfer_size=%u\n", ch,
                      c->xfer_size);
        s->doorbells_dropped++;
        c->status = R100_HDMA_STATUS_ABORTED;
        c->int_status |= R100_HDMA_INT_ABORT_BIT;
        r100_hdma_signal_completion(s, R100_HDMA_DIR_RD, ch);
        return;
    }
    c->pending_req_id = req_id;
    c->status = R100_HDMA_STATUS_RUNNING;
    if (!r100_hdma_emit_read_req(s, req_id, host_src, c->xfer_size,
                                 "ch-rd")) {
        c->status = R100_HDMA_STATUS_ABORTED;
        c->int_status |= R100_HDMA_INT_ABORT_BIT;
        s->doorbells_dropped++;
        r100_hdma_signal_completion(s, R100_HDMA_DIR_RD, ch);
        return;
    }
    s->doorbells_started++;
}

/* ------------------------------------------------------------------ */
/* P5: LL-mode walk                                                    */
/* ------------------------------------------------------------------ */

/* Run one chunk of a host→NPU LLI: emit OP_READ_REQ, park on the
 * channel's resp_cond until r100_hdma_complete_rd_resp signals, then
 * write the payload at dar_npu. Returns false on any failure (chardev
 * disconnect, address_space_write fault). The caller is responsible
 * for the chunk-loop bookkeeping (offsets) and for clearing
 * c->pending_req_id once the LLI's chunks are exhausted. */
static bool r100_hdma_lli_rd_chunk(R100HDMAState *s, uint32_t ch,
                                   uint64_t sar_host, uint64_t dar_npu,
                                   uint32_t chunk_len)
{
    R100HdmaChan *c = &s->ch[R100_HDMA_DIR_RD][ch];
    uint32_t req_id = r100_hdma_make_req_id(R100_HDMA_DIR_RD, ch);
    MemTxResult mr;

    c->walk.resp_ready = false;
    c->walk.resp_len = 0;
    c->pending_req_id = req_id;

    if (!r100_hdma_emit_read_req(s, req_id, sar_host, chunk_len,
                                 "ll-rd")) {
        return false;
    }

    /* BQL released across the wait so the chardev iothread can
     * deliver OP_READ_RESP — same pattern as r100-pcie-outbound. */
    while (!c->walk.resp_ready) {
        qemu_cond_wait_bql(&c->walk.resp_cond);
    }
    if (c->walk.resp_len < chunk_len) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: LL RD ch=%u short resp got=%u "
                      "want=%u dar=0x%" PRIx64 "\n",
                      ch, c->walk.resp_len, chunk_len, dar_npu);
        /* Still write whatever came back — q-cp's CB walker will
         * surface garbage in the destination, which is preferable
         * to silently corrupting the channel state. */
    }
    mr = address_space_write(&address_space_memory, dar_npu,
                             MEMTXATTRS_UNSPECIFIED, c->walk.resp_buf,
                             c->walk.resp_len);
    if (mr != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: LL RD ch=%u dar=0x%" PRIx64
                      " write failed\n", ch, dar_npu);
        return false;
    }
    return true;
}

static bool r100_hdma_lli_rd(R100HDMAState *s, uint32_t ch,
                             uint64_t sar_host, uint64_t dar_npu,
                             uint32_t len)
{
    uint32_t off = 0;
    hwaddr   dar_pa;

    /* P11: translate the NPU-side DAR; SAR is the host bus address.
     * Same one-shot translate + block-contiguity assumption as the
     * WR / D2D helpers — see comment in r100_hdma_lli_wr. */
    if (!r100_hdma_translate(s, dar_npu, R100_SMMU_ACCESS_WRITE,
                             "ll-rd-dar", &dar_pa)) {
        return false;
    }

    while (off < len) {
        uint32_t chunk = MIN(len - off, REMU_HDMA_MAX_PAYLOAD);
        if (!r100_hdma_lli_rd_chunk(s, ch, sar_host + off,
                                    dar_pa + off, chunk)) {
            return false;
        }
        off += chunk;
    }
    return true;
}

static bool r100_hdma_lli_wr(R100HDMAState *s, uint32_t ch,
                             uint64_t sar_npu, uint64_t dar_host,
                             uint32_t len)
{
    uint32_t req_id = r100_hdma_make_req_id(R100_HDMA_DIR_WR, ch);
    uint8_t  buf[REMU_HDMA_MAX_PAYLOAD];
    uint32_t off = 0;
    MemTxResult mr;
    hwaddr   sar_pa;

    /* P11: translate the NPU-side SAR through the chiplet's SMMU
     * (DAR is the host bus address — never goes through this SMMU,
     * the host-side r100-npu-pci handles that side). v1 translates
     * once and assumes the transfer fits in one mapped block; q-sys's
     * runtime_pt uses 2 MB / 1 GB block descriptors so this holds in
     * practice (TODO: chunked translate for v2 if a workload crosses
     * a block boundary). */
    if (!r100_hdma_translate(s, sar_npu, R100_SMMU_ACCESS_READ,
                             "ll-wr-sar", &sar_pa)) {
        return false;
    }

    while (off < len) {
        uint32_t chunk = MIN(len - off, REMU_HDMA_MAX_PAYLOAD);
        mr = address_space_read(&address_space_memory, sar_pa + off,
                                MEMTXATTRS_UNSPECIFIED, buf, chunk);
        if (mr != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-hdma: LL WR ch=%u sar=0x%" PRIx64
                          " (pa=0x%" PRIx64 ") read failed\n", ch,
                          sar_npu + off, (uint64_t)(sar_pa + off));
            return false;
        }
        if (!r100_hdma_emit_write_tagged(s, req_id, dar_host + off, buf,
                                         chunk, "ll-wr")) {
            return false;
        }
        off += chunk;
    }
    return true;
}

/* D2D / NPU-internal LLI copy. Both SAR and DAR are NPU-side DVAs
 * that go through the chiplet's SMMU on real silicon (see file-banner
 * SMMU note). 64 KB scratch on the stack is comfortably below
 * FreeRTOS_CP* task stacks (we're running on a vCPU thread, not in
 * a guest task). Larger LLIs loop the scratch.
 *
 * `g_alloca` would be clearer but linux/posix `alloca` is
 * size-bounded by the platform stack — keep this on a fixed-size
 * static buffer to avoid surprises on stripped-down hosts. */
static bool r100_hdma_lli_d2d(R100HDMAState *s, uint32_t ch,
                              uint64_t sar_npu, uint64_t dar_npu,
                              uint32_t len)
{
    uint8_t  buf[R100_HDMA_D2D_CHUNK];
    uint32_t off = 0;
    MemTxResult mr;
    hwaddr   sar_pa;
    hwaddr   dar_pa;

    /* P11: both endpoints are NPU-side, both go through the chiplet's
     * SMMU. v1 translates once and assumes block-contiguity (FW
     * runtime_pt's 2 MB / 1 GB block descriptors hold this). v2 will
     * walk per-page if a workload crosses block boundaries. */
    if (!r100_hdma_translate(s, sar_npu, R100_SMMU_ACCESS_READ,
                             "ll-d2d-sar", &sar_pa)) {
        return false;
    }
    if (!r100_hdma_translate(s, dar_npu, R100_SMMU_ACCESS_WRITE,
                             "ll-d2d-dar", &dar_pa)) {
        return false;
    }

    while (off < len) {
        uint32_t chunk = MIN(len - off, sizeof(buf));
        mr = address_space_read(&address_space_memory, sar_pa + off,
                                MEMTXATTRS_UNSPECIFIED, buf, chunk);
        if (mr != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-hdma: LL D2D ch=%u sar=0x%" PRIx64
                          " (pa=0x%" PRIx64 ") read failed\n", ch,
                          sar_npu + off, (uint64_t)(sar_pa + off));
            return false;
        }
        mr = address_space_write(&address_space_memory, dar_pa + off,
                                 MEMTXATTRS_UNSPECIFIED, buf, chunk);
        if (mr != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-hdma: LL D2D ch=%u dar=0x%" PRIx64
                          " (pa=0x%" PRIx64 ") write failed\n", ch,
                          dar_npu + off, (uint64_t)(dar_pa + off));
            return false;
        }
        off += chunk;
    }
    return true;
}

/* Walk one LL chain rooted at c->llp. Returns true iff the chain
 * terminated on an LIE LLI (q-cp's "last data element" marker); false
 * on any read/write error, malformed cycle bit, or runaway chain
 * (R100_HDMA_LL_MAX_ELEMS guard).
 *
 * Element discrimination: the first 4 bytes of both shapes are the
 * `control` u32. For dw_hdma_v0_lli (24 B) the layout is
 * {control, transfer_size, sar.reg, dar.reg}; for dw_hdma_v0_llp
 * (16 B) it's {control, reserved, llp.reg}. We pre-read 24 bytes
 * (the larger of the two) and reuse the field positions accordingly.
 * For an LLP element `transfer_size` slot is the reserved word and
 * `sar` slot is the next-chain `llp.reg` — see qman_if_common.h. */
static bool r100_hdma_walk_ll(R100HDMAState *s, R100HdmaDir dir,
                              uint32_t ch)
{
    R100HdmaChan *c = &s->ch[dir][ch];
    uint64_t cursor = c->llp;
    int elems = 0;
    bool last_seen = false;

    if (cursor == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: LL walk dir=%d ch=%u with LLP=0\n",
                      (int)dir, ch);
        return false;
    }

    qemu_log_mask(LOG_TRACE,
                  "r100-hdma cl=%u ll_walk_start dir=%s ch=%u "
                  "llp=0x%" PRIx64 "\n",
                  s->chiplet_id,
                  dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                  ch, cursor);
    r100_hdma_emit_trace(s,
                         "hdma cl=%u ll_walk_start dir=%s ch=%u "
                         "llp=0x%" PRIx64 "\n",
                         s->chiplet_id,
                         dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                         ch, cursor);

    c->walk.ll_active = true;

    while (cursor != 0 && elems < R100_HDMA_LL_MAX_ELEMS) {
        struct {
            uint32_t control;
            uint32_t transfer_size;
            uint64_t sar;
            uint64_t dar;
        } QEMU_PACKED elem;
        MemTxResult mr;
        bool ok = true;
        hwaddr cursor_pa;

        /* P11: cursor (LL element address) is itself an NPU-side
         * IPA — q-cp's `cmd_descr_hdma` programs the channel's LLP
         * register with a stage-2 IPA (cb[1]'s 0x42b86740 lives in
         * the SYSTEM IPA window). Translate before fetching the
         * 24-byte element. */
        if (!r100_hdma_translate(s, cursor, R100_SMMU_ACCESS_READ,
                                 "ll-cursor", &cursor_pa)) {
            c->walk.ll_active = false;
            return false;
        }

        mr = address_space_read(&address_space_memory, cursor_pa,
                                MEMTXATTRS_UNSPECIFIED, &elem,
                                sizeof(elem));
        if (mr != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-hdma: LL walk read elem ch=%u "
                          "addr=0x%" PRIx64 " (pa=0x%" PRIx64
                          ") failed\n", ch, (uint64_t)cursor,
                          (uint64_t)cursor_pa);
            r100_hdma_emit_trace(s,
                                 "hdma cl=%u ll_walk_read_fail dir=%s "
                                 "ch=%u cursor=0x%" PRIx64
                                 " pa=0x%" PRIx64 " mr=%d\n",
                                 s->chiplet_id,
                                 dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                                 ch, (uint64_t)cursor,
                                 (uint64_t)cursor_pa, (int)mr);
            c->walk.ll_active = false;
            return false;
        }
        elems++;

        /* Always emit raw read for diagnostic — even when we break
         * on CB=0 below, we need to see what we actually read. */
        r100_hdma_emit_trace(s,
                             "hdma cl=%u ll_walk_read dir=%s ch=%u "
                             "elem=%d cursor=0x%" PRIx64
                             " ctrl=0x%08x xsize=%u sar=0x%" PRIx64
                             " dar=0x%" PRIx64 "\n",
                             s->chiplet_id,
                             dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                             ch, elems, (uint64_t)cursor,
                             elem.control, elem.transfer_size,
                             elem.sar, elem.dar);

        /* Cycle-bit invalid → end of chain (or trailing zero LLP
         * record_llp(0,0) terminator). q-cp always sets CB on the
         * LLI/LLP elements it builds. */
        if (!(elem.control & R100_HDMA_LL_CTRL_CB)) {
            r100_hdma_emit_trace(s,
                                 "hdma cl=%u ll_walk_break_no_cb dir=%s "
                                 "ch=%u elem=%d ctrl=0x%08x\n",
                                 s->chiplet_id,
                                 dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                                 ch, elems, elem.control);
            break;
        }

        /* LLP shape: jump to next chain head. The dw_hdma_v0_llp
         * layout puts llp.reg at offset +8, which lines up with our
         * `sar` slot in the over-read above. */
        if (elem.control & R100_HDMA_LL_CTRL_LLP) {
            cursor = elem.sar;
            continue;
        }

        /* LLI shape: dispatch by SAR/DAR direction. */
        {
            bool sar_host = (elem.sar >= REMU_HOST_PHYS_BASE);
            bool dar_host = (elem.dar >= REMU_HOST_PHYS_BASE);
            const char *route =
                (sar_host && dar_host) ? "host-host"
                : (dir == R100_HDMA_DIR_WR && dar_host) ? "wr-host-leg"
                : (dir == R100_HDMA_DIR_RD && sar_host) ? "rd-host-leg"
                : "d2d";

            qemu_log_mask(LOG_TRACE,
                          "r100-hdma cl=%u ll_lli dir=%s ch=%u "
                          "elem=%d sar=0x%" PRIx64 " dar=0x%" PRIx64
                          " size=%u ctrl=0x%08x route=%s\n",
                          s->chiplet_id,
                          dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                          ch, elems, elem.sar, elem.dar,
                          elem.transfer_size, elem.control, route);

            if (elem.transfer_size == 0) {
                /* Empty LLI — q-cp doesn't emit these in the
                 * cmd_descr_hdma path but the test_hdma_if unit
                 * tests do; treat as no-op. */
            } else if (sar_host && dar_host) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "r100-hdma: LL ch=%u dir=%d both addrs "
                              "host (sar=0x%" PRIx64 " dar=0x%" PRIx64
                              ") — unsupported\n",
                              ch, (int)dir, elem.sar, elem.dar);
                ok = false;
            } else if (dir == R100_HDMA_DIR_WR && dar_host) {
                ok = r100_hdma_lli_wr(
                        s, ch, elem.sar,
                        elem.dar - REMU_HOST_PHYS_BASE,
                        elem.transfer_size);
            } else if (dir == R100_HDMA_DIR_RD && sar_host) {
                ok = r100_hdma_lli_rd(
                        s, ch,
                        elem.sar - REMU_HOST_PHYS_BASE,
                        elem.dar, elem.transfer_size);
            } else {
                /* D2D / NPU-internal copy. Both SAR and DAR are
                 * NPU-side DVAs; r100_hdma_lli_d2d runs them through
                 * `r100_smmu_translate` (P11). When CR0.SMMUEN=0 the
                 * walker is a passthrough so kmd/q-cp's "all entries
                 * bypass-region" regime keeps working. */
                ok = r100_hdma_lli_d2d(s, ch, elem.sar, elem.dar,
                                       elem.transfer_size);
            }
        }

        if (!ok) {
            c->walk.ll_active = false;
            return false;
        }

        if (elem.control & R100_HDMA_LL_CTRL_LIE) {
            last_seen = true;
            break;
        }

        cursor += R100_HDMA_LLI_SIZE;
    }

    c->walk.ll_active = false;
    qemu_log_mask(LOG_TRACE,
                  "r100-hdma cl=%u ll_walk_end dir=%s ch=%u "
                  "elems=%d last_seen=%d\n",
                  s->chiplet_id,
                  dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                  ch, elems, (int)last_seen);
    r100_hdma_emit_trace(s,
                         "hdma cl=%u ll_walk_end dir=%s ch=%u "
                         "elems=%d last_seen=%d cursor=0x%" PRIx64 "\n",
                         s->chiplet_id,
                         dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                         ch, elems, (int)last_seen, (uint64_t)cursor);
    if (!last_seen) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: LL walk dir=%d ch=%u terminated "
                      "without LIE (elems=%d cursor=0x%" PRIx64 ")\n",
                      (int)dir, ch, elems, (uint64_t)cursor);
        return false;
    }
    return true;
}

static void r100_hdma_kick_ll(R100HDMAState *s, R100HdmaDir dir,
                              uint32_t ch)
{
    R100HdmaChan *c = &s->ch[dir][ch];
    bool ok;

    c->status = R100_HDMA_STATUS_RUNNING;
    c->int_status = 0;
    s->doorbells_started++;

    ok = r100_hdma_walk_ll(s, dir, ch);
    if (ok) {
        c->status = R100_HDMA_STATUS_STOPPED;
        c->int_status |= R100_HDMA_INT_STOP_BIT;
    } else {
        c->status = R100_HDMA_STATUS_ABORTED;
        c->int_status |= R100_HDMA_INT_ABORT_BIT;
        s->doorbells_dropped++;
    }
    c->pending_req_id = 0;
    r100_hdma_signal_completion(s, dir, ch);
}

/* ------------------------------------------------------------------ */
/* Doorbell entry                                                      */
/* ------------------------------------------------------------------ */

static void r100_hdma_doorbell(R100HDMAState *s, R100HdmaDir dir,
                               uint32_t ch, uint32_t val)
{
    R100HdmaChan *c = &s->ch[dir][ch];
    bool llen = !!(c->ctrl1 & R100_HDMA_CTRL1_LLEN_BIT);

    qemu_log_mask(LOG_TRACE,
                  "r100-hdma cl=%u doorbell dir=%s ch=%u val=0x%x "
                  "llen=%d ctrl1=0x%08x sar=0x%" PRIx64
                  " dar=0x%" PRIx64 " xfer=%u llp=0x%" PRIx64 "\n",
                  s->chiplet_id,
                  dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                  ch, val, (int)llen, c->ctrl1, c->sar, c->dar,
                  c->xfer_size, c->llp);
    r100_hdma_emit_trace(s,
                         "hdma cl=%u doorbell dir=%s ch=%u val=0x%x "
                         "llen=%d ctrl1=0x%08x sar=0x%" PRIx64
                         " dar=0x%" PRIx64 " xfer=%u llp=0x%" PRIx64 "\n",
                         s->chiplet_id,
                         dir == R100_HDMA_DIR_RD ? "rd" : "wr",
                         ch, val, (int)llen, c->ctrl1, c->sar, c->dar,
                         c->xfer_size, c->llp);

    if (val & R100_HDMA_DB_START_BIT) {
        if (llen) {
            r100_hdma_kick_ll(s, dir, ch);
        } else if (dir == R100_HDMA_DIR_WR) {
            r100_hdma_kick_wr(s, ch);
        } else {
            r100_hdma_kick_rd(s, ch);
        }
        return;
    }
    if (val & R100_HDMA_DB_STOP_BIT) {
        /* DW HDMA STOP semantics: if a transfer was running, force
         * it to stop and raise INT_ABORT. If the channel was idle
         * (CH_REG_ENABLE = 0), STOP is silently absorbed — no IRQ,
         * no SUBCTRL_EDMA_INT_CA73 pending bit, no int_status update.
         *
         * q-cp's `hdma_init_channels` posts STOP to all 32 channels
         * at boot *before* any channel is enabled, so c->enable=0
         * for that pass. If we surfaced those as ABORT IRQs, q-cp's
         * `hdma_forward_irq` later picks the lowest pending bit
         * (always WR ch 0 from init pollution), dispatches into
         * `hdma_irq_handler` with no `chan->cb_tcb`, and the *real*
         * completion's `cb_pkt_done` (RD ch 0 LL walk) never fires —
         * pkt_pend_cnt stays >0, cb_complete stays parked behind
         * wait_barrier, and the host's TDR eventually times out
         * (= P10 cb[1] never completes — observed in
         * output/p10-bigdram/hils.log: `pi[2] ... done 1` with
         * `last_di 1` and "send MSIx interrupt to host" missing).
         *
         * The status-field check (RUNNING) is unreliable as a gate
         * because R100_HDMA_STATUS_RUNNING == 0 is also the cold-
         * reset value, so init-time STOPs would still fall through.
         * `c->enable` is set explicitly by q-cp via the ENABLE reg
         * before the START doorbell, so it's a reliable proxy for
         * "this channel had a transfer to interrupt". */
        if (c->enable & R100_HDMA_ENABLE_BIT) {
            c->status = R100_HDMA_STATUS_ABORTED;
            c->int_status |= R100_HDMA_INT_ABORT_BIT;
            r100_hdma_signal_completion(s, dir, ch);
        }
    }
}

static uint64_t r100_hdma_read(void *opaque, hwaddr addr, unsigned size)
{
    R100HDMAState *s = R100_HDMA(opaque);
    R100HdmaDir dir;
    uint32_t ch, reg;
    R100HdmaChan *c;

    if (!r100_hdma_decode(addr, &dir, &ch, &reg)) {
        return 0;
    }
    c = &s->ch[dir][ch];
    switch (reg) {
    case R100_HDMA_CH_REG_ENABLE:        return c->enable;
    case R100_HDMA_CH_REG_DOORBELL:      return c->doorbell;
    case R100_HDMA_CH_REG_ELEM_PF:       return c->elem_pf;
    case R100_HDMA_CH_REG_HANDSHAKE:     return c->handshake;
    case R100_HDMA_CH_REG_LLP_LO:        return (uint32_t)c->llp;
    case R100_HDMA_CH_REG_LLP_HI:        return (uint32_t)(c->llp >> 32);
    case R100_HDMA_CH_REG_CYCLE:         return c->cycle;
    case R100_HDMA_CH_REG_XFER_SIZE:     return c->xfer_size;
    case R100_HDMA_CH_REG_SAR_LO:        return (uint32_t)c->sar;
    case R100_HDMA_CH_REG_SAR_HI:        return (uint32_t)(c->sar >> 32);
    case R100_HDMA_CH_REG_DAR_LO:        return (uint32_t)c->dar;
    case R100_HDMA_CH_REG_DAR_HI:        return (uint32_t)(c->dar >> 32);
    case R100_HDMA_CH_REG_WATERMARK:     return c->watermark;
    case R100_HDMA_CH_REG_CTRL1:         return c->ctrl1;
    case R100_HDMA_CH_REG_FUNC_NUM:      return c->func_num;
    case R100_HDMA_CH_REG_QOS:           return c->qos;
    case R100_HDMA_CH_REG_STATUS:        return c->status;
    case R100_HDMA_CH_REG_INT_STATUS:    return c->int_status;
    case R100_HDMA_CH_REG_INT_SETUP:     return c->int_setup;
    case R100_HDMA_CH_REG_INT_CLEAR:     return 0;
    case R100_HDMA_CH_REG_MSI_STOP_LO:
        return (uint32_t)c->msi_stop;
    case R100_HDMA_CH_REG_MSI_STOP_HI:
        return (uint32_t)(c->msi_stop >> 32);
    case R100_HDMA_CH_REG_MSI_WATERMARK_LO:
        return (uint32_t)c->msi_watermark;
    case R100_HDMA_CH_REG_MSI_WATERMARK_HI:
        return (uint32_t)(c->msi_watermark >> 32);
    case R100_HDMA_CH_REG_MSI_ABORT_LO:
        return (uint32_t)c->msi_abort;
    case R100_HDMA_CH_REG_MSI_ABORT_HI:
        return (uint32_t)(c->msi_abort >> 32);
    case R100_HDMA_CH_REG_MSI_MSGD:      return c->msi_msgd;
    default:                              return 0;
    }
}

static void r100_hdma_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    R100HDMAState *s = R100_HDMA(opaque);
    R100HdmaDir dir;
    uint32_t ch, reg;
    R100HdmaChan *c;
    uint32_t v32 = (uint32_t)val;

    if (!r100_hdma_decode(addr, &dir, &ch, &reg)) {
        return;
    }
    c = &s->ch[dir][ch];
    switch (reg) {
    case R100_HDMA_CH_REG_ENABLE:
        c->enable = v32;
        break;
    case R100_HDMA_CH_REG_DOORBELL:
        c->doorbell = v32;
        r100_hdma_doorbell(s, dir, ch, v32);
        break;
    case R100_HDMA_CH_REG_ELEM_PF:
        c->elem_pf = v32;
        break;
    case R100_HDMA_CH_REG_HANDSHAKE:
        c->handshake = v32;
        break;
    case R100_HDMA_CH_REG_LLP_LO:
        c->llp = (c->llp & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case R100_HDMA_CH_REG_LLP_HI:
        c->llp = (c->llp & 0x00000000FFFFFFFFULL) |
                 ((uint64_t)v32 << 32);
        break;
    case R100_HDMA_CH_REG_CYCLE:
        c->cycle = v32;
        break;
    case R100_HDMA_CH_REG_XFER_SIZE:
        c->xfer_size = v32;
        break;
    case R100_HDMA_CH_REG_SAR_LO:
        c->sar = (c->sar & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case R100_HDMA_CH_REG_SAR_HI:
        c->sar = (c->sar & 0x00000000FFFFFFFFULL) |
                 ((uint64_t)v32 << 32);
        break;
    case R100_HDMA_CH_REG_DAR_LO:
        c->dar = (c->dar & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case R100_HDMA_CH_REG_DAR_HI:
        c->dar = (c->dar & 0x00000000FFFFFFFFULL) |
                 ((uint64_t)v32 << 32);
        break;
    case R100_HDMA_CH_REG_WATERMARK:
        c->watermark = v32;
        break;
    case R100_HDMA_CH_REG_CTRL1:
        c->ctrl1 = v32;
        break;
    case R100_HDMA_CH_REG_FUNC_NUM:
        c->func_num = v32;
        break;
    case R100_HDMA_CH_REG_QOS:
        c->qos = v32;
        break;
    case R100_HDMA_CH_REG_INT_SETUP:
        c->int_setup = v32;
        break;
    case R100_HDMA_CH_REG_INT_CLEAR:
        c->int_status &= ~v32;
        break;
    case R100_HDMA_CH_REG_MSI_STOP_LO:
        c->msi_stop = (c->msi_stop & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case R100_HDMA_CH_REG_MSI_STOP_HI:
        c->msi_stop = (c->msi_stop & 0x00000000FFFFFFFFULL) |
                      ((uint64_t)v32 << 32);
        break;
    case R100_HDMA_CH_REG_MSI_WATERMARK_LO:
        c->msi_watermark = (c->msi_watermark & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case R100_HDMA_CH_REG_MSI_WATERMARK_HI:
        c->msi_watermark = (c->msi_watermark & 0x00000000FFFFFFFFULL) |
                           ((uint64_t)v32 << 32);
        break;
    case R100_HDMA_CH_REG_MSI_ABORT_LO:
        c->msi_abort = (c->msi_abort & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case R100_HDMA_CH_REG_MSI_ABORT_HI:
        c->msi_abort = (c->msi_abort & 0x00000000FFFFFFFFULL) |
                       ((uint64_t)v32 << 32);
        break;
    case R100_HDMA_CH_REG_MSI_MSGD:
        c->msi_msgd = v32;
        break;
    case R100_HDMA_CH_REG_STATUS:
    case R100_HDMA_CH_REG_INT_STATUS:
    default:
        break;
    }
}

static const MemoryRegionOps r100_hdma_ops = {
    .read = r100_hdma_read,
    .write = r100_hdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* Chardev RX                                                          */
/* ------------------------------------------------------------------ */

static void r100_hdma_complete_rd_resp(R100HDMAState *s,
                                       R100HdmaDir dir, uint32_t ch,
                                       const RemuHdmaHeader *hdr,
                                       const uint8_t *payload)
{
    R100HdmaChan *c = &s->ch[dir][ch];
    uint64_t npu_dst;
    MemTxResult mr;

    /* P5: LL walker has BQL released and is parked on resp_cond.
     * Hand off the payload + signal; the walker proceeds with
     * address_space_write itself and counts the chunk towards the
     * LLI offset. status/SUBCTRL pending is not flipped here —
     * r100_hdma_kick_ll does that after the whole chain finishes. */
    if (c->walk.ll_active) {
        if (hdr->req_id != c->pending_req_id) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-hdma: LL RD ch=%u stray RESP req_id="
                          "0x%x (pending=0x%x)\n",
                          ch, hdr->req_id, c->pending_req_id);
            return;
        }
        c->walk.resp_len = MIN(hdr->len,
                               (uint32_t)sizeof(c->walk.resp_buf));
        memcpy(c->walk.resp_buf, payload, c->walk.resp_len);
        c->walk.resp_ready = true;
        qemu_cond_broadcast(&c->walk.resp_cond);
        return;
    }

    /* Non-LL single-shot path (M9-1c hand-driven test). */
    npu_dst = (uint64_t)s->chiplet_id * R100_CHIPLET_OFFSET + c->dar;
    if (hdr->len != c->xfer_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: RD ch=%u resp len mismatch got=%u "
                      "want=%u\n", ch, hdr->len, c->xfer_size);
    }
    mr = address_space_write(&address_space_memory, npu_dst,
                             MEMTXATTRS_UNSPECIFIED, payload, hdr->len);
    if (mr != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: RD ch=%u dar=0x%" PRIx64
                      " write failed\n", ch, npu_dst);
        c->status = R100_HDMA_STATUS_ABORTED;
        c->int_status |= R100_HDMA_INT_ABORT_BIT;
    } else {
        c->status = R100_HDMA_STATUS_STOPPED;
        c->int_status |= R100_HDMA_INT_STOP_BIT;
    }
    c->pending_req_id = 0;
    r100_hdma_signal_completion(s, dir, ch);
}

static void r100_hdma_dispatch(R100HDMAState *s,
                               const RemuHdmaHeader *hdr,
                               const uint8_t *payload)
{
    R100HdmaDir dir;
    uint32_t ch;

    s->hdma_frames_received++;
    r100_hdma_emit_debug(s, "rx", hdr->op, hdr->req_id, hdr->dst,
                         hdr->len, "resp");

    if (hdr->op != REMU_HDMA_OP_READ_RESP) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-hdma: unexpected op=%s req_id=0x%x "
                      "dst=0x%" PRIx64 " len=%u (only READ_RESP "
                      "dispatched NPU-side)\n",
                      remu_hdma_op_str(hdr->op), hdr->req_id,
                      hdr->dst, hdr->len);
        return;
    }

    if (r100_hdma_decode_req_id(hdr->req_id, &dir, &ch)) {
        r100_hdma_complete_rd_resp(s, dir, ch, hdr, payload);
        return;
    }
    /* Only the channel partition (0x80..0xBF) round-trips
     * OP_READ_RESP through this device today. 0x00..0x7F is reserved
     * with no consumer (legacy cm7 BD-done at 0x01..0x0F retired in
     * P7; P1b cfg-mirror reverse-emit at 0x00 retired with the
     * shm-backed cfg-shadow alias). 0xC0..0xFF was the
     * r100-pcie-outbound chardev partition; that path was retired
     * when r100-pcie-outbound switched to a plain MemoryRegion alias
     * over the shared host-ram backend, so no RESPs should land
     * there anymore. */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "r100-hdma: READ_RESP req_id=0x%x has no matching "
                  "partition (channel 0x80..0xBF)\n",
                  hdr->req_id);
}

static void r100_hdma_receive(void *opaque, const uint8_t *buf, int size)
{
    R100HDMAState *s = opaque;
    const RemuHdmaHeader *hdr;
    const uint8_t *payload;

    while (size > 0) {
        if (remu_hdma_rx_feed(&s->rx, &buf, &size, &hdr, &payload)) {
            r100_hdma_dispatch(s, hdr, payload);
        }
    }
}

static int r100_hdma_can_receive(void *opaque)
{
    R100HDMAState *s = opaque;
    return remu_hdma_rx_headroom(&s->rx);
}

/* ------------------------------------------------------------------ */
/* Realize / reset / properties                                        */
/* ------------------------------------------------------------------ */

static void r100_hdma_realize(DeviceState *dev, Error **errp)
{
    R100HDMAState *s = R100_HDMA(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char name[64];
    uint32_t dir, ch;

    snprintf(name, sizeof(name), "r100-hdma.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_hdma_ops, s,
                          name, R100_HDMA_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->hdma_irq);

    /* P5: per-channel cond for the LL walker's RD parking. Init
     * here (not in reset) so a cluster reset that fires while a vCPU
     * is parked doesn't tear down the cond underneath it — reset just
     * clears the boolean state and broadcasts. */
    for (dir = 0; dir < 2; dir++) {
        for (ch = 0; ch < R100_HDMA_CH_COUNT; ch++) {
            qemu_cond_init(&s->ch[dir][ch].walk.resp_cond);
        }
    }

    if (qemu_chr_fe_backend_connected(&s->hdma_chr)) {
        qemu_chr_fe_set_handlers(&s->hdma_chr, r100_hdma_can_receive,
                                 r100_hdma_receive, NULL, NULL, s, NULL,
                                 true);
    }
}

static void r100_hdma_unrealize(DeviceState *dev)
{
    R100HDMAState *s = R100_HDMA(dev);
    uint32_t dir, ch;

    for (dir = 0; dir < 2; dir++) {
        for (ch = 0; ch < R100_HDMA_CH_COUNT; ch++) {
            qemu_cond_destroy(&s->ch[dir][ch].walk.resp_cond);
        }
    }

    qemu_chr_fe_deinit(&s->hdma_chr, false);
    qemu_chr_fe_deinit(&s->hdma_debug_chr, false);
}

static void r100_hdma_reset(DeviceState *dev)
{
    R100HDMAState *s = R100_HDMA(dev);
    uint32_t dir, ch;

    remu_hdma_rx_reset(&s->rx);

    /* Reset clears the programming state but preserves the conds —
     * memset over the cond is undefined since QemuCond wraps a
     * pthread_cond_t. Field-clear here, then wake any parked walker
     * so it bails out cleanly with resp_ready=false (the walker
     * polls the bool and re-loops, but ll_active=false makes the
     * outer kick path exit on the next iteration). */
    for (dir = 0; dir < 2; dir++) {
        for (ch = 0; ch < R100_HDMA_CH_COUNT; ch++) {
            R100HdmaChan *c = &s->ch[dir][ch];

            c->enable = 0;
            c->doorbell = 0;
            c->elem_pf = 0;
            c->handshake = 0;
            c->llp = 0;
            c->cycle = 0;
            c->xfer_size = 0;
            c->sar = 0;
            c->dar = 0;
            c->watermark = 0;
            c->ctrl1 = 0;
            c->func_num = 0;
            c->qos = 0;
            c->status = 0;
            c->int_status = 0;
            c->int_setup = 0;
            c->msi_stop = 0;
            c->msi_watermark = 0;
            c->msi_abort = 0;
            c->msi_msgd = 0;
            c->pending_req_id = 0;
            c->walk.ll_active = false;
            c->walk.resp_ready = false;
            c->walk.resp_len = 0;
            memset(c->walk.resp_buf, 0, sizeof(c->walk.resp_buf));
            qemu_cond_broadcast(&c->walk.resp_cond);
        }
    }
    /* Counters survive reset by convention (see r100-cm7). */
}

static const VMStateDescription r100_hdma_vmstate = {
    .name = "r100-hdma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(hdma_frames_sent, R100HDMAState),
        VMSTATE_UINT64(hdma_frames_dropped, R100HDMAState),
        VMSTATE_UINT64(hdma_frames_received, R100HDMAState),
        VMSTATE_UINT64(doorbells_started, R100HDMAState),
        VMSTATE_UINT64(doorbells_dropped, R100HDMAState),
        VMSTATE_UINT64(channel_completions, R100HDMAState),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_hdma_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100HDMAState, chiplet_id, 0),
    DEFINE_PROP_CHR("chardev", R100HDMAState, hdma_chr),
    DEFINE_PROP_CHR("debug-chardev", R100HDMAState, hdma_debug_chr),
    DEFINE_PROP_LINK("smmu", R100HDMAState, smmu,
                     TYPE_R100_SMMU, R100SMMUState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_hdma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 PCIe DesignWare HDMA register block (q-cp WR/RD "
               "channel doorbells)";
    dc->realize = r100_hdma_realize;
    dc->unrealize = r100_hdma_unrealize;
    dc->vmsd = &r100_hdma_vmstate;
    device_class_set_legacy_reset(dc, r100_hdma_reset);
    device_class_set_props(dc, r100_hdma_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->user_creatable = false;
}

static const TypeInfo r100_hdma_info = {
    .name          = TYPE_R100_HDMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100HDMAState),
    .class_init    = r100_hdma_class_init,
};

static void r100_hdma_register_types(void)
{
    type_register_static(&r100_hdma_info);
}

type_init(r100_hdma_register_types)
