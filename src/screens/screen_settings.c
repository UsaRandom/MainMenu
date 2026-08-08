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
#include "menu/fonts.h"
#include "library/boxart.h"
#include "library/cache.h"
#include "library/thumbcache.h"
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

/**
 * @brief The pinned Build and Cheat engine lines below the rows.
 *
 * Those two are diagnostics rather than settings -- they are read, never changed -- so they stay
 * put while the rows move under them. Scrolling the build string off the bottom would make "what
 * version is this" a question about scroll position.
 */
#define INFO_GAP    24      /**< separator, and the air around it */
#define INFO_LINE_H 22
#define INFO_LINES  2       /**< Build, Cheat engine */
#define INFO_H      76      /**< reserved below the rows; must cover the lines above */

/**
 * @brief Rows visible at once, and the reason this screen scrolls at all.
 *
 * At seven rows the list ended at y=334 with the info block below it reaching 402, against a
 * footer at 424 -- twenty-two pixels of slack. The Credits row makes it eight and that slack is
 * gone: the info block would have been drawn straight through the footer.
 *
 * VISIBLE is derived from INFO_H, so the list can never overrun the footer by construction and
 * an assertion saying so would be a tautology -- it was written that way first and could not be
 * made to fail, which is the whole reason for checking. What *can* go wrong is the reservation
 * being too small for what is drawn into it: a third diagnostic line added without growing
 * INFO_H puts text under the footer, silently. That is what these two check, and both fail when
 * fed a number that would break them.
 */
#define LIST_H      (FOOTER_Y - LIST_Y - INFO_H)
#define VISIBLE     (LIST_H / ROW_H)

_Static_assert(VISIBLE >= 1, "settings list has no room for a single row");
_Static_assert(INFO_H >= INFO_GAP + INFO_LINES * INFO_LINE_H,
               "the info block draws more lines than the space reserved for it below the rows");

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
    ROW_BOXART,
    ROW_SFX,
    ROW_MUSIC,
    ROW_TRACK,
    ROW_CLOCK,
    ROW_PARENTAL,
    ROW_CREDITS,
    ROW_COUNT,
} row_t;

static int cursor;
static int top;         /**< first visible row; see VISIBLE */

/**
 * @brief Set on the way out to a screen that comes back here, so enter() keeps the cursor.
 *
 * Four rows open something -- the roster, the clock, the parental panel and the credits -- and
 * every one of them returns to SCREEN_SETTINGS, which re-enters this screen and used to reset
 * the cursor to the top. That was survivable while the list was seven rows and fitted on screen.
 * It stopped being survivable when Credits became the eighth row and the list started scrolling:
 * reading the licence and pressing B put you back at Players with the window scrolled home, so
 * getting back to where you were meant seven presses down through a list that moves under you.
 *
 * Measured on the frames from tools/inputs/credits.txt: before this, dump 7 was byte-identical to
 * dump 1 -- the whole round trip left no trace, which is exactly the complaint.
 */
static bool returning;
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
    if (!returning) {
        cursor = 0;
        top = 0;
    }
    returning = false;
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

    /* The window follows the cursor rather than the other way round, so a row is never selected
     * off screen. Same shape as the lock list, and it is inert while ROW_COUNT fits in VISIBLE. */
    if (cursor < top) {
        top = cursor;
    }
    if (cursor >= top + VISIBLE) {
        top = cursor - VISIBLE + 1;
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
                returning = true;
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
        case ROW_BOXART:
            /* Nothing to step through unless the card defines sections beyond the built-in one,
             * and a row that plays a confirmation sound and changes nothing is worse than a row
             * that does nothing. It still draws, because it is how anybody finds out that
             * boxart.ini exists -- see library/boxart.h. */
            if ((delta != 0 || toggle) && boxart_region_count() > 1) {
                sound_play_effect(SFX_SETTING);
                int n = boxart_region_count();
                int i = boxart_region_current() + (delta != 0 ? delta : 1);
                boxart_set_region(((i % n) + n) % n);

                free(app->settings.boxart_region);
                app->settings.boxart_region = strdup(boxart_region_name(boxart_region_current()));
                settings_save(&app->settings);

                /* Every tile in the pool is the old shape. Dropping them here rather than
                 * letting the grid discover it means the grid never draws a frame of art at a
                 * height its cell no longer is. */
                thumbcache_reshape(app->thumbs, app->lib);
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
                    returning = true;
                app_goto(app, SCREEN_CODE);
                } else {
                    returning = true;
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
                    returning = true;
                app_goto(app, SCREEN_CODE);
                } else {
                    returning = true;
                app_goto(app, SCREEN_PARENTAL);
                }
                return;
            }
            break;
        case ROW_CREDITS:
            if (toggle) {
                sound_play_effect(SFX_SETTING);
                /* Not behind the parental code and not behind anything else. The AGPL's offer of
                 * source has to be reachable by whoever is holding the console, and a licence a
                 * parent can lock away is not an offer. */
                returning = true;
                app_goto(app, SCREEN_CREDITS);
                return;
            }
            break;
        default:
            break;
    }
}

