/*
 * R100 HILS log-buffer tail. Polls FreeRTOS `.logbuf` ring at phys
 * 0x10000000 (2 MB, 16384 x 128 B struct rl_log) and emits formatted
 * lines to a chardev. Independent of the in-FW terminal_task flush
 * path (blocked by PSCI CPU_ON warm-boot). Entry layout:
 *   0x00 u64 tick
 *   0x08 u8 type (0 ERR / 1 INFO / 2 DBG / 3 VERBOSE)
 *   0x09 u8 cpu / 0x0A func_id / 0x0B rsvd
 *   0x0C char task[16] / 0x1C char logstr[100]
 * FW writes tick→fields→logstr, so tick==0 && logstr[0]==0 is the
 * stop condition. On end-of-ring, wrap to 0 (matches FW log_idx & mask).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/sysbus.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "exec/cpu-common.h"
#include "r100_soc.h"

#define R100_LOGBUF_ENTRY_SIZE      128u    /* sizeof(struct rl_log) */
#define R100_LOGBUF_TASK_OFFSET     12u
#define R100_LOGBUF_TASK_LEN        16u
#define R100_LOGBUF_STR_OFFSET      (R100_LOGBUF_TASK_OFFSET + R100_LOGBUF_TASK_LEN)
#define R100_LOGBUF_STR_LEN         100u

#define R100_LOGBUF_DEFAULT_BASE    0x10000000ULL   /* FreeRTOS.ld .logbuf phys */
#define R100_LOGBUF_DEFAULT_SIZE    0x00200000ULL   /* 2 MB */
#define R100_LOGBUF_DEFAULT_POLL_MS 50u

static const char *const r100_logbuf_type_name[] = {
    "ERR", "INFO", "DBG", "VERBOSE",
};

struct R100LogBufState {
    SysBusDevice parent_obj;

    /* Properties */
    uint64_t base;
    uint64_t size;
    uint32_t poll_ms;
    CharBackend chr;

    /* Runtime */
    QEMUTimer *timer;
    uint32_t nr_entries;
    uint32_t next_idx;
    uint64_t last_tick;
    bool wrapped;
};

typedef struct R100LogBufState R100LogBufState;

DECLARE_INSTANCE_CHECKER(R100LogBufState, R100_LOGBUF, TYPE_R100_LOGBUF)

/* Read one entry; return true when consumed, false to stop this tick
 * (empty slot = tick==0 && logstr[0]==0). */
static bool r100_logbuf_emit_one(R100LogBufState *s, uint32_t idx)
{
    uint8_t entry[R100_LOGBUF_ENTRY_SIZE];
    uint64_t tick;
    uint8_t type, cpu, func_id;
    char task[R100_LOGBUF_TASK_LEN + 1];
    char logstr[R100_LOGBUF_STR_LEN + 1];
    char outbuf[256];
    const char *type_name;
    int outlen;
    hwaddr addr = s->base + (hwaddr)idx * R100_LOGBUF_ENTRY_SIZE;

    cpu_physical_memory_read(addr, entry, sizeof(entry));

    memcpy(&tick, &entry[0], sizeof(tick));
    type    = entry[8];
    cpu     = entry[9];
    func_id = entry[10];

    memcpy(task, &entry[R100_LOGBUF_TASK_OFFSET], R100_LOGBUF_TASK_LEN);
    task[R100_LOGBUF_TASK_LEN] = '\0';
    memcpy(logstr, &entry[R100_LOGBUF_STR_OFFSET], R100_LOGBUF_STR_LEN);
    logstr[R100_LOGBUF_STR_LEN] = '\0';

    if (tick == 0 && logstr[0] == '\0') {
        return false;
    }

    /* Trim trailing CR/LF so our format controls the newline. */
    size_t ll = strlen(logstr);
    while (ll > 0 && (logstr[ll - 1] == '\n' || logstr[ll - 1] == '\r')) {
        logstr[--ll] = '\0';
    }

    type_name = (type < ARRAY_SIZE(r100_logbuf_type_name))
                ? r100_logbuf_type_name[type] : "?";

    if (task[0] != '\0') {
        outlen = snprintf(outbuf, sizeof(outbuf),
                          "[HILS %010" PRIu64 " cpu=%u %-7s %s] %s\r\n",
                          tick, (unsigned)cpu, type_name, task, logstr);
    } else {
        outlen = snprintf(outbuf, sizeof(outbuf),
                          "[HILS %010" PRIu64 " cpu=%u %-7s func=%u] %s\r\n",
                          tick, (unsigned)cpu, type_name,
                          (unsigned)func_id, logstr);
    }

    if (outlen > 0) {
        qemu_chr_fe_write_all(&s->chr, (const uint8_t *)outbuf,
                              MIN(outlen, (int)sizeof(outbuf) - 1));
    }

    if (tick > s->last_tick) {
        s->last_tick = tick;
    }
    return true;
}

