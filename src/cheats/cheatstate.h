/**
 * @file cheatstate.h
 * @brief Which cheats the user ticked, remembered across boots.
 * @ingroup cheats
 *
 * `menu/cache/cheatstate.dat`: one 16-byte row per enabled group, keyed by the game and by a hash
 * of the group's **name**.
 *
 * The name, not the index. `cheats.db` is a release artifact regenerated from a corpus that gains
 * and loses entries between versions, so a game's third cheat today is not its third cheat after
 * the next corpus refresh. Keying on the index would silently re-point every selection the user
 * ever made -- and a cheat engine that patches a different address than the one the user asked
 * for is the exact failure mode AUDIT.md 2.2 exists to record. Hashing the name means a renamed
 * cheat is forgotten, which is visible and harmless, instead of being confused with another one.
 *
 * Only enabled groups are stored. A game with nothing ticked occupies no rows, so the file tracks
 * what the user actually did rather than the size of the database.
 */

#ifndef CHEATS_CHEATSTATE_H__
#define CHEATS_CHEATSTATE_H__

#include <stdbool.h>
#include <stdint.h>

#include "cheatdb.h"

/** @brief 'M64S' */
#define CHEATSTATE_MAGIC 0x4D363453

/** @brief Read cheatstate.dat into memory. Called once at boot; absent is normal. */
void cheatstate_load (void);

/** @brief Free the in-memory table. */
void cheatstate_free (void);

/**
 * @brief Apply remembered selections to a freshly loaded @p set for @p game_key.
 *
 * Called when the detail sheet opens a game's cheats. Groups whose names are not remembered are
 * left disabled.
 *
 * @return how many groups were re-enabled.
 */
int cheatstate_apply (cheatset_t *set, uint64_t game_key);

/**
 * @brief Replace everything remembered for @p game_key with what @p set currently has ticked.
 *
 * Called when the cheats screen closes. Marks the table dirty; does not write.
 */
void cheatstate_capture (const cheatset_t *set, uint64_t game_key);

/** @brief Write the table if it changed and storage allows it. */
bool cheatstate_save (void);

/** @brief Has anything changed since the last successful save? */
bool cheatstate_dirty (void);

#endif /* CHEATS_CHEATSTATE_H__ */
