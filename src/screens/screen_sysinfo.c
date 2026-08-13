/**
 * @file screen_sysinfo.c
 * @brief Build, card, memory, and the other facts a support conversation asks for.
 * @ingroup screens
 *
 * Reached from Settings. Not a setting: nothing here can be changed, and that is the point.
 * The boot plate shows at most two warnings and then goes away. launch.log is on the card.
 * This page is what you photograph from the sofa.
 *
 * The list is built each frame from cardstat_info(), which reads state that already exists.
 * Nothing is stored here.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "menu/cardstat.h"
#include "menu/fonts.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

#define LIST_X      SAFE_X
#define LIST_Y      80
#define LIST_W      SAFE_W
#define ROW_H       28
#define LIST_H      (FOOTER_Y - LIST_Y)
#define VISIBLE     (LIST_H / ROW_H)
#define INFO_MAX    16

_Static_assert(VISIBLE >= 1, "system info has no room for a single row");

static int top;

static void sysinfo_enter (app_t *app) {
    (void)app;
    top = 0;
}

static void sysinfo_update (app_t *app, float dt) {
    (void)dt;
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B) || input_pressed(in, BTN_START)) {
        sound_play_effect(SFX_EXIT);
        app_goto(app, SCREEN_SETTINGS);
        return;
    }

    cardstat_row_t rows[INFO_MAX];
    int n = cardstat_info(rows, INFO_MAX);
    /* Build is drawn as an extra row above the cardstat list. */
    int total = n + 1;

    int prev = top;
    if (in->up && top > 0) {
        top--;
    }
    if (in->down && top + VISIBLE < total) {
        top++;
    }
    if (top != prev) {
        sound_play_effect(SFX_CURSOR);
    }
}

static void draw_pair (int y, const char *label, const char *value, bool dim) {
    int style = dim ? STL_GRAY : STL_DEFAULT;
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, style, label);
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_RIGHT, style, value);
}

static void sysinfo_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    cardstat_row_t rows[INFO_MAX];
    int n = cardstat_info(rows, INFO_MAX);
    int total = n + 1;

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "System info");

    rdpq_set_scissor(0, LIST_Y, SCREEN_W, LIST_Y + VISIBLE * ROW_H);

    char build[96];
    snprintf(build, sizeof(build), "%s  %s", MENU_VERSION, BUILD_TIMESTAMP);

    for (int i = 0; i < total; i++) {
        int y = LIST_Y + (i - top) * ROW_H + 20;
        if (i == 0) {
            draw_pair(y, "Build", build, false);
        } else {
            draw_pair(y, rows[i - 1].label, rows[i - 1].value, false);
        }
    }

    rdpq_set_scissor(0, 0, SCREEN_W, SCREEN_H);

    if (total > VISIBLE) {
        int track_h = VISIBLE * ROW_H;
        int thumb = (track_h * VISIBLE) / total;
        if (thumb < POSBAR_THUMB_MIN) {
            thumb = POSBAR_THUMB_MIN;
        }
        int travel = track_h - thumb;
        int pos = (travel * top) / (total - VISIBLE);
        ui_fill(POSBAR_X, LIST_Y, POSBAR_W, track_h, th->panel);
        ui_fill(POSBAR_X, LIST_Y + pos, POSBAR_W, thumb, th->text_dim);
    }

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, "Back");

    rdpq_detach_show();
}

const screen_t SCREEN_SYSINFO_DEF = {
    .id     = SCREEN_SYSINFO,
    .enter  = sysinfo_enter,
    .update = sysinfo_update,
    .render = sysinfo_render,
};
