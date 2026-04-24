/*
 * REMU - R100 NPU System Emulator
 * R100 integrated MSI-X trigger — public helper API
 *
 * Companion to r100_imsix.c. Mirrors the r100_mailbox.h pattern:
 * exposes only the QOM type-name string (already in r100_soc.h) plus
 * an opaque `R100IMSIXState` typedef and the single cross-device
 * helper that other NPU-side models (in practice: r100-cm7 / BD-done
 * state machine) use to synthesise an MSI-X trigger without reaching
 * into the device's private struct. Full R100IMSIXState layout and
 * the R100_IMSIX() cast macro stay private to r100_imsix.c.
 *
 * On silicon, the CM7 firmware hits REBELH_PCIE_MSIX_ADDR directly
 * when a BD completes; in REMU the NPU-side CM7 stub lives in QEMU C
 * code, so we expose a function-level hook instead of re-routing
 * fake FW stores through the MMIO region.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef R100_IMSIX_H
#define R100_IMSIX_H

#include "qemu/osdep.h"
#include "qom/object.h"

typedef struct R100IMSIXState R100IMSIXState;

/*
 * Synthesise an iMSIX doorbell for `vector` as if the FW had stored
 * the corresponding db_data at R100_PCIE_IMSIX_DB_OFFSET. The vector
 * is packed into bits [10:0] of db_data per rbln_hw_msix_trigger; PF,
 * VF, VF-Act, and TC fields stay zero (CR03 single-PF VF-Act=0 path,
 * matching the kmd's `rbln_irq_handler` dispatch). No-op (with an
 * LOG_GUEST_ERROR) when the chardev isn't connected — the host side
 * quietly drops; we don't back-pressure the CM7 state machine on a
 * disconnected peer.
 */
void r100_imsix_notify(R100IMSIXState *s, uint32_t vector);

#endif /* R100_IMSIX_H */
