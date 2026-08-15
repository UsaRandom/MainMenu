/**
 * @file screen_profiles.c
 * @brief Who's playing: ten slots, a face each, and the only screen that can delete a save.
 * @ingroup screens
 *
 * One screen, two ways in. At boot it asks; from the grid's player chip it edits. The difference
 * is a single flag, because the cards and every verb on them are the same either way -- a boot
 * picker that could not also rename would mean two screens drawing the same ten cards, and the
 * second one would be the one that drifted.
 *
 * ## Renaming was not actually possible
 *
 * The paragraph above was aspirational for a while. A name could only be set at the instant a slot
 * was created, on the way out of START or A-on-an-empty-card, and Z went straight to the appearance
 * editor under a footer reading "Edit" -- so anybody looking for a rename pressed the button that
 * said so, found colours, and concluded there was none. Z now opens two rows, Name and Icon and
 * colours, and the footer says both nouns. See #pmode_t for why it is a menu and not a second
 * button.
 *
 * ## Why it is a grid of cards and not a list of rows
 *
 * It was a list, and the list was right while a profile was a name. A profile is now a name and a
 * face, and a face in a 32 px row is a thumbnail nobody can tell apart from the next one. The
 * handoff's answer is a 5 x 2 grid of 112 x 158 cards -- 608 px wide, which is exactly #SAFE_W --
 * with a 64 px plate carrying a 40 px sprite. At that size the sprite is the thing you read and
 * the name is the confirmation, which is the right way round for a screen answering "which one
 * is me".
 *
 * ## It does not appear for one profile
 *
 * #screen_profiles_needed() is false when there is exactly one profile, and app.c uses it to
 * decide whether to boot into this screen or straight into the grid. A card nobody has added a
 * second player to therefore boots exactly as it did before this existed -- same first frame,
 * same number of presses to a game.
 *
 * That is now the *only* place the feature hides. The grid's player chip draws always, because it
 * is how an appearance is reached and a single-profile card would otherwise have no route to one.
 *
 * ## Slots are stable, and deleting takes the saves
 *
 * Removing a profile used to close the gap above it, and the slot number names the folder on
 * disk -- so deleting player 2 turned player 3's `saves/p3/` into player 2's. Slots are fixed
 * now: slot 4 stays slot 4 forever, an empty slot draws as a dashed "+ Empty" card, and the next
 * player to take it gets an empty folder because #profile_erase_saves ran when the last one left.
 *
 * The confirmation therefore says something it never had to before: **the saves go too**. It
 * counts them first and puts the number on screen, because "and 41 saved games" is a different
 * decision from "and no saved games" and the user is the only one who can tell which they are in.
 * The cursor starts on Keep.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "menu/fonts.h"
#include "menu/music.h"
#include "menu/profile.h"
#include "menu/sound.h"
#include "screens.h"
#include "screens/boot_plate.h"
#include "ui/draw.h"
#include "ui/icon.h"
#include "ui/theme.h"
#include "library/thumbstore.h"

/* Handoff section 5. Five columns of 112 with 12 between them is 608, which is SAFE_W to the
 * pixel -- the board was drawn to the same safe area this program already used. */
#define CARD_W      112
#define CARD_H      158
#define COL_GAP     12
#define ROW_GAP     14
#define GRID_ORG_X  16
#define GRID_ORG_Y  78
#define COLS        5
#define ROWS        2

#define PLATE       64
#define PLATE_TOP   24
#define NAME_GAP    16

/** Selected cards lift and gain an outline; unselected ones dim. Section 8: the chosen thing is
 *  the brightest thing, and nothing is distinguished by colour alone. */
#define LIFT        3
#define OUTLINE     2
#define SHADOW_DY   5
/** 0.66 brightness, as an alpha over the card. 255 - 0.66*255 = 87. */
#define DIM_ALPHA   87

/** The dashed edge of an empty slot. Fixed rather than themed, and this is the one place the
 *  handoff's #52525A is allowed: section 8 forbids it for *text* because it fails on a CRT, and a
 *  border that nearly disappears is exactly what an empty slot should look like. The "+ Empty"
 *  label beside it takes the theme's dim text, which the handoff floors at #737384. */
#define EMPTY_EDGE  RGBA5551(0x52, 0x52, 0x5A)

_Static_assert(GRID_ORG_X + COLS * CARD_W + (COLS - 1) * COL_GAP == SAFE_X + SAFE_W,
               "the profile card grid is not the width of the safe area");
