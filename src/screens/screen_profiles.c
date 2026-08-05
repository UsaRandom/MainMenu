/**
 * @file screen_profiles.c
 * @brief Who is playing: the boot picker, and the list that manages it.
 * @ingroup screens
 *
 * One screen, two ways in. At boot it asks; from Settings it edits. The difference is a single
 * flag, because the list, the rows and every verb on them are the same either way -- a boot
 * picker that could not also rename would mean two screens drawing the same list, and the second
 * one would be the one that drifted.
 *
 * ## It does not appear for one profile
 *
 * #screen_profiles_needed() is false when there is exactly one profile, and app.c uses it to
 * decide whether to boot into this screen or straight into the grid. A card that nobody has added
 * a second player to therefore boots exactly as it did before this existed -- same first frame,
 * same number of presses to a game. The whole feature stays invisible until it is asked for,
 * which is the only reason it is affordable to put a screen in front of the grid at all.
 *
 * ## Renaming, without a keyboard
 *
 * The same per-character odometer screen_cheatedit.c uses for cheat names: Left and Right pick a
 * cell, Up and Down wind it through the alphabet. It is not fast and it does not need to be --
 * a name is typed once, and the alternative is an on-screen keyboard that would be the largest
 * single piece of UI in the program in order to be used twice per household.
 *
 * ## Deleting
 *
 * Confirmed, and the confirmation says the two true things that are not obvious: the saves stay
 * on the card, and every profile above this one shifts down a slot. The second is the surprising
 * one -- slot numbers name the folders, so deleting player 2 makes player 3's `saves/p3/` into
 * player 2's `saves/p2/`. Nothing on disk moves, so the saves follow the slot rather than the
 * person. That is worth one sentence on screen rather than a support question later.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "library/cache.h"
#include "menu/fonts.h"
#include "menu/profile.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

#define LIST_X      SAFE_X
#define LIST_Y      92
#define LIST_W      SAFE_W
#define ROW_H       32

/** @brief Cells in the name editor. One short of the buffer, which holds the terminator. */
#define NAME_CELLS  (PROFILE_NAME_CAP - 1)
#define CELL_W      26

/** Same set as the cheat name editor, and deliberately the same order: anyone who has typed one
 *  already knows which way Up goes. Space first so a name can be shortened from the right. */
static const char ALPHABET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-'!";
#define ALPHA_N ((int)(sizeof(ALPHABET) - 1))

typedef enum {
    MODE_LIST = 0,
    MODE_RENAME,
    MODE_CONFIRM_REMOVE,
} pmode_t;

static bool   picking;          /**< entered at boot, so B has nowhere to go back to */
static pmode_t mode;
static int    cursor;
static char   name[PROFILE_NAME_CAP];
static int    name_cursor;

bool screen_profiles_needed (void) {
    return profile_count() > 1;
}

void screen_profiles_ask (void) {
    picking = true;
}

/** @brief Rows in the list: every profile, then "Add a player" if there is room. */
static int row_count (void) {
    return profile_count() + (profile_count() < PROFILE_MAX ? 1 : 0);
}

static bool on_add_row (void) {
    return cursor >= profile_count();
}

static void profiles_enter (app_t *app) {
    (void)app;
    mode = MODE_LIST;
    /* Opens on whoever is already active, so the common case -- boot, confirm it is still you,
     * press A -- is one press and no steering. */
    cursor = profile_active();
}

/** @brief Start editing @p index's name. */
static void begin_rename (int index) {
    mode = MODE_RENAME;
    name_cursor = 0;

    /* The raw name, not profile_name(): the fallback "Player 3" is what an unnamed profile reads
     * as, not what it is called, and pre-filling the editor with it would make every new player
     * start by deleting eight characters they never typed. */
    snprintf(name, sizeof(name), "%s", profile_name_raw(index));

    /* Padded to full width so every cell is editable. Without this, winding right past the end of
     * a short name lands on the terminator and typing there leaves a hole in the string. */
    for (int i = (int)strlen(name); i < NAME_CELLS; i++) {
        name[i] = ' ';
    }
    name[NAME_CELLS] = '\0';
}

static void commit_rename (void) {
    profile_set_name(cursor, name);     /* trims the padding back off; see profile_set_name() */
    profile_save();
    mode = MODE_LIST;
    sound_play_effect(SFX_ENTER);
}

/** @brief Switch to @p index and leave. */
static void choose (app_t *app, int index) {
    if (index != profile_active()) {
        profile_activate(index, app->lib);
        /* The theme is the profile's, so it changes with them. theme_apply() and not just the
         * assignment, because the font styles are registered state that does not follow
         * app->theme on its own -- see the same pairing in screen_settings.c. */
        app->theme = theme_by_name(profile_theme(index));
        theme_apply(app->theme);
    }
    picking = false;
    sound_play_effect(SFX_ENTER);
    app_goto(app, SCREEN_GRID);
}

