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
 * BAR2/4 are plain lazily-allocated host RAM (no side effects on
 * writes); BAR5 holds the MSI-X table + PBA in its first few KB with
 * plain RAM filling the rest so the driver's size check passes. Lazy
 * allocation keeps the 64 GB BAR0 from actually costing 64 GB of
 * host RSS unless the guest touches every page.
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
 *        NPU CPUs see x86-guest writes as DRAM accesses.
 *   M6 — swap BAR4 RAM for an MMIO region whose writes signal an
 *        eventfd that the NPU process converts into its local IRQ.
 *   M7 — wire MSI-X back: NPU-side eventfd fires msix_notify() here.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "sysemu/hostmem.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "qapi/error.h"

#define R100_PCI_VENDOR_ID      0x1eff  /* PCI_VENDOR_ID_REBEL */
#define R100_PCI_DEVICE_ID      0x2030  /* PCI_ID_CR03 (CR03 quad) */
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

    MemoryRegion bar0_ddr;
    MemoryRegion bar0_tail;
    MemoryRegion bar2_acp;
    MemoryRegion bar4_doorbell;
    MemoryRegion bar5_msix;
};

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

    /* BAR4 — Doorbell window. Real silicon raises NPU-internal IRQs on
     * writes here; for M3 writes just hit RAM so the driver's doorbell
     * ioctls don't fault. M5 will swap this for an MMIO region whose
     * writes eventfd-signal the NPU-side QEMU. 32-bit non-prefetchable
     * to match real PCIe topology. */
    memory_region_init_ram(&s->bar4_doorbell, OBJECT(s),
                           "r100.bar4.doorbell", R100_BAR4_DB_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    pci_register_bar(pdev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->bar4_doorbell);

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

    pci_config_set_interrupt_pin(pdev->config, 1);
}

static void r100_npu_pci_exit(PCIDevice *pdev)
{
    R100NpuPciState *s = R100_NPU_PCI(pdev);
    msix_uninit(pdev, &s->bar5_msix, &s->bar5_msix);
    if (s->hostmem) {
        MemoryRegion *shared = host_memory_backend_get_memory(s->hostmem);
        vmstate_unregister_ram(shared, DEVICE(s));
        host_memory_backend_set_mapped(s->hostmem, false);
    }
}

static Property r100_npu_pci_properties[] = {
    DEFINE_PROP_LINK("memdev", R100NpuPciState, hostmem,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_npu_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = r100_npu_pci_realize;
    k->exit      = r100_npu_pci_exit;
    k->vendor_id = R100_PCI_VENDOR_ID;
    k->device_id = R100_PCI_DEVICE_ID;
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