_Static_assert(COLS * ROWS == PROFILE_MAX, "the card grid does not hold PROFILE_MAX slots");
_Static_assert(GRID_ORG_Y + ROWS * CARD_H + (ROWS - 1) * ROW_GAP <= FOOTER_Y,
               "the profile card grid overruns the footer");

/**
 * @brief What Z offers, and why it is a menu rather than a second button.
 *
 * Z used to go straight to the appearance editor and the footer called it "Edit", which meant a
 * player's *name* could only ever be set once -- at the moment the slot was created, on the way
 * out of START or A-on-an-empty-card. Get it wrong there and it was wrong forever. There was no
 * rename at all.
 *
 * The obvious fix is a second button, and the obvious second button is R. R is taken: it is how
 * this screen gets back onto the tab rail, which is the whole reason the rail stays drawn. Even
 * before that it was the one to avoid -- a rename keyboard opens pre-filled with the player's own
 * name and B deletes a letter, so a mis-press there silently shortens a name.
 *
 * So Z means what the footer already said it meant, and there are two things under it.
 */
typedef enum {
    MODE_GRID = 0,
    MODE_EDIT,
    MODE_CONFIRM_REMOVE,
    MODE_DELETING,              /**< the walk is running; drawn by erase_tick(), not the loop */
} pmode_t;

enum { EDIT_NAME = 0, EDIT_LOOK, EDIT_ROWS };

static bool    picking;         /**< entered at boot, so B has nowhere to go back to */
static pmode_t mode;
static int     cursor;          /**< slot 0..PROFILE_MAX-1 */
static int     confirm_keep;    /**< 1 = cursor on Keep, which is where it starts */
static int     edit_row;        /**< #EDIT_NAME or #EDIT_LOOK */

/** Where a name typed on the keyboard lands, and which slot asked for it. */
static char pending_name[PROFILE_NAME_CAP];
static int  pending_slot = -1;

bool screen_profiles_needed (void) {
    return profile_count() > 1;
}

void screen_profiles_ask (void) {
    picking = true;
}

static int card_x (int slot) {
    return GRID_ORG_X + (slot % COLS) * (CARD_W + COL_GAP);
}

static int card_y (int slot) {
    return GRID_ORG_Y + (slot / COLS) * (CARD_H + ROW_GAP);
}

/**
 * @brief Are the faces on the cards drawn yet?
 *
 * What the boot plate waits for when it is covering this screen, the way it waits for the first
 * row of covers when it is covering the grid. Slots whose sprite cannot resolve are skipped rather
 * than waited on: icon_get() answers NULL both for "not decoded yet" and for "no such icon in this
 * pack", and a capped build would otherwise hold the plate to its three-second ceiling every boot.
 */
static bool picker_worth_revealing (void) {
    if (icon_count() == 0) {
        return true;
    }
    for (int i = 0; i < PROFILE_MAX; i++) {
        uint16_t idx = profile_icon(i);
        if (!profile_slot_used(i) || idx == ICON_NONE || idx >= icon_count()) {
            continue;
        }
        if (icon_get(idx, ICON_SMALL, profile_colour_fill(profile_ink(i)),
                     profile_colour_fill(profile_plate(i))) == NULL) {
            return false;
        }
    }
    return true;
}

static void profiles_enter (app_t *app) {
    (void)app;
    mode = MODE_GRID;

    /* The boot plate, when this is the first screen there is. A card with several players used to
     * show these ten cards cold and unannounced, and only played the boot animation afterwards,
     * over the grid -- so the loading screen came *after* the thing it was supposed to be loading
     * for. Armed here instead; boot_plate_arm() is a no-op if the grid got there first, which is
     * what happens on a card with one player. */
    if (picking) {
        boot_plate_arm();
    }

    /* A name may have come back from the keyboard. Applied here rather than by the keyboard,
     * which does not know what it was typing for -- it writes a buffer and returns. */
    if (pending_slot >= 0) {
        if (pending_name[0] != '\0') {
            profile_set_name(pending_slot, pending_name);
            profile_save();
        }
        pending_slot = -1;
    } else {
        /* Opens on whoever is already active, so the common case -- boot, confirm it is still
         * you, press A -- is one press and no steering. Not reset when returning from the
         * keyboard or the appearance editor, which would throw away the slot being worked on. */
        cursor = profile_active();
    }
}

