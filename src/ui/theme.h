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
#define TAB_ICON            20      /**< icon box for a virtual tab that is not active */
#define TAB_GLYPH_W         12      /**< body font advance, size 20 */

#define GRID_X              16
#define GRID_Y              72
#define GRID_W              596
#define GRID_H              352

#define TILE_W              140
#define TILE_H              98
#define TILE_GAP            12
#define GRID_COLS           4
#define COL_PITCH           (TILE_W + TILE_GAP)   /* 152 */
#define ROW_PITCH           (TILE_H + TILE_GAP)   /* 110 */

/* Selection is a whole-pixel rect, not a scale factor. A fractional origin would put a
 * texture blit on a half pixel, and that shimmers on this hardware. */
#define SEL_W               152
#define SEL_H               106
#define SEL_DX              (-6)
#define SEL_DY              (-4)
#define SEL_SHADOW_DX       4
#define SEL_SHADOW_DY       6
#define SEL_OUTLINE         2

/* How far a selected tile reaches outside its own cell, vertically.
 *
 * It is centred on the cell as it grows, so it gains (SEL_H - TILE_H) / 2 = 4 px at each end,
 * then the outline adds 2 above and the shadow offset adds 6 below. The grid is scissored top and
 * bottom -- it has to be, or tiles would scroll over the tab rail and the footer -- so at the
 * ends of the list the overhang has nowhere to go and the first and last rows were being clipped:
 * 6 px off the top of the top row, 10 px off the bottom of the last one, visible as a flat cut
 * across the selection outline. Padding the scrollable content by the overhang gives it room
 * inside the window instead. Costs 16 px of the peek row, which is the right trade. */
#define SEL_GROW_Y          ((SEL_H - TILE_H) / 2)                  /* 4 */
#define GRID_PAD_TOP        (SEL_GROW_Y + SEL_OUTLINE)              /* 6 */
#define GRID_PAD_BOT        (SEL_GROW_Y + SEL_SHADOW_DY)            /* 10 */

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

#define HAIRLINE            1
#define ACCENT_BAR          4

/** @brief Left edge of column @p c. */
#define COL_X(c)            (GRID_X + (c) * COL_PITCH)
/** @brief Top edge of row @p r in content space, before scroll. Includes the overhang pad. */
#define ROW_Y(r)            (GRID_Y + GRID_PAD_TOP + (r) * ROW_PITCH)

extern const theme_t THEME_MIDNIGHT;
extern const theme_t THEME_CARTRIDGE;
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
