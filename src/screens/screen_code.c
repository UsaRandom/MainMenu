/**
 * @file screen_code.c
 * @brief The parental code pad: four button presses, no keyboard.
 * @ingroup screens
 *
 * Every other menu that asks for a PIN on a console draws a grid of digits and makes you steer a
 * cursor onto each one, which is slow, and which a child can read over your shoulder from across
 * the room. Here the code IS buttons: the parent presses four of A, Z, L, R and the four C
 * directions, and the screen shows filled dots rather than which ones. It cannot be shoulder-read
 * and it takes about a second.
 *
 * B is not in the alphabet, so it is always "delete the last press" -- and on an empty entry it
 * cancels. That is the whole interaction; there is no confirm button, because a four-press code
 * submits itself on the fourth press.
 *
 * Used from three places, which is why the request is armed with screen_code_ask() rather than
 * hardcoded: unlocking a game before launch, getting into the parental panel, and setting or
 * clearing the code itself.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "menu/fonts.h"
#include "menu/parental.h"
#include "menu/settings.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

#define DOT_D        28                  /**< diameter of one entered-press dot */
#define DOT_GAP      22
#define DOTS_W       (PARENTAL_CODE_LEN * DOT_D + (PARENTAL_CODE_LEN - 1) * DOT_GAP)
#define DOTS_X       ((SCREEN_W - DOTS_W) / 2)
#define DOTS_Y       220

/** How long a rejected entry stays red before the pad clears itself. */
#define WRONG_HOLD_S 0.9f

static code_ask_t   ask;
static const char  *prompt;
static screen_id_t  on_ok, on_cancel;

static uint8_t digits[PARENTAL_CODE_LEN];
static uint8_t first[PARENTAL_CODE_LEN];   /**< the first entry, while confirming a new code */
static int     count;
static bool    confirming;
static float   wrong_t;                    /**< > 0 while the rejection is being shown */
static const char *message;

void screen_code_ask (code_ask_t what, const char *p, screen_id_t ok, screen_id_t cancel) {
    ask = what;
    prompt = p;
    on_ok = ok;
    on_cancel = cancel;
}

static void code_enter (app_t *app) {
    (void)app;
    count = 0;
    confirming = false;
    wrong_t = 0.0f;
    message = NULL;
}

static void code_leave (app_t *app) {
    (void)app;
    /* Not tidiness. These are the presses the parent just made, and the screen they navigate to
     * has no business being able to read them out of a static that outlived the pad. */
    memset(digits, 0, sizeof(digits));
    memset(first, 0, sizeof(first));
    count = 0;
}

/** @brief Which alphabet button was pressed this frame, or -1. */
static int digit_pressed (const input_t *in) {
    static const struct { button_t bit; int digit; } MAP[] = {
        { BTN_A,      PBTN_A },
        { BTN_Z,      PBTN_Z },
        { BTN_L,      PBTN_L },
        { BTN_R,      PBTN_R },
        { BTN_CUP,    PBTN_CUP },
        { BTN_CDOWN,  PBTN_CDOWN },
        { BTN_CLEFT,  PBTN_CLEFT },
        { BTN_CRIGHT, PBTN_CRIGHT },
    };
    for (unsigned i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (input_pressed(in, MAP[i].bit)) {
            return MAP[i].digit;
        }
    }
    return -1;
}

