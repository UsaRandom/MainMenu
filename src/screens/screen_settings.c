/**
 * @file screen_settings.c
 * @brief Settings, and the numbers you need when something is wrong.
 * @ingroup screens
 *
 * Reached with Start from the grid; B goes back. Deliberately short: every row here is either
 * something a user changes to suit themselves, or a fact they would otherwise have to ask for.
 *
 * The status block at the bottom is the second half of that. "How many games did it find", "is
 * there a cheat database", "which build is this" are the first three questions of every support
 * conversation, and a menu that cannot answer them turns each one into a round trip.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "cheats/cheatdb.h"
#include "menu/fonts.h"
#include "library/cache.h"
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
    ROW_THEME = 0,
    ROW_SAVES_FOLDER,
    ROW_FAST_REBOOT,
    ROW_SOUNDFX,
    ROW_COUNT,
} row_t;

static int cursor;

static const theme_t *const THEMES[] = { &THEME_MIDNIGHT, &THEME_CARTRIDGE, &THEME_PHOSPHOR };
#define THEME_COUNT ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

static int theme_index (const theme_t *th) {
    for (int i = 0; i < THEME_COUNT; i++) {
        if (THEMES[i] == th) {
            return i;
        }
    }
    return 0;
}

static void settings_enter (app_t *app) {
    (void)app;
    cursor = 0;
}

static void settings_update (app_t *app, float dt) {
    (void)dt;
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B) || input_pressed(in, BTN_START)) {
        sound_play_effect(SFX_EXIT);
        /* Written on the way out rather than on every toggle: the settings screen is a handful
         * of booleans and a user flipping four of them should cost one write, not four. On
         * read-only storage this fails and says so in the log, and the change lasts the session
         * exactly as it did before -- which is what the footer still promises when
         * cache_writable() is false. */
        settings_save(&app->settings);
        app_goto(app, SCREEN_GRID);
        return;
    }

    int prev = cursor;
    if (in->up   && cursor > 0)             cursor--;
    if (in->down && cursor < ROW_COUNT - 1) cursor++;
    if (cursor != prev) {
        sound_play_effect(SFX_CURSOR);
    }

    int delta = (in->right ? 1 : 0) - (in->left ? 1 : 0);
    bool toggle = input_pressed(in, BTN_A);
    if (delta != 0 || toggle) {
        sound_play_effect(SFX_SETTING);
    }

    switch ((row_t)cursor) {
        case ROW_THEME:
            if (delta != 0 || toggle) {
                int i = theme_index(app->theme) + (delta != 0 ? delta : 1);
                app->theme = THEMES[((i % THEME_COUNT) + THEME_COUNT) % THEME_COUNT];
            }
            break;
        case ROW_SAVES_FOLDER:
            if (toggle || delta != 0) {
                app->settings.use_saves_folder = !app->settings.use_saves_folder;
            }
            break;
        case ROW_FAST_REBOOT:
            if (toggle || delta != 0) {
                app->settings.rom_fast_reboot_enabled = !app->settings.rom_fast_reboot_enabled;
            }
            break;
        case ROW_SOUNDFX:
            if (toggle || delta != 0) {
                app->settings.soundfx_enabled = !app->settings.soundfx_enabled;
                sound_use_sfx(app->settings.soundfx_enabled);
            }
            break;
        default:
            break;
    }
}

static void draw_row (app_t *app, int idx, const char *label, const char *value) {
    const theme_t *th = app->theme;
    int y = LIST_Y + idx * ROW_H;

    if (idx == cursor) {
        ui_fill(LIST_X, y, LIST_W, ROW_H, th->panel_alt);
        ui_fill(LIST_X, y, ACCENT_BAR, ROW_H, th->tab_underline);
    }
    ui_label(LIST_X + 16, y + 23, LIST_W - 32, ALIGN_LEFT,
             idx == cursor ? STL_DEFAULT : STL_GRAY, label);
    ui_label(LIST_X + 16, y + 23, LIST_W - 32, ALIGN_RIGHT, STL_DEFAULT, value);
}

static void settings_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    char buf[96];

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "Settings");

    draw_row(app, ROW_THEME, "Theme", th->name);
    draw_row(app, ROW_SAVES_FOLDER, "Keep saves in a saves/ folder",
             app->settings.use_saves_folder ? "Yes" : "No");
    draw_row(app, ROW_FAST_REBOOT, "Fast reboot back to the menu",
             app->settings.rom_fast_reboot_enabled ? "Yes" : "No");
    draw_row(app, ROW_SOUNDFX, "Sound effects",
             app->settings.soundfx_enabled ? "Yes" : "No");

    /* Status: the answers to the first three support questions, in one place. */
    int y = LIST_Y + ROW_COUNT * ROW_H + 24;
    ui_fill(LIST_X, y, LIST_W, HAIRLINE, th->panel_alt);
    y += 22;

    snprintf(buf, sizeof(buf), "%d games", app->lib != NULL ? app->lib->count : 0);
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_GRAY, "Library");
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_RIGHT, STL_GRAY, buf);
    y += 24;

    if (cheatdb_available()) {
        snprintf(buf, sizeof(buf), "%d games", cheatdb_game_count());
    } else {
        snprintf(buf, sizeof(buf), "not installed");
    }
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_GRAY, "Cheat database");
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_RIGHT, STL_GRAY, buf);
    y += 24;

    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_GRAY, "Storage");
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_RIGHT, STL_GRAY,
             app->storage != NULL ? app->storage : "none");
    y += 24;

    snprintf(buf, sizeof(buf), "%s  %s", MENU_VERSION, BUILD_TIMESTAMP);
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_GRAY, "Build");
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_RIGHT, STL_GRAY, buf);

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    (void)ui_hint(SAFE_X, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC, "Change");
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, cache_writable() ? "Back" : "Back (not saved to card)");

    rdpq_detach_show();
}

const screen_t SCREEN_SETTINGS_DEF = {
    .id     = SCREEN_SETTINGS,
    .enter  = settings_enter,
    .update = settings_update,
    .render = settings_render,
};
