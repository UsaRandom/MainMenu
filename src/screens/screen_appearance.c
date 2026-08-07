/**
 * @file screen_appearance.c
 * @brief Choosing a face: a category list, a page of sprites, and two colours.
 * @ingroup screens
 *
 * The handoff calls this frame 2a, "browse by category". There is a second frame on the board --
 * 1a, EDIT, APPEARANCE, a flat sixteen-icon picker -- which is kept for history and is explicitly
 * superseded. This is 2a, and it has grown a second colour row the board does not have.
 *
 * ## Browsing 3,894 things with a d-pad
 *
 * Filename order is useless: 163 pages of `abstract-01` through `zombie`. So every icon carries a
 * hand-written category, baked into `icons.meta` by tools/mkiconmeta.py, and the grid shows one
 * category at a time -- 45 cells to a page, L and R to page within it.
 *
 * The largest category is 276 icons, so no category is more than seven pages. The whole corpus
 * would have been 87.
 *
 * ## Two colours, not one
 *
 * The board gives a profile a single swatch and derives the artwork colour from it by a fixed
 * pairing -- light plates get dark artwork, dark plates get light. That is a good default and a
 * poor rule: it means there is no way to ask for white-on-red rather than the red-on-white the
 * table decided, and the choice a person is actually making about their own card is half made
 * for them.
 *
 * So there are two rows, both drawing from the same nine-colour palette, and they are labelled.
 * The pairing survives as #profile_default_ink, which is what a new slot gets and what a card
 * written before the two could differ is read as -- so nothing already on a card changes
 * appearance, the choice simply becomes reachable.
 *
 * Three separate things say what is chosen, because the first version had only the weakest of
 * them and was unreadable: the chosen swatch is a third taller than its neighbours, it is ringed
 * in white when its row has the cursor and in grey when it does not, and the colour is named in
 * words at the end of the row. The footer says which way the d-pad goes from wherever the cursor
 * is -- the rows are reached by walking off the bottom of the icon grid, which is not a thing
 * anybody guesses.
 *
 * Picking the same colour twice would make the artwork vanish into its own plate. #apply refuses
 * that rather than preventing it in the cursor: a swatch the cursor cannot land on is a rule
 * nothing on screen explains, and refusing at the moment of choosing can say why.
 *
 * ## The cursor is not the choice
 *
 * A takes the sprite under the grid cursor and it stays taken -- accent bar under its cell, and
 * the 60 px preview holds it. Moving on does not un-take it, so the colour rows can be worked
 * against a face that is standing still.
 *
 * It did not work that way at first: A applied everything and left, which made the cursor itself
 * the choice. Walking down to the colour rows therefore dragged the icon with it, and there was
 * no way to see a sprite in a colour you were considering, which is the only thing this screen is
 * for. Three presses now mean three different things -- A takes, START keeps, B keeps and leaves.
 *
 * B does not discard, and that is deliberate rather than an oversight. Nothing on this screen
 * shows the *stored* appearance next to the working one, so a Back that threw the work away would
 * be indistinguishable from a Back that saved it until you were already looking at the card grid.
 * Both buttons go through #apply, so neither can leave a combination that cannot be stored.
 *
 * ## The category list scrolls, which the handoff does not mention
 *
 * The board draws a flat list of categories at 24 px rows in a column that holds nine. There are
 * thirty. The handoff says "flat list, no nesting" and stops there, so the list gets the same
 * window-follows-cursor treatment the settings list and the lock list use.
 *
 * ## Nothing here rasterises
 *
 * A 40 px icon is measured at 6,377 us on this build and a page holds 45, so filling one in a
 * frame would be nearly 300 ms. #icon_request marks what is wanted and app.c's pump does what a
 * time budget allows; a cell without pixels yet draws its empty plate and fills in a frame or two
 * later. Measured over the appearance editor: one icon a frame, `f3=0 f4+=0`, worst frame 26-30 ms
 * -- see AUDIT.md 1ah.
 *
 * Requests go out nearest-the-cursor last, because the queue is one deep and the last request in
 * is the one that gets served.
 *
 * A page therefore takes about three quarters of a second to fill, and then it stays filled --
 * every cell is rasterised in one colour pair whatever the cursor is doing, so moving costs
 * nothing. See cell_colours(), which exists to make that impossible to get wrong twice.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "menu/fonts.h"
#include "menu/profile.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/icon.h"
#include "ui/theme.h"

/* The sprite name and page counter, above the grid rather than below it. They were underneath,
 * which is where the second colour row now goes -- and a caption reads better next to the thing
 * it captions than three rows away from it. */
