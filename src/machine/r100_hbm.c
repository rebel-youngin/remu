/*
 * REMU - R100 NPU System Emulator
 * HBM3 memory controller stub device
 *
 * Covers the full HBM3 controller window (16x CON, 16x PHY, ICON),
 * returning 0xFFFFFFFF for any unwritten offset so the FW's DFI/PHY
 * training "complete" polls succeed on every channel. Written values
 * are kept in a sparse hash so read-modify-write sequences work.
 *
 * A few registers need custom behaviour:
 *   - ICON test_instruction_req0 (0x5F0008): default 0 (no outstanding
 *     request) so the FW's `req0 |= (1<<bit)` / `req0 &= ~(1<<bit)`
 *     RMW sequence cleanly transitions between non-zero and zero.
 *   - ICON test_instruction_req1.test_done (bit 5 of 0x5F000C) is
 *     synthesised from req0: non-zero req0 → test_done=1, req0==0 →
 *     test_done=0. See drivers/hbm3/icon.c:icon_test_instruction_req()
 *     for the exact FW pattern.
 *   - ICON EXTEST lane-repair scan (drivers/hbm3/icon.c:icon_extest):
 *     BL2 runs 4 sub-phases per channel (TX0/TX1/RX0/RX1), each of
 *     which writes a known pattern to `wr_wdr0` (or the WR_WDR_IDX
 *     register at offset 0x58), asserts extest_{tx,rx}_req, then reads
 *     `rd_wdrIDX_ch_X` and XORs against a mode-specific expected
 *     vector. We detect the mode from the value latched in wr_wdr0 at
 *     the moment extest_{tx,rx}_req gets set, and return the matching
 *     expected pattern on subsequent rd_wdr reads so
 *     compare_result==0 and rep_status[ch]==0 for every channel.
 *     Without this, every channel reads `HBM_UNREPAIRABLE` and BL2
 *     stops with "CH[N] Unrepairable PKG" → "HBM Boot on chiplet{N}
 *     FAIL". See docs/roadmap.md (Phase 1) and icon.c icon_tx_extest /
 *     icon_rx_extest for the full sequence.
 *
 * Mapped at DRAM_CNTL_BASE (0x1FF7400000) per chiplet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

#define HBM_STATUS_READY            0xFFFFFFFFU

/*
 * PHY block window relative to HBM stub base (0x1FF7400000):
 *   HBM3_PHY_0_BASE = 0x1FF7800000 → stub-relative 0x400000.
 *   16 PHY blocks at 0x10000 stride → ends at stub-relative 0x500000.
 *
 * hbm3_run_phy_training() in tf-a/drivers/hbm3/hbm3_init.c is
 * dominated by *fail-status* / *overflow* / *busy* bitfields that
 * should read back 0 on a clean pass (Write Leveling, Read/Write
 * Training, Vref search, PRBS, duty-cycle calibration, ...). The
 * legacy 0xFFFFFFFF default made every such check look like a
 * hardware failure, so we flip the PHY default to 0 and then
 * whitelist the few offsets that are polled for a "done = 1" bit:
 *
 *   - cal_con4    (0x14): phy_train_done is bit 7, polled by
 *     phy_do_training_resp() for every training phase.
 *   - prbs_con0   (0x390): prbs_write_done (bit 4) / prbs_read_done
 *     (bit 5), polled by phy_do_prbs_*_training_resp().
 *   - scheduler_state (0x44c): schd_fifo_empty_status is bit 4,
 *     polled at the end of phy_command_training() (CBT Done).
 *
 * Any write from the FW is still preserved verbatim by the sparse
 * hash, so RMW sequences like `cal_con4.phy_train_en = 1` keep
 * working — the FW clears the enable bit after training and the
 * hash drops back to the default for the "done" poll.
 */
#define HBM_PHY_REGION_START        0x400000U
#define HBM_PHY_REGION_END          0x500000U

/* Per-channel PHY offsets (hbm3ephy.h) of the "done" polling regs. */
#define HBM_PHY_CAL_CON4_OFFSET         0x014U
#define HBM_PHY_CAL_CON4_TRAIN_DONE     (1U << 7)
#define HBM_PHY_PRBS_CON0_OFFSET        0x390U
#define HBM_PHY_PRBS_CON0_WRITE_DONE    (1U << 4)
#define HBM_PHY_PRBS_CON0_READ_DONE     (1U << 5)
#define HBM_PHY_SCHED_STATE_OFFSET      0x44CU
#define HBM_PHY_SCHED_FIFO_EMPTY        (1U << 4)