static void begin_rename (app_t *app, int slot, const char *prompt) {
    pending_slot = slot;
    pending_name[0] = '\0';
    /* The raw name, not profile_name(): the fallback "Player 3" is what an unnamed profile reads
     * as, not what it is called, and pre-filling the field with it would make every new player
     * start by deleting eight characters they never typed. */
    screen_keyboard_ask(KB_NAME, prompt, profile_name_raw(slot),
                        pending_name, sizeof(pending_name), SCREEN_PROFILES);
    app_goto(app, SCREEN_KEYBOARD);
}

static void choose (app_t *app, int slot) {
    if (slot != profile_active()) {
        profile_activate(slot, app->lib);
        /* The theme is the profile's, so it changes with them. theme_apply() and not just the
         * assignment, because the font styles are registered state that does not follow
         * app->theme on its own -- see the same pairing in screen_settings.c. */
        app->theme = theme_by_name(profile_theme(slot));
        theme_apply(app->theme);
    }
    picking = false;
    sound_play_effect(SFX_ENTER);
    app_goto(app, SCREEN_GRID);
}

static void update_grid (app_t *app) {
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B)) {
        /* Nowhere to go back to at boot: the question has to be answered, and answering it with
         * the profile already active is what A on the pre-selected card does. */
        if (picking) {
            sound_play_effect(SFX_ERROR);
            return;
        }
        sound_play_effect(SFX_EXIT);
        app_goto(app, SCREEN_GRID);
        return;
    }

    /* R is the way back onto the rail, mirroring L on the grid's first tab. Refused while the
     * boot question is up, for the same reason B is: there is no grid to go back to yet. */
    if (input_pressed(in, BTN_R)) {
        if (picking) {
            sound_play_effect(SFX_ERROR);
        } else {
            sound_play_effect(SFX_CURSOR);
            app_goto(app, SCREEN_GRID);
        }
        return;
    }

    int prev = cursor;
    if (in->left  && (cursor % COLS) > 0)        cursor--;
    if (in->right && (cursor % COLS) < COLS - 1) cursor++;
    if (in->up    && cursor >= COLS)             cursor -= COLS;
    if (in->down  && cursor < PROFILE_MAX - COLS) cursor += COLS;
    if (cursor != prev) {
        sound_play_effect(SFX_CURSOR);
    }

    bool used = profile_slot_used(cursor);

    if (input_pressed(in, BTN_A)) {
        if (used) {
            choose(app, cursor);
        } else {
            /* A on an empty card creates the profile there and goes straight to naming it, which
             * is the handoff's START-on-a-new-slot flow arriving from the other direction. The
             * slot is the one under the cursor rather than the next free one, because the card
             * being pressed is the promise being made. */
            if (profile_add_at(cursor)) {
                sound_play_effect(SFX_ENTER);
                begin_rename(app, cursor, "Name this player");
            } else {
                sound_play_effect(SFX_ERROR);
            }
        }
        return;
    }

    if (input_pressed(in, BTN_START)) {
        /* START makes a new player in the lowest free slot, wherever the cursor is. The handoff
         * offers both; this is the one for somebody who has not thought about slots. */
        int slot = profile_add();
        if (slot >= 0) {
            cursor = slot;
            sound_play_effect(SFX_ENTER);
            begin_rename(app, slot, "Name this player");
        } else {
            sound_play_effect(SFX_ERROR);
        }
        return;
    }

    if (input_pressed(in, BTN_Z) && used) {
        sound_play_effect(SFX_SETTING);
        edit_row = EDIT_NAME;
        mode = MODE_EDIT;
        return;
    }

    /* C-left removes, as it did on the list this replaced. Not A and not B: both of those are
     * one press away from being pressed by somebody scrolling, and this is the only irreversible
     * thing in the program. */
    if (input_pressed(in, BTN_CLEFT) && used) {
        if (cursor == 0) {
            /* Player one owns the unsuffixed `saves/` -- on a card that predates profiles, every
             * save on it. Removing them is not a thing this screen offers at any confirmation
             * depth. The refusal is a sound rather than a dialog, because explaining it would
             * mean explaining slot numbering to somebody who asked to delete a row. */
            sound_play_effect(SFX_ERROR);
            return;
        }
        /* No save count any more, and so no dry-run walk here. Counting was this same
         * one-probe-per-library-record walk over the card, run before the popup could appear --
         * a music-stopping stall spent pricing a deletion the user usually answers "Keep" to.
         * The dialog warns unconditionally instead, and the card is only walked by a Delete. */
        confirm_keep = 1;
        mode = MODE_CONFIRM_REMOVE;
        sound_play_effect(SFX_SETTING);
    }
}

