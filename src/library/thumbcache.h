/**
 * @file thumbcache.h
 * @brief Resident title-card art for the grid.
 * @ingroup library
 *
 * A fixed pool of slots holding decoded box art -- 109 px wide, as tall as the system's box is,
 * see library/boxart.h -- filled on demand from whatever the PNGs on the card happen to be and evicted least-recently-requested when the pool is full. The grid asks for
 * the art it is about to draw; the cache decides what that costs.
 *
 * ### Why RGBA16 here and CI8 later
 *
 * The format decision in DESIGN.md picks CI8, and the reason given is SD streaming bandwidth:
 * one row of scroll is four new tiles, 61 KB as CI8 against 115 KB as RGBA16, which at ~3 MB/s
 * over FatFs is 20 ms against 38 ms. That argument only applies once tiles are being *read from
 * a cache file*. This stage has no cache file -- art is decoded from the source PNG straight
 * into a slot -- so the bandwidth in question does not exist and quantising would cost decode
 * time to buy nothing. 36 slots at RGBA16 is at most 1.19 MB, which is affordable on 8 MB.
 *
 * CI8 and the on-disk atlas arrive together, because that is when the streaming argument bites.
 * The slot API below does not expose the pixel format, so that change stays inside this file.
 *
 * ### The pool is not the atlas
 *
 * There are two caches and they answer different questions. This one is 36 RAM slots and holds
 * what is on screen. `thumbs.pak` -- library/thumbstore.h -- is the card, and holds everything
 * decoded on any boot ever. Filling the atlas used to be a side effect of filling the pool, which
 * meant it could only ever contain tiles the user had personally scrolled past, and could never
 * finish. thumbcache_build() is the separate, ordered pass that finishes it.
 *
 * Under ares the storage prefix is "rom:/" and the DFS is read-only, so there is no atlas at all
 * on the machine this is developed against: thumbstore_available() is false, the build is a no-op,
 * and the grid falls back to thumbcache_run()'s on-demand decode. Everything above the fallback is
 * therefore exercised only on hardware.
 */

#ifndef THUMBCACHE_H__
#define THUMBCACHE_H__

#include <stdbool.h>
#include <stdint.h>
#include <surface.h>

#include "boxart.h"
#include "library.h"

/* 36 slots, up from 20.
 *
 * The worst case is a landscape tab: four columns on a 110 px row pitch in a 352 px window, so a
 * scroll position that straddles rows shows four of them -- sixteen tiles, not the twelve the old
 * comment counted. Twenty slots therefore left four spare, which is one row of margin at best and
 * none at all when the selection sits on a boundary. Scrolling two rows evicted art that was about
 * to be needed again, and the tile came back as a placeholder even though its pixels were already
 * on the card. A portrait tab is five columns on a 167 px pitch: fifteen tiles, and a square one
 * is twelve, so sixteen is the number to size for.
 *
 * 36 is sixteen visible plus THUMB_PREFETCH_ROWS above and below, and four spare. The cost is
 * bounded and lazy: a slot allocates its surface only when something lands in it, and the surface
 * is that cover's own box shape -- 39,200 bytes for a square cover at 140 x 140, 33,790 for a
 * portrait one at 109 x 155, 27,440 for a landscape one at 140 x 98. Full of the largest it is
 * 1.35 MB against the ~3.8 MB free after the framebuffers -- affordable only because the M64 has
 * the Expansion Pak built in. It was 1.19 MB when every tile was 109 wide. */
#define THUMB_SLOTS  36

/* How far past the visible window art is fetched. Two rows either side, so a page of scrolling
 * lands on tiles that are already resident instead of on the placeholder-then-pop the cache was
 * meant to prevent. The grid asks for these explicitly -- see screen_grid's render -- because the
 * prefetch pass inside thumbcache_run walks the library from index 0, which fills the cache with
 * whatever happens to be near the front of the card rather than near the cursor. */
#define THUMB_PREFETCH_ROWS  2

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
 * @brief Decode on demand into the pool. Only for storage with no atlas; see thumbcache_build().
 *
 * @return true if any decoding happened, so a caller can tell "still working" from "idle".
 */
bool thumbcache_run (thumbcache_t *tc, library_t *lib, uint32_t budget_us);

/**
 * @brief Advance the ordered background build of the atlas by up to @p budget_us.
 *
 * Walks the library **in index order**, once, decoding every cover that is not in `thumbs.pak`
 * yet straight into it and freeing the surface immediately. It claims no slot, evicts nothing, and
 * does not care where the cursor is.
 *
 * ### Why this is not the display cache doing its own prefetch
 *
 * It used to be, and it could not finish. The pool is 36 slots and prefetch into it never evicts
 * -- deliberately, because prefetch that evicts thrashes for ever on a library larger than the
 * pool (AUDIT 1u). So once 36 tiles were resident the walk returned "no slot", set the idle flag,
 * and stopped. On a 289-title card the atlas therefore only ever gained tiles the user had
 * personally scrolled past, and the order they arrived in was whatever the cursor had done. The
 * build is decoupled from the pool precisely so that finishing is possible at all.
 *
 * ### Why in order rather than nearest the cursor
 *
 * Because it finishes, and because what it produces is on the card rather than on the screen.
 * Chasing the cursor means the work depends on where the user went, which is the behaviour that
 * made this unpredictable; a straight walk covers the card once and is done with it for good.
 *
 * @return true while there is still work, false once the whole library has been covered.
 */
