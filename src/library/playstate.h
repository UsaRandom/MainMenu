/**
 * @file playstate.h
 * @brief What the user did, kept separately from what the card contains.
 * @ingroup library
 *
 * Favourites, last played and play count live in `menu/cache/playstate.dat` and NOT in
 * `library.idx`, and the split is the whole design rather than tidiness.
 *
 * `library.idx` is derived data: it is a faster copy of what a scan of the card would produce,
 * and it must be safe to delete and rebuild at any moment -- on a format bump, on a CRC failure,
 * on the user adding a folder of ROMs. Play history is the opposite. It is the only thing here
 * that cannot be recovered from the card, so it can never live in a file whose recovery strategy
 * is "throw it away".
 *
 * Records are keyed on **check_code**, the 64-bit value `rom_info.c` extracts from the ROM
 * header, and not on the path. That is what lets a favourite survive the user reorganising their
 * card, renaming a file, or moving a game between folders -- the identity of a game is in the
 * game, not in where it happens to sit. Titles with no usable header (every emulated-system ROM,
 * since only N64 headers are parsed) fall back to a hash of the filename, which is weaker but
 * still survives a move between directories.
 */

#ifndef LIBRARY_PLAYSTATE_H__
#define LIBRARY_PLAYSTATE_H__

#include <stdbool.h>
#include <stdint.h>

#include "library.h"

/** @brief 'M64P' */
#define PLAYSTATE_MAGIC 0x4D363450

/** @brief Load playstate.dat and apply it to @p lib. Absent is normal and silent. */
void playstate_load (library_t *lib);

/**
 * @brief Write playstate.dat from @p lib, if anything changed and storage allows it.
 *
 * Only records carrying something worth keeping are written, so a library where nothing has been
 * favourited or played produces no file at all rather than 500 zeroed rows.
 *
 * @return true if a write happened.
 */
bool playstate_save (const library_t *lib);

/** @brief Mark the state dirty. Cheap; the write happens at a sensible moment, not here. */
void playstate_touch (void);

/** @brief Has anything changed since the last successful save? */
bool playstate_dirty (void);

/** @brief Record that @p rec was launched: stamps last_played and bumps play_count. */
void playstate_played (lib_record_t *rec, uint32_t now);

/** @brief The key @p rec is stored under. Exposed so cheatstate can key on the same thing. */
uint64_t playstate_key (const lib_record_t *rec);

#endif /* LIBRARY_PLAYSTATE_H__ */
