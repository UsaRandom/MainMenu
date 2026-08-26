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

/**
 * @brief How far below the scan root a directory can be and still be indexed.
 *
 * Five, not four. The scan root moved from `/roms` to `/`, so everything on the card is one level
 * further down than it was; four here would have quietly shortened the reach of a card organised
 * exactly as before.
 *
 * In the header rather than in library.c because libindex.c's signature walk has to stop in the
 * same place. It stopped one level deeper for as long as both existed, which cost only a pointless
 * rescan -- until incremental revalidation, where it would have indexed games out of a directory
 * a full scan never reaches. See SIG_MAX_DEPTH.
 */
#define LIBRARY_SCAN_MAX_DEPTH  5

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
    /** Display name was typed by the user and lives in `<rom>.ini`. Collision must not overwrite
     *  it, and clearing it reverts to header / meta / filename. Persisted in the index flags so a
     *  warm boot does not need to re-open the sidecar to know the name is theirs. */
    LIBF_CUSTOM_TITLE = (1 << 6),
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
    /** What the scan or the user assigned, before collisions are applied. Header, homebrew
     *  meta.name, filename, or a typed override. This is what library.idx stores as title_off.
     *  Display is @ref title, computed by library_finish(). */
    char    *given;
    char    *title;             /**< display title, heap; do not persist, recompute */
    /** Where this record's art was found, resolved once and remembered. NULL until the first
     *  resolve. Caching it is not an optimisation for its own sake: art can now be found five
     *  ways, and re-walking that list on every pass is exactly the mistake that once cost 180
     *  filesystem probes and 6,437 us a frame. */
    char    *art_file;

    uint8_t  art_state;         /**< art_state_t */
    /** Which of the three tile shapes this record's cover snapped to, or #ART_KIND_UNKNOWN.
     *
     *  Read off the image header once and then persisted in library.idx, because it is the
     *  destination size of the decode and the height of the row -- both of which are needed
     *  before the art exists. A record with no art keeps UNKNOWN forever and falls back to its
     *  system's shape, which is the right answer for a tile that will only ever be a plate with
     *  a name on it. See library/boxart.h. */
    uint8_t  art_kind;
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
    int   seq;                  /**< scan order; breaks ties so the shallowest duplicate wins */
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
    /** True once the art table is sorted by key and deduplicated, which is what makes
     *  library_find_art() a bsearch instead of a walk. Cleared by every push. */
    bool art_sorted;

    /** The fullest directory the scan or the signature walk saw, and how many entries it had.
     *
     * FatFs resolves every path by walking the directory linearly, so a flat folder of several
     * hundred files is the scan's real cost -- not the header reads. The plate and Settings
     * mention it once it crosses CARDSTAT_BUSY_WARN. */
    int  dir_busiest;
    char dir_busiest_name[40];

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
 * @brief Join a directory and a name the way the scan always has. Shared with libindex.c.
 *
 * Inserts a slash even when @p dir already ends in one, so `sd:/` + `roms` is `sd://roms`.
 * That doubled separator is what library.idx and thumbs.pak are keyed on. Do not collapse it.
 */
void library_join_child (char *out, size_t cap, const char *dir, const char *name);

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

/**
 * @brief Called during library_scan() so the caller can show that something is happening.
 *
 * @param found  Titles indexed so far. There is no total to divide by -- the scan discovers the
 *               tree as it walks it -- so this is a count that climbs, not a fraction. The boot
 *               plate already prints "<n> TITLES", which turns out to be exactly the right
 *               readout for a scan with no denominator.
 *
 *               **-1 means "no count to report, just tick".** libindex.c's revalidation walk uses
 *               it: that walk finds no titles at all, it only fingerprints directories, but it is
 *               the longest blocking call at boot and it needs the callback for the same reason
 *               everything else does. An implementation must keep whatever count it last had
 *               rather than printing zero, or the plate counts back down to nothing every time
 *               the index is written. libindex_load() therefore calls once with the cached count
 *               before the walk starts.
 *
 * Called often (once per directory entry), so an implementation that draws MUST throttle itself.
 * It also runs inside a blocking call with the main loop suspended above it, which means nothing
 * else is feeding the mixer -- see the sound_poll() in screen_launch.c's on_progress for what an
 * unfed AI does on this console.
 */
typedef void (*library_scan_progress_t)(int found);

/** @brief Free the library and every string it owns. */
void library_free (library_t *lib);

/**
 * @brief Drop every record and every noticed image, leaving an empty library the caller can fill.
 *
 * For abandoning a half-built library. libindex.c's incremental repair pushes records before it
 * can be certain of finishing, and if it gives up part way the caller runs a full scan into the
 * same library -- which without this would append to what was already there and produce a grid
 * showing some games twice.
 */
void library_clear (library_t *lib);

