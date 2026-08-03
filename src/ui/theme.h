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
    bool     ambient_wash; /**< per-game accent behind the grid */
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

/* Ambient wash, docs/design/README.md section 4. Centred on the grid viewport, deliberately
 * smaller than it so the colour reads as light in the room rather than as a coloured panel. */
#define AMBIENT_W           420
#define AMBIENT_H           300
#define AMBIENT_X           ((SCREEN_W - AMBIENT_W) / 2)
#define AMBIENT_Y           (GRID_Y + (GRID_H - AMBIENT_H) / 2)
#define AMBIENT_ALPHA       38          /**< ~15 %; any more and unselected tiles take a tint */
#define AMBIENT_RATE        6.0f        /**< smooth_towards rate; 0.22 s to settle, per DUR_AMBIENT_REKEY */

#define HAIRLINE            1
#define ACCENT_BAR          4

/** @brief Left edge of column @p c. */
#define COL_X(c)            (GRID_X + (c) * COL_PITCH)
/** @brief Top edge of row @p r in content space, before scroll. */
#define ROW_Y(r)            (GRID_Y + (r) * ROW_PITCH)

extern const theme_t THEME_MIDNIGHT;
extern const theme_t THEME_CARTRIDGE;
extern const theme_t THEME_PHOSPHOR;

/** @brief Look a theme up by name, falling back to Midnight. */
const theme_t *theme_by_name (const char *name);

/** @brief Number of built-in themes. */
int theme_count (void);

/** @brief Built-in theme @p index, or Midnight if out of range. */
const theme_t *theme_at (int index);

#endif /* UI_THEME_H__ */
