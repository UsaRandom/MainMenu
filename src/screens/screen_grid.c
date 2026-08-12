/**
 * @file screen_grid.c
 * @brief The library grid. The screen everything is judged on.
 * @ingroup screens
 *
 * Geometry and colour are transcribed from docs/design/README.md sections 1-4 by way of
 * ui/theme.h. Nothing here invents a number.
 *
 * The selection model is worth stating because it inverts what you would expect to draw: the
 * signal is not that the selected tile is decorated, it is that every OTHER tile is washed out.
 * The handoff calls this the one signal that survives a room's width. So the common case is a
 * blended quad per unselected tile, and the grow, shadow and outline are reinforcement.
 *
 * A tile has three appearances, not one: decoded art, a "no art" state that is fully specified
 * rather than a placeholder, and a loading state carrying the title and a decode progress worm.
 * A cold or unillustrated library therefore stays navigable instead of becoming a wall of grey.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <libdragon.h>

#include "app.h"
#include "menu/fonts.h"
#include "menu/image_decoder.h"
#include "menu/profile.h"
#include "menu/sound.h"
#include "library/boxart.h"
#include "library/playstate.h"
#include "library/thumbstore.h"
#include "screens.h"
#include "screens/boot_plate.h"
#include "ui/draw.h"
#include "ui/icon.h"
#include "ui/theme.h"
#include "ui/tween.h"

#define MAX_VIEW 1024

static uint16_t view[MAX_VIEW];
static int view_count;

static tab_t tab = TAB_N64;      /**< replaced at first entry; see pick_opening_tab() */
static int cursor;              /**< index into view[] */

static float scroll_y;          /**< pixels, content space; always rounded before use */
static float scroll_target;
static float pulse_phase;
static tween_t grow;

/** Frames of stillness before the decode budget opens up. ~0.25 s at 60 Hz. */
#define DECODE_SETTLE_FRAMES    15

/* Starts settled rather than at zero. The cursor cannot move while the boot plate is up, so this
 * still holds its initial value on the first frame after the curtain -- which means decoding
 * carries straight on through the reveal instead of stopping for a quarter of a second at the
 * exact moment the user first sees the grid. */
static int frames_since_move = DECODE_SETTLE_FRAMES;

/* ------------------------------------------------------------------ small drawing helpers */


/* ------------------------------------------------------------------ model */

/**
 * @brief The cell this tab lays out on, and why it is a tab-wide number.
 *
 * A cell is the tallest box in the tab, and art shorter or narrower than that is centred in it
 * rather than stretched or cropped -- so the Game Boy tab is a tight grid of squares, the N64 tab
 * a tight grid of whatever shape its covers turn out to be, and Recent, which mixes them, takes
 * the tallest and shows the rest with a little plate around them.
 *
 * The WIDTH is part of it, which it was not before. A shape earns a grid column -- four across or
 * five, see ui/theme.h -- and the tallest shape in a tab always has the narrowest column in it,
 * so taking the tallest settles both numbers at once and every other shape in the tab is drawn
 * smaller than the size it was cached at. Never larger: that is the property the whole scheme
 * rests on, because upscaling out of the atlas is exactly what caching at the drawn size avoids.
 *
 * Per tab rather than per row, which would be a masonry layout: row heights that change as you
 * scroll make the scroll position stop meaning anything, and every tile below a short row would
 * shift when a favourite was added. Per tab costs one pass over the view when the tab changes.
 */
static int cell_w = TILE_W_NARROW;
static int cell_h = TILE_H_MAX;
static int cols   = TILE_COLS_NARROW;

static int rows_total (void) {
    return (view_count + cols - 1) / cols;
}

static int row_pitch (void) {
    return cell_h + TILE_GAP;
}

/** @brief Left edge of column @p c. */
static int col_x (int c) {
    return GRID_X + c * (cell_w + TILE_GAP);
}

/** @brief Top of row @p r in content space, before scroll. Includes the overhang pad. */
static int row_y (int r) {
    return GRID_Y + GRID_PAD_TOP + r * row_pitch();
}

static float scroll_max (void) {
    /* The pads are part of the content, not of the window: the last row has to be able to scroll
     * GRID_PAD_BOT further than its cell needs, or its shadow lands under the scissor. */
    int content = rows_total() * row_pitch() - TILE_GAP + GRID_PAD_TOP + GRID_PAD_BOT;
    float m = (float)(content - GRID_H);
    return m > 0.0f ? m : 0.0f;
}

/**
 * @brief Recompute #cell_w, #cell_h and #cols from what the tab actually holds.
 *
 * An unmeasured record contributes BOXART_FALLBACK_KIND, which is landscape and therefore the
 * shortest of the three -- and now also one of the two WIDEST, so the consequence is bigger than
 * it was. The FIRST visit to a tab on a cold card is laid out from the fallback: four columns of
 * 140 x 98. A tab whose covers all turn out portrait then re-lays itself to five columns of
 * 109 x 155 on the next visit, which moves every tile rather than just resizing it. The tiles are
 * not cropped while that is true -- the draw scales to fit, it does not cut -- and on a warm card
 * none of it happens, because every shape is in library.idx before the first frame.
 *
 * The alternative was to take the tallest shape whenever anything is unknown, which never
 * under-pitches and always over-pitches: a tab of genuinely artless games would be five narrow
 * columns of tall cells holding short plates, permanently, to avoid a transient. Guessing the
 * fallback and being briefly wrong is the cheaper mistake.
 */
static void measure_cells (app_t *app) {
    art_shape_t tall = { 0, 0 };
    for (int i = 0; i < view_count; i++) {
        art_shape_t s = thumbcache_record_shape(&app->lib->records[view[i]]);
        if (s.h > tall.h) {
            tall = s;
        }
    }
    /* An empty tab takes the tallest shape there is. It draws nothing, but scroll_max() and the
     * position bar are computed from the pitch either way, and a zero pitch divides by zero in
     * the prefetch loop. */
    if (tall.h == 0) {
        tall = boxart_tallest();
    }
    cell_w = tall.w;
    cell_h = tall.h;
    /* Derived rather than stored beside the width, so the two can never disagree: whatever width
     * the tallest shape earned, this is how many of them fit. */
    cols = (GRID_W + TILE_GAP) / (cell_w + TILE_GAP);
}

static void rebuild_view (app_t *app) {
    view_count = library_tab_view(app->lib, tab, view, MAX_VIEW);
    /* Say so when a tab is clipped. library_tab_view() stops at cap and returns cap, which is
     * indistinguishable from a tab that happens to hold exactly that many -- so a card with more
     * than MAX_VIEW titles on one system would quietly present a library missing its tail, with
     * nothing anywhere saying which games went. 1024 is comfortably past the 500+ this is
     * designed for; the log line is here so that if it is ever not, the symptom names itself. */
    if (view_count == MAX_VIEW && app->lib->count > MAX_VIEW) {
        debugf("GRID %s clipped at %d titles; the library holds %d\n",
               library_tab_label(tab), MAX_VIEW, app->lib->count);
    }
    if (cursor >= view_count) {
        cursor = view_count > 0 ? view_count - 1 : 0;
    }
    measure_cells(app);
}

