/**
 * @file cheatcheck.h
 * @brief Will the cheat engine actually hook this game?
 * @ingroup menu
 *
 * The cheat engine installs itself by overwriting one instruction in the game's IPL3, at an
 * offset that depends on the CIC. Before writing it checks that the word really is `jr $t1`,
 * because a ROM can be signed with a retail CIC seed without having the retail layout. If that
 * check fails the engine is never hooked and every ticked cheat silently does nothing.
 *
 * ## Why this is a separate thing from boot/cheats.c
 *
 * That check runs inside `boot()` -- after the display is closed, after the filesystem is
 * unmounted, microseconds before jumping into the cartridge. There is nothing left to draw on and
 * nothing left to write to. A failure there is, by construction, invisible.
 *
 * So the same question is asked here instead, while the menu is still alive: read the ROM's first
 * 4 KB, run the real `cic_detect()`, and put the word at the real patch offset through the real
 * `cheats_ipl3_layout_ok()`. Nothing is reimplemented -- if this and the console ever disagreed,
 * this would be worse than useless.
 *
 * The answer goes two places: onto the launch screen, so a player is told before they commit, and
 * into `/mainmenu/launch.log`, because this cart's USB port does not work and a file on the card
 * is the only diagnostic channel left. See launchlog.h.
 */

#ifndef MENU_CHEATCHECK_H__
#define MENU_CHEATCHECK_H__

#include <stdbool.h>
#include <stddef.h>

/** @brief What a pre-flight concluded. */
typedef enum {
    CHEATFIT_OK = 0,        /**< the engine will hook; cheats will run */
    CHEATFIT_UNREADABLE,    /**< the ROM could not be read; nothing can be said */
    CHEATFIT_UNKNOWN_CIC,   /**< no patch offset for this CIC -- the engine declines, correctly */
    CHEATFIT_BAD_LAYOUT,    /**< the CIC is known but the IPL3 is not the expected one */
} cheatfit_t;

/**
 * @brief Decide whether the engine can hook @p rom_path.
 *
 * @p detail, if non-NULL, receives a short human-readable reason -- the CIC name and the word
 * that was found -- suitable for a footer line and for the log.
 */
cheatfit_t cheatcheck_rom (const char *rom_path, char *detail, size_t cap);

/** @brief One line for a player, or NULL when there is nothing worth saying. */
const char *cheatcheck_message (cheatfit_t fit);

#endif /* MENU_CHEATCHECK_H__ */
