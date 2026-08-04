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
#include <libdragon.h>

#include "app.h"
#include "cheats/cheatdb.h"
#include "cheats/cheatstate.h"
#include "library/playstate.h"
#include "menu/fonts.h"
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

static const char *SAVE_NAME[] = {
    "None", "EEPROM 4K", "EEPROM 16K", "SRAM 256K", "SRAM banked",
    "SRAM 1M", "FlashRAM 1M", "FlashRAM (PKST2)",
};

static const char *tv_name (rom_tv_type_t t) {
    switch (t) {
        case ROM_TV_TYPE_PAL:  return "PAL";
        case ROM_TV_TYPE_NTSC: return "NTSC";
        case ROM_TV_TYPE_MPAL: return "MPAL";
        default:               return "Unknown";
    }
}

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

    if (input_pressed(in, BTN_Z) && app->cheats.group_count > 0) {
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

    if (rec != NULL) {
        snprintf(buf, sizeof(buf), "%s  v1.%d", rec->game_code, rec->version);
        y = info_row(INFO_X, y, INFO_W, "Code", buf);
    }

    rom_save_type_t st = rom_info_get_save_type((rom_info_t *)ri);
    y = info_row(INFO_X, y, INFO_W, "Save",
                 (st < (rom_save_type_t)(sizeof(SAVE_NAME) / sizeof(SAVE_NAME[0])))
                     ? SAVE_NAME[st] : "Unknown");

    y = info_row(INFO_X, y, INFO_W, "Video", tv_name(rom_info_get_tv_type((rom_info_t *)ri)));

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

    /* Cheats as a row here rather than a badge on the tile: the grid says what a game IS, and
     * whether someone has codes for it is not part of that. */
    if (app->cheats.group_count > 0) {
        int on = 0;
        for (int i = 0; i < app->cheats.group_count; i++) {
            if (app->cheats.groups[i].enabled) {
                on++;
            }
        }
        snprintf(buf, sizeof(buf), "%d of %d enabled", on, app->cheats.group_count);
        y = info_row(INFO_X, y, INFO_W, "Cheats", buf);
    }

    if (rec != NULL && (rec->flags & LIBF_NO_MATCH)) {
        /* Said plainly rather than hidden: an unmatched ROM gets a guessed save type, and if it
         * boots wrong this line is the explanation. */
        y += 8;
        ui_label(INFO_X, y, INFO_W, ALIGN_LEFT, STL_YELLOW, "Not in the ROM database");
        y += ROW_H;
    }

    /* Footer hints belong to the sheet while it is up, so the grid's own hints do not show
     * through and offer actions that are not available. */
    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel_alt);
    int hx = SAFE_X;
    hx = ui_hint(hx, FOOTER_Y + 14, "S", BTN_START_COLOR, UI_BTN_DISC, "Play");
    hx = ui_hint(hx, FOOTER_Y + 14, ">", BTN_C_COLOR, UI_BTN_DISC, "Fav");
    if (app->cheats.group_count > 0) {
        hx = ui_hint(hx, FOOTER_Y + 14, "Z", BTN_Z_COLOR, UI_BTN_TALL, "Cheats");
    }
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