/** @brief Centre the selected row, clamped, snapped to whole pixels. */
static void retarget_scroll (void) {
    int row = cursor / cols;
    /* GRID_PAD_TOP appears here because row_y() carries it: the scroll that puts row r's cell at
     * a given screen y is larger by the pad than the cell arithmetic alone would say. */
    float want = (float)(GRID_PAD_TOP + row * row_pitch()) - (float)(GRID_H - cell_h) * 0.5f;
    scroll_target = roundf(clampf(want, 0.0f, scroll_max()));
}

/* ------------------------------------------------------------------ tiles */

/* The player chip, at the left end of the rail. A 40 px sprite is the floor the handoff sets --
 * nothing in the UI draws an icon smaller -- and a 44 px plate leaves 2 px of colour around it.
 * Centred in the rail above its accent bar, so a lit chip underlines exactly like a lit tab. */
#define CHIP_PLATE      44
#define CHIP_MARGIN     8
#define CHIP_GAP        6
#define CHIP_X          (TABRAIL_X + CHIP_MARGIN)
#define CHIP_Y          (TABRAIL_Y + (TABRAIL_H - ACCENT_BAR - CHIP_PLATE) / 2)

/**
 * Room for the name beside the plate, reserved whatever the name actually is.
 *
 * Nine characters, which is #PROFILE_LABEL_CAP - 1: eight is the most anybody can type and nine is
 * "Player 10". A box measured from the current name would move every tab when somebody switched to
 * a player with a longer one, which is the reason the name was dropped from the chip when it moved
 * to this end of the rail. Reserving the worst case costs 108 px of a rail that had 164 spare.
 */
#define CHIP_NAME_W     ((PROFILE_LABEL_CAP - 1) * TAB_GLYPH_W)
#define CHIP_W          (CHIP_PLATE + CHIP_GAP + CHIP_NAME_W)

/** Where the tabs begin, which is past the chip. Fixed, so who is playing never moves a tab. */
#define TAB_X0          (CHIP_X + CHIP_W + TAB_PAD)

static void draw_tile (app_t *app, const lib_record_t *rec, uint16_t rom_id,
                       int x, int y, int w, int h, bool selected) {
    const theme_t *th = app->theme;

    surface_t *art = thumbcache_get(app->thumbs, app->lib, rom_id);

    if (art != NULL) {
        /* Arrival: 0.12 s, scale 0.86 -> 1.00 on the spec's overshoot curve, silent. Section 6 is
         * explicit that 512 of these must make no sound. */
        float arrive = 1.0f;
        if (rec->art_age < DUR_TILE_ARRIVAL) {
            float k = ease_bezier(rec->art_age / DUR_TILE_ARRIVAL, EASE_TILE_GROW);
            arrive = lerpf(TILE_ARRIVE_SCALE, 1.0f, k);
        }
        int aw = (int)(w * arrive);
        int ah = (int)(h * arrive);
        int ax = x + (w - aw) / 2;
        int ay = y + (h - ah) / 2;

        /* Copy mode cannot scale, so an arriving tile goes through the standard pipeline for the
         * few frames it is animating and drops back to copy once it settles.
         *
         * And a tile that is not the size it is drawn at is permanently in that position. On a
         * console with no Expansion Pak the pool holds tiles at half their drawn size -- a quarter
         * of the bytes, so twelve fit where three did (see thumbcache_store_shape) -- and copy mode
         * would ignore the scale and draw a quarter of the picture into the corner of the cell at
         * 1:1. Tested by size rather than by profile: it is the surface that decides, so a full
         * size tile on either console still takes the fast path. */
        bool scaled = (art->width != aw) || (art->height != ah);
        if (arrive < 1.0f || scaled) {
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_TEX);
        } else {
            rdpq_set_mode_copy(false);
        }
        /* Scaled against the surface's own size, not against a tile constant. Tiles are box
         * shapes now -- a Game Boy cover is cached 140 x 140 where a portrait one is 109 x 155,
         * and either may be drawn smaller again in a mixed tab -- and dividing by the wrong
         * constant does not fail, it silently draws the square art at 1.4x its height. */
        rdpq_tex_blit(art, ax, ay, &(rdpq_blitparms_t){
            .scale_x = (float)aw / (float)art->width,
            .scale_y = (float)ah / (float)art->height,
        });
    } else if (rec->art_state == ART_NONE) {
        /* No art: bg_alt fill, 2 px inner border, and the title. Nothing else.
         *
         * There used to be a large system-code watermark here -- N64, SNES, SMS -- on the
         * reasoning that a tile should say something rather than nothing. It says the wrong
         * thing: the tab rail above already states the system, so the watermark repeated known
         * information in the largest type on the tile, and it read as a label for the game
         * rather than for the console. An empty plate with the title on it is honest about
         * there being no art; a big SNES across the middle is not. */
        ui_fill(x, y, w, h, th->bg_alt);
        ui_border(x, y, w, h, 2, th->panel_alt);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
        if (rec->title != NULL) {
            ui_text(x + 4, y + h - 16, w - 8, ALIGN_CENTER, STL_DEFAULT, rec->title);
        }
    } else {
        /* Loading: panel fill, the title immediately from the database, and a worm along the
         * bottom edge showing decode progress. Deliberately not a spinner -- the handoff bans
         * them for both of the product's long waits. */
        ui_fill(x, y, w, h, th->panel);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
        if (rec->title != NULL) {
            ui_text(x + 4, y + h / 2, w - 8, ALIGN_CENTER, STL_DEFAULT, rec->title);
        }
        if (rec->art_state == ART_DECODING) {
            int p = (int)(image_decoder_get_progress() * (float)w);
            ui_fill(x, y + h - 3, p, 3, th->text_accent);
        }
    }

    if (!selected) {
        /* The primary selection signal: everything that is NOT selected is washed toward bg.
         * These quads never overlap each other, so this stays one layer of blended fill. */
        ui_wash(x, y, w, h, th->bg, th->tile_dim_a);
    }
}

