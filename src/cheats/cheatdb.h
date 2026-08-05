/**
 * @file cheatdb.h
 * @brief Read-only cheat database, keyed by ROM header.
 * @ingroup cheats
 *
 * `<storage>/menu/cheats.db`, produced by tools/mkcheatdb.py. A release artifact placed on the
 * card, never committed and never linked into the ROM.
 *
 * The whole index is read once at boot -- 20 bytes per game, so 936 games is 18.7 KB -- and the
 * file handle stays open. Selecting a game binary-searches the index and reads that game's blob
 * with one fseek and one fread.
 *
 * ## Why groups, not lines
 *
 * A Datel cheat is often several lines that only work together: `D0` reads an address and skips
 * the next line unless it matches, so `D0…` / `80…` is one indivisible thing. Upstream let the
 * user toggle each LINE independently and then emitted only the enabled ones
 * (datel_codes.c generate_enabled_cheats_array), which means disabling half a pair leaves the
 * conditional to pair with whatever code follows it and patch an unrelated address. Silently.
 *
 * So the unit here is a named group, `cheat_group_t`, and the only thing the UI can toggle is a
 * group. cheatdb_emit() walks groups and writes every line of an enabled one contiguously. There
 * is no API that can separate a conditional from its write. See docs/AUDIT.md 2.2.
 */

#ifndef CHEATS_CHEATDB_H__
#define CHEATS_CHEATDB_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Bumped whenever the on-disk layout changes; a mismatch is a hard reject, not a migration. */
#define CHEATDB_FORMAT_VER  1

/** @brief 'M64C' */
#define CHEATDB_MAGIC       0x4D363443

/** @brief Version byte meaning "any revision of this game code". */
#define CHEATDB_ANY_VERSION 0xFF

/** @brief One toggleable cheat: a name and a contiguous run of code lines. */
typedef struct {
    char    *name;
    uint16_t first;      /**< index into the loaded code array */
    uint16_t count;      /**< lines, never zero */
    bool     enabled;
} cheat_group_t;

/** @brief One Datel line, exactly as the engine wants it. */
typedef struct {
    uint32_t address;
    uint32_t value;
} cheat_code_t;

/** @brief Everything loaded for the currently selected game. */
typedef struct {
    cheat_group_t *groups;
    int            group_count;
    cheat_code_t  *codes;
    int            code_count;
    char          *strtab;      /**< one allocation backing every group name from the database */
    /** Names of hand-entered groups, which cannot point into the database's table. Owned here and
     *  freed by cheatdb_free() so callers have one release function, not two. */
    char          *user_strtab;
    /** Index of the first hand-entered group, so usercheats.c can find its own names again after
     *  a realloc. Equal to group_count when there are none. */
    int            user_first;
} cheatset_t;

/** @brief Open the database. Returns false when there is no usable file, which is not an error. */
bool cheatdb_open (const char *storage_prefix);

/** @brief Close the handle and free the index. */
void cheatdb_close (void);

/** @brief True once cheatdb_open() has succeeded. */
bool cheatdb_available (void);

/** @brief How many games the database covers, for the settings screen. */
int cheatdb_game_count (void);

/**
 * @brief Does this game have cheats? Cheap -- an index binary search, no file read.
 *
 * Used at scan time to set LIBF_HAS_CHEATS so the detail sheet can offer the row without a
 * per-game file read.
 */
bool cheatdb_has (uint64_t check_code, const char *game_code, uint8_t version);

/**
 * @brief Load one game's groups and codes.
 *
 * Matches on check_code first, then (game_code, version), then (game_code, any). The check code
 * is the strongest key the header offers; the fallbacks exist because a corpus keyed by filename
 * cannot always supply one.
 *
 * @return true on a hit, with @p out populated. Free with cheatdb_free().
 */
bool cheatdb_load (uint64_t check_code, const char *game_code, uint8_t version, cheatset_t *out);

/** @brief Release a set loaded by cheatdb_load(). Safe on a zeroed struct. */
void cheatdb_free (cheatset_t *set);

/**
 * @brief Flatten every ENABLED group into the engine's address/value array.
 *
 * Emits each enabled group's lines contiguously and in order, then two trailing zero words,
 * which is how boot/cheats.c knows the list has ended.
 *
 * @param out     destination, at least (2 * total lines + 2) words
 * @param out_cap capacity in words
 * @return words written including the terminator, or 0 if nothing is enabled or it did not fit
 */
size_t cheatdb_emit (const cheatset_t *set, uint32_t *out, size_t out_cap);

/** @brief Total lines across all groups, for sizing an emit buffer. */
int cheatdb_total_lines (const cheatset_t *set);

#endif /* CHEATS_CHEATDB_H__ */
