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
 * database is consulted -- and compares.
 *
 * Signatures count **every** entry the scan would look at, art files included, so dropping a new
 * `.jpeg` next to a ROM invalidates the index and the art is picked up.
 *
 * ## Repairing it rather than throwing it away
 *
 * One mismatch anywhere used to mean a full rescan, and that was the wrong shape of answer. The
 * signature is already **per directory**: it knows not merely that the card moved but exactly
 * where. Measured on the M64 at 289 titles, believing the index costs 0.72 s and rebuilding it
 * costs 14.4 s -- so adding one game to one folder paid the second price for a change confined to
 * a twenty-sixth of the card.
 *
 * A load now has three outcomes rather than two. Everything matching is believed as before. Some
 * directories matching means the titles in those come out of the file, every directory that is
 * new or changed is indexed with **no recursion** -- its children carry their own signatures and
 * get their own verdict -- and the repaired index is written back so the next boot is a plain
 * revalidation again. Nothing matching, or a walk that failed, still returns false and still
 * costs a full scan.
 *
 * Records are attributed to directories by their own stored path, not by a directory number in
 * the record. See path_dir_hash() for why the redundant copy is the more dangerous option.
 *
 * The signature walk fills the loose-art table on the way past, which the scan used to be the only
 * thing that did. Without it, a repair could only see the covers in the directories it happened to
 * rescan -- so a game added to one folder with its cover kept in another would resolve to nothing,
 * record that as LIBF_ART_MISSING, and be wrong on every boot afterwards, because by then the
 * directories all match again.
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

/** @brief 'M64N' */
/* 'M64N'. Was 'M64M' until a record carried the shape its cover snapped to. That field was a
 * reserved byte, so an index written by the old build reads back as art_kind 0 -- which is
 * ART_PORTRAIT, not "unknown". Every square Game Boy cover on the card would be declared portrait
 * and never re-probed, and nothing would look wrong enough to investigate.
 *
 * 'M64M' was itself a bump from 'M64L' (0x4D36344C), when art paths started being resolved at scan
 * time rather
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
#define LIBINDEX_MAGIC 0x4D36344E

/** @brief What a load did, for the boot record. See app.c. */
typedef struct {
    /** True when the card had moved and the index was repaired rather than believed or thrown
     *  away. `records_kept` then counts the titles that came out of the file and
     *  `records_scanned` the ones read off the card again. */
    bool incremental;
    int  dirs_total;            /**< directories the signature walk visited */
    int  dirs_rescanned;        /**< of those, the ones whose signature had moved */
    int  records_kept;          /**< titles taken from the file; the whole library on a plain hit */
    int  records_scanned;       /**< titles indexed from the card during a repair */
} libindex_result_t;

/**
 * @brief Fill @p lib from the cache, repairing it if the card has moved underneath it.
 *
 * Three outcomes, and only the third costs a full scan:
 *
 * - **Unchanged.** Every directory signature matches; the file is believed as it stands.
 * - **Moved.** Some directories match and some do not. Titles in the ones that match are taken
 *   from the file, the rest of the card is read again one directory at a time, and the repaired
 *   index is written back. Adding a game to a 289-title card costs the directory it went in
 *   rather than all 26 of them.
 * - **Unusable.** No file, a header that does not survive its bounds checks, a walk that failed,
 *   or nothing at all left to keep. Returns false and the caller scans.
 *
 * @param on_progress  Optional; called once with the cached title count, then during the
 *                     revalidation walk (always -1) and any rescan, so the boot plate keeps
 *                     moving and -- the reason it reaches the walk at all -- the mixer keeps
 *                     being fed. The seed is what stops a warm walk painting 0 TITLES. The walk
 *                     is one blocking pass over every directory on the card and nothing else
 *                     touches the AI while it runs. NULL is silent, which is what the host tests
 *                     use.
 * @param out          Optional; filled in on every non-trivial return.
 *
 * @return true if @p lib is now populated and no scan is needed.
 */
bool libindex_load (library_t *lib, const char *storage_prefix, const char *root,
                    library_scan_progress_t on_progress, libindex_result_t *out);

/**
 * @brief Write @p lib and a fresh set of directory signatures. False if storage is read-only.
 *
 * @param on_progress  Optional, and only ever called with -1: this takes the same full walk of the
 *                     card libindex_load() does, so it needs the same tick to keep the audio alive.
 *                     It has nothing to report about progress and does not pretend to.
 */
bool libindex_save (const library_t *lib, const char *storage_prefix, const char *root,
                    library_scan_progress_t on_progress);

#endif /* LIBRARY_LIBINDEX_H__ */