static void draw_badges (app_t *app, const lib_record_t *rec, int x, int y, int w, int h) {
    const theme_t *th = app->theme;

    /* Two marks, two corners, so there is no precedence to resolve. Save is a game state and
     * owns the top-right; favourite is a user property and owns the top-left. Cheats
     * deliberately never appear here. */
    if (rec->flags & LIBF_HAS_SAVE) {
        int bx = x + w - BADGE_SLOT - BADGE_INSET;
        int by = y + BADGE_INSET;
        ui_fill(bx, by, BADGE_SLOT, BADGE_SLOT, th->badge_save);
        ui_fill(bx + 5, by + 4, BADGE_SLOT - 10, BADGE_SLOT - 8, th->text);
    }

    /* Bottom-left, the one corner the other two badges do not use, so there is still no
     * precedence to resolve. A locked game keeps its tile and its art on purpose -- the padlock
     * is there so pressing A is an informed choice rather than a surprise. */
    if (rec->flags & LIBF_LOCKED) {
        /* Drawn twice, black then text, because unlike the other two badges this one sits
         * directly on the art with no slot behind it -- and box art is as often pale as dark. A
         * white padlock on a bright card was legible only if you knew it was there. */
        int lx = x + BADGE_INSET;
        /* Off the tile's own height, not off a constant. The padlock sat at a fixed 98 - 24 from
         * the top, which on a 155 px portrait tile is the middle of the cover. */
        int ly = y + h - BADGE_INSET - BADGE_SLOT;
        ui_padlock(lx + 1, ly + 1, BADGE_SLOT - 4, BADGE_SLOT, RGBA5551(0, 0, 0));
        ui_padlock(lx, ly, BADGE_SLOT - 4, BADGE_SLOT, th->text);
    }

    if ((rec->flags & LIBF_FAVORITE) && tab != TAB_FAVORITES) {
        /* Right triangle, hypotenuse from top-right to bottom-left. Distinguished from the
         * save badge by silhouette, not by colour -- under the Phosphor theme the two are
         * near-identical greens by design. */
        for (int i = 0; i < FAV_TRIANGLE; i++) {
            ui_fill(x, y + i, FAV_TRIANGLE - i, 1, th->badge_fav);
        }
    }
}

/**
 * @brief Hand the selected game to @p screen.
 *
 * The path is cloned rather than borrowed because the library owns its strings and a launch
 * outlives the record's residency in a tab view -- switching tabs rebuilds view[] and the index
 * the launch was started from stops meaning anything.
 *
 * rom_config_load() is re-run here rather than cached per record: it fills a rom_info_t of a few
 * hundred bytes, which is not worth holding 500 of, and at 20,700 us for one ROM it is invisible
 * against the seconds a load takes.
 */
static void grid_open (app_t *app, screen_id_t screen) {
    const lib_record_t *rec = &app->lib->records[view[cursor]];
    if (rec->path == NULL) {
        return;
    }

    if (app->launch.rom_path != NULL) {
        path_free(app->launch.rom_path);
    }
    app->launch.rom_path = path_create(rec->path);
    app->launch.rom_id = (int)view[cursor];
    memset(&app->launch.rom_info, 0, sizeof(app->launch.rom_info));
    rom_config_load(app->launch.rom_path, &app->launch.rom_info);

    app_goto(app, screen);
}

/* ------------------------------------------------------------------ chrome */

/**
 * @brief A five-pointed star with rounded points and intersections, as horizontal runs.
 *
 * `{row, x, width}`, baked rather than computed. Rounding a star is a morphological closing --
 * which rounds the five concave intersections -- followed by an opening, which blunts the five
 * points. That is a supersampled distance operation and not something to do per frame for a
 * glyph this size, so it was done once at 8x with a 0.4 px radius and an inner radius of 0.36.
 *
 * Fitted to its own bounding box rather than placed by radius. A star is 1.902 outer-radii wide
 * and 1.809 tall, so centring it on the radius leaves two dead rows under the legs -- which at
 * this size is most of a leg.
 *
 * 24 px and not TAB_ICON's old 20. Four pixels is the difference between legs that separate and
 * legs that merge into the waist: at 20 the rounding consumed them and the shape read as a
 * pentagon with a spike on top. The rail can afford it now that the two virtual tabs never spell
 * themselves out -- worst case is about 444 px of 608, against 468 before.
 */
static const uint8_t STAR[][3] = {
    { 2,11, 2}, { 3,11, 2}, { 4,11, 2}, { 5,10, 4}, { 6,10, 4}, { 7,10, 4},
    { 8,10, 4}, { 9, 1,22}, {10, 2,20}, {11, 3,18}, {12, 4,16}, {13, 6,12},
    {14, 7,10}, {15, 7,10}, {16, 7,10}, {17, 7,10}, {18, 6, 5}, {18,13, 5},
    {19, 6, 4}, {19,14, 4}, {20, 6, 3}, {20,15, 3}, {21, 5, 2}, {21,17, 2},
    {22, 5, 1}, {22,18, 1},
};
/**
 * @brief Glyph for a virtual tab.
 *
 * Drawn whether the tab is active or not, unlike every other tab, which spells its label out.
 * Recent and Favourites used to swap to text when selected and that is what broke the rail: the
 * word FAVORITES is nine glyphs where the icon is one, so selecting it shoved everything right
 * and ran the tabs into the player name. An icon that changes size when you look at it is a
 * layout that moves under the cursor.
 *
 * Favourites is a star; Recent is a clock. Both are built from fills rather than sprites -- two
 * shapes did not justify an asset pipeline.
 */
static void draw_tab_icon (int x, int y, tab_t t, uint16_t c) {
    switch (t) {
        case TAB_FAVORITES:
            for (size_t i = 0; i < sizeof(STAR) / sizeof(STAR[0]); i++) {
                ui_fill(x + STAR[i][1], y + STAR[i][0], STAR[i][2], 1, c);
            }
            break;
        case TAB_RECENT:
            ui_border(x, y, TAB_ICON, TAB_ICON, 2, c);
            ui_fill(x + TAB_ICON / 2 - 1, y + 5, 2, TAB_ICON / 2 - 4, c);   /* hand, up */
            ui_fill(x + TAB_ICON / 2, y + TAB_ICON / 2 - 1, TAB_ICON / 4, 2, c); /* hand, right */
            break;
        default:
            break;
    }
}

/**
 * @brief Who is playing, at the left end of the tab rail, and how you get to the picker.
 *
 * The entry point used to be a Z hint in the footer, shown only when a second profile existed.
 * That was right while profiles were a thing you opted into; it is wrong now that the picker is
 * where an appearance is chosen, because a card with one player had no route to it at all.
 *
 * So the chip draws always, and it is a stop on the tab rail rather than a separate control: L
 * from the first tab opens the picker. One axis, one button, and nothing on the pad had to be
 * rebound -- L and R have paged the tabs since the grid existed and they still do.
 *
 * It was at the *right* end, past SMS, and moved for one reason: the picker now keeps the rail on
 * screen, so the chip is a position the rail cursor can occupy and not just a door. A cursor that
 * lives past the last tab means "keep pressing R" walks the whole rail to reach the player and
 * then has nowhere to go; at the left it is where a cursor starts.
 *
 * Arriving IS opening it. There was a focus state here first -- the chip lit, A opened it -- and
 * it was wrong twice over: two presses to reach a destination that has no other use, and while
 * the chip was lit the last tab still drew as selected, so the rail showed two selected things at
 * once. A stop that needs confirming is a menu item; this is a rail.
 *
 * The name is beside the plate in a box of fixed width. It used to be measured from the name and
 * dropped when the tabs needed the room, so whether it appeared depended on which tab was
 * selected; at this end of the rail that would instead have moved every tab when somebody switched
 * to a player with a longer name. #CHIP_NAME_W reserves the worst case -- nine characters, which
 * is "Player 10" -- and the rail had the room.
 *
 * @param selected the rail cursor is on the chip, which is true exactly when the picker is open.
 */
