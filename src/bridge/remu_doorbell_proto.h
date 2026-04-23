/*
 * REMU — shared BAR4 doorbell-offset classifier.
 *
 * Host-side r100-npu-pci has to decide which BAR4 MMIO writes get
 * forwarded to the NPU over the doorbell chardev, and NPU-side
 * r100-doorbell has to route received frames to the matching mailbox
 * action. Both sides asked the same question of the same offset
 * before this header existed; now they use one classifier so the
 * wire protocol stays single-sourced.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef REMU_BRIDGE_REMU_DOORBELL_PROTO_H
#define REMU_BRIDGE_REMU_DOORBELL_PROTO_H

#include "qemu/osdep.h"
#include "r100/remu_addrmap.h"

/*
 * Offsets within BAR4 that have a meaningful remote effect. Any offset
 * not classified here is local-only on the host side (lazy RAM) and
 * a GUEST_ERROR on the NPU side (silicon would NAK the TLP).
 */
typedef enum {
    REMU_DB_KIND_OTHER = 0,  /* not a bridge-relevant offset */
    REMU_DB_KIND_INTGR0,     /* M6 host→NPU mailbox INTGR0 (SOFT_RESET+…) */
    REMU_DB_KIND_INTGR1,     /* M6 host→NPU mailbox INTGR1 (SPI) */
    REMU_DB_KIND_ISSR,       /* M8a host→NPU ISSR scratch payload */
} RemuDoorbellKind;

/*
 * Classify a BAR4 offset. For REMU_DB_KIND_ISSR, *issr_idx_out receives
 * the ISSR register index (0..63); otherwise it's left unchanged.
 * REMU_DB_KIND_OTHER is returned for any offset the bridge doesn't
 * relay — callers on the host side use this to skip the chardev
 * write and on the NPU side to report GUEST_ERROR.
 */
static inline RemuDoorbellKind
remu_doorbell_classify(uint32_t bar4_off, uint32_t *issr_idx_out)
{
    if (bar4_off == R100_BAR4_MAILBOX_INTGR0) {
        return REMU_DB_KIND_INTGR0;
    }
    if (bar4_off == R100_BAR4_MAILBOX_INTGR1) {
        return REMU_DB_KIND_INTGR1;
    }
    if (bar4_off >= R100_BAR4_MAILBOX_BASE &&
        bar4_off <  R100_BAR4_MAILBOX_END &&
        (bar4_off & 0x3u) == 0u) {
        if (issr_idx_out) {
            *issr_idx_out = (bar4_off - R100_BAR4_MAILBOX_BASE) >> 2;
        }
        return REMU_DB_KIND_ISSR;
    }
    return REMU_DB_KIND_OTHER;
}

/* Does this BAR4 offset need to be forwarded to the NPU? */
static inline bool remu_doorbell_is_bridged(uint32_t bar4_off)
{
    return remu_doorbell_classify(bar4_off, NULL) != REMU_DB_KIND_OTHER;
}

#endif /* REMU_BRIDGE_REMU_DOORBELL_PROTO_H */