static void update_edit (app_t *app) {
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B) || input_pressed(in, BTN_Z)) {
        /* Z closes it as well as opens it, so a press that was meant for the grid underneath does
         * not leave the user hunting for the way out of a two-line menu. */
        mode = MODE_GRID;
        sound_play_effect(SFX_EXIT);
        return;
    }
    if (in->up && edit_row > 0) {
        edit_row--;
        sound_play_effect(SFX_CURSOR);
    }
    if (in->down && edit_row < EDIT_ROWS - 1) {
        edit_row++;
        sound_play_effect(SFX_CURSOR);
    }
    if (!input_pressed(in, BTN_A)) {
        return;
    }

    /* Back to the grid first in both cases. Both destinations return here, and profiles_enter()
     * leaves the cursor where it was -- so coming back into an open menu would be a menu the user
     * has to dismiss after every edit. */
    mode = MODE_GRID;
    if (edit_row == EDIT_NAME) {
        sound_play_effect(SFX_ENTER);
        begin_rename(app, cursor, "Rename this player");
    } else {
        sound_play_effect(SFX_ENTER);
        screen_appearance_ask(cursor);
        app_goto(app, SCREEN_APPEARANCE);
    }
}

/* The app whose frame erase_tick() paints -- same pattern as screen_launch's progress_app: the
 * walk is one blocking call and the main loop is suspended above it, so either the tick draws or
 * nothing does. */
static app_t *erase_app;
static bool   delete_pending;   /**< set by the confirm, consumed by render(); see run_delete() */
static void profiles_render (app_t *app, surface_t *fb);

/**
 * Called from inside profile_erase_saves(), between card round-trips.
 *
 * The mixer first and EVERY time: the whole reason the callback exists is that a few hundred
 * serial FatFs probes outlast the 316 ms the audio queue holds, and sound_poll() is cheap when
 * there is nothing to top up. The paint is throttled the way boot_tick() throttles -- a probe is
 * a few milliseconds and painting each one would cost more than the walk -- and skips rather
 * than blocks when no framebuffer is free, because a dropped frame of "Deleting..." is invisible
 * and a stalled walk is not.
 */
static void erase_tick (void) {
    static uint32_t last_paint;

    sound_poll();
    music_poll();

    uint32_t now = TICKS_READ();
    if (last_paint != 0 && TIMER_MICROS(TICKS_DISTANCE(last_paint, now)) < 33000) {
        return;
    }
    surface_t *fb = display_try_get();
    if (fb == NULL) {
        return;
    }
    last_paint = now;
    profiles_render(erase_app, fb);
}

static void update_confirm (app_t *app) {
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B)) {
        mode = MODE_GRID;
        sound_play_effect(SFX_EXIT);
        return;
    }
    if (in->left || in->right) {
        confirm_keep = !confirm_keep;
        sound_play_effect(SFX_CURSOR);
    }
    if (!input_pressed(in, BTN_A)) {
        return;
    }
    if (confirm_keep) {
        mode = MODE_GRID;
        sound_play_effect(SFX_EXIT);
        return;
    }

    /* Only the MODE changes here. The walk itself runs from render(), after a frame has been
     * attached and shown -- NOT from this update handler, for the reason launch_update()
     * documents: the main loop calls display_try_get() BEFORE update(), so a blocking walk here
     * holds a framebuffer that is never attached or shown, and every paint erase_tick() attempts
     * finds no buffer free. That is not a theory: the first version of this blocked right here,
     * and the console showed a frozen confirm dialog for the whole walk with "Deleting..."
     * never drawn once. */
    mode = MODE_DELETING;
    delete_pending = true;
    sound_play_effect(SFX_ENTER);
}