static void draw_chip (app_t *app, bool selected) {
    const theme_t *th = app->theme;
    int who = profile_active();

    if (selected) {
        ui_fill(CHIP_X - TAB_PAD / 2, TABRAIL_Y, CHIP_W + TAB_PAD, TABRAIL_H, th->panel_alt);
        ui_fill(CHIP_X - TAB_PAD / 2, TABRAIL_Y + TABRAIL_H - ACCENT_BAR, CHIP_W + TAB_PAD,
                ACCENT_BAR, th->tab_underline);
    }

    uint16_t fill = profile_colour_fill(profile_plate(who));
    uint16_t ink  = profile_colour_fill(profile_ink(who));

    ui_fill(CHIP_X, CHIP_Y, CHIP_PLATE, CHIP_PLATE, fill);

    uint16_t icon = profile_icon(who);
    const surface_t *pix = icon_get(icon, ICON_SMALL, ink, fill);
    if (pix != NULL) {
        rdpq_set_mode_copy(false);
        rdpq_tex_blit(pix, CHIP_X + (CHIP_PLATE - ICON_SMALL) / 2,
                      CHIP_Y + (CHIP_PLATE - ICON_SMALL) / 2, NULL);
    } else {
        /* Not decoded yet, or this profile has no icon. Either way the plate is the placeholder:
         * it is already the right colour and the right size, so an icon arriving two frames later
         * fills a shape that was always there rather than making one appear. */
        icon_request(icon, ICON_SMALL, ink, fill);
    }

    /* Brightens with the chip, like a tab label. The rail is the only place the active player's
     * name and face appear together, which is what makes the chip readable as a *player* rather
     * than as a decoration whose meaning you have to remember. */
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text(CHIP_X + CHIP_PLATE + CHIP_GAP, TABRAIL_Y + 32, CHIP_NAME_W, ALIGN_LEFT,
            selected ? STL_DEFAULT : STL_GRAY, profile_name(who));
}

void screen_grid_draw_rail (app_t *app, bool chip_selected) {
    const theme_t *th = app->theme;

    ui_fill(TABRAIL_X, TABRAIL_Y, TABRAIL_W, TABRAIL_H, th->panel);
    draw_chip(app, chip_selected);

    /* All eight tabs are always present, empty or not, left-aligned, never centred or
     * distributed. A tab you have nothing for is still exactly where you left it.
     *
     * The two virtual tabs are icon-only, always, which is section 4.3 and also what makes the
     * rail fit. At the body font's 12 px glyph metric the eight tabs measure 24 + 24 for the two
     * icons, 36 + 36 + 48 + 24 + 36 + 36 for the words, and 8 * 20 of padding: 424 px. The chip
     * and its name take 158 more plus a pad, so the run ends at 626 in a rail that reaches 624 --
     * the last tab's own edge lands at 616, because the trailing pad is not drawn.
     *
     * That is close enough that a ninth tab or a longer label would overflow silently, so
     * rail_overflow() below says so once rather than letting SMS slide off a CRT. */
    int x = TAB_X0;
    for (int t = 0; t < TAB_COUNT; t++) {
        bool active = (t == (int)tab);
        bool virt = (t < TAB_N64);
        /* Not `virt && !active`. See draw_tab_icon(): a tab that grows from 20 px to a nine-letter
         * word when selected moves every tab to its right, and the rail has a fixed width. */
        bool icon_only = virt;

        const char *label = library_tab_label((tab_t)t);
        int w = icon_only ? TAB_ICON : (int)strlen(label) * TAB_GLYPH_W;

        /* Nothing but the chip looks selected while the chip is selected.
         *
         * This took two goes. The tab kept both its plate and its accent bar first, which put two
         * gold bars side by side -- the chip sits immediately left of the first tab, so they merged
         * into one 108 px stripe spanning both. Dropping only the bar was not enough either: a
         * lit plate is still a lit plate, and the tab went on reading as the selected thing on a
         * screen where the cursor was somewhere else.
         *
         * So the tab gives up both, and keeps only its brighter label -- which is a legible "this
         * is where R goes back to" and is not a selection. */
        if (active && !chip_selected) {
            ui_fill(x - TAB_PAD / 2, TABRAIL_Y, w + TAB_PAD, TABRAIL_H, th->panel_alt);
            ui_fill(x - TAB_PAD / 2, TABRAIL_Y + TABRAIL_H - ACCENT_BAR, w + TAB_PAD,
                    ACCENT_BAR, th->tab_underline);
        }

        if (icon_only) {
            /* text, not text_dim, when active. The underline says which tab is selected, but the
             * two icon tabs no longer have a label to brighten, so without this they read as
             * permanently dimmed next to the words beside them. */
            draw_tab_icon(x, TABRAIL_Y + (TABRAIL_H - TAB_ICON) / 2 - 2, (tab_t)t,
                          active ? th->text : th->text_dim);
        } else {
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
            ui_text(x, TABRAIL_Y + 32, w, ALIGN_LEFT,
                    active ? STL_DEFAULT : STL_GRAY, label);
        }
        x += w + TAB_PAD;
    }

    /* The chip is drawn before the loop, not after it. It used to be last, because it was right-
     * aligned and needed to know where the tabs finished; the leftover call took that `x` as its
     * new `bool selected` argument, which is non-zero always -- so the grid drew a second, lit chip
     * over its own. Nothing warned: int to bool is a legal conversion. */
    _Static_assert(TAB_X0 > CHIP_X + CHIP_W, "the first tab overlaps the player chip");

    /* The tab labels come from library_tab_label(), so the rail's width is not a compile-time
     * quantity and cannot be asserted. Said once instead: a rail that has outgrown its box draws
     * its last tab off the side of the screen, and on a CRT that is a tab nobody knows exists. */
    static bool warned;
    int right = x - TAB_PAD;
    if (!warned && right > TABRAIL_X + TABRAIL_W) {
        warned = true;
        debugf("GRID tab rail runs to %d, past the rail's %d -- the last tab is off screen\n",
               right, TABRAIL_X + TABRAIL_W);
    }
}

