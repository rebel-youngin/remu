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

/* INTGR1 bit 7 = REBEL_DOORBELL_QUEUE_INIT
 * (rebellions/rebel/rebel.h:123). kmd rings it right after staging the
 * rbln_device_desc via dma_alloc_coherent; on silicon PCIE_CM7
 * responds by DMA-ing init_done=1 back into host RAM. Used by both
 * the NPU-side CM7 stub (M8b Stage 3b — r100_doorbell.c emits the
 * HDMA write-backs) and by test harness code that impersonates the
 * kmd side. */
#define REMU_DB_QUEUE_INIT_INTGR1_BIT  7u

/* Host-side BAR2 cfg-head MMIO trap layout.
 *
 * Real silicon splits BAR2 into a 2 MB FW logbuf head followed by a
 * dual-ported "device communication space" (rbln_device_cfg;
 * headers/fw/autogen/g_device_communication_space.h). The kmd writes
 * DDH_BASE_{LO,HI} at +0xC0/+0xC4 before ringing INTGR1 bit 7 so the
 * FW knows where the rbln_device_desc lives in host RAM (the DMA
 * address returned by dma_alloc_coherent + HOST_PHYS_BASE).
 *
 * In REMU the NPU-side q-sys never reads BAR2 (we only model CA73 +
 * mailbox), so we only trap the first 4 KB of cfg-head — enough for
 * the DDH_BASE pair. Every write in that range gets mirrored onto a
 * small local register file (so a guest read-back sees its own store)
 * AND forwarded to the NPU-side r100-doorbell as an 8-byte
 * (cfg_off, val) frame, where cfg_off is the BYTE offset within the
 * cfg head (0..0xFFC, naturally-aligned to 4). The NPU maintains a
 * mirror shadow and consults DDH_BASE_{LO,HI} when ringing HDMA. */
#define REMU_BAR2_CFG_HEAD_OFF  0x00200000u   /* FW_LOGBUF_SIZE (rebel.h) */
#define REMU_BAR2_CFG_HEAD_SIZE 0x00001000u   /* 4 KB */

/* DDH = "device descriptor in host" base registers inside cfg head.
 * Offsets from g_device_communication_space.h; copied here so the
 * bridge code doesn't pull in the kmd header tree. */
#define REMU_CFG_DDH_BASE_LO    0x000000C0u
#define REMU_CFG_DDH_BASE_HI    0x000000C4u

/* Fields inside rbln_device_desc (headers/fw/rbln/ddh.h). */
#define REMU_DDH_DRIVER_VERSION_OFF 0x00000024u
#define REMU_DDH_INIT_DONE_OFF      0x00000058u
#define REMU_DDH_FW_VERSION_OFF     0x0000005Cu
#define REMU_DDH_VERSION_MAX        52u

/* kmd iATU fallback: rbln_dma_host_convert adds this to every DMA
 * address the kmd publishes to the NPU (rebellions/rebel/rebel.h
 * HOST_PHYS_BASE). Used to recover the pdev DMA address from the
 * kmd-written DDH_BASE: desc_dma = (hi:lo) - HOST_PHYS_BASE. */
#define REMU_HOST_PHYS_BASE     0x8000000000ULL

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
