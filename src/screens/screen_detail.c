/**
 * @file screen_detail.c
 * @brief One game, at rest: art, what it needs, and how to start it.
 * @ingroup screens
 *
 * Reached with A from the grid; B goes back; Start boots. The sheet rises from the bottom over
 * DUR_SHEET_OPEN rather than cutting, so the grid stays visible behind it and the transition
 * says "this is the thing you were pointing at" instead of "you are somewhere else now".
 *
 * Art is the grid's 140 x 98 thumbnail scaled up, not a fresh 1:1 decode. AUDIT.md 1h measures
 * PNG decode at 2.4-7.5 us/pixel, so a full-size decode on sheet open would cost seconds and
 * arrive long after the animation finished. A crisp sheet needs the on-disk cache first.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <libdragon.h>

#include "app.h"
#include "cheats/cheatdb.h"
#include "cheats/cheatstate.h"
#include "cheats/usercheats.h"
#include "library/playstate.h"
#include "menu/fonts.h"
#include "menu/parental.h"
#include "menu/rom_info.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"
#include "ui/tween.h"

#define SHEET_X         16
#define SHEET_W         608
#define SHEET_TOP       48                       /**< where the sheet sits fully risen */
#define SHEET_H         (SCREEN_H - SHEET_TOP)

#define ART_X           (SHEET_X + 24)
#define ART_W           280                      /**< 2x the cached tile, exact integer scale */
#define ART_H           196
#define ART_Y_OFF       28                       /**< from the sheet's own top edge */

#define INFO_X          (ART_X + ART_W + 24)
#define INFO_W          (SHEET_X + SHEET_W - 24 - INFO_X)

#define ROW_H           26
#define GLYPH_W         12       /**< Firple-Bold at size 20, near enough monospace */
#define ROW_MIN_GAP     16       /**< label-to-value gap below which the row wraps */

static tween_t rise;
static bool    closing;

/**
 * @brief Which record app->cheats currently holds, or -1 for none.
 *
 * `group_count == 0` used to stand in for this, and it is not the same question. Leaving the
 * sheet with Start does NOT free the set -- screen_launch has to read it -- so the set outlives
 * the sheet that loaded it, and the next game's sheet then found group_count non-zero and skipped
 * the load entirely. Two different games showed one game's cheats: NGEE and NTEA rendered a
 * byte-identical "0 of N enabled" row off GoldenEye's set, and cheatstate_capture() went on to
 * persist those group-name hashes under the other game's key -- a selection the user never made,
 * written to the card, against a game it does not belong to.
 *
 * Reproduced by tools/inputs/cheat-leak.txt. It is unmissable under ares, where a launch always
 * returns to the grid, and reachable on hardware wherever a launch does not end in boot().
 */
static int cheats_for_rom = -1;

static void detail_enter (app_t *app) {
    closing = false;
    tween_start(&rise, DUR_SHEET_OPEN);

    /* Load once per game rather than once per visit: coming back from the cheats screen must not
     * throw away what the user just ticked. Keyed on WHICH game, not on whether anything is
     * loaded -- see cheats_for_rom. */
    if (app->launch.rom_id >= 0 && cheats_for_rom != app->launch.rom_id) {
        cheatdb_free(&app->cheats);
        cheats_for_rom = app->launch.rom_id;
        const lib_record_t *r = &app->lib->records[app->launch.rom_id];
        cheatdb_load(r->check_code, r->game_code, r->version, &app->cheats);
        /* Before cheatstate_apply(), so a remembered tick covers a hand-entered cheat too. They
         * are keyed by name like every other group and need no special case. */
        usercheats_apply(&app->cheats, playstate_key(r));
        /* Re-tick whatever the user had on last time. Matched by group NAME, so a refreshed
         * cheats.db that reorders its entries cannot silently enable a different cheat --
         * see cheatstate.h. */
        int on = cheatstate_apply(&app->cheats, playstate_key(r));
        if (on > 0) {
            debugf("CHEATSTATE restored %d cheats for %s\n", on, r->game_code);
        }
    }
}

