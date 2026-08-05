/**
 * @file parental.h
 * @brief A code on the games a parent picks, and optionally on the hours.
 * @ingroup menu
 *
 * ## What this is, and what it is not
 *
 * It is a lock on a menu, defeated by anyone who can put the SD card in a computer, hold Reset
 * with a different cart, or work through 4,096 combinations at a few seconds each. It is meant to
 * stop a seven-year-old from starting a game they were told not to start. Nothing here should be
 * described to a user as security, and nothing else in the menu may come to depend on it.
 *
 * ## Locked, never hidden
 *
 * A locked game stays in the grid, keeps its art, and shows a padlock. Pressing A on it asks for
 * the code. Hiding it was considered and rejected: hiding is a filter, and a filter touches every
 * tab view, the position counter, Recent, Favourites and the opening-tab logic, while locking is
 * one predicate at one call site. It is also worse for the user -- a game that vanished is a
 * support question, a game with a padlock explains itself.
 *
 * ## Where the state lives, and the asymmetry in it
 *
 * The code and the schedule are **settings**, written by `ini_save()` -- upstream's own writer,
 * exercised for years. The per-game locks are **playstate flags**, written through
 * `src/library/cache.c`, which has never run against real storage (AUDIT 1r). So on a card the
 * menu cannot write, the code survives a reboot and the locks do not.
 *
 * That asymmetry matters more here than it does for favourites, which share the same exposure: a
 * lost favourite is an annoyance, a lost lock is the feature quietly not working. The parental
 * screen therefore says so on screen when `cache_writable()` is false, rather than accepting a
 * lock it knows it is about to drop.
 *
 * ## The clock
 *
 * The schedule needs an RTC and the M64's is unverified (see `play_timestamp()` in
 * screen_launch.c). With no clock the window **fails open** and the screen says the schedule
 * cannot be enforced. A time lock that failed closed on a dead battery would brick the menu for
 * a family that never asked for one.
 */

#ifndef MENU_PARENTAL_H__
#define MENU_PARENTAL_H__

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "menu/settings.h"

/** @brief Presses in a code. Four, because a parent has to key it in front of a child. */
#define PARENTAL_CODE_LEN 4

/**
 * @brief The buttons a code may be made of.
 *
 * Eight, and deliberately not the whole controller. B is excluded so it can always mean "delete
 * that press" without a mode; Start is excluded because it is the button that opens Settings and
 * a code beginning with it would be entered by accident. The four C directions and the four
 * remaining face/shoulder buttons are all unambiguous on the glyph row -- the D-pad is left out
 * because a D-pad arrow and a C arrow are the same picture.
 */
typedef enum {
    PBTN_A = 0,
    PBTN_Z,
    PBTN_L,
    PBTN_R,
    PBTN_CUP,
    PBTN_CDOWN,
    PBTN_CLEFT,
    PBTN_CRIGHT,
    PBTN_COUNT,
} parental_btn_t;

/** @brief Single-character glyph for @p btn, as ui_button() wants it. */
const char *parental_glyph (int btn);

/** @brief Controller colour for @p btn: A is blue, C is yellow, the shoulders are the shell. */
uint16_t parental_colour (int btn);

/** @brief Is a code set at all? Everything else here is inert when this is false. */
bool parental_code_set (const settings_t *s);

/**
 * @brief Store @p digits as the code, or clear it when @p digits is NULL.
 *
 * Stored as a hash, not as the presses. It is not a meaningful defence -- 4,096 candidates fall
 * to a four-line script -- but the realistic attacker here is a child who opens config.ini in
 * Notepad, and `parental_code = 3F2A91C4` tells them nothing where `parental_code = AZLR` tells
 * them everything.
 */
void parental_code_store (settings_t *s, const uint8_t *digits);

/** @brief Does @p digits (PARENTAL_CODE_LEN of them) match the stored code? */
bool parental_code_matches (const settings_t *s, const uint8_t *digits);

/** @brief Why a launch was refused. */
typedef enum {
    PARENTAL_ALLOW = 0,
    PARENTAL_GAME_LOCKED,
    PARENTAL_OUTSIDE_HOURS,
} parental_verdict_t;

/**
 * @brief May this game start right now?
 *
 * @p flags is the library record's flags; @p now is app->now. Always PARENTAL_ALLOW when no code
 * is set, so the whole feature costs one comparison until a parent turns it on.
 */
parental_verdict_t parental_check (const settings_t *s, uint16_t flags, time_t now);

/** @brief Is there a clock the schedule can be enforced against? */
bool parental_clock_ok (time_t now);

/**
 * @brief Is @p hour inside the allowed window?
 *
 * Split out and named because the interesting case is the ordinary one: a parent sets 07:00 to
 * 20:00 and the naive `h >= from && h < to` works, then sets 20:00 to 07:00 for a bedtime rule
 * and that expression is false for every hour of the day.
 */
bool parental_hour_allowed (int hour, int from, int to);

/** @brief "7 am to 8 pm" into @p buf, for the settings row. */
void parental_window_text (const settings_t *s, char *buf, int cap);

/** @brief "8 pm" into @p buf. Twelve-hour, because the row is read by a parent, not by a log. */
void parental_hour_text (int hour, char *buf, int cap);

#endif /* MENU_PARENTAL_H__ */
