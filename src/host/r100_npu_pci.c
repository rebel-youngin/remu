/*
 * R100 NPU host-side PCIe endpoint (Phase 2, M3).
 *
 * This device model lives in the x86_64 QEMU guest and exposes the
 * R100 NPU to its Linux driver (rebellions.ko, built from
 * external/ssw-bundle/products/common/kmd/). Vendor/device IDs match
 * PCI_VENDOR_ID_REBEL / PCI_ID_CR03 from
 *   external/ssw-bundle/.../common/kmd/rebellions/{common/rebellions.h,
 *                                                 common/rebellions_drv.c}
 * so the stock driver binds to us with no modifications.
 *
 * BAR layout (from external/ssw-bundle/.../kmd/rebellions/rebel/rebel.h
 * and rebel.c:rebel_check_pci_bars_size). Sizes are the NEXT POWER OF 2
 * >= the driver's minimum, which is what PCI BARs require anyway:
 *
 *   BAR0 (DDR,      64-bit prefetch): >= RBLN_DRAM_SIZE  (36 GB)
 *   BAR2 (ACP/SRAM, 64-bit prefetch): >= RBLN_SRAM_SIZE  (64 MB)
 *   BAR4 (Doorbell, 32-bit MMIO):     >= RBLN_PERI_SIZE  (8 MB)
 *   BAR5 (MSI-X,    32-bit MMIO):     >= RBLN_PCIE_SIZE  (1 MB)
 *
 * BAR2 is plain lazily-allocated host RAM (no side effects on
 * writes); BAR4 is normally lazy RAM too, but when the `doorbell`
 * chardev property is wired (M6+) a 4 KB MMIO head overlay
 * intercepts MAILBOX_INTGR0/INTGR1 writes and forwards them as
 * 8-byte (offset, value) frames to the NPU-side r100-doorbell
 * device, which injects a GIC SPI. MAILBOX_BASE (0x80..0x180) and
 * the rest of BAR4 remain RAM-like so the driver's mailbox payload
 * writes don't fault. BAR5 holds the MSI-X table + PBA in its first
 * few KB with plain RAM filling the rest so the driver's size
 * check passes. Lazy allocation keeps the 64 GB BAR0 from actually
 * costing 64 GB of host RSS unless the guest touches every page.
 *
 * BAR0 has two shapes:
 *
 *   - Without `memdev`: plain lazy RAM (M3 behavior). Useful for
 *     bring-up / driver-only experiments where the NPU-side QEMU
 *     isn't involved.
 *
 *   - With `memdev` (M4+): BAR0 is a container MemoryRegion of the
 *     full declared size (64 GB) with the backend's MR added as a
 *     subregion at offset 0 and plain lazy RAM filling the tail.
 *     Both QEMU processes (x86 host + aarch64 NPU) open the same
 *     memory-backend-file with share=on, so writes from the x86
 *     guest into BAR0 offset 0..backend_size land in a page the NPU-
 *     side QEMU also has mmap'd. The NPU-side address-map integration
 *     (so NPU CPUs see those bytes as their DRAM) lands in M5.
 *
 * Later milestones layer real behavior on top:
 *   M5 — alias the same backend into the NPU-side HBM memory map so
 *        NPU CPUs see x86-guest writes as DRAM accesses.  [done]
 *   M6 — hybrid MMIO+RAM BAR4 that forwards MAILBOX_INTGR writes to
 *        the NPU process over a chardev (see `doorbell` property and
 *        src/machine/r100_doorbell.c).                    [done]
 *   M7 — the `msix` chardev consumes 8-byte frames emitted by the
 *        NPU-side r100-imsix device (FW stores to
 *        REBELH_PCIE_MSIX_ADDR = 0x1B_FFFF_FFFC) and calls
 *        msix_notify() for the encoded vector. Mirrors the M6
 *        doorbell wire format but flowing in the opposite direction.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "sysemu/hostmem.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "hw/arm/remu_addrmap.h"

#define R100_PCI_REVISION       0x01
#define R100_PCI_CLASS          0x1200  /* Processing accelerator */

#define R100_BAR0_DDR_SIZE      (64ULL * GiB)  /* >= RBLN_DRAM_SIZE (36 GB) */
#define R100_BAR2_ACP_SIZE      (64ULL * MiB)  /* == RBLN_SRAM_SIZE  */
#define R100_BAR4_DB_SIZE       (8ULL  * MiB)  /* == RBLN_PERI_SIZE  */
#define R100_BAR5_MSIX_SIZE     (1ULL  * MiB)  /* == RBLN_PCIE_SIZE  */

