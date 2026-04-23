/*
 * REMU - R100 NPU System Emulator
 * SMMU-600 TCU stub device
 *
 * Minimal stub for the Arm SMMU-600 register block at TCU_OFFSET
 * (0x1FF4200000, per external/.../rebel_h_baseoffset.h). Covers two
 * firmware paths:
 *
 *   1. BL2's smmu_early_init() (EL3, one-shot at boot):
 *      - programs EVENTQ base, STRTAB base, GBPA fence, then enables
 *        event queues via CR0 and spins on:
 *          a) `while (!(cr0ack & EVENTQEN))` — we mirror writes to
 *             SMMU_CR0 into SMMU_CR0ACK (masked to EN bits).
 *          b) `while (gbpa & UPDATE)` — we always clear the UPDATE
 *             bit on GBPA writes.
 *
 *   2. FreeRTOS's EL1 SMMU driver (drivers/smmu/smmu.c):
 *      - smmu_cmdq_enqueue_cmd() writes a command to the next slot in
 *        the in-DRAM CMDQ (base = SMMU_CMDQ_BASE PA, size = 1<<log2),
 *        increments its local PROD, writes SMMU_CMDQ_PROD, and polls
 *        SMMU_CMDQ_CONS for space.
 *      - smmu_sync() posts a CMD_SYNC with CS=SIG_IRQ and msiaddr set
 *        back to the sync slot itself, then spins on the first u32 of
 *        that slot turning 0 (the SMMU's MSI write data, reset value 0,
 *        overwrites cmd[0] on completion).
 *     Without CMDQ processing the poll times out 1000 times a second
 *     and the HILS log fills with `[smmu] Failed to sync`. We hook
 *     the PROD write: walk from old-CONS to new-PROD, write a u32 0
 *     to the MSI address of every CMD_SYNC entry, and advance CONS to
 *     match PROD so `smmu_q_has_space` immediately reports space.
 *
 * Everything else is plain read/write-back. The STE / CD / event queue
 * structures themselves live in DRAM (SMMU_STE_BASE_ADDR = 0x14000000
 * etc., see platform_def.h) and are already covered by the chiplet
 * DRAM backing — no MMIO routing needed for them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "exec/cpu-common.h"
#include "r100_soc.h"

#define R100_SMMU_REG_SIZE      0x10000
#define R100_SMMU_REG_COUNT     (R100_SMMU_REG_SIZE / 4)

struct R100SMMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[R100_SMMU_REG_COUNT];
    uint32_t chiplet_id;

    /* Cached CMDQ geometry from the last SMMU_CMDQ_BASE write. */
    uint64_t cmdq_base_pa;      /* PA of first entry in guest memory */
    uint32_t cmdq_log2size;     /* log2(#entries); valid range 0..19 */
};

typedef struct R100SMMUState R100SMMUState;

DECLARE_INSTANCE_CHECKER(R100SMMUState, R100_SMMU, TYPE_R100_SMMU)

/* SMMU-600 register offsets used by TF-A (see struct smmu600_regs). */
#define SMMU_CR0            0x20
#define SMMU_CR0ACK         0x24
#define SMMU_GBPA           0x44
#define SMMU_IRQ_CTRL       0x50
#define SMMU_IRQ_CTRLACK    0x54
#define SMMU_CMDQ_BASE      0x90    /* 64-bit */
#define SMMU_CMDQ_BASE_HI   0x94    /* upper 32 bits of SMMU_CMDQ_BASE */
#define SMMU_CMDQ_PROD      0x98
#define SMMU_CMDQ_CONS      0x9C

/* Bit masks matched against firmware-side SMMU_{EVENTQEN,SMMUEN,CMDQEN}. */
#define SMMU_CR0_SMMUEN     (1u << 0)
#define SMMU_CR0_EVENTQEN   (1u << 2)
#define SMMU_CR0_CMDQEN     (1u << 3)
#define SMMU_CR0_ACK_MASK   (SMMU_CR0_SMMUEN | SMMU_CR0_EVENTQEN | \
                             SMMU_CR0_CMDQEN)

#define SMMU_GBPA_UPDATE    (1u << 31)

