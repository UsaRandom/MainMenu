/**
 * @file profile.h
 * @brief Who is playing, and what that changes.
 * @ingroup menu
 *
 * One console, one card, several people. A profile carries the three things that belong to a
 * person rather than to the machine: their favourites and play history, which cheats they leave
 * switched on, and their saves.
 *
 * ## What a profile is not
 *
 * Not a login and not a lock. Anyone can pick any profile off the boot screen with one press --
 * profiles separate people who are cooperating, not people who are not. The one thing that does
 * hold people apart is the parental code, and that is deliberately **outside** this: it lives in
 * `parental.ini` and applies to every profile at once, so switching profile can never be the way
 * around a locked game or a bedtime. See parental.h.
 *
 * Volume is not here either. Music and effect levels stay in `config.ini` with the rest of the
 * device settings, because a console that gets loud again every time someone else picks up the
 * controller is a console with a volume bug, not a feature.
 *
 * ## Where the split falls
 *
 * | file | scope | why |
 * |---|---|---|
 * | `library.idx`, `thumbs.pak`/`.idx` | shared | derived from the card, which does not change with who is holding the pad. Ten copies of a 292 KB atlas would be the largest thing on the card and every byte of it identical. |
 * | `cheats.db`, `usercheats.dat` | shared | one is read-only, the other is authoring. Writing a cheat is a thing done *to the card*; enabling it is a thing done by a person. |
 * | `parental.ini` | shared | see above. |
 * | `config.ini` | shared | device settings, plus which profile is active. |
 * | `playstate.dat`, `cheatstate.dat` | **per profile** | favourites, recents, play counts, enabled cheats. |
 * | `saves/` | **per profile** | the whole point of the feature. |
 *
 * ## The first profile writes where it always did
 *
 * Profile 1 is index 0 and its files carry no suffix at all: `cache/playstate.dat` and
 * `<romdir>/saves/Game.sav`, exactly the paths a card written before this feature existed already
 * holds. Profiles 2..10 nest -- `cache/p2/playstate.dat`, `<romdir>/saves/p2/Game.sav`.
 *
 * That asymmetry is load-bearing twice over. It means upgrading a card cannot lose a save, which
 * a scheme numbering every profile (`p1saves/`, `p2saves/`) would do to every existing card on
 * the first boot. And it means a card with one profile is byte-for-byte a card from before this
 * existed, so the feature can be abandoned without stranding anyone.
 *
 * Nesting inside `saves/` rather than beside it also costs the scanner nothing:
 * `library.c`'s SCAN_SKIP already excludes that directory and everything under it. A sibling
 * `p2saves/` would be walked as if it held games.
 *
 * ## Profile 1 always exists
 *
 * There is no "no profile" state and no empty roster. A card with no `profiles.ini` has exactly
 * one nameless profile, which is the state every existing card is in, and the entire feature is
 * invisible until somebody adds a second. #profile_count() returning 1 is the signal for that:
 * the boot picker does not appear, the grid draws no chip, and Z does nothing.
 */

#ifndef MENU_PROFILE_H__
#define MENU_PROFILE_H__

#include <stdbool.h>
#include <stddef.h>

#include "library/library.h"

/** @brief Ten players. The picker is one screen of rows and this is what fits on it. */
#define PROFILE_MAX         10

/**
 * @brief Name length, including the terminator.
 *
 * Eight characters. The name is drawn in the grid's footer as the label on a Z hint, beside
 * Details, Fav and Settings, and that row has to fit inside the safe area at every theme. Eight
 * is long enough for any first name and short enough that a fourth hint cannot push the row wide.
 *
 * It was twelve when the name lived in the tab rail. That position is gone -- twelve characters
 * of name plus a spelled-out FAVORITES tab overflowed the rail and the two collided.
 */
#define PROFILE_NAME_CAP    9

/** @brief Load `profiles.ini` and select the active profile. Absent is normal: one nameless profile. */
void profile_load (const char *storage_prefix);

/** @brief Write `profiles.ini`, if storage allows it. */
void profile_save (void);

/** @brief How many profiles exist. Always at least 1. */
int profile_count (void);

/** @brief Index of the profile in use, 0..profile_count()-1. */
int profile_active (void);

/**
 * @brief What @p index is called. Never NULL.
 *
 * A profile with no name of its own reads as "Player N", so the picker never shows a blank row
 * and the roster is usable before anybody has typed anything.
 */
const char *profile_name (int index);

/** @brief The name as stored, which may be empty. For the rename editor, which must not edit "Player 2". */
const char *profile_name_raw (int index);

/** @brief Rename @p index. Truncated to PROFILE_NAME_CAP; empty restores the "Player N" default. */
void profile_set_name (int index, const char *name);

/** @brief Which theme @p index prefers, by name. Empty until they change it. */
const char *profile_theme (int index);

/** @brief Remember @p name as @p index's theme. */
void profile_set_theme (int index, const char *name);

/**
 * @brief Add a profile, returning its index or -1 if the roster is full.
 *
 * Does not switch to it. Adding somebody to the list and handing them the console are two
 * separate decisions, and the screen that does the first is usually being driven by the person
 * who wants to keep the second.
 */
int profile_add (void);

/**
 * @brief Remove @p index, closing the gap above it.
 *
 * **Saves are not touched.** Finding them all would mean walking the entire card -- they live
 * beside each ROM, not in one place -- and deleting somebody's saves because they came off a
 * family list is the one outcome here that cannot be undone. The `saves/pN/` folders are left
 * where they are, and the card guide says so. What does go is the profile's playstate and cheat
 * selections, which are this menu's own bookkeeping.
 *
 * Profile 1 cannot be removed, because something has to own the unsuffixed paths. Returns false
 * if asked to, or if only one profile is left.
 *
 * Note that removing a profile renumbers every profile above it, and the numbering is what names
 * the folders on disk -- so profile 3's `saves/p3/` becomes profile 2's after a deletion. Nothing
 * on disk moves, so the saves follow the slot rather than the person. That is why the screen
 * confirms, and why the confirmation says so in as many words.
 */
bool profile_remove (int index);

/**
 * @brief Switch to @p index: reloads playstate and cheat selections for the new profile.
 *
 * @p lib is re-keyed in place -- favourite flags and play counts are cleared off every record and
 * the new profile's are applied over the top, because those fields live on the library records
 * and the library itself is shared.
 */
void profile_activate (int index, library_t *lib);

/**
 * @brief The cache filename @p name takes for the active profile.
 *
 * `"playstate.dat"` for profile 1, `"p2/playstate.dat"` for profile 2. Callers pass the result
 * straight to cache_load()/cache_store()/cache_drop().
 */
void profile_cache_name (char *out, size_t cap, const char *name);

/**
 * @brief The subdirectory under `saves/` the active profile writes into, or NULL for profile 1.
 *
 * NULL rather than `""` so a caller cannot accidentally push an empty path component and produce
 * `saves//Game.sav`.
 */
const char *profile_save_subdir (void);

#endif /* MENU_PROFILE_H__ */