static void update_list (app_t *app) {
    const input_t *in = &app->input;
    int rows = row_count();

    if (input_pressed(in, BTN_B)) {
        /* Nowhere to go back to at boot: the question has to be answered, and answering it with
         * the profile already active is what A on the pre-selected row does. From Settings, B is
         * Back as it is everywhere else. */
        if (picking) {
            choose(app, cursor);
        } else {
            sound_play_effect(SFX_EXIT);
            app_goto(app, SCREEN_SETTINGS);
        }
        return;
    }

    int prev = cursor;
    if (in->up   && cursor > 0)        cursor--;
    if (in->down && cursor < rows - 1) cursor++;
    if (cursor != prev) {
        sound_play_effect(SFX_CURSOR);
    }

    if (input_pressed(in, BTN_A)) {
        if (on_add_row()) {
            int added = profile_add();
            if (added >= 0) {
                /* Added, then named, in one go. A row called "Player 4" that the user has to
                 * discover a second button to rename is a row most people would leave alone. */
                cursor = added;
                begin_rename(added);
                sound_play_effect(SFX_ENTER);
            } else {
                sound_play_effect(SFX_ERROR);
            }
        } else {
            choose(app, cursor);
        }
        return;
    }

    if (on_add_row()) {
        return;
    }

    if (input_pressed(in, BTN_CRIGHT)) {
        begin_rename(cursor);
        sound_play_effect(SFX_SETTING);
    }

    if (input_pressed(in, BTN_CLEFT)) {
        /* Profile 1 owns the unsuffixed paths and cannot go -- profile_remove() refuses it too,
         * but refusing silently after a press reads as a broken button. */
        if (cursor == 0) {
            sound_play_effect(SFX_ERROR);
        } else {
            mode = MODE_CONFIRM_REMOVE;
            sound_play_effect(SFX_SETTING);
        }
    }
}

static void update_rename (app_t *app) {
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B)) {
        mode = MODE_LIST;
        sound_play_effect(SFX_EXIT);
        return;
    }
    if (input_pressed(in, BTN_A) || input_pressed(in, BTN_START)) {
        commit_rename();
        return;
    }

    if (in->right) {
        name_cursor = (name_cursor + 1) % NAME_CELLS;
        sound_play_effect(SFX_CURSOR);
    }
    if (in->left) {
        name_cursor = (name_cursor + NAME_CELLS - 1) % NAME_CELLS;
        sound_play_effect(SFX_CURSOR);
    }

    int step = (in->up ? 1 : 0) - (in->down ? 1 : 0);
    if (step != 0) {
        const char *p = strchr(ALPHABET, name[name_cursor]);
        int idx = (p != NULL) ? (int)(p - ALPHABET) : 0;
        name[name_cursor] = ALPHABET[(idx + step + ALPHA_N) % ALPHA_N];
        sound_play_effect(SFX_SETTING);
    }
}

static void update_confirm (app_t *app) {
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B)) {
        mode = MODE_LIST;
        sound_play_effect(SFX_EXIT);
        return;
    }
    if (input_pressed(in, BTN_A)) {
        int removed = cursor;
        if (profile_remove(removed)) {
            sound_play_effect(SFX_ENTER);
            /* profile_remove() may have moved the active profile -- either because it was the one
             * deleted, or because it sat above the hole and shifted down. Either way the library
             * in memory now belongs to somebody else's slot, so it is re-keyed rather than left
             * showing the removed player's favourites. */
            profile_activate(profile_active(), app->lib);
            app->theme = theme_by_name(profile_theme(profile_active()));
            theme_apply(app->theme);
        } else {
            sound_play_effect(SFX_ERROR);
        }
        if (cursor >= row_count()) {
            cursor = row_count() - 1;
        }
        mode = MODE_LIST;
    }
}

static void profiles_update (app_t *app, float dt) {
    (void)dt;
    switch (mode) {
        case MODE_RENAME:         update_rename(app);  break;
        case MODE_CONFIRM_REMOVE: update_confirm(app); break;
        default:                  update_list(app);    break;
    }
}

/* ------------------------------------------------------------------ drawing */

static void draw_header (const theme_t *th) {
    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);

    /* The boot question and the settings heading are not the same sentence. "Who's playing?" is
     * asking; "Players" is a list of things you may change. */
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT,
             (mode == MODE_LIST && picking) ? "Who's playing?" : "Players");

    if (mode == MODE_LIST) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d of %d", profile_count(), PROFILE_MAX);
        ui_label(SAFE_X, 36, SAFE_W, ALIGN_RIGHT, STL_GRAY, buf);
    }
}

