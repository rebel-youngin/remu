/*
 * REMU - R100 NPU System Emulator
 * Designware DWC_SSI master-QSPI controller stub (QSPI_ROT)
 *
 * Unlike the `r100-qspi-bridge` device, which sits on PERI1 and models the
 * inter-chiplet register-access protocol used by BL1 during secondary-chiplet
 * discovery, this device models the *master* QSPI controller that the R100
 * FW uses to reach the on-board serial NOR flash. Its silicon base is
 *
 *   QSPI_ROT_OFFSET         = 0x1FF0500000  (config-space)
 *   QSPI_ROT_PRIVATE        = 0x1E00500000  (chiplet-local private alias)
 *
 * FreeRTOS's driver (external/.../q/sys/drivers/qspi_boot/qspi_boot.c) hard-
 * codes its reg_base to `QSPI_ROT_PRIVATE`, so every CP0 core on every
 * chiplet reaches this device via its own chiplet-local CPU view. BL31's
 * SMC handler (external/.../tf-a/services/std_svc/nor_flash/nor_flash_main.c)
 * calls into the same driver for NOR_FLASH_SVC_{READ_DATA,ERASE_4K,...}.
 *
 * Without a status-register model BL31's `erase_flash()` -> `write_enable_
 * command()` -> `qspi_ssi_tx_i()` -> `tx_available()` spins for 2 000 000
 * microseconds on `SR.TFNF` / `!SR.BUSY` before printing
 *
 *     INFO:    spi tx available timeout error
 *
 * to the primary UART. On silicon the DW SSI drains its TX FIFO every cycle
 * and the poll exits on first iteration; we mirror that "always idle"
 * behaviour and return a status byte from the data register that satisfies
 * the two response checks the driver actually performs:
 *
 *   check_read_status()      -> ret & WRITE_ENABLE_LATCH (BIT(1) = 0x02)
 *   check_read_flag_status() -> ret & FLASH_READY        (BIT(7) = 0x80)
 *
 * We therefore return (FLASH_READY | WRITE_ENABLE_LATCH) = 0x82 on DRX
 * reads. READ_STATUS_COMMAND and READ_FLAG_STATUS each test a different
 * bit, so a constant 0x82 passes both without having to track which
 * opcode was last issued.
 *
 * Actual flash write/erase is *not* modelled here — the backing RAM for
 * the flash staging region already satisfies direct-read (`flash_nor_read`)
 * memcpys, and nothing in Phase 1 depends on the log-erase persisting. If
 * log persistence is later needed, the FIFO writer can be routed into
 * address_space_write against R100_FLASH_BASE; the sparse status model
 * here stays correct.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "r100_soc.h"

/* Synopsys DWC_SSI register offsets (struct dwc_ssi_synopsys in qspi_boot.h). */
#define DWC_SSI_CTRLR0      0x00
#define DWC_SSI_CTRLR1      0x04
#define DWC_SSI_SSIENR      0x08
#define DWC_SSI_MWCR        0x0C
#define DWC_SSI_SER         0x10
#define DWC_SSI_BAUDR       0x14
#define DWC_SSI_TXFTLR      0x18
#define DWC_SSI_RXFTLR      0x1C
#define DWC_SSI_TXFLR       0x20
#define DWC_SSI_RXFLR       0x24
#define DWC_SSI_SR          0x28
#define DWC_SSI_IMR         0x2C
#define DWC_SSI_ISR         0x30
#define DWC_SSI_RISR        0x34
#define DWC_SSI_DRX_BASE    0x60  /* DRX[0..35] — TX/RX FIFO access */
#define DWC_SSI_DRX_END     (0x60 + 36 * 4)

/* SR bits (dwc_ssi 2.00a: see DW_SR_*_BIT in qspi_boot.h). */
#define DWC_SSI_SR_BUSY     (1u << 0)
#define DWC_SSI_SR_TFNF     (1u << 1)  /* TX FIFO not full */
#define DWC_SSI_SR_TFE      (1u << 2)  /* TX FIFO empty */
#define DWC_SSI_SR_RFNE     (1u << 3)  /* RX FIFO not empty */

/*
 * Status byte returned to DRX reads. FLASH_READY (bit 7) satisfies
 * READ_FLAG_STATUS, WRITE_ENABLE_LATCH (bit 1) satisfies READ_STATUS_
 * COMMAND's check_write_enable_command_available() poll.
 */
#define R100_QSPI_BOOT_STATUS_READY 0x82u