#define CAPTION_Y   88

/* The handoff says the category column is 132 wide. Measured against the names it has to hold,
 * that is too narrow: at 24 px "Monsters" came out as "Monster" and "Abstract" as "Abstrac",
 * clipped mid-word. 140, and the display names in tools/mkiconmeta.py are capped at eight
 * characters to match. */
#define CAT_X       16
#define CAT_Y       100
#define CAT_W       140
#define CAT_ROW_H   24
#define CAT_GAP     2
#define CAT_PITCH   (CAT_ROW_H + CAT_GAP)

#define GRID_X2     160
#define GRID_Y2     100
#define CELL        44
#define CELL_GAP    6
#define GRID_COLS2  9
#define GRID_ROWS2  5
#define PER_PAGE    (GRID_COLS2 * GRID_ROWS2)

#define GRID_H2     (GRID_ROWS2 * CELL + (GRID_ROWS2 - 1) * CELL_GAP)
#define GRID_BOT    (GRID_Y2 + GRID_H2)
#define CAT_VISIBLE (GRID_H2 / CAT_PITCH)

/* Two colour rows under the grid: a label, nine swatches, and the name of the chosen one.
 *
 * The chosen swatch is #SW_GROW pixels taller than the rest at both ends rather than merely
 * ringed. A 2 px ring is what the first version had and it is not enough -- "it is also not
 * obvious from the controls which colour is selected" -- because a rectangle with a border beside
 * eight rectangles without one is a difference you have to go looking for. A swatch a third taller
 * than its neighbours is one you cannot miss, and the name spells it out in words besides. */
#define SW_LABEL_X  16
#define SW_LABEL_W  60
#define SW_X        84
#define SW_W        44
#define SW_H        24
#define SW_GAP      6
#define SW_GROW     5
#define SW_NAME_X   534
#define SW_NAME_W   84
#define SW_INK_Y    (GRID_BOT + 10)
#define SW_PLATE_Y  (SW_INK_Y + SW_H + 14)

/** The 60 px edit preview, drawn in the header. #ICON_LARGE, exactly. */
#define PREVIEW     ICON_LARGE

/* The board puts the position bar at x=628. The safe area is x in [16, 624), so that is ten
 * pixels off the right of a CRT. The grid screen hit the same thing and settled on 618 -- see
 * docs/DESIGN.md 5.1 -- so this asserts against the same constant rather than inventing a
 * second answer. */
_Static_assert(GRID_X2 + GRID_COLS2 * CELL + (GRID_COLS2 - 1) * CELL_GAP <= POSBAR_X,
               "the icon grid runs into the position bar");
_Static_assert(SW_X + PROFILE_COLOURS * SW_W + (PROFILE_COLOURS - 1) * SW_GAP <= SW_NAME_X - SW_GAP,
               "the swatch rows run into the colour name");
_Static_assert(SW_NAME_X + SW_NAME_W <= POSBAR_X, "the colour name runs outside the safe area");
/* The grown swatch is what has to clear the footer and the grid, not the nominal one. Both of
 * these had 5 px of slack when they were written, which is the whole reason they say SW_GROW. */
_Static_assert(SW_PLATE_Y + SW_H + SW_GROW <= FOOTER_Y, "the chosen swatch runs into the footer");
_Static_assert(SW_INK_Y - SW_GROW >= GRID_BOT, "the chosen swatch runs into the icon grid");
_Static_assert(SW_INK_Y + SW_H + SW_GROW < SW_PLATE_Y - SW_GROW, "the two swatch rows touch");
_Static_assert(CAT_VISIBLE >= 1, "no room for a category row");

/** Which pane the d-pad is driving. Ordered top to bottom: Down walks through them. */
typedef enum { PANE_CATS = 0, PANE_GRID, PANE_INK, PANE_PLATE } pane_t;

