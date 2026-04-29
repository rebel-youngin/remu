# HDMA / SMMU — Notion Reference Notes

Distilled from the Engineering Wiki → CP FW pages. Source of truth is
Notion; this file is a pinned snapshot for offline reference while
working on `r100-hdma` (P5) and any DVA / IPA translation question.
Re-fetch the originals if anything below looks stale.

## Sources

| Page | URL |
|---|---|
| HDMA - Hyper DMA | https://www.notion.so/4c5dd3cb256e4e7183d53edea6239795 |
| HDMA Task / Exclusive HDMA / External Sync | https://www.notion.so/cdbf49185a8c494590cbc257f0072a57 |
| Device HDMA Descriptor Caching | https://www.notion.so/179494d2a63d80ac9431fbc65bad06d1 |
| REBELQ SMMU Design | https://www.notion.so/a27418f9fef34eca8ed4c2dd27f55d26 |

## 1. HDMA hardware shape

DesignWare PCIe HDMA (Hyper DMA), part of the Synopsys DesignWare
Cores PCI Express Controller. Sits at the PCIe-controller side, not
on the NoC.

- **Channels**: 1–64 Write + 1–64 Read; ATOM RTL configures 8 + 8.
  REBELQ silicon: 16 WR + 16 RD per chiplet 0 PCIe block.
- **Full duplex**: WR (local→remote / NPU→host) and RD (remote→local
  / host→NPU) run simultaneously.
- **Per-channel SRAM**: 4 KB block to store the LL element queue
  (recycled when it reaches the end of region).
- **Channel separation**: `CC_DMA_PF_ENABLE=1`, `CX_DMAREG_CHADDR_SPACE
  = CHSEP_4K` ⇒ 4 KB per-channel register stride.
- **VSEC DMA**: capability registers in PCIe extended cap space carry
  `PFN/VFN/BAR`, channel separation, and W/R channel counts.

### 1.1 Linked-list mode (the path q-cp uses)

q-cp's `hdma_if.c` sets `chan->mode = HDMA_LL_MODE` for every
`cmd_descr_hdma`. LL mode means:

- DMA controller fetches an LL chain whose head address is in
  `HDMA_LLP_LOW_OFF_{WR,RD}CH_i` (low+high pair).
- LL element format on this hardware is `struct dw_hdma_v0_lli` (data
  element) chained by `struct dw_hdma_v0_llp` (link element). Confirmed
  by the **Device HDMA Descriptor Caching** page's sizing math
  (`MAX_TLB_NUM_PER_CMD * sizeof(struct dw_hdma_v0_lli) +
   sizeof(struct dw_hdma_v0_llp) * 512`).
- `LLEN` bit in `HDMA_CONTROL1_OFF_{WR,RD}CH_i` enables LL mode for
  the channel.
- HDMA decouples descriptor fetch / data fetch / completion / IRQ —
  multiple descriptors processed in parallel.
- Interrupts available in LL mode: **Done**, **Abort**, **Watermark**.
  Watermark fires per LL element completion; useful for streaming.

### 1.2 FW driver API surface (informational)

```c
hdev = rl_hdma_get_dev();
chan = rl_hdma_get_wr_avail_chan(hdev);     /* or get_rd_avail */
desc = rl_hdma_create_xfer_list(chan, ctx, callback_fn);
rl_hdma_push_xfer_list_element(chan, desc, size, sar, dar, flags);
rl_hdma_start_xfer(chan, desc);             /* writes doorbell */
/* IRQ → callback_fn: desc->status == HDMA_COMPLETE,
                       desc->result == HDMA_INT_ST_NO_ERR */
```

The HDMA HAL register set q-cp touches per channel
(`hdma_if.c HDMA_CH_GET_REG/ADDR`):
`enable, doorbell, elem_pf, handshake, llp, cycle, sar, dar,
xfer_size, watermark, ctrl1, func_num, qos, status, int_status,
int_setup, int_clear, msi_stop, msi_watermark, msi_abort, msi_msgd`.

## 2. q-cp task design (HDMA Task / Exclusive HDMA / External Sync)

End-to-end flow once a host BD lands and `cb_parse_*` runs:

```
Q TASK
  Parse BD → allocate BD_TCB → copy CB to device mem (if not reused)
  → parse CB packets:
      LINKED_DMA      → enqueue (with BD_TCB) to HDMA Read Queue
                          + wakeup HDMA TASK
      BARRIER         → if not released, park BD_TCB on WAITING list
      IB              → enqueue to CP Queue

HDMA TASK
  loop:
    dequeue from HDMA Read Queue   → trigger HDMA HW (Read channel)
    dequeue from HDMA Write Queue  → trigger HDMA HW (Write channel)
    sleep when both queues empty / no available channels
  Two modes:
    Exclusive → trigger then sleep until done
    Normal    → fire-and-forget, advance pending_hdma_id on event

HDMA ISR
  All-idle + exclusive waiting          → wakeup HDMA TASK
  Exclusive done                        → wakeup HDMA TASK
  All packets above a BARRIER are done  → move BD_TCB to READY,
                                          wakeup PENDING Q TASK

PENDING Q TASK
  Drain READY list:
    BARRIER → mark done, parse next packet in CB
    HDMA(D2H) → enqueue to HDMA Write Queue + wakeup HDMA TASK
    IB        → enqueue to CP Queue
    end-of-CB → add to COMPLETION list + wakeup COMPLETION TASK

COMPLETION TASK
  Notify host after incrementing CI in the order requested.
```

Task priority (lowest → highest):
`Q TASK < HDMA TASK = PENDING Q TASK < COMPLETION TASK`.

