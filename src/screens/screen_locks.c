/**
 * @file screen_locks.c
 * @brief The whole library, one padlock per title.
 * @ingroup screens
 *
 * Deliberately a flat alphabetical list of everything rather than the tabbed box-art grid. The
 * grid is built for choosing something to play; this is built for a parent working through a
 * shelf, and for that a dense list of names beats twelve pictures a screenful. It is also the
 * only screen in the menu that shows every system in one place, which is what "lock everything
 * called Mortal Kombat" needs.
 *
 * Reached only from the parental panel, which is itself behind the code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "library/cache.h"
#include "library/library.h"
#include "library/playstate.h"
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
#define LOCK_W      16
#define LOCK_H      18
#define LOCK_X      (LIST_X + 12)
#define LABEL_X     (LOCK_X + LOCK_W + 16)

static int cursor;
static int top;

static void locks_enter (app_t *app) {
    (void)app;
    cursor = 0;
    top = 0;
}

static void locks_update (app_t *app, float dt) {
    (void)dt;
    const input_t *in = &app->input;
    library_t *lib = app->lib;
    int count = (lib != NULL) ? lib->count : 0;

    if (input_pressed(in, BTN_B)) {
        sound_play_effect(SFX_EXIT);
        app_goto(app, SCREEN_PARENTAL);
        return;
    }

    if (count == 0) {
        return;
    }

    int prev = cursor;
    if (in->up   && cursor > 0)         cursor--;
    if (in->down && cursor < count - 1) cursor++;

    /* L and R page, as they do on the cheats list. A parent locking four games out of five
     * hundred is not going to hold Down. */
    if (input_pressed(in, BTN_L)) cursor -= VISIBLE;
    if (input_pressed(in, BTN_R)) cursor += VISIBLE;
    if (cursor < 0)      cursor = 0;
    if (cursor >= count) cursor = count - 1;

    if (input_pressed(in, BTN_A)) {
        lib->records[cursor].flags ^= LIBF_LOCKED;
        /* Marked dirty rather than written, the same as a favourite: a parent going down a shelf
         * ticking six games should cost one write on the way out, not six. */
        playstate_touch();
        sound_play_effect(SFX_SETTING);
    }

    if (cursor != prev) {
        sound_play_effect(SFX_CURSOR);
    }

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

static void locks_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    const library_t *lib = app->lib;
    int count = (lib != NULL) ? lib->count : 0;
    char buf[96];

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "Locked games");

    int locked = 0;
    for (int i = 0; i < count; i++) {
        if (lib->records[i].flags & LIBF_LOCKED) {
            locked++;
        }
    }
    snprintf(buf, sizeof(buf), "%d of %d locked", locked, count);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_RIGHT, STL_GRAY, buf);

    if (count == 0) {
        ui_label(LIST_X, LIST_Y + 40, LIST_W, ALIGN_CENTER, STL_GRAY, "No games on this card.");
    }

    for (int i = 0; i < VISIBLE; i++) {
        int idx = top + i;
        if (idx >= count) {
            break;
        }
        const lib_record_t *r = &lib->records[idx];
        int y = LIST_Y + i * ROW_H;

        if (idx == cursor) {
            ui_fill(LIST_X, y, LIST_W, ROW_H, th->panel_alt);
        }

        if (r->flags & LIBF_LOCKED) {
            ui_padlock(LOCK_X, y + 5, LOCK_W, LOCK_H, th->text_accent);
        } else {
            ui_border(LOCK_X, y + 5, LOCK_W, LOCK_H, 2, th->text_dim);
        }

        ui_label(LABEL_X, y + 20, LIST_W - (LABEL_X - LIST_X) - 70, ALIGN_LEFT,
                 idx == cursor ? STL_DEFAULT : STL_GRAY,
                 r->title != NULL ? r->title : "(untitled)");

        /* Which console, so two games with the same name are distinguishable. */
        ui_label(LIST_X, y + 20, LIST_W - 12, ALIGN_RIGHT, STL_GRAY,
                 library_tab_label((tab_t)(TAB_N64 + r->system)));
    }

    if (count > VISIBLE) {
        int track_h = VISIBLE * ROW_H;
        int thumb = (track_h * VISIBLE) / count;
        if (thumb < POSBAR_THUMB_MIN) {
            thumb = POSBAR_THUMB_MIN;
        }
        int travel = track_h - thumb;
        int pos = (travel * top) / (count - VISIBLE);
        ui_fill(POSBAR_X, LIST_Y, POSBAR_W, track_h, th->panel);
        ui_fill(POSBAR_X, LIST_Y + pos, POSBAR_W, thumb, th->text_dim);
    }

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    (void)ui_hint(SAFE_X, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC, "Lock");
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, cache_writable() ? "Back" : "Back (not saved to card)");

    rdpq_detach_show();
}

const screen_t SCREEN_LOCKS_DEF = {
    .id     = SCREEN_LOCKS,
    .enter  = locks_enter,
    .update = locks_update,
    .render = locks_render,
};
