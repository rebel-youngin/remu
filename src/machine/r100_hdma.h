/*
 * REMU - R100 NPU System Emulator
 * DesignWare dw_hdma_v0 register-block model — public helper API
 *
 * Companion to r100_hdma.c. Exposes the QOM type name (declared in
 * r100_soc.h) plus an opaque typedef and the small set of helpers
 * other NPU-side models call against the chardev backend r100-hdma
 * owns.
 *
 * As of M9-1c r100-hdma is the single owner of the `hdma` chardev:
 *
 *   - r100-cm7 (Stage 3a/3b/3c BD-done + QINIT) calls into the
 *     r100_hdma_emit_* helpers below to push frames to the host side.
 *     A registered RX callback is dispatched whenever a matching
 *     OP_READ_RESP arrives (req_id partition 0x01..0x0F).
 *
 *   - q-cp itself drives MMIO-level channel transactions through the
 *     r100-hdma SysBus MMIO region; the device translates each
 *     doorbell into a chardev frame using a disjoint req_id partition
 *     (0x80..0xBF) and consumes its own OP_READ_RESPs locally.
 *
 * Splitting the chardev would have required a second host-side
 * frontend — QEMU CharBackends are single-frontend — so r100-hdma
 * mediates and r100-cm7 reaches it via a QOM link.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef R100_HDMA_H
#define R100_HDMA_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "remu_hdma_proto.h"

typedef struct R100HDMAState R100HDMAState;

/*
 * Emit one OP_WRITE / OP_READ_REQ / OP_CFG_WRITE frame on the chardev.
 * Returns true on success, false on disconnected backend or short
 * write (counters incremented on the device for either path; caller
 * may log additional context). `req_id` must follow the partitioning
 * documented in remu_hdma_proto.h — passing a value outside the
 * caller's allocated range is a programming error and emits
 * LOG_GUEST_ERROR but the frame still goes out.
 *
 * The `tag` string is purely for the optional debug-tail trace; pass
 * "" if you don't care.
 */
bool r100_hdma_emit_write_tagged(R100HDMAState *s, uint32_t req_id,
                                 uint64_t dst, const void *payload,
                                 uint32_t len, const char *tag);
bool r100_hdma_emit_read_req(R100HDMAState *s, uint32_t req_id,
                             uint64_t src, uint32_t read_len,
                             const char *tag);
bool r100_hdma_emit_cfg_write(R100HDMAState *s, uint32_t req_id,
                              uint32_t cfg_off, uint32_t val,
                              const char *tag);

/*
 * Register response callbacks for OP_READ_RESP frames whose req_id
 * falls in an "external subscriber" partition. r100-hdma's RX demux
 * always handles its own MMIO-channel partition (0x80..0xBF) and the
 * untagged QINIT path internally; everything else is routed by
 * partition through the registered callbacks. Setting `cb=NULL`
 * unbinds.
 *
 * Each callback runs synchronously inside the chardev RX context
 * (BQL held) so it must not block on any other lock. r100-cm7's
 * BD-done state machine and r100-pcie-outbound's sync-read pump are
 * the only callers and re-enter r100_hdma_emit_* freely.
 *
 * Partition map (also documented in remu_addrmap.h):
 *
 *   set_cm7_callback      — req_id 1..R100_CM7_MAX_QUEUES (BD-done).
 *   set_outbound_callback — req_id 0xC0..0xFF (P1 PCIe outbound
 *                            iATU-window synchronous reads).
 */
typedef void (*R100HDMARespCb)(void *opaque, const RemuHdmaHeader *hdr,
                               const uint8_t *payload);
void r100_hdma_set_cm7_callback(R100HDMAState *s, R100HDMARespCb cb,
                                void *opaque);
void r100_hdma_set_outbound_callback(R100HDMAState *s, R100HDMARespCb cb,
                                     void *opaque);

#endif /* R100_HDMA_H */
