/**
 * @file theme.h
 * @brief Colour tokens and layout metrics.
 * @ingroup ui
 *
 * Transcribed from docs/design/README.md sections 1 and 2. Every colour is stored as a packed
 * RGBA5551 word because that is what the framebuffer is; the source hex in the handoff already
 * sits on the 5-bit ladder, so rounding a channel to the nearest 1/31 is a no-op and the
 * mockup and the console draw the same pixel.
 *
 * Alpha is a blend factor, not a texel channel -- RGBA5551 carries one alpha bit -- so the
 * blended tokens are a colour plus a separate 0-255 factor fed to the blender.
 *
 * Button glyph colours are deliberately NOT in the theme. They describe a physical controller,
 * so they must not change when the theme does.
 */

#ifndef UI_THEME_H__
#define UI_THEME_H__

#include <stdbool.h>
#include <stdint.h>
#include <libdragon.h>

/** @brief Pack 8-bit RGB into RGBA5551 with the alpha bit set. */
#define RGBA5551(r, g, b) \
    ((uint16_t)((((r) >> 3) << 11) | (((g) >> 3) << 6) | (((b) >> 3) << 1) | 1))

/** @brief Colour and blend tokens. See docs/design/README.md section 2. */
typedef struct {
    const char *name;

    uint16_t bg, bg_alt, panel, panel_alt;
    uint16_t text, text_dim, text_accent;
    uint16_t tab_active, tab_inactive, tab_underline;
    uint16_t sel_outline;
    uint16_t sel_shadow;   uint8_t sel_shadow_a;
    uint16_t badge_fav, badge_save;
    uint16_t overlay;      uint8_t overlay_a;
    uint8_t  tile_dim_a;   /**< wash of bg over every unselected tile */
} theme_t;

/** @brief Controller glyph colours. Hardware, not theme. */
#define BTN_A_COLOR      RGBA5551(0x29, 0x63, 0xCE)
#define BTN_B_COLOR      RGBA5551(0x42, 0xA5, 0x4A)
#define BTN_Z_COLOR      RGBA5551(0x6B, 0x6B, 0x73)
#define BTN_C_COLOR      RGBA5551(0xF7, 0xB5, 0x21)
#define BTN_START_COLOR  RGBA5551(0xDE, 0x21, 0x31)

/* ---------------------------------------------------------------- layout metrics
 * Fixed, not themeable. One resolution, no breakpoints, no reflow. The grid arithmetic
 * depends on these exactly; see docs/DESIGN.md.
 */
#define SCREEN_W            640
#define SCREEN_H            480

#define SAFE_X              16
#define SAFE_Y              16
#define SAFE_W              608
#define SAFE_H              448

#define TABRAIL_X           16
#define TABRAIL_Y           16
#define TABRAIL_W           608
#define TABRAIL_H           48
#define TAB_PAD             20      /**< total horizontal padding around a tab's content */
#define TAB_ICON            24      /**< icon box for Recent and Favourites, which are always icons */
#define TAB_GLYPH_W         12      /**< body font advance, size 20 */

#define GRID_X              16
#define GRID_Y              72
#define GRID_W              596
#define GRID_H              352

/* A tile is as wide as a column and as tall as the box it holds -- and how wide a column is
 * depends on the box. See library/boxart.h.
 *
 * There are two column counts and a rule that picks between them: **the fewest columns that still
 * show two whole rows**. Four columns is 140 px, five is 109, and a row must fit twice inside the
 * grid window, so a tile may be at most TILE_H_TWO_ROW = 162 px tall.
 *
 *   portrait   0.702   140 wide would be 200 tall, too tall   -> 5 columns, 109 x 155
 *   square     1.000   140 wide is 140 tall                   -> 4 columns, 140 x 140
 *   landscape  1.429   140 wide is 98 tall                    -> 4 columns, 140 x 98
 *
 * A single width was tried first and cannot serve both ends. 109 was picked for portrait -- at
 * four columns a portrait tile is 199 px, one and a half rows -- and it makes a landscape cover
 * 109 x 76, which is 8,284 pixels against the 13,720 the old fixed 140 x 98 tile gave it. The SD
 * card this is developed against holds 28 covers and every single one of them is landscape,
 * 1.369 to 1.469, so on that card five columns shrank every tile on the screen by 40 %.
 *
 * The rule gives each shape whichever of the two widths it can afford, and it is monotone in
 * height -- a taller shape never gets a wider column -- which is what makes a mixed tab work: a
 * tab takes the column count of the tallest shape it holds, so every other shape in it is drawn
 * NARROWER than the size it is cached at, never wider. Nothing is ever upscaled out of the atlas.
 *
 * Four columns of 140 uses all 596 px exactly. Five of 109 leaves 3 px over; giving them to the
 * tiles would need 110 px columns, which do not fit. */
