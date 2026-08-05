/**
 * @file parental.h
 * @brief A code on the games a parent picks, and optionally on the hours.
 * @ingroup menu
 *
 * ## What this is
 *
 * A lock on a menu. It is meant to stop a seven-year-old from starting a game they were told not
 * to start, and it is defeated by anyone who can put the SD card in a computer. Nothing else in
 * the menu may come to depend on it.
 *
 * ## Locked, never hidden
 *
 * A locked game stays in the grid, keeps its art, and shows a padlock. Pressing A on it asks for
 * the code. Hiding it was considered and rejected: hiding is a filter, and a filter touches every
 * tab view, the position counter, Recent, Favourites and the opening-tab logic, while locking is
 * one predicate at one call site. It is also worse for the user -- a game that vanished is a
 * support question, a game with a padlock explains itself.
 *
 * ## Six presses, four buttons
 *
 * The alphabet is the four C directions and nothing else, and a code is six of them: 4^6 = 4,096,
 * which is exactly what four presses of eight buttons gave. Four buttons is simpler to explain to
 * the person who has to key it in front of a child, simpler to draw, and it frees A, B, Z and the
 * shoulders to keep meaning what they mean everywhere else in the menu. The presses are never
 * shown as glyphs -- the pad draws dots -- so it cannot be read over a shoulder either.
 *
 * ## Its own file, and why not a cache
 *
 * The code, the failure count and the schedule live in `/mainmenu/parental.ini`, alone. Two
 * reasons, and they point the same way:
 *
 * **Forgetting the code has to be recoverable, and deleting one file is the whole recovery.** No
 * master code to build, document and defend, and nothing else the parent set is lost with it.
 * That is only true if the file holds nothing else, which is why the schedule moved here too.
 *
 * **It must not go through `src/library/cache.c`.** Every file that layer writes is a *cache*: a
 * version mismatch deletes it and rebuilds from the card, which is right for art and play history
 * and catastrophic here. There is nothing to rebuild a code from, so a routine format bump would
 * silently unlock every locked game on every card in the field. It is written with `ini_save()`
 * instead -- upstream's writer, exercised for years, and the same one `config.ini` uses.
 *
 * **No code means no enforcement**, so deleting the file also releases the locked games. Their
 * `LIBF_LOCKED` flags stay in `playstate.dat` and come back into effect the moment a code is set
 * again, which is what a parent recovering their own card would want. An unreadable or truncated
 * file is treated exactly like an absent one, for the same reason: the failure mode of this
 * feature has to be "the lock is off", never "the console is unusable".
 *
 * ## What a wrong guess costs
 *
 * A count of failed entries is kept **in the file**, and each one adds five seconds to the wait
 * before the pad accepts anything, up to ten minutes. Three properties make that work:
 *
 * - **The count is written before the guess is compared.** The other order makes pulling the power
 *   on a wrong answer free, and the whole thing collapses to nothing.
 * - **A correct entry clears it.** This is what makes a counter viable with no clock: nothing has
 *   to expire, because the person who knows the code clears it every time they use it and the
 *   person who does not never clears it.
 * - **Ten minutes is a ceiling, not politeness.** Uncapped, a child who cannot get in can still
 *   leave a few hundred failures behind and the parent waits forty minutes -- the feature turned
 *   against its owner. Ten minutes still makes 4,096 combinations hopeless.
 *
 * Resetting the console is not a way round it, because the number is not in RAM; a reset re-reads
 * the file and re-arms the full wait. The countdown itself is plain accumulated frame time, so no
 * clock is involved anywhere in it.
 *
 * If storage is not writable the count cannot persist and the backoff lasts only as long as the
 * console stays on. The parental panel already says when the card cannot be written to.
 *
 * ## The clock
 *
 * The schedule needs one, and the M64's is unverified. With no clock the window **fails open** and
 * the screen says the schedule cannot be enforced. A time lock that failed closed on a dead
 * battery would brick the menu for a family that never asked for one. There is no timezone and no
 * daylight saving anywhere in this menu -- see AUDIT.md 2d.
 */