/* REBEL_MSIX_ENTRIES from rebel.h (REBEL_MAX_PORTS*VEC_PER_PORT + GLOBAL,
 * 32 on CR03). Pin to 32 so the msix table fits comfortably below the
 * 64 KB PBA offset we pick in realize. */
#define R100_NUM_MSIX           32
#define R100_MSIX_TABLE_OFF     0x00000
#define R100_MSIX_PBA_OFF       0x10000

#define TYPE_R100_NPU_PCI "r100-npu-pci"
OBJECT_DECLARE_SIMPLE_TYPE(R100NpuPciState, R100_NPU_PCI)

struct R100NpuPciState {
    PCIDevice parent_obj;

    /* When set (via -device r100-npu-pci,memdev=<id>), the backend's
     * MR is added as a subregion of bar0_ddr at offset 0 for cross-
     * process DRAM sharing with the NPU-side QEMU. `bar0_tail` then
     * fills the remainder of BAR0 so the driver's size check passes.
     * When unset, bar0_ddr is a plain RAM region of the full size and
     * bar0_tail is unused. */
    HostMemoryBackend *hostmem;

    /* When set (via -device r100-npu-pci,doorbell=<chardev-id>),
     * BAR4 becomes a container with a small MMIO head that intercepts
     * MAILBOX_INTGR0/INTGR1 writes and emits an 8-byte frame on this
     * chardev, plus lazy RAM filling the rest of BAR4 for backward-
     * compatible mailbox-payload semantics. When unset, the whole BAR
     * is plain RAM (M3/M5 behaviour). */
    CharBackend doorbell_chr;

    /* When set (via -device r100-npu-pci,msix=<chardev-id>, M7+),
     * receives 8-byte (offset, db_data) frames from the NPU-side
     * r100-imsix device. On each complete frame we mask the vector
     * out of db_data[10:0] and call msix_notify() on the PCI device.
     * When unset the chardev is simply not polled — no MSI-X can
     * ever be fired from the NPU side, which matches pre-M7
     * behaviour. */
    CharBackend msix_chr;
    /* Optional: per-frame ASCII tail (debug). */
    CharBackend msix_debug_chr;
    uint8_t msix_rx_buf[8];
    uint32_t msix_rx_len;
    uint64_t msix_frames_received;
    uint64_t msix_frames_dropped;   /* bad offset / vector out of range */
    uint32_t msix_last_db_data;
    uint32_t msix_last_vector;

    MemoryRegion bar0_ddr;
    MemoryRegion bar0_tail;
    MemoryRegion bar2_acp;

    /* BAR4 layout:
     *   bar4_container (8 MB) holds the BAR. When doorbell_chr is
     *   connected, bar4_mmio (4 KB MMIO, priority 10) overlays offset
     *   0..R100_BAR4_MMIO_SIZE and handles INTGR triggers + holds a
     *   backing register file for reads; bar4_ram (8 MB lazy RAM,
     *   priority 0) catches everything else (including MAILBOX_BASE
     *   payload writes). When doorbell_chr is not connected, only
     *   bar4_ram is added at priority 0 for full-BAR RAM semantics. */
    MemoryRegion bar4_container;
    MemoryRegion bar4_mmio;
    MemoryRegion bar4_ram;

    /* Backing for bar4_mmio so reads return the last written value
     * (KMD doesn't read INTGR registers, but we keep consistent
     * semantics across save/restore). R100_BAR4_MMIO_SIZE is 4 KB =
     * 1024 u32s. */
    uint32_t bar4_mmio_regs[R100_BAR4_MMIO_SIZE / 4];
    uint64_t doorbell_frames_sent;

    MemoryRegion bar5_msix;
};

/*
 * Emit an 8-byte (offset, value) frame to the NPU-side doorbell
 * chardev. The NPU process (r100-doorbell) decodes this as "BAR4
 * offset <off> got <val>" and pulses a GIC SPI (see
 * src/machine/r100_doorbell.c). Best-effort write: if the socket is
 * temporarily full we drop the frame and log — silicon doesn't
 * back-pressure on doorbell writes either.
 */
static void r100_doorbell_emit(R100NpuPciState *s, uint32_t off, uint32_t val)
{
    uint8_t frame[8];
    int rc;

    if (!qemu_chr_fe_backend_connected(&s->doorbell_chr)) {
        return;
    }

    stl_le_p(&frame[0], off);
    stl_le_p(&frame[4], val);

    rc = qemu_chr_fe_write(&s->doorbell_chr, frame, sizeof(frame));
    if (rc != sizeof(frame)) {
        qemu_log_mask(LOG_UNIMP,
                      "r100-npu-pci: doorbell frame off=0x%x val=0x%x "
                      "dropped (rc=%d)\n", off, val, rc);
        return;
    }
    s->doorbell_frames_sent++;
}

