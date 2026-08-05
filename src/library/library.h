/**
 * @file library.h
 * @brief The game index the grid draws from.
 * @ingroup library
 *
 * The menu presents GAMES, not files. Records are keyed on the ROM header -- check code, game
 * code, version -- which is what makes a favourite survive the user reorganising their card,
 * and what lets the ares-derived database in rom_info.c supply a real save type and feature
 * mask instead of a filename and a shrug.
 *
 * This is the M2 subset: an in-memory scan, no on-disk cache yet. Records are laid out as the
 * eventual library.idx will store them so the cache can be added underneath without the grid
 * noticing. See docs/AUDIT.md for the format plan.
 */

#ifndef LIBRARY_H__
#define LIBRARY_H__

#include <stdbool.h>
#include <stdint.h>

#include "menu/rom_info.h"

/** @brief Which console a title runs on. Tabs filter on this. */
typedef enum {
    SYS_N64 = 0,
    SYS_NES,
    SYS_SNES,
    SYS_GB,
    SYS_GBC,
    SYS_SMS,
    SYS_COUNT,
} system_t;

/** @brief Per-record flags. */
typedef enum {
    LIBF_FAVORITE = (1 << 0),
    LIBF_HAS_SAVE = (1 << 1),
    LIBF_HOMEBREW = (1 << 2),
    LIBF_NO_MATCH = (1 << 3),   /**< not found in the ROM database */
    /** Art was looked for and is not there. Persisted, because "no art" is the answer that costs
     *  the most to reach -- five resolution rules and up to three filesystem probes -- and it is
     *  the answer for every title on a card with no art pack. Without this the warm path pays the
     *  full search again on every boot for exactly the records that gain nothing from it. */
    LIBF_ART_MISSING = (1 << 4),
    /** A parent locked this game. It stays in the grid and keeps its art -- see menu/parental.h
     *  for why locked rather than hidden. Persisted, because a lock that forgot itself on the
     *  next boot is worse than no lock at all: the parent would believe it was still there. */
    LIBF_LOCKED = (1 << 5),
} lib_flags_t;

/**
 * @brief Capability bits, flattened from rom_info_t.features.
 *
 * rom_info stores these as a struct of bools plus an expansion-pak enum. The detail sheet
 * draws them as a row of chips, so a bitmask is the shape that is actually wanted; keeping the
 * struct would mean the UI reaching into rom_info's layout.
 */
typedef enum {
    CAP_CPAK      = (1 << 0),
    CAP_RPAK      = (1 << 1),
    CAP_TPAK      = (1 << 2),
    CAP_VRU       = (1 << 3),
    CAP_RTC       = (1 << 4),
    CAP_EXP_REQ   = (1 << 5),
    CAP_EXP_SUGG  = (1 << 6),
    CAP_64DD      = (1 << 7),
} capability_t;

/** @brief State of a record's title-card art. */
typedef enum {
    ART_PENDING = 0,
    ART_DECODING,
    ART_READY,
    ART_NONE,
    /** Art exists but is large enough to block the queue; decoded only once nothing cheap is
     *  left. Distinct from ART_PENDING so the size is probed once per card, not once per pass. */
    ART_COSTLY,
} art_state_t;

/** @brief One game. */
typedef struct {
    uint64_t check_code;
    char     game_code[5];      /**< NUL-terminated for printing */
    uint8_t  version;
    uint8_t  system;            /**< system_t */
    uint8_t  save_type;         /**< rom_save_type_t */
    uint16_t feat;              /**< feat_t bitmask from rom_info.c */
    uint16_t flags;             /**< lib_flags_t */

    char    *path;              /**< full path, heap */
    char    *title;             /**< display title, heap */
    /** Where this record's art was found, resolved once and remembered. NULL until the first
     *  resolve. Caching it is not an optimisation for its own sake: art can now be found five
     *  ways, and re-walking that list on every pass is exactly the mistake that once cost 180
     *  filesystem probes and 6,437 us a frame. */
    char    *art_file;

    uint8_t  art_state;         /**< art_state_t */
    /** Seconds since this tile's art landed, or >= DUR_TILE_ARRIVAL once it has settled. Drives
     *  the arrival pop; kept per record rather than per cache slot so a tile that scrolls off and
     *  back does not replay it. */
    float    art_age;
    uint16_t dominant;          /**< RGBA5551, drives the ambient wash; 0 until art loads */

    uint32_t last_played;
    uint32_t play_count;
} lib_record_t;

/** @brief One loose PNG noticed during the scan, keyed by its own bare name. */
typedef struct {
    char *key;                  /**< basename with the extension removed, lowercased */
    char *path;                 /**< full path, heap */
} art_entry_t;

