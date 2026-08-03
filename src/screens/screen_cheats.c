/**
 * @file screen_cheats.c
 * @brief Named cheats with checkboxes. No hex anywhere in this path.
 * @ingroup screens
 *
 * The list is of GROUPS, never of individual code lines, and that is a correctness constraint
 * rather than a simplification. See cheatdb.h: a Datel conditional and the write it guards are
 * two lines that only mean anything together, and upstream's per-line toggles let you disable
 * one half and leave the engine pairing the survivor with an unrelated code.
 *
 * The corpus is not small -- some games carry four figures of cheats -- so this is a plain
 * scrolling list with a fixed row height rather than anything cleverer.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "cheats/cheatdb.h"
#include "menu/fonts.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

#define LIST_X      SAFE_X
#define LIST_Y      88
#define LIST_W      SAFE_W
#define ROW_H       28
#define VISIBLE     11                      /**< (424 - 88) / 28 */
#define CHECK_SZ    16
#define CHECK_X     (LIST_X + 12)
#define LABEL_X     (CHECK_X + CHECK_SZ + 14)

static int cursor;
static int top;                             /**< first visible row */

static void cheats_enter (app_t *app) {
    (void)app;
    cursor = 0;
    top = 0;
}

static void cheats_update (app_t *app, float dt) {
    (void)dt;
    const input_t *in = &app->input;
    cheatset_t *set = &app->cheats;

    if (input_pressed(in, BTN_B)) {
        sound_play_effect(SFX_EXIT);
        app_goto(app, SCREEN_DETAIL);
        return;
    }

    if (set->group_count == 0) {
        return;
    }

    int prev = cursor;
    if (in->up   && cursor > 0)                    cursor--;
    if (in->down && cursor < set->group_count - 1) cursor++;

    /* L and R page, because a four-figure list is not navigable one row at a time. They mean
     * "sideways through the list" here as they mean "sideways through the tabs" on the grid. */
    if (input_pressed(in, BTN_L)) cursor -= VISIBLE;
    if (input_pressed(in, BTN_R)) cursor += VISIBLE;
    if (cursor < 0)                 cursor = 0;
    if (cursor >= set->group_count) cursor = set->group_count - 1;

    if (input_pressed(in, BTN_A)) {
        sound_play_effect(SFX_SETTING);
        set->groups[cursor].enabled = !set->groups[cursor].enabled;
    }

    if (cursor != prev) {
        sound_play_effect(SFX_CURSOR);
    }

    /* Scroll only enough to keep the cursor on screen, so paging does not recentre the list and
     * lose the reader's place. */
    if (cursor < top) {
        top = cursor;
    }
    if (cursor >= top + VISIBLE) {
        top = cursor - VISIBLE + 1;
    }
    if (top < 0) {
        top = 0;
    }
}

static int enabled_count (const cheatset_t *set) {
    int n = 0;
    for (int i = 0; i < set->group_count; i++) {
        if (set->groups[i].enabled) {
            n++;
        }
    }
    return n;
}

static void cheats_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    cheatset_t *set = &app->cheats;
    char buf[96];

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "Cheats");

    snprintf(buf, sizeof(buf), "%d of %d enabled", enabled_count(set), set->group_count);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_RIGHT, STL_GRAY, buf);

    if (set->group_count == 0) {
        ui_label(LIST_X, LIST_Y + 40, LIST_W, ALIGN_CENTER, STL_GRAY,
                 "No cheats for this game.");
    }

    for (int i = 0; i < VISIBLE; i++) {
        int idx = top + i;
        if (idx >= set->group_count) {
            break;
        }
        const cheat_group_t *g = &set->groups[idx];
        int y = LIST_Y + i * ROW_H;

        if (idx == cursor) {
            ui_fill(LIST_X, y, LIST_W, ROW_H, th->panel_alt);
        }

        ui_border(CHECK_X, y + 6, CHECK_SZ, CHECK_SZ, 2, th->text_dim);
        if (g->enabled) {
            ui_fill(CHECK_X + 4, y + 10, CHECK_SZ - 8, CHECK_SZ - 8, th->text_accent);
        }

        ui_label(LABEL_X, y + 20, LIST_W - (LABEL_X - LIST_X) - 60, ALIGN_LEFT,
                 idx == cursor ? STL_DEFAULT : STL_GRAY, g->name);

        /* Line count, so a one-line "infinite health" and a forty-line "have everything" are
         * distinguishable before you turn them on. */
        snprintf(buf, sizeof(buf), "%u", (unsigned)g->count);
        ui_label(LIST_X, y + 20, LIST_W - 12, ALIGN_RIGHT, STL_GRAY, buf);
    }

    /* Position bar, same geometry rule as the grid. */
    if (set->group_count > VISIBLE) {
        int track_h = VISIBLE * ROW_H;
        int thumb = (track_h * VISIBLE) / set->group_count;
        if (thumb < POSBAR_THUMB_MIN) {
            thumb = POSBAR_THUMB_MIN;
        }
        int travel = track_h - thumb;
        int pos = (set->group_count > VISIBLE)
                ? (travel * top) / (set->group_count - VISIBLE) : 0;
        ui_fill(POSBAR_X, LIST_Y, POSBAR_W, track_h, th->panel);
        ui_fill(POSBAR_X, LIST_Y + pos, POSBAR_W, thumb, th->text_dim);
    }

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    (void)ui_hint(SAFE_X, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC, "Toggle");
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, "Back");

    rdpq_detach_show();
}

const screen_t SCREEN_CHEATS_DEF = {
    .id     = SCREEN_CHEATS,
    .enter  = cheats_enter,
    .update = cheats_update,
    .render = cheats_render,
};
