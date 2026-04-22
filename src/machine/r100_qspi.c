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
 *   - Instruction 0x83: Write 16 consecutive words from a starting address
 *
 * Since all chiplets share QEMU's system address space, cross-chiplet
 * access is implemented as direct memory reads/writes at the appropriate
 * chiplet offset.
 *
 * The 16-word write is used by qspi_bridge_load_image() in BL1 to stage
 * tboot_n into each secondary chiplet's local iRAM before releasing its
 * CP0.cpu0 from reset. Without it, the secondary cores would resume at
 * an RVBAR whose backing iRAM is empty.
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
#define QSPI_INST_READ_24WAIT           0x70
#define QSPI_INST_WRITE                 0x80
#define QSPI_INST_WRITE_16WORD          0x83
#define QSPI_INST_WRITE_STATUS_2WAIT    0xD0

/*
 * WRITESTATUS response byte. The FW driver (qspi_bridge_wait_write_complete)
 * expects bit[7] READY (0x80) after every 1-word or 16-word write completes
 * on the AHB side of the remote slave. bit[6] ERROR (0x40) signals an AHB
 * error. Since our write path is synchronous (qspi_remote_write runs
 * inline), every write has effectively already completed by the time the FW
 * polls, so we always return READY.
 */
#define QSPI_WRITE_STATUS_READY         0x80

#define QSPI_BURST16_WORDS      16

/*
 * The FW driver's 24-bit address field loses bits [31:26] of the target
 * address. Those 6 bits come from an "upper-address" register on the slave
 * SPI controller, written via qspi_bridge_set_upper_addr() which maps to
 *
 *   DW_SPI_SYSREG_ADDRESS = 0x1E04310160  (qspi_bridge.h)
 *
 * encoded as 24-bit address field 0x0C4058 ((addr >> 2) & 0xFFFFFF). When
 * the master stub sees a write with this address field, it latches the
 * data into s->upper_addr instead of forwarding it to sysmem. Subsequent
 * remote read/write instructions reconstruct a 32-bit low address as
 *   (upper_addr & 0xFC000000) | (address << 2)
 * and the low 28 bits are used as an offset inside the target chiplet's
 * PRIVATE_BASE window. Without this, cross-chiplet accesses above the
 * 64 MB 26-bit word window (RBC at 0x1E05..., etc.) land on unrelated
 * offsets — the symptom was UCIe linkup timing out on chiplet 1.
 */
#define QSPI_UPPER_ADDR_FIELD   0x0C4058u

/*
 * State machine for tracking multi-word DRX writes.
 *
 * The bridge driver pushes a 3-word (1-word write), 2-word (read) or
 * 18-word (16-word write) sequence into DRX[0..N-1]:
 *
 *   DRX[0] = instruction byte
 *   DRX[1] = 24-bit word-aligned address
 *   DRX[2..N-1] = data words (for writes only)
 *
 * For reads the result is placed in the RX FIFO; a subsequent DRX read
 * consumes it. For writes we forward each data word in order.
 */
