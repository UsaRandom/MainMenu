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
#include "menu/cheatcheck.h"
#include "cheats/cheatstate.h"
#include "cheats/usercheats.h"
#include "library/boxart.h"
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
/* The art column, and it is fixed: the info column beside it must not move between one game and
 * the next, and it must not narrow either, because ui_label clips rather than ellipsising and
 * this sheet has already lost the tail of two strings that way.
 *
 * Two of the NARROW tile across. A tile has two possible widths now -- a landscape cover is
 * cached 140 x 98 where a portrait one is 109 x 155 -- so reserving two of the wider one would
 * have been the tidy answer: exactly 2x for every shape, an integer scale, no resampling. It
 * costs 62 px, and the info column is 318 px of a sheet that cannot grow. Measured rather than
 * argued: at the 256 px that leaves, the cheat row draws as "Not supported for thi" and any title
 * over 21 characters loses its tail. Sharper art is not worth a truncated title.
 *
 * So a wide tile is fitted to this column instead, at 1.557x rather than 2x. That is still more
 * source pixels than the sheet had before the wide column existed -- 140 x 98 upscaled 1.557x,
 * where it used to be 109 x 76 upscaled 2x -- so the art got better here as well, just not by as
 * much as it could have. */
#define ART_W           (TILE_W_NARROW * 2)
#define ART_H_MAX       (TILE_H_MAX * 2)
#define ART_Y_OFF       28                       /**< from the sheet's own top edge */

#define INFO_X          (ART_X + ART_W + 24)
#define INFO_W          (SHEET_X + SHEET_W - 24 - INFO_X)

_Static_assert(SHEET_TOP + ART_Y_OFF + ART_H_MAX <= SCREEN_H,
               "the tallest box art runs off the bottom of the detail sheet");

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

/** Whether a cheat load is owed for the current game. See detail_enter(). */
static bool cheats_pending;

/** Whether the set in memory was loaded with the database on. A toggle in Settings must
 *  not keep showing the previous list for the same game. */
static bool cheats_from_db;

/** Whether the cheat engine can hook this game at all, decided alongside the load. */
static cheatfit_t cheats_fit = CHEATFIT_OK;

static float marquee_t;
static bool  pending_rename;
static char  pending_name[64];
static char  pending_path[512];

static void apply_pending_rename (app_t *app) {
    if (!pending_rename) {
        return;
    }
    pending_rename = false;
    if (!screen_keyboard_accepted() || app->lib == NULL) {
        return;
    }

    int id = library_find_path(app->lib, pending_path);
    if (id < 0) {
        return;
    }

    /* The RAM art pool is keyed on rom_id. library_set_title qsorts the records, so every
     * slot would then hold the wrong game -- or ART_READY with no surface, which the decoder
     * will not restart, which is a blank tile until the next boot. */
    thumbcache_prepare_shuffle(app->thumbs, app->lib);
    if (!library_set_title(app->lib, &app->lib->records[id], pending_name)) {
        sound_play_effect(SFX_ERROR);
        return;
    }
    thumbcache_rebind(app->thumbs, app->lib);

    /* library_finish sorts, so the index we opened this sheet with is stale. */
    int found = library_find_path(app->lib, pending_path);
    if (found >= 0) {
        app->launch.rom_id = found;
        cheats_for_rom = found;
        screen_grid_focus(found);
    }
}

static void detail_enter (app_t *app) {
    apply_pending_rename(app);

    closing = false;
    marquee_t = 0.0f;
    tween_start(&rise, DUR_SHEET_OPEN);

    /* Load once per game rather than once per visit: coming back from the cheats screen must not
     * throw away what the user just ticked. Keyed on WHICH game, not on whether anything is
     * loaded -- see cheats_for_rom.
     *
     * Owed here, done in background(). The load is an fseek and an fread of this game's blob out
     * of cheats.db, which is nothing under ares -- the DFS is in the ROM -- and about a second on
     * FatFs over a real SC64. Doing it inside enter() put that second inside a screen transition,
     * where nothing feeds the mixer, and the music audibly stopped every time a sheet opened. */
    cheats_pending = (app->launch.rom_id >= 0 &&
                      (cheats_for_rom != app->launch.rom_id ||
                       cheats_from_db != app->settings.use_cheat_database));
}

/**
 * @brief Do the owed cheat load, if there is one.
 *
 * Called from background(), which runs while the RDP drains, and from the Z handler for the case
 * where the user opens the cheats screen before background() has had a turn. Idempotent.
 */