static void draw_footer (app_t *app) {
    const theme_t *th = app->theme;
    char buf[96];

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, HAIRLINE, th->panel_alt);

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);

    /* Tile titles live here at full width rather than on the tile, which is what lets the
     * product hold the no-marquee rule: 608 px is about 52 characters at 20 px. */
    if (view_count > 0) {
        const lib_record_t *rec = &app->lib->records[view[cursor]];
        ui_text(SAFE_X, FOOTER_Y + 26, SAFE_W, ALIGN_CENTER, STL_DEFAULT,
                rec->title ? rec->title : "?");
        snprintf(buf, sizeof(buf), "%d / %d", cursor + 1, view_count);
    } else {
        ui_text(SAFE_X, FOOTER_Y + 26, SAFE_W, ALIGN_CENTER, STL_GRAY, "NO TITLES IN THIS TAB");
        buf[0] = '\0';
    }

    /* L and R are dropped: the rail is on screen with the current tab underlined, so the tabs
     * advertise themselves and the shoulder buttons are the only thing they could mean. */
    int hx = SAFE_X;
    hx = ui_hint(hx, FOOTER_Y + 32, "A", BTN_A_COLOR, UI_BTN_DISC, "Details");
    /* Fav is C-right: a yellow disc carrying the arrow that is printed on the pad itself, so the
     * hint names the key by its shape rather than by a letter nobody calls it. Labelled "Fav"
     * rather than "Favourite" because the same hint has to fit the detail sheet's footer beside
     * Play and Cheats. */
    hx = ui_hint(hx, FOOTER_Y + 32, ">", BTN_C_COLOR, UI_BTN_DISC, "Fav");
    hx = ui_hint(hx, FOOTER_Y + 32, "S", BTN_START_COLOR, UI_BTN_DISC, "Settings");

    /* The shortcut to the picker, and only when there is more than one player. It used to be
     * labelled with the active player's name, because the name had nowhere else to live -- the
     * rail's chip dropped it whenever the tabs wanted the room. The chip carries it now, in a box
     * reserved for the worst case, so labelling this with the name would print it twice on one
     * screen. It says what the button does instead.
     *
     * UI_BTN_DISC, because this is B and B is a face button. It was Z drawn UI_BTN_TALL, on the
     * reasoning that a trigger under the controller should not read as another coloured disc --
     * that reasoning was right and it still applies, which is exactly why the shape has to move
     * with the binding. A disc labelled Z, or a trigger labelled B, would each be telling the
     * hand to go to the wrong place. */
    if (profile_count() > 1) {
        (void)ui_hint(hx, FOOTER_Y + 32, "B", BTN_B_COLOR, UI_BTN_DISC, "Players");
    }
    if (buf[0]) {
        ui_text(SAFE_X, FOOTER_Y + 48, SAFE_W, ALIGN_RIGHT, STL_ORANGE, buf);
    }
}

static void draw_position_bar (app_t *app) {
    const theme_t *th = app->theme;

    float max = scroll_max();
    if (max <= 0.0f) {
        return;                     /* hidden when nothing scrolls */
    }

    ui_fill(POSBAR_X, POSBAR_Y, POSBAR_W, POSBAR_H, th->panel);

    int track = POSBAR_H;
    int thumb = (int)((float)track * (float)GRID_H / (float)(GRID_H + max));
    if (thumb < POSBAR_THUMB_MIN) {
        thumb = POSBAR_THUMB_MIN;   /* so it never vanishes at 128 rows */
    }
    int travel = track - thumb;
    int pos = (int)((scroll_y / max) * (float)travel);

    ui_fill(POSBAR_X, POSBAR_Y + pos, POSBAR_W, thumb, th->text_dim);
}

/* ------------------------------------------------------------------ screen */

/**
 * @brief Open on the first tab that has anything in it.
 *
 * Rail order is the priority order, so this resolves to Recent if you have played anything,
 * Favourites if you have not but have starred something, and otherwise the first system you own
 * games for. A fixed starting tab was N64, which on a first boot is right by luck and on every
 * subsequent boot lands you one tab away from the thing you were doing.
 *
 * Falls back to N64 when the whole library is empty, so the empty state appears somewhere
 * sensible rather than on Recent, where "nothing here" is ambiguous between "no library" and
 * "nothing played yet".
 */
static tab_t pick_opening_tab (app_t *app) {
    static uint16_t probe[MAX_VIEW];
    for (int t = 0; t < TAB_COUNT; t++) {
        if (library_tab_view(app->lib, (tab_t)t, probe, MAX_VIEW) > 0) {
            return (tab_t)t;
        }
    }
    return TAB_N64;
}

static void grid_enter (app_t *app) {
    /* Armed once per power-on, not once per visit: coming back from the detail sheet must not
     * replay the boot animation -- nor silently move the user to another tab, which is why the
     * opening tab is chosen here and not on every entry.
     *
     * boot_plate_arm() is a no-op when the picker already armed it, which is what happens on a
     * card with more than one player: the plate lifted off the picker and the grid is arriving
     * behind a screen the user has already been looking at. The opening tab still has to be
     * chosen exactly once, so that keeps its own flag. */
    static bool opened;
    if (!opened) {
        opened = true;
        boot_plate_arm();
        tab = pick_opening_tab(app);
        debugf("GRID opening on %s\n", library_tab_label(tab));
    }

    /* The cursor is NOT reset here, and that is the whole point of it being a file-scope static.
     * It used to be, so backing out of a game sheet dropped you at the top of the tab -- open the
     * fortieth title, read it, press B, and you are looking at the first four again with forty
     * presses between you and where you were. Every list in every console menu keeps its place;
     * this one lost it on the one journey people make most.
     *
     * rebuild_view() has already clamped it, which covers the two ways the view can shrink
     * underneath a held position: un-favouriting from the sheet while the Favorites tab is up,
     * and switching to a player whose Recent list is shorter.
     *
     * The scroll is snapped rather than animated, because the sheet has been covering the grid --
     * a scroll that animates from wherever it left off would play under a screen nobody can see
     * and arrive looking like a jump. */
    rebuild_view(app);
    retarget_scroll();
    scroll_y = scroll_target;
    tween_start(&grow, DUR_TILE_GROW);
}

/**
 * @brief Is the grid worth revealing yet?
 *
 * The first row, counting a tile with no art as settled -- waiting for a card that will never
 * arrive is how an adaptive hold turns into the fixed ceiling for every user with an
 * unillustrated library.
 *
 * One row and not two, which is what this asked for first. On the SD card's own corpus a card
 * costs 259,633 us, and the plate converts 72 % of its working hold into decode (1.04 s of decode
 * inside the 1.45 s between the rise ending and the release). One row therefore lands at a
 * measured 1,998 ms hold, released by the grid. Two rows would be 2.89 s of hold, which overruns
 * the 3.0 s ceiling and gives the worst of both -- the longest possible plate AND an incomplete
 * first screen, which is exactly what the two-row version measured before this was cut back.
 */
