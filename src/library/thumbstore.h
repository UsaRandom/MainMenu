/**
 * @file thumbstore.h
 * @brief The decoded-art atlas on the card. Never decode the same box art twice.
 * @ingroup library
 *
 * A title card costs a measured **259,633 µs** to decode on the test corpus. Twelve of those is
 * 3.1 seconds of cold screen, and without persistence it recurs on every boot forever. That, and
 * not streaming bandwidth, is the argument for a cache file -- see AUDIT.md 1f.
 *
 * ## Layout
 *
 * `menu/cache/thumbs.pak` is a flat array of fixed-size slots, and `menu/cache/thumbs.idx` says
 * which slot holds which source image. Two files rather than one because the index is rewritten
 * whenever a tile is added and the atlas is only ever appended to; keeping them together would
 * mean rewriting a header in the middle of a 16 MB file.
 *
 * **Slot size is 49,152 bytes, and that number is chosen, not rounded.** The test card's FAT32
 * volume has a 16,384-byte allocation unit, and libdragon's FatFs clips every `disk_read` at a
 * cluster boundary (`ff.c:3978`). A slot that is an exact multiple of the cluster therefore never
 * straddles one, so a tile is one seek and one contiguous run instead of two reads with a FAT
 * walk between them. Three clusters rather than two because a tile is a box shape now and the
 * tallest is 109 × 176 = 38,368 bytes; the biggest actually in use, an N64 cover at 109 × 155, is
 * 33,790. So 31 % of a slot is padding -- 24.6 MB across 500 titles, on a card with 29 GB free --
 * where it used to be 16 %. The header occupies slot 0's space for the same alignment reason.
 *
 * **A slot does not imply a tile size.** Every tile is TILE_W wide, because that is the grid
 * column, but the height is the system's box aspect and a Game Boy cover is square. The index
 * carries each tile's own dimensions and a fetch into a differently-shaped surface is refused as
 * a miss rather than read at the wrong stride.
 *
 * **RGBA16, not CI8.** DESIGN.md picks CI8, on a scroll-bandwidth argument that is real. It is
 * also worth about 3.6 ms per tile, and it costs a median-cut quantizer, a 32 KB inverse LUT and
 * a per-tile TLUT -- a meaningful pile of new code whose bugs would look exactly like "the art is
 * slightly wrong". RGBA16 is what the decoder already produces, so storing it is a `memcpy` and
 * a `fwrite`, and the win over a 259 ms decode is ~29× either way. CI8 stays the right answer for
 * a 500-title card that scrolls fast; it is an optimisation of this, not a prerequisite.
 *
 * ## Invalidation
 *
 * A slot is keyed by the FNV-1a hash of its source path **and** the source file's size, so
 * replacing `Zelda.jpg` with a different scan of the same name misses and re-decodes. Slots are
 * never reused or compacted: a stale one is simply unreferenced, and the whole pair is deleted
 * and rebuilt on a format bump. An atlas that only grows is an atlas with no free-list to get
 * wrong.
 */

#ifndef LIBRARY_THUMBSTORE_H__
#define LIBRARY_THUMBSTORE_H__

#include <stdbool.h>
#include <stdint.h>
#include <surface.h>

/** @brief 'M64U'.
 *
 * Was 'M64T' (0x4D363454) until tiles became box shapes. Everything about the atlas changed: the
 * slot grew from 32,768 to 49,152 bytes, the index record from 16 to 20, and every tile already
 * in it is 140 x 98 landscape against covers that are now portrait or square.
 *
 * Its own magic rather than MENU_CACHE_FORMAT_VER, for the reason cache.h now spells out: the
 * shared version would take playstate.dat with it, and every favourite and play count on the card
 * with it. The atlas can be rebuilt from the covers; a play history cannot be rebuilt from
 * anything. */
#define THUMBSTORE_MAGIC 0x4D363455

/** @brief Open the atlas and its index. Safe to call on read-only storage. */
void thumbstore_open (void);

/** @brief Flush the index if it changed, then close. */
void thumbstore_close (void);

/** @brief Is there a usable atlas to read from? */
bool thumbstore_available (void);

/**
 * @brief Is there a tile for @p src_path, without reading it?
 *
 * An index lookup and nothing else -- the index is resident, so this costs a hash and a scan of a
 * few hundred rows. It exists so a caller can find out before committing to the ~34,000-byte
 * surface a fetch needs: thumbcache.c used to allocate one, attempt the fetch, and free it again
 * on every miss, which on a cold card is an allocate-and-free of a whole tile per candidate per
 * pass. See AUDIT.md 1ae.
 */
bool thumbstore_has (const char *src_path, int64_t src_size);

/**
 * @brief Read the tile for @p src_path into @p dst, if one is cached and still matches.
 *
 * @p dst must already be an FMT_RGBA16 surface of the shape the caller wants. A cached tile of a
 * different shape is reported as a miss -- see the note on shapes above.
 * @p dominant receives the cached wash colour so it does not have to be recomputed.
 *
 * @return true on a hit. A miss is not an error.
 */
bool thumbstore_fetch (const char *src_path, int64_t src_size, surface_t *dst, uint16_t *dominant);

/**
 * @brief Append @p art to the atlas as the tile for @p src_path.
 *
 * No-op on read-only storage. Failure is not propagated to the caller because there is nothing
 * useful it could do: the tile is already decoded and on screen either way.
 */
void thumbstore_put (const char *src_path, int64_t src_size, const surface_t *art, uint16_t dominant);

/** @brief Write the index now, if it is dirty. Called at natural pauses, not per tile. */
void thumbstore_flush (void);

/** @brief Tiles currently in the atlas, for the settings screen and the log. */
int thumbstore_count (void);

#endif /* LIBRARY_THUMBSTORE_H__ */
