/*
 * R100 NPU host-side PCIe endpoint (Phase 2; M3 BARs, M4 shared DRAM,
 * M6 doorbell, M7 MSI-X, M8a ISSR shadow). Vendor/device = CR03 quad;
 * stock rebellions.ko binds unmodified.
 *
 * BARs (next pow2 >= driver's RBLN_* sizes in rebel.h):
 *   BAR0 64 GB  DDR (lazy RAM; shared /dev/shm head when memdev wired — M4/M5)
 *   BAR2 64 MB  ACP/SRAM (lazy RAM)
 *   BAR4 8 MB   Doorbell: 8 MB container; when doorbell or issr chardev
 *               is wired, 4 KB MMIO head (prio 10) intercepts
 *                 - 0x08/0x1c MAILBOX_INTGR{0,1}   → doorbell frame (M6)
 *                 - 0x80..0x180 MAILBOX_BASE        → doorbell frame (M8a host→NPU)
 *               Reads of MAILBOX_BASE serve the ISSR shadow fed by
 *               `issr` chardev (M8a NPU→host). Rest of BAR4 = lazy RAM.
 *   BAR5 1 MB   MSI-X table+PBA head; rest lazy RAM.
 *
 * Wire frames on all three chardevs: u32 bar4_off LE + u32 val LE = 8 B.
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

    /* memdev (M4): backend spliced over BAR0 head; bar0_tail covers the
     * rest so driver's size check passes. Unset = full lazy RAM (M3). */
    HostMemoryBackend *hostmem;

    /* doorbell (M6+M8a host→NPU): 8-byte (off, val) frames. */
    CharBackend doorbell_chr;

    /* msix (M7 NPU→host): frames from r100-imsix → msix_notify(vec). */
    CharBackend msix_chr;
    CharBackend msix_debug_chr;
    uint8_t msix_rx_buf[8];
    uint32_t msix_rx_len;
    uint64_t msix_frames_received;
    uint64_t msix_frames_dropped;
    uint32_t msix_last_db_data;
    uint32_t msix_last_vector;

    /* issr (M8a NPU→host): frames from r100-mailbox → bar4_mmio_regs. */
    CharBackend issr_chr;
    CharBackend issr_debug_chr;
    uint8_t issr_rx_buf[8];
    uint32_t issr_rx_len;
    uint64_t issr_frames_received;
    uint64_t issr_frames_dropped;
    uint32_t issr_last_offset;
    uint32_t issr_last_value;
    /* M8a host→NPU ISSR-payload frames (shared wire with doorbell). */
    uint64_t issr_payload_frames_sent;

    MemoryRegion bar0_ddr;
    MemoryRegion bar0_tail;
    MemoryRegion bar2_acp;

    /* BAR4: container + prio-0 lazy RAM + optional prio-10 MMIO head
     * (installed when doorbell or issr chardev is connected). */
    MemoryRegion bar4_container;
    MemoryRegion bar4_mmio;
    MemoryRegion bar4_ram;

    uint32_t bar4_mmio_regs[R100_BAR4_MMIO_SIZE / 4];
    uint64_t doorbell_frames_sent;

    MemoryRegion bar5_msix;
};

/* Emit 8-byte (off, val) frame on doorbell chardev. Best-effort;
 * silicon doesn't back-pressure on doorbell writes either. */
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
    uint32_t v;

    if (idx >= ARRAY_SIZE(s->bar4_mmio_regs)) {
        return 0;
    }
    v = s->bar4_mmio_regs[idx];
    /* Trace MAILBOX_BASE reads for KMD poll-vs-issr-deliver correlation. */
    if (addr >= 0x80 && addr < 0x180) {
        fprintf(stderr, "REMU-TRACE: bar4_mmio_read off=0x%x -> 0x%x\n",
                (uint32_t)addr, v);
    }
    return v;
}