static int    slot = -1;        /**< the profile being edited */
static pane_t pane;
static int    cat, cat_top;
static int    page, cell;
static int    ink, plate;
static uint16_t chosen;
/** Seconds left of a refusal message, and what it says. */
static float  refuse_t;
static char   refuse_msg[80];

void screen_appearance_ask (int profile_slot) {
    slot = profile_slot;
}

static int cat_pages (void) {
    int n = icon_cat_size(cat);
    return (n + PER_PAGE - 1) / PER_PAGE;
}

/** The pack index under the grid cursor, or ICON_NONE past the end of a short last page. */
static uint16_t cursor_icon (void) {
    return icon_cat_at(cat, page * PER_PAGE + cell);
}

static void appearance_enter (app_t *app) {
    (void)app;
    if (slot < 0) {
        slot = profile_active();
    }
    pane = PANE_GRID;
    cat = 0;
    cat_top = 0;
    page = 0;
    cell = 0;
    refuse_t = 0.0f;
    plate = profile_plate(slot);
    ink = profile_ink(slot);
    chosen = profile_icon(slot);

    /* Open on the category the current face is in, so editing an appearance starts where it is
     * rather than at Animals every time. Linear over 30 categories times their contents is a few
     * thousand comparisons on one screen entry, which is not worth an index to avoid. */
    for (int c = 0; c < icon_cat_count(); c++) {
        int n = icon_cat_size(c);
        for (int i = 0; i < n; i++) {
            if (icon_cat_at(c, i) == chosen) {
                cat = c;
                page = i / PER_PAGE;
                cell = i % PER_PAGE;
                cat_top = (cat >= CAT_VISIBLE) ? (cat - CAT_VISIBLE + 1) : 0;
                return;
            }
        }
    }
}

static void refuse (const char *msg) {
    snprintf(refuse_msg, sizeof(refuse_msg), "%s", msg);
    refuse_t = 3.0f;
    sound_play_effect(SFX_ERROR);
}

/**
 * @brief Commit the three choices and leave, or say why not.
 *
 * Reached by START and by B, and they do the same thing on purpose. B does not discard: there is
 * nowhere on this screen to see what you picked except the preview, so a Back that threw it away
 * would be indistinguishable from a Back that saved it until you were already on the next screen.
 * Neither button can leave with a combination that cannot be stored, which is why the refusals
 * below hold the screen rather than beeping on the way out.
 */
static void apply (app_t *app) {
    if (chosen == ICON_NONE) {
        refuse("Pick an icon first");
        return;
    }
    if (ink == plate) {
        /* Not prevented in the cursor, because a swatch you cannot land on is a rule nothing on
         * screen explains. Refused here, where there is somewhere to say why. */
        refuse("Pick a different colour for the icon and the plate");
        return;
    }
    int owner = profile_appearance_owner(chosen, plate, ink, slot);
    if (owner >= 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "%s already looks like that", profile_name(owner));
        refuse(msg);
        return;
    }
    profile_set_icon(slot, chosen);
    profile_set_plate(slot, plate);
    profile_set_ink(slot, ink);
    profile_save();
    sound_play_effect(SFX_ENTER);
    app_goto(app, SCREEN_PROFILES);
}

/** @brief Take the sprite under the grid cursor as the chosen one. */
static void choose_icon (void) {
    uint16_t want = cursor_icon();
    if (want == ICON_NONE) {
        sound_play_effect(SFX_ERROR);
        return;
    }
    if (want == chosen) {
        return;
    }
    chosen = want;
    sound_play_effect(SFX_SETTING);
}

