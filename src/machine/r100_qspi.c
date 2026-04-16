/*
 * REMU - R100 NPU System Emulator
 * QSPI Bridge device model (Designware DWC_ssi based)
 *
 * The QSPI bridge provides inter-chiplet register access. Chiplet 0 (primary)
 * uses channel 2 to read/write registers on chiplets 1-3 (secondary).
 *
 * During BL1 boot, the primary chiplet discovers secondaries by:
 *   1. qspi_bridge_init(ch2)
 *   2. qspi_bridge_set_upper_addr(ch2, 0, chiplet_id)
 *   3. qspi_bridge_read_1word(ch2, SYSREG_SYSREMAP_CHIPLET, chiplet_id)
 *   4. If returned value == chiplet_id, chiplet is present
 *
 * The bridge uses Designware SSI registers. We model the key registers
 * and implement the read/write instruction protocol:
 *   - Instruction 0x70: Read 1 word from remote address
 *   - Instruction 0x80: Write 1 word to remote address
 *
 * Since all chiplets share QEMU's system address space, cross-chiplet
 * access is implemented as direct memory reads/writes at the appropriate
 * chiplet offset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "r100_soc.h"

/* Designware SSI register offsets (byte addresses) */
#define DW_SSI_CTRLR0       0x00
#define DW_SSI_CTRLR1       0x04
#define DW_SSI_SSIENR       0x08
#define DW_SSI_MWCR         0x0C
#define DW_SSI_SER          0x10
#define DW_SSI_BAUDR        0x14
#define DW_SSI_TXFTLR       0x18
#define DW_SSI_RXFTLR       0x1C
#define DW_SSI_TXFLR        0x20
#define DW_SSI_RXFLR        0x24
#define DW_SSI_SR           0x28
#define DW_SSI_IMR          0x2C
#define DW_SSI_ISR          0x30
#define DW_SSI_RISR         0x34
#define DW_SSI_DRX_BASE     0x60    /* Data register FIFO base */
#define DW_SSI_SPI_CTRLR0   0xF4

/* SR (Status Register) bits */
#define SR_BUSY             (1 << 0)
#define SR_TFNF             (1 << 1)  /* TX FIFO not full */
#define SR_TFE              (1 << 2)  /* TX FIFO empty */
#define SR_RFNE             (1 << 3)  /* RX FIFO not empty */

/* QSPI instructions used by the bridge driver */
#define QSPI_INST_READ_24WAIT  0x70
#define QSPI_INST_WRITE        0x80

/* State machine for tracking multi-word DRX writes */
typedef enum {
    QSPI_STATE_IDLE,
    QSPI_STATE_GOT_INSTRUCTION,
    QSPI_STATE_GOT_ADDRESS,
} QSPIXferState;

#define QSPI_REG_SIZE       0x100
#define QSPI_REG_COUNT      (QSPI_REG_SIZE / 4)

typedef struct R100QSPIBridgeState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[QSPI_REG_COUNT];

    /* Transfer state machine */
    QSPIXferState xfer_state;
    uint32_t instruction;
    uint32_t address;       /* 24-bit address from command */
    uint32_t upper_addr;    /* Upper address bits (set via set_upper_addr) */
    uint32_t rx_data;       /* Data read from remote chiplet */
    bool rx_data_valid;

    /* Currently selected slave (chiplet ID 1-3) */
    uint32_t selected_slave;

    /* Chiplet ID of the bridge itself (always 0 for primary) */
    uint32_t chiplet_id;
} R100QSPIBridgeState;

#define TYPE_R100_QSPI_BRIDGE "r100-qspi-bridge"
DECLARE_INSTANCE_CHECKER(R100QSPIBridgeState, R100_QSPI_BRIDGE,
                         TYPE_R100_QSPI_BRIDGE)

/*
 * Perform a cross-chiplet read via QEMU's system address space.
 * The target address is: slave_chiplet * CHIPLET_OFFSET + full_addr
 * where full_addr = (upper_addr & 0xFC000000) | (address << 2)
 */
static uint32_t qspi_remote_read(R100QSPIBridgeState *s)
{
    uint64_t full_addr;
    uint64_t target;
    uint32_t val = 0;
    MemTxResult result;

    full_addr = (uint64_t)(s->upper_addr & 0xFC000000) |
                ((uint64_t)s->address << 2);
    target = (uint64_t)s->selected_slave * R100_CHIPLET_OFFSET + full_addr;

    result = address_space_read(&address_space_memory, target,
                                MEMTXATTRS_UNSPECIFIED,
                                &val, sizeof(val));

    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-qspi: remote read failed at 0x%"PRIx64
                      " (slave %u)\n", target, s->selected_slave);
        return 0;
    }

    return val;
}

/*
 * Perform a cross-chiplet write via QEMU's system address space.
 */
static void qspi_remote_write(R100QSPIBridgeState *s, uint32_t data)
{
    uint64_t full_addr;
    uint64_t target;
    MemTxResult result;

    full_addr = (uint64_t)(s->upper_addr & 0xFC000000) |
                ((uint64_t)s->address << 2);
    target = (uint64_t)s->selected_slave * R100_CHIPLET_OFFSET + full_addr;

    result = address_space_write(&address_space_memory, target,
                                 MEMTXATTRS_UNSPECIFIED,
                                 &data, sizeof(data));

    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "r100-qspi: remote write failed at 0x%"PRIx64
                      " (slave %u)\n", target, s->selected_slave);
    }
}

