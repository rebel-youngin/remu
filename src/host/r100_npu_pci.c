/*
 * R100 NPU host-side PCIe endpoint (Phase 2; M3 BARs, M4 shared DRAM,
 * M6 doorbell, M7 MSI-X, M8a ISSR shadow, M8b Stage 3b hdma + P10-fix
 * cfg-shadow / host-ram aliases).
 * Vendor/device = CR03 quad; stock rebellions.ko binds unmodified.
 *
 * BARs (next pow2 >= driver's RBLN_* sizes in rebel.h):
 *   BAR0 64 GB  DDR (lazy RAM; shared /dev/shm head when memdev wired — M4/M5)
 *   BAR2 64 MB  ACP/SRAM (lazy RAM; a 4 KB MMIO head @ FW_LOGBUF_SIZE
 *               aliases the shared `cfg-shadow` memory-backend-file —
 *               P10-fix. The NPU r100-cm7 aliases the same backend
 *               over its cfg-mirror trap at DEVICE_COMMUNICATION_SPACE
 *               so kmd writes to FUNC_SCRATCH / DDH_BASE_LO are
 *               observable on q-cp's next read with no chardev hop.)
 *   BAR4 8 MB   Doorbell: 8 MB container; when doorbell or issr chardev
 *               is wired, 4 KB MMIO head (prio 10) intercepts
 *                 - 0x08/0x1c MAILBOX_INTGR{0,1}   → doorbell frame (M6)
 *                 - 0x80..0x180 MAILBOX_BASE        → doorbell frame (M8a host→NPU)
 *               Reads of MAILBOX_BASE serve the ISSR shadow fed by
 *               `issr` chardev (M8a NPU→host). Rest of BAR4 = lazy RAM.
 *   BAR5 1 MB   MSI-X table+PBA head; rest lazy RAM.
 *
 * One cross-process bridge back to the NPU QEMU:
 *
 *   hdma chardev (server): NPU→host — dispatches on the op field of
 *                          each incoming frame (remu_hdma_proto.h):
 *                            OP_WRITE     : pci_dma_write(payload)
 *                            OP_READ_REQ  : pci_dma_read + emit
 *                                           OP_READ_RESP tagged by req_id
 *                          Used by r100-hdma's MMIO-driven channels
 *                          (req_id 0x80..0xBF) for q-cp's dw_hdma_v0
 *                          LL kicks. P10-fix retired the prior cfg
 *                          chardev (host→NPU 8-byte (cfg_off, val)
 *                          frames) and the OP_CFG_WRITE reverse-path
 *                          on req_id 0x00 — both replaced by the
 *                          shared cfg-shadow alias above.
 *                          P10-fix also retired the r100-pcie-outbound
 *                          chardev fallback (req_id 0xC0..0xFF
 *                          synchronous PF-window reads) — the NPU
 *                          aliases the host x86 QEMU's main RAM
 *                          (`host-ram` backend) directly so those
 *                          loads/stores are plain RAM accesses.
 *
 * Wire formats:
 *   doorbell / issr / msix : 8-byte (off, val) LE frames (remu_frame.h)
 *   hdma                   : 24-byte header + variable payload
 *                            (remu_hdma_proto.h)
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "exec/memory.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "sysemu/hostmem.h"
#include "sysemu/dma.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "r100/remu_addrmap.h"
#include "remu_frame.h"
#include "remu_doorbell_proto.h"
#include "remu_hdma_proto.h"

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

    /* cfg-shadow (P10-fix): 4 KB shareable backend aliased over the
     * BAR2 cfg-head subregion at REMU_BAR2_CFG_HEAD_OFF. The NPU
     * r100-cm7 aliases the same backend over its cfg-mirror trap at
     * R100_DEVICE_COMM_SPACE_BASE, so kmd writes to FUNC_SCRATCH /
     * DDH_BASE_LO are observable on q-cp's next read with no chardev
     * round trip — eliminates the cfg/doorbell ordering race exposed
     * once the outbound iATU stopped serialising on `hdma`. NULL =
     * fall back to the prior chardev path (cfg + cfg-debug). */
    HostMemoryBackend *cfg_shadow_be;

    /* doorbell (M6+M8a host→NPU): 8-byte (off, val) frames. */
    CharBackend doorbell_chr;

    /* msix (M7 NPU→host): frames from r100-imsix → msix_notify(vec). */
    CharBackend msix_chr;
    CharBackend msix_debug_chr;
    RemuFrameRx msix_rx;
    uint64_t msix_frames_received;
    uint64_t msix_frames_dropped;
    uint32_t msix_last_db_data;
    uint32_t msix_last_vector;

    /* issr (M8a NPU→host): frames from r100-mailbox → bar4_mmio_regs. */
    CharBackend issr_chr;
    CharBackend issr_debug_chr;
    RemuFrameRx issr_rx;
    uint64_t issr_frames_received;
    uint64_t issr_frames_dropped;
    uint32_t issr_last_offset;
    uint32_t issr_last_value;
    /* M8a host→NPU ISSR-payload frames (shared wire with doorbell). */
    uint64_t issr_payload_frames_sent;

    /* hdma (M8b Stage 3b/3c NPU→host): variable-length frames decoded
     * by remu_hdma_proto.h. OP_WRITE executed as pci_dma_write,
     * OP_READ_REQ answered with OP_READ_RESP (frames sent counter
     * tracks the responses). The OP_CFG_WRITE reverse path was
     * retired alongside the `cfg` chardev in P10-fix. */
    CharBackend hdma_chr;
    CharBackend hdma_debug_chr;
    RemuHdmaRx  hdma_rx;
    uint64_t hdma_frames_received;
    uint64_t hdma_frames_dropped;
    uint64_t hdma_frames_sent;   /* OP_READ_RESP egress */
    uint64_t hdma_last_dst;
    uint32_t hdma_last_len;
    uint32_t hdma_last_op;

    MemoryRegion bar0_ddr;
    MemoryRegion bar0_tail;

    /* BAR2: container + prio-0 lazy RAM + optional prio-10 cfg-head MMIO
     * trap (installed when cfg chardev is wired). */
    MemoryRegion bar2_container;
    MemoryRegion bar2_ram;
    MemoryRegion bar2_cfg_mmio;

    /* BAR4: container + prio-0 lazy RAM + optional prio-10 MMIO head
     * (installed when doorbell or issr chardev is connected). */
    MemoryRegion bar4_container;
    MemoryRegion bar4_mmio;
    MemoryRegion bar4_ram;

    uint32_t bar4_mmio_regs[R100_BAR4_MMIO_SIZE / 4];
    uint64_t doorbell_frames_sent;

    MemoryRegion bar5_msix;
};

