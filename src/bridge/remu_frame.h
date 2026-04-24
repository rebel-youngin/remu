/*
 * REMU — shared 8-byte (u32 a, u32 b) cross-QEMU frame codec.
 *
 * The host-side `r100-npu-pci` and NPU-side `r100-cm7` /
 * `r100-imsix` / `r100-mailbox` all speak the same wire format on
 * their Unix-socket chardev bridges:
 *
 *    +---------------+---------------+
 *    |   a (u32 LE)  |   b (u32 LE)  |     total = 8 bytes
 *    +---------------+---------------+
 *
 * Field roles per channel:
 *
 *   doorbell / issr (host→NPU):   a = BAR4 off,           b = value
 *   issr egress      (NPU→host):  a = BAR4 off,           b = ISSR value
 *   msix             (NPU→host):  a = R100_PCIE_IMSIX_DB, b = db_data
 *
 * This header used to be four hand-rolled copies of the same emit /
 * rx-accumulate scaffolding. Making it one set of `static inline`
 * helpers means both arm_ss (NPU side) and system_ss (host side) pick
 * up the same definitions without introducing a new TU that would
 * need to link into both sides of QEMU's meson tree.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef REMU_BRIDGE_REMU_FRAME_H
#define REMU_BRIDGE_REMU_FRAME_H

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "chardev/char-fe.h"

#define REMU_FRAME_SIZE 8

/*
 * Reassembly buffer for one frame. Unix-socket chardevs give us the
 * byte stream in arbitrary-sized chunks, so each receive callback
 * appends into `buf` until `len` reaches REMU_FRAME_SIZE, at which
 * point the 8 bytes decode into (a, b) and `len` resets to 0.
 *
 * Place this on the device state struct and init with remu_frame_rx_reset().
 */
typedef struct RemuFrameRx {
    uint8_t  buf[REMU_FRAME_SIZE];
    uint32_t len;
} RemuFrameRx;

/* Result of an egress attempt — lets each device decide whether the
 * disconnected / short-write case should bump its own dropped-counter. */
typedef enum {
    REMU_FRAME_EMIT_OK = 0,
    REMU_FRAME_EMIT_DISCONNECTED,
    REMU_FRAME_EMIT_SHORT_WRITE,
} RemuFrameEmitResult;

static inline void remu_frame_rx_reset(RemuFrameRx *rx)
{
    rx->len = 0;
}

/* Headroom in bytes — use as the CharBackend `can_receive` callback. */
static inline int remu_frame_rx_headroom(const RemuFrameRx *rx)
{
    return (int)(REMU_FRAME_SIZE - rx->len);
}

/*
 * Consume up to *size bytes from *buf into the accumulator. Returns
 * true iff this call completed a frame, in which case *out_a / *out_b
 * hold the decoded (a, b) pair and the buffer has been reset. Returns
 * false with the accumulator partially filled otherwise.
 *
 * Typical driver: loop while *size > 0, handling every completed
 * frame inside the loop, until the call returns false and size is 0.
 */
static inline bool remu_frame_rx_feed(RemuFrameRx *rx,
                                      const uint8_t **buf, int *size,
                                      uint32_t *out_a, uint32_t *out_b)
{
    uint32_t want, take;

    if (*size <= 0) {
        return false;
    }
    want = REMU_FRAME_SIZE - rx->len;
    take = (uint32_t)*size < want ? (uint32_t)*size : want;
    memcpy(rx->buf + rx->len, *buf, take);
    rx->len += take;
    *buf    += take;
    *size   -= take;

    if (rx->len < REMU_FRAME_SIZE) {
        return false;
    }
    *out_a = ldl_le_p(&rx->buf[0]);
    *out_b = ldl_le_p(&rx->buf[4]);
    rx->len = 0;
    return true;
}

/*
 * Emit a single (a, b) frame on `chr`. Returns OK, DISCONNECTED
 * (backend not attached — always expected during host-guest settle),
 * or SHORT_WRITE (rc != 8 — rare, logged once per drop). Callers own
 * their success/drop counters so the helper stays side-effect-free
 * beyond the write and the short-write log line.
 */
static inline RemuFrameEmitResult
remu_frame_emit(CharBackend *chr, const char *role,
                uint32_t a, uint32_t b)
{
    uint8_t frame[REMU_FRAME_SIZE];
    int rc;

    if (!qemu_chr_fe_backend_connected(chr)) {
        return REMU_FRAME_EMIT_DISCONNECTED;
    }
    stl_le_p(&frame[0], a);
    stl_le_p(&frame[4], b);
    rc = qemu_chr_fe_write(chr, frame, sizeof(frame));
    if (rc != (int)sizeof(frame)) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: frame a=0x%x b=0x%x short-write rc=%d\n",
                      role, a, b, rc);
        return REMU_FRAME_EMIT_SHORT_WRITE;
    }
    return REMU_FRAME_EMIT_OK;
}

#endif /* REMU_BRIDGE_REMU_FRAME_H */
