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
#include "chardev/char-fe.h"
#include "remu_addrmap.h"

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
};

typedef struct R100SoCMachineState R100SoCMachineState;

DECLARE_INSTANCE_CHECKER(R100SoCMachineState, R100_SOC_MACHINE,
                         TYPE_R100_SOC_MACHINE)

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
#define TYPE_R100_MAILBOX       "r100-mailbox"
#define TYPE_R100_IMSIX         "r100-imsix"
#define TYPE_R100_UNIMPL        "r100-unimpl"

#define R100_RBC_BLOCK_SIZE     0x80000ULL  /* 512KB per RBC block */

/* ========================================================================
 * CMU (Clock Management Unit) device
 * ======================================================================== */

#define R100_CMU_REG_COUNT      (R100_CMU_BLOCK_SIZE / 4)

struct R100CMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_CMU_REG_COUNT];
    uint32_t chiplet_id;
    char *name;  /* e.g., "CP0", "ROT", "DCL0" for debug */
};

typedef struct R100CMUState R100CMUState;

DECLARE_INSTANCE_CHECKER(R100CMUState, R100_CMU, TYPE_R100_CMU)

/* ========================================================================
 * PMU (Power Management Unit) device
 * ======================================================================== */

#define R100_PMU_REG_SIZE       0x10000
#define R100_PMU_REG_COUNT      (R100_PMU_REG_SIZE / 4)

struct R100PMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_PMU_REG_COUNT];
    uint32_t chiplet_id;
    uint32_t secondary_chiplet_count;
};

/*
 * Pull in the ARM power-control helpers (arm_set_cpu_on / off) so the
 * PMU can release vCPUs from reset in response to CPU_CONFIGURATION
 * writes. Matches what TF-A's `plat_pmu_cpu_on()` does on silicon —
 * writing LOCAL_PWR_ON to CPU_CONFIGURATION is the reset-release signal.
 */

typedef struct R100PMUState R100PMUState;

DECLARE_INSTANCE_CHECKER(R100PMUState, R100_PMU, TYPE_R100_PMU)

/* ========================================================================
 * SYSREG (System Register / Chiplet ID) device
 * ======================================================================== */

#define R100_SYSREG_REG_SIZE    0x10000
#define R100_SYSREG_REG_COUNT   (R100_SYSREG_REG_SIZE / 4)

struct R100SysregState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_SYSREG_REG_COUNT];
    uint32_t chiplet_id;
};

typedef struct R100SysregState R100SysregState;

DECLARE_INSTANCE_CHECKER(R100SysregState, R100_SYSREG, TYPE_R100_SYSREG)

/* ========================================================================
 * HBM3 controller stub
 * ======================================================================== */

/*
 * HBM3 controller covers 16 memory channels at 0x40000 stride plus
 * 16 PHY blocks at 0x10000 stride plus the ICON block, all in a
 * contiguous 6MB window from 0x1FF7400000. See hbm3.h for the layout.
 * The stub returns 0xFFFFFFFF for unwritten offsets (which satisfies
 * dfi_init_complete and other "training ready" polls) and remembers
 * individual writes in a sparse hash so ioctl-style RMW patterns work.
 */
#define R100_HBM_REG_SIZE       0x600000

struct R100HBMState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    GHashTable *regs;     /* hwaddr -> uint32_t (sparse write-back store) */
    uint32_t chiplet_id;
    uint8_t extest_mode;  /* current ICON EXTEST scan: see r100_hbm.c */
};

typedef struct R100HBMState R100HBMState;

DECLARE_INSTANCE_CHECKER(R100HBMState, R100_HBM, TYPE_R100_HBM)

/* ========================================================================
 * PL330 DMA controller stub
 *
 * Fake-completion stub: BL1's dma_load_image() polls ch_stat[0].csr and
 * dbgcmd for completion. Returning zero on those reads satisfies the poll.
 * Destination contents are never consumed (RBC stubs report UCIe link-up
 * without running PHY microcode), so no real memcpy is performed.
 * ======================================================================== */

#define R100_DMA_REG_COUNT      (R100_DMA_PL330_SIZE / 4)

struct R100DMAPl330State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_DMA_REG_COUNT];
    uint32_t chiplet_id;
};

typedef struct R100DMAPl330State R100DMAPl330State;

DECLARE_INSTANCE_CHECKER(R100DMAPl330State, R100_DMA_PL330, TYPE_R100_DMA_PL330)

