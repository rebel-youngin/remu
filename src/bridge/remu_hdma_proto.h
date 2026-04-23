/*
 * REMU — HDMA (NPU → host) wire protocol.
 *
 * Silicon: q-sys CM7 issues reads/writes against host physical memory
 * through the iATU + DW_EDMA pair; the TLPs land in the kmd-side
 * dma_alloc_coherent pages. REMU models neither CM7 nor HDMA, so the
 * NPU side instead emits "please write these bytes at this host DMA
 * address" frames to the x86 host QEMU, which executes them via
 * pci_dma_write on the `r100-npu-pci` endpoint's DMA address space.
 *
 * One-way NPU→host channel. Variable-length frames:
 *
 *   +0  u32 magic   = 'HDMA' little-endian (sync / framing paranoia)
 *   +4  u32 op      = REMU_HDMA_OP_WRITE
 *   +8  u64 dst     = destination DMA address on host
 *   +16 u32 len     = payload length in bytes
 *   +20 u32 flags   = reserved = 0
 *   +24 u8  data[len]
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
    REMU_HDMA_OP_WRITE = 1u,
};

typedef struct RemuHdmaHeader {
    uint32_t magic;
    uint32_t op;
    uint64_t dst;
    uint32_t len;
    uint32_t flags;
} RemuHdmaHeader;

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
        rx->hdr.magic = ldl_le_p(&rx->hdr_buf[0]);
        rx->hdr.op    = ldl_le_p(&rx->hdr_buf[4]);
        rx->hdr.dst   = ldq_le_p(&rx->hdr_buf[8]);
        rx->hdr.len   = ldl_le_p(&rx->hdr_buf[16]);
        rx->hdr.flags = ldl_le_p(&rx->hdr_buf[20]);
        rx->hdr_ready = true;
        rx->payload_len = 0;

        if (rx->hdr.magic != REMU_HDMA_MAGIC ||
            rx->hdr.len > REMU_HDMA_MAX_PAYLOAD) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "remu-hdma: bad frame magic=0x%x op=%u "
                          "dst=0x%" PRIx64 " len=%u\n",
                          rx->hdr.magic, rx->hdr.op, rx->hdr.dst,
                          rx->hdr.len);
            remu_hdma_rx_reset(rx);
            return false;
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
 * Emit one HDMA_OP_WRITE frame. Returns OK / DISCONNECTED / SHORT_WRITE
 * mirroring remu_frame_emit's contract. Two qemu_chr_fe_write calls
 * (header + payload) so the caller doesn't need a scratch buffer; the
 * chardev layer is a byte stream so the two writes coalesce on the
 * peer side exactly like one write of (hdr||payload). len=0 skips the
 * payload write — used by future REMU_HDMA_OP_* that carry no data.
 */
typedef enum {
    REMU_HDMA_EMIT_OK = 0,
    REMU_HDMA_EMIT_DISCONNECTED,
    REMU_HDMA_EMIT_SHORT_WRITE,
} RemuHdmaEmitResult;

static inline RemuHdmaEmitResult
remu_hdma_emit_write(CharBackend *chr, const char *role,
                     uint64_t dst, const void *payload, uint32_t len)
{
    uint8_t hdr[REMU_HDMA_HDR_SIZE];
    int rc;

    if (!qemu_chr_fe_backend_connected(chr)) {
        return REMU_HDMA_EMIT_DISCONNECTED;
    }
    stl_le_p(&hdr[0],  REMU_HDMA_MAGIC);
    stl_le_p(&hdr[4],  REMU_HDMA_OP_WRITE);
    stq_le_p(&hdr[8],  dst);
    stl_le_p(&hdr[16], len);
    stl_le_p(&hdr[20], 0u);

    rc = qemu_chr_fe_write(chr, hdr, sizeof(hdr));
    if (rc != (int)sizeof(hdr)) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: hdma hdr short-write dst=0x%" PRIx64
                      " len=%u rc=%d\n", role, dst, len, rc);
        return REMU_HDMA_EMIT_SHORT_WRITE;
    }
    if (len > 0) {
        rc = qemu_chr_fe_write(chr, (const uint8_t *)payload, len);
        if (rc != (int)len) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: hdma payload short-write dst=0x%" PRIx64
                          " len=%u rc=%d\n", role, dst, len, rc);
            return REMU_HDMA_EMIT_SHORT_WRITE;
        }
    }
    return REMU_HDMA_EMIT_OK;
}

#endif /* REMU_BRIDGE_REMU_HDMA_PROTO_H */