/** The walk, run from render() after the "Deleting..." frame is already on screen. */
static void run_delete (app_t *app) {
    erase_app = app;

    /* The saves first, then the profile. That order matters: profile_erase_saves() needs the slot
     * to still exist to name its folder, and profile_remove() clears it. */
    int gone = profile_erase_saves(cursor, app->lib, erase_tick);
    /* profile_remove() writes too -- two cache drops and the ini -- so keep the queue topped
     * across it as well. */
    erase_tick();
    if (profile_remove(cursor)) {
        debugf("PROFILE removed slot %d, %d save file(s) deleted\n", cursor + 1, gone);
        /* If the deleted profile was the active one, profile_remove() moved it back to slot 0, so
         * the library in memory belongs to somebody else now and is re-keyed rather than left
         * showing the removed player's favourites. Nobody else moved -- that is the point of
         * stable slots -- so this is the only case. */
        profile_activate(profile_active(), app->lib);
        app->theme = theme_by_name(profile_theme(profile_active()));
        theme_apply(app->theme);
    } else {
        sound_play_effect(SFX_ERROR);
    }
    mode = MODE_GRID;
}

static void profiles_update (app_t *app, float dt) {
    /* The plate swallows input while it is up, so a button pressed during boot does not answer a
     * question the user cannot see yet. Same contract the grid has: the screen underneath goes on
     * updating, which is what makes the curtain a reveal rather than a cut. */
    if (boot_plate_step(dt, picker_worth_revealing())) {
        return;
    }
    switch (mode) {
        case MODE_EDIT:           update_edit(app);    break;
        case MODE_CONFIRM_REMOVE: update_confirm(app); break;
        case MODE_DELETING:       /* unreachable: the walk blocks inside update_confirm() and
                                   * leaves this mode before the loop runs again */ break;
        default:                  update_grid(app);    break;
    }
}

/* ------------------------------------------------------------------ drawing */

/** Ask the icon cache for every visible face, so a page of cards fills over a few frames rather
 *  than the cursor's one arriving and the rest staying blank. */
static void profiles_background (app_t *app, uint32_t budget) {
    (void)budget;
    /* The plate's hold is the boot budget, and it has to be spent here now rather than in the grid.
     * It was the grid that held the plate and therefore the grid that got a second of free
     * decoding out of it; with the plate moved in front of the picker, that second would have gone
     * nowhere and the grid would have arrived cold behind a curtain that had already lifted --
     * which is precisely the failure the plate exists to prevent, just relocated. So the covers
     * decode under the picker instead, and by the time somebody has answered "who's playing" the
     * first rows are painted. */
    if (boot_plate_working() && app->thumbs != NULL && app->lib != NULL) {
        /* Same split the grid uses: read what the atlas already holds, then spend what is left
         * building what it does not. Without an atlas -- ares -- neither works and the on-demand
         * decoder is the only thing that can put art on screen. See screen_grid's art_background. */
        if (thumbstore_available()) {
            thumbcache_fetch(app->thumbs, app->lib, DECODE_BUDGET_BOOT_US);
            thumbcache_build(app->thumbs, app->lib, DECODE_BUDGET_BOOT_US);
        } else {
            thumbcache_run(app->thumbs, app->lib, DECODE_BUDGET_BOOT_US);
        }
    }
    for (int i = 0; i < PROFILE_MAX; i++) {
        if (!profile_slot_used(i)) {
            continue;
        }
        icon_request(profile_icon(i), ICON_SMALL,
                     profile_colour_fill(profile_ink(i)),
                     profile_colour_fill(profile_plate(i)));
    }
}

/**
 * @brief The rail, or the question.
 *
 * Reached from the grid, this screen keeps the tab rail with the player chip lit: L and R still
 * walk it, so pressing L one too many times is not a trap you have to answer your way out of.
 * That replaced a "Who's playing? / 2 of 10 used" header, which said what the cards already say
 * and cost the rail its place.
 *
 * At boot there is no rail to keep, because there is no grid behind this yet and R is refused
 * until the question is answered -- so drawing one would advertise a move that does not work.
 * That is the only case the header survives for, and it is the one place the words belong.
 */
static void draw_header (app_t *app) {
    const theme_t *th = app->theme;

    if (!picking) {
        screen_grid_draw_rail(app, true);
        return;
    }
    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text_font(FNT_KEY, SAFE_X, 42, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "Who's playing?");
}

/**
 * @brief One dashed edge, landing a whole dash on each end.
 *
 * The pattern is stretched to fit rather than truncated. Stepping a fixed 12 px and clipping the
 * last dash to what was left put a two-pixel stub in the corner of a 158 px edge with a six-pixel
 * hole in front of it -- so the bottom of every empty card read as unfinished, which is exactly
 * what it looked like.
 *
 * Horizontal dashes are #HAIRLINE tall and vertical ones one pixel wide, and the asymmetry is not
 * a slip. The VI scans two framebuffer rows per scanline and shows only the even ones, so a
 * one-pixel horizontal dash is invisible half the time -- see the note on HAIRLINE. Columns are
 * not decimated: the output area is 640 px wide against a 640 px framebuffer, so a vertical
 * hairline is a hairline.
 */
