/*
 * R100 Samsung IPM mailbox peripheral (Phase 2, M7 prep).
 *
 * Models one instance of the Samsung mailbox SFR block that sits on the
 * config-space path between two CPUs. The real silicon has 20+ copies
 * of this same register layout scattered around the SoC (ROT_M0..M2,
 * CP{0,1}_M3/M4, PERI0/1_M5..M14, PCIE_PF/VF0..15). All of them share
 * the exact register layout documented in
 * external/ssw-bundle/.../drivers/mailbox/ipm_samsung.h:
 *
 *   0x00   MCUCTRL                (bit 0 = mswrst)
 *   0x08   INTGR0                 W1S pending.0 (generate to CPU0-side)
 *   0x0C   INTCR0                 W1C pending.0
 *   0x10   INTMR0                 mask for group 0
 *   0x14   INTSR0                 raw pending.0                (R)
 *   0x18   INTMSR0                pending.0 & ~INTMR0          (R)
 *   0x1C   INTGR1                 W1S pending.1 (generate to CPU1-side)
 *   0x20   INTCR1                 W1C pending.1
 *   0x24   INTMR1                 mask for group 1
 *   0x28   INTSR1                 raw pending.1                (R)
 *   0x2C   INTMSR1                pending.1 & ~INTMR1          (R)
 *   0x6C   MIF_INIT               bit 2 = mif_init (unused here; RW)
 *   0x70   IS_VERSION             RW scratch
 *   0x80   ISSR0..ISSR63          64 x u32 payload scratch regs
 *
 * ipm_samsung_send() writes BIT(channel) into INTGR{0,1}. ipm_samsung_isr()
 * finds the MSB set in INTMSR{0,1} and then clears it via INTCR. Our model
 * keeps a single 32-bit `pending[2]` shadow for both INTSR and INTGR reads
 * (they are the same storage on silicon) and derives INTMSR lazily.
 *
 * Two qemu_irq outputs (`irq[0]`, `irq[1]`) follow INTMSR0/INTMSR1
 * respectively: level-high whenever the corresponding INTMSR has any
 * pending bit unmasked. A pending bit is cleared either by an INTCR
 * write (the normal ISR path) or a MCUCTRL mswrst.
 *
 * For Phase 2/M7 only chiplet 0's PCIE mailbox is actually wired to a
 * GIC (see r100_soc.c). The device is otherwise untargeted from
 * MMIO — FW accesses it through its cfg-space alias via the sysmem
 * mapping installed by the machine.
 *
 * The M6 doorbell path (host BAR4 MAILBOX_INTGR writes forwarded over
 * a chardev) used to pulse a placeholder SPI directly; with this
 * device in place, src/machine/r100_doorbell.c calls
 * r100_mailbox_raise_intgr() instead and the SPI is asserted as the
 * natural consequence of the mailbox's INTMSR going non-zero. That
 * matches silicon: on the real chip, a host PCIe write to
 * MAILBOX_PCIE_PRIVATE + INTGR1 sets pending bits in the same SFR
 * the ISR then reads from.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "r100_soc.h"

/* Register offsets — match struct ipm_samsung in
 * external/ssw-bundle/.../drivers/mailbox/ipm_samsung.h. */
#define R100_MBX_MCUCTRL        0x00
#define R100_MBX_INTGR0         0x08
#define R100_MBX_INTCR0         0x0C
#define R100_MBX_INTMR0         0x10
#define R100_MBX_INTSR0         0x14
#define R100_MBX_INTMSR0        0x18
#define R100_MBX_INTGR1         0x1C
#define R100_MBX_INTCR1         0x20
#define R100_MBX_INTMR1         0x24
#define R100_MBX_INTSR1         0x28
#define R100_MBX_INTMSR1        0x2C
#define R100_MBX_MIF_INIT       0x6C
#define R100_MBX_IS_VERSION     0x70
#define R100_MBX_ISSR0          0x80

#define R100_MBX_MCUCTRL_MSWRST BIT(0)

static inline uint32_t r100_mailbox_masked(const R100MailboxState *s, int g)
{
    return s->pending[g] & ~s->intmr[g];
}

static void r100_mailbox_update_irq(R100MailboxState *s, int g)
{
    int level = r100_mailbox_masked(s, g) != 0;
    qemu_set_irq(s->irq[g], level);
}

void r100_mailbox_raise_intgr(R100MailboxState *s, int group, uint32_t val)
{
    if (!s || (group != 0 && group != 1)) {
        return;
    }
    s->pending[group] |= val;
    s->intgr_writes[group]++;
    r100_mailbox_update_irq(s, group);
}

static uint64_t r100_mailbox_read(void *opaque, hwaddr addr, unsigned size)
{
    R100MailboxState *s = R100_MAILBOX(opaque);

    switch (addr) {
    case R100_MBX_MCUCTRL:
        return s->mcuctrl;
    /* INTGR reads return the current pending state on silicon (same
     * storage as INTSR). Not used by the FW driver but exposed for
     * test introspection. */
    case R100_MBX_INTGR0:
    case R100_MBX_INTSR0:
        return s->pending[0];
    case R100_MBX_INTGR1:
    case R100_MBX_INTSR1:
        return s->pending[1];
    case R100_MBX_INTCR0:
    case R100_MBX_INTCR1:
        return 0; /* write-only */
    case R100_MBX_INTMR0:
        return s->intmr[0];
    case R100_MBX_INTMR1:
        return s->intmr[1];
    case R100_MBX_INTMSR0:
        return r100_mailbox_masked(s, 0);
    case R100_MBX_INTMSR1:
        return r100_mailbox_masked(s, 1);
    case R100_MBX_MIF_INIT:
        return s->mif_init;
    case R100_MBX_IS_VERSION:
        return s->is_version;
    default:
        if (addr >= R100_MBX_ISSR0 &&
            addr < R100_MBX_ISSR0 + R100_MBX_ISSR_COUNT * 4) {
            return s->issr[(addr - R100_MBX_ISSR0) >> 2];
        }
        qemu_log_mask(LOG_UNIMP,
                      "r100-mailbox[%s]: unimplemented read @ 0x%" HWADDR_PRIx "\n",
                      s->name ? s->name : "?", addr);
        return 0;
    }
}