/* ========================================================================
 * SMMU-600 TCU stub (Arm SMMU-v3 register block at TCU_OFFSET = 0x1FF4200000)
 *
 * BL2's smmu_early_init() programs the event queue / strtab / global
 * bypass and enables event queues via CR0. Without an ack-mirror device
 * it hangs on `while (!(cr0ack & EVENTQEN_MASK))`. We implement a tiny
 * register file that mirrors CR0 into CR0ACK and auto-clears the GBPA
 * UPDATE bit so the complete-poll exits.
 *
 * FreeRTOS's EL1 SMMU driver (external/.../q/sys/drivers/smmu/smmu.c)
 * also posts CMD_SYNC entries to the command queue in DRAM and polls
 * the entry slot waiting for the SMMU to overwrite it with the MSI
 * data (0). Without any CMDQ processing the poll times out 1000 times
 * a second and floods `[smmu] Failed to sync` into the HILS log. We
 * cache the NS CMDQ base + size (from SMMU_CMDQ_BASE), and on every
 * producer-side write to SMMU_CMDQ_PROD walk the newly-enqueued
 * entries: for each CMD_SYNC with CS=SIG_IRQ we write a 32-bit 0 to
 * the MSI address encoded in cmd[1] (which the FW points back at the
 * CMD_SYNC slot itself), then advance SMMU_CMDQ_CONS to match PROD so
 * the producer sees all commands consumed.
 * ======================================================================== */

#define R100_SMMU_REG_SIZE      0x10000
#define R100_SMMU_REG_COUNT     (R100_SMMU_REG_SIZE / 4)

struct R100SMMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_SMMU_REG_COUNT];
    uint32_t chiplet_id;

    /* Cached CMDQ geometry from the last SMMU_CMDQ_BASE write. */
    uint64_t cmdq_base_pa;      /* PA of first entry in guest memory */
    uint32_t cmdq_log2size;     /* log2(#entries); valid range 0..19 */
};

typedef struct R100SMMUState R100SMMUState;

DECLARE_INSTANCE_CHECKER(R100SMMUState, R100_SMMU, TYPE_R100_SMMU)

/* ========================================================================
 * PVT controller stub (Samsung Process-Voltage-Temperature monitor)
 *
 * FreeRTOS's pvt_init() (external/.../drivers/pvt_con/pvt_con.c) spins on
 * PVT_CON_STATUS.ps_con_idle / ts_con_idle inside PVT_ENABLE_*_CONTROLLER
 * macros, and bounded-waits on per-sensor ps_valid/vs_valid/ts_valid bits.
 * Without a device stub the status register is unmodelled and the unbounded
 * idle-poll hangs the primary CPU just after FreeRTOS enters.
 *
 * The stub is a 64 KB register file that:
 *   - returns 0x3 on reads of PVT_CON_STATUS (+0x1C) so the ps/ts idle
 *     polls (bits 0 & 1) exit immediately;
 *   - returns 0x1 on per-sensor *_status reads so PVT_WAIT_UNTIL_VALID
 *     exits on the first iteration instead of burning its 10 000-count
 *     timeout per sensor;
 *   - reads/write-backs everything else.
 * ======================================================================== */

#define R100_PVT_REG_SIZE       0x10000
#define R100_PVT_REG_COUNT      (R100_PVT_REG_SIZE / 4)

struct R100PVTState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_PVT_REG_COUNT];
    uint32_t chiplet_id;
    char *name;  /* e.g. "ROT", "DCL0_0", "DCL0_1" for debug */
};

typedef struct R100PVTState R100PVTState;

DECLARE_INSTANCE_CHECKER(R100PVTState, R100_PVT, TYPE_R100_PVT)

/* ========================================================================
 * Samsung IPM mailbox SFR block
 *
 * One 4 KB SFR at a fixed chiplet-relative base. Layout matches
 * struct ipm_samsung in external/ssw-bundle/.../drivers/mailbox/
 * ipm_samsung.h. We emulate MCUCTRL + the two INTGR/INTCR/INTMR/INTSR/
 * INTMSR register groups + 64 x 32-bit ISSR scratch registers. See
 * r100_mailbox.c for the full semantics.
 *
 * Two qemu_irq outputs follow INTMSR0 / INTMSR1 (group 0 targets the
 * "CPU0-side" receiver, group 1 the "CPU1-side"). The machine wires
 * whichever lines are active for the mailbox instance's silicon role;
 * for chiplet 0's PCIE mailbox only group 1 lands on a CA73 GIC SPI
 * (matching mailbox_data[IDX_MAILBOX_PCIE_VF0] cpu_id=CPU1 in the
 * __TARGET_CP==0 table).
 * ======================================================================== */

