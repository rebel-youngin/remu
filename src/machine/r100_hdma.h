/*
 * REMU - R100 NPU System Emulator
 * DesignWare dw_hdma_v0 register-block model — public helper API
 *
 * Companion to r100_hdma.c. Exposes the QOM type name (declared in
 * r100_soc.h) plus an opaque typedef and the small set of helpers
 * other NPU-side models call against the chardev backend r100-hdma
 * owns.
 *
 * As of M9-1c r100-hdma is the single owner of the `hdma` chardev.
 * Two on-NPU senders share the wire under disjoint req_id partitions
 * (canonical map in src/include/r100/remu_addrmap.h):
 *
 *   - q-cp itself drives MMIO-level channel transactions through the
 *     r100-hdma SysBus MMIO region; the device translates each
 *     doorbell into a chardev frame using partition 0x80..0xBF and
 *     consumes its own OP_READ_RESPs locally.
 *
 * Two historical chardev consumers retired with the P10-fix shared-
 * memory plumbing:
 *
 *   - r100-pcie-outbound used to park vCPUs on synchronous PF-window
 *     reads in the 0xC0..0xFF partition with an OP_READ_REQ /
 *     OP_READ_RESP round-trip. It now aliases the host-ram backend
 *     directly, so the partition is reserved.
 *
 *   - r100-cm7 used to push P1b cfg-mirror writes upstream as
 *     OP_CFG_WRITE frames at req_id 0x00. The NPU and host x86 QEMUs
 *     now alias a shared `cfg-shadow` memory-backend-file over their
 *     BAR2 cfg-head / cfg-mirror MMIO traps, so NPU-side stores are
 *     visible to the kmd's next `rebel_cfg_read` with no chardev round
 *     trip; the opcode and partition are reserved.
 *
 * See r100_cm7.c / r100_npu_pci.c / r100_pcie_outbound.c file banners.
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
 * Emit one OP_WRITE / OP_READ_REQ frame on the chardev. Returns true
 * on success, false on disconnected backend or short write (counters
 * incremented on the device for either path; caller may log
 * additional context). `req_id` must follow the partitioning
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

#endif /* R100_HDMA_H */
