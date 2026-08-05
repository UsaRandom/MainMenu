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
#include <time.h>
#include <libdragon.h>

#include "app.h"
#include "menu/enginetest.h"
#include "menu/fonts.h"
#include "library/cache.h"
#include "menu/music.h"
#include "menu/parental.h"
#include "menu/settings.h"
#include "menu/profile.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

#define LIST_X      SAFE_X
#define LIST_Y      96
#define LIST_W      SAFE_W
#define ROW_H       34

/** The level meter drawn in place of a number. A number tells you 6; a meter tells you 6 of 10. */
#define METER_W     14
#define METER_GAP   4
#define METER_H     18

/** How long the track row must sit still before the chosen track is actually started.
 *
 * Choosing and playing have to be separate, and this is why. A held direction repeats at up to
 * 20 steps a second (ui/input.h), and starting a track tears down the player, frees the old song,
 * reads a new one and builds a new sequencer. Twenty of those a second, against an audio pipeline
 * that already has eight buffers of the previous song handed to the DMA and no way to recall
 * them, plays fragments of half a dozen songs on top of each other and lands on audio that has
 * nothing to do with the row you stopped on. That is exactly what it did.
 *
 * 0.30 s is longer than the 0.05 s gap between repeats and shorter than a deliberate second
 * press, so scrolling the list costs one load at the end rather than one per step, and a single
 * press still feels immediate. */
#define TRACK_SETTLE_S  0.30f

typedef enum {
    ROW_PROFILES = 0,
    ROW_THEME,
    ROW_SFX,
    ROW_MUSIC,
    ROW_TRACK,
    ROW_CLOCK,
    ROW_PARENTAL,
    ROW_COUNT,
} row_t;

static int cursor;
static float track_settle;      /**< seconds left before the chosen track starts; 0 = none pending */

/* theme.c owns the list. This screen used to keep a second copy of it, which meant adding a
 * theme in one place left the other short and the new one simply unreachable from the only UI
 * that can select it. */
static int theme_index (const theme_t *th) {
    for (int i = 0; i < theme_count(); i++) {
        if (theme_at(i) == th) {
            return i;
        }
    }
    return 0;
}

/** Start the chosen track now, if one is waiting. */
static void flush_track (app_t *app) {
    if (track_settle <= 0.0f) return;
    track_settle = 0.0f;
    music_set_track(app->settings.music_track);
}

static void settings_enter (app_t *app) {
    (void)app;
    cursor = 0;
    track_settle = 0.0f;
}

/** Leaving must not swallow a pending choice.
 *
 * Without this, picking a track and pressing B inside 0.30 s selects it in the settings file and
 * never plays it -- so the one case where the debounce is invisible would be the one case where
 * it looks broken. It also has to run when the code pad or the clock takes over the screen, which
 * is why it hangs off leave() rather than off the B handler. */
static void settings_leave (app_t *app) {
    flush_track(app);
}

/** Step a volume, clamped rather than wrapped.
 *
 * Wrapping is wrong for a level: holding left to silence something and having it jump to maximum
 * is the one outcome a volume control must never produce. */
static int step_volume (int value, int delta) {
    value += delta;
    if (value < 0) return 0;
    if (value > SOUND_SFX_VOLUME_MAX) return SOUND_SFX_VOLUME_MAX;
    return value;
}

/** Step the track, wrapping, with Shuffle sitting at the top of the list where a default belongs. */
static int step_track (int track, int delta) {
    int n = music_track_count();
    /* Map [SHUFFLE, 0 .. n-1] onto [0 .. n] so the wrap is one modulo rather than three branches. */
    int slot = (track == MUSIC_TRACK_SHUFFLE) ? 0 : track + 1;
    slot = ((slot + delta) % (n + 1) + (n + 1)) % (n + 1);
    return (slot == 0) ? MUSIC_TRACK_SHUFFLE : slot - 1;
}

