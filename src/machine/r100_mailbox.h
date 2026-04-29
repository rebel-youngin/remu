/*
 * REMU - R100 NPU System Emulator
 * Samsung IPM mailbox — public helper API
 *
 * This header exposes only what other NPU-side devices need from
 * r100_mailbox.c: the QOM type name (so machine / doorbell / ... can
 * refer to it), an opaque typedef (so they can hold pointers in their
 * own state and DEFINE_PROP_LINK targets), and the three helpers the
 * bridge paths use to drive the mailbox from outside its own MMIO
 * region. The full R100MailboxState layout and R100_MAILBOX() cast
 * macro stay private to r100_mailbox.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef R100_MAILBOX_H
#define R100_MAILBOX_H

#include "qemu/osdep.h"
#include "qom/object.h"

#define TYPE_R100_MAILBOX       "r100-mailbox"

/*
 * FW_BOOT_DONE magic. q-sys CP0's bootdone_task writes this into
 * PF.ISSR[4] from `bootdone_notify_to_host` once cold-boot reaches
 * "all chiplets up" — see q/sys/osl/FreeRTOS/Source/services/
 * bootdone_service/bootdone_service.c:67. Used here by:
 *   - r100-mailbox: latch `fw_boot_done_seen` on the matching
 *     NPU_MMIO write to ISSR[4], so callers can tell whether
 *     q-sys's cold-boot main.c (incl. its DCS memset) has run.
 *   - r100-cm7:     synthesise the post-soft-reset re-handshake
 *     (write 0xFB0D back into PF.ISSR[4]) on INTGR0 bit 0.
 */
#define R100_FW_BOOT_DONE       0xFB0Du

typedef struct R100MailboxState R100MailboxState;

/*
 * Inject a pending-bit set from outside the MMIO path. Used by the
 * M6 doorbell bridge: a chardev frame from the x86 host's BAR4
 * MAILBOX_INTGR write arrives on the NPU side, and we want the same
 * effect as if the FW had issued a store to this mailbox's INTGR
 * register at offset 0x8 (group=0) / 0x1c (group=1). Asserts the
 * matching qemu_irq when INTMSR goes non-zero.
 */
void r100_mailbox_raise_intgr(R100MailboxState *s, int group, uint32_t val);

/*
 * Inject an ISSR register write from outside the MMIO path. Used by
 * the M8 extension to r100-cm7: frames arriving from the x86
 * host-side BAR4 writes into the MAILBOX_BASE payload range update
 * the backing scratch register but must NOT re-emit on the ISSR
 * egress chardev (that would loop the host's own write back at
 * itself). Out-of-range `idx` is a no-op.
 */
void r100_mailbox_set_issr(R100MailboxState *s, uint32_t idx, uint32_t val);

/*
 * CM7-stub egress write. Used by the REMU CM7-relay shortcut in
 * r100-cm7: on silicon, certain host doorbells (notably
 * SOFT_RESET) get handled by the PCIE_CM7 subcontroller's firmware,
 * which then writes scratch values like FW_BOOT_DONE (0xFB0D) into
 * PF.ISSR[4] via MMIO. REMU doesn't model CM7; this helper updates
 * the backing ISSR[idx] *and* emits the egress frame on the ISSR
 * chardev so the host BAR4 shadow converges the same way it would
 * if CM7 had done `sfr->issrN = val` directly. Out-of-range `idx`
 * is a no-op.
 */
void r100_mailbox_cm7_stub_write_issr(R100MailboxState *s, uint32_t idx,
                                      uint32_t val);

/*
 * Read the current ISSR[idx] value without going through the MMIO
 * path. Returns 0 for out-of-range `idx` — a safe no-op because
 * `pi == 0` means "nothing to consume". Strictly read-only so the
 * three-way source bookkeeping in r100_mailbox_issr_store() stays
 * authoritative.
 */
uint32_t r100_mailbox_get_issr(R100MailboxState *s, uint32_t idx);

/*
 * In-process multi-slot ISSR write — copies `count` u32 values into
 * ISSR[idx..idx+count). Skips the host-relay counter and the
 * egress-emit chardev — intended for NPU-internal mailboxes (q-cp
 * polls them directly, no host mirror). Out-of-range slots are
 * dropped silently.
 */
void r100_mailbox_set_issr_words(R100MailboxState *s, uint32_t idx,
                                 const uint32_t *vals, uint32_t count);

/*
 * Cold-boot publish observer. Returns true once an NPU-MMIO write of
 * R100_FW_BOOT_DONE has hit ISSR[4] on this mailbox (the canonical
 * q-sys `bootdone_task` handshake). One-shot latch: never clears
 * after the first observation, including across kmd-driven
 * `r100_mailbox_set_issr` clears of the same slot.
 *
 * Used by r100-cm7 to gate the post-soft-reset FW_BOOT_DONE
 * re-synthesis: kmd's RBLN_RESET_FIRST flow rings INTGR0 bit 0
 * before q-sys has reached its cold-boot `main.c:250` DCS memset, so
 * answering with a fake `0xFB0D` immediately makes the kmd race
 * ahead and write `DDH_BASE_LO` into the shared `cfg-shadow` shm
 * just before q-sys's memset zeroes offsets 0..0x103F. Holding the
 * synthesis until the natural cold-boot publish has happened forces
 * the kmd's cfg writes to be causally ordered after the memset.
 * See `docs/debugging.md` → Side bug 2.
 */
bool r100_mailbox_fw_boot_done_seen(R100MailboxState *s);

#endif /* R100_MAILBOX_H */