/**
 * @brief Y of row @p idx inside the scroll window, or a value off screen if it is not visible.
 *
 * Returning an off-screen coordinate rather than a flag keeps every caller a straight line: the
 * scissor around the list is what actually stops the drawing, so a row above or below the window
 * costs a few clipped primitives and no branch at each call site.
 */
static int row_y (int idx) {
    return LIST_Y + (idx - top) * ROW_H;
}

static void draw_row (app_t *app, int idx, const char *label, const char *value) {
    const theme_t *th = app->theme;
    int y = row_y(idx);

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
    int y = row_y(idx);

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
    /* Scissored to the window, so a row scrolling past either end is cut rather than drawn over
     * the header or the info block. The rows themselves are drawn unconditionally: at eight rows
     * and seven visible there is exactly one off screen, and a branch per row to save one clipped
     * fill would be the more complicated of the two. */
    rdpq_set_scissor(0, LIST_Y, SCREEN_W, LIST_Y + VISIBLE * ROW_H);

    draw_row(app, ROW_PROFILES, "Players", profile_name(profile_active()));
    draw_row(app, ROW_THEME, "Theme", th->name);
    draw_row(app, ROW_BOXART, "Box art", boxart_region_name(boxart_region_current()));
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

    /* Last, because it is the row nobody is looking for and the one row here that changes
     * nothing about the console. */
    draw_row(app, ROW_CREDITS, "Credits and licences", "");

    rdpq_set_scissor(0, 0, SCREEN_W, SCREEN_H);

    if (ROW_COUNT > VISIBLE) {
        int track_h = VISIBLE * ROW_H;
        int thumb = (track_h * VISIBLE) / ROW_COUNT;
        if (thumb < POSBAR_THUMB_MIN) {
            thumb = POSBAR_THUMB_MIN;
        }
        int travel = track_h - thumb;
        int pos = (travel * top) / (ROW_COUNT - VISIBLE);
        ui_fill(POSBAR_X, LIST_Y, POSBAR_W, track_h, th->panel);
        ui_fill(POSBAR_X, LIST_Y + pos, POSBAR_W, thumb, th->text_dim);
    }

    /* Just the build. There were two more lines here -- a library count and a cheat database
     * count -- on the theory that they answered the first support questions anyone would ask.
     * They do not: the grid already shows how many games there are, one tab at a time and with
     * their names on, and the cheat count answers a question nobody asks in the form "how many
     * games does the database cover". The build string is the only one of the three that cannot
     * be read off any other screen. */
    /* Anchored to the bottom of the window rather than to the number of rows. It was
     * LIST_Y + ROW_COUNT * ROW_H + 24, which is where the eighth row put it straight through the
     * footer -- the reason VISIBLE and its static assertion exist. */
    int y = LIST_Y + VISIBLE * ROW_H + INFO_GAP;
    ui_fill(LIST_X, y, LIST_W, HAIRLINE, th->panel_alt);
    y += INFO_LINE_H;

    snprintf(buf, sizeof(buf), "%s  %s", MENU_VERSION, BUILD_TIMESTAMP);
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_LEFT, STL_GRAY, "Build");
    ui_label(LIST_X + 16, y, LIST_W - 32, ALIGN_RIGHT, STL_GRAY, buf);
    y += INFO_LINE_H;

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
