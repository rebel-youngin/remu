/*
 * REMU - R100 NPU System Emulator
 * r100-rbdma — RBDMA register block + functional kick → done IRQ stub.
 *
 * Public API:
 *   - TYPE_R100_RBDMA: QOM type name, defined here so the .c keeps its
 *     state struct private (project header discipline; see CLAUDE.md).
 *
 * No cross-device helpers are exported today — q-cp drives the device
 * entirely through MMIO + the wired GIC SPIs. Future RBDMA features
 * (e.g. an out-of-band kick from r100-cm7 for DMA-side debug
 * injection) would land their prototypes here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef R100_RBDMA_H
#define R100_RBDMA_H

#define TYPE_R100_RBDMA         "r100-rbdma"

#endif /* R100_RBDMA_H */