/* SMMU_CMDQ_BASE packs PA in bits[51:5] and log2size in bits[4:0]. */
#define SMMU_Q_BASE_ADDR_MASK   (0x7fffffffffffULL << 5)
#define SMMU_Q_LOG2SIZE_MASK    0x1fULL
#define SMMU_Q_LOG2SIZE_CAP     19  /* >1M entries is absurd; bail out */

/* CMDQ entry is 2 dwords (16 bytes) per the SMMU-600 spec + FW. */
#define SMMU_CMDQ_ENTRY_BYTES   16u

/* Command opcode lives in cmd[0] bits[7:0]. */
#define SMMU_CMD_OPCODE_MASK    0xFFULL
#define SMMU_CMD_SYNC           0x46ULL

/* CMD_SYNC fields (see SYNC_0_* / SYNC_1_* in drivers/smmu/smmu.h). */
#define SMMU_SYNC_CS_MASK       (0x3ULL << 12)
#define SMMU_SYNC_CS_SIG_IRQ    (0x1ULL << 12)
#define SMMU_SYNC_MSIADDR_MASK  (0x3ffffffffffffULL << 2)

static void r100_smmu_update_cmdq_base(R100SMMUState *s)
{
    uint64_t lo = s->regs[SMMU_CMDQ_BASE >> 2];
    uint64_t hi = s->regs[SMMU_CMDQ_BASE_HI >> 2];
    uint64_t val = lo | (hi << 32);

    s->cmdq_base_pa = val & SMMU_Q_BASE_ADDR_MASK;
    s->cmdq_log2size = val & SMMU_Q_LOG2SIZE_MASK;
}

/*
 * Walk CMDQ entries in (old_cons, new_prod] and satisfy every CMD_SYNC
 * by writing the (default-zero) MSI data to the msiaddr encoded in
 * cmd[1]. Non-SYNC entries are no-ops for the stub: the FW only polls
 * CONS to reclaim space, and we'll rewrite that below.
 *
 * `cons` / `prod` are the composite PROD/CONS registers: bits
 * [log2size-1:0] are the index, bit [log2size] is the wrap flag, and
 * higher bits are reserved (OVF in the event queue case). Equality of
 * both fields means the queue is empty; inequality in the wrap bit
 * alone means the queue is full. We iterate one entry at a time and
 * stop when cons == prod.
 */
static void r100_smmu_process_cmdq(R100SMMUState *s, uint32_t old_cons,
                                   uint32_t new_prod)
{
    uint32_t log2size = s->cmdq_log2size;
    uint32_t idx_mask;
    uint32_t wrp_mask;
    uint32_t cons = old_cons;

    if (s->cmdq_base_pa == 0 || log2size == 0 ||
        log2size > SMMU_Q_LOG2SIZE_CAP) {
        /* Base not yet programmed (or insane) — nothing to do. */
        return;
    }

    idx_mask = (1u << log2size) - 1;
    wrp_mask = 1u << log2size;

    while ((cons & (idx_mask | wrp_mask)) !=
           (new_prod & (idx_mask | wrp_mask))) {
        uint32_t idx = cons & idx_mask;
        hwaddr entry_pa = s->cmdq_base_pa +
                          (hwaddr)idx * SMMU_CMDQ_ENTRY_BYTES;
        uint64_t cmd[2];

        cpu_physical_memory_read(entry_pa, cmd, sizeof(cmd));

        if ((cmd[0] & SMMU_CMD_OPCODE_MASK) == SMMU_CMD_SYNC &&
            (cmd[0] & SMMU_SYNC_CS_MASK) == SMMU_SYNC_CS_SIG_IRQ) {
            hwaddr msi_pa = cmd[1] & SMMU_SYNC_MSIADDR_MASK;
            uint32_t zero = 0;

            cpu_physical_memory_write(msi_pa, &zero, sizeof(zero));
        }

        /* Advance CONS by one, preserving the wrap bit on overflow.
         * Mirrors smmu_q_inc_cons() in the firmware. */
        idx = (cons & idx_mask) + 1;
        if (idx & wrp_mask) {
            cons = (cons & ~idx_mask) ^ wrp_mask;
        } else {
            cons = (cons & ~idx_mask) | idx;
        }
    }

    /* Publish the new CONS. Error field (bits[30:24]) stays 0. */
    s->regs[SMMU_CMDQ_CONS >> 2] = cons;
}

