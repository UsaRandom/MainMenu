/**
 * @file thumbcache.h
 * @brief Resident title-card art for the grid.
 * @ingroup library
 *
 * A fixed pool of slots holding decoded 140 x 98 art, filled on demand from the 280 x 196 PNGs
 * on the card and evicted least-recently-requested when the pool is full. The grid asks for
 * the art it is about to draw; the cache decides what that costs.
 *
 * ### Why RGBA16 here and CI8 later
 *
 * The format decision in DESIGN.md picks CI8, and the reason given is SD streaming bandwidth:
 * one row of scroll is four new tiles, 61 KB as CI8 against 115 KB as RGBA16, which at ~3 MB/s
 * over FatFs is 20 ms against 38 ms. That argument only applies once tiles are being *read from
 * a cache file*. This stage has no cache file -- art is decoded from the source PNG straight
 * into a slot -- so the bandwidth in question does not exist and quantising would cost decode
 * time to buy nothing. 20 slots at RGBA16 is 573 KB, which is affordable on 8 MB.
 *
 * CI8 and the on-disk atlas arrive together, because that is when the streaming argument bites.
 * The slot API below does not expose the pixel format, so that change stays inside this file.
 *
 * ### Why there is no cache file yet
 *
 * Under ares the storage prefix is "rom:/" and the DFS is read-only, so a cache file cannot be
 * written at all on the machine this is developed against. Persistence therefore has to be
 * built and measured against real hardware, not bolted on where it cannot be exercised.
 */

#ifndef THUMBCACHE_H__
#define THUMBCACHE_H__

#include <stdbool.h>
#include <stdint.h>
#include <surface.h>

#include "library.h"

/* 20 slots: 12 tiles visible plus one row above and one below, so the row a scroll is about to
 * reveal is already decoded rather than popping in after the motion stops. */
#define THUMB_SLOTS  20

typedef struct thumbcache_s thumbcache_t;

/** @brief Allocate the pool. Returns NULL if the slots do not fit. */
thumbcache_t *thumbcache_init (const char *storage_prefix);

void thumbcache_free (thumbcache_t *tc);

/**
 * @brief Ask for @p rec's art and mark it as wanted this frame.
 *
 * Returns the decoded surface, or NULL if it is not ready. Never blocks and never decodes --
 * the work happens in thumbcache_run(). Callers draw the record's art_state when this is NULL.
 */
surface_t *thumbcache_get (thumbcache_t *tc, library_t *lib, uint16_t rom_id);

/** @brief Begin a frame: nothing is wanted until it is requested. */
void thumbcache_begin_frame (thumbcache_t *tc);

/**
 * @brief Spend up to @p budget_us decoding. Call from a screen's background() phase.
 *
 * @return true if any decoding happened, so a caller can tell "still working" from "idle".
 */
bool thumbcache_run (thumbcache_t *tc, library_t *lib, uint32_t budget_us);

/** @brief Slots currently holding decoded art, for diagnostics. */
int thumbcache_resident (const thumbcache_t *tc);

/** Split of thumbcache_run's cost: rows actually decoded vs the walk that finds the next image,
 *  plus how many decodes were started and how many art_path/file_exists probes it took. */
extern uint32_t thumb_rows_us, thumb_scan_us, thumb_starts, thumb_statcalls;

#endif /* THUMBCACHE_H__ */