#ifndef MENU_PARENTAL_H__
#define MENU_PARENTAL_H__

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/** @brief Presses in a code. Six of four buttons is 4,096, the same space four of eight gave. */
#define PARENTAL_CODE_LEN 6

/**
 * @brief The buttons a code may be made of: the four C directions, and nothing else.
 *
 * Everything else on the pad is excluded so it can go on meaning one thing. B always deletes the
 * last press, and cancels on an empty entry. A, Z and the shoulders are left alone because the
 * detail sheet behind this screen binds all three. The D-pad is out because a D-pad arrow and a C
 * arrow are the same picture, and a code you cannot describe out loud to the other parent is a
 * code that gets written on the console.
 */
typedef enum {
    PBTN_CUP = 0,
    PBTN_CDOWN,
    PBTN_CLEFT,
    PBTN_CRIGHT,
    PBTN_COUNT,
} parental_btn_t;

/** @brief Everything the feature persists. One file, and the only one anybody deletes by hand. */
typedef struct {
    /** @brief Hash of the code, hex; empty when none is set. Never the presses themselves --
     *  `code = <>^v<^` in a file a child can open in Notepad tells them everything. */
    char code[24];

    /** @brief Consecutive wrong entries. Written before each guess is judged; cleared by a right
     *  one. See the header comment for why that order is the whole mechanism. */
    uint32_t failures;

    /** @brief Restrict launching to a window of the day. Inert without a code and without a clock. */
    bool hours_enabled;

    /** @brief The hour the window opens, 0-23. */
    int hour_from;

    /** @brief The hour it closes, 0-23. Wraps midnight when it is less than the opening hour. */
    int hour_to;
} parental_t;

/** @brief Read `/mainmenu/parental.ini`, and arm the wait the stored failure count has earned. */
void parental_load (const char *storage_prefix);

/** @brief Write it back. Silently does nothing useful on a card that cannot be written. */
void parental_save (void);

/** @brief The one instance. Mutable: the panel edits the schedule through it, then saves. */
parental_t *parental_state (void);

/** @brief Is a code set at all? Everything else here is inert when this is false. */
bool parental_code_set (void);

/** @brief Store @p digits as the code, or clear it when @p digits is NULL. Saves. */
void parental_code_store (const uint8_t *digits);

/** @brief Does @p digits (PARENTAL_CODE_LEN of them) match the stored code? */
bool parental_code_matches (const uint8_t *digits);

/**
 * @brief Count a guess about to be made, and persist it.
 *
 * Call this *before* comparing, always. It re-arms the wait from the new count, so a failed entry
 * is followed by a longer one whether or not the console stays on.
 */
void parental_note_attempt (void);

/** @brief A correct entry: clear the count, drop the wait, persist. */
void parental_note_success (void);

/** @brief Seconds still to wait before the pad will take a press. 0 when it is ready. */
float parental_wait_left (void);

/** @brief Spend @p dt of the wait. Called by the pad, which is the only screen that can be in it. */
void parental_wait_tick (float dt);

/** @brief "4 minutes", "35 seconds" -- what is left, for the pad to show. */
void parental_wait_text (char *buf, int cap);

/** @brief Single-character glyph for @p btn, as ui_button() wants it. */
const char *parental_glyph (int btn);

/** @brief Controller colour for @p btn. All four are C buttons, so all four are yellow. */
uint16_t parental_colour (int btn);

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
parental_verdict_t parental_check (uint16_t flags, time_t now);

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

/**
 * @brief How long until the window opens again, in seconds, or -1 if it is open or unknowable.
 *
 * For the detail sheet, which says "Play unlocked in 2h 15m" rather than letting the code prompt
 * be the first mention that a game will not start.
 */
long parental_seconds_until_open (time_t now);

/** @brief "7 am to 8 pm" into @p buf, for the settings row. */
void parental_window_text (char *buf, int cap);

/** @brief "8 pm" into @p buf. Twelve-hour, because the row is read by a parent, not by a log. */
void parental_hour_text (int hour, char *buf, int cap);

#endif /* MENU_PARENTAL_H__ */