static void load_cheats_now (app_t *app) {
    if (!cheats_pending || app->launch.rom_id < 0) {
        return;
    }
    cheats_pending = false;

    cheatdb_free(&app->cheats);
    cheats_for_rom = app->launch.rom_id;
    cheats_from_db = app->settings.use_cheat_database;
    const lib_record_t *r = &app->lib->records[app->launch.rom_id];
    /* Skip the file read entirely rather than load-and-hide. A game with four figures of
     * groups costs about a second on FatFs, and the user who turned the database off asked
     * not to see those groups. The file stays open; only this list is empty of them.
     * usercheats_apply() on a zeroed set just appends, so typed codes still appear. */
    if (app->settings.use_cheat_database) {
        cheatdb_load(r->check_code, r->game_code, r->version, &app->cheats);
    }
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

    /* Whether the engine can hook this ROM, asked here rather than at launch because the answer
     * belongs next to the cheats row -- somebody should learn that cheats will not run before
     * they spend a minute ticking them, not after the game boots without them. It reads 4 KB off
     * the card, so it rides in background() with the load rather than blocking a transition. */
    char why[96];
    cheats_fit = (r->path != NULL) ? cheatcheck_rom(r->path, why, sizeof(why)) : CHEATFIT_OK;
    if (cheats_fit != CHEATFIT_OK) {
        debugf("CHEATCHECK %s: %s\n", r->game_code, why);
    }
}

static void detail_background (app_t *app, uint32_t budget_ticks) {
    (void)budget_ticks;
    load_cheats_now(app);
}