#define TILE_GAP            12
#define TILE_COLS_WIDE      4
#define TILE_COLS_NARROW    5
#define TILE_W_FOR(c)       ((GRID_W - ((c) - 1) * TILE_GAP) / (c))
#define TILE_W_WIDE         TILE_W_FOR(TILE_COLS_WIDE)      /* 140 */
#define TILE_W_NARROW       TILE_W_FOR(TILE_COLS_NARROW)    /* 109 */

/* Heights the shape table may produce. The floor and ceiling exist so a typo in boxart.ini
 * cannot make a tile that will not fit its slot in the atlas or its row on the screen:
 * TILE_H_MAX bounds thumbstore's slot size, and both bounds are checked at parse time. 176 is an
 * aspect of 0.62 at the narrow width, taller than any box anybody has claimed; 64 is 2.19 at the
 * wide one, wider than any cover that is not a banner. */
#define TILE_H_MIN          64
#define TILE_H_MAX          176

/* Selection is a whole-pixel rect, not a scale factor. A fractional origin would put a
 * texture blit on a half pixel, and that shimmers on this hardware.
 *
 * It grows by a fixed number of pixels rather than by a fraction, so a square Game Boy tile and a
 * tall N64 one lift by the same amount -- a percentage would make the tall one jump further and
 * the two would read as different animations. */
#define SEL_GROW_W          12
#define SEL_GROW_H          8
#define SEL_DX              (-(SEL_GROW_W / 2))
#define SEL_DY              (-(SEL_GROW_H / 2))
#define SEL_SHADOW_DX       4
#define SEL_SHADOW_DY       6
#define SEL_OUTLINE         2

/* How far a selected tile reaches outside its own cell, vertically.
 *
 * It is centred on the cell as it grows, so it gains SEL_GROW_H / 2 = 4 px at each end, then the
 * outline adds 2 above and the shadow offset adds 6 below. The grid is scissored top and
 * bottom -- it has to be, or tiles would scroll over the tab rail and the footer -- so at the
 * ends of the list the overhang has nowhere to go and the first and last rows were being clipped:
 * 6 px off the top of the top row, 10 px off the bottom of the last one, visible as a flat cut
 * across the selection outline. Padding the scrollable content by the overhang gives it room
 * inside the window instead. Costs 16 px of the peek row, which is the right trade. */
#define SEL_GROW_Y          (SEL_GROW_H / 2)                        /* 4 */
#define GRID_PAD_TOP        (SEL_GROW_Y + SEL_OUTLINE)              /* 6 */
#define GRID_PAD_BOT        (SEL_GROW_Y + SEL_SHADOW_DY)            /* 10 */

/* The tallest a tile may be and still show two whole rows, which is the rule that chooses between
 * the two column widths above. Two rows and a gap inside the padded window: (352 - 6 - 10 - 12)/2.
 *
 * Two rather than three, and this is the trade the whole scheme turns on. Three rows of landscape
 * would need a 108 px ceiling, which sends square art to the narrow column as well and gives back
 * most of what this bought. Two rows is what a portrait grid has always shown here, so the rule
 * asks nothing of the wide shapes that the narrow one does not already accept. */
#define TILE_H_TWO_ROW      ((GRID_H - GRID_PAD_TOP - GRID_PAD_BOT - TILE_GAP) / 2)   /* 162 */

/* The handoff puts the position bar at 616. A selected column-3 tile spans 466..618 and its
 * shadow reaches 622, so both would sit on top of it -- and column 3 is a quarter of all
 * selections. Moved to 618 so the tile's exclusive right edge at 617 clears it, and drawn
 * last so the shadow sliver behind it is hidden. See docs/DESIGN.md section 5.1. */
#define POSBAR_X            618
#define POSBAR_Y            GRID_Y
#define POSBAR_W            6
#define POSBAR_H            GRID_H
#define POSBAR_THUMB_MIN    24

#define FOOTER_X            0
#define FOOTER_Y            424
#define FOOTER_W            640
#define FOOTER_H            56
#define FOOTER_CONTENT_H    40