static void appearance_update (app_t *app, float dt) {
    const input_t *in = &app->input;

    if (refuse_t > 0.0f) {
        refuse_t -= dt;
    }

    if (input_pressed(in, BTN_B) || input_pressed(in, BTN_START)) {
        apply(app);
        return;
    }
    if (input_pressed(in, BTN_A)) {
        /* A takes the sprite under the cursor and nothing else. It used to apply everything and
         * leave, which made the cursor the choice -- so scrolling down to the colour rows dragged
         * the icon along with it and there was no way to hold a sprite still while trying colours
         * against it. The choice is now a separate thing from where you are looking. */
        if (pane == PANE_GRID) {
            choose_icon();
        }
        return;
    }

    bool moved = false;
    int n = icon_cat_size(cat);
    int pages = cat_pages();

    /* L and R page within the category from anywhere, which is the handoff's rule and the reason
     * the grid cursor never has to leave the grid to move a long way. */
    if (input_pressed(in, BTN_L) && page > 0) {
        page--;
        cell = 0;
        moved = true;
    }
    if (input_pressed(in, BTN_R) && page < pages - 1) {
        page++;
        cell = 0;
        moved = true;
    }

    switch (pane) {
        case PANE_CATS:
            if (in->up   && cat > 0)                     { cat--; moved = true; }
            if (in->down && cat < icon_cat_count() - 1)  { cat++; moved = true; }
            /* Moving the category selection resets to page 1 of it, per the handoff. */
            if (moved) {
                page = 0;
                cell = 0;
            }
            if (in->right) {
                pane = PANE_GRID;
                moved = true;
            }
            break;

        case PANE_GRID: {
            int row = cell / GRID_COLS2, col = cell % GRID_COLS2;
            if (in->left) {
                if (col > 0) {
                    cell--;
                } else {
                    pane = PANE_CATS;
                }
                moved = true;
            }
            if (in->right && col < GRID_COLS2 - 1) { cell++; moved = true; }
            if (in->up && row > 0)                 { cell -= GRID_COLS2; moved = true; }
            if (in->down) {
                if (row < GRID_ROWS2 - 1) {
                    cell += GRID_COLS2;
                } else {
                    pane = PANE_INK;
                }
                moved = true;
            }
            /* A short last page leaves the cursor past the end. Clamped to the last real cell
             * rather than allowed to sit on empty space, so A always has something to apply. */
            int here = page * PER_PAGE + cell;
            if (here >= n && n > 0) {
                cell = (n - 1) - page * PER_PAGE;
                if (cell < 0) {
                    cell = 0;
                }
            }
            break;
        }

        case PANE_INK:
            if (in->left  && ink > 0)                    { ink--; moved = true; }
            if (in->right && ink < PROFILE_COLOURS - 1)  { ink++; moved = true; }
            if (in->up)   { pane = PANE_GRID;  moved = true; }
            if (in->down) { pane = PANE_PLATE; moved = true; }
            break;

        case PANE_PLATE:
            if (in->left  && plate > 0)                    { plate--; moved = true; }
            if (in->right && plate < PROFILE_COLOURS - 1)  { plate++; moved = true; }
            if (in->up) { pane = PANE_INK; moved = true; }
            break;
    }

    if (cat < cat_top) {
        cat_top = cat;
    }
    if (cat >= cat_top + CAT_VISIBLE) {
        cat_top = cat - CAT_VISIBLE + 1;
    }
    if (moved) {
        sound_play_effect(SFX_CURSOR);
    }
}

/**
 * @brief The one colour pair every grid cell is rasterised in.
 *
 * Every cell, including the one under the cursor. The cursor cell used to be drawn in brighter
 * ink on a lighter plate, which meant that moving one square threw away two cached icons and
 * asked for two new ones: the cell being left had to come back in idle colours and the cell being
 * entered in selected ones. At 6,377 us a decode and a one-deep queue, that is two frames of the
 * cursor sitting on a blank plate every single press -- the blinking.
 *
 * So selection is drawn with geometry instead: the cell lifts two pixels and gains a white frame,
 * neither of which touches a pixel svg64 produced. Nothing in the grid is ever re-rasterised for
 * a cursor move now, and a page decodes exactly 45 times however long you spend on it.
 *
 * The cache is keyed on (index, ink, paper), so this must be the *only* place either colour is
 * decided -- a screen that requests one pair and draws another misses on every cell forever with
 * nothing in any log to say why, which is what the first version of this screen did.
 */
static void cell_colours (const app_t *app, uint16_t *ink_out, uint16_t *paper_out) {
    *ink_out   = app->theme->text;
    *paper_out = app->theme->bg_alt;
}

/**
 * @brief Ask for the page's icons, nearest the cursor last.
 *
 * Order is the whole point. The queue is one deep, so whatever is requested last wins -- issuing
 * them in cell order would mean the cursor's icon is asked for once and then overwritten by cell
 * 44's request before the pump ever ran.
 *
 * Every cell is asked for every frame even once it is resident, and that is not waste: a hit
 * marks the entry as recently used, which is what stops the cache evicting the far end of the
 * page to make room for the near end. See icon.c's icon_request().
 */
