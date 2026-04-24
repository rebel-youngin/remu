/*
 * REMU — HDMA (NPU <-> host) wire protocol.
 *
 * Silicon: q-sys CM7 issues reads/writes against host physical memory
 * through the iATU + DW_EDMA pair; the TLPs land in the kmd-side
 * dma_alloc_coherent pages. REMU models neither CM7 nor HDMA, so the
 * NPU side instead emits framed operations on the `hdma` chardev. The
 * host-side r100-npu-pci decodes each frame and executes it against
 * the x86 guest's PCI DMA space via pci_dma_{write,read}.
 *
 * The channel was one-way (NPU -> host, OP_WRITE only) up through M8b
 * Stage 3b. Stage 3c (BD-done) needs read-backs (queue_desc, BD, and
 * packet payload) plus a direct write into the host's BAR2 cfg-head
 * shadow (the kmd reads FUNC_SCRATCH from there, not from host RAM),
 * so the channel is now bidirectional with four opcodes:
 *
 *   OP_WRITE     (NPU -> host) pci_dma_write(dst, payload, len)
 *   OP_READ_REQ  (NPU -> host) "please pci_dma_read(src=dst, len) and
 *                               reply with OP_READ_RESP tagged req_id"
 *                               — len is the requested read length; NO
 *                               payload follows the header on the wire.
 *   OP_READ_RESP (host -> NPU) "here's len bytes that were at src=dst
 *                               for req_id"
 *   OP_CFG_WRITE (NPU -> host) "store payload[0] into your BAR2
 *                               cfg-head shadow at offset dst" — this
 *                               is the reverse of the Stage-3b
 *                               host -> NPU cfg forwarding path, used
 *                               so the NPU-side CM7 stub can publish
 *                               FUNC_SCRATCH values the kmd reads via
 *                               rebel_cfg_read().
 *
 * Variable-length frames:
 *
 *   +0  u32 magic   = 'HDMA' little-endian (sync / framing paranoia)
 *   +4  u32 op      = REMU_HDMA_OP_*
 *   +8  u64 dst     = destination DMA addr (WRITE) / source DMA addr
 *                     (READ_REQ, echoed in RESP) / cfg-head byte
 *                     offset (CFG_WRITE)
 *   +16 u32 len     = payload length in bytes (WRITE / READ_RESP /
 *                     CFG_WRITE), OR requested read length for
 *                     READ_REQ — that op carries no payload on the
 *                     wire so rx_feed short-circuits payload
 *                     accumulation after decoding the header.
 *   +20 u32 req_id  = opaque tag echoed from REQ into matching RESP.
 *                     0 is reserved for untagged ops (OP_WRITE /
 *                     OP_CFG_WRITE from the QINIT stub pre-Stage-3c);
 *                     BD-done jobs use req_id = qid + 1.
 *   +24 u8  data[len]  (empty for OP_READ_REQ; mandatory for the rest)
 *
 * REMU_HDMA_HDR_SIZE = 24. Total on-wire per frame = 24 + len.
 *
 * Mirrors `remu_frame.h`'s static-inline-only layout so both arm_ss
 * (NPU side) and system_ss (host side) pick up the same definitions
 * without introducing a new TU to link into both QEMU targets.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef REMU_BRIDGE_REMU_HDMA_PROTO_H
#define REMU_BRIDGE_REMU_HDMA_PROTO_H

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "chardev/char-fe.h"

#define REMU_HDMA_MAGIC       0x414D4448u  /* 'H','D','M','A' LE */
#define REMU_HDMA_HDR_SIZE    24u
#define REMU_HDMA_MAX_PAYLOAD 4096u        /* cap staging buffer; see rx */

enum {
    REMU_HDMA_OP_WRITE     = 1u,
    REMU_HDMA_OP_READ_REQ  = 2u,
    REMU_HDMA_OP_READ_RESP = 3u,
    REMU_HDMA_OP_CFG_WRITE = 4u,
};