static uint64_t r100_qspi_read(void *opaque, hwaddr addr, unsigned size)
{
    R100QSPIBridgeState *s = R100_QSPI_BRIDGE(opaque);

    switch (addr) {
    case DW_SSI_SR:
        /*
         * Status register: never busy, TX FIFO always empty/not-full,
         * RX FIFO not-empty if we have read data.
         */
        return SR_TFNF | SR_TFE | (s->rx_data_valid ? SR_RFNE : 0);

    case DW_SSI_RXFLR:
        /* RX FIFO level: 1 if data is available, 0 otherwise */
        return s->rx_data_valid ? 1 : 0;

    case DW_SSI_TXFLR:
        /* TX FIFO level: always 0 (empty, writes are instant) */
        return 0;

    case DW_SSI_DRX_BASE:
        /* Reading DRx returns the result of the last remote read */
        if (s->rx_data_valid) {
            s->rx_data_valid = false;
            return s->rx_data;
        }
        return 0;

    default:
        if ((addr >> 2) < QSPI_REG_COUNT) {
            return s->regs[addr >> 2];
        }
        return 0;
    }
}

static void r100_qspi_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    R100QSPIBridgeState *s = R100_QSPI_BRIDGE(opaque);

    switch (addr) {
    case DW_SSI_SER:
        /*
         * Slave Enable Register: bit N selects slave N.
         * slave_num in FW is 1-based (1=chiplet1, 2=chiplet2, 3=chiplet3),
         * mapped to SER bits: bit0=slave1, bit1=slave2, bit2=slave3.
         */
        if (val & 0x1) {
            s->selected_slave = 1;
        } else if (val & 0x2) {
            s->selected_slave = 2;
        } else if (val & 0x4) {
            s->selected_slave = 3;
        } else {
            s->selected_slave = 0;  /* deselected */
        }
        s->regs[addr >> 2] = (uint32_t)val;
        break;

    case DW_SSI_DRX_BASE:
    case DW_SSI_DRX_BASE + 4:
    case DW_SSI_DRX_BASE + 8:
        /*
         * Data register writes form the QSPI command:
         *   DRX[0] = instruction byte (0x70=read, 0x80=write)
         *   DRX[1] = 24-bit address (word-aligned, already >> 2)
         *   DRX[2] = data (for writes only)
         *
         * Process the command when all parts are received.
         */
        switch (s->xfer_state) {
        case QSPI_STATE_IDLE:
            s->instruction = (uint32_t)val;
            s->xfer_state = QSPI_STATE_GOT_INSTRUCTION;
            break;

        case QSPI_STATE_GOT_INSTRUCTION:
            s->address = (uint32_t)val & 0x00FFFFFF;
            if (s->instruction == QSPI_INST_READ_24WAIT) {
                /* Read: execute immediately, put result in RX FIFO */
                s->rx_data = qspi_remote_read(s);
                s->rx_data_valid = true;
                s->xfer_state = QSPI_STATE_IDLE;
            } else if (s->instruction == QSPI_INST_WRITE) {
                /* Write: need one more DRX write for the data */
                s->xfer_state = QSPI_STATE_GOT_ADDRESS;
            } else {
                /* Unknown instruction, reset */
                qemu_log_mask(LOG_GUEST_ERROR,
                              "r100-qspi: unknown instruction 0x%x\n",
                              s->instruction);
                s->xfer_state = QSPI_STATE_IDLE;
            }
            break;

        case QSPI_STATE_GOT_ADDRESS:
            /* Write data word to remote chiplet */
            qspi_remote_write(s, (uint32_t)val);
            s->xfer_state = QSPI_STATE_IDLE;
            break;
        }
        break;

    case DW_SSI_SSIENR:
        /* SSI enable/disable — reset transfer state on disable */
        if (!(val & 1)) {
            s->xfer_state = QSPI_STATE_IDLE;
        }
        s->regs[addr >> 2] = (uint32_t)val;
        break;

    default:
        /* Store for readback */
        if ((addr >> 2) < QSPI_REG_COUNT) {
            s->regs[addr >> 2] = (uint32_t)val;
        }
        break;
    }
}

static const MemoryRegionOps r100_qspi_ops = {
    .read = r100_qspi_read,
    .write = r100_qspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void r100_qspi_realize(DeviceState *dev, Error **errp)
{
    R100QSPIBridgeState *s = R100_QSPI_BRIDGE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_qspi_ops, s,
                          "r100-qspi-bridge", QSPI_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_qspi_reset(DeviceState *dev)
{
    R100QSPIBridgeState *s = R100_QSPI_BRIDGE(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->xfer_state = QSPI_STATE_IDLE;
    s->instruction = 0;
    s->address = 0;
    s->upper_addr = 0;
    s->rx_data = 0;
    s->rx_data_valid = false;
    s->selected_slave = 0;
}

static Property r100_qspi_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100QSPIBridgeState, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_qspi_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_qspi_realize;
    device_class_set_legacy_reset(dc, r100_qspi_reset);
    device_class_set_props(dc, r100_qspi_properties);
}

static const TypeInfo r100_qspi_info = {
    .name = TYPE_R100_QSPI_BRIDGE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100QSPIBridgeState),
    .class_init = r100_qspi_class_init,
};

static void r100_qspi_register_types(void)
{
    type_register_static(&r100_qspi_info);
}

type_init(r100_qspi_register_types)
