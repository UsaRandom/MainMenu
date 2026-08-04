/* Host-test shim. Only what cache.c and thumbstore.c actually use.
 *
 * The timing macros collapse to zero rather than reading a host clock. thumbstore.c uses them
 * only to log how long a slot took, and a test that measured host I/O would be measuring the
 * wrong machine anyway -- the numbers that matter come off the cart. */
#ifndef HOSTTEST_LIBDRAGON_H
#define HOSTTEST_LIBDRAGON_H

#include <stdio.h>
#include <stdint.h>

#include "surface.h"

#define debugf(...) fprintf(stderr, "    [rom] " __VA_ARGS__)

#define TICKS_READ()        ((uint32_t)0)
#define TICKS_SINCE(t)      ((uint32_t)((void)(t), 0))
#define TIMER_MICROS(t)     ((uint32_t)((void)(t), 0))

#endif
