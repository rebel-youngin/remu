/*
 * REMU - R100 NPU System Emulator
 * Machine type and device model declarations
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef R100_SOC_H
#define R100_SOC_H

#include "hw/sysbus.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "qom/object.h"
#include "r100/remu_addrmap.h"

/* Forward declaration — full def in sysemu/hostmem.h (included only by
 * r100_soc.c, which needs the type name for the memdev link property). */
typedef struct HostMemoryBackend HostMemoryBackend;

/* ========================================================================
 * QOM type names
 * ======================================================================== */

#define TYPE_R100_SOC_MACHINE   "r100-soc-machine"

/* ========================================================================
 * Machine state subclass (adds the optional memdev link used by Phase 2
 * to splice a cross-process memory-backend-file over chiplet 0's DRAM
 * head, so the x86 host guest's BAR0 and the NPU CA73 cores see the
 * same bytes. See docs/roadmap.md Phase 2 / M5 for the wiring details.)
 * ======================================================================== */

struct R100SoCMachineState {
    MachineState parent_obj;

    /* `-machine r100-soc,memdev=<id>` stores the backend id here; the
     * HostMemoryBackend itself is resolved at machine-init time (see
     * r100_soc_init) because memory-backend-* objects are created AFTER
     * -machine options are applied in system/vl.c, so a link property
     * would fail with "Device not found". */
    char *memdev_id;

    /* `-machine r100-soc,doorbell=<chardev-id>` : M6 cross-process
     * doorbell ingress from the x86 host guest's BAR4. Resolved to a
     * Chardev at machine-init time for the same reason memdev is
     * deferred. When unset, the r100-doorbell device is not created
     * (single-QEMU runs are unaffected). */
    char *doorbell_chardev_id;
    /* Optional: debug tail chardev id that the r100-doorbell device
     * echoes each received frame to as an ASCII line. Used by the
     * M6 verification test and humans; does not affect signalling. */
    char *doorbell_debug_chardev_id;

    /* `-machine r100-soc,msix=<chardev-id>` : M7 reverse-direction
     * egress. When set, the machine instantiates an `r100-imsix`
     * device overlayed at R100_PCIE_IMSIX_BASE on the chiplet-0 CPU
     * view. FW writes to the IMSIX doorbell (offset 0xFFC) get
     * forwarded as 8-byte (offset, value) frames to this chardev,
     * matching the M6 frame format but in the opposite direction —
     * host-side r100-npu-pci consumes them and calls msix_notify().
     * Resolved to a Chardev at machine-init time (same late-binding
     * reason as memdev / doorbell). */
    char *msix_chardev_id;
    /* Optional debug tail for r100-imsix (one ASCII line per frame
     * emitted). Mirror of doorbell_debug_chardev_id. */
    char *msix_debug_chardev_id;

    /* `-machine r100-soc,issr=<chardev-id>` : M8 ISSR shadow-egress.
     * When set, every NPU-side write to one of the chiplet-0 PCIE
     * r100-mailbox ISSR0..63 scratch registers (4 KB MMIO block at
     * R100_PCIE_MAILBOX_BASE + 0x80..0x180) emits an 8-byte
     * (offset, value) frame on this chardev. The offset field
     * carries the matching BAR4 offset the host sees (i.e. 0x80 +
     * idx*4, matching the KMD MAILBOX_BASE layout), so host-side
     * r100-npu-pci can write-through bar4_mmio_regs[offset >> 2]
     * without knowing which mailbox instance the frame came from.
     * This is the NPU→host half of the FW_BOOT_DONE handshake;
     * the reverse host→NPU half flows over the existing `doorbell`
     * chardev, since M8 extends r100-doorbell to recognise
     * MAILBOX_BASE-range offsets and write them through to the
     * mailbox's ISSR register file (no interrupt). Resolved to a
     * Chardev at machine-init time (same late-binding reason as
     * memdev / doorbell / msix). */
    char *issr_chardev_id;
    /* Optional debug tail for the ISSR egress (one ASCII line per
     * frame emitted). Mirror of doorbell_debug_chardev_id. */
    char *issr_debug_chardev_id;

