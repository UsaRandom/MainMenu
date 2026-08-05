/**
 * @file locks.h
 * @brief Which games the parental code holds shut, for everyone at once.
 * @ingroup library
 *
 * `menu/cache/locks.dat`: one 8-byte key per locked game, keyed exactly as playstate.dat is --
 * see playstate_key(), and note that a lock therefore survives the game being moved or renamed.
 *
 * ## Why this is not in playstate.dat
 *
 * It was, and it could not stay there once profiles existed. `playstate.dat` became per-profile,
 * and `LIBF_LOCKED` rode along in the same flags word as `LIBF_FAVORITE` -- which would have made
 * the padlock per-profile too. Two presses on the boot screen and every locked game on the card
 * is open, with no code entered and nothing to notice it happened.
 *
 * The parental code is the one thing here that holds people apart rather than merely keeping
 * their stuff separate, so it and everything it decides are shared: `parental.ini` for the code
 * and the schedule, this file for the list. Switching profile changes what you like. It changes
 * nothing about what you are allowed to play.
 *
 * ## Cards written before this file existed
 *
 * They have their locks inside `playstate.dat`, and that read path is untouched -- playstate
 * still ORs `LIBF_LOCKED` in when it finds it, it simply no longer writes it back. #locks_load
 * notices the case (no `locks.dat`, but the library already has locked records) and marks itself
 * dirty, so the first save moves them across without the user doing anything and without the
 * version bump that would have cost them their favourites.
 */

#ifndef LIBRARY_LOCKS_H__
#define LIBRARY_LOCKS_H__

#include <stdbool.h>
#include <stdint.h>

#include "library.h"

/** @brief 'M64K' */
#define LOCKS_MAGIC 0x4D36344B

/**
 * @brief Apply locks.dat to @p lib. Call after playstate_load(), never before.
 *
 * The order matters for the migration above: this has to be able to see the locks playstate just
 * applied in order to decide that they need writing here.
 */
void locks_load (library_t *lib);

/** @brief Write locks.dat from @p lib's flags, if anything changed and storage allows it. */
bool locks_save (const library_t *lib);

/** @brief Mark the list dirty. Called wherever LIBF_LOCKED is flipped. */
void locks_touch (void);

/** @brief Has anything changed since the last successful save? */
bool locks_dirty (void);

#endif /* LIBRARY_LOCKS_H__ */