static void r100_bar4_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    R100NpuPciState *s = opaque;
    uint32_t idx = (uint32_t)(addr >> 2);
    uint32_t v32 = (uint32_t)val;

    if (idx < ARRAY_SIZE(s->bar4_mmio_regs)) {
        s->bar4_mmio_regs[idx] = v32;
        /* Trace MAILBOX_BASE writes to spot KMD clears that would
         * overwrite an NPU-emitted 0xfb0d. */
        if (addr >= 0x80 && addr < 0x180) {
            fprintf(stderr, "REMU-TRACE: bar4_mmio_write off=0x%x val=0x%x\n",
                    (uint32_t)addr, v32);
        }
    }

    /* INTGR{0,1} (M6): frame → NPU mailbox INTGR → SPI.
     * MAILBOX_BASE..END (M8a host→NPU): frame → r100_mailbox_set_issr.
     * Other offsets: local stash only (reads serve from bar4_mmio_regs). */
    if (addr == R100_BAR4_MAILBOX_INTGR0 ||
        addr == R100_BAR4_MAILBOX_INTGR1) {
        r100_doorbell_emit(s, (uint32_t)addr, v32);
    } else if (addr >= R100_BAR4_MAILBOX_BASE &&
               addr < R100_BAR4_MAILBOX_END) {
        r100_doorbell_emit(s, (uint32_t)addr, v32);
        s->issr_payload_frames_sent++;
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

/* M7: MSI-X ingress. Frame = (off, db_data); vector = db_data & MASK. */

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

    /* r100-imsix only accepts IMSIX_DB_OFFSET; validate for parser sanity. */
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

    /* msix_notify is idempotent wrt guest state: pending bit latches
     * in PBA regardless of enable/mask — correct primitive here. */
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

/* M8a: ISSR ingress. Frame (off, val) → bar4_mmio_regs[off>>2].
 * Drives KMD FW_BOOT_DONE visibility at BAR4+MAILBOX_BASE+0x10. */

static int r100_issr_can_receive(void *opaque)
{
    R100NpuPciState *s = opaque;
    return sizeof(s->issr_rx_buf) - s->issr_rx_len;
}

static void r100_issr_emit_debug(R100NpuPciState *s, uint32_t off,
                                 uint32_t val, const char *status)
{
    char line[128];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->issr_debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "issr off=0x%x val=0x%x status=%s count=%" PRIu64 "\n",
                 off, val, status, s->issr_frames_received);
    if (n > 0) {
        qemu_chr_fe_write(&s->issr_debug_chr, (const uint8_t *)line, n);
    }
}

static void r100_issr_deliver(R100NpuPciState *s, uint32_t off, uint32_t val)
{
    s->issr_last_offset = off;
    s->issr_last_value = val;

    if (off < R100_BAR4_MAILBOX_BASE || off >= R100_BAR4_MAILBOX_END ||
        (off & 0x3u) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-npu-pci: issr frame off=0x%x out of "
                      "MAILBOX_BASE range / unaligned\n", off);
        s->issr_frames_dropped++;
        r100_issr_emit_debug(s, off, val, "bad-offset");
        return;
    }

    /* Mirror into BAR4 MMIO reg file; no IRQ (NPU sets INTGR separately). */
    s->bar4_mmio_regs[off >> 2] = val;
    s->issr_frames_received++;
    r100_issr_emit_debug(s, off, val, "ok");
    /* Trace for FW_BOOT_DONE visibility debug — did chardev fire, and
     * does the store survive subsequent KMD writes? */
    fprintf(stderr, "REMU-TRACE: issr_deliver off=0x%x val=0x%x "
            "stored=0x%x received=%" PRIu64 "\n",
            off, val, s->bar4_mmio_regs[off >> 2],
            s->issr_frames_received);
}

