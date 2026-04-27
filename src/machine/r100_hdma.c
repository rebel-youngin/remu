/*
 * REMU - R100 NPU System Emulator
 * DesignWare dw_hdma_v0 register-block model (M9-1c).
 *
 * On silicon, q-cp's `hdma_if.c` programs DMA channels in a PCIe-
 * attached DesignWare HDMA controller at
 *
 *     hdma_base = cl_id * CHIPLET_INTERVAL
 *               + U_PCIE_CORE_OFFSET (0x1C00000000)
 *               + PCIE_HDMA_OFFSET   (0x180380000)
 *
 * 16 WR + 16 RD channels each with a small register window (SAR,
 * DAR, XferSize, doorbell, status, int_status). q-cp:
 *   1. fills SAR/DAR/XferSize for a chosen channel,
 *   2. writes HDMA_DB_START_BIT to the doorbell,
 *   3. either polls hdma_ch_read_status until STOPPED or waits for
 *      an MSI on INT_ID_HDMA (= 186, single line shared across all
 *      32 channels; the per-channel "which one finished" bitmap
 *      lives in SUBCTRL_EDMA_INT_CA73 = 0x1FF8184368).
 *
 * REMU has neither the DW HDMA IP nor the actual PCIe RC, so this
 * device emulates the *peripheral effect* over the existing `hdma`
 * chardev (remu_hdma_proto.h). For each WR/RD doorbell:
 *
 *   - WR (NPU → host): we emit OP_WRITE with payload pulled from
 *     chiplet-local sysmem at SAR. dst on the wire is the host DMA
 *     address (DAR, after stripping REMU_HOST_PHYS_BASE — kmd
 *     publishes in either convention; cm7_host_dma() does the same
 *     fix-up). The completion is fire-and-forget: status switches to
 *     STOPPED before the doorbell write returns, the SUBCTRL pending
 *     bit is set, and SPI 186 is pulsed.
 *
 *   - RD (host → NPU): we emit OP_READ_REQ tagged with a
 *     channel-derived req_id (R100_HDMA_REQ_ID_CH_MASK_BASE | dir |
 *     ch). Status stays RUNNING until the matching OP_READ_RESP
 *     lands; on receipt we address_space_write the payload back into
 *     chiplet sysmem at DAR, then flip status + pulse SPI 186.
 *
 * The chardev backend is single-frontend (CharBackend), so r100-hdma
 * is the sole host-side counterpart and r100-cm7 reaches it through
 * a QOM link + the public helpers in r100_hdma.h. Disjoint req_id
 * partitions keep cm7's BD-done responses (1..R100_CM7_MAX_QUEUES)
 * from colliding with this device's channel-driven 0x80..0xBF range.
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

typedef struct R100HdmaChan {
    uint32_t enable;
    uint32_t doorbell;
    uint32_t xfer_size;
    uint64_t sar;            /* WR: NPU phys; RD: host phys */
    uint64_t dar;            /* WR: host phys; RD: NPU phys */
    uint32_t status;         /* RUNNING / STOPPED / ABORTED */
    uint32_t int_status;     /* STOP / ABORT / WATERMARK bits */
    uint32_t pending_req_id; /* req_id assigned to RUNNING RD frame */
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

    /* Optional cm7 RX callback for OP_READ_RESPs in the cm7 req_id
     * partition (1..R100_CM7_MAX_QUEUES). r100-cm7 sets this at
     * realize time via r100_hdma_set_cm7_callback. */
    R100HDMARespCb cm7_cb;
    void *cm7_cb_opaque;

    /* Counters. */
    uint64_t hdma_frames_sent;
    uint64_t hdma_frames_dropped;
    uint64_t hdma_frames_received;

    uint64_t doorbells_started;
    uint64_t doorbells_dropped;
    uint64_t channel_completions;
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