typedef enum {
    QSPI_STATE_IDLE,
    QSPI_STATE_GOT_INSTRUCTION,
    QSPI_STATE_WRITING,   /* expecting `data_words_remaining` more data words */
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
    uint32_t address;               /* 24-bit address from command */
    uint32_t write_offset;          /* # of data words already forwarded */
    uint32_t data_words_remaining;  /* data words pending for current burst */
    uint32_t upper_addr;            /* Upper address bits (set via set_upper_addr) */
    uint32_t rx_data;               /* Data read from remote chiplet */
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
 * Compose a target sysmem address from the current transfer state.
 *
 * The slave-side controller reconstructs a 32-bit word-address
 *     full_addr = (upper_addr & 0xFC000000) | (address << 2)
 * then interprets the bottom 28 bits as an offset inside its own 256 MB
 * private-alias window (PRIVATE_BASE, 0x1E00000000). The top 4 bits of
 * `full_addr` are a leftover of the FW driver's uint32_t cast of a
 * 0x1E...-based absolute address and are discarded. Without the
 * `& 0x0FFFFFFF` mask, reads past the 64 MB 26-bit direct-address window
 * (RBC blocks at 0x1E05xx, etc.) miss the device alias entirely.
 */
static uint64_t qspi_compose_target(R100QSPIBridgeState *s,
                                    uint32_t word_offset)
{
    uint64_t full_addr = (uint64_t)(s->upper_addr & 0xFC000000) |
                         ((uint64_t)s->address << 2);

    full_addr &= 0x0FFFFFFFULL;
    full_addr += (uint64_t)word_offset << 2;

    uint64_t target = (uint64_t)s->selected_slave * R100_CHIPLET_OFFSET +
                      R100_PRIVATE_BASE + full_addr;

    qemu_log_mask(LOG_UNIMP,
                  "r100-qspi: compose upper=0x%08x addr24=0x%06x off=%u "
                  "-> full=0x%"PRIx64" slave=%u target=0x%"PRIx64"\n",
                  s->upper_addr, s->address, word_offset,
                  full_addr, s->selected_slave, target);

    return target;
}

/*
 * Perform a cross-chiplet read via QEMU's system address space.
 */
static uint32_t qspi_remote_read(R100QSPIBridgeState *s)
{
    uint64_t target = qspi_compose_target(s, 0);
    uint32_t val = 0;
    MemTxResult result;

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
 *
 * `word_offset` is measured in 4-byte units past the instruction's base
 * address. For a 1-word write this is always 0; for a 16-word burst it
 * walks 0..15 as successive data words arrive on DRX.
 */
static void qspi_remote_write(R100QSPIBridgeState *s, uint32_t word_offset,
                              uint32_t data)
{
    uint64_t target = qspi_compose_target(s, word_offset);
    MemTxResult result;

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

    if (addr == DW_SSI_SER) {
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
        return;
    }

    if (addr == DW_SSI_SSIENR) {
        /* SSI enable/disable — reset transfer state on disable */
        if (!(val & 1)) {
            s->xfer_state = QSPI_STATE_IDLE;
        }
        s->regs[addr >> 2] = (uint32_t)val;
        return;
    }

    /*
     * The data register (DRX) lives at DW_SSI_DRX_BASE (0x60) and the FW
     * driver writes DRX[0..17] via `regs->drx[i]`. In the real DWC_ssi
     * all DRX slots share a single TX FIFO push address; but the FW lays
     * out `struct dw_ssi_regs` as a contiguous uint32_t[36] array starting
     * at 0x60, so writes actually hit DW_SSI_DRX_BASE + i*4. Treat any
     * access in [0x60, 0xF0) as a FIFO push.
     */
    if (addr >= DW_SSI_DRX_BASE && addr < DW_SSI_DRX_BASE + 36 * 4) {
        switch (s->xfer_state) {
        case QSPI_STATE_IDLE:
            s->instruction = (uint32_t)val;
            if (s->instruction == QSPI_INST_WRITE_STATUS_2WAIT) {
                /*
                 * WRITESTATUS2 is address-less: the FW pushes only the
                 * instruction byte and then polls RXFLR for a single 8-bit
                 * status response. Because our qspi_remote_write path is
                 * synchronous, the remote AHB write has already landed by
                 * the time the FW reads this back, so return READY.
                 */
                s->rx_data = QSPI_WRITE_STATUS_READY;
                s->rx_data_valid = true;
                s->xfer_state = QSPI_STATE_IDLE;
            } else {
                s->xfer_state = QSPI_STATE_GOT_INSTRUCTION;
            }
            break;

        case QSPI_STATE_GOT_INSTRUCTION:
            s->address = (uint32_t)val & 0x00FFFFFF;
            s->write_offset = 0;
            if (s->instruction == QSPI_INST_READ_24WAIT) {
                s->rx_data = qspi_remote_read(s);
                s->rx_data_valid = true;
                s->xfer_state = QSPI_STATE_IDLE;
            } else if (s->instruction == QSPI_INST_WRITE) {
                s->data_words_remaining = 1;
                s->xfer_state = QSPI_STATE_WRITING;
            } else if (s->instruction == QSPI_INST_WRITE_16WORD) {
                /*
                 * Bulk image staging always uses upper_addr=0 because BL1
                 * only targets iRAM (0x1E00010000..0x1E00050000) which fits
                 * in the 26-bit word window. Leave the upper latch alone.
                 */
                s->data_words_remaining = QSPI_BURST16_WORDS;
                s->xfer_state = QSPI_STATE_WRITING;
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "r100-qspi: unknown instruction 0x%x\n",
                              s->instruction);
                s->xfer_state = QSPI_STATE_IDLE;
            }
            break;

        case QSPI_STATE_WRITING:
            if (s->instruction == QSPI_INST_WRITE &&
                s->address == QSPI_UPPER_ADDR_FIELD) {
                /*
                 * qspi_bridge_set_upper_addr(): the FW writes bits [31:26]
                 * of the next target address to the slave's internal
                 * DW_SPI_SYSREG_ADDRESS register. Model it by latching
                 * s->upper_addr so subsequent remote read/write instructions
                 * can reconstruct addresses that exceed the 26-bit field.
                 */
                s->upper_addr = (uint32_t)val;
                qemu_log_mask(LOG_UNIMP,
                              "r100-qspi: latch upper_addr=0x%08x (slave %u)\n",
                              s->upper_addr, s->selected_slave);
            } else {
                qspi_remote_write(s, s->write_offset, (uint32_t)val);
            }
            s->write_offset++;
            if (--s->data_words_remaining == 0) {
                s->xfer_state = QSPI_STATE_IDLE;
            }
            break;
        }
        return;
    }

    /* Everything else: store for readback. */
    if ((addr >> 2) < QSPI_REG_COUNT) {
        s->regs[addr >> 2] = (uint32_t)val;
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
    s->write_offset = 0;
    s->data_words_remaining = 0;
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