static uint64_t r100_bar4_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    R100NpuPciState *s = opaque;
    uint32_t idx = (uint32_t)(addr >> 2);

    if (idx >= ARRAY_SIZE(s->bar4_mmio_regs)) {
        return 0;
    }
    return s->bar4_mmio_regs[idx];
}

static void r100_bar4_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    R100NpuPciState *s = opaque;
    uint32_t idx = (uint32_t)(addr >> 2);
    uint32_t v32 = (uint32_t)val;

    if (idx < ARRAY_SIZE(s->bar4_mmio_regs)) {
        s->bar4_mmio_regs[idx] = v32;
    }

    /* Triggers: only the MAILBOX_INTGR* registers forward to the NPU.
     * Everything else in the MMIO window (MAILBOX_BASE payload at
     * 0x80..0x180, etc.) is stash-only — the NPU reads it out of DRAM
     * once it takes the SPI. */
    if (addr == R100_BAR4_MAILBOX_INTGR0 ||
        addr == R100_BAR4_MAILBOX_INTGR1) {
        r100_doorbell_emit(s, (uint32_t)addr, v32);
    }
}

static const MemoryRegionOps r100_bar4_mmio_ops = {
    .read       = r100_bar4_mmio_read,
    .write      = r100_bar4_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ========================================================================
 * M7: MSI-X reverse-direction chardev — consumes frames emitted by
 * the NPU-side r100-imsix device and fires msix_notify() for the
 * encoded vector. Frame format (matches r100-doorbell / r100-imsix):
 *   [0..3]  u32 offset   (always R100_PCIE_IMSIX_DB_OFFSET today)
 *   [4..7]  u32 db_data  (raw value FW wrote to REBELH_PCIE_MSIX_ADDR)
 * vector = db_data & R100_PCIE_IMSIX_VECTOR_MASK (11 bits).
 * ======================================================================== */

static int r100_msix_can_receive(void *opaque)
{
    R100NpuPciState *s = opaque;
    return sizeof(s->msix_rx_buf) - s->msix_rx_len;
}

static void r100_msix_emit_debug(R100NpuPciState *s, uint32_t off,
                                 uint32_t db_data, uint32_t vector,
                                 const char *status)
{
    char line[128];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->msix_debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "msix off=0x%x db_data=0x%x vector=%u status=%s "
                 "count=%" PRIu64 "\n",
                 off, db_data, vector, status,
                 s->msix_frames_received);
    if (n > 0) {
        qemu_chr_fe_write(&s->msix_debug_chr, (const uint8_t *)line, n);
    }
}

static void r100_msix_deliver(R100NpuPciState *s, uint32_t off,
                              uint32_t db_data)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint32_t vector = db_data & R100_PCIE_IMSIX_VECTOR_MASK;

    s->msix_last_db_data = db_data;
    s->msix_last_vector = vector;

    /* The NPU-side r100-imsix only accepts writes to
     * R100_PCIE_IMSIX_DB_OFFSET today, so in practice we'll always
     * see that value here. Validate anyway — a mismatched offset
     * means a frame parser bug or a rogue writer. */
    if (off != R100_PCIE_IMSIX_DB_OFFSET) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-npu-pci: msix frame with unexpected off=0x%x "
                      "db_data=0x%x\n", off, db_data);
        s->msix_frames_dropped++;
        r100_msix_emit_debug(s, off, db_data, vector, "bad-offset");
        return;
    }
    if (vector >= R100_NUM_MSIX) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-npu-pci: msix vector %u out of range "
                      "(db_data=0x%x; max %u)\n",
                      vector, db_data, R100_NUM_MSIX - 1);
        s->msix_frames_dropped++;
        r100_msix_emit_debug(s, off, db_data, vector, "oor");
        return;
    }

    s->msix_frames_received++;

    /* msix_notify is a no-op when MSI-X is disabled by the guest
     * (driver not yet loaded / guest still in SeaBIOS / vector
     * masked); the pending bit is latched in the PBA either way, so
     * this is the correct primitive regardless of MSI-X state. */
    msix_notify(pdev, vector);
    r100_msix_emit_debug(s, off, db_data, vector, "ok");
}