static void appearance_background (app_t *app, uint32_t budget) {
    (void)budget;
    if (icon_count() == 0) {
        return;
    }
    uint16_t ink_c, paper_c;
    cell_colours(app, &ink_c, &paper_c);

    /* Every cell, farthest first, both directions at each distance.
     *
     * The first version folded the distance and the direction into one counter -- `cell + (d odd ?
     * -(d+1)/2 : (d+1)/2)` over d in 0..44 -- which reaches a distance of at most 22 either way.
     * With the cursor at cell 0 that is cells 0 to 22 and no others: the bottom half of the page
     * was never *asked for*, so it could not arrive however long you waited, and it only appeared
     * once the cursor had walked far enough down to bring it into the window. Measured before the
     * fix: 24 decodes in the first second on the appearance screen and then nothing at all for
     * four seconds, with 21 cells still empty. That is the other half of "not all icons are
     * displaying, and which ones changes as you move around".
     *
     * Distance and direction are two loops now because they are two things. 88 iterations of an
     * integer compare against a 6,377 us decode. */
    for (int d = PER_PAGE - 1; d >= 1; d--) {
        for (int s = -1; s <= 1; s += 2) {
            int i = cell + s * d;
            if (i < 0 || i >= PER_PAGE) {
                continue;
            }
            uint16_t idx = icon_cat_at(cat, page * PER_PAGE + i);
            if (idx != ICON_NONE) {
                icon_request(idx, ICON_SMALL, ink_c, paper_c);
            }
        }
    }
    uint16_t here = cursor_icon();
    if (here != ICON_NONE) {
        icon_request(here, ICON_SMALL, ink_c, paper_c);
    }
    /* And the 60 px preview -- of the *chosen* icon, not the one under the cursor, in the two
     * colours being chosen. The grid cells deliberately are NOT recoloured: the handoff draws them
     * in chrome ink on a chrome plate, and forty-five copies of one colour is a page that reads as
     * a swatch rather than as a set of pictures. So the preview is the only place the three
     * choices are shown combined, which is what makes moving along either colour row mean
     * anything -- and it is why it has to hold still while the cursor wanders. */
    if (chosen != ICON_NONE) {
        icon_request(chosen, ICON_LARGE, profile_colour_fill(ink), profile_colour_fill(plate));
    }
}

/* ------------------------------------------------------------------ drawing */

static void draw_cats (app_t *app) {
    const theme_t *th = app->theme;
    int n = icon_cat_count();

    for (int i = 0; i < CAT_VISIBLE; i++) {
        int c = cat_top + i;
        if (c >= n) {
            break;
        }
        int y = CAT_Y + i * CAT_PITCH;
        bool here = (c == cat);
        /* Outlined when the cursor is on it, not inverted. A white plate with near-black 24 px
         * text on it is the one thing this display renders worst, and the category names were the
         * biggest instance of it on any screen. Same treatment as the popups. */
        if (here) {
            ui_fill(CAT_X, y, CAT_W, CAT_ROW_H, th->panel_alt);
            if (pane == PANE_CATS) {
                ui_border(CAT_X, y, CAT_W, CAT_ROW_H, 2, th->text);
            }
        }
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
        ui_text_font(FNT_SMALL, CAT_X + 8, y + 19, CAT_W - 14, ALIGN_LEFT,
                     here ? STL_DEFAULT : STL_GRAY, icon_cat_name(c));
    }

    if (n > CAT_VISIBLE) {
        int track = CAT_VISIBLE * CAT_PITCH;
        int thumb = (track * CAT_VISIBLE) / n;
        if (thumb < 16) {
            thumb = 16;
        }
        int pos = ((track - thumb) * cat_top) / (n - CAT_VISIBLE);
        ui_fill(CAT_X + CAT_W + 4, CAT_Y, 4, track, th->panel);
        ui_fill(CAT_X + CAT_W + 4, CAT_Y + pos, 4, thumb, th->text_dim);
    }
}

