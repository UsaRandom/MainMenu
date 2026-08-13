/**
 * @file screens.h
 * @brief Screen registry.
 * @ingroup screens
 */

#ifndef SCREENS_H__
#define SCREENS_H__

#include "app.h"

extern const screen_t SCREEN_GRID_DEF;
extern const screen_t SCREEN_DETAIL_DEF;
extern const screen_t SCREEN_CHEATS_DEF;
extern const screen_t SCREEN_CHEATEDIT_DEF;
extern const screen_t SCREEN_SETTINGS_DEF;
extern const screen_t SCREEN_PARENTAL_DEF;
extern const screen_t SCREEN_LOCKS_DEF;
extern const screen_t SCREEN_PROFILES_DEF;
extern const screen_t SCREEN_CLOCK_DEF;
extern const screen_t SCREEN_CODE_DEF;
extern const screen_t SCREEN_CREDITS_DEF;
extern const screen_t SCREEN_SYSINFO_DEF;
extern const screen_t SCREEN_KEYBOARD_DEF;
extern const screen_t SCREEN_APPEARANCE_DEF;

/** @brief Which profile the appearance editor is about to edit. Call before app_goto(). */
void screen_appearance_ask (int profile_slot);

/** @brief Which characters the keyboard offers. See screen_keyboard.c. */
typedef enum {
    KB_NAME = 0,   /**< A-Z and space. A profile name: no shift key, no digits, eight characters. */
    KB_TEXT,       /**< adds digits and marks, for cheat names, which the corpus is full of */
} kb_charset_t;

/**
 * @brief Arm the keyboard, then `app_goto(app, SCREEN_KEYBOARD)`.
 *
 * @p out is written only when the user confirms with a non-empty field, so a cancelled edit
 * leaves the old value alone without the caller having to keep a copy.
 *
 * @p initial may contain characters this charset cannot type -- a name written by the odometer
 * this replaced can hold digits. Those display and can be deleted; only input is restricted.
 */
void screen_keyboard_ask (kb_charset_t set, const char *prompt, const char *initial,
                          char *out, size_t cap, screen_id_t back);

/** @brief Characters @p set accepts before it starts refusing. */
int screen_keyboard_limit (kb_charset_t set);
extern const screen_t SCREEN_LAUNCH_DEF;

extern const screen_t SCREEN_FAULT_DEF;

/** @brief Populate @p table, indexed by screen_id_t. */
void screens_register (const screen_t **table);

/**
 * @brief Is there more than one profile, and therefore a question to ask at boot?
 *
 * app.c uses this to decide the first screen. False is the shipped state and every state a card
 * has ever been in before profiles existed, and it has to stay cheap to be true: the cost of this
 * feature for somebody who never uses it must be exactly zero presses.
 */
/**
 * @brief Draw the tab rail, from the screen that owns which tab is current.
 *
 * The picker keeps the rail on screen so somebody who pressed L one too many times can get back
 * to a tab without first picking a player. It draws through here rather than owning a copy,
 * because the rail's contents -- which tab is underlined -- are the grid's state, and a second
 * copy is a second thing to keep in step.
 *
 * @param chip_selected the rail cursor is on the player chip rather than on a tab.
 */
void screen_grid_draw_rail (app_t *app, bool chip_selected);

bool screen_profiles_needed (void);

/** @brief Mark the next entry to SCREEN_PROFILES as the boot question rather than the editor. */
void screen_profiles_ask (void);

/** @brief What the code pad is being asked for. */
typedef enum {
    CODE_ASK_UNLOCK = 0,   /**< prove you know the code; one entry */
    CODE_ASK_SET,          /**< choose a new one; entered twice and compared */
    CODE_ASK_CLEAR,        /**< prove you know it, then remove it */
    CODE_ASK_TOGGLE_LOCK,  /**< prove you know it, then flip one game's padlock */
} code_ask_t;

/**
 * @brief Arm the code pad, then `app_goto(app, SCREEN_CODE)`.
 *
 * The request lives in screen_code.c's own statics rather than in app_t, because it is the
 * caller's business for exactly as long as the pad is up and nothing else ever reads it. @p ok
 * and @p cancel are where the pad navigates on success and on B.
 */
void screen_code_ask (code_ask_t what, const char *prompt, screen_id_t ok, screen_id_t cancel);

/**
 * @brief Arm the pad to flip @p rom_id's padlock on a correct entry, then return to @p back.
 *
 * The only request that carries an *action* rather than only a destination, and it has to: the
 * screen that asked is destroyed and rebuilt on the way back, and by the time it returns its
 * cursor may be on a different game. Applying it inside the pad is what makes the action belong
 * to the game the user was looking at when they pressed the button.
 */
void screen_code_ask_toggle_lock (int rom_id, screen_id_t back);

/**
 * @brief Arm the cheat editor, then `app_goto(app, SCREEN_CHEATEDIT)`.
 *
 * @p g NULL starts an empty cheat. Otherwise the editor opens filled in from that group, whether
 * it came from the shipped database or from a previous edit; saving it writes a user cheat, which
 * takes the group over by name. See usercheats.h.
 *
 * @p codes is the set's codes array, which @p g's `first` indexes into.
 *
 * Same reasoning as screen_code_ask(): the request is the caller's business for as long as the
 * editor is up, so it lives in that screen's statics rather than in app_t.
 */
void screen_cheatedit_open (const cheat_group_t *g, const cheat_code_t *codes);

/** @brief Can @p g be represented in the editor at all? See screen_cheats.c for what happens if not. */
bool screen_cheatedit_can_edit (const cheat_group_t *g);

#endif /* SCREENS_H__ */