static void detail_update (app_t *app, float dt) {
    const input_t *in = &app->input;

    marquee_t += dt;
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

    /* Rename. C-up is the unused C button for "do something to this game"; Fav is C-right and
     * lock is C-left. Seeded with the display title so a colliding Zelda opens on the filename,
     * not THE LEGEND OF ZELDA. Empty confirm reverts. */
    if (input_pressed(in, BTN_CUP) && app->launch.rom_id >= 0) {
        const lib_record_t *r = &app->lib->records[app->launch.rom_id];
        pending_rename = true;
        pending_name[0] = '\0';
        snprintf(pending_path, sizeof(pending_path), "%s", r->path ? r->path : "");
        screen_keyboard_ask(KB_TEXT, "Name this game",
                            r->title ? r->title : "",
                            pending_name, sizeof(pending_name), SCREEN_DETAIL, true);
        sound_play_effect(SFX_ENTER);
        app_goto(app, SCREEN_KEYBOARD);
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

    /* Locking from the sheet, beside favouriting: the two C buttons either side are the two
     * things you can do *to* a game, rather than with it. The list under Parental controls stays
     * for working through a shelf; this is the short path for the one in your hand.
     *
     * The code is asked for in both directions, locking as well as unlocking. Locking without it
     * would let a child pad every game on the card with locks their parent then has to clear one
     * at a time, which is a nuisance the feature has no business enabling. No conflict with the
     * C-only code alphabet: that is read on the pad, and this navigates away to it first. */
    if (input_pressed(in, BTN_CLEFT) && app->launch.rom_id >= 0 && parental_code_set()) {
        sound_play_effect(SFX_ENTER);
        screen_code_ask_toggle_lock(app->launch.rom_id, SCREEN_DETAIL);
        app_goto(app, SCREEN_CODE);
        return;
    }

    /* Unconditional now. It was guarded on group_count, which locked the cheats screen away
     * for exactly the games that have no cheats -- and those are the ones somebody wants to type
     * one in for. The screen says so itself when the list is empty. */
    if (input_pressed(in, BTN_Z)) {
        sound_play_effect(SFX_ENTER);
        /* Forced, in case background() has not had a turn yet -- the cheats screen with an empty
         * set is indistinguishable from a game that has no cheats. Blocking here is fine: the
         * user has asked for the list and is waiting for it, which is the one moment the wait
         * is honest. */
        load_cheats_now(app);
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
            /* Saved now, not at app_deinit(): deinit only runs on a clean boot, and a user who
             * ticks cheats, backs out and powers off from the grid never has one. See the
             * matching save in screen_launch.c for the week this cost. */
            if (cheatstate_dirty()) {
                cheatstate_save();
            }
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
        switch (parental_check(flags, time(NULL))) {
            case PARENTAL_GAME_LOCKED:
                sound_play_effect(SFX_ERROR);
                screen_code_ask(CODE_ASK_UNLOCK, "This game is locked",
                                SCREEN_LAUNCH, SCREEN_DETAIL);
                app_goto(app, SCREEN_CODE);
                return;
            case PARENTAL_OUTSIDE_HOURS: {
                char window[48];
                static char why[80];
                parental_window_text(window, sizeof(window));
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

    /* The cached tile blown up to fill the art column. Derived from the tile the cache actually
     * holds, so the sheet cannot disagree with the grid about what shape this cover is. */
    int art_y = sheet_y + ART_Y_OFF;
    art_shape_t sh = (rec != NULL) ? thumbcache_record_shape(rec) : boxart_shape(SYS_N64);
    float scale = (float)ART_W / (float)sh.w;
    int art_h = (int)((float)sh.h * scale + 0.5f);
    surface_t *art = (rec != NULL)
                   ? thumbcache_get(app->thumbs, app->lib, (uint16_t)app->launch.rom_id) : NULL;
    if (art != NULL) {
        /* Standard mode, not copy: copy mode cannot scale by anything but 1, and the scale here
         * is 2.0 for a narrow tile and 1.557 for a wide one. */
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX);
        rdpq_tex_blit(art, ART_X, art_y,
                      &(rdpq_blitparms_t){ .scale_x = scale, .scale_y = scale });
    } else {
        ui_fill(ART_X, art_y, ART_W, art_h, th->bg_alt);
        ui_border(ART_X, art_y, ART_W, art_h, 2, th->panel_alt);
    }

    int y = art_y;
    ui_text_marquee(INFO_X, y, INFO_W, ALIGN_LEFT, STL_DEFAULT,
                    (rec != NULL && rec->title != NULL) ? rec->title : ri->title,
                    marquee_t);
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
     * pressing A and it should not be the code prompt that first mentions it -- and if the reason
     * is the hour rather than a padlock, saying *when* it opens is the difference between waiting
     * and giving up. Both cases still open the pad on A; this only stops the refusal being a
     * surprise. */
    if (rec != NULL && parental_code_set()) {
        long until = parental_seconds_until_open(time(NULL));
        if (rec->flags & LIBF_LOCKED) {
            y = info_row(INFO_X, y, INFO_W, "Locked", "Code needed");
        } else if (until > 0) {
            long mins = (until + 59) / 60;
            if (mins >= 60) {
                snprintf(buf, sizeof(buf), "In %ldh %02ldm", mins / 60, mins % 60);
            } else {
                snprintf(buf, sizeof(buf), "In %ld min", mins);
            }
            y = info_row(INFO_X, y, INFO_W, "Play unlocked", buf);
        }
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
    if (cheats_pending) {
        /* Not "None available" -- that is an answer, and we do not have one yet. Saying so for
         * the frame or two before background() runs stops the row flicking from a wrong answer
         * to a right one. */
        y = info_row(INFO_X, y, INFO_W, "Cheats", "Checking...");
    } else if (cheats_fit != CHEATFIT_OK && cheats_fit != CHEATFIT_UNREADABLE) {
        /* The engine cannot hook this ROM, so whatever is in the database for it would be ticked
         * and then silently ignored. Saying so where the count would go is the only place a
         * player looks before deciding to bother. */
        /* "ROM", not "game", and it is two characters shorter for a reason: at 12 px a glyph the
         * old wording measured 324 px against a 318 px column and lost its last letter, so the
         * sheet read "Not supported for this gam". It is also the more accurate word -- the
         * engine hooks a ROM image, and the same game in another region may well be fine. */
        y = info_row(INFO_X, y, INFO_W, "Cheats", "Not supported for this ROM");
    } else if (app->cheats.group_count == 0) {
        y = info_row(INFO_X, y, INFO_W, "Cheats",
                     app->settings.use_cheat_database ? "None available" : "Database off");
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

    /* Path at the bottom of the sheet, not in the footer: regular use is play / cheats / fav,
     * and the path is for telling hacks apart. Shown only while the sheet is at rest -- not
     * during the rise, and gone the instant close starts, so it does not travel with the panel.
     * The storage prefix (rom:/, sd:/) is the cartridge's mount, not a folder the user made. */
    if (rec != NULL && rec->path != NULL && !closing && !rise.running) {
        const char *shown = rec->path;
        const char *colon = strchr(shown, ':');
        if (colon != NULL) {
            shown = colon + 1;
            while (*shown == '/') {
                shown++;
            }
        }
        ui_text_marquee(SAFE_X + 8, FOOTER_Y - 12, SAFE_W - 8, ALIGN_LEFT, STL_GRAY,
                        shown, marquee_t);
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
    hx = ui_hint(hx, FOOTER_Y + 14, "^", BTN_C_COLOR, UI_BTN_DISC, "Name");
    /* Only when a code is set, because without one the button does nothing -- and a hint for a
     * button that does nothing is worse than no hint. This is also the only place the padlock is
     * advertised, so it appears exactly when it has become real. */
    if (parental_code_set()) {
        hx = ui_hint(hx, FOOTER_Y + 14, "<", BTN_C_COLOR, UI_BTN_DISC,
                     (rec != NULL && (rec->flags & LIBF_LOCKED)) ? "Unlock" : "Lock");
    }
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
    .background = detail_background,
};
