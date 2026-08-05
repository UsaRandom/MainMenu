/**
 * @file screen_parental.c
 * @brief The parent's panel. Reached from Settings, and behind the code once one is set.
 * @ingroup screens
 *
 * The one coupling that makes or breaks the whole feature: **this screen is itself behind the
 * code**, because the schedule is enforced against a clock this menu can set and the lock list is
 * edited here. A parental panel reachable without the code is a parental panel a child turns off.
 * Settings does that check before navigating; see screen_settings.c.
 *
 * Every warning at the bottom is a case where the feature is on and not working. They are on
 * screen rather than in the log because the person who needs them is a parent, and the failure is
 * silent by nature -- a lock that was not written and a schedule with no clock both look exactly
 * like a lock and a schedule that are fine.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <libdragon.h>

#include "app.h"
#include "library/cache.h"
#include "library/library.h"
#include "menu/fonts.h"
#include "menu/parental.h"
#include "menu/settings.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

#define LIST_X      SAFE_X
#define LIST_Y      96
#define LIST_W      SAFE_W
#define ROW_H       34

typedef enum {
    ROW_CODE = 0,
    ROW_LOCKS,
    ROW_HOURS,
    ROW_FROM,
    ROW_TO,
    ROW_COUNT,
} row_t;

static int cursor;

static int locked_count (const app_t *app) {
    int n = 0;
    if (app->lib != NULL) {
        for (int i = 0; i < app->lib->count; i++) {
            if (app->lib->records[i].flags & LIBF_LOCKED) {
                n++;
            }
        }
    }
    return n;
}

static void parental_enter (app_t *app) {
    (void)app;
    cursor = 0;
}

static void parental_update (app_t *app, float dt) {
    (void)dt;
    const input_t *in = &app->input;
    parental_t *s = parental_state();

    if (input_pressed(in, BTN_B) || input_pressed(in, BTN_START)) {
        sound_play_effect(SFX_EXIT);
        parental_save();
        app_goto(app, SCREEN_SETTINGS);
        return;
    }

    int prev = cursor;
    if (in->up   && cursor > 0)             cursor--;
    if (in->down && cursor < ROW_COUNT - 1) cursor++;
    if (cursor != prev) {
        sound_play_effect(SFX_CURSOR);
    }

    int delta = (in->right ? 1 : 0) - (in->left ? 1 : 0);
    bool activate = input_pressed(in, BTN_A);

    switch ((row_t)cursor) {
        case ROW_CODE:
            if (activate) {
                /* Setting a code writes it; clearing one has to prove you know it first,
                 * otherwise the lock is removable by anyone who can find this screen -- and this
                 * screen is only protected by the code being there. */
                if (parental_code_set()) {
                    screen_code_ask(CODE_ASK_CLEAR, "Enter it once more to remove it",
                                    SCREEN_PARENTAL, SCREEN_PARENTAL);
                } else {
                    screen_code_ask(CODE_ASK_SET, NULL, SCREEN_PARENTAL, SCREEN_PARENTAL);
                }
                sound_play_effect(SFX_ENTER);
                app_goto(app, SCREEN_CODE);
                return;
            }
            break;

        case ROW_LOCKS:
            if (activate) {
                sound_play_effect(SFX_ENTER);
                app_goto(app, SCREEN_LOCKS);
                return;
            }
            break;

        case ROW_HOURS:
            if (activate || delta != 0) {
                s->hours_enabled = !s->hours_enabled;
                sound_play_effect(SFX_SETTING);
            }
            break;

        case ROW_FROM:
            if (delta != 0 || activate) {
                int step = (delta != 0) ? delta : 1;
                s->hour_from = ((s->hour_from + step) % 24 + 24) % 24;
                sound_play_effect(SFX_SETTING);
            }
            break;

        case ROW_TO:
            if (delta != 0 || activate) {
                int step = (delta != 0) ? delta : 1;
                s->hour_to = ((s->hour_to + step) % 24 + 24) % 24;
                sound_play_effect(SFX_SETTING);
            }
            break;

        default:
            break;
    }
}

static void draw_row (app_t *app, int idx, const char *label, const char *value, bool dim) {
    const theme_t *th = app->theme;
    int y = LIST_Y + idx * ROW_H;

    if (idx == cursor) {
        ui_fill(LIST_X, y, LIST_W, ROW_H, th->panel_alt);
        ui_fill(LIST_X, y, ACCENT_BAR, ROW_H, th->tab_underline);
    }
    ui_label(LIST_X + 16, y + 23, LIST_W - 32, ALIGN_LEFT,
             idx == cursor ? STL_DEFAULT : STL_GRAY, label);
    ui_label(LIST_X + 16, y + 23, LIST_W - 32, ALIGN_RIGHT,
             dim ? STL_GRAY : STL_DEFAULT, value);
}

static void parental_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    const parental_t *s = parental_state();
    char buf[96];

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "Parental controls");

    bool have_code = parental_code_set();

    draw_row(app, ROW_CODE, have_code ? "Code (A to remove)" : "Code (A to set)",
             have_code ? "Set" : "Not set", !have_code);

    int n = locked_count(app);
    if (n == 0) {
        snprintf(buf, sizeof(buf), "None");
    } else {
        snprintf(buf, sizeof(buf), "%d %s", n, n == 1 ? "game" : "games");
    }
    draw_row(app, ROW_LOCKS, "Locked games", buf, n == 0);

    parental_window_text(buf, sizeof(buf));
    draw_row(app, ROW_HOURS, "Playing allowed", buf, !s->hours_enabled);

    char hour[16];
    parental_hour_text(s->hour_from, hour, sizeof(hour));
    draw_row(app, ROW_FROM, "From", hour, !s->hours_enabled);
    parental_hour_text(s->hour_to, hour, sizeof(hour));
    draw_row(app, ROW_TO, "Until", hour, !s->hours_enabled);

    /* The three ways this can be switched on and not working. */
    int y = LIST_Y + ROW_COUNT * ROW_H + 24;
    ui_fill(LIST_X, y, LIST_W, HAIRLINE, th->panel_alt);
    y += 26;

    if (!have_code) {
        ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_GRAY,
                 "Nothing here applies until a code is set.");
        y += 26;
    }
    if (!cache_writable()) {
        /* Three things fail on a card that cannot be written, and they fail differently. The code
         * and its failure count go through ini_save() into parental.ini, which upstream has
         * shipped for years but still cannot write to a locked card. The locks go through cache.c,
         * which has never run against real storage at all. So the code may survive a reboot while
         * the locks do not, and the wait after a wrong entry lasts only until the console is
         * switched off -- and a parent must not be left believing otherwise. */
        ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_YELLOW,
                 "Nothing here is saved to this card.");
        y += 26;
    }
    if (s->hours_enabled && !parental_clock_ok(time(NULL))) {
        /* Fails open, and says so. A schedule that failed closed on a console with no clock would
         * lock the family out of a menu they never asked to be locked out of. */
        ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_YELLOW,
                 "No clock, so the hours are not enforced.");
        y += 26;
    }
    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    (void)ui_hint(SAFE_X, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC, "Change");
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, "Back");

    rdpq_detach_show();
}

const screen_t SCREEN_PARENTAL_DEF = {
    .id     = SCREEN_PARENTAL,
    .enter  = parental_enter,
    .update = parental_update,
    .render = parental_render,
};