static void dashed_edge (int origin, int len, int fixed, bool vertical, uint16_t c) {
    const int DASH = 6, STEP = 12;
    int n = (len - DASH + STEP / 2) / STEP;      /* gaps, so n+1 dashes */
    if (n < 1) {
        n = 1;
    }
    for (int i = 0; i <= n; i++) {
        int at = origin + ((len - DASH) * i) / n;
        if (vertical) {
            ui_fill(fixed, at, 1, DASH, c);
        } else {
            ui_fill(at, fixed, DASH, HAIRLINE, c);
        }
    }
}

/** A dashed rectangle, for an empty slot. Drawn from dashes rather than as a solid border because
 *  a solid one reads as a card that is there and merely blank.
 *
 *  The bottom edge hangs from y + h - HAIRLINE rather than y + h - 1, so a thicker hairline grows
 *  inwards and the card keeps the height its neighbours were laid out against. */
static void dashed_rect (int x, int y, int w, int h, uint16_t c) {
    dashed_edge(x, w, y,                false, c);
    dashed_edge(x, w, y + h - HAIRLINE, false, c);
    dashed_edge(y, h, x,                true,  c);
    dashed_edge(y, h, x + w - 1,        true,  c);
}

static void draw_card (app_t *app, int slot, bool selected) {
    const theme_t *th = app->theme;
    int x = card_x(slot);
    int y = card_y(slot) - (selected ? LIFT : 0);

    if (!profile_slot_used(slot)) {
        dashed_rect(x, y, CARD_W, CARD_H, EMPTY_EDGE);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
        ui_text_font(FNT_KEY, x, y + CARD_H / 2, CARD_W, ALIGN_CENTER, STL_GRAY, "+");
        ui_text(x, y + CARD_H / 2 + 28, CARD_W, ALIGN_CENTER, STL_GRAY, "Empty");
        if (selected) {
            ui_border(x - OUTLINE, y - OUTLINE, CARD_W + OUTLINE * 2, CARD_H + OUTLINE * 2,
                      OUTLINE, th->text);
        }
        return;
    }

    if (selected) {
        ui_fill(x, y + CARD_H, CARD_W, SHADOW_DY, th->sel_shadow);
    }
    ui_fill(x, y, CARD_W, CARD_H, th->panel);

    uint16_t fill = profile_colour_fill(profile_plate(slot));
    uint16_t ink  = profile_colour_fill(profile_ink(slot));
    int px = x + (CARD_W - PLATE) / 2;
    int py = y + PLATE_TOP;

    ui_fill(px, py, PLATE, PLATE, fill);
    const surface_t *pix = icon_get(profile_icon(slot), ICON_SMALL, ink, fill);
    if (pix != NULL) {
        rdpq_set_mode_copy(false);
        rdpq_tex_blit(pix, px + (PLATE - ICON_SMALL) / 2, py + (PLATE - ICON_SMALL) / 2, NULL);
    }

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    /* The whole card wide, not CARD_W - 8. "Player 10" measures 105 px of ink against a 104 px box
     * and lost its last glyph, so slot 10's card read "Player 1" -- the same clipping the fallback
     * buffer had just been widened to stop, moved from the string into the box. Measured off the
     * framebuffer, not estimated: tools/inputs/tenplayers.txt is the frame that shows it.
     *
     * 112 leaves 7 px. A name of eight wide capitals could still overrun it, which is a thing
     * somebody would do to themselves and see immediately; "Player 10" is a thing the menu does to
     * a card that has ten players on it. */
    ui_text(x, py + PLATE + NAME_GAP + 16, CARD_W, ALIGN_CENTER,
            selected ? STL_DEFAULT : STL_GRAY, profile_name(slot));

    if (slot == profile_active()) {
        /* A bar under the active player's card, the same accent the tab rail uses for the
         * selected tab. Without it the boot picker cannot say which profile it opened on -- the
         * cursor is where you are, not who you are, and after one press of Right they differ. */
        ui_fill(x, y + CARD_H - ACCENT_BAR, CARD_W, ACCENT_BAR, th->tab_underline);
    }

    if (!selected) {
        /* 0.66 brightness. A wash over the finished card rather than dimmer colours per element,
         * so the plate, the sprite and the name all dim by the same amount and the swatch stays
         * recognisable as itself. */
        ui_wash(x, y, CARD_W, CARD_H, 0x0001, DIM_ALPHA);
    } else {
        ui_border(x - OUTLINE, y - OUTLINE, CARD_W + OUTLINE * 2, CARD_H + OUTLINE * 2,
                  OUTLINE, 0xFFFF);
    }
}

