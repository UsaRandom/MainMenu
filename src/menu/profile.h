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
#include <stdint.h>
#include <stddef.h>

#include "library/library.h"

/** @brief Ten players. The picker is a 5 x 2 grid of cards and this is what fits on it. */
#define PROFILE_MAX         10

/**
 * @brief The closed palette a profile's plate and artwork both pick from.
 *
 * Nine: seven hues, then black and white. Slots default to one of the first #PROFILE_PLATES,
 * because a black or white plate reads as an empty card -- but both neutrals are choosable, and
 * white-on-red is the reason this is one list rather than two.
 *
 * It was ten. The handoff's eighth swatch was a bone white that quantised to three levels off the
 * pure one in RGBA5551, which is a palette with two whites in it; see the SWATCH table.
 */
#define PROFILE_COLOURS     9
#define PROFILE_PLATES      7       /**< the hues, which defaults come from */
#define PROFILE_COLOUR_INK    7     /**< the dark neutral */
#define PROFILE_COLOUR_PAPER  8     /**< the light neutral */

/**
 * @brief `profiles.ini` format.
 *
 * 1: `count` profiles in slots 0..count-1, no holes. Deleting one moved every profile above it
 *    down a slot -- and since the slot number names `saves/pN/`, that handed one player's saves
 *    to another. Still read, and read without renumbering anything; see profile_load().
 * 2: every slot carries its own `used` flag, so a deleted slot stays empty and nobody inherits a
 *    folder. Adds `icon`, `colour` (the plate) and `ink` (the artwork on it). A file with no
 *    `ink` key -- which is every file written before the two could differ -- takes
 *    #profile_default_ink, the pairing that used to be hardcoded, so nothing changes appearance
 *    on upgrade. No version bump for that: the key is additive and its absence has a defined
 *    meaning, which is the whole reason it was given one.
 */
#define PROFILE_VERSION     2

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

/**
 * @brief Longest string #profile_name can return, including the terminator.
 *
 * Ten, not #PROFILE_NAME_CAP's nine, and the extra character is entirely "Player 10". A typed name
 * is eight characters; the unnamed fallback for the last slot is nine, so anything that reserves
 * space for a name has to reserve for nine or slot 10 gets clipped. It shared the cap once and the
 * fallback came out as "Player 1" -- the same string slot 1 shows.
 */
#define PROFILE_LABEL_CAP   10

/** @brief Load `profiles.ini` and select the active profile. Absent is normal: one nameless profile. */
void profile_load (const char *storage_prefix);

/** @brief Write `profiles.ini`, if storage allows it. */
void profile_save (void);

/** @brief How many profiles exist. Always at least 1. */
int profile_count (void);

/** @brief Index of the profile in use, 0..PROFILE_MAX-1. */
int profile_active (void);

/**
 * @brief Is slot @p index occupied?
 *
 * Slots are stable and the roster can have holes, so this is what the picker draws its "+ Empty"
 * cards from and what any loop over profiles must test. Iterating to profile_count() was correct
 * under format 1 and is wrong now: three profiles in slots 0, 1 and 4 make profile_count() 3, and
 * a loop bounded by it stops before the third.
 */
bool profile_slot_used (int index);

/** @brief The icon @p index wears, or #ICON_NONE if they have not chosen one. */
uint16_t profile_icon (int index);

/** @brief Set the icon. Not saved until profile_save(). */
void profile_set_icon (int index, uint16_t icon);

/** @brief The plate @p index wears: a #PROFILE_COLOURS index. */
int profile_plate (int index);

/** @brief Set the plate. Out-of-range values are ignored rather than clamped. */
void profile_set_plate (int index, int colour);

/** @brief The colour the artwork on that plate is drawn in. Also a #PROFILE_COLOURS index. */
int profile_ink (int index);

/** @brief Set the artwork colour. Out-of-range values are ignored rather than clamped. */
void profile_set_ink (int index, int colour);

/** @brief The RGBA5551 value of palette entry @p colour. */
uint16_t profile_colour_fill (int colour);