#define BADGE_SLOT          20
#define BADGE_INSET         4
#define FAV_TRIANGLE        18

/* The ambient wash's geometry lived here. Removed with the wash itself -- see the note in
 * screen_grid.c's render() for why a hard-edged quad behind a gapped grid could not work. */

/**
 * Two, not one, and this is a property of the video mode rather than a taste in line weights.
 *
 * The framebuffer is 640x480 and the VI output area is 240 lines, so display_init() programs
 * VI_Y_SCALE to 2048/1024 -- measured, not inferred: the register reads 2048 with the output
 * region at y 35..515. Two framebuffer rows per scanline, and with INTERLACE_OFF the sampling
 * offset never alternates between fields, so the VI scans rows 0, 2, 4, ... and *the odd rows of
 * the framebuffer are never shown at all*.
 *
 * A one-pixel horizontal line is therefore a coin flip. The bottom edge of an empty profile card
 * lands at y 235 and y 407, both odd, so it was drawn correctly every frame and displayed never
 * -- which is exactly what it looked like. The top edge at y 78 and y 250 is even and was fine,
 * so the card read as a three-sided box.
 *
 * Two pixels always covers one even row, so a HAIRLINE is one visible scanline wherever it is
 * put. It costs nothing on screen: a 1 px line at an even y is also one scanline.
 *
 * The harness cannot see this. DBG_FBDUMP copies RDRAM, so a framebuffer dump shows both edges
 * present and hashes them identically -- which is why this survived a round of measuring the
 * dashes themselves. See docs/AUDIT.md 1ai.
 */
#define HAIRLINE            2
#define ACCENT_BAR          4

/* There is no COL_X, no ROW_Y and no ROW_PITCH here any more. A column is as wide as the tallest
 * box in the tab can afford and a row is as tall as that box, both of which are runtime facts --
 * see col_x() and row_pitch() in screen_grid.c. Leaving compile-time macros behind would have
 * given half the file a stale answer that still compiled. */

_Static_assert(TILE_COLS_WIDE * TILE_W_WIDE + (TILE_COLS_WIDE - 1) * TILE_GAP <= GRID_W,
               "four wide columns do not fit across the grid");
_Static_assert(TILE_COLS_NARROW * TILE_W_NARROW + (TILE_COLS_NARROW - 1) * TILE_GAP <= GRID_W,
               "five narrow columns do not fit across the grid");
_Static_assert(GRID_X + (TILE_COLS_WIDE - 1) * (TILE_W_WIDE + TILE_GAP)
               + TILE_W_WIDE + SEL_GROW_W / 2 <= POSBAR_X,
               "a selected wide tile in the last column overlaps the position bar");
_Static_assert(GRID_X + (TILE_COLS_NARROW - 1) * (TILE_W_NARROW + TILE_GAP)
               + TILE_W_NARROW + SEL_GROW_W / 2 <= POSBAR_X,
               "a selected narrow tile in the last column overlaps the position bar");

/* A wide tile is bounded by the two-row rule, not by TILE_H_MAX, so it is the rule that has to
 * clear the atlas slot. And the wide column is only worth having if a square fits in it -- if it
 * did not, square art would join portrait in the narrow column and there would be one width. */
_Static_assert(TILE_H_TWO_ROW <= TILE_H_MAX,
               "a wide tile could be taller than the atlas slot allows");
_Static_assert(TILE_W_WIDE <= TILE_H_TWO_ROW,
               "a square cover does not fit the wide column, so there is no wide column");

extern const theme_t THEME_MIDNIGHT;
extern const theme_t THEME_PHOSPHOR;
extern const theme_t THEME_PURPLE;
extern const theme_t THEME_RED;

/**
 * @brief Make @p th the live theme's font colours. Call after every change to app->theme.
 *
 * Surfaces are read from the struct as each screen draws, so they need nothing. Text does: the
 * rdpq font styles are registered state, and until this exists they were registered once in
 * fixed white and stayed white under a light palette.
 */
void theme_apply (const theme_t *th);

/** @brief Look a theme up by name, falling back to Midnight. */
const theme_t *theme_by_name (const char *name);

/** @brief Number of built-in themes. */
int theme_count (void);

/** @brief Built-in theme @p index, or Midnight if out of range. */
const theme_t *theme_at (int index);

#endif /* UI_THEME_H__ */