/* P10-fix: BAR2 cfg-head subregion is a MemoryRegion alias over the
 * shared `cfg-shadow` memory-backend-file. The pre-P10-fix io-ops
 * trap (r100_bar2_cfg_{read,write} + matching debug emit + cfg
 * chardev frame egress) is gone — the alias is the single source of
 * truth and the NPU r100-cm7 aliases the same backend on its side. */

/* ----- BAR4 doorbell ingress trap (unchanged from M6/M8a) ------------- */

/* Emit 8-byte (off, val) frame on doorbell chardev. Best-effort;
 * silicon doesn't back-pressure on doorbell writes either. */
static void r100_doorbell_emit(R100NpuPciState *s, uint32_t off, uint32_t val)
{
    if (remu_frame_emit(&s->doorbell_chr, "r100-npu-pci doorbell",
                        off, val) == REMU_FRAME_EMIT_OK) {
        s->doorbell_frames_sent++;
    }
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
        qemu_log_mask(LOG_TRACE,
                      "r100-npu-pci: bar4 read off=0x%x -> 0x%x\n",
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
            qemu_log_mask(LOG_TRACE,
                          "r100-npu-pci: bar4 write off=0x%x val=0x%x\n",
                          (uint32_t)addr, v32);
        }
    }

    /* Only bridge-relevant offsets (INTGR{0,1} → M6 SPI path, ISSR
     * payload range → M8a host→NPU) cross the chardev; other BAR4
     * writes stay local (reads serve from bar4_mmio_regs). Classifier
     * is shared with the NPU-side deliver path so there's one wire
     * spec. */
    switch (remu_doorbell_classify((uint32_t)addr, NULL)) {
    case REMU_DB_KIND_INTGR0:
    case REMU_DB_KIND_INTGR1:
        r100_doorbell_emit(s, (uint32_t)addr, v32);
        break;
    case REMU_DB_KIND_ISSR:
        r100_doorbell_emit(s, (uint32_t)addr, v32);
        s->issr_payload_frames_sent++;
        break;
    case REMU_DB_KIND_OTHER:
        break;
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

/* ----- M7: MSI-X ingress (unchanged) --------------------------------- */

static int r100_msix_can_receive(void *opaque)
{
    R100NpuPciState *s = opaque;
    return remu_frame_rx_headroom(&s->msix_rx);
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
    uint32_t off, db;

    while (size > 0) {
        if (remu_frame_rx_feed(&s->msix_rx, &buf, &size, &off, &db)) {
            r100_msix_deliver(s, off, db);
        }
    }
}

/* ----- M8a: ISSR ingress (unchanged) --------------------------------- */

static int r100_issr_can_receive(void *opaque)
{
    R100NpuPciState *s = opaque;
    return remu_frame_rx_headroom(&s->issr_rx);
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

    /* ISSR frames must land in the MAILBOX_BASE range and be 4-byte
     * aligned; the shared classifier is the single source of truth. */
    if (remu_doorbell_classify(off, NULL) != REMU_DB_KIND_ISSR) {
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
    qemu_log_mask(LOG_TRACE,
                  "r100-npu-pci: issr deliver off=0x%x val=0x%x "
                  "stored=0x%x received=%" PRIu64 "\n",
                  off, val, s->bar4_mmio_regs[off >> 2],
                  s->issr_frames_received);
}

static void r100_issr_receive(void *opaque, const uint8_t *buf, int size)
{
    R100NpuPciState *s = opaque;
    uint32_t off, val;

    while (size > 0) {
        if (remu_frame_rx_feed(&s->issr_rx, &buf, &size, &off, &val)) {
            r100_issr_deliver(s, off, val);
        }
    }
}

/* ----- M8b 3b: HDMA ingress (NPU→host write executor) ---------------- */

static int r100_hdma_can_receive(void *opaque)
{
    R100NpuPciState *s = opaque;
    return remu_hdma_rx_headroom(&s->hdma_rx);
}

static void r100_hdma_emit_debug(R100NpuPciState *s,
                                 const char *dir,
                                 const RemuHdmaHeader *hdr,
                                 const char *status)
{
    char line[192];
    int n;

    if (!qemu_chr_fe_backend_connected(&s->hdma_debug_chr)) {
        return;
    }
    n = snprintf(line, sizeof(line),
                 "hdma %s op=%s req_id=%u dst=0x%" PRIx64 " len=%u "
                 "status=%s recv=%" PRIu64 " sent=%" PRIu64 "\n",
                 dir, remu_hdma_op_str(hdr->op), hdr->req_id,
                 hdr->dst, hdr->len, status,
                 s->hdma_frames_received, s->hdma_frames_sent);
    if (n > 0) {
        qemu_chr_fe_write(&s->hdma_debug_chr, (const uint8_t *)line, n);
    }
}

/*
 * OP_READ_REQ handler — pull `hdr->len` bytes out of the x86 guest's
 * DMA space via pci_dma_read and emit a matching OP_READ_RESP frame
 * (same req_id echoed, src = original hdr->dst). Bounded by
 * REMU_HDMA_MAX_PAYLOAD so the stack buffer stays small; a request
 * that wants more than 4 KB is a protocol violation from the
 * NPU-side stub, not something the kmd can steer, so we drop it
 * rather than chunk the reply.
 */
static void r100_hdma_handle_read_req(R100NpuPciState *s,
                                      const RemuHdmaHeader *hdr)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint8_t buf[REMU_HDMA_MAX_PAYLOAD];
    MemTxResult res;
    RemuHdmaEmitResult erc;

    if (hdr->len == 0 || hdr->len > sizeof(buf)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-npu-pci: hdma READ_REQ len=%u out of range "
                      "(max %zu) req_id=%u\n",
                      hdr->len, sizeof(buf), hdr->req_id);
        s->hdma_frames_dropped++;
        r100_hdma_emit_debug(s, "rx", hdr, "len-range");
        return;
    }
    res = pci_dma_read(pdev, hdr->dst, buf, hdr->len);
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-npu-pci: hdma pci_dma_read src=0x%" PRIx64
                      " len=%u failed (res=%d) req_id=%u\n",
                      hdr->dst, hdr->len, res, hdr->req_id);
        s->hdma_frames_dropped++;
        r100_hdma_emit_debug(s, "rx", hdr, "dma-fail");
        return;
    }
    s->hdma_frames_received++;
    r100_hdma_emit_debug(s, "rx", hdr, "ok");

    erc = remu_hdma_emit_read_resp(&s->hdma_chr, "r100-npu-pci hdma",
                                   hdr->req_id, hdr->dst, buf, hdr->len);
    if (erc != REMU_HDMA_EMIT_OK) {
        qemu_log_mask(LOG_UNIMP,
                      "r100-npu-pci: hdma READ_RESP emit failed "
                      "src=0x%" PRIx64 " len=%u req_id=%u rc=%d\n",
                      hdr->dst, hdr->len, hdr->req_id, (int)erc);
        /* Don't bump hdma_frames_dropped — we already counted the
         * successful ingress. Tests should notice by observing no
         * matching READ_RESP in hdma.log. */
        return;
    }
    s->hdma_frames_sent++;
    {
        /* Log the response separately so the hdma.log pairing is
         * unambiguous; reuse the header with op overridden to RESP. */
        RemuHdmaHeader resp = {
            .magic  = REMU_HDMA_MAGIC,
            .op     = REMU_HDMA_OP_READ_RESP,
            .dst    = hdr->dst,
            .len    = hdr->len,
            .req_id = hdr->req_id,
        };
        r100_hdma_emit_debug(s, "tx", &resp, "ok");
    }
    qemu_log_mask(LOG_TRACE,
                  "r100-npu-pci: hdma READ_REQ->RESP src=0x%" PRIx64
                  " len=%u req_id=%u\n",
                  hdr->dst, hdr->len, hdr->req_id);
}