static void detail_update (app_t *app, float dt) {
    const input_t *in = &app->input;

    tween_step(&rise, dt);

    if (closing) {
        /* The sheet owns its own exit: it plays the fall, then navigates. Navigating on the
         * button press instead would cut the animation off at frame one and there would be no
         * point having it. */
        if (tween_t01(&rise) >= 1.0f) {
            app_goto(app, SCREEN_GRID);
        }
        return;
    }

    /* Favouriting from the sheet, which was not possible before: the sheet is where you decide
     * whether you want a game, and the only way to act on that was to back out to the grid and
     * find the tile again. Same button as the grid, so Fav means one thing everywhere. */
    if (input_pressed(in, BTN_CRIGHT) && app->launch.rom_id >= 0) {
        lib_record_t *r = &app->lib->records[app->launch.rom_id];
        r->flags ^= LIBF_FAVORITE;
        playstate_touch();
        sound_play_effect(SFX_SETTING);
    }

    /* Unconditional now. It was guarded on group_count, which locked the cheats screen away
     * for exactly the games that have no cheats -- and those are the ones somebody wants to type
     * one in for. The screen says so itself when the list is empty. */
    if (input_pressed(in, BTN_Z)) {
        sound_play_effect(SFX_ENTER);
        app_goto(app, SCREEN_CHEATS);
        return;
    }

    if (input_pressed(in, BTN_B)) {
        sound_play_effect(SFX_EXIT);
        /* Capture before the free: the groups own the names cheatstate hashes. Done on the way
         * out of the sheet rather than on every checkbox, for the same reason favourites are. */
        if (app->launch.rom_id >= 0) {
            cheatstate_capture(&app->cheats,
                               playstate_key(&app->lib->records[app->launch.rom_id]));
        }
        cheatdb_free(&app->cheats);
        /* Required, not tidiness: without it, re-entering this same sheet would match
         * cheats_for_rom, skip the load, and show a game with cheats as a game with none. */
        cheats_for_rom = -1;
        closing = true;
        tween_start(&rise, DUR_SHEET_CLOSE);
        return;
    }

    if (input_pressed(in, BTN_START) || input_pressed(in, BTN_A)) {
        /* The only place a game starts, which is what made "locked, never hidden" cheap: one
         * predicate at one call site, against a filter that would have had to touch every tab
         * view, the position counter, Recent, Favourites and the opening-tab logic. */
        uint16_t flags = (app->launch.rom_id >= 0)
                       ? app->lib->records[app->launch.rom_id].flags : 0;
        switch (parental_check(&app->settings, flags, time(NULL))) {
            case PARENTAL_GAME_LOCKED:
                sound_play_effect(SFX_ERROR);
                screen_code_ask(CODE_ASK_UNLOCK, "This game is locked",
                                SCREEN_LAUNCH, SCREEN_DETAIL);
                app_goto(app, SCREEN_CODE);
                return;
            case PARENTAL_OUTSIDE_HOURS: {
                char window[48];
                static char why[80];
                parental_window_text(&app->settings, window, sizeof(window));
                snprintf(why, sizeof(why), "Playing is allowed %s", window);
                sound_play_effect(SFX_ERROR);
                screen_code_ask(CODE_ASK_UNLOCK, why, SCREEN_LAUNCH, SCREEN_DETAIL);
                app_goto(app, SCREEN_CODE);
                return;
            }
            case PARENTAL_ALLOW:
                break;
        }
        sound_play_effect(SFX_ENTER);
        app_goto(app, SCREEN_LAUNCH);
    }
}

/** @brief Approximate rendered width. Firple-Bold at size 15 is near enough monospace at 9 px. */
static int text_w (const char *s) {
    return (int)strlen(s) * GLYPH_W;
}

