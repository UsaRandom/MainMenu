/**
 * @file allocwatch.c
 * @brief Count heap traffic, so "no allocation in the steady-state frame" is a measurement.
 * @ingroup dev
 *
 * DEV_HARNESS only. Linked in with `-Wl,--wrap=malloc` and friends, which is the only way to see
 * every allocation including the ones inside libdragon and libspng -- a malloc hook installed
 * from our own code would miss those, and they are precisely the ones worth catching.
 *
 * The claim this exists to test is from the plan's M5: a frame that is only drawing should touch
 * the heap zero times. An allocation per frame is invisible at 60 Hz until the heap fragments an
 * hour in, and this hardware has no MMU to make that fail loudly.
 *
 * Counting is deliberately not attributed to a call site. A count and a byte total is enough to
 * answer "is it zero", and anything richer needs a backtrace this platform will not give cheaply.
 */

#include <stdint.h>
#include <stdlib.h>
#include <libdragon.h>

#include "dev/allocwatch.h"

alloc_stats_t alloc_stats;

extern void *__real_malloc (size_t size);
extern void *__real_calloc (size_t n, size_t size);
extern void *__real_realloc (void *p, size_t size);
extern void  __real_free (void *p);

void *__wrap_malloc (size_t size) {
    alloc_stats.mallocs++;
    alloc_stats.bytes += size;
    return __real_malloc(size);
}

void *__wrap_calloc (size_t n, size_t size) {
    alloc_stats.mallocs++;
    alloc_stats.bytes += (uint32_t)(n * size);
    return __real_calloc(n, size);
}

void *__wrap_realloc (void *p, size_t size) {
    alloc_stats.reallocs++;
    alloc_stats.bytes += size;
    return __real_realloc(p, size);
}

void __wrap_free (void *p) {
    if (p != NULL) {
        alloc_stats.frees++;
    }
    __real_free(p);
}

void allocwatch_reset (void) {
    alloc_stats.mallocs = 0;
    alloc_stats.reallocs = 0;
    alloc_stats.frees = 0;
    alloc_stats.bytes = 0;
}