static void r100_issr_receive(void *opaque, const uint8_t *buf, int size)
{
    R100NpuPciState *s = opaque;

    while (size > 0) {
        uint32_t want = sizeof(s->issr_rx_buf) - s->issr_rx_len;
        uint32_t take = size < (int)want ? (uint32_t)size : want;

        memcpy(s->issr_rx_buf + s->issr_rx_len, buf, take);
        s->issr_rx_len += take;
        buf += take;
        size -= take;

        if (s->issr_rx_len == sizeof(s->issr_rx_buf)) {
            uint32_t off = ldl_le_p(s->issr_rx_buf);
            uint32_t val = ldl_le_p(s->issr_rx_buf + 4);
            s->issr_rx_len = 0;
            r100_issr_deliver(s, off, val);
        }
    }
}

static void r100_npu_pci_realize(PCIDevice *pdev, Error **errp)
{
    R100NpuPciState *s = R100_NPU_PCI(pdev);
    Error *err = NULL;

    /* BAR0 DDR (64 GB, lazy-committed). With memdev: backend @ offset 0
     * (shared /dev/shm with NPU QEMU) + lazy-RAM tail. Without: full RAM. */
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

    /* BAR2 ACP/SRAM: CP logbuf, CPU config, MMU page tables, vserial
     * shim. Plain RAM until NPU-side models serve reads here. */
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

    /* BAR4 Doorbell: 8 MB container, 32-bit non-prefetch. lazy-RAM @
     * prio 0; 4 KB MMIO head @ prio 10 installed when doorbell or
     * issr chardev is wired (M6/M8a). */
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
    /* MMIO head needed for M6 doorbell egress (writes) OR M8a ISSR
     * shadow reads. Without either, full-BAR lazy RAM matches M3/M5. */
    if (qemu_chr_fe_backend_connected(&s->doorbell_chr) ||
        qemu_chr_fe_backend_connected(&s->issr_chr)) {
        memory_region_init_io(&s->bar4_mmio, OBJECT(s),
                              &r100_bar4_mmio_ops, s,
                              "r100.bar4.mmio", R100_BAR4_MMIO_SIZE);
        memory_region_add_subregion_overlap(&s->bar4_container, 0,
                                            &s->bar4_mmio, 10);
    }
    pci_register_bar(pdev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->bar4_container);

    /* BAR5 MSI-X: driver wants 1 MB BAR; table @0 / PBA @64 KB overlay
     * via msix_init, rest lazy RAM for guest mmap compatibility. */
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

    /* Enable every vector — PBA latches regardless of guest mask, so
     * unmask isn't a prerequisite for msix_notify (matches silicon). */
    for (int i = 0; i < R100_NUM_MSIX; i++) {
        msix_vector_use(pdev, i);
    }

    pci_config_set_interrupt_pin(pdev->config, 1);

    if (qemu_chr_fe_backend_connected(&s->msix_chr)) {
        qemu_chr_fe_set_handlers(&s->msix_chr,
                                 r100_msix_can_receive,
                                 r100_msix_receive,
                                 NULL, NULL, s, NULL, true);
    }
    if (qemu_chr_fe_backend_connected(&s->issr_chr)) {
        qemu_chr_fe_set_handlers(&s->issr_chr,
                                 r100_issr_can_receive,
                                 r100_issr_receive,
                                 NULL, NULL, s, NULL, true);
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
    qemu_chr_fe_deinit(&s->issr_chr, false);
    qemu_chr_fe_deinit(&s->issr_debug_chr, false);
}

static Property r100_npu_pci_properties[] = {
    DEFINE_PROP_LINK("memdev", R100NpuPciState, hostmem,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_CHR("doorbell", R100NpuPciState, doorbell_chr),
    DEFINE_PROP_CHR("msix", R100NpuPciState, msix_chr),
    DEFINE_PROP_CHR("msix-debug", R100NpuPciState, msix_debug_chr),
    DEFINE_PROP_CHR("issr", R100NpuPciState, issr_chr),
    DEFINE_PROP_CHR("issr-debug", R100NpuPciState, issr_debug_chr),
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