/* Periodic drain: walk forward from next_idx until an empty entry
 * or ring end; naturally bounded by first-empty-slot early-out. */
static void r100_logbuf_drain(void *opaque)
{
    R100LogBufState *s = R100_LOGBUF(opaque);

    while (s->next_idx < s->nr_entries) {
        if (!r100_logbuf_emit_one(s, s->next_idx)) {
            break;
        }
        s->next_idx++;
    }

    if (s->next_idx >= s->nr_entries && !s->wrapped) {
        /* Wrap once; subsequent laps re-emit (no tick dedup). */
        s->wrapped = true;
        s->next_idx = 0;
    }

    timer_mod(s->timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->poll_ms);
}

static void r100_logbuf_realize(DeviceState *dev, Error **errp)
{
    R100LogBufState *s = R100_LOGBUF(dev);

    if (s->size == 0 || s->size % R100_LOGBUF_ENTRY_SIZE != 0) {
        error_setg(errp,
                   "r100-logbuf-tail: size 0x%" PRIx64
                   " must be non-zero and a multiple of %u",
                   s->size, R100_LOGBUF_ENTRY_SIZE);
        return;
    }
    if (s->poll_ms == 0) {
        s->poll_ms = R100_LOGBUF_DEFAULT_POLL_MS;
    }

    s->nr_entries = s->size / R100_LOGBUF_ENTRY_SIZE;
    s->next_idx = 0;
    s->last_tick = 0;
    s->wrapped = false;

    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, r100_logbuf_drain, s);
    timer_mod(s->timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->poll_ms);
}

static void r100_logbuf_unrealize(DeviceState *dev)
{
    R100LogBufState *s = R100_LOGBUF(dev);

    if (s->timer) {
        timer_free(s->timer);
        s->timer = NULL;
    }
}

static Property r100_logbuf_properties[] = {
    DEFINE_PROP_UINT64("base", R100LogBufState, base,
                       R100_LOGBUF_DEFAULT_BASE),
    DEFINE_PROP_UINT64("size", R100LogBufState, size,
                       R100_LOGBUF_DEFAULT_SIZE),
    DEFINE_PROP_UINT32("poll-ms", R100LogBufState, poll_ms,
                       R100_LOGBUF_DEFAULT_POLL_MS),
    DEFINE_PROP_CHR("chardev", R100LogBufState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void r100_logbuf_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r100_logbuf_realize;
    dc->unrealize = r100_logbuf_unrealize;
    device_class_set_props(dc, r100_logbuf_properties);
    dc->user_creatable = false;
}

static const TypeInfo r100_logbuf_info = {
    .name = TYPE_R100_LOGBUF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(R100LogBufState),
    .class_init = r100_logbuf_class_init,
};

static void r100_logbuf_register_types(void)
{
    type_register_static(&r100_logbuf_info);
}

type_init(r100_logbuf_register_types)