static void settings_update (app_t *app, float dt) {
    const input_t *in = &app->input;

    if (track_settle > 0.0f) {
        track_settle -= dt;
        if (track_settle <= 0.0f) {
            track_settle = 0.0f;
            music_set_track(app->settings.music_track);
        }
    }

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

    /* The click used to be played here for every row at once. It cannot be, now that three of the
     * rows are audio: the effects row has to play its click AFTER the new level is applied,
     * because that click is the thing being set rather than feedback for the press; the music row
     * must stay quiet so the level being adjusted is what you hear; and the track row is a list,
     * not a value. So each row says what it sounds like. */

    switch ((row_t)cursor) {
        case ROW_PROFILES:
            if (toggle) {
                sound_play_effect(SFX_SETTING);
                /* Not behind the parental code, deliberately. A profile is not a permission --
                 * the padlocks and the schedule apply to every profile at once and switching
                 * cannot get round either -- so gating this would cost every family a code entry
                 * to protect nothing. See profile.h. */
                app_goto(app, SCREEN_PROFILES);
                return;
            }
            break;
        case ROW_THEME:
            if (delta != 0 || toggle) {
                sound_play_effect(SFX_SETTING);
                int n = theme_count();
                int i = theme_index(app->theme) + (delta != 0 ? delta : 1);
                app->theme = theme_at(((i % n) + n) % n);
                /* Surfaces follow app->theme on the next draw, but the font styles are
                 * registered state and do not. Without this the palette changes underneath
                 * text that stays the previous theme's colour. */
                theme_apply(app->theme);
                /* Written through to the profile, which is the first time this setting has ever
                 * been persisted at all: before profiles existed the theme was assigned at boot
                 * and a change survived exactly as long as the power did. Saved on the step
                 * rather than on leaving, because profiles.ini is four short lines and the row
                 * is not a repeat-until-it-lands control the way the volume meters are. */
                profile_set_theme(profile_active(), app->theme->name);
                profile_save();
            }
            break;
        case ROW_SFX: {
            /* A gets the same meaning it has everywhere else on this list -- step forward one --
             * so someone who never tries left and right still gets somewhere. */
            int step = (delta != 0) ? delta : (toggle ? 1 : 0);
            if (step == 0) break;
            int next = step_volume(app->settings.sfx_volume, step);
            if (next == app->settings.sfx_volume) break;
            app->settings.sfx_volume = next;
            sound_set_sfx_volume(next);
            /* After the level is applied, not before: the point is to hear the new one. Silent at
             * zero, which is itself the correct answer to "what does off sound like". */
            sound_play_effect(SFX_SETTING);
            break;
        }
        case ROW_MUSIC: {
            int step = (delta != 0) ? delta : (toggle ? 1 : 0);
            if (step == 0) break;
            int next = step_volume(app->settings.music_volume, step);
            if (next == app->settings.music_volume) break;
            app->settings.music_volume = next;
            music_set_volume(next);
            break;
        }
        case ROW_TRACK: {
            int step = (delta != 0) ? delta : (toggle ? 1 : 0);
            if (step == 0) break;
            /* The row moves now; the music starts when the row stops. See TRACK_SETTLE_S. */
            app->settings.music_track = step_track(app->settings.music_track, step);
            track_settle = TRACK_SETTLE_S;
            sound_play_effect(SFX_CURSOR);
            break;
        }
        case ROW_CLOCK:
            if (toggle) {
                sound_play_effect(SFX_SETTING);
                /* Behind the code, but only once there is one. The schedule is enforced against
                 * this clock and nothing else, so a child who can set the time can move bedtime
                 * -- which would make the whole "Playing allowed 8 am to 8 pm" row decoration.
                 * The panel is gated for the same reason and by the same call.
                 *
                 * Only when a code is set, because with none there is nothing to enforce and
                 * nothing to protect: making everyone key in a code to correct the date on a
                 * console whose owner never asked for parental controls would be a cost with no
                 * matching benefit. */
                if (parental_code_set()) {
                    screen_code_ask(CODE_ASK_UNLOCK, "Enter the code to set the clock",
                                    SCREEN_CLOCK, SCREEN_SETTINGS);
                    app_goto(app, SCREEN_CODE);
                } else {
                    app_goto(app, SCREEN_CLOCK);
                }
                return;
            }
            break;
        case ROW_PARENTAL:
            if (toggle) {
                sound_play_effect(SFX_SETTING);
                /* The code guards its own panel. Without this the lock list and the schedule are
                 * one press from any child who found Settings, and the feature is decoration --
                 * see the note at the top of screen_parental.c. */
                settings_save(&app->settings);
                if (parental_code_set()) {
                    screen_code_ask(CODE_ASK_UNLOCK, "Enter the parental code",
                                    SCREEN_PARENTAL, SCREEN_SETTINGS);
                    app_goto(app, SCREEN_CODE);
                } else {
                    app_goto(app, SCREEN_PARENTAL);
                }
                return;
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

/** A volume row: same left half as every other row, a run of blocks where the value would be.
 *
 * Blocks rather than a number because the two audio rows sit in a list of things that read as
 * words, and "6" in that column invites the question "out of what". Ten blocks answer it without
 * a second label. */
static void draw_volume_row (app_t *app, int idx, const char *label, int value) {
    const theme_t *th = app->theme;
    int y = LIST_Y + idx * ROW_H;

    if (idx == cursor) {
        ui_fill(LIST_X, y, LIST_W, ROW_H, th->panel_alt);
        ui_fill(LIST_X, y, ACCENT_BAR, ROW_H, th->tab_underline);
    }
    ui_label(LIST_X + 16, y + 23, LIST_W - 32, ALIGN_LEFT,
             idx == cursor ? STL_DEFAULT : STL_GRAY, label);

    /* The unfilled blocks take whichever of the two row backgrounds this row is NOT drawn on.
     * panel_alt alone would have been invisible on the selected row, because that is exactly what
     * the selection fills with -- so the empty half of the meter would disappear on the one row
     * where the reader is adjusting it and needs to see how much is left. */
    uint16_t empty = (idx == cursor) ? th->bg : th->panel_alt;

    int span = SOUND_SFX_VOLUME_MAX * (METER_W + METER_GAP) - METER_GAP;
    int mx = LIST_X + LIST_W - 16 - span;
    int my = y + (ROW_H - METER_H) / 2;
    for (int i = 0; i < SOUND_SFX_VOLUME_MAX; i++) {
        ui_fill(mx + i * (METER_W + METER_GAP), my, METER_W, METER_H,
                i < value ? th->tab_underline : empty);
    }
}

static void settings_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    char buf[96];

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "Settings");

    /* First, above Theme, because the theme belongs to whoever this row names -- reading the two
     * in order is what says so. The value is the active player, so the row answers "who am I"
     * without being opened. */
    draw_row(app, ROW_PROFILES, "Players", profile_name(profile_active()));
    draw_row(app, ROW_THEME, "Theme", th->name);
    draw_volume_row(app, ROW_SFX, "Sound effects", app->settings.sfx_volume);
    draw_volume_row(app, ROW_MUSIC, "Music", app->settings.music_volume);
    draw_row(app, ROW_TRACK, "Track", music_track_name(app->settings.music_track));

    /* The row shows the clock rather than the word "Clock" twice, because "is it right" is the
     * only question anyone opens this for. An unset clock reads as 1970 and saying so is more use
     * than "Not set" -- it tells the reader the console is answering, just wrongly. */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt != NULL && lt->tm_year + 1900 >= 2000) {
        strftime(buf, sizeof(buf), "%d %b %Y  %H:%M", lt);
    } else {
        snprintf(buf, sizeof(buf), "Not set");
    }
    draw_row(app, ROW_CLOCK, "Clock", buf);

    draw_row(app, ROW_PARENTAL, "Parental controls",
             parental_code_set() ? "On" : "Off");

    /* Just the build. There were two more lines here -- a library count and a cheat database
     * count -- on the theory that they answered the first support questions anyone would ask.
     * They do not: the grid already shows how many games there are, one tab at a time and with
     * their names on, and the cheat count answers a question nobody asks in the form "how many
     * games does the database cover". The build string is the only one of the three that cannot
     * be read off any other screen. */
    int y = LIST_Y + ROW_COUNT * ROW_H + 24;
    ui_fill(LIST_X, y, LIST_W, HAIRLINE, th->panel_alt);
    y += 22;

    snprintf(buf, sizeof(buf), "%s  %s", MENU_VERSION, BUILD_TIMESTAMP);
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_GRAY, "Build");
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_RIGHT, STL_GRAY, buf);
    y += 22;

    /* Whether the cheat engine actually runs on this console, which is the one thing about cheats
     * that cannot be established from the menu side and the one thing the first hardware run left
     * open -- everything observable said "will hook" and no cheat did anything in game.
     *
     * A line rather than a row, deliberately. It is a diagnostic, it is read and not changed, and
     * making it selectable would renumber every row above it for the fourth time and move thirteen
     * input scripts with them. See enginetest.h for what to do with the code it prints. */
    if (enginetest_seen()) {
        ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_GRAY, "Cheat engine");
        ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_RIGHT, STL_DEFAULT, "Confirmed running");
    } else {
        char code[32];
        enginetest_code(code, sizeof(code));
        snprintf(buf, sizeof(buf), "Untested, try %s", code);
        ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_GRAY, "Cheat engine");
        ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_RIGHT, STL_GRAY, buf);
    }

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
    .leave  = settings_leave,
    .update = settings_update,
    .render = settings_render,
};