static void draw_footer (const theme_t *th) {
    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);

    int y = FOOTER_Y + 14;
    const char *back = "Back";

    switch (mode) {
        case MODE_RENAME: {
            int x = ui_hint(SAFE_X, y, "A", BTN_A_COLOR, UI_BTN_DISC, "Done");
            (void)ui_hint(x + 20, y, "^", BTN_C_COLOR, UI_BTN_DISC, "Letter");
            back = "Cancel";
            break;
        }
        case MODE_CONFIRM_REMOVE:
            (void)ui_hint(SAFE_X, y, "A", BTN_A_COLOR, UI_BTN_DISC, "Remove");
            back = "Keep";
            break;
        default: {
            /* The verbs offered are the verbs that will work on the row the cursor is on. A
             * "Remove" hint above Player 1 -- which can never be removed, because it owns the
             * unsuffixed paths -- would be advertising a button whose only response is an error
             * sound, and the same goes for renaming the "Add a player" row. */
            int x = ui_hint(SAFE_X, y, "A", BTN_A_COLOR, UI_BTN_DISC,
                            on_add_row() ? "Add" : "Play as");
            if (!on_add_row()) {
                x = ui_hint(x + 20, y, ">", BTN_C_COLOR, UI_BTN_DISC, "Rename");
                if (cursor > 0) {
                    x = ui_hint(x + 20, y, "<", BTN_C_COLOR, UI_BTN_DISC, "Remove");
                }
            }
            (void)x;
            /* Three hints, so the right-hand label has to stay short. The other screens append
             * "(not saved to card)" here and it does not fit beside three: it drew straight
             * through the Remove hint. The warning moved into the list area instead, where it is
             * more visible anyway -- see draw_list(). */
            break;
        }
    }

    ui_button(SAFE_X + SAFE_W - UI_BTN_D, y, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, y + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT, STL_GRAY, back);
}

static void draw_list (const theme_t *th) {
    char buf[64];

    /* A card that cannot be written takes every one of these and forgets it at power off. Said
     * here rather than in the footer, which on this screen already carries three hints -- and
     * this is the one screen where the warning matters most, because a roster that does not
     * persist is a feature that does not work rather than a preference that resets. */
    if (!cache_writable()) {
        ui_label(LIST_X, FOOTER_Y - 20, LIST_W, ALIGN_CENTER, STL_YELLOW,
                 "Read-only card: players reset at power off.");
    }

    for (int i = 0; i < row_count(); i++) {
        int y = LIST_Y + i * ROW_H;
        bool sel = (i == cursor);

        if (sel) {
            ui_fill(LIST_X, y, LIST_W, ROW_H, th->panel_alt);
        }

        if (i >= profile_count()) {
            ui_label(LIST_X + 16, y + 22, LIST_W - 32, ALIGN_LEFT,
                     sel ? STL_DEFAULT : STL_GRAY, "Add a player");
            continue;
        }

        ui_label(LIST_X + 16, y + 22, LIST_W - 200, ALIGN_LEFT,
                 sel ? STL_DEFAULT : STL_GRAY, profile_name(i));

        /* Which folder this player's saves are in, spelled out rather than implied. Somebody
         * copying a save off the card on a computer has no other way to find out, and the answer
         * for player 1 -- the plain saves folder, no suffix -- is the one people assume is wrong. */
        if (i == 0) {
            snprintf(buf, sizeof(buf), "saves/");
        } else {
            snprintf(buf, sizeof(buf), "saves/p%d/", i + 1);
        }
        ui_label(LIST_X, y + 22, LIST_W - 16, ALIGN_RIGHT,
                 i == profile_active() ? STL_YELLOW : STL_GRAY, buf);
    }
}

static void draw_rename (const theme_t *th) {
    char one[2] = { 0, 0 };

    ui_label(SAFE_X, LIST_Y + 20, SAFE_W, ALIGN_CENTER, STL_GRAY, "Name this player");

    int span = NAME_CELLS * CELL_W;
    int ox = (SCREEN_W - span) / 2;
    int y = LIST_Y + 90;

    for (int i = 0; i < NAME_CELLS; i++) {
        int cx = ox + i * CELL_W;
        if (i == name_cursor) {
            ui_fill(cx - 2, y - 20, CELL_W, 26, th->text_accent);
        }
        one[0] = name[i];
        ui_label(cx, y, CELL_W, ALIGN_LEFT, i == name_cursor ? STL_ONBTN : STL_DEFAULT, one);
    }
    ui_fill(ox, y + 6, span, HAIRLINE, th->panel_alt);
}

static void draw_confirm (const theme_t *th) {
    char buf[96];
    (void)th;

    snprintf(buf, sizeof(buf), "Remove %s?", profile_name(cursor));
    ui_label(SAFE_X, LIST_Y + 40, SAFE_W, ALIGN_CENTER, STL_DEFAULT, buf);

    ui_label(SAFE_X, LIST_Y + 90, SAFE_W, ALIGN_CENTER, STL_GRAY,
             "Their saves stay on the card.");
    ui_label(SAFE_X, LIST_Y + 120, SAFE_W, ALIGN_CENTER, STL_GRAY,
             "Players below them move up a slot, and their save");
    ui_label(SAFE_X, LIST_Y + 146, SAFE_W, ALIGN_CENTER, STL_GRAY,
             "folders do not follow.");
}

static void profiles_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    draw_header(th);

    switch (mode) {
        case MODE_RENAME:         draw_rename(th);  break;
        case MODE_CONFIRM_REMOVE: draw_confirm(th); break;
        default:                  draw_list(th);    break;
    }

    draw_footer(th);
    rdpq_detach_show();
}

const screen_t SCREEN_PROFILES_DEF = {
    .id     = SCREEN_PROFILES,
    .enter  = profiles_enter,
    .update = profiles_update,
    .render = profiles_render,
};