#define R100_QSPI_BOOT_REG_COUNT    (R100_QSPI_ROT_REG_SIZE / 4)

typedef struct R100QSPIBootState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    /*
     * Sparse register file — 64 KB is larger than necessary, but it costs
     * 256 KB of zero-initialised host memory and keeps us from having to
     * enumerate every reset default in the DWC_SSI spec. Writes go through
     * unchanged so driver code that reads back what it just programmed
     * (baudr, spi_ctrlr0, xip_*) sees coherent values.
     */
    uint32_t regs[R100_QSPI_BOOT_REG_COUNT];
    uint32_t chiplet_id;
} R100QSPIBootState;

#define TYPE_R100_QSPI_BOOT "r100-qspi-boot"
DECLARE_INSTANCE_CHECKER(R100QSPIBootState, R100_QSPI_BOOT,
                         TYPE_R100_QSPI_BOOT)

static uint64_t r100_qspi_boot_read(void *opaque, hwaddr addr, unsigned size)
{
    R100QSPIBootState *s = R100_QSPI_BOOT(opaque);

    switch (addr) {
    case DWC_SSI_SR:
        /*
         * Always idle + ready: TX FIFO not full, TX FIFO empty, RX FIFO
         * "has data" (so rx_available() passes on first iteration), and
         * BUSY clear. This exits every poll in qspi_boot.c on iteration 0.
         */
        return DWC_SSI_SR_TFNF | DWC_SSI_SR_TFE | DWC_SSI_SR_RFNE;

    case DWC_SSI_TXFLR:
        /* TX FIFO level: always empty — raw_write_flash's
         * `cnt = PREVENT_OVERFLOW - txflr` stays at 30. */
        return 0;

    case DWC_SSI_RXFLR:
        /* RX FIFO level: always 1, so any driver-side drain loop that
         * waits for a non-zero count terminates. */
        return 1;

    default:
        if (addr >= DWC_SSI_DRX_BASE && addr < DWC_SSI_DRX_END) {
            /*
             * DRX read: return a constant status byte satisfying both
             * READ_STATUS_COMMAND (needs WRITE_ENABLE_LATCH, bit 1) and
             * READ_FLAG_STATUS (needs FLASH_READY, bit 7). The driver
             * reads the low 8 bits after a single-byte instruction was
             * pushed, so any high-bit clutter is harmless.
             */
            return R100_QSPI_BOOT_STATUS_READY;
        }
        if ((addr >> 2) < R100_QSPI_BOOT_REG_COUNT) {
            return s->regs[addr >> 2];
        }
        return 0;
    }
}

static void r100_qspi_boot_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    R100QSPIBootState *s = R100_QSPI_BOOT(opaque);

    /*
     * DRX writes model a zero-depth TX FIFO: every word is "drained"
     * instantly. For Phase 1 the actual wire protocol (erase_4k, write
     * page, program VCR) is not needed — the flash staging region is a
     * plain RAM model accessed via direct memory-mapped reads, and no
     * code path depends on the erase being persisted across boots.
     *
     * We still capture the write into the register file so a debugger
     * reading back DRX at the same offset sees the last-written byte.
     */
    if ((addr >> 2) < R100_QSPI_BOOT_REG_COUNT) {
        s->regs[addr >> 2] = (uint32_t)val;
    }
}

static const MemoryRegionOps r100_qspi_boot_ops = {
    .read = r100_qspi_boot_read,
    .write = r100_qspi_boot_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_qspi_boot_realize(DeviceState *dev, Error **errp)
{
    R100QSPIBootState *s = R100_QSPI_BOOT(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_qspi_boot_ops, s,
                          "r100-qspi-boot", R100_QSPI_ROT_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_qspi_boot_reset(DeviceState *dev)
{
    R100QSPIBootState *s = R100_QSPI_BOOT(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static Property r100_qspi_boot_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100QSPIBootState, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_qspi_boot_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_qspi_boot_realize;
    device_class_set_legacy_reset(dc, r100_qspi_boot_reset);
    device_class_set_props(dc, r100_qspi_boot_properties);
}

static const TypeInfo r100_qspi_boot_info = {
    .name = TYPE_R100_QSPI_BOOT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100QSPIBootState),
    .class_init = r100_qspi_boot_class_init,
};

static void r100_qspi_boot_register_types(void)
{
    type_register_static(&r100_qspi_boot_info);
}

type_init(r100_qspi_boot_register_types)