/* Offsets relative to the HBM stub base (0x1FF7400000). */
#define HBM_ICON_OFFSET             0x5F0000U     /* 0x1FF79F0000 */
#define HBM_ICON_CTRL_CON0_OFFSET   (HBM_ICON_OFFSET + 0x0U)
#define HBM_ICON_REQ0_OFFSET        (HBM_ICON_OFFSET + 0x8U)
#define HBM_ICON_REQ1_OFFSET        (HBM_ICON_OFFSET + 0xCU)
#define HBM_ICON_REQ1_TEST_DONE     (1U << 5)
#define HBM_ICON_CTRL_SW_MODE_REQ   (1U << 29)

/* test_instruction_req0 bits (ieee1500_icon.h). */
#define HBM_ICON_REQ0_EXTEST_RX     (1U << 1)
#define HBM_ICON_REQ0_EXTEST_TX     (1U << 2)

/*
 * WR_WDR_IDX(addr, idx, v)  writes at ICON + 0x58 - idx*4 (idx=0..18).
 * RD_WDR_CH(addr, idx, ch)  reads  at ICON + 0xa4 + 0x4c*ch - idx*4.
 *
 * Derived rd_wdr window (16 channels × 19 indices):
 *   [ICON+0x5c, ICON+0x54c) — rd_wdr18_ch_0 .. rd_wdr0_ch_15.
 */
#define HBM_ICON_WR_WDR0_OFFSET     (HBM_ICON_OFFSET + 0x58U)
#define HBM_ICON_RD_WDR_START       (HBM_ICON_OFFSET + 0x5CU)
#define HBM_ICON_RD_WDR_END         (HBM_ICON_OFFSET + 0x54CU)
#define HBM_ICON_RD_WDR_CH_STRIDE   0x4CU
#define HBM_ICON_RD_WDR_MAX_IDX     19U

/* Mode-specific TX1/RX0/RX1 expected patterns, verbatim from icon.c. */
#define EXTEST_MODE_NONE            0U
#define EXTEST_MODE_TX0             1U
#define EXTEST_MODE_TX1             2U
#define EXTEST_MODE_RX0             3U
#define EXTEST_MODE_RX1             4U

/* wr_wdr0 sentinels latched by FW right before asserting extest_*_req. */
#define EXTEST_TX0_WDR0_PATTERN     0x40040000U
#define EXTEST_TX1_WDR0_PATTERN     0xBFFBFFFFU

/* icon.c:icon_tx_extest() — TX1 compare vector (19 entries). */
static const uint32_t extest_tx1_expected[HBM_ICON_RD_WDR_MAX_IDX] = {
    0x42108421, 0x40842108, 0x10842108, 0x84210842,
    0x21084210, 0x21081084, 0x08421084, 0x42108421,
    0x10842108, 0x10842102, 0x84210842, 0x21084210,
    0x20421084, 0x08421084, 0x42108421, 0x10842108,
    0x10840842, 0x84210842, 0x00000010,
};

/* icon.c:icon_rx_extest() — RX compare vectors (only idx 0..3 are read). */
static const uint32_t extest_rx0_pattern[4] = {
    0x40040000, 0x00000000, 0x04000004, 0x00000040,
};
static const uint32_t extest_rx1_pattern[4] = {
    0xBFFBFFFF, 0xFFFFFFFF, 0xFBFFFFFB, 0x00FFFFBF,
};

static bool r100_hbm_lookup(R100HBMState *s, hwaddr addr, uint32_t *out)
{
    gpointer val;

    if (!s->regs) {
        return false;
    }
    if (!g_hash_table_lookup_extended(s->regs,
                                      GUINT_TO_POINTER((guint)addr),
                                      NULL, &val)) {
        return false;
    }
    *out = (uint32_t)GPOINTER_TO_UINT(val);
    return true;
}

/*
 * Return the "no-error" compare vector for the active EXTEST sub-phase.
 * `idx` is the WDR index (0..18); `ch` is the HBM channel (0..15, unused
 * since all 16 channels share the same expected pattern).
 */
static uint32_t r100_hbm_extest_expected(uint8_t mode, uint32_t idx)
{
    switch (mode) {
    case EXTEST_MODE_TX0:
        /* compare_result = rdata ^ 0 ⇒ expected value is 0. */
        return 0;
    case EXTEST_MODE_TX1:
        if (idx < ARRAY_SIZE(extest_tx1_expected)) {
            return extest_tx1_expected[idx];
        }
        return 0;
    case EXTEST_MODE_RX0:
        if (idx < ARRAY_SIZE(extest_rx0_pattern)) {
            return extest_rx0_pattern[idx];
        }
        return 0;
    case EXTEST_MODE_RX1:
        if (idx < ARRAY_SIZE(extest_rx1_pattern)) {
            return extest_rx1_pattern[idx];
        }
        return 0;
    default:
        return HBM_STATUS_READY;
    }
}