static void draw_grid (app_t *app) {
    int n = icon_cat_size(cat);
    uint16_t ink_c, paper_c;
    cell_colours(app, &ink_c, &paper_c);

    for (int i = 0; i < PER_PAGE; i++) {
        int at = page * PER_PAGE + i;
        if (at >= n) {
            break;
        }
        int x = GRID_X2 + (i % GRID_COLS2) * (CELL + CELL_GAP);
        int y = GRID_Y2 + (i / GRID_COLS2) * (CELL + CELL_GAP);
        bool here = (i == cell && pane != PANE_CATS);

        /* The cursor cell lifts two pixels and wears a three-pixel white frame. Both are drawn
         * around the icon rather than into it: the plate stays the colour the icon was rasterised
         * against, which is what lets the cursor move without re-rasterising anything.
         *
         * Chrome, not the profile's colours -- those belong to the preview, and a page of
         * forty-five identical swatches would read as one colour rather than as a set of
         * pictures. */
        int cy = here ? y - 2 : y;
        if (here) {
            /* White while the grid has the cursor, grey once it has moved to a colour row. The
             * frame still marks where the grid cursor will come back to, but brightness is what
             * says which pane Left and Right are driving -- otherwise a white cell frame and a
             * white swatch ring both claim to be the live thing at once. */
            ui_fill(x - 3, cy - 3, CELL + 6, CELL + 6,
                    (pane == PANE_GRID) ? 0xFFFF : app->theme->text_dim);
        }
        ui_fill(x, cy, CELL, CELL, paper_c);

        uint16_t idx = icon_cat_at(cat, at);
        const surface_t *pix = icon_get(idx, ICON_SMALL, ink_c, paper_c);
        if (pix != NULL) {
            rdpq_set_mode_copy(false);
            rdpq_tex_blit(pix, x + (CELL - ICON_SMALL) / 2, cy + (CELL - ICON_SMALL) / 2, NULL);
        }

        /* The chosen sprite keeps an accent bar along its bottom whether or not the cursor is on
         * it. Two different marks, because they are two different facts: white frame and a lift
         * for "you are looking at this", the accent bar for "this is the one you took". The same
         * bar means the same thing under the active tab and under the active profile's card. */
        if (idx != ICON_NONE && idx == chosen) {
            ui_fill(x, cy + CELL - ACCENT_BAR, CELL, ACCENT_BAR, app->theme->tab_underline);
        }
    }
}

/** One labelled row of the palette. @p sel is the chosen entry, @p live whether it has the cursor. */
static void draw_swatch_row (app_t *app, int y, const char *label, int sel, bool live) {
    const theme_t *th = app->theme;

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text(SW_LABEL_X, y + 18, SW_LABEL_W, ALIGN_LEFT, live ? STL_DEFAULT : STL_GRAY, label);
    /* The chosen colour said in words, which is the only unambiguous version of it. A ring says
     * "that one"; it does not say *which* one when the swatches either side are also blue-ish. */
    ui_text(SW_NAME_X, y + 18, SW_NAME_W, ALIGN_RIGHT, live ? STL_DEFAULT : STL_GRAY,
            profile_colour_name(sel));

    for (int i = 0; i < PROFILE_COLOURS; i++) {
        int x = SW_X + i * (SW_W + SW_GAP);
        bool here = (i == sel);
        /* The chosen one is taller at both ends. Marked whether or not the row has the cursor, so
         * both choices stay visible while the other row is being changed; the ring's brightness is
         * what says which row Left and Right are driving. */
        int sy = here ? y - SW_GROW : y;
        int sh = here ? SW_H + 2 * SW_GROW : SW_H;

        /* A hairline around every swatch, and the dark neutral is why. #101019 on a dark panel is
         * the same colour as the gap between swatches, so the palette read as eight colours and a
         * hole -- you could put the cursor on a swatch that appeared not to exist. */
        ui_fill(x - 1, sy - 1, SW_W + 2, sh + 2, th->text_dim);
        ui_fill(x, sy, SW_W, sh, profile_colour_fill(i));
        if (here) {
            ui_border(x - 2, sy - 2, SW_W + 4, sh + 4, 2, live ? 0xFFFF : th->text_dim);
        }
    }
}

