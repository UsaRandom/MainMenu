/**
 * @file libindex.h
 * @brief The library index cache: the difference between a 6-second boot and a fast one.
 * @ingroup library
 *
 * `menu/cache/library.idx` holds the result of a full scan so the next boot does not repeat it.
 * Measured on the test card, a scan costs **11,499 µs per ROM** -- one 4 KB header read, a
 * byte-swap, a lookup in the ~450-entry database, and the art resolution walk -- which is 5.75
 * seconds at 500 titles, on a path that is *faster* than the card because ares reads through the
 * DFS. This is by a wide margin the largest fixed cost in the product.
 *
 * ## Deciding whether the cache is still true
 *
 * The index is only usable if the card has not changed underneath it, and answering that must
 * cost far less than the scan or there is no point. So the file also stores a **signature per
 * directory**: `{path hash, entry count, sum of file sizes}`. Revalidation walks the tree with
 * `dir_findfirst`/`dir_findnext` and nothing else -- no file is opened, no header is read, no
 * database is consulted -- and compares. One mismatch anywhere means a full rescan.
 *
 * Signatures count **every** entry the scan would look at, art files included, so dropping a new
 * `.jpeg` next to a ROM invalidates the index and the art is picked up.
 *
 * What this deliberately does not catch: a file edited in place to exactly the same size. There
 * is no mtime available -- libdragon's `dir_t` carries `d_name`, `d_type` and `d_size` and
 * nothing else -- so that case would need a content hash, which would cost the read the whole
 * scheme exists to avoid. The failure mode is a stale title for one game until something else in
 * that directory changes, which is a fair price and is written down here rather than discovered.
 *
 * ## What is and is not in it
 *
 * Everything derived from the card: header fields, database results, titles, paths, and the
 * resolved art path -- including the fact that a title has **no** art, which is the most
 * expensive answer to reach and the usual one on a card with no art pack.
 *
 * Nothing the user did. Favourites and play counts are in `playstate.dat`, because this file's
 * recovery strategy is "delete it" and theirs cannot be. See playstate.h.
 */

#ifndef LIBRARY_LIBINDEX_H__
#define LIBRARY_LIBINDEX_H__

#include <stdbool.h>

#include "library.h"

/** @brief 'M64L' */
/* 'M64M'. Was 'M64L' (0x4D36344C) until art paths started being resolved at scan time rather
 * than lazily per tile. An index written by the old build carries no art at all, and nothing in
 * the staleness check would notice -- the directories have not changed, so it would be believed
 * forever and every boot after the first would show a library with no art.
 *
 * Bumping this file's own magic rather than MENU_CACHE_FORMAT_VER is deliberate. The shared
 * version is one number for every cache on purpose, and raising it would take playstate.dat with
 * it -- the one file here that cannot be rebuilt from the card, holding every favourite and every
 * play count. The index can be rebuilt; a scan is what it is for. So only the index is
 * invalidated, and it is invalidated the way cache.c already handles: magic mismatch, delete,
 * rebuild. */
#define LIBINDEX_MAGIC 0x4D36344D

/**
 * @brief Fill @p lib from the cache, if the cache is present and still matches the card.
 *
 * @return true if @p lib is now populated and no scan is needed.
 */
bool libindex_load (library_t *lib, const char *storage_prefix, const char *root);

/** @brief Write @p lib and a fresh set of directory signatures. False if storage is read-only. */
bool libindex_save (const library_t *lib, const char *storage_prefix, const char *root);

#endif /* LIBRARY_LIBINDEX_H__ */