/**
 * @brief Does @p name look like art? `.png`, `.jpg` or `.jpeg`, case-insensitively.
 *
 * One function rather than one per walker, and that is the point of exporting it. libindex.c's
 * signature walk now fills the same loose-art table the scan does, and if the two disagreed about
 * what counts as an image the table would depend on which walk produced it -- a cover found on a
 * cold boot and missing on a warm one, with nothing to say why.
 */
bool library_is_art_name (const char *name);

/**
 * @brief Remember a loose image under its own bare name. First one seen for a key wins.
 *
 * Exported for libindex.c. On an incremental revalidation most of the card is never scanned, so
 * the scan is no longer the only thing that can fill this table -- and it has to be filled for
 * ALL directories, not just the rescanned ones, or a game added to one folder could not find a
 * cover that lives in another. The signature walk is already reading every filename on the card
 * for the staleness check, so noticing the images among them costs one string compare each.
 */
void library_art_note (library_t *lib, const char *name, const char *full_path);

/** @brief Sort records into display order. Both the scan and the index merge end with this. */
void library_sort (library_t *lib);

/**
 * @brief Apply collision display titles, then sort.
 *
 * Among records without #LIBF_CUSTOM_TITLE, a given name that appears more than once is replaced
 * on screen by the filename (region tags and all). Unique headers stay. Must run on the assembled
 * library, not per directory: incremental repair only rescans the folder that moved.
 *
 * Call after a scan, an index load, an incremental merge, and a rename.
 *
 * A title whose display string did not change keeps its allocation. A rename that used to
 * free and strdup every record now touches only the ones the collision pass actually moved.
 */
void library_finish (library_t *lib);

/**
 * @brief Filename without directory or extension. Keeps `(U)`, `[!]` and the rest.
 *
 * NULL @p path or a trailing slash returns NULL. The caller owns the result.
 */
char *library_title_from_path (const char *path);

/**
 * @brief Index of the record whose path is @p path, or -1.
 *
 * library_finish() sorts, so a rom_id taken before a rename is stale. Look the game up again.
 */
int library_find_path (const library_t *lib, const char *path);

/**
 * @brief Set or clear a typed display name.
 *
 * Non-empty @p name writes `[menu] display_name` to `<rom>.ini` and sets #LIBF_CUSTOM_TITLE.
 * Empty or NULL deletes the key and re-derives the given name from the header / meta / filename.
 * Then library_finish(). False if the sidecar could not be written (read-only card).
 */
bool library_set_title (library_t *lib, lib_record_t *rec, const char *name);

/** @brief Note that a record changed and the on-disk index is now behind. */
void library_touch (library_t *lib);

/**
 * @brief Remember @p dir if it is the fullest seen so far.
 *
 * Called from the scan and from the signature walk, which visit the same tree. The name stored
 * is the last path component, so Settings can say "278 files in roms" without printing a
 * storage prefix.
 */
void library_note_dir (library_t *lib, const char *dir, int entries);

/**
 * @brief Walk @p root recursively and index every ROM found.
 *
 * Reuses rom_config_load() unchanged for N64 titles: one 4 KB header read per file, byte-swap,
 * then the ~450-entry ares database. That single 4 KB read is why a large scan is minutes
 * rather than hours.
 *
 * @param on_progress  Optional; NULL for a silent scan. See library_scan_progress_t.
 *
 * @return number of records added.
 */
int library_scan (library_t *lib, const char *storage_prefix, const char *root,
                  library_scan_progress_t on_progress);

/**
 * @brief Index one directory and stop. No recursion, no sort.
 *
 * @param dir  A full path as the scan builds them -- storage prefix included, joined the same
 *             way, because libindex.c matches these against the directory signatures it stored
 *             and a differently-spelled path is a different directory to a hash.
 *
 * For incremental revalidation. The signature walk has already worked out which directories moved;
 * this indexes one of them without touching the children, whose records the index still holds and
 * whose signatures still match. Records land unsorted at the end of the library, so the caller
 * calls library_finish() once after merging rather than once per directory.
 *
 * @return number of records added.
 */
int library_scan_dir (library_t *lib, const char *dir, library_scan_progress_t on_progress);

/**
 * @brief Point every record at its loose art file, using the table the scan just built.
 *
 * Call once, immediately after library_scan() and before libindex_save(), so the paths reach the
 * index on the first write rather than on the first launch.
 *
 * This is pure memory: the scan already noticed every image on the card and put it in `lib->art`,
 * so this is a hash-keyed lookup per record and no filesystem access at all. It exists because
 * `lib->art` is built ONLY by the scan -- nothing rebuilds it when the index loads instead -- so
 * a menu that boots from a warm index had no way to find art for anything. Resolving eagerly puts
 * the answers somewhere that survives, which is the index.
 *
 * Records whose art is not a loose file are left alone; the metadata-pack search needs real
 * filesystem probes and stays lazy, in thumbcache's art_resolve().
 *
 * @return how many records were given an art path.
 */
int library_resolve_loose_art (library_t *lib);

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