bool r100_hdma_emit_cfg_write(R100HDMAState *s, uint32_t req_id,
                              uint32_t cfg_off, uint32_t val,
                              const char *tag)
{
    RemuHdmaEmitResult rc;

    rc = remu_hdma_emit_cfg_write(&s->hdma_chr, "r100-hdma", req_id,
                                  cfg_off, val);
    if (rc == REMU_HDMA_EMIT_OK) {
        s->hdma_frames_sent++;
        r100_hdma_emit_debug(s, "tx", REMU_HDMA_OP_CFG_WRITE, req_id,
                             cfg_off, 4, tag);
        return true;
    }
    s->hdma_frames_dropped++;
    qemu_log_mask(LOG_UNIMP,
                  "r100-hdma: CFG_WRITE %s dropped off=0x%x val=0x%x "
                  "req_id=0x%x rc=%d\n", tag, cfg_off, val, req_id,
                  (int)rc);
    return false;
}

void r100_hdma_set_cm7_callback(R100HDMAState *s, R100HDMARespCb cb,
                                void *opaque)
{
    s->cm7_cb = cb;
    s->cm7_cb_opaque = opaque;
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

static void r100_hdma_doorbell(R100HDMAState *s, R100HdmaDir dir,
                               uint32_t ch, uint32_t val)
{
    if (val & R100_HDMA_DB_START_BIT) {
        if (dir == R100_HDMA_DIR_WR) {
            r100_hdma_kick_wr(s, ch);
        } else {
            r100_hdma_kick_rd(s, ch);
        }
        return;
    }
    if (val & R100_HDMA_DB_STOP_BIT) {
        R100HdmaChan *c = &s->ch[dir][ch];
        c->status = R100_HDMA_STATUS_ABORTED;
        c->int_status |= R100_HDMA_INT_ABORT_BIT;
        r100_hdma_signal_completion(s, dir, ch);
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
    case R100_HDMA_CH_REG_ENABLE:     return c->enable;
    case R100_HDMA_CH_REG_DOORBELL:   return c->doorbell;
    case R100_HDMA_CH_REG_XFER_SIZE:  return c->xfer_size;
    case R100_HDMA_CH_REG_SAR_LO:     return (uint32_t)c->sar;
    case R100_HDMA_CH_REG_SAR_HI:     return (uint32_t)(c->sar >> 32);
    case R100_HDMA_CH_REG_DAR_LO:     return (uint32_t)c->dar;
    case R100_HDMA_CH_REG_DAR_HI:     return (uint32_t)(c->dar >> 32);
    case R100_HDMA_CH_REG_STATUS:     return c->status;
    case R100_HDMA_CH_REG_INT_STATUS: return c->int_status;
    case R100_HDMA_CH_REG_INT_CLEAR:  return 0;
    default:                           return 0;
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
    case R100_HDMA_CH_REG_INT_CLEAR:
        c->int_status &= ~v32;
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
    uint64_t npu_dst = (uint64_t)s->chiplet_id * R100_CHIPLET_OFFSET +
                       c->dar;
    MemTxResult mr;

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
    /* Otherwise route to the cm7 partition (1..R100_CM7_MAX_QUEUES).
     * BD-done jobs use req_id = qid + 1. */
    if (s->cm7_cb) {
        s->cm7_cb(s->cm7_cb_opaque, hdr, payload);
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "r100-hdma: READ_RESP req_id=0x%x has no handler "
                  "(cm7 cb unbound)\n", hdr->req_id);
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

    snprintf(name, sizeof(name), "r100-hdma.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_hdma_ops, s,
                          name, R100_HDMA_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->hdma_irq);

    if (qemu_chr_fe_backend_connected(&s->hdma_chr)) {
        qemu_chr_fe_set_handlers(&s->hdma_chr, r100_hdma_can_receive,
                                 r100_hdma_receive, NULL, NULL, s, NULL,
                                 true);
    }
}

static void r100_hdma_unrealize(DeviceState *dev)
{
    R100HDMAState *s = R100_HDMA(dev);

    qemu_chr_fe_deinit(&s->hdma_chr, false);
    qemu_chr_fe_deinit(&s->hdma_debug_chr, false);
}

static void r100_hdma_reset(DeviceState *dev)
{
    R100HDMAState *s = R100_HDMA(dev);

    remu_hdma_rx_reset(&s->rx);
    memset(s->ch, 0, sizeof(s->ch));
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
