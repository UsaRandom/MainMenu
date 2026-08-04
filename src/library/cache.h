/**
 * @file cache.h
 * @brief Shared discipline for every file the menu writes to the card.
 * @ingroup library
 *
 * Five things are persisted -- settings, the library index, play history, cheat selections and
 * decoded art -- and every one of them is a cache: derivable from the card's contents, worth
 * keeping only because deriving it again is slow. That shapes the rules, and the rules are here
 * rather than repeated five times.
 *
 * ## The rules
 *
 * **A cache file is never migrated.** Magic plus format version in the header; a mismatch deletes
 * the file and rebuilds from source. Migration code for a cache is code that runs rarely, is
 * tested never, and buys nothing that a rebuild does not. `src/cheats/cheatdb.c` established
 * this and it is the reference implementation.
 *
 * **A truncated file must be detected, not consumed.** Power can go at any point in a write, and
 * the N64 has no journalling filesystem underneath. Every payload therefore carries its own
 * length and CRC32, checked before a single byte is believed. A failed check is treated exactly
 * like a version mismatch: delete and rebuild.
 *
 * **Nothing here may ever be load-bearing.** Every read can fail and every write can fail, and
 * the menu must behave identically to the way it did before any of this existed -- slower, but
 * identical. This is not defensive politeness: the machine this is developed against *cannot*
 * write at all, because ares exposes the ROM's DFS as the storage prefix and the DFS is read
 * only. Graceful degradation is the only mode that has ever been tested here, so it is the one
 * that has to be right.
 *
 * ## Why a single writability probe
 *
 * `cache_writable()` answers once, at boot, by attempting a real write to a real file in the real
 * cache directory. The alternative -- letting each subsystem discover the failure on its own --
 * means a menu on a write-protected card pays a failed `fopen` per thumbnail, forever, and each
 * one is a filesystem round trip. Ask once, remember, and let every writer short-circuit.
 */

#ifndef LIBRARY_CACHE_H__
#define LIBRARY_CACHE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Bumped whenever ANY cache layout changes.
 *
 * Deliberately one number for all of them rather than one each. The caches reference each other
 * -- thumbs.idx holds slot numbers that only mean anything against the pak, playstate keys on the
 * check codes the index computed -- so a version scheme that lets them disagree is a scheme that
 * eventually lets a stale one be believed. One number, everything rebuilds together.
 */
#define MENU_CACHE_FORMAT_VER   1

/** @brief Header on every cache file. 16 bytes, so the payload stays 8-byte aligned. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format_ver;
    uint16_t flags;
    uint32_t payload_bytes;
    uint32_t payload_crc;
} cache_header_t;

/**
 * @brief Point the cache layer at storage and decide, once, whether it can be written.
 *
 * Creates `<storage>menu/cache/` if it is missing and probes it with a real write. Safe to call
 * when storage is read-only; that is the expected case under ares.
 */
void cache_init (const char *storage_prefix);

/** @brief Can anything be written at all? False under ares, and on a locked or full card. */
bool cache_writable (void);

/** @brief Why not, for the settings screen and the log. Never NULL. */
const char *cache_status (void);

/**
 * @brief Build `<storage>menu/cache/<name>` into a caller-supplied buffer.
 *
 * Not a static buffer: two of these are live at once when the thumbnail atlas opens its pak and
 * its index together.
 */
void cache_path (char *out, size_t cap, const char *name);

/**
 * @brief Read a whole cache file, verifying magic, version, length and CRC.
 *
 * On success @p payload is a fresh allocation the caller owns and @p bytes is its length. On any
 * failure -- absent, short, wrong magic, wrong version, bad CRC -- returns false, having deleted
 * the file if it existed and was unusable. An absent file is not an error and is not logged as
 * one; a corrupt file is both.
 */
bool cache_load (const char *name, uint32_t magic, void **payload, uint32_t *bytes);

/** @brief Write a whole cache file with header and CRC. False if storage is not writable. */
bool cache_store (const char *name, uint32_t magic, const void *payload, uint32_t bytes);

/** @brief Delete a cache file, if it exists and if that is even possible. */
void cache_drop (const char *name);

/** @brief CRC32 (IEEE), nibble-table. ~7 ms on an 84 KB index at 93 MHz. */
uint32_t cache_crc32 (const void *data, size_t bytes);

/** @brief FNV-1a 64, used to key records on strings without storing the strings twice. */
uint64_t cache_hash64 (const char *s);

/** @brief FNV-1a 32, for cheat group names -- see cheatstate.h on why the name and not the index. */
uint32_t cache_hash32 (const char *s);

#endif /* LIBRARY_CACHE_H__ */