/**
 * @brief One "label ........ value" row. Returns the next y.
 *
 * Falls to two lines when the pair does not fit, because the one-line form silently overlaps
 * rather than clipping: "Accessories" against "Controller Rumble" drew the two strings through
 * each other and read as a font bug.
 */
static int info_row (int x, int y, int w, const char *label, const char *value) {
    ui_label(x, y, w, ALIGN_LEFT, STL_GRAY, label);

    if (text_w(label) + text_w(value) + ROW_MIN_GAP <= w) {
        ui_label(x, y, w, ALIGN_RIGHT, STL_DEFAULT, value);
        return y + ROW_H;
    }

    ui_label(x, y + ROW_H - 4, w, ALIGN_RIGHT, STL_DEFAULT, value);
    return y + ROW_H * 2 - 4;
}

static void detail_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    const rom_info_t *ri = &app->launch.rom_info;
    const lib_record_t *rec = (app->launch.rom_id >= 0 && app->launch.rom_id < app->lib->count)
                            ? &app->lib->records[app->launch.rom_id] : NULL;

    rdpq_attach(fb, NULL);

    /* Behind the sheet: the grid's background, dimmed. Not the live grid -- re-running its
     * render here would need its scroll state and would draw twelve tiles that are about to be
     * covered. A flat scrim is what the handoff specifies anyway. */
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    float t = closing
            ? 1.0f - ease_bezier(tween_t01(&rise), EASE_SHEET_CLOSE)
            :        ease_bezier(tween_t01(&rise), EASE_SHEET_OPEN);
    int sheet_y = SHEET_TOP + (int)((1.0f - t) * (float)(SCREEN_H - SHEET_TOP));

    ui_wash(0, 0, SCREEN_W, SCREEN_H, th->bg, (uint8_t)(t * 140.0f));

    ui_fill(SHEET_X, sheet_y, SHEET_W, SCREEN_H - sheet_y, th->panel);
    ui_fill(SHEET_X, sheet_y, SHEET_W, ACCENT_BAR, th->tab_underline);

    /* Art, 2x the cached thumbnail. An exact integer scale, so nearest-neighbour upscaling
     * doubles pixels cleanly instead of shimmering along a fractional edge. */
    int art_y = sheet_y + ART_Y_OFF;
    surface_t *art = (rec != NULL)
                   ? thumbcache_get(app->thumbs, app->lib, (uint16_t)app->launch.rom_id) : NULL;
    if (art != NULL) {
        rdpq_set_mode_copy(false);
        rdpq_tex_blit(art, ART_X, art_y, &(rdpq_blitparms_t){ .scale_x = 2.0f, .scale_y = 2.0f });
    } else {
        ui_fill(ART_X, art_y, ART_W, ART_H, th->bg_alt);
        ui_border(ART_X, art_y, ART_W, ART_H, 2, th->panel_alt);
    }

    int y = art_y;
    ui_label(INFO_X, y, INFO_W, ALIGN_LEFT, STL_DEFAULT,
             (rec != NULL && rec->title != NULL) ? rec->title : ri->title);
    y += ROW_H + 8;

    char buf[64];

    /* Play count leads, because it is the only row on this sheet that is about the person
     * holding the controller rather than about the cartridge.
     *
     * The rows this replaced -- game code, save type, TV standard -- were three facts a player
     * cannot act on. Save type in particular is only interesting when it is WRONG, and the sheet
     * has a row for that case already: "Not in the ROM database", below. */
    if (rec != NULL) {
        if (rec->play_count == 0) {
            y = info_row(INFO_X, y, INFO_W, "Played", "Never");
        } else if (rec->play_count == 1) {
            y = info_row(INFO_X, y, INFO_W, "Played", "Once");
        } else {
            snprintf(buf, sizeof(buf), "%lu times", (unsigned long)rec->play_count);
            y = info_row(INFO_X, y, INFO_W, "Played", buf);
        }
    }

    /* Said here as well as badged on the tile, because the sheet is where someone stands before
     * pressing A and it should not be the code prompt that first mentions it. */
    if (rec != NULL && (rec->flags & LIBF_LOCKED)) {
        y = info_row(INFO_X, y, INFO_W, "Locked", "Code needed");
    }

    /* Always, including when there are none. The row was conditional, which meant its absence
     * had two meanings a player cannot tell apart: this game has no codes, or the card has no
     * cheats.db at all. Saying "None available" answers the first and leaves the second visible
     * as every game reading the same. */
    int on = 0;
    for (int i = 0; i < app->cheats.group_count; i++) {
        if (app->cheats.groups[i].enabled) {
            on++;
        }
    }
    if (app->cheats.group_count == 0) {
        y = info_row(INFO_X, y, INFO_W, "Cheats", "None available");
    } else {
        snprintf(buf, sizeof(buf), "%d of %d enabled", on, app->cheats.group_count);
        y = info_row(INFO_X, y, INFO_W, "Cheats", buf);
    }

    if (ri->features.expansion_pak == EXPANSION_PAK_REQUIRED) {
        y = info_row(INFO_X, y, INFO_W, "Expansion Pak", "Required");
    } else if (ri->features.expansion_pak == EXPANSION_PAK_RECOMMENDED) {
        y = info_row(INFO_X, y, INFO_W, "Expansion Pak", "Recommended");
    }

    /* Accessories as one line rather than a row each: on a 480-line screen a column of six
     * mostly-"No" rows crowds out the things that differ between games. */
    buf[0] = '\0';
    if (ri->features.controller_pak)        strncat(buf, "Controller ", sizeof(buf) - strlen(buf) - 1);
    if (ri->features.rumble_pak)            strncat(buf, "Rumble ",     sizeof(buf) - strlen(buf) - 1);
    if (ri->features.transfer_pak)          strncat(buf, "Transfer ",   sizeof(buf) - strlen(buf) - 1);
    if (ri->features.real_time_clock)       strncat(buf, "RTC ",        sizeof(buf) - strlen(buf) - 1);
    if (ri->features.voice_recognition_unit) strncat(buf, "VRU ",       sizeof(buf) - strlen(buf) - 1);
    if (buf[0] != '\0') {
        y = info_row(INFO_X, y, INFO_W, "Accessories", buf);
    }

    if (rec != NULL && (rec->flags & LIBF_NO_MATCH)) {
        /* Said plainly rather than hidden: an unmatched ROM gets a guessed save type, and if it
         * boots wrong this line is the explanation.
         *
         * "Not in the ROM database" was two characters too wide for INFO_W and ui_label clips
         * rather than ellipsising, so it drew as "Not in the ROM databa" -- a warning that looks
         * like a rendering fault, which is the last thing a warning should look like. It was
         * invisible until the demo tree, where every title misses the database on purpose. */
        y += 8;
        ui_label(INFO_X, y, INFO_W, ALIGN_LEFT, STL_YELLOW, "Not in the database");
        y += ROW_H;
    }

    /* Footer hints belong to the sheet while it is up, so the grid's own hints do not show
     * through and offer actions that are not available. */
    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel_alt);
    int hx = SAFE_X;
    /* A, not Start. Both have always launched -- see the update() handler -- but the footer
     * advertised only Start, which made the button you had just pressed to get here appear to
     * do nothing on the screen it opened. A is the affirmative button everywhere else in the
     * menu; Start stays bound because nothing is gained by taking it away. */
    hx = ui_hint(hx, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC, "Play");
    hx = ui_hint(hx, FOOTER_Y + 14, ">", BTN_C_COLOR, UI_BTN_DISC, "Fav");
    hx = ui_hint(hx, FOOTER_Y + 14, "Z", BTN_Z_COLOR, UI_BTN_TALL, "Cheats");
    (void)hx;
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, "Back");

    rdpq_detach_show();
}

const screen_t SCREEN_DETAIL_DEF = {
    .id     = SCREEN_DETAIL,
    .enter  = detail_enter,
    .update = detail_update,
    .render = detail_render,
};
