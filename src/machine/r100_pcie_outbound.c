/*
 * REMU - R100 NPU System Emulator
 * PCIe outbound iATU window stub (P1 — honest BD lifecycle on q-cp/CP0).
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
 * REMU has neither the DW PCIe IP nor a real iATU. Until P1 the q-cp
 * loads above were silently reading garbage out of unmapped 4 GB of
 * the NPU AXI map. r100-pcie-outbound trades a 4 GB MMIO trap for the
 * iATU and tunnels each access to the host QEMU over the existing
 * `hdma` chardev (remu_hdma_proto.h):
 *
 *   - reads issue OP_READ_REQ tagged with a req_id in the
 *     0xC0..0xFF outbound partition (see remu_addrmap.h) and block on
 *     a per-device condition variable until the matching OP_READ_RESP
 *     arrives. We use qemu_cond_wait_bql() so the BQL is released
 *     while we're parked, letting the chardev RX iothread service the
 *     response and other vCPUs make progress; at most one outbound
 *     request is in flight at a time, so ordering matches the
 *     program order of the loads on a single CA73.
 *
 *   - writes are fire-and-forget OP_WRITEs (kmd doesn't fence on
 *     completion of these specific writes; the doorbells that *do*
 *     need ordering already use the BD-done state machine in r100-cm7
 *     or r100-hdma's MMIO channels).
 *
 * The MMIO `addr` parameter inside read()/write() is the offset into
 * the MR (= AXI address - PCIE_AXI_SLV_BASE_ADDR), which by the iATU
 * mapping is exactly the PCIe bus address — kmd's HOST_PHYS_BASE is
 * stripped automatically by QEMU's MR base subtraction. The host-side
 * pci_dma_{read,write} (in r100-npu-pci) then resolves that bus
 * address against the x86 guest's IOMMU.
 *
 * Stage-3c retirement plan
 * ------------------------
 * r100-cm7's BD-done state machine (Stage 3c, src/machine/r100_cm7.c)
 * is QEMU-side scaffolding that walks the BD ring on q-cp's behalf
 * via OP_READ_REQ/RESP. Once the q-cp/CP0 load path lands genuine
 * traffic through this device, the cm7 BD-done state machine can be
 * progressively dismantled (P1's verification step: see
 * docs/roadmap.md). r100-cm7 keeps cfg-shadow, QINIT, and SOFT_RESET
 * for now since those don't need a real CA73 vCPU to drive them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "r100_soc.h"
#include "r100_hdma.h"
#include "remu_hdma_proto.h"

OBJECT_DECLARE_SIMPLE_TYPE(R100PcieOutboundState, R100_PCIE_OUTBOUND)

/* Cap any single MMIO access at 8 B (max u64 load/store on AArch64).
 * The chardev round-trip itself can carry up to REMU_HDMA_MAX_PAYLOAD,
 * but the MMIO ops max_access_size keeps us well clear of that. */
#define R100_PCIE_OUTBOUND_MAX_ACCESS  8u

struct R100PcieOutboundState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    R100HDMAState *hdma;     /* QOM link to the chardev owner. */

    uint32_t chiplet_id;     /* PF only today (chiplet 0). */

    /* Single-slot synchronous read pump. The BQL serialises check +
     * set of `in_flight`, so two vCPUs can't both claim ownership at
     * once; the second one parks in cond_wait_bql() and retries on
     * each broadcast. */
    QemuCond cond;
    bool     in_flight;
    bool     resp_ready;
    uint32_t pending_req_id;
    uint32_t resp_len;
    uint8_t  resp_buf[R100_PCIE_OUTBOUND_MAX_ACCESS];

    /* Cookie rotates over the 0x00..0x3F low bits of the
     * 0xC0..0xFF outbound partition. Only one in flight, so this is
     * mostly cosmetic — but it makes per-request frames in the
     * hdma debug log distinguishable. */
    uint8_t  next_cookie;

    uint64_t reads_dispatched;
    uint64_t reads_completed;
    uint64_t reads_dropped;
    uint64_t writes_dispatched;
    uint64_t writes_dropped;
    uint64_t resp_unexpected;
};

/* ------------------------------------------------------------------ */
/* HDMA RX callback — runs in the chardev iothread context, BQL held. */
/* ------------------------------------------------------------------ */