static uint64_t r100_smmu_read(void *opaque, hwaddr addr, unsigned size)
{
    R100SMMUState *s = R100_SMMU(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_SMMU_REG_COUNT) {
        return 0;
    }

    switch (addr) {
    case SMMU_CR0ACK:
        return s->regs[SMMU_CR0 >> 2] & SMMU_CR0_ACK_MASK;
    case SMMU_IRQ_CTRLACK:
        return s->regs[SMMU_IRQ_CTRL >> 2];
    case SMMU_CMDQ_BASE:
        if (size == 8) {
            return (uint64_t)s->regs[SMMU_CMDQ_BASE >> 2] |
                   ((uint64_t)s->regs[SMMU_CMDQ_BASE_HI >> 2] << 32);
        }
        return s->regs[reg_idx];
    default:
        return s->regs[reg_idx];
    }
}

static void r100_smmu_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    R100SMMUState *s = R100_SMMU(opaque);
    uint32_t reg_idx = addr >> 2;

    if (reg_idx >= R100_SMMU_REG_COUNT) {
        return;
    }

    switch (addr) {
    case SMMU_GBPA:
        /*
         * Firmware sets GBPA_UPDATE|SHCFG and then polls GBPA_UPDATE to
         * clear. Latch the UPDATE bit as always-clear so the poll exits
         * immediately.
         */
        s->regs[reg_idx] = (uint32_t)val & ~SMMU_GBPA_UPDATE;
        break;
    case SMMU_CMDQ_BASE:
        /*
         * FW uses sys_write64() so QEMU will normally deliver size=8;
         * cover size=4 too (upper half is then written separately at
         * SMMU_CMDQ_BASE_HI).
         */
        if (size == 8) {
            s->regs[SMMU_CMDQ_BASE >> 2] = (uint32_t)val;
            s->regs[SMMU_CMDQ_BASE_HI >> 2] = (uint32_t)(val >> 32);
        } else {
            s->regs[reg_idx] = (uint32_t)val;
        }
        r100_smmu_update_cmdq_base(s);
        break;
    case SMMU_CMDQ_BASE_HI:
        s->regs[reg_idx] = (uint32_t)val;
        r100_smmu_update_cmdq_base(s);
        break;
    case SMMU_CMDQ_PROD: {
        uint32_t old_cons = s->regs[SMMU_CMDQ_CONS >> 2];
        uint32_t new_prod = (uint32_t)val;

        s->regs[SMMU_CMDQ_PROD >> 2] = new_prod;
        r100_smmu_process_cmdq(s, old_cons, new_prod);
        break;
    }
    default:
        s->regs[reg_idx] = (uint32_t)val;
        break;
    }
}

static const MemoryRegionOps r100_smmu_ops = {
    .read = r100_smmu_read,
    .write = r100_smmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
};

static void r100_smmu_realize(DeviceState *dev, Error **errp)
{
    R100SMMUState *s = R100_SMMU(dev);
    char name[64];

    snprintf(name, sizeof(name), "r100-smmu.cl%u", s->chiplet_id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_smmu_ops, s, name,
                          R100_SMMU_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void r100_smmu_reset(DeviceState *dev)
{
    R100SMMUState *s = R100_SMMU(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->cmdq_base_pa = 0;
    s->cmdq_log2size = 0;
}

static Property r100_smmu_properties[] = {
    DEFINE_PROP_UINT32("chiplet-id", R100SMMUState, chiplet_id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_smmu_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_smmu_realize;
    device_class_set_legacy_reset(dc, r100_smmu_reset);
    device_class_set_props(dc, r100_smmu_properties);
}

static const TypeInfo r100_smmu_info = {
    .name = TYPE_R100_SMMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100SMMUState),
    .class_init = r100_smmu_class_init,
};

static void r100_smmu_register_types(void)
{
    type_register_static(&r100_smmu_info);
}

type_init(r100_smmu_register_types)
