/**
 * @file allocwatch.h
 * @brief Heap traffic counters. See allocwatch.c.
 * @ingroup dev
 */

#ifndef DEV_ALLOCWATCH_H__
#define DEV_ALLOCWATCH_H__

#include <stdint.h>

typedef struct {
    uint32_t mallocs, reallocs, frees, bytes;
} alloc_stats_t;

#ifdef DEV_HARNESS

extern alloc_stats_t alloc_stats;
void allocwatch_reset (void);

#else

/* Not a macro: an unused-value expression under -Werror is how the last no-op broke the release
 * build, and that only surfaced when someone asked to run the menu. */
static const alloc_stats_t alloc_stats = { 0, 0, 0, 0 };
static inline void allocwatch_reset (void) { }

#endif

#endif /* DEV_ALLOCWATCH_H__ */