External sync between packets (cross-device sync IDs) is layered on
the same path: HDMA TASK checks `(sync_id, get/put)` before triggering
HW; sleeps if the sync isn't put yet, gets re-woken from HDMA ISR
when the producing device puts.

## 3. Device HDMA Descriptor Caching

Optimization that caches `dw_hdma_v0_lli/llp` chains per
`(context, HTID, command_stream)` so subsequent runs of the same CS
skip descriptor synthesis. Useful here only because it pins down the
descriptor structure types and the memory pool sizing:

- `MAX_ASID = 60`, `MAX_HTID = 4`, `MAX_CS = 2`,
  `MAX_DEV_HDMA_CMD_CNT_PER_CS = 2`, `MAX_TLB_NUM_PER_CMD = 2`,
  `MAX_NODE = 32`.
- Memory pool of 2 MB allocated at boot; descriptors come from it,
  freed back in 4 KB chunks.
- Net benefit on tp16 LLM: descriptor-build cycles dropped 39.31 →
  2.07 (@32 MHz), but no e2e speedup. Feature kept on a branch only.

Not directly needed for P5; kept for context.

## 4. SMMU regime — what HDMA addresses mean

REBELQ SMMU design enumerates how each engine's transactions are
tagged and translated. HDMA-specific points:

- HDMA can operate in **IPA mode** or **PA mode** depending on SID
  configuration.
- HDMA-SID LUT must be configured before any HDMA trigger.
- HDMA TBU does **1-stage translation** (DNC / DDMA TBUs do 2-stage).
  Trying to use a 2-stage page table on the HDMA TBU "conflicts".
- HDMA's transactions are SID-tagged with `SID = 17`; an option is
  also `SSID = 60` with VA = IPA (identical mapping).
- **Bypass region** `0x0 – 0x80_0000_0000`: HDMA address ==
  device PA. Setting all LUT entries to `SID=17, SSID.V=0` makes the
  whole region pass through unchanged. This is the regime kmd /
  q-cp use today on REMU and on early silicon bring-up.
- HDMA only runs on **chiplet 0** (matches `r100-hdma` instantiation
  scope in REMU).

### REMU implication

**Pre-P11**: REMU's `r100_smmu.c` was a register-only stub
(`CR0→CR0ACK`, `CMDQ_CONS=PROD`, no STE / CD / page-table walk),
giving an effective `S1 ∘ S2 = identity`. The Notion bypass-region
rule is *exactly* this regime — HDMA / RBDMA / DNC engines could
call `address_space_{read,write}` directly on the kmd-published
address plus `chiplet_id * R100_CHIPLET_OFFSET` (REMU's flat-global
vs. chiplet-local plumbing) and be silicon-faithful while kmd / q-cp
left the LUT in all-bypass shape.

**P11** replaced the stub with a real stage-2 walker (PF only).
HDMA's NPU-side SAR/DAR/cursor go through
`r100_smmu_translate(SID=0, …)` before each `address_space_*` call;
when `CR0.SMMUEN=0` the translate is identity, so the bypass regime
described above keeps working unchanged. When q-sys's
`smmu_s2_enable` flips STE0.config to ALL_TRANS with stage-2 fields
filled, the walker honours the in-DRAM page tables. v2 follow-ons
(stage-1 walk via CD per SSID, multi-VF, IOTLB cache, dedicated
HDMA-PA-mode SID 16 wiring, host-inbound SID 17 PCIe TBU) are gated
on workload need.

## 5. Direct implications for P5

Today's `src/machine/r100_hdma.c` models the **non-LL doorbell
path**: per-channel `ENABLE / DOORBELL / XFER_SIZE / SAR / DAR /
STATUS / INT_STATUS / INT_CLEAR`, single transfer per doorbell.
That's enough for a hand-driven test (M9-1c) but not enough for
q-cp's `cmd_descr_hdma` because q-cp always uses LL mode.

Closing P5 means adding to `r100-hdma`:

1. **Missing per-channel registers**: `CTRL1` (with the `LLEN` bit),
   `LLP_LO / LLP_HI`, `WATERMARK`, `ELEM_PF`, `HANDSHAKE`, `CYCLE`,
   `FUNC_NUM`, `QOS`, `INT_SETUP`, `MSI_MSGD`, `MSI_STOP`,
   `MSI_WATERMARK`, `MSI_ABORT`. Most can be plain regstore; `LLEN`
   and `LLP_*` are load-bearing for the kick path.
2. **LL walk on doorbell** when `LLEN=1`:
   - read `dw_hdma_v0_llp` at `LLP_LO|HI` (chiplet 0 sysmem),
   - for each chained `dw_hdma_v0_lli`: extract `(size, sar, dar,
     ctrl)` and execute the SAR→DAR copy using the same path as
     today (cross-PCIe → `OP_WRITE`/`OP_READ_REQ` over `hdma`
     chardev; intra-NPU → `address_space_{read,write}` directly),
   - on each element completion with the watermark bit set, raise
     watermark IRQ; on chain end, raise done IRQ.
3. **MSI fan-out**: writing to `MSI_MSGD` at the addresses programmed
   in `msi_{stop,watermark,abort}` is an MSI-X frame back to host —
   reuse the existing `r100-imsix` chardev path so kmd's IRQ handler
   gets fed naturally.
4. **DVA passthrough** stays identity-mapped per the SMMU doc — no
   new translation hook needed for P5; `chiplet_id *
   R100_CHIPLET_OFFSET` add is sufficient.

The bidirectional `hdma` chardev wire format and the `0x80..0xBF`
`req_id` partition are already in place from M9-1c — P5 only adds
LL-walk callers, not a new transport.