static void r100_hdma_deliver(R100NpuPciState *s,
                              const RemuHdmaHeader *hdr,
                              const uint8_t *payload)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    MemTxResult res;

    s->hdma_last_dst = hdr->dst;
    s->hdma_last_len = hdr->len;
    s->hdma_last_op  = hdr->op;

    switch (hdr->op) {
    case REMU_HDMA_OP_WRITE:
        res = pci_dma_write(pdev, hdr->dst, payload, hdr->len);
        if (res != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "r100-npu-pci: hdma pci_dma_write dst=0x%"
                          PRIx64 " len=%u failed (res=%d) req_id=%u\n",
                          hdr->dst, hdr->len, res, hdr->req_id);
            s->hdma_frames_dropped++;
            r100_hdma_emit_debug(s, "rx", hdr, "dma-fail");
            return;
        }
        s->hdma_frames_received++;
        r100_hdma_emit_debug(s, "rx", hdr, "ok");
        qemu_log_mask(LOG_TRACE,
                      "r100-npu-pci: hdma WRITE dst=0x%" PRIx64 " len=%u "
                      "req_id=%u received=%" PRIu64 "\n",
                      hdr->dst, hdr->len, hdr->req_id,
                      s->hdma_frames_received);
        break;
    case REMU_HDMA_OP_READ_REQ:
        r100_hdma_handle_read_req(s, hdr);
        break;
    case REMU_HDMA_OP_READ_RESP:
        /* READ_RESP is host->NPU only; the NPU side should never send
         * one to us. Treat as protocol violation rather than silently
         * dropping so a future refactor doesn't mask real bugs. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-npu-pci: hdma unexpected READ_RESP "
                      "dst=0x%" PRIx64 " len=%u req_id=%u "
                      "(host-side only sends, never receives)\n",
                      hdr->dst, hdr->len, hdr->req_id);
        s->hdma_frames_dropped++;
        r100_hdma_emit_debug(s, "rx", hdr, "unexpected-op");
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-npu-pci: hdma unknown op=%u dst=0x%" PRIx64
                      " len=%u req_id=%u\n", hdr->op, hdr->dst, hdr->len,
                      hdr->req_id);
        s->hdma_frames_dropped++;
        r100_hdma_emit_debug(s, "rx", hdr, "bad-op");
        break;
    }
}

static void r100_hdma_receive(void *opaque, const uint8_t *buf, int size)
{
    R100NpuPciState *s = opaque;
    const RemuHdmaHeader *hdr;
    const uint8_t *payload;

    while (size > 0) {
        if (remu_hdma_rx_feed(&s->hdma_rx, &buf, &size, &hdr, &payload)) {
            r100_hdma_deliver(s, hdr, payload);
        }
    }
}

/* ----- realize / exit / props ---------------------------------------- */

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

    /* BAR2 ACP/SRAM: lazy RAM container; the 4 KB cfg-head subregion at
     * FW_LOGBUF_SIZE is a MemoryRegion alias over the shared `cfg-shadow`
     * memory-backend-file. The NPU r100-cm7 aliases the same backend
     * over its cfg-mirror trap, so kmd writes are visible on q-cp's
     * next read with zero round-trip latency (single source of truth;
     * no chardev queueing, no ordering race against the doorbell). */
    memory_region_init(&s->bar2_container, OBJECT(s),
                       "r100.bar2.container", R100_BAR2_ACP_SIZE);
    memory_region_init_ram(&s->bar2_ram, OBJECT(s), "r100.bar2.ram",
                           R100_BAR2_ACP_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion_overlap(&s->bar2_container, 0,
                                        &s->bar2_ram, 0);
    if (s->cfg_shadow_be) {
        MemoryRegion *cfg_mr = host_memory_backend_get_memory(s->cfg_shadow_be);
        uint64_t alias_size = MIN((uint64_t)memory_region_size(cfg_mr),
                                  (uint64_t)REMU_BAR2_CFG_HEAD_SIZE);

        memory_region_init_alias(&s->bar2_cfg_mmio, OBJECT(s),
                                 "r100.bar2.cfg.alias",
                                 cfg_mr, 0, alias_size);
        memory_region_add_subregion_overlap(&s->bar2_container,
                                            REMU_BAR2_CFG_HEAD_OFF,
                                            &s->bar2_cfg_mmio, 10);
        host_memory_backend_set_mapped(s->cfg_shadow_be, true);
    }
    pci_register_bar(pdev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->bar2_container);

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
    if (qemu_chr_fe_backend_connected(&s->hdma_chr)) {
        remu_hdma_rx_reset(&s->hdma_rx);
        qemu_chr_fe_set_handlers(&s->hdma_chr,
                                 r100_hdma_can_receive,
                                 r100_hdma_receive,
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
    qemu_chr_fe_deinit(&s->hdma_chr, false);
    qemu_chr_fe_deinit(&s->hdma_debug_chr, false);
}

static Property r100_npu_pci_properties[] = {
    DEFINE_PROP_LINK("memdev", R100NpuPciState, hostmem,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_LINK("cfg-shadow", R100NpuPciState, cfg_shadow_be,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_CHR("doorbell", R100NpuPciState, doorbell_chr),
    DEFINE_PROP_CHR("msix", R100NpuPciState, msix_chr),
    DEFINE_PROP_CHR("msix-debug", R100NpuPciState, msix_debug_chr),
    DEFINE_PROP_CHR("issr", R100NpuPciState, issr_chr),
    DEFINE_PROP_CHR("issr-debug", R100NpuPciState, issr_debug_chr),
    DEFINE_PROP_CHR("hdma", R100NpuPciState, hdma_chr),
    DEFINE_PROP_CHR("hdma-debug", R100NpuPciState, hdma_debug_chr),
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