static void r100_mailbox_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    R100MailboxState *s = R100_MAILBOX(opaque);
    uint32_t v = (uint32_t)val;

    switch (addr) {
    case R100_MBX_MCUCTRL:
        s->mcuctrl = v & R100_MBX_MCUCTRL_MSWRST;
        if (v & R100_MBX_MCUCTRL_MSWRST) {
            /* MSWRST: reset all pending and unmask everything. Docs
             * describe this as a soft-reset of the SFR block. */
            s->pending[0] = s->pending[1] = 0;
            s->intmr[0] = s->intmr[1] = 0;
            r100_mailbox_update_irq(s, 0);
            r100_mailbox_update_irq(s, 1);
        }
        break;
    case R100_MBX_INTGR0:
        s->pending[0] |= v;
        s->intgr_writes[0]++;
        r100_mailbox_update_irq(s, 0);
        break;
    case R100_MBX_INTGR1:
        s->pending[1] |= v;
        s->intgr_writes[1]++;
        r100_mailbox_update_irq(s, 1);
        break;
    case R100_MBX_INTCR0:
        s->pending[0] &= ~v;
        r100_mailbox_update_irq(s, 0);
        break;
    case R100_MBX_INTCR1:
        s->pending[1] &= ~v;
        r100_mailbox_update_irq(s, 1);
        break;
    case R100_MBX_INTMR0:
        s->intmr[0] = v;
        r100_mailbox_update_irq(s, 0);
        break;
    case R100_MBX_INTMR1:
        s->intmr[1] = v;
        r100_mailbox_update_irq(s, 1);
        break;
    case R100_MBX_INTSR0:
    case R100_MBX_INTSR1:
    case R100_MBX_INTMSR0:
    case R100_MBX_INTMSR1:
        /* Read-only status mirrors; writes are ignored on silicon. */
        break;
    case R100_MBX_MIF_INIT:
        s->mif_init = v;
        break;
    case R100_MBX_IS_VERSION:
        s->is_version = v;
        break;
    default:
        if (addr >= R100_MBX_ISSR0 &&
            addr < R100_MBX_ISSR0 + R100_MBX_ISSR_COUNT * 4) {
            s->issr[(addr - R100_MBX_ISSR0) >> 2] = v;
            return;
        }
        qemu_log_mask(LOG_UNIMP,
                      "r100-mailbox[%s]: unimplemented write @ 0x%" HWADDR_PRIx
                      " = 0x%x\n", s->name ? s->name : "?", addr, v);
        break;
    }
}

static const MemoryRegionOps r100_mailbox_ops = {
    .read = r100_mailbox_read,
    .write = r100_mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void r100_mailbox_realize(DeviceState *dev, Error **errp)
{
    R100MailboxState *s = R100_MAILBOX(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char region_name[64];

    snprintf(region_name, sizeof(region_name),
             "r100-mailbox.%s", s->name ? s->name : "anon");
    memory_region_init_io(&s->iomem, OBJECT(dev), &r100_mailbox_ops, s,
                          region_name, R100_MBX_SFR_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq[0]);
    sysbus_init_irq(sbd, &s->irq[1]);
}

static void r100_mailbox_reset(DeviceState *dev)
{
    R100MailboxState *s = R100_MAILBOX(dev);

    s->mcuctrl = 0;
    s->pending[0] = s->pending[1] = 0;
    s->intmr[0] = s->intmr[1] = 0;
    s->mif_init = 0;
    s->is_version = 0;
    memset(s->issr, 0, sizeof(s->issr));
    /* intgr_writes is observability only; keep it across reset so
     * tests can inspect cumulative activity. */
    r100_mailbox_update_irq(s, 0);
    r100_mailbox_update_irq(s, 1);
}

static const VMStateDescription r100_mailbox_vmstate = {
    .name = "r100-mailbox",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(mcuctrl, R100MailboxState),
        VMSTATE_UINT32_ARRAY(pending, R100MailboxState, 2),
        VMSTATE_UINT32_ARRAY(intmr, R100MailboxState, 2),
        VMSTATE_UINT32(mif_init, R100MailboxState),
        VMSTATE_UINT32(is_version, R100MailboxState),
        VMSTATE_UINT32_ARRAY(issr, R100MailboxState, R100_MBX_ISSR_COUNT),
        VMSTATE_UINT64_ARRAY(intgr_writes, R100MailboxState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static Property r100_mailbox_properties[] = {
    DEFINE_PROP_STRING("name", R100MailboxState, name),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_mailbox_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "R100 Samsung IPM mailbox SFR block";
    dc->realize = r100_mailbox_realize;
    dc->vmsd = &r100_mailbox_vmstate;
    device_class_set_legacy_reset(dc, r100_mailbox_reset);
    device_class_set_props(dc, r100_mailbox_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    /* Instantiation is the machine's job — SPI wiring is topology. */
    dc->user_creatable = false;
}

static const TypeInfo r100_mailbox_info = {
    .name          = TYPE_R100_MAILBOX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100MailboxState),
    .class_init    = r100_mailbox_class_init,
};

static void r100_mailbox_register_types(void)
{
    type_register_static(&r100_mailbox_info);
}

type_init(r100_mailbox_register_types)