static bool grid_worth_revealing (app_t *app) {
    if (app->lib == NULL) {
        return true;            /* nothing to wait for; the fault screen owns this case */
    }
    int want = cols;
    if (want > view_count) {
        want = view_count;
    }
    for (int i = 0; i < want; i++) {
        uint8_t s = app->lib->records[view[i]].art_state;
        if (s != ART_READY && s != ART_NONE) {
            return false;
        }
    }
    return true;
}

static void grid_update (app_t *app, float dt) {
    const input_t *in = &app->input;

    /* The plate swallows input while it is up, so a button pressed during boot does not land on
     * a grid the user cannot see yet. It does NOT stop the grid updating -- scrolling, decoding
     * and the selection tween all run underneath, which is what makes the reveal a reveal. */
    if (boot_plate_step(dt, grid_worth_revealing(app))) {
        return;
    }
    int prev = cursor;

    /* The rail runs left to right and stops at both ends: tabs 0..TAB_COUNT-1, then the chip.
     * It used to wrap with a modulo, which cannot survive the chip -- wrapping would make
     * "keep pressing L" cycle past the player plate forever instead of arriving at it. */
    /* Z pages left alongside L. Both triggers sit under the same finger on the left shoulder, and
     * a rail that only moves one way from there is a rail you have to look at the controller to
     * use. R alone pages right, because there is no second right trigger to pair it with. */
    if (input_pressed(in, BTN_L) || input_pressed(in, BTN_Z)) {
        if (tab > 0) {
            sound_play_effect(SFX_CURSOR);
            tab = (tab_t)(tab - 1);
            rebuild_view(app);
            cursor = 0;
        } else {
            /* Left of the first tab is the picker, and arriving IS opening it. There used to be a
             * focus state here -- the chip lit up, A opened it -- and it was wrong twice over: it
             * made reaching the picker two presses instead of one, and while the chip was lit the
             * tab still drew as selected, so the rail showed two selected things at once. A
             * destination that needs confirming is a menu item, and this is a rail. */
            sound_play_effect(SFX_ENTER);
            app_goto(app, SCREEN_PROFILES);
            return;
        }
    }
    if (input_pressed(in, BTN_R) && tab < TAB_COUNT - 1) {
        sound_play_effect(SFX_CURSOR);
        tab = (tab_t)(tab + 1);
        rebuild_view(app);
        cursor = 0;
    }

    if (view_count > 0) {
        if (in->left  && cursor > 0)                cursor--;
        if (in->right && cursor < view_count - 1)   cursor++;
        if (in->up    && cursor >= cols)            cursor -= cols;
        if (in->down) {
            /* Stepping down from the last partial row must land on the final title rather than
             * refusing to move, or the bottom-right corner of the library is unreachable. */
            if (cursor + cols < view_count) {
                cursor += cols;
            } else if (cursor / cols < rows_total() - 1) {
                cursor = view_count - 1;
            }
        }
    }

    if (cursor != prev) {
        /* Section 6: cursor fires on every accepted step INCLUDING repeats, because a held
         * direction that stops moving at the end of a row must sound different from one that is
         * still travelling. Tab changes get it too -- they are the same gesture sideways. */
        sound_play_effect(SFX_CURSOR);
        tween_start(&grow, DUR_TILE_GROW);
        frames_since_move = 0;
    } else {
        frames_since_move++;
    }

    if (view_count > 0 && input_pressed(in, BTN_A)) {
        sound_play_effect(SFX_ENTER);
        grid_open(app, SCREEN_DETAIL);
    }
    if (view_count > 0 && input_pressed(in, BTN_CRIGHT)) {
        /* Marked dirty, not written. A favourite is one button press and the user may make a
         * dozen in a row; a file rewritten per press is a filesystem round trip per press.
         * playstate_save() runs when the menu is on its way out instead. */
        lib_record_t *r = &app->lib->records[view[cursor]];
        r->flags ^= LIBF_FAVORITE;
        playstate_touch();
        sound_play_effect(SFX_SETTING);
        if (tab == TAB_FAVORITES) {
            rebuild_view(app);          /* un-favouriting from the Favorites tab removes it */
            if (cursor >= view_count) {
                cursor = view_count > 0 ? view_count - 1 : 0;
            }
        }
    }

    /* B, and only when there is somebody to switch to. This was Z, on the reasoning that Z was the
     * one button the grid did not already spend -- but Z now pages the rail left with L, which is
     * worth more, because paging is something you do constantly and switching player is something
     * you do once a session. B is free here in a way it is nowhere else: every other screen spends
     * it on Back, and the grid is the root, so there is nothing behind it to go back to.
     *
     * Switching player is a per-session action for a family, so burying it in Settings would be
     * wrong even though Settings also offers it. Silent when there is one profile: a button that
     * does nothing is better unbound than bound to a screen with one row. */
    if (input_pressed(in, BTN_B) && profile_count() > 1) {
        sound_play_effect(SFX_ENTER);
        app_goto(app, SCREEN_PROFILES);
    }

    if (input_pressed(in, BTN_START)) {
        sound_play_effect(SFX_ENTER);
        /* Start is settings, not launch. Launching from the grid without seeing what a game
         * needs -- save type, Expansion Pak, whether cheats are ticked -- is how you boot with
         * the wrong thing on. The sheet is one button away and answers all three. */
        app_goto(app, SCREEN_SETTINGS);
    }

    retarget_scroll();
    scroll_y = smooth_towards(scroll_y, scroll_target, SMOOTH_RATE_SCROLL, dt);
    if (fabsf(scroll_target - scroll_y) < 0.5f) {
        scroll_y = scroll_target;
    }

    /* The whole tab view, not just the visible window -- the comment here used to claim the
     * opposite of what the loop does. It is a compare and a rare add per record, so at 500 titles
     * it is roughly 50 us a frame against 16,700, and narrowing it to the twelve on screen would
     * mean tracking which twelve. Left as it is, described as it is. */
    for (int i = 0; i < view_count; i++) {
        lib_record_t *r = &app->lib->records[view[i]];
        if (r->art_age < DUR_TILE_ARRIVAL) {
            r->art_age += dt;
        }
    }

    tween_step(&grow, dt);
    pulse_phase += dt * SEL_PULSE_HZ * 2.0f * (float)M_PI;
    if (pulse_phase > 2.0f * (float)M_PI) {
        pulse_phase -= 2.0f * (float)M_PI;
    }
}

