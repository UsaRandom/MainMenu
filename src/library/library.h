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

    uint8_t  art_state;         /**< art_state_t */
    /** Seconds since this tile's art landed, or >= DUR_TILE_ARRIVAL once it has settled. Drives
     *  the arrival pop; kept per record rather than per cache slot so a tile that scrolls off and
     *  back does not replay it. */
    float    art_age;
    uint16_t dominant;          /**< RGBA5551, drives the ambient wash; 0 until art loads */

    uint32_t last_played;
    uint32_t play_count;
} lib_record_t;

/** @brief The whole index. */
typedef struct {
    lib_record_t *records;
    int count;
    int capacity;
} library_t;

/** @brief Tabs, in rail order. All are always shown, empty or not. */
typedef enum {
    TAB_FAVORITES = 0,
    TAB_RECENT,
    TAB_MOST_PLAYED,
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

/** @brief Free the library and every string it owns. */
void library_free (library_t *lib);

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
