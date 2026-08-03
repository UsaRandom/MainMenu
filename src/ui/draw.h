/**
 * @file draw.h
 * @brief Primitive drawing helpers shared by every screen.
 * @ingroup ui
 *
 * These lived as statics in screen_grid.c until a second and third screen needed them. They are
 * deliberately thin -- each is one rdpq mode change plus one primitive -- because the point is
 * that every screen fills a rect the same way, not that there is a drawing framework here.
 */

#ifndef UI_DRAW_H__
#define UI_DRAW_H__

#include <stdint.h>
#include <libdragon.h>

#include "menu/fonts.h"

/** @brief Solid rect in an RGBA5551 theme colour. */
void ui_fill (int x, int y, int w, int h, uint16_t c);

/** @brief Blended wash of @p c at @p alpha. One blended layer; do not stack these. */
void ui_wash (int x, int y, int w, int h, uint16_t c, uint8_t alpha);

/** @brief Hollow rect of @p t pixels, drawn inside the given bounds. */
void ui_border (int x, int y, int w, int h, int t, uint16_t c);

/** @brief One line of text in a box @p w wide. Caller sets the render mode first. */
void ui_text (int x, int y, int w, rdpq_align_t align, int style, const char *s);

/** @brief ui_text() that also sets the standard text render mode. For one-off labels. */
void ui_label (int x, int y, int w, rdpq_align_t align, int style, const char *s);

/** @brief Height, and disc diameter, of a controller-button glyph. */
#define UI_BTN_D        20

/** @brief Which controller part a glyph is standing for. */
typedef enum {
    UI_BTN_DISC = 0,   /**< A, B, Start: round face buttons */
    UI_BTN_TALL,       /**< Z: the trigger, which is not a face button and should not read as one */
} ui_btn_shape_t;

/**
 * @brief A filled controller-button glyph with a letter in it.
 *
 * The one place radius is allowed. docs/design/README.md sets radius 0 everywhere "except baked
 * button glyphs", and this is that exception -- the colours come from the hardware
 * (BTN_A_COLOR and friends) rather than from the theme, because a blue A is blue on every N64
 * regardless of what palette the menu is wearing.
 *
 * Shape carries the same information as colour and survives being looked at quickly: Z is under
 * the controller and shaped nothing like A, so drawing it as another coloured disc says "face
 * button" about something that is not one.
 */
void ui_button (int x, int y, const char *glyph, uint16_t colour, ui_btn_shape_t shape);

/**
 * @brief A button disc followed by its label. Returns the x to place the next hint at.
 *
 * Footers are built by chaining these left to right rather than by formatting one string, so the
 * glyph is a glyph and not the letter "A" in body text.
 */
int ui_hint (int x, int y, const char *glyph, uint16_t colour, ui_btn_shape_t shape,
             const char *label);

#endif /* UI_DRAW_H__ */
