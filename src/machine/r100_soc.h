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

    /* `-machine r100-soc,host-ram=<id>` : P10-fix splice. The host
     * x86 QEMU's main RAM is itself backed by a shareable
     * memory-backend-file; mounting the same backend on the NPU side
     * lets r100-pcie-outbound's PF-window MR be a plain alias over
     * those bytes (instead of round-tripping iATU reads/writes through
     * the `hdma` chardev). q-cp's `hq_task` then dereferences the kmd's
     * coherent DMA pages as cheaply as a normal RAM load — no
     * BQL-stalled cond_wait, no chardev iothread on the hot path.
     * Resolved lazily at machine-init time (same late-binding
     * reasoning as memdev). */
    char *host_ram_id;

    /* `-machine r100-soc,cfg-shadow=<id>` : P10-fix cfg-mirror splice.
     * 4 KB shareable memory-backend-file aliased over the r100-cm7
     * cfg-mirror trap at DEVICE_COMMUNICATION_SPACE_BASE. The host's
     * r100-npu-pci aliases the same backend over its BAR2 cfg-head
     * subregion, so kmd writes to FUNC_SCRATCH / DDH_BASE_LO are
     * observable on q-cp's next read with no cfg-chardev round trip
     * — eliminates the cfg/doorbell ordering race exposed once the
     * outbound iATU stopped serialising on `hdma`. Resolved lazily
     * at machine-init time (same late-binding reasoning as memdev /
     * host-ram). */
    char *cfg_shadow_id;

    /* `-machine r100-soc,doorbell=<chardev-id>` : M6 cross-process
     * doorbell ingress from the x86 host guest's BAR4. Resolved to a
     * Chardev at machine-init time for the same reason memdev is
     * deferred. When unset, the r100-cm7 device is not created
     * (single-QEMU runs are unaffected). Named "doorbell" because
     * it's the single ingress chardev the device's doorbell RX
     * handler reads. */
    char *doorbell_chardev_id;
    /* Optional: debug tail chardev id that the r100-cm7 device
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
     * chardev, since M8 extends r100-cm7 to recognise
     * MAILBOX_BASE-range offsets and write them through to the
     * mailbox's ISSR register file (no interrupt). Resolved to a
     * Chardev at machine-init time (same late-binding reason as
     * memdev / doorbell / msix). */
    char *issr_chardev_id;
    /* Optional debug tail for the ISSR egress (one ASCII line per
     * frame emitted). Mirror of doorbell_debug_chardev_id. */
    char *issr_debug_chardev_id;

    /* `-machine r100-soc,hdma=<chardev-id>` : NPU→host HDMA executor.
     * Variable-length frames (remu_hdma_proto.h): r100-hdma's
     * MMIO-driven channels publish OP_WRITE / OP_READ_REQ in the
     * 0x80..0xBF req_id partition; host-side r100-npu-pci decodes and
     * executes them as pci_dma_{read,write} and emits OP_READ_RESP
     * back. P10-fix retired the prior r100-cm7 OP_CFG_WRITE
     * reverse-path (req_id 0x00) and the r100-pcie-outbound
     * synchronous PF-window reads (0xC0..0xFF) — both partitions are
     * available again for UMQ multi-queue scaffolding. Same
     * late-binding rule as memdev / doorbell / msix / issr. */
    char *hdma_chardev_id;
    /* Optional debug tail for hdma (one ASCII line per frame sent or
     * received). Mirror of doorbell_debug_chardev_id. */
    char *hdma_debug_chardev_id;

    /* P11: per-chiplet r100-smmu device pointers, populated in
     * r100_create_smmu and read back by the rbdma / hdma instantiation
     * sites so they can wire their `smmu` link to the correct chiplet's
     * TCU. Held as opaque `DeviceState *` here (header doesn't pull in
     * r100_smmu.h to avoid a public dependency on the SMMU type
     * machinery — only the wiring code needs the link target, and it
     * does so via OBJECT(...) casts). NULL slots indicate a chiplet
     * whose SMMU hasn't been instantiated yet (impossible after
     * machine init completes). */
    DeviceState *smmu_dev[R100_NUM_CHIPLETS];

    /* `-machine r100-soc,smmu-debug=<chardev-id>` : optional ASCII
     * trace tail wired to chiplet 0's r100-smmu only (CharBackend is
     * single-frontend; chiplets 1..3 are untouched today since q-cp
     * only programs the chiplet-0 SMMU on the BD lifecycle path).
     * One line per translate / STE decode / PT-walk dispatch / CMDQ
     * op / eventq emit / GERROR raise. Always-on (no -d / --trace
     * dependency). Used by P10 / P11 SMMU post-mortems and the
     * `tests/scripts/smmu_inspect.py` companion script. Mirror of
     * rbdma_debug_chardev_id but for the SMMU TCU. If a future
     * workload exercises the SMMU on chiplet N>0, split the prop
     * into `smmu-debug-cl<N>` and create one chardev per chiplet. */
    char *smmu_debug_chardev_id;

    /* `-machine r100-soc,rbdma-debug=<chardev-id>` : optional ASCII
     * trace tail wired to chiplet 0's r100-rbdma only (CharBackend is
     * single-frontend; chiplets 1..3 don't share — their RBDMAs are
     * untouched today since P10's cb lifecycle runs entirely on
     * chiplet 0's q-cp). One line per kickoff / BH fire / FNSH pop,
     * always-on (no -d / --trace dependency), used by P10
     * cb-lifecycle post-mortems. If a future workload exercises
     * RBDMA on chiplet N>0, split this prop into per-chiplet
     * `rbdma-debug-cl<N>` and create one chardev per chiplet.
     * Mirror of hdma_debug_chardev_id but for the RBDMA engine. */
    char *rbdma_debug_chardev_id;
};

typedef struct R100SoCMachineState R100SoCMachineState;

DECLARE_INSTANCE_CHECKER(R100SoCMachineState, R100_SOC_MACHINE,
                         TYPE_R100_SOC_MACHINE)

/* ========================================================================
 * QOM type names for the R100 devices.
 *
 * Only the type names live here; each device's state struct and the
 * R100_<dev>() cast macro stay private to its src/machine/r100_<dev>.c
 * file. Other files (e.g., the machine itself, r100-cm7 holding
 * links to the mailbox) access these devices only via opaque Object*
 * / DeviceState* pointers, so they only need the type *name* to
 * resolve links and push instances onto the sysbus.
 *
 * The mailbox + imsix are partial exceptions: r100-cm7 calls helpers
 * on both from outside the MMIO path. Those prototypes and the
 * typedef-forward-decls live in r100_mailbox.h / r100_imsix.h
 * (included below) so everyone sees the same API without exposing
 * the internal struct layout.
 * ======================================================================== */

#include "r100_mailbox.h"               /* TYPE_R100_MAILBOX + helpers */
#include "r100_imsix.h"                 /* TYPE_R100_IMSIX + notify helper */
#include "r100_rbdma.h"                 /* TYPE_R100_RBDMA */

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
#define TYPE_R100_CM7           "r100-cm7"
#define TYPE_R100_IMSIX         "r100-imsix"
#define TYPE_R100_HDMA          "r100-hdma"
#define TYPE_R100_PCIE_OUTBOUND "r100-pcie-outbound"

#define R100_RBC_BLOCK_SIZE     0x80000ULL  /* 512KB per RBC block */

#endif /* R100_SOC_H */