static void draw_footer (app_t *app) {
    const theme_t *th = app->theme;
    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);

    int hx = SAFE_X;
    hx = ui_hint(hx, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC,
                 profile_slot_used(cursor) ? "Select" : "New");
    if (profile_slot_used(cursor)) {
        /* "Name or icon", not "Edit". The old label was accurate about the button and useless
         * about the feature: it went straight to the appearance editor, so somebody looking for a
         * rename read "Edit", found only colours, and concluded there was no rename. Saying both
         * nouns is the whole fix for that -- the menu behind it is only how they fit. */
        hx = ui_hint(hx, FOOTER_Y + 14, "Z", BTN_Z_COLOR, UI_BTN_TALL, "Name or icon");
        if (cursor != 0) {
            hx = ui_hint(hx, FOOTER_Y + 14, "<", BTN_C_COLOR, UI_BTN_DISC, "Remove");
        }
    }
    (void)ui_hint(hx, FOOTER_Y + 14, "S", BTN_START_COLOR, UI_BTN_DISC, "New player");
}

static void draw_edit (app_t *app) {
    const theme_t *th = app->theme;
    char line[64];

    ui_wash(0, 0, SCREEN_W, SCREEN_H, 0x0001, 180);

    const int W = 420, H = 178;   /* title, two 40 px rows, and 16 px of margin */
    const int X = (SCREEN_W - W) / 2, Y = (SCREEN_H - H) / 2;
    ui_fill(X, Y, W, H, th->panel);
    ui_border(X, Y, W, H, 2, th->text_dim);

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    snprintf(line, sizeof(line), "Edit %s", profile_name(cursor));
    ui_text(X + 24, Y + 42, W - 48, ALIGN_CENTER, STL_DEFAULT, line);

    /* The name row shows the name as it will be typed, not as it reads: an unnamed profile shows
     * nothing rather than "Player 3", because "Player 3" is the placeholder and offering to rename
     * it to itself would be a lie about what is in the field. */
    const char *raw = profile_name_raw(cursor);
    const char *rows[EDIT_ROWS] = { "Name", "Icon and colours" };
    char detail[EDIT_ROWS][32];
    snprintf(detail[EDIT_NAME], sizeof(detail[0]), "%s", (raw[0] != '\0') ? raw : "Not set");
    snprintf(detail[EDIT_LOOK], sizeof(detail[0]), "%s",
             profile_colour_name(profile_plate(cursor)));

    /* Outlined, not inverted. The selected row was a white plate with near-black text on it, and
     * near-black on white is the one combination this display does worst -- 32 levels a channel
     * and a CRT's bloom turn small dark glyphs on a bright field into a smear. The row keeps the
     * panel behind it and gains a border; the text just brightens. */
    const int RH = 40, R0 = Y + 74;
    for (int i = 0; i < EDIT_ROWS; i++) {
        int ry = R0 + i * (RH + 8);
        bool here = (i == edit_row);
        if (here) {
            ui_fill(X + 20, ry, W - 40, RH, th->panel_alt);
            ui_border(X + 20, ry, W - 40, RH, 2, th->text);
        }
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
        ui_text(X + 34, ry + 26, W - 68, ALIGN_LEFT,
                here ? STL_DEFAULT : STL_GRAY, rows[i]);
        ui_text(X + 34, ry + 26, W - 68, ALIGN_RIGHT, STL_GRAY, detail[i]);
    }
}

