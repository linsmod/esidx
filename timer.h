/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
#ifndef ESIDX_TIMER_H
#define ESIDX_TIMER_H

/* Monotonic wall-clock helpers used for the phase timings in scan/store/query.
 * Header-only: these are called on the hot path only when the corresponding log
 * level is enabled, and clock_gettime is a vDSO call on Linux. */

#include <stdint.h>
#include <time.h>

#include "log.h"

static inline uint64_t ts_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* microseconds elapsed since t0, as a double in milliseconds */
static inline double ts_ms_since(uint64_t t0)
{
    return (double)(ts_us() - t0) / 1000.0;
}

/* LOGI the elapsed time of a phase; the arguments are only evaluated when
 * INFO logging is on. */
#define TSDONE(label, t0) \
    do { if (log_enabled(LOG_INFO)) LOGI("%s: %.3f ms", (label), ts_ms_since(t0)); } while (0)

#define TSDONE2(label, t0, extra_fmt, ...)                                     \
    do { if (log_enabled(LOG_INFO))                                             \
             LOGI("%s: %.3f ms " extra_fmt, (label), ts_ms_since(t0), __VA_ARGS__); \
    } while (0)

#endif /* ESIDX_TIMER_H */