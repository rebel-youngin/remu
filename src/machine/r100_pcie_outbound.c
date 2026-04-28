/*
 * REMU - R100 NPU System Emulator
 * PCIe outbound iATU window stub.
 * P1 — honest BD lifecycle on q-cp/CP0.
 * P10-fix — host-RAM shared-memory alias replaces the chardev hop;
 *           chardev fallback retired.
 *
 * Real silicon
 * ------------
 * q-cp's host-queue manager dereferences PCIe-bus addresses directly:
 *
 *     // q/cp/src/hq_mgr/host_queue_manager.c
 *     bd   = (struct rbln_bd *)(addr + RBLN_BD_SIZE * ci);
 *     addr = bd->addr;          // host RAM
 *
 *     // q/cp/src/hil/hil.c
 *     dev_desc[fid] = (struct rbln_device_desc *)
 *                     FUNC_READQ(fid, DDH_BASE_LO);
 *
 * The kmd publishes those bus addresses **after** adding HOST_PHYS_BASE
 * (= 0x8000000000ULL) so the AXI loads land in the chiplet-0 DesignWare
 * outbound iATU window — pcie_ep.c programs PF cpu_addr = 0x8000000000,
 * pci_addr = 0x0, size = 4 GB. The iATU translates those AXI
 * transactions into PCIe TLPs that the host's RC then services from
 * dma_alloc_coherent'd buffers.
 *
 * REMU plumbing
 * -------------
 * The host x86 QEMU's main RAM is a shareable memory-backend-file
 * (--host-mem sized, mounted on /dev/shm); the NPU QEMU mounts the
 * same backend at machine-init via `-machine r100-soc,host-ram=hostram`.
 * r100-pcie-outbound's realize installs a MemoryRegion alias of that
 * backend over the PF window, so q-cp's outbound loads / stores are
 * plain TCG accesses against the same mmap the kmd polls. No chardev,
 * no cond_wait, no BQL contention with the kmd's
 * `readl_poll_timeout_atomic` hot loop.
 *
 * The pre-P10-fix chardev fallback (per-access OP_READ_REQ / OP_WRITE
 * over the `hdma` chardev with a per-device condvar parking the vCPU
 * until OP_READ_RESP arrived; req_id partition 0xC0..0xFF) is gone:
 * it deadlocked under the kmd's busy-poll because the chardev RX
 * iothread couldn't acquire BQL. The 0xC0..0xFF partition is reserved
 * in remu_addrmap.h in case we ever need a true asynchronous outbound
 * path again.
 *
 * The MMIO `addr` parameter inside the alias is the offset into the
 * MR (= AXI address - PCIE_AXI_SLV_BASE_ADDR), which by the iATU
 * mapping is exactly the PCIe bus address — kmd's HOST_PHYS_BASE is
 * stripped automatically by QEMU's MR base subtraction. The address
 * goes straight through to the host-ram MR.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "r100_soc.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100PcieOutboundState, R100_PCIE_OUTBOUND)

struct R100PcieOutboundState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;          /* alias over host_ram. */
    MemoryRegion *host_ram;      /* QOM link: x86 guest main-RAM backend. */

    uint32_t chiplet_id;     /* PF only today (chiplet 0). */
};

/* ------------------------------------------------------------------ */
/* Realize / vmstate / properties                                      */
/* ------------------------------------------------------------------ */

static void r100_pcie_outbound_realize(DeviceState *dev, Error **errp)
{
    R100PcieOutboundState *s = R100_PCIE_OUTBOUND(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char name[64];
    uint64_t alias_size;

    if (s->host_ram == NULL) {
        error_setg(errp,
                   "r100-pcie-outbound: 'host-ram' link is required "
                   "(machine wires the shared host-ram backend)");
        return;
    }
    if (s->chiplet_id != 0) {
        error_setg(errp,
                   "r100-pcie-outbound: only chiplet 0 (PF) is supported "
                   "(got chiplet=%u)", s->chiplet_id);
        return;
    }

    snprintf(name, sizeof(name), "r100-pcie-outbound.cl%u", s->chiplet_id);

    /* Alias the entire host-ram backend over the PF-window. The alias
     * size is the backend's full size — the PCIe outbound iATU is 4 GB
     * on silicon, but the x86 guest's RAM never exceeds host-mem
     * (typically 512 MB), so anything the kmd actually allocates lives
     * in [0, host_ram_size). Accesses above that fall through to
     * unassigned-region the same way they would on real hardware
     * reading past the actual PCIe BAR space behind the iATU. */
    alias_size = MIN((uint64_t)memory_region_size(s->host_ram),
                     (uint64_t)R100_PCIE_OUTBOUND_PF_SIZE);
    memory_region_init_alias(&s->iomem, OBJECT(dev), name,
                             s->host_ram, 0, alias_size);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription r100_pcie_outbound_vmstate = {
    .name = "r100-pcie-outbound",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_pcie_outbound_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100PcieOutboundState, chiplet_id, 0),
    /* Required: shared host-ram MemoryRegion. The machine wires this
     * from `-machine r100-soc,host-ram=<id>` (the NPU-side mount of
     * the same memory-backend-file the host x86 QEMU uses for
     * `-machine pc,memory-backend=<id>`). */
    DEFINE_PROP_LINK("host-ram", R100PcieOutboundState, host_ram,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_pcie_outbound_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 PCIe outbound iATU stub (PF window — host-ram alias)";
    dc->realize = r100_pcie_outbound_realize;
    dc->vmsd = &r100_pcie_outbound_vmstate;
    device_class_set_props(dc, r100_pcie_outbound_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->user_creatable = false;
}

static const TypeInfo r100_pcie_outbound_info = {
    .name          = TYPE_R100_PCIE_OUTBOUND,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100PcieOutboundState),
    .class_init    = r100_pcie_outbound_class_init,
};

static void r100_pcie_outbound_register_types(void)
{
    type_register_static(&r100_pcie_outbound_info);
}

type_init(r100_pcie_outbound_register_types)