/** @brief The whole index. */
typedef struct {
    lib_record_t *records;
    int count;
    int capacity;

    /** Every PNG seen during the same directory walk that found the ROMs. Noticing them costs
     *  one extension compare per file rather than a search per title, which is the only reason
     *  "art can live anywhere" is affordable at all -- a probe per candidate directory per game
     *  would be hundreds of stats on a cold FatFs. */
    art_entry_t *art;
    int art_count;
    int art_capacity;

    /** Set when a record has changed in a way the index does not yet know about.
     *
     * This exists because of an ordering problem: libindex_save() runs straight after the scan,
     * but art resolution is lazy and happens later, per tile, as the grid asks for them. So the
     * index written at boot records no art paths at all, and the search -- five rules, up to
     * three filesystem probes, and the AUDIT's 180-probe cautionary tale -- would be repeated in
     * full on every single boot forever. The flag lets the index be rewritten on the way out
     * once the answers are actually known. */
    bool dirty;
} library_t;

/**
 * @brief Look up a loose art file by bare name, case-insensitively.
 *
 * @p name is matched against the PNG's own filename minus its extension, so both a game code
 * ("NGEE.png") and a copy of the ROM's name ("Super Mario World (U) [!].png") resolve through
 * the same table. Returns NULL when nothing matches.
 */
const char *library_find_art (const library_t *lib, const char *name);

/**
 * @brief Tabs, in rail order. All are always shown, empty or not.
 *
 * Recent leads, because the overwhelmingly common reason to open a launcher is to carry on with
 * the thing you were already playing, and that should cost no navigation at all. Favourites is
 * second: deliberate, but a smaller set and a less frequent intent than "again".
 *
 * There is no Most Played. It was a third ranking of the same handful of games, and a tab whose
 * contents are almost always a permutation of the two beside it is a tab that costs rail width
 * and decision time to tell you nothing new. play_count is still recorded and still shown on the
 * detail sheet -- the statistic was worth keeping, the tab was not.
 */
typedef enum {
    TAB_RECENT = 0,
    TAB_FAVORITES,
    TAB_N64,
    TAB_NES,
    TAB_SNES,
    TAB_GB,
    TAB_GBC,
    TAB_SMS,
    TAB_COUNT,
} tab_t;

/** @brief Short label for @p tab, uppercase per the chrome casing rule. */
const char *library_tab_label (tab_t tab);

/** @brief Allocate an empty library. */
library_t *library_init (void);

/**
 * @brief Append a zeroed record and return it, growing the array. NULL if out of memory.
 *
 * Exposed for libindex.c, which fills the library from a cache file rather than from a scan.
 * Both paths must grow the array the same way or the two produce differently-shaped libraries.
 */
lib_record_t *library_push (library_t *lib);

/**
 * @brief Join a storage prefix and an absolute root without doubling the separator.
 *
 * Shared with libindex.c so the directory signatures are taken over exactly the path the scan
 * walked. See the comment in library_scan() for what the doubled separator cost last time.
 */
void library_join (char *out, size_t cap, const char *storage_prefix, const char *root);

/**
 * @brief Is @p name a directory the scan refuses to enter?
 *
 * Exported for libindex.c, which fingerprints the same tree to decide whether the index is still
 * good. The two walks MUST agree on what they are looking at: a signature that recursed into a
 * directory the scan skips would fire on changes the index does not contain, and `mainmenu/cache`
 * changes on every boot -- so the index would be declared stale every time and the full scan the
 * index exists to avoid would run for ever. Under ares that cannot happen, because the DFS is
 * read-only and nothing under mainmenu/ ever moves.
 */
bool library_scan_skipped (const char *name);

/** @brief Free the library and every string it owns. */
void library_free (library_t *lib);

/** @brief Note that a record changed and the on-disk index is now behind. */
void library_touch (library_t *lib);

/**
 * @brief Walk @p root recursively and index every ROM found.
 *
 * Reuses rom_config_load() unchanged for N64 titles: one 4 KB header read per file, byte-swap,
 * then the ~450-entry ares database. That single 4 KB read is why a large scan is minutes
 * rather than hours.
 *
 * @return number of records added.
 */
int library_scan (library_t *lib, const char *storage_prefix, const char *root);

/**
 * @brief Fill @p out with the record indices belonging to @p tab, in display order.
 *
 * A view, not a copy: one pass over the records plus a sort of at most @p cap shorts. At 500
 * titles that is tens of microseconds, so there is nothing worth precomputing or persisting.
 *
 * @return number of indices written.
 */
int library_tab_view (const library_t *lib, tab_t tab, uint16_t *out, int cap);

#endif /* LIBRARY_H__ */