    /* `-machine r100-soc,cfg=<chardev-id>` : M8b Stage 3b host→NPU BAR2
     * cfg-head mirror. When set, host-side r100-npu-pci installs a
     * 4 KB MMIO trap at BAR2 offset FW_LOGBUF_SIZE and forwards every
     * write to this chardev as an 8-byte (cfg_off, val) frame; NPU-side
     * r100-doorbell consumes them into cfg_shadow[] so the CM7 stub
     * can read DDH_BASE_{LO,HI} when synthesising HDMA writes. Same
     * late-binding rule as memdev / doorbell / msix / issr. */
    char *cfg_chardev_id;
    /* Optional debug tail for cfg ingress (one ASCII line per
     * received frame). Mirror of doorbell_debug_chardev_id. */
    char *cfg_debug_chardev_id;

    /* `-machine r100-soc,hdma=<chardev-id>` : M8b Stage 3b NPU→host HDMA
     * executor. When set, r100-doorbell emits variable-length
     * HDMA_OP_WRITE frames (remu_hdma_proto.h) on this chardev; the
     * host-side r100-npu-pci decodes and executes them as
     * pci_dma_write into the kmd's dma_alloc_coherent pages. Used
     * today by the QINIT CM7 stub (fw_version + init_done writes);
     * M9 will reuse it for BD-done completions. Same late-binding
     * rule as memdev / doorbell / msix / issr / cfg. */
    char *hdma_chardev_id;
    /* Optional debug tail for hdma egress (one ASCII line per
     * sent frame). Mirror of doorbell_debug_chardev_id. */
    char *hdma_debug_chardev_id;
};

typedef struct R100SoCMachineState R100SoCMachineState;

DECLARE_INSTANCE_CHECKER(R100SoCMachineState, R100_SOC_MACHINE,
                         TYPE_R100_SOC_MACHINE)

/* ========================================================================
 * QOM type names for the R100 devices.
 *
 * Only the type names live here; each device's state struct and the
 * R100_<dev>() cast macro stay private to its src/machine/r100_<dev>.c
 * file. Other files (e.g., the machine itself, r100-doorbell holding
 * links to the mailbox) access these devices only via opaque Object*
 * / DeviceState* pointers, so they only need the type *name* to
 * resolve links and push instances onto the sysbus.
 *
 * The mailbox is a partial exception: r100-doorbell calls three
 * helpers on it from outside the MMIO path. Those prototypes and the
 * typedef-forward-decl live in r100_mailbox.h (included below) so
 * everyone sees the same API without exposing the internal struct
 * layout.
 * ======================================================================== */

#include "r100_mailbox.h"               /* TYPE_R100_MAILBOX + helpers */

#define TYPE_R100_CMU           "r100-cmu"
#define TYPE_R100_PMU           "r100-pmu"
#define TYPE_R100_SYSREG        "r100-sysreg"
#define TYPE_R100_HBM           "r100-hbm"
#define TYPE_R100_QSPI_BRIDGE   "r100-qspi-bridge"
#define TYPE_R100_QSPI_BOOT     "r100-qspi-boot"
#define TYPE_R100_RBC           "r100-rbc"
#define TYPE_R100_DMA_PL330     "r100-dma-pl330"
#define TYPE_R100_SMMU          "r100-smmu"
#define TYPE_R100_PVT           "r100-pvt"
#define TYPE_R100_LOGBUF        "r100-logbuf-tail"
#define TYPE_R100_DNC_CLUSTER   "r100-dnc-cluster"
#define TYPE_R100_RBDMA         "r100-rbdma"
#define TYPE_R100_DOORBELL      "r100-doorbell"
#define TYPE_R100_IMSIX         "r100-imsix"

#define R100_RBC_BLOCK_SIZE     0x80000ULL  /* 512KB per RBC block */

#endif /* R100_SOC_H */