static void r100_pcie_outbound_resp(void *opaque,
                                    const RemuHdmaHeader *hdr,
                                    const uint8_t *payload)
{
    R100PcieOutboundState *s = opaque;
    uint32_t copy_len;

    /* r100-hdma already filtered op == OP_READ_RESP and req_id in our
     * partition (0xC0..0xFF). Defensive recheck so a future demux
     * regression here doesn't trash the wait state silently. */
    if (hdr->op != REMU_HDMA_OP_READ_RESP) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pcie-outbound: unexpected op=%s req_id=0x%x\n",
                      remu_hdma_op_str(hdr->op), hdr->req_id);
        s->resp_unexpected++;
        return;
    }
    if (!s->in_flight || hdr->req_id != s->pending_req_id) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pcie-outbound: stray RESP req_id=0x%x "
                      "(in_flight=%d pending=0x%x)\n",
                      hdr->req_id, (int)s->in_flight, s->pending_req_id);
        s->resp_unexpected++;
        return;
    }

    copy_len = MIN(hdr->len, (uint32_t)sizeof(s->resp_buf));
    memcpy(s->resp_buf, payload, copy_len);
    s->resp_len = copy_len;
    s->resp_ready = true;
    qemu_cond_broadcast(&s->cond);
}

/* ------------------------------------------------------------------ */
/* MMIO ops                                                            */
/* ------------------------------------------------------------------ */

static uint8_t r100_pcie_outbound_alloc_req_id(R100PcieOutboundState *s)
{
    uint8_t cookie = s->next_cookie & R100_PCIE_OUTBOUND_REQ_ID_MASK;
    s->next_cookie = (s->next_cookie + 1u) & R100_PCIE_OUTBOUND_REQ_ID_MASK;
    return R100_PCIE_OUTBOUND_REQ_ID_BASE | cookie;
}

static uint64_t r100_pcie_outbound_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    R100PcieOutboundState *s = R100_PCIE_OUTBOUND(opaque);
    uint64_t v = 0;
    uint8_t  req_id;

    if (size > sizeof(s->resp_buf)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pcie-outbound: read addr=0x%" PRIx64
                      " bad size=%u\n", (uint64_t)addr, size);
        return 0;
    }
    if (s->hdma == NULL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pcie-outbound: read addr=0x%" PRIx64
                      " but hdma link unbound\n", (uint64_t)addr);
        s->reads_dropped++;
        return 0;
    }

    /* Wait for any prior in-flight read to finish (BQL released
     * during the wait so the iothread can deliver the response). */
    while (s->in_flight) {
        qemu_cond_wait_bql(&s->cond);
    }

    req_id = r100_pcie_outbound_alloc_req_id(s);
    s->in_flight = true;
    s->resp_ready = false;
    s->resp_len = 0;
    s->pending_req_id = req_id;

    if (!r100_hdma_emit_read_req(s->hdma, req_id, (uint64_t)addr, size,
                                 "outbound-rd")) {
        s->reads_dropped++;
        s->in_flight = false;
        s->resp_ready = false;
        qemu_cond_broadcast(&s->cond);
        return 0;
    }
    s->reads_dispatched++;

    while (!s->resp_ready) {
        qemu_cond_wait_bql(&s->cond);
    }

    if (s->resp_len < size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pcie-outbound: short resp addr=0x%" PRIx64
                      " size=%u got=%u req_id=0x%x\n",
                      (uint64_t)addr, size, s->resp_len, req_id);
    }
    memcpy(&v, s->resp_buf, MIN(size, s->resp_len));

    s->in_flight = false;
    s->resp_ready = false;
    s->reads_completed++;
    qemu_cond_broadcast(&s->cond);

    qemu_log_mask(LOG_TRACE,
                  "r100-pcie-outbound: rd addr=0x%" PRIx64
                  " size=%u val=0x%" PRIx64 " req_id=0x%x\n",
                  (uint64_t)addr, size, v, req_id);
    return v;
}

