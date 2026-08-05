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
extern const screen_t SCREEN_CLOCK_DEF;
extern const screen_t SCREEN_CODE_DEF;
extern const screen_t SCREEN_LAUNCH_DEF;
extern const screen_t SCREEN_FAULT_DEF;

/** @brief Populate @p table, indexed by screen_id_t. */
void screens_register (const screen_t **table);

/** @brief What the code pad is being asked for. */
typedef enum {
    CODE_ASK_UNLOCK = 0,   /**< prove you know the code; one entry */
    CODE_ASK_SET,          /**< choose a new one; entered twice and compared */
    CODE_ASK_CLEAR,        /**< prove you know it, then remove it */
} code_ask_t;

/**
 * @brief Arm the code pad, then `app_goto(app, SCREEN_CODE)`.
 *
 * The request lives in screen_code.c's own statics rather than in app_t, because it is the
 * caller's business for exactly as long as the pad is up and nothing else ever reads it. @p ok
 * and @p cancel are where the pad navigates on success and on B.
 */
void screen_code_ask (code_ask_t what, const char *prompt, screen_id_t ok, screen_id_t cancel);

#endif /* SCREENS_H__ */