static uint64_t r100_hbm_read(void *opaque, hwaddr addr, unsigned size)
{
    R100HBMState *s = R100_HBM(opaque);
    uint32_t stored;
    uint64_t ret;

    /*
     * ICON icon_ctrl_con0 and test_instruction_req0 default to 0 so
     * the FW's RMW `|=` / `&=~` cycle transitions cleanly between
     * non-zero and zero values. Any explicit write is preserved.
     */
    if (addr == HBM_ICON_CTRL_CON0_OFFSET ||
        addr == HBM_ICON_REQ0_OFFSET) {
        ret = r100_hbm_lookup(s, addr, &stored) ? stored : 0;
        return ret;
    }

    /*
     * Synthesise test_done (bit 5 of test_instruction_req1) based on
     * whether any request is active: either a bit in req0 is set, or
     * icon_ctrl_con0.sw_mode_req (bit 29) is set. Firmware uses both
     * paths — see drivers/hbm3/icon.c icon_test_instruction_req() /
     * icon_sw_instruction_req(). Other bits of req1 (ch_select, etc.)
     * keep whatever was last written.
     */
    if (addr == HBM_ICON_REQ1_OFFSET) {
        uint32_t req0 = 0;
        uint32_t ctrl = 0;
        uint32_t req1 = HBM_STATUS_READY;
        bool active;

        (void)r100_hbm_lookup(s, HBM_ICON_REQ0_OFFSET, &req0);
        (void)r100_hbm_lookup(s, HBM_ICON_CTRL_CON0_OFFSET, &ctrl);
        (void)r100_hbm_lookup(s, HBM_ICON_REQ1_OFFSET, &req1);
        active = (req0 != 0) || ((ctrl & HBM_ICON_CTRL_SW_MODE_REQ) != 0);
        if (active) {
            req1 |= HBM_ICON_REQ1_TEST_DONE;
        } else {
            req1 &= ~HBM_ICON_REQ1_TEST_DONE;
        }
        qemu_log_mask(LOG_UNIMP,
                      "r100-hbm.cl%u: ICON req1 read (req0=0x%x ctrl=0x%x)"
                      " -> 0x%x\n",
                      s->chiplet_id, req0, ctrl, req1);
        return req1;
    }

    /*
     * During an active EXTEST scan, synthesise rd_wdrIDX_ch_X so the
     * FW's XOR compare lands on 0. The rd_wdr window spans 16 channels
     * with a 0x4c stride; within each channel idx 18 is at the lowest
     * offset and idx 0 at the highest (0x48 above channel base).
     */
    if (s->extest_mode != EXTEST_MODE_NONE &&
        addr >= HBM_ICON_RD_WDR_START && addr < HBM_ICON_RD_WDR_END) {
        uint32_t rel = (uint32_t)(addr - HBM_ICON_RD_WDR_START);
        uint32_t oc = rel % HBM_ICON_RD_WDR_CH_STRIDE;
        uint32_t idx = (HBM_ICON_RD_WDR_MAX_IDX - 1U) - (oc / 4U);

        return r100_hbm_extest_expected(s->extest_mode, idx);
    }

    if (r100_hbm_lookup(s, addr, &stored)) {
        return stored;
    }

    /*
     * PHY region (HBM3_PHY_0_BASE..+16*0x10000). The PHY's training
     * registers are mostly "busy" / "fail" / "overflow" status bits
     * that should be 0 for a clean pass, so default 0 gets every
     * polling loop through on the first read:
     *
     *   - phy_recv_offset_calibration polls duty_ctrl_en.busy (bit
     *     31 of 0x7d0) and branches while set.
     *   - phy_wdqs2ck_training reads wr_lvl_fail_stat0 / overflow
     *     (0x844 / 0x848) and errors out if non-zero.
     *   - phy_{read,write}_training_fail_check reads rd/wr_cal_fail_*
     *     and errors out on non-zero.
     *
     * Polls that wait for a "done" = 1 bit (phy_train_done,
     * schd_fifo_empty_status, prbs_{read,write}_done, zq_done, ...)
     * must therefore be handled explicitly — we set their bits in
     * r100_hbm_phy_default() below.
     */
    if (addr >= HBM_PHY_REGION_START && addr < HBM_PHY_REGION_END) {
        uint32_t ch_off = (uint32_t)((addr - HBM_PHY_REGION_START) & 0xFFFFU);
        switch (ch_off) {
        case HBM_PHY_CAL_CON4_OFFSET:
            return HBM_PHY_CAL_CON4_TRAIN_DONE;
        case HBM_PHY_PRBS_CON0_OFFSET:
            return HBM_PHY_PRBS_CON0_WRITE_DONE | HBM_PHY_PRBS_CON0_READ_DONE;
        case HBM_PHY_SCHED_STATE_OFFSET:
            return HBM_PHY_SCHED_FIFO_EMPTY;
        default:
            return 0;
        }
    }

    return HBM_STATUS_READY;
}