static void draw_confirm (app_t *app) {
    const theme_t *th = app->theme;
    char line[128];

    ui_wash(0, 0, SCREEN_W, SCREEN_H, 0x0001, 180);

    /* 220 while there were three lines of text in it. Losing the one about slots and
     * renumbering left a third of the panel empty above the buttons, which reads as a dialog
     * that failed to draw something. */
    const int W = 520, H = 176;
    const int X = (SCREEN_W - W) / 2, Y = (SCREEN_H - H) / 2;
    ui_fill(X, Y, W, H, th->panel);
    ui_border(X, Y, W, H, 2, th->text_dim);

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);

    snprintf(line, sizeof(line), "Remove %s?", profile_name(cursor));
    ui_text(X + 24, Y + 44, W - 48, ALIGN_CENTER, STL_DEFAULT, line);

    /* Unconditional, where a count used to be. The count was honest but it was priced in card
     * time: producing it meant running the same per-record walk the deletion runs, before the
     * popup could appear, and the stall stopped the music. A warning that is always true costs
     * nothing and still tells the person the one thing they must know before answering. */
    ui_text(X + 24, Y + 86, W - 48, ALIGN_CENTER, STL_ORANGE,
            "Their saved games on this card go too.");

    /* Both buttons keep the same plate and the chosen one is outlined, for the same reason the
     * edit rows are: the selected one used to be a white fill with near-black text, which is the
     * worst thing this display renders. Which button is dangerous is carried by the word and by
     * the colour of the word, not by which one is lit. */
    const int BW = 180, BH = 44, BY = Y + H - 68;
    int kx = X + 40, dx = X + W - 40 - BW;

    ui_fill(kx, BY, BW, BH, th->panel_alt);
    if (confirm_keep) {
        ui_border(kx, BY, BW, BH, 3, th->text);
    }
    ui_text(kx, BY + 29, BW, ALIGN_CENTER, confirm_keep ? STL_DEFAULT : STL_GRAY, "Keep");

    ui_fill(dx, BY, BW, BH, th->panel_alt);
    if (!confirm_keep) {
        ui_border(dx, BY, BW, BH, 3, th->text);
    }
    ui_text(dx, BY + 29, BW, ALIGN_CENTER, STL_ORANGE, "Delete");
}

/** The card shown while the walk runs. Drawn by erase_tick()'s frames, so the dots have to be a
 *  pure function of the clock -- there is no update() running to animate them. */
static void draw_deleting (app_t *app) {
    const theme_t *th = app->theme;
    char line[64];

    const int W = 520, H = 176;
    const int X = (SCREEN_W - W) / 2, Y = (SCREEN_H - H) / 2;
    ui_fill(X, Y, W, H, th->panel);
    ui_border(X, Y, W, H, 2, th->text_dim);

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);

    snprintf(line, sizeof(line), "Removing %s", profile_name(cursor));
    ui_text(X + 24, Y + 44, W - 48, ALIGN_CENTER, STL_DEFAULT, line);

    /* One to four dots on a 300 ms clock. Left-aligned from the centre so the line does not
     * shuffle sideways as the dots come and go. */
    int dots = 1 + (int)((TICKS_READ() / TICKS_FROM_MS(300)) % 4);
    snprintf(line, sizeof(line), "Deleting saves%.*s", dots, "....");
    ui_text(X + W / 2 - 90, Y + 96, 200, ALIGN_LEFT, STL_ORANGE, line);
}

static void profiles_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    draw_header(app);
    /* Unselected first, selected last: it lifts, outlines and casts a shadow, all of which
     * overlap its neighbours and have to land on top of them. */
    for (int i = 0; i < PROFILE_MAX; i++) {
        if (i != cursor) {
            draw_card(app, i, false);
        }
    }
    draw_card(app, cursor, true);
    draw_footer(app);

    if (mode == MODE_EDIT) {
        draw_edit(app);
    } else if (mode == MODE_CONFIRM_REMOVE) {
        draw_confirm(app);
    } else if (mode == MODE_DELETING) {
        draw_deleting(app);
    }

    /* Last, and inside the attach: render() owns the framebuffer from attach to detach, so there
     * is no later moment for anything to draw over this screen. */
    boot_plate_draw(MENU_VERSION, app->lib != NULL ? app->lib->count : 0);

    rdpq_detach_show();

    /* The delete runs HERE, after the frame above -- which is the first "Deleting..." card -- has
     * been shown and its buffer released. screen_launch.c's do_load() has the same shape for the
     * same reason: blocking work belongs after a detach_show, where erase_tick() can win a
     * framebuffer for its progress frames. Guarded on delete_pending rather than on the mode,
     * because erase_tick() re-enters this render for every frame it paints and must not start a
     * second walk from inside the first. */
    if (delete_pending) {
        delete_pending = false;
        run_delete(app);
    }
}

const screen_t SCREEN_PROFILES_DEF = {
    .id         = SCREEN_PROFILES,
    .enter      = profiles_enter,
    .update     = profiles_update,
    .render     = profiles_render,
    .background = profiles_background,
};