static void grid_render (app_t *app, surface_t *fb) {
#ifdef ALLOCWATCH_SELFTEST
    /* Deliberate per-frame allocation, so the zero reported by tools/inputs/idle.txt can be
     * shown to be a measurement rather than a broken counter. Never defined by any real build.
     *
     * The pointer goes through a volatile, because the obvious `free(malloc(64))` is deleted
     * outright by GCC's allocation DCE under -flto -- which made the first version of this
     * self-test report zero and very nearly certified a broken counter as a passing gate. */
    static volatile uint32_t selftest_acc;
    void *p = malloc(64);
    selftest_acc += (uint32_t)(uintptr_t)p;      /* the value escapes, so DCE cannot remove it */
    if (p == NULL) {
        debugf("selftest: malloc failed\n");
    }
    free(p);
#endif

    const theme_t *th = app->theme;

    thumbcache_begin_frame(app->thumbs);

    /* Ask for the selected tile before anything else.
     *
     * The draw loop below runs unselected-first, so the selection's shadow and growth land on top
     * of its neighbours instead of under them. The side effect is that the tile the user is
     * actually looking at is the LAST one added to the decoder's want list -- sixteenth of
     * sixteen on a full screen -- and the decoder serves that list in order. On the SD card's own
     * library that put 1080 Snowboarding, the tile under the cursor at boot, behind every other
     * visible card: its art path was not even resolved until log line 10,246, by which point the
     * rest of the grid had filled in and it was still drawing as a placeholder.
     *
     * This is the same priority inversion recorded in AUDIT.md 1f.1, arriving by a different
     * route -- that one was the decoder walking the library from index 0, this one is the want
     * list being built in painter's order. thumbcache_get() dedupes, so this only reorders. */
    if (view_count > 0) {
        (void)thumbcache_get(app->thumbs, app->lib, view[cursor]);
    }

    /* Then the rows just off screen, so the art for the row a scroll is about to reveal is
     * already in a slot rather than being looked up once it arrives.
     *
     * Asked for here, from the screen, rather than left to thumbcache_run's own prefetch passes.
     * Those walk the library from index 0, so on a 500-title card they fill the pool with
     * whatever sits at the front of the alphabet and the tiles either side of the cursor never
     * get a slot. Proximity is a fact only the grid knows -- it owns the scroll position and the
     * tab view -- so the grid is where it has to be expressed.
     *
     * These are ordinary wants, so they claim free slots but come after everything visible: the
     * visible passes inside thumbcache_run are tried first and a want made this frame can never
     * be evicted. */
    {
        int sy0 = (int)roundf(scroll_y);
        int lo = sy0 / row_pitch() - THUMB_PREFETCH_ROWS;
        int hi = (sy0 + GRID_H) / row_pitch() + THUMB_PREFETCH_ROWS;
        for (int row = lo; row <= hi; row++) {
            for (int col = 0; col < cols; col++) {
                int idx = row * cols + col;
                if (idx >= 0 && idx < view_count) {
                    (void)thumbcache_get(app->thumbs, app->lib, view[idx]);
                }
            }
        }
    }

    rdpq_attach(fb, NULL);
    rdpq_set_mode_fill(color_from_packed16(th->bg));
    rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H);

    /* The ambient wash used to be here: a 420 x 300 quad of the selected game's colour, eased,
     * meant to read as light in the room behind the art.
     *
     * It never did. The quad has hard edges and the grid does not cover it -- not on an empty tab,
     * not on a sparse one, and not even on a full one, because the 12 px gaps between tiles let it
     * through in a band that stops dead at the quad's border. What it actually read as was a
     * rectangle sitting behind the library, which is precisely the failure docs/design/README.md
     * warned about when it asked for "light in the room rather than a coloured panel".
     *
     * The handoff already nominated it as the first thing to cut (10.3). Cut. That also buys back
     * the 126,000 blended pixels DESIGN.md section 4 costed it at, roughly 2.0 ms of the 10.3 ms
     * fill estimate. */

    screen_grid_draw_rail(app, false);

    /* Round once, here. Everything downstream is integer, so a fractional scroll can never
     * reach a blit and shimmer. */
    int sy = (int)roundf(scroll_y);

    /* Clip top and bottom only. A selected tile in column 0 or 3 overhangs the grid block
     * horizontally by 6 px and must not be cut. */
    rdpq_set_scissor(0, GRID_Y, SCREEN_W, GRID_Y + GRID_H);

    int first_row = sy / row_pitch();
    int last_row = (sy + GRID_H) / row_pitch();

    for (int pass = 0; pass < 2; pass++) {
        for (int row = first_row; row <= last_row; row++) {
            for (int col = 0; col < cols; col++) {
                int idx = row * cols + col;
                if (idx < 0 || idx >= view_count) {
                    continue;
                }
                bool selected = (idx == cursor);
                /* Unselected first, selected last, so its shadow and growth overlap
                 * neighbours instead of being overdrawn by them. */
                if (selected != (pass == 1)) {
                    continue;
                }

                const lib_record_t *rec = &app->lib->records[view[idx]];
                int cx = col_x(col);
                int cy = row_y(row) - sy;

                /* The cell is the tab's, the art is this cover's own shape scaled to fit inside
                 * it and centred both ways. On a tab of one shape the two are the same and the
                 * centring is a no-op -- which is the point of taking the tallest shape rather
                 * than a fixed one.
                 *
                 * Fitted rather than clamped. The old version pinned the height to the cell and
                 * left the width alone, which squashed rather than scaled; it only ever showed up
                 * in the transient where a cover is measured after its tab was laid out, but a
                 * distorted cover is a worse lie than a small one, and both are worse than the
                 * crop this whole scheme exists to avoid. */
                art_shape_t a = boxart_fit_into(thumbcache_record_shape(rec), cell_w, cell_h);
                int aw = a.w, ah = a.h;
                int x = cx + (cell_w - aw) / 2;
                int y = cy + (cell_h - ah) / 2;

                if (selected) {
                    float t = ease_bezier(tween_t01(&grow), EASE_TILE_GROW);
                    int w = (int)lerpf(aw, aw + SEL_GROW_W, t);
                    int h = (int)lerpf(ah, ah + SEL_GROW_H, t);
                    int gx = x + (aw - w) / 2;
                    int gy = y + (ah - h) / 2;

                    ui_wash(gx + SEL_SHADOW_DX, gy + SEL_SHADOW_DY, w, h,
                         th->sel_shadow, th->sel_shadow_a);
                    draw_tile(app, rec, view[idx], gx, gy, w, h, true);
                    draw_badges(app, rec, gx, gy, w, h);

                    /* Hue crossfade rather than a brightness blink, so the tile stays equally
                     * visible at every point in the cycle. */
                    float k = (1.0f - cosf(pulse_phase)) * 0.5f;
                    color_t a = color_from_packed16(th->sel_outline);
                    color_t b = color_from_packed16(th->text_accent);
                    color_t mix = RGBA32((int)lerpf(a.r, b.r, k),
                                         (int)lerpf(a.g, b.g, k),
                                         (int)lerpf(a.b, b.b, k), 255);
                    rdpq_set_mode_fill(mix);
                    rdpq_fill_rectangle(gx - SEL_OUTLINE, gy - SEL_OUTLINE,
                                        gx + w + SEL_OUTLINE, gy);
                    rdpq_fill_rectangle(gx - SEL_OUTLINE, gy + h,
                                        gx + w + SEL_OUTLINE, gy + h + SEL_OUTLINE);
                    rdpq_fill_rectangle(gx - SEL_OUTLINE, gy, gx, gy + h);
                    rdpq_fill_rectangle(gx + w, gy, gx + w + SEL_OUTLINE, gy + h);
                } else {
                    draw_tile(app, rec, view[idx], x, y, aw, ah, false);
                    draw_badges(app, rec, x, y, aw, ah);
                }
            }
        }
    }

    rdpq_set_scissor(0, 0, SCREEN_W, SCREEN_H);

    /* Drawn after the tiles so the 4 px of selection shadow that reaches x 622 sits behind it
     * rather than on top. See docs/DESIGN.md 5.1. */
    draw_position_bar(app);
    draw_footer(app);

    /* Last, over a finished grid. The plate is opaque black, so everything above is drawn and
     * immediately covered for the first 1.3 s -- deliberately. It costs one screen of fill on
     * ~78 frames of a boot and it is what makes the curtain reveal a grid that is already alive
     * rather than one that starts when the curtain lifts. */
    boot_plate_draw(MENU_VERSION, app->lib != NULL ? app->lib->count : 0);

    rdpq_detach_show();
}