static void r100_hbm_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    R100HBMState *s = R100_HBM(opaque);

    if (addr == HBM_ICON_CTRL_CON0_OFFSET ||
        addr == HBM_ICON_REQ0_OFFSET ||
        addr == HBM_ICON_REQ1_OFFSET) {
        const char *name = addr == HBM_ICON_CTRL_CON0_OFFSET ? "ctrl_con0"
                         : addr == HBM_ICON_REQ0_OFFSET      ? "req0"
                                                             : "req1";
        qemu_log_mask(LOG_UNIMP,
                      "r100-hbm.cl%u: ICON %s write 0x%"PRIx64"\n",
                      s->chiplet_id, name, val);
    }
    g_hash_table_insert(s->regs, GUINT_TO_POINTER((guint)addr),
                        GUINT_TO_POINTER((guint)(uint32_t)val));

    /*
     * Latch the EXTEST sub-phase when the FW asserts extest_tx_req /
     * extest_rx_req in test_instruction_req0. The sub-phase (TX0 vs
     * TX1, RX0 vs RX1) is determined by the value previously written
     * to wr_wdr0 (offset 0x58) — TX sets it explicitly to a magic
     * pattern, RX leaves the last WR_WDR_IDX(0,·) value there.
     *
     * The mode stays latched *after* the FW clears the req bit,
     * because the FW reads rd_wdr only once that clear has completed
     * (see icon_tx_extest / icon_rx_extest). In hardware the scan
     * result remains valid until another scan is triggered, so we
     * mimic that and only overwrite extest_mode on the next req
     * assertion.
     */
    if (addr == HBM_ICON_REQ0_OFFSET) {
        uint32_t v = (uint32_t)val;
        uint32_t wdr0 = 0;

        (void)r100_hbm_lookup(s, HBM_ICON_WR_WDR0_OFFSET, &wdr0);

        if (v & HBM_ICON_REQ0_EXTEST_TX) {
            if (wdr0 == EXTEST_TX0_WDR0_PATTERN) {
                s->extest_mode = EXTEST_MODE_TX0;
            } else if (wdr0 == EXTEST_TX1_WDR0_PATTERN) {
                s->extest_mode = EXTEST_MODE_TX1;
            } else {
                qemu_log_mask(LOG_UNIMP,
                              "r100-hbm.cl%u: EXTEST TX req with "
                              "unknown wr_wdr0=0x%x\n",
                              s->chiplet_id, wdr0);
                s->extest_mode = EXTEST_MODE_NONE;
            }
        } else if (v & HBM_ICON_REQ0_EXTEST_RX) {
            s->extest_mode = (wdr0 == 0) ? EXTEST_MODE_RX0 : EXTEST_MODE_RX1;
        }
    }
}

static const MemoryRegionOps r100_hbm_ops = {
    .read = r100_hbm_read,
    .write = r100_hbm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_hbm_realize(DeviceState *dev, Error **errp)
{
    R100HBMState *s = R100_HBM(dev);
    char name[64];

    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
    snprintf(name, sizeof(name), "r100-hbm.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_hbm_ops, s,
                          name, R100_HBM_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_hbm_reset(DeviceState *dev)
{
    R100HBMState *s = R100_HBM(dev);

    if (s->regs) {
        g_hash_table_remove_all(s->regs);
    }
    s->extest_mode = EXTEST_MODE_NONE;
}

static Property r100_hbm_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100HBMState, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_hbm_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_hbm_realize;
    device_class_set_legacy_reset(dc, r100_hbm_reset);
    device_class_set_props(dc, r100_hbm_properties);
}

static const TypeInfo r100_hbm_info = {
    .name = TYPE_R100_HBM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100HBMState),
    .class_init = r100_hbm_class_init,
};

static void r100_hbm_register_types(void)
{
    type_register_static(&r100_hbm_info);
}

type_init(r100_hbm_register_types)