static void r100_pcie_outbound_write(void *opaque, hwaddr addr,
                                     uint64_t val, unsigned size)
{
    R100PcieOutboundState *s = R100_PCIE_OUTBOUND(opaque);
    uint8_t buf[R100_PCIE_OUTBOUND_MAX_ACCESS];

    if (size > sizeof(buf)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pcie-outbound: write addr=0x%" PRIx64
                      " bad size=%u\n", (uint64_t)addr, size);
        s->writes_dropped++;
        return;
    }
    if (s->hdma == NULL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-pcie-outbound: write addr=0x%" PRIx64
                      " but hdma link unbound\n", (uint64_t)addr);
        s->writes_dropped++;
        return;
    }

    memcpy(buf, &val, size);
    /* req_id stays in the outbound partition (host doesn't ack
     * writes; this just keeps the partition tag consistent in
     * any debug traces). */
    if (!r100_hdma_emit_write_tagged(s->hdma,
                                     R100_PCIE_OUTBOUND_REQ_ID_BASE,
                                     (uint64_t)addr, buf, size,
                                     "outbound-wr")) {
        s->writes_dropped++;
        return;
    }
    s->writes_dispatched++;
    qemu_log_mask(LOG_TRACE,
                  "r100-pcie-outbound: wr addr=0x%" PRIx64
                  " size=%u val=0x%" PRIx64 "\n",
                  (uint64_t)addr, size, val);
}

static const MemoryRegionOps r100_pcie_outbound_ops = {
    .read = r100_pcie_outbound_read,
    .write = r100_pcie_outbound_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = R100_PCIE_OUTBOUND_MAX_ACCESS,
    .valid.min_access_size = 1,
    .valid.max_access_size = R100_PCIE_OUTBOUND_MAX_ACCESS,
    .valid.unaligned = true,
};

/* ------------------------------------------------------------------ */
/* Realize / reset / vmstate / properties                              */
/* ------------------------------------------------------------------ */

static void r100_pcie_outbound_realize(DeviceState *dev, Error **errp)
{
    R100PcieOutboundState *s = R100_PCIE_OUTBOUND(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char name[64];

    if (s->hdma == NULL) {
        error_setg(errp, "r100-pcie-outbound: 'hdma' link is required");
        return;
    }
    if (s->chiplet_id != 0) {
        error_setg(errp,
                   "r100-pcie-outbound: only chiplet 0 (PF) is supported "
                   "(got chiplet=%u)", s->chiplet_id);
        return;
    }

    qemu_cond_init(&s->cond);

    snprintf(name, sizeof(name), "r100-pcie-outbound.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_pcie_outbound_ops,
                          s, name, R100_PCIE_OUTBOUND_PF_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    r100_hdma_set_outbound_callback(s->hdma, r100_pcie_outbound_resp, s);
}

static void r100_pcie_outbound_unrealize(DeviceState *dev)
{
    R100PcieOutboundState *s = R100_PCIE_OUTBOUND(dev);

    if (s->hdma) {
        r100_hdma_set_outbound_callback(s->hdma, NULL, NULL);
    }
    qemu_cond_destroy(&s->cond);
}

static void r100_pcie_outbound_reset(DeviceState *dev)
{
    R100PcieOutboundState *s = R100_PCIE_OUTBOUND(dev);

    /* If a vCPU is parked in cond_wait_bql() across reset (unlikely
     * but possible during a CA73 cluster reset path), wake it so it
     * sees in_flight=false and bails out cleanly. */
    s->in_flight = false;
    s->resp_ready = false;
    s->resp_len = 0;
    s->pending_req_id = 0;
    s->next_cookie = 0;
    memset(s->resp_buf, 0, sizeof(s->resp_buf));
    qemu_cond_broadcast(&s->cond);
}

static const VMStateDescription r100_pcie_outbound_vmstate = {
    .name = "r100-pcie-outbound",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(reads_dispatched, R100PcieOutboundState),
        VMSTATE_UINT64(reads_completed, R100PcieOutboundState),
        VMSTATE_UINT64(reads_dropped, R100PcieOutboundState),
        VMSTATE_UINT64(writes_dispatched, R100PcieOutboundState),
        VMSTATE_UINT64(writes_dropped, R100PcieOutboundState),
        VMSTATE_UINT64(resp_unexpected, R100PcieOutboundState),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_pcie_outbound_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100PcieOutboundState, chiplet_id, 0),
    DEFINE_PROP_LINK("hdma", R100PcieOutboundState, hdma,
                     TYPE_R100_HDMA, R100HDMAState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_pcie_outbound_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 PCIe outbound iATU stub (PF window via hdma chardev)";
    dc->realize = r100_pcie_outbound_realize;
    dc->unrealize = r100_pcie_outbound_unrealize;
    dc->vmsd = &r100_pcie_outbound_vmstate;
    device_class_set_legacy_reset(dc, r100_pcie_outbound_reset);
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