typedef struct RemuHdmaHeader {
    uint32_t magic;
    uint32_t op;
    uint64_t dst;
    uint32_t len;
    uint32_t req_id;
} RemuHdmaHeader;

/* Stringify op for debug-tail ASCII lines; returns a short tag so the
 * per-run hdma.log is grep-friendly ("op=WRITE/READ_REQ/READ_RESP/
 * CFG_WRITE/UNK"). */
static inline const char *remu_hdma_op_str(uint32_t op)
{
    switch (op) {
    case REMU_HDMA_OP_WRITE:     return "WRITE";
    case REMU_HDMA_OP_READ_REQ:  return "READ_REQ";
    case REMU_HDMA_OP_READ_RESP: return "READ_RESP";
    case REMU_HDMA_OP_CFG_WRITE: return "CFG_WRITE";
    default:                     return "UNK";
    }
}

/*
 * Reassembly state: fills the 24-byte header first, then slurps `len`
 * payload bytes. Completion fires `cb` with the decoded header and a
 * pointer into the accumulator. Caller-owned; typically embedded on
 * the device state struct and init'd with remu_hdma_rx_reset().
 */
typedef struct RemuHdmaRx {
    uint8_t  hdr_buf[REMU_HDMA_HDR_SIZE];
    uint32_t hdr_len;       /* bytes accumulated into hdr_buf */
    bool     hdr_ready;     /* hdr_buf fully decoded into `hdr` */
    RemuHdmaHeader hdr;
    uint8_t  payload[REMU_HDMA_MAX_PAYLOAD];
    uint32_t payload_len;   /* bytes accumulated into payload */
} RemuHdmaRx;

static inline void remu_hdma_rx_reset(RemuHdmaRx *rx)
{
    rx->hdr_len = 0;
    rx->hdr_ready = false;
    rx->payload_len = 0;
}

/* can_receive() headroom: header bytes first, then payload bytes. */
static inline int remu_hdma_rx_headroom(const RemuHdmaRx *rx)
{
    if (!rx->hdr_ready) {
        return (int)(REMU_HDMA_HDR_SIZE - rx->hdr_len);
    }
    /* Staging buffer has room for REMU_HDMA_MAX_PAYLOAD; feed reports
     * a frame-too-big error before we ever exceed that. */
    if (rx->hdr.len > REMU_HDMA_MAX_PAYLOAD) {
        return 1;
    }
    return (int)(rx->hdr.len - rx->payload_len);
}

/*
 * Consume up to *size bytes. Returns true iff a full frame is ready,
 * in which case:
 *   *out_hdr     = decoded header
 *   *out_payload = pointer into rx->payload (valid until next feed call)
 * rx then auto-resets for the next frame. Returns false otherwise with
 * rx partially filled and size drained. A header with a bad magic or
 * oversize len emits a GUEST_ERROR, resets the accumulator, and keeps
 * returning false so the stream stays usable after the bad frame.
 */
