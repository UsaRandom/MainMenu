/**
 * @file usercheats.h
 * @brief Cheats the user typed in, alongside the ones the database shipped.
 * @ingroup cheats
 *
 * `menu/cache/usercheats.dat`. The shipped corpus covers a few hundred N64 titles and nothing
 * else -- no NES, no SNES, no homebrew, and none of the codes published after it was built. This
 * is how a game with no entry gets one.
 *
 * ## They are groups, like everything else
 *
 * A user cheat is a **named group of up to eight lines**, not a line. That is the same constraint
 * cheatdb.h is built around and it is not negotiable here either: a `D0` conditional and the write
 * it guards only mean anything together, and somebody typing in a two-line GameShark code must not
 * end up able to enable half of it. The editor adds and removes whole lines within one group and
 * the cheats list toggles the group.
 *
 * ## Why fixed-size records
 *
 * 140 bytes each, with the name and the lines inline. Variable-length records would pack better
 * and would need a parser, and every on-disk format in this project is instead a fixed row with a
 * compile-time size assertion -- because the one failure this family of files cannot survive is a
 * silent layout change that still passes its own magic and CRC. The full table of 256 is 35 KB.
 *
 * ## Where they live in the set
 *
 * `usercheats_apply()` appends them to a `cheatset_t` after `cheatdb_load()` has filled it (or
 * after a skip, when Settings has the shipped database off), so everything downstream -- the
 * cheats list, `cheatstate` restore and capture, `cheatdb_emit()` -- treats them exactly like
 * shipped ones. `cheatstate` keys on the group NAME, so a user cheat's
 * enabled flag is remembered by the same mechanism and needs no special case.
 *
 * Their names cannot point into the database's string table, so `cheatset_t` carries a second
 * `user_strtab` that `cheatdb_free()` releases with the rest.
 *
 * ## A matching name replaces, it does not add
 *
 * **A user cheat whose name matches one already in the set takes that group over** -- the group
 * keeps its name and its place in the list, and its codes become the user's. This is what makes
 * the shipped corpus editable: plenty of published cheats are only useful once a value is changed
 * ("give item in slot"), the database is read-only, and the alternative is the original and the
 * edit sitting next to each other under the same name with nothing to tell them apart.
 *
 * Matching ignores case, because the editor's alphabet is uppercase only. Keeping the group's
 * original name is what lets `cheatstate` go on remembering the tick -- renaming an edited cheat
 * does forget it, which is the honest consequence of keying on the name.
 */

#ifndef CHEATS_USERCHEATS_H__
#define CHEATS_USERCHEATS_H__

#include <stdbool.h>
#include <stdint.h>

#include "cheatdb.h"

/** @brief 'M64W' */
#define USERCHEATS_MAGIC 0x4D363457

/** @brief Lines in one user cheat. Four is a long published code; eight is room to be wrong. */
#define USERCHEAT_MAX_LINES 8

/**
 * @brief Characters in a user cheat's name, NUL included.
 *
 * Sized by the corpus rather than by the screen. Cheat names in libretro's collection run to 197
 * characters; 63 covers 99.4% of the 228,209 of them, where 23 -- the widest the editor's cell
 * strip can show -- covers only 80.5%. Since a stored name that does not match the group it came
 * from turns an edit into a duplicate, the STORE has to hold the whole thing even when the editor
 * can only display it. See screen_cheatedit.c.
 */
#define USERCHEAT_NAME_CAP 64

/** @brief Read usercheats.dat into memory. Called once at boot; absent is normal. */
void usercheats_load (void);

/** @brief Free the in-memory table. */
void usercheats_free (void);

/** @brief Has anything changed since the last successful save? */
bool usercheats_dirty (void);

/** @brief Write the table back, if anything changed and storage allows it. */
bool usercheats_save (void);

/**
 * @brief Append @p game_key's user cheats to @p set.
 *
 * Grows the set's groups and codes arrays and allocates its `user_strtab`. Call after
 * cheatdb_load() (or a skip, when the shipped database is off), and before cheatstate_apply()
 * so the restored selections cover these too.
 *
 * @return how many groups were appended.
 */
int usercheats_apply (cheatset_t *set, uint64_t game_key);

/**
 * @brief Record a new cheat for @p game_key and append it to the live @p set.
 *
 * The set is updated in place so the cheats list shows it without a reload -- the alternative is
 * throwing away and re-reading the whole set, which would also throw away every tick the user has
 * made since opening the sheet.
 *
 * @return false if the table is full, the input is empty, or an allocation failed.
 */
bool usercheats_add (cheatset_t *set, uint64_t game_key, const char *name,
                     const cheat_code_t *lines, int line_count);

/** @brief How many user cheats exist for @p game_key. */
int usercheats_count (uint64_t game_key);

#endif /* CHEATS_USERCHEATS_H__ */