static void appearance_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    char buf[96];

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    snprintf(buf, sizeof(buf), "%s's appearance", profile_name(slot));
    ui_text_font(FNT_KEY, SAFE_X, 42, SAFE_W - PREVIEW - 16, ALIGN_LEFT, STL_DEFAULT, buf);

    /* The chosen face in the chosen colours, at 60 px -- the second of the handoff's two sizes,
     * and the only place the icon and both colours are shown together. In the header because
     * there is nowhere below the grid for it: the two colour rows take that space. */
    if (icon_count() > 0) {
        int px = SAFE_X + SAFE_W - PREVIEW;
        uint16_t paper = profile_colour_fill(plate);
        ui_fill(px, 0, PREVIEW, PREVIEW, paper);
        const surface_t *big = icon_get(chosen, ICON_LARGE,
                                        profile_colour_fill(ink), paper);
        if (big != NULL) {
            rdpq_set_mode_copy(false);
            rdpq_tex_blit(big, px, 0, NULL);
        }
    }

    if (icon_count() == 0) {
        ui_label(SAFE_X, 200, SAFE_W, ALIGN_CENTER, STL_GRAY,
                 "This cartridge was built without the icon artwork.");
    } else {
        /* The sprite's own name, which is what makes a picture findable by eye: the artwork is
         * often not what the filename says, and the tags behind these names were written by
         * looking at the drawings. */
        char name[64];
        icon_name(cursor_icon(), name, sizeof(name));
        const char *shown = strchr(name, '/');
        ui_label(GRID_X2, CAPTION_Y, 300, ALIGN_LEFT, STL_GRAY,
                 (shown != NULL) ? shown + 1 : name);

        snprintf(buf, sizeof(buf), "PAGE %d / %d", page + 1, cat_pages() > 0 ? cat_pages() : 1);
        ui_label(SAFE_X, CAPTION_Y, SAFE_W - 12, ALIGN_RIGHT, STL_GRAY, buf);

        draw_cats(app);
        draw_grid(app);
        draw_swatch_row(app, SW_INK_Y,   "Icon",  ink,   pane == PANE_INK);
        draw_swatch_row(app, SW_PLATE_Y, "Plate", plate, pane == PANE_PLATE);
    }

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    int hx = SAFE_X;
    /* A only where it means something. On the grid it takes the sprite under the cursor; on the
     * category list and the two colour rows the cursor *is* the choice, so there is nothing for it
     * to take and advertising it would be a button that does nothing. */
    if (pane == PANE_GRID) {
        hx = ui_hint(hx, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC, "Choose");
    }
    hx = ui_hint(hx, FOOTER_Y + 14, "S", BTN_START_COLOR, UI_BTN_DISC, "Done");
    (void)ui_hint(hx, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC, "Back");

    /* Where the d-pad goes from here, in words, because there is no glyph for it and because the
     * colour rows are otherwise unreachable by anything but accident: the only way to them is to
     * walk off the bottom of the icon grid, and nothing on screen said so. Written out rather than
     * drawn as arrow discs -- those are the C buttons on this controller, and they do something
     * else on the screen this one is reached from. */
    const char *nav = "";
    switch (pane) {
        case PANE_CATS:  nav = "Right: icons";                            break;
        case PANE_GRID:  nav = "L / R: page      Down: colours";          break;
        case PANE_INK:   nav = "Left / Right: colour      Down: plate";   break;
        case PANE_PLATE: nav = "Left / Right: colour      Up: icon";      break;
    }
    /* Its own line under the hints, not beside them. Right-aligned on the same row as "A Apply
     * B Back" put "Left / Right: colour" through the middle of the word Back -- the longest of
     * these is 37 characters and the hints already reach x=238, which leaves 386 px for something
     * that wants about 400. A second line has room for any of them and needs no counting.
     *
     * The refusal replaces it rather than taking a third line: there is no third line, and a
     * message about why A did nothing is worth more for three seconds than a reminder of which
     * way the d-pad goes. */
    if (refuse_t > 0.0f) {
        ui_label(SAFE_X, FOOTER_Y + 49, SAFE_W, ALIGN_RIGHT, STL_ORANGE, refuse_msg);
    } else {
        ui_label(SAFE_X, FOOTER_Y + 49, SAFE_W, ALIGN_RIGHT, STL_GRAY, nav);
    }

    rdpq_detach_show();
}

const screen_t SCREEN_APPEARANCE_DEF = {
    .id         = SCREEN_APPEARANCE,
    .enter      = appearance_enter,
    .update     = appearance_update,
    .render     = appearance_render,
    .background = appearance_background,
};