static inline bool remu_hdma_rx_feed(RemuHdmaRx *rx,
                                     const uint8_t **buf, int *size,
                                     const RemuHdmaHeader **out_hdr,
                                     const uint8_t **out_payload)
{
    uint32_t want, take;

    /* Phase 1: accumulate the 24-byte header. */
    if (!rx->hdr_ready) {
        if (*size <= 0) {
            return false;
        }
        want = REMU_HDMA_HDR_SIZE - rx->hdr_len;
        take = (uint32_t)*size < want ? (uint32_t)*size : want;
        memcpy(rx->hdr_buf + rx->hdr_len, *buf, take);
        rx->hdr_len += take;
        *buf        += take;
        *size       -= take;
        if (rx->hdr_len < REMU_HDMA_HDR_SIZE) {
            return false;
        }
        rx->hdr.magic  = ldl_le_p(&rx->hdr_buf[0]);
        rx->hdr.op     = ldl_le_p(&rx->hdr_buf[4]);
        rx->hdr.dst    = ldq_le_p(&rx->hdr_buf[8]);
        rx->hdr.len    = ldl_le_p(&rx->hdr_buf[16]);
        rx->hdr.req_id = ldl_le_p(&rx->hdr_buf[20]);
        rx->hdr_ready = true;
        rx->payload_len = 0;

        if (rx->hdr.magic != REMU_HDMA_MAGIC ||
            rx->hdr.len > REMU_HDMA_MAX_PAYLOAD) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "remu-hdma: bad frame magic=0x%x op=%u "
                          "dst=0x%" PRIx64 " len=%u req_id=%u\n",
                          rx->hdr.magic, rx->hdr.op, rx->hdr.dst,
                          rx->hdr.len, rx->hdr.req_id);
            remu_hdma_rx_reset(rx);
            return false;
        }
        /* OP_READ_REQ is header-only on the wire — len is the
         * requested read length, not a payload size, so complete
         * the frame as soon as the header is decoded. Every other
         * opcode falls through to the payload-accumulation phase. */
        if (rx->hdr.op == REMU_HDMA_OP_READ_REQ) {
            *out_hdr     = &rx->hdr;
            *out_payload = rx->payload; /* unused; len==read_len */
            rx->hdr_len = 0;
            rx->hdr_ready = false;
            rx->payload_len = 0;
            return true;
        }
    }

    /* Phase 2: slurp payload. len=0 is legal (completes immediately). */
    if (rx->payload_len < rx->hdr.len) {
        if (*size <= 0) {
            return false;
        }
        want = rx->hdr.len - rx->payload_len;
        take = (uint32_t)*size < want ? (uint32_t)*size : want;
        memcpy(rx->payload + rx->payload_len, *buf, take);
        rx->payload_len += take;
        *buf            += take;
        *size           -= take;
        if (rx->payload_len < rx->hdr.len) {
            return false;
        }
    }

    *out_hdr     = &rx->hdr;
    *out_payload = rx->payload;
    /* Keep the header + payload readable through the caller's frame
     * handler, then reset on next feed entry — no, simpler: reset now
     * after copying out the pointers; the caller must consume within
     * this invocation (it does — r100_hdma_deliver runs synchronously). */
    rx->hdr_len = 0;
    rx->hdr_ready = false;
    rx->payload_len = 0;
    return true;
}

/*
 * Emit one HDMA frame. Returns OK / DISCONNECTED / SHORT_WRITE
 * mirroring remu_frame_emit's contract. Two qemu_chr_fe_write calls
 * (header + payload) so the caller doesn't need a scratch buffer; the
 * chardev layer is a byte stream so the two writes coalesce on the
 * peer side exactly like one write of (hdr||payload). len=0 skips the
 * payload write (used by OP_READ_REQ and any future zero-payload op).
 */
typedef enum {
    REMU_HDMA_EMIT_OK = 0,
    REMU_HDMA_EMIT_DISCONNECTED,
    REMU_HDMA_EMIT_SHORT_WRITE,
} RemuHdmaEmitResult;

static inline RemuHdmaEmitResult
remu_hdma_emit(CharBackend *chr, const char *role, uint32_t op,
               uint32_t req_id, uint64_t dst,
               const void *payload, uint32_t len)
{
    uint8_t hdr[REMU_HDMA_HDR_SIZE];
    int rc;

    if (!qemu_chr_fe_backend_connected(chr)) {
        return REMU_HDMA_EMIT_DISCONNECTED;
    }
    stl_le_p(&hdr[0],  REMU_HDMA_MAGIC);
    stl_le_p(&hdr[4],  op);
    stq_le_p(&hdr[8],  dst);
    stl_le_p(&hdr[16], len);
    stl_le_p(&hdr[20], req_id);

    rc = qemu_chr_fe_write(chr, hdr, sizeof(hdr));
    if (rc != (int)sizeof(hdr)) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: hdma hdr short-write op=%s dst=0x%" PRIx64
                      " len=%u req_id=%u rc=%d\n", role,
                      remu_hdma_op_str(op), dst, len, req_id, rc);
        return REMU_HDMA_EMIT_SHORT_WRITE;
    }
    if (len > 0) {
        rc = qemu_chr_fe_write(chr, (const uint8_t *)payload, len);
        if (rc != (int)len) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: hdma payload short-write op=%s dst=0x%"
                          PRIx64 " len=%u req_id=%u rc=%d\n", role,
                          remu_hdma_op_str(op), dst, len, req_id, rc);
            return REMU_HDMA_EMIT_SHORT_WRITE;
        }
    }
    return REMU_HDMA_EMIT_OK;
}