#define R100_MBX_SFR_SIZE       0x1000   /* 4 KB per SFR block */
#define R100_MBX_ISSR_COUNT     64

struct R100MailboxState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq[2];            /* [0] = INTMSR0, [1] = INTMSR1 */
    char *name;                 /* e.g. "pcie.chiplet0" for debug */

    /* M8 NPU→host ISSR shadow-egress. When `issr_chr` is connected,
     * every FW-initiated ISSR write (MMIO path) is also emitted as
     * an 8-byte (BAR4-offset, value) frame on this chardev so the
     * host-side r100-npu-pci can mirror the write into its BAR4
     * MMIO register file. Writes driven by r100_mailbox_set_issr()
     * (the host→NPU ingress path via r100-doorbell) deliberately
     * skip the emit to avoid echoing frames back at the host. */
    CharBackend issr_chr;
    CharBackend issr_debug_chr;

    uint32_t mcuctrl;
    /* Combined INTGR/INTSR storage: INTGR is W1S into `pending`, INTSR
     * and INTGR reads both return it. */
    uint32_t pending[2];
    uint32_t intmr[2];
    uint32_t mif_init;
    uint32_t is_version;
    uint32_t issr[R100_MBX_ISSR_COUNT];

    /* Observability counters (survive reset, inspectable via HMP). */
    uint64_t intgr_writes[2];
    uint64_t issr_egress_frames;    /* emitted to host (MMIO writes) */
    uint64_t issr_egress_dropped;   /* short write / backend gone */
    uint64_t issr_ingress_writes;   /* set via r100_mailbox_set_issr */
};

typedef struct R100MailboxState R100MailboxState;

DECLARE_INSTANCE_CHECKER(R100MailboxState, R100_MAILBOX, TYPE_R100_MAILBOX)

/*
 * Inject a pending-bit set from outside the MMIO path. Used by the
 * M6 doorbell bridge: a chardev frame from the x86 host's BAR4
 * MAILBOX_INTGR write arrives on the NPU side, and we want the same
 * effect as if the FW had issued a store to this mailbox's INTGR
 * register at offset 0x8 (group=0) / 0x1c (group=1). Asserts the
 * matching qemu_irq when INTMSR goes non-zero.
 */
void r100_mailbox_raise_intgr(R100MailboxState *s, int group, uint32_t val);

/*
 * Inject an ISSR register write from outside the MMIO path. Used by
 * the M8 extension to r100-doorbell: frames arriving from the x86
 * host-side BAR4 writes into the MAILBOX_BASE payload range update
 * the backing scratch register but must NOT re-emit on the ISSR
 * egress chardev (that would loop the host's own write back at
 * itself). Out-of-range `idx` is a no-op.
 */
void r100_mailbox_set_issr(R100MailboxState *s, uint32_t idx, uint32_t val);

/* ========================================================================
 * Integrated MSI-X trigger (iMSIX-DB) — FW→host reverse-direction.
 *
 * Overlays a 4 KB MMIO page at R100_PCIE_IMSIX_BASE on the chiplet-0
 * CPU view. On a 4-byte write to offset R100_PCIE_IMSIX_DB_OFFSET
 * (0xFFC), emits an 8-byte little-endian frame (offset, value) over
 * its chardev — same wire format as the M6 doorbell, just inverted.
 * The host-side r100-npu-pci consumes these frames and calls
 * msix_notify() for the encoded vector.
 *
 * The device state is private to src/machine/r100_imsix.c (matching
 * the r100-doorbell pattern); only the QOM type name is exported so
 * the machine can instantiate it. Instantiation is conditional on
 * `-machine r100-soc,msix=<chardev-id>` being set (single-QEMU
 * bring-up paths are unaffected).
 * ======================================================================== */

/* ========================================================================
 * Unimplemented region (catch-all for unmapped config space reads)
 * Returns 0 on read, ignores writes, logs access for debugging
 * ======================================================================== */

struct R100UnimplState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    char *name;
    uint64_t size;
};

typedef struct R100UnimplState R100UnimplState;

DECLARE_INSTANCE_CHECKER(R100UnimplState, R100_UNIMPL, TYPE_R100_UNIMPL)

#endif /* R100_SOC_H */