/** @brief What palette entry @p colour is called -- "Red", "Black". Never NULL. */
const char *profile_colour_name (int colour);

/**
 * @brief The artwork colour that stays legible on plate @p plate.
 *
 * The pairing the swatch table used to hardcode -- amber, green, cyan, pink and white take black
 * artwork; red, blue, purple and black take white. It is now a *default* rather than a rule: it is
 * what a new slot gets and what a card written before the two could differ is read as, and the
 * user can then pick anything.
 */
int profile_default_ink (int plate);

/**
 * @brief Which slot already wears (@p icon, @p plate, @p ink), ignoring @p except. -1 if none.
 *
 * The whole appearance has to be unique, so two people are never the same card in the grid; the
 * sprite alone may be shared, and so may a colour. Checked when an appearance is applied rather
 * than by hiding taken combinations, because a cell the cursor cannot land on is a worse answer
 * than a refusal that says who has it.
 *
 * An unchosen icon never collides -- ten slots with #ICON_NONE are not ten conflicts.
 */
int profile_appearance_owner (uint16_t icon, int plate, int ink, int except);

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
 * @brief Fill a specific empty slot, returning false if it is out of range or already taken.
 *
 * The picker offers both this and #profile_add. Pressing A on a particular empty card should fill
 * *that* card -- the card being pressed is the promise being made -- while START is for somebody
 * who has not thought about slots and just wants another player.
 *
 * The new slot gets a colour and a starter face immediately, by slot number, so it is never a
 * blank panel and ten of them are never the same one.
 */
bool profile_add_at (int slot);

/**
 * @brief Empty slot @p index, leaving the slot itself where it is.
 *
 * **Nothing above it moves.** That is the change format 2 exists for. Removing a profile used to
 * close the gap by shifting every profile above it down a slot -- and the slot number is what
 * names the folder on disk, so deleting player 2 turned player 3's `saves/p3/` into player 2's.
 * The saves followed the slot rather than the person, which is a thing a menu should never do and
 * which took a paragraph of on-screen confirmation to excuse.
 *
 * Now slot 2 stays empty and keeps its number forever. The next player to fill it gets an empty
 * folder, because #profile_erase_saves has already run.
 *
 * This call removes the profile's own bookkeeping -- its playstate and cheat selections. It does
 * **not** touch saves; that is #profile_erase_saves, deliberately separate so the irreversible
 * half is explicit at the call site.
 *
 * Profile 1 cannot be removed, because something has to own the unsuffixed paths. Returns false
 * if asked to.
 */
bool profile_remove (int index);

/**
 * @brief Delete the saves belonging to @p index, across every directory the library knows.
 *
 * Separate from profile_remove() on purpose. Removing a name from a list and destroying somebody's
 * saved games are different sizes of decision, and the screen has to have confirmed the second
 * before this is reached -- so it is its own call at its own call site rather than a side effect.
 *
 * Saves live beside their ROM rather than in one place, so the set of directories to visit is the
 * set the library already walked. Taking @p lib rather than walking the card again is both faster
 * and narrower: a second walk would also find `saves/` trees under directories holding no game,
 * and guessing is not a thing to do while deleting.
 *
 * Profile 1 is refused. Its saves are the unsuffixed `saves/`, which on a card that predates
 * profiles is every save on it.
 *
 * There is no dry-run mode any more. It existed to put a save count in the confirmation dialog,
 * and pricing that number honestly killed it: the count is this same walk -- one FatFs probe per
 * library record over the SC64 -- so the dialog was paying the whole cost of the deletion just to
 * describe it, as a music-stopping stall between the press and the popup. The dialog now warns
 * unconditionally and the walk runs once, when the person has actually said delete.
 *
 * @param tick  called once per record and once per file inside each folder, or NULL. This walk is
 *              hundreds of serial card round-trips; the caller's tick is what keeps the mixer fed
 *              and the "Deleting..." frame moving through them.
 * @return files removed
 */
int profile_erase_saves (int index, const library_t *lib, void (*tick)(void));

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
