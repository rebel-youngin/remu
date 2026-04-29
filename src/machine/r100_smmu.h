/*
 * REMU - R100 NPU System Emulator
 * SMMU-600 TCU — public translate API
 *
 * This header exposes only what other NPU-side device models (engines
 * that consume DVAs — r100-rbdma, r100-hdma, eventually r100-dnc-cluster
 * cmd_descr fields) need from r100_smmu.c: the QOM type name, an opaque
 * R100SMMUState typedef so they can hold pointers in their own state and
 * DEFINE_PROP_LINK targets, the public translate entry point, and a
 * fault-stringifier for diagnostics.
 *
 * The full R100SMMUState layout, register-cache fields, STE-decode
 * helpers, and R100_SMMU() cast macro stay private to r100_smmu.c per
 * project header discipline (CLAUDE.md → "Header discipline").
 *
 * Scope (P11a / v1):
 *   - Stage-2 only. Matches q-sys's `smmu_init_ste` (STE1.S1DSS=BYPASS
 *     for SIDs 0..4 → stage-1 disabled) + `smmu_s2_enable` flipping
 *     STE0.config to ALL_TRANS with the stage-2 fields filled.
 *     Stage-1 STEs (config==S1_TRANS) fall through to identity in v1
 *     with a LOG_UNIMP log line so a future stage-1 walker is easy to
 *     plug in.
 *   - PF only on chiplet 0 (func_id=0 / num_vfs=0 — q-cp's
 *     `host_queue_manager.c:456 ptw_init_smmu_s2(0)`). Multi-VF (SIDs
 *     1..4 with per-VF tables) is v2.
 *   - No IOTLB cache. The LL chains we walk for cb[0] / cb[1] are 3-4
 *     elements; cost is dominated by the chain reads, not the page
 *     walk. Add a cache when a workload demands it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef R100_SMMU_H
#define R100_SMMU_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "exec/hwaddr.h"

#define TYPE_R100_SMMU          "r100-smmu"

typedef struct R100SMMUState R100SMMUState;

/*
 * Result of a single r100_smmu_translate() call. On success, `pa` is
 * the QEMU global physical address (the same one a caller would feed
 * to `address_space_{read,write}(&address_space_memory, ...)`); on
 * failure, `fault` indicates which of the SMMU-600 PTW event types
 * triggered, and the engine should bail with LOG_GUEST_ERROR.
 *
 * The fault classes mirror the subset of `SMMUPTWErrorType` that
 * `smmu_ptw_64_s2` produces (`smmu-common.c`) plus three R100-specific
 * decode failures (invalid STE, stage-1 not implemented in v1,
 * mis-typed STE config). Stage-1 fault classes are reserved for v2.
 */
typedef enum {
    R100_SMMU_OK = 0,
    R100_SMMU_FAULT_INV_STE,         /* STE0.V=0 or config==ABORT */
    R100_SMMU_FAULT_STE_FETCH,       /* STRTAB read failed */
    R100_SMMU_FAULT_S1_NOT_IMPL,     /* config==S1_TRANS — v1 no-op (identity + log) */
    R100_SMMU_FAULT_S2_TRANSLATION,  /* PTE invalid / IPA out of S2T0SZ */
    R100_SMMU_FAULT_S2_PERMISSION,   /* PTE.S2AP forbids access */
    R100_SMMU_FAULT_S2_ACCESS,       /* PTE.AF=0 + S2AFFD=0 */
    R100_SMMU_FAULT_S2_ADDR_SIZE,    /* PA out of S2PS or IPA out of S2T0SZ */
    R100_SMMU_FAULT_WALK_EABT,       /* Page-table walk read failed */
} R100SMMUFault;

typedef struct R100SMMUTranslateResult {
    bool          ok;            /* false → check fault */
    hwaddr        pa;            /* valid only when ok */
    R100SMMUFault fault;         /* valid only when !ok */
    hwaddr        fault_addr;    /* IPA / IOVA that faulted, for logging */
    uint32_t      sid;           /* echoed for logging */
} R100SMMUTranslateResult;

/* Access-type tags for the translate hook. We don't report stage-1
 * permissions today, but stage-2 needs to know read vs. write to
 * apply S2AP correctly (q-sys sets `STE2_S2R` for read+write on the
 * PF table, so this is mostly observability today — but engines like
 * RBDMA OTO should still pass the right access for each leg of a
 * src→dst copy so future stricter PTEs don't surprise us). */
#define R100_SMMU_ACCESS_READ   0
#define R100_SMMU_ACCESS_WRITE  1

/*
 * Translate a Device Virtual Address (DVA / IPA, depending on STE
 * config) to a QEMU global physical address.
 *
 * Caller contract:
 *   - `s` is the chiplet's r100-smmu device pointer (typically obtained
 *     via a QOM `link<r100-smmu>` property on the calling engine, set
 *     up in `r100_soc.c` machine wiring).
 *   - `sid` is the Stream ID for the master making the access. The
 *     SID-to-master allocation lives in `docs/rbdma-smmu-review.md` § 1
 *     (18 SIDs, RBDMA at 0..3, HDMA at 5..12 with one per WR/RD pair,
 *     PCIe TBU at 17, etc.). Out-of-range `sid` → INV_STE fault.
 *   - `ssid` is the SubStream ID for stage-1 (CD index). v1 ignores it
 *     since stage-1 is bypass; pass 0.
 *   - `dva` is what the engine read from the descriptor / register —
 *     untranslated.
 *   - `access` is one of R100_SMMU_ACCESS_READ / R100_SMMU_ACCESS_WRITE.
 *   - `out` is the result block. Caller pre-zeroes; this fills in
 *     `ok` + `pa` (or `fault` + `fault_addr` on miss) and copies
 *     `sid` for log lines.
 *
 * Behavior:
 *   - If `CR0.SMMUEN=0` (FW hasn't enabled the SMMU yet, e.g. early
 *     boot): identity (out->pa = dva, ok=true). Matches Arm-SMMU
 *     pre-enable bypass behaviour and lets engine bring-up tests run
 *     before FW programs any STEs.
 *   - If STE0.config == BYPASS: identity.
 *   - If STE0.config == ABORT or !STE0.V: INV_STE fault.
 *   - If STE0.config == S1_TRANS: v1 identity + LOG_UNIMP. Returns
 *     ok=true so existing engines keep moving — q-cp's `smmu_init_ste`
 *     sets STE1.S1DSS=BYPASS for the 4 SIDs it leaves at S1_TRANS, so
 *     the effective behaviour is identity. v2 walks the CD chain.
 *   - If STE0.config == S2_TRANS or ALL_TRANS: build SMMUTransCfg
 *     from STE2/STE3 and dispatch to QEMU's `smmu_ptw()` with
 *     stage=SMMU_STAGE_2. ALL_TRANS treats the IPA as the S1-bypass
 *     output (kmd's "stage-1 bypass + stage-2 walk" regime that
 *     q-sys's `smmu_s2_enable` programs).
 *
 * The translate is BQL-safe (BQL must be held — same constraint as
 * the engine-side `address_space_*` calls it pairs with) and stateless
 * across calls in v1.
 */
void r100_smmu_translate(R100SMMUState *s, uint32_t sid, uint32_t ssid,
                         hwaddr dva, int access,
                         R100SMMUTranslateResult *out);

/* Returns a static printable name for `fault` ("ok", "inv_ste",
 * "s2_translation", ...). Useful in LOG_GUEST_ERROR lines from
 * engines so the diagnostic tail says *why* the walk failed. */
const char *r100_smmu_fault_str(R100SMMUFault fault);

#endif /* R100_SMMU_H */