/** @brief The fourth press landed. Decide what it meant. */
static void submit (app_t *app) {
    switch (ask) {
        case CODE_ASK_UNLOCK:
            if (parental_code_matches(&app->settings, digits)) {
                sound_play_effect(SFX_ENTER);
                app_goto(app, on_ok);
            } else {
                sound_play_effect(SFX_ERROR);
                message = "Not that one";
                wrong_t = WRONG_HOLD_S;
                count = 0;
            }
            break;

        case CODE_ASK_CLEAR:
            if (parental_code_matches(&app->settings, digits)) {
                parental_code_store(&app->settings, NULL);
                settings_save(&app->settings);
                sound_play_effect(SFX_ENTER);
                app_goto(app, on_ok);
            } else {
                sound_play_effect(SFX_ERROR);
                message = "Not that one";
                wrong_t = WRONG_HOLD_S;
                count = 0;
            }
            break;

        case CODE_ASK_SET:
            if (!confirming) {
                /* Twice, because there is no way to review a code made of dots. A parent who
                 * fumbles the fourth press and does not find out until their child is locked out
                 * of the console has been failed by the screen, not by themselves. */
                memcpy(first, digits, sizeof(first));
                confirming = true;
                count = 0;
                sound_play_effect(SFX_SETTING);
            } else if (memcmp(first, digits, sizeof(first)) == 0) {
                parental_code_store(&app->settings, digits);
                settings_save(&app->settings);
                sound_play_effect(SFX_ENTER);
                app_goto(app, on_ok);
            } else {
                sound_play_effect(SFX_ERROR);
                message = "Those did not match";
                wrong_t = WRONG_HOLD_S;
                confirming = false;
                count = 0;
            }
            break;
    }
}

static void code_update (app_t *app, float dt) {
    const input_t *in = &app->input;

    if (wrong_t > 0.0f) {
        wrong_t -= dt;
        /* Input is dead while the rejection shows. Not an anti-guessing measure -- it is under a
         * second -- but a press that lands mid-flash would otherwise be swallowed into a fresh
         * entry the user has not started yet. */
        return;
    }

    if (input_pressed(in, BTN_B)) {
        if (count > 0) {
            count--;
            sound_play_effect(SFX_EXIT);
        } else {
            sound_play_effect(SFX_EXIT);
            app_goto(app, on_cancel);
        }
        return;
    }

    int d = digit_pressed(in);
    if (d < 0 || count >= PARENTAL_CODE_LEN) {
        return;
    }

    digits[count++] = (uint8_t)d;
    sound_play_effect(SFX_CURSOR);

    if (count == PARENTAL_CODE_LEN) {
        submit(app);
    }
}

static void code_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "Parental code");

    const char *line = prompt;
    if (ask == CODE_ASK_SET) {
        line = confirming ? "Again, to be sure" : "Choose four buttons";
    }
    ui_label(SAFE_X, 150, SAFE_W, ALIGN_CENTER, STL_DEFAULT, line ? line : "Enter the code");

    /* Dots, never glyphs. Showing which button was pressed would make the code readable from the
     * other side of the room by the person it exists to stop. */
    for (int i = 0; i < PARENTAL_CODE_LEN; i++) {
        int x = DOTS_X + i * (DOT_D + DOT_GAP);
        if (wrong_t > 0.0f) {
            ui_fill(x, DOTS_Y, DOT_D, DOT_D, BTN_START_COLOR);
        } else if (i < count) {
            ui_fill(x, DOTS_Y, DOT_D, DOT_D, th->text_accent);
        } else {
            ui_border(x, DOTS_Y, DOT_D, DOT_D, 3, th->text_dim);
        }
    }

    if (wrong_t > 0.0f && message != NULL) {
        ui_label(SAFE_X, DOTS_Y + DOT_D + 44, SAFE_W, ALIGN_CENTER, STL_YELLOW, message);
    } else {
        /* The alphabet, drawn as the buttons themselves. A parent who has never seen this screen
         * should not have to discover by experiment that Start and B are not in it. */
        int total = PBTN_COUNT * UI_BTN_D + (PBTN_COUNT - 1) * 10;
        int x = (SCREEN_W - total) / 2;
        for (int i = 0; i < PBTN_COUNT; i++) {
            ui_button(x, DOTS_Y + DOT_D + 40, parental_glyph(i), parental_colour(i), UI_BTN_DISC);
            x += UI_BTN_D + 10;
        }
        ui_label(SAFE_X, DOTS_Y + DOT_D + 96, SAFE_W, ALIGN_CENTER, STL_GRAY,
                 "Any four of these");
    }

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, count > 0 ? "Delete" : "Back");

    rdpq_detach_show();
}

const screen_t SCREEN_CODE_DEF = {
    .id     = SCREEN_CODE,
    .enter  = code_enter,
    .leave  = code_leave,
    .update = code_update,
    .render = code_render,
};