/* ----- thin op-specific wrappers ------------------------------------- */

/* NPU -> host bulk write (M8b 3b QINIT stub + M8b 3c BD-done commit). */
static inline RemuHdmaEmitResult
remu_hdma_emit_write(CharBackend *chr, const char *role,
                     uint64_t dst, const void *payload, uint32_t len)
{
    return remu_hdma_emit(chr, role, REMU_HDMA_OP_WRITE,
                          0u, dst, payload, len);
}

/* Same as above but with a req_id (M8b 3c BD-done). */
static inline RemuHdmaEmitResult
remu_hdma_emit_write_tagged(CharBackend *chr, const char *role,
                            uint32_t req_id, uint64_t dst,
                            const void *payload, uint32_t len)
{
    return remu_hdma_emit(chr, role, REMU_HDMA_OP_WRITE,
                          req_id, dst, payload, len);
}

/* NPU -> host read request. The on-wire frame is header-only: the
 * `len` field carries the REQUESTED read length, and no payload
 * bytes follow (matching the rx_feed short-circuit above). The host
 * answers with OP_READ_RESP of the same length, echoing req_id +
 * src addr. Using remu_hdma_emit directly would append len bytes of
 * payload, so we hand-roll the header write here. */
static inline RemuHdmaEmitResult
remu_hdma_emit_read_req(CharBackend *chr, const char *role,
                        uint32_t req_id, uint64_t src, uint32_t read_len)
{
    uint8_t hdr[REMU_HDMA_HDR_SIZE];
    int rc;

    if (!qemu_chr_fe_backend_connected(chr)) {
        return REMU_HDMA_EMIT_DISCONNECTED;
    }
    stl_le_p(&hdr[0],  REMU_HDMA_MAGIC);
    stl_le_p(&hdr[4],  REMU_HDMA_OP_READ_REQ);
    stq_le_p(&hdr[8],  src);
    stl_le_p(&hdr[16], read_len);
    stl_le_p(&hdr[20], req_id);
    rc = qemu_chr_fe_write(chr, hdr, sizeof(hdr));
    if (rc != (int)sizeof(hdr)) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: hdma READ_REQ hdr short-write src=0x%" PRIx64
                      " read_len=%u req_id=%u rc=%d\n",
                      role, src, read_len, req_id, rc);
        return REMU_HDMA_EMIT_SHORT_WRITE;
    }
    return REMU_HDMA_EMIT_OK;
}

/* Host -> NPU read response. `src` should echo the REQ's `src` so the
 * NPU-side dispatcher can sanity-check the address. */
static inline RemuHdmaEmitResult
remu_hdma_emit_read_resp(CharBackend *chr, const char *role,
                         uint32_t req_id, uint64_t src,
                         const void *payload, uint32_t len)
{
    return remu_hdma_emit(chr, role, REMU_HDMA_OP_READ_RESP,
                          req_id, src, payload, len);
}

/* NPU -> host BAR2 cfg-head shadow write (M8b 3c). `cfg_off` is a
 * byte offset within the 4 KB cfg head; `val` is a single u32. */
static inline RemuHdmaEmitResult
remu_hdma_emit_cfg_write(CharBackend *chr, const char *role,
                         uint32_t req_id, uint32_t cfg_off, uint32_t val)
{
    uint8_t buf[4];
    stl_le_p(buf, val);
    return remu_hdma_emit(chr, role, REMU_HDMA_OP_CFG_WRITE,
                          req_id, (uint64_t)cfg_off, buf, sizeof(buf));
}

#endif /* REMU_BRIDGE_REMU_HDMA_PROTO_H */