static void r100_msix_receive(void *opaque, const uint8_t *buf, int size)
{
    R100NpuPciState *s = opaque;

    while (size > 0) {
        uint32_t want = sizeof(s->msix_rx_buf) - s->msix_rx_len;
        uint32_t take = size < (int)want ? (uint32_t)size : want;

        memcpy(s->msix_rx_buf + s->msix_rx_len, buf, take);
        s->msix_rx_len += take;
        buf += take;
        size -= take;

        if (s->msix_rx_len == sizeof(s->msix_rx_buf)) {
            uint32_t off = ldl_le_p(s->msix_rx_buf);
            uint32_t db  = ldl_le_p(s->msix_rx_buf + 4);
            s->msix_rx_len = 0;
            r100_msix_deliver(s, off, db);
        }
    }
}

static void r100_npu_pci_realize(PCIDevice *pdev, Error **errp)
{
    R100NpuPciState *s = R100_NPU_PCI(pdev);
    Error *err = NULL;

    /* BAR0 — DDR window. Declared at 64 GB (next pow2 >= 36 GB) so the
     * driver's rebel_check_pci_bars_size() passes. QEMU maps anonymous
     * RAM with MAP_NORESERVE semantics for the tail, so the host
     * doesn't actually commit 64 GB unless the guest touches every
     * page.
     *
     * If `memdev` is wired, splice the backend's MR over offset 0 so
     * stores from the x86 guest land in the shared /dev/shm file that
     * the NPU-side QEMU has mmap'd as well; a plain RAM subregion
     * fills offsets [backend_size, BAR0_SIZE). Otherwise the whole BAR
     * is one plain RAM MR (M3-compatible bring-up path). */
    if (s->hostmem) {
        MemoryRegion *shared = host_memory_backend_get_memory(s->hostmem);
        uint64_t shared_size = memory_region_size(shared);
        if (shared_size == 0 || shared_size > R100_BAR0_DDR_SIZE) {
            error_setg(errp,
                       "r100-npu-pci: memdev size 0x%" PRIx64
                       " is out of range for BAR0 (max 0x%" PRIx64 ")",
                       shared_size, (uint64_t)R100_BAR0_DDR_SIZE);
            return;
        }
        memory_region_init(&s->bar0_ddr, OBJECT(s), "r100.bar0.ddr",
                           R100_BAR0_DDR_SIZE);
        memory_region_add_subregion(&s->bar0_ddr, 0, shared);
        host_memory_backend_set_mapped(s->hostmem, true);
        vmstate_register_ram(shared, DEVICE(s));
        if (shared_size < R100_BAR0_DDR_SIZE) {
            memory_region_init_ram(&s->bar0_tail, OBJECT(s),
                                   "r100.bar0.tail",
                                   R100_BAR0_DDR_SIZE - shared_size, &err);
            if (err) {
                error_propagate(errp, err);
                return;
            }
            memory_region_add_subregion(&s->bar0_ddr, shared_size,
                                        &s->bar0_tail);
        }
    } else {
        memory_region_init_ram(&s->bar0_ddr, OBJECT(s), "r100.bar0.ddr",
                               R100_BAR0_DDR_SIZE, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    }
    pci_register_bar(pdev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->bar0_ddr);

    /* BAR2 — ACP / SRAM window. Holds CP firmware logbuf, CPU config,
     * MMU page tables and (in PF mode) the vserial SMC shim per
     * rebel.c. Plain RAM is sufficient until the NPU-side models
     * actually serve reads here. */
    memory_region_init_ram(&s->bar2_acp, OBJECT(s), "r100.bar2.acp",
                           R100_BAR2_ACP_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    pci_register_bar(pdev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->bar2_acp);

    /* BAR4 — Doorbell window. Always an 8 MB container so the driver's
     * size check (rebel_check_pci_bars_size) passes. Layout depends on
     * whether the `doorbell` chardev is wired:
     *
     *   - With chardev (M6+): small 4 KB MMIO head overlay (priority
     *     10) at offset 0 intercepts MAILBOX_INTGR0/INTGR1 writes and
     *     forwards them as 8-byte frames to the NPU process. The rest
     *     of the 4 KB head is RAM-backed from an internal register
     *     file so the MAILBOX_BASE payload (0x80..0x180) is readable.
     *     A lazy RAM region (priority 0) covers the full 8 MB as a
     *     fallback for offsets outside the MMIO window.
     *   - Without chardev (M3/M5 behaviour): plain 8 MB RAM, no MMIO
     *     overlay. Lets BAR4 mmaps from the guest driver still work
     *     for single-QEMU bring-up runs.
     *
     * 32-bit non-prefetchable to match real PCIe topology. */
    memory_region_init(&s->bar4_container, OBJECT(s),
                       "r100.bar4.container", R100_BAR4_DB_SIZE);
    memory_region_init_ram(&s->bar4_ram, OBJECT(s),
                           "r100.bar4.ram", R100_BAR4_DB_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion_overlap(&s->bar4_container, 0,
                                        &s->bar4_ram, 0);
    if (qemu_chr_fe_backend_connected(&s->doorbell_chr)) {
        memory_region_init_io(&s->bar4_mmio, OBJECT(s),
                              &r100_bar4_mmio_ops, s,
                              "r100.bar4.mmio", R100_BAR4_MMIO_SIZE);
        memory_region_add_subregion_overlap(&s->bar4_container, 0,
                                            &s->bar4_mmio, 10);
    }
    pci_register_bar(pdev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->bar4_container);

    /* BAR5 — MSI-X. Driver requires the BAR to be >= 1 MB, but the
     * actual MSI-X table (32 * 16 B) + PBA (4 B) is tiny. We allocate
     * the full 1 MB as RAM and let msix_init() overlay its MMIO regions
     * for the table (offset 0) and PBA (offset 64 KB); unused area
     * stays as plain RAM so mmap() from the guest still works. */
    memory_region_init_ram(&s->bar5_msix, OBJECT(s), "r100.bar5.msix",
                           R100_BAR5_MSIX_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    pci_register_bar(pdev, 5,
                     PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->bar5_msix);
    if (msix_init(pdev, R100_NUM_MSIX,
                  &s->bar5_msix, 5, R100_MSIX_TABLE_OFF,
                  &s->bar5_msix, 5, R100_MSIX_PBA_OFF,
                  0, errp) < 0) {
        return;
    }

    /* Enable every MSI-X vector so the guest driver's unmask path
     * isn't a prerequisite for host-side msix_notify() to latch a
     * pending bit — matches silicon (PBA bits latch independent of
     * the mask register). */
    for (int i = 0; i < R100_NUM_MSIX; i++) {
        msix_vector_use(pdev, i);
    }

    pci_config_set_interrupt_pin(pdev->config, 1);

    /* M7: If a msix chardev is wired, install the receive handler.
     * Mirror of the doorbell ingress path on the NPU side: complete
     * 8-byte frames get parsed and forwarded to msix_notify(). */
    if (qemu_chr_fe_backend_connected(&s->msix_chr)) {
        qemu_chr_fe_set_handlers(&s->msix_chr,
                                 r100_msix_can_receive,
                                 r100_msix_receive,
                                 NULL,   /* event */
                                 NULL,   /* be_change */
                                 s,
                                 NULL,   /* context */
                                 true);  /* set_open */
    }
}

static void r100_npu_pci_exit(PCIDevice *pdev)
{
    R100NpuPciState *s = R100_NPU_PCI(pdev);
    for (int i = 0; i < R100_NUM_MSIX; i++) {
        msix_vector_unuse(pdev, i);
    }
    msix_uninit(pdev, &s->bar5_msix, &s->bar5_msix);
    if (s->hostmem) {
        MemoryRegion *shared = host_memory_backend_get_memory(s->hostmem);
        vmstate_unregister_ram(shared, DEVICE(s));
        host_memory_backend_set_mapped(s->hostmem, false);
    }
    qemu_chr_fe_deinit(&s->doorbell_chr, false);
    qemu_chr_fe_deinit(&s->msix_chr, false);
    qemu_chr_fe_deinit(&s->msix_debug_chr, false);
}

static Property r100_npu_pci_properties[] = {
    DEFINE_PROP_LINK("memdev", R100NpuPciState, hostmem,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_CHR("doorbell", R100NpuPciState, doorbell_chr),
    DEFINE_PROP_CHR("msix", R100NpuPciState, msix_chr),
    DEFINE_PROP_CHR("msix-debug", R100NpuPciState, msix_debug_chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_npu_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = r100_npu_pci_realize;
    k->exit      = r100_npu_pci_exit;
    k->vendor_id = R100_PCI_VENDOR_ID;
    k->device_id = R100_PCI_DEVICE_ID_CR03;
    k->revision  = R100_PCI_REVISION;
    k->class_id  = R100_PCI_CLASS;

    dc->desc = "R100 NPU (CR03 quad) host-side PCIe endpoint";
    device_class_set_props(dc, r100_npu_pci_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo r100_npu_pci_info = {
    .name          = TYPE_R100_NPU_PCI,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(R100NpuPciState),
    .class_init    = r100_npu_pci_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void r100_npu_pci_register_types(void)
{
    type_register_static(&r100_npu_pci_info);
}

type_init(r100_npu_pci_register_types)