bool thumbcache_build (thumbcache_t *tc, library_t *lib, uint32_t budget_us);

/**
 * @brief Fill slots from the on-disk atlas only, never from a PNG. Safe while scrolling.
 *
 * The grid stops decoding entirely while the cursor is moving, because one row of a real PNG can
 * cost more than the frame it is trying to stay out of the way of. That gate was applied to the
 * atlas too, and it should never have been: a cached tile is one seek and a 27 KB read, tens of
 * times cheaper than a decode and nowhere near a field. The visible result was a grid that
 * refused to fill in until the scroll stopped even though every tile on it had been decoded on a
 * previous boot and was sitting in thumbs.pak.
 *
 * Loops until @p budget_us is spent rather than doing one tile per call, so a fast scroll can
 * keep up with the four tiles a row costs.
 *
 * @return true if anything landed.
 */
bool thumbcache_fetch (thumbcache_t *tc, library_t *lib, uint32_t budget_us);

/** @brief Slots currently holding decoded art, for diagnostics. */
/**
 * @brief Throw every resident tile away, because the shape they were cut to has changed.
 *
 * Called when the box art region changes in Settings. Not an invalidation of the atlas -- the
 * tiles on the card are still perfectly good for whatever shape they were cut at, and if the
 * region is switched back they are hits again.
 */
void thumbcache_reshape (thumbcache_t *tc, library_t *lib);

/**
 * @brief Remember which resident tiles belong to which games, by path, before the records move.
 *
 * The RAM pool is keyed on rom_id -- an index into library.records. library_finish() qsorts that
 * array when a display name changes, so every slot would then hold someone else's art (or, for
 * any id that no longer matches a slot, ART_READY with no surface: a permanent blank, because
 * the decoder only starts ART_PENDING records). Call this, then the sort, then #thumbcache_rebind.
 *
 * Also drops any decode in flight, before the sort: decode_done holds a record pointer that
 * qsort invalidates. #thumbcache_rebind still defends the same case after the sort.
 */
void thumbcache_prepare_shuffle (thumbcache_t *tc, const library_t *lib);

/**
 * @brief Point every resident slot at the record that still owns its pixels, after a sort.
 *
 * Also forget the per-index size memo and the wanted list, both of which are rom_id arrays,
 * and drop any decode in flight (its callback holds a record pointer that qsort invalidates).
 * Does not drop the atlas: thumbs.pak is keyed by source path, which did not move.
 */
void thumbcache_rebind (thumbcache_t *tc, library_t *lib);

/**
 * @brief The tile shape @p rec's art is cut to: its own snapped shape, or its system's.
 *
 * The grid needs this for the row height and the detail sheet for its art panel, both of which
 * happen outside this file, and both of which must agree with what was actually decoded.
 */
art_shape_t thumbcache_record_shape (const lib_record_t *rec);

/**
 * @brief The shape a tile is DECODED AND STORED at, which is not always the shape it is drawn at.
 *
 * The same as thumbcache_record_shape() with an Expansion Pak, and half of it without. A tile is
 * RGBA16 and the pool is the only thing on a 4 MB console big enough to matter: at full size a
 * 499-title card affords three resident tiles against a twelve-tile screen, because the library
 * has taken everything else. Half the width and half the height is a quarter of the bytes, so the
 * same memory holds twelve.
 *
 * The grid keeps using thumbcache_record_shape() for its LAYOUT -- row heights and column widths
 * must not change with the amount of RAM in the console -- and draw_tile() already scales against
 * the surface's own size, so a half-size tile lands in a full-size cell without arithmetic. What
 * it costs is sharpness, and rdpq copy mode: copy cannot scale, so a scaled tile draws through the
 * standard pipeline. See screen_grid's draw_tile.
 *
 * thumbstore keys its slots on the tile's own dimensions and refuses a mismatch as a miss, so the
 * two sizes coexist in one thumbs.pak without either being read at the wrong stride -- a card moved
 * between a 4 MB and an 8 MB console builds both and neither is wrong.
 */
art_shape_t thumbcache_store_shape (const lib_record_t *rec);

int thumbcache_resident (const thumbcache_t *tc);

/** Split of thumbcache_run's cost: rows actually decoded vs the walk that finds the next image,
 *  plus how many decodes were started and how many art_path/file_exists probes it took. */
extern uint32_t thumb_rows_us, thumb_scan_us, thumb_starts, thumb_statcalls;

#endif /* THUMBCACHE_H__ */