/* Decode budget per frame, in microseconds of emulated CPU.
 *
 * These are measured, not guessed. A card costs ~155,000 us to decode, so the budget divided
 * into that is the wall-clock wait per tile: at the 2,000 us this originally used, one tile
 * took 1.5 s and filling the twelve visible ones took nineteen seconds. That is the whole
 * reason the number is split in two.
 *
 * IDLE is spent when the cursor has not moved recently, which is when the user is looking at
 * the grid and waiting for it to fill. MOVING is spent while they are scrolling, when frame
 * rate matters more than art and the tiles are going past too fast to read anyway. */
#ifndef DECODE_BUDGET_IDLE_US
#define DECODE_BUDGET_IDLE_US   6000
#endif
#ifndef DECODE_BUDGET_MOVING_US
#define DECODE_BUDGET_MOVING_US 1200
#endif

/* Atlas fetches are budgeted separately from decodes, because they are a different size of thing:
 * a slot is 27,440 bytes read in one go, where a decode is measured in whole fields per row.
 *
 * The moving budget is deliberately the larger of the two. While the cursor is travelling the
 * decoder is switched off entirely, so this is all the background phase has left to do, and a
 * scroll is precisely the moment new tiles are needed fastest. 4,000 us is a quarter of a field
 * and covers several slots at any plausible SD rate. */
#ifndef FETCH_BUDGET_MOVING_US
#define FETCH_BUDGET_MOVING_US  4000
#endif
#ifndef FETCH_BUDGET_IDLE_US
#define FETCH_BUDGET_IDLE_US    2000
#endif

/**
 * @brief The two halves of getting art on screen, in the order they have to happen.
 *
 * **Fetching** is reading a finished tile out of `thumbs.pak` -- one seek and about 27 KB -- and
 * is what actually fills the grid. It is cheap enough to do while the cursor is moving and always
 * runs first, because a tile the atlas already has should never wait behind anything.
 *
 * **Building** is decoding a cover that is not in the atlas yet, and it is the expensive half:
 * one row of a real card costs 5,000-19,000 us, a whole 60 Hz field for a single row. It runs on
 * @p decode_us, which callers set to zero whenever a dropped frame would read as judder.
 *
 * On storage with no atlas -- ares, whose DFS is read-only -- neither of those can work, so the
 * old on-demand decoder runs instead and behaviour there is unchanged. That is also the reason no
 * regression script exercises the path above: see AUDIT §5, which already names this hole.
 */
static void art_background (app_t *app, uint32_t decode_us, uint32_t fetch_us) {
    if (!thumbstore_available()) {
        if (decode_us > 0) {
            thumbcache_run(app->thumbs, app->lib, decode_us);
        }
        return;
    }
    thumbcache_fetch(app->thumbs, app->lib, fetch_us);
    if (decode_us > 0) {
        thumbcache_build(app->thumbs, app->lib, decode_us);
    }
}

/**
 * @brief Decode art in the window where the CPU would otherwise wait on the RDP.
 */
static void grid_background (app_t *app, uint32_t budget_ticks) {
    (void)budget_ticks;

    /* The boot plate decoded nothing at all, for its entire duration.
     *
     * grid_update() returns early while the plate is stepping, before the frames_since_move++
     * below it, so the counter sat at 0 for all ~78 frames of the plate and the settle gate never
     * opened. The file comment in boot_plate.c claims the library "has been scanning and decoding
     * underneath for the whole 1.64 s"; it had been doing neither, and the first decode began a
     * quarter-second AFTER the curtain lifted, which is exactly the cold reveal the plate exists
     * to prevent. Found by reading, not by measurement -- it is invisible unless you notice that
     * the tiles pop in slightly too late.
     *
     * boot_plate_working() excludes the rise and the curtain, so the two animated stretches keep
     * the whole field and only the static hold is spent working. */
    if (boot_plate_working()) {
        art_background(app, DECODE_BUDGET_BOOT_US, DECODE_BUDGET_BOOT_US);
        return;
    }
    if (!boot_plate_done()) {
        return;
    }

    /* Decode NOTHING while the selection is moving, rather than decoding on a small budget.
     *
     * A budget can only stop between rows, and one row of a real card costs 5,000-19,000 us --
     * a whole 60 Hz field for a single row of a single image. So "1,200 us while moving" spent
     * 9,000 us per frame in practice and 40 % of frames took two fields. Sweeping the budget
     * from 0 to 12,000 us moved the frame rate by less than one fps, because the smallest unit
     * of work the budget can stop on is already larger than the frame.
     *
     * Nothing here makes decoding cheaper. It moves it to where dropped frames do not read as
     * judder: art appears a quarter-second after you stop, which is when you can actually look
     * at it, and the scroll itself stays smooth. The permanent answer is to not decode twice --
     * see docs/AUDIT.md on the on-disk cache.
     *
     * The atlas is exempt, and always should have been. Everything above is an argument about
     * PNG decoding; a tile that has already been decoded on some previous boot is one seek and a
     * 27 KB read, which is nowhere near a field. Gating that too is what made a warm card behave
     * like a cold one -- the reported symptom was tiles refusing to appear until the scrolling
     * stopped, on a library whose art was entirely in thumbs.pak. */
    if (frames_since_move < DECODE_SETTLE_FRAMES) {
        art_background(app, 0, FETCH_BUDGET_MOVING_US);
        return;
    }
    art_background(app, DECODE_BUDGET_IDLE_US, FETCH_BUDGET_IDLE_US);
}

const screen_t SCREEN_GRID_DEF = {
    .id         = SCREEN_GRID,
    .enter      = grid_enter,
    .update     = grid_update,
    .render     = grid_render,
    .background = grid_background,
};
