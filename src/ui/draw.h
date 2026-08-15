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

/**
 * @brief ui_text() in a font other than the body face.
 *
 * Every face except #FNT_DEFAULT carries a restricted charset, because a full-charset bake at
 * 40 px is about 2.7 MB of glyphs against 681 KB at 20 px -- see docs/GOTCHAS-PROFILES.md
 * section 1.4. So a string drawn through here can only contain what that face was baked with,
 * and a character outside it draws as a hole rather than as an error. tools/charsetcheck.py
 * is what turns that into a build failure; keep new strings within reach of it.
 */
void ui_text_font (menu_font_type_t font, int x, int y, int w, rdpq_align_t align,
                   int style, const char *s);

/**
 * @brief Word-wrapped text in a box @p w wide, returning how far the pen advanced in Y.
 *
 * The credits screen flows paragraphs it did not write and cannot measure ahead of time, so it
 * needs the height back rather than assuming a row pitch. Wrapping is WRAP_WORD at @p w, which
 * is why the baked credits file joins hard-wrapped source lines into whole paragraphs -- handing
 * this 100-column fragments would re-wrap them at a width nobody chose.
 *
 * @return the Y advance of what was drawn, which is the line height times the number of lines.
 */
int ui_text_wrap (menu_font_type_t font, int x, int y, int w, int style, const char *s);

/**
 * @brief Like @ref ui_text_wrap, but breaks mid-word rather than giving up on the line.
 *
 * For URLs, and only for URLs. A URL has no spaces in it, so WRAP_WORD cannot break one and rdpq
 * ellipsises instead: the credits screen was drawing the fork's source address as
 * "https://github.com/Polprzewodnikowy/N64Flashca..." with the tail missing. An address somebody
 * has to type by hand is the one string on that screen that must survive whole, and a licence
 * screen quietly dropping the end of a source link is the worst place to save a line break.
 */
int ui_text_wrap_url (menu_font_type_t font, int x, int y, int w, int style, const char *s);

/**
 * @brief How wide @p s is in @p font, in pixels. Draws nothing.
 *
 * The keyboard needs it to put the caret after the text. The face is proportional, so a caret
 * placed at `length * a constant` drifts along a name and ends up inside the last letter -- which
 * is the kind of wrong that looks like a rounding bug and is actually a wrong model.
 */
int ui_text_width (menu_font_type_t font, const char *s);

/** @brief ui_text() that also sets the standard text render mode. For one-off labels. */
void ui_label (int x, int y, int w, rdpq_align_t align, int style, const char *s);

/**
 * @brief A padlock: a hollow shackle over a solid body.
 *
 * Drawn from primitives rather than carried as a sprite so it scales to both places it appears --
 * a row marker in the lock list and a corner badge on a grid tile. The silhouette is the whole
 * point: theme.c rule 2 forbids a badge that is only distinguishable by colour, and under
 * Phosphor the save square, the favourite triangle and this are near-identical greens.
 */
void ui_padlock (int x, int y, int w, int h, uint16_t c);

/** @brief Height, and disc diameter, of a controller-button glyph. */
#define UI_BTN_D        20

/** @brief Which controller part a glyph is standing for. */
typedef enum {
    UI_BTN_DISC = 0,   /**< A, B, Start: round face buttons */
    UI_BTN_TALL,       /**< Z: the trigger, which is not a face button and should not read as one */
} ui_btn_shape_t;

/**
 * @brief A controller button drawn as the controller wears it.
 *
 * Shell, face and glyph as pixel-span sprites, adapted at 2x from garfbargle/n64-game-template
 * (MIT) -- A blue, B green, START red, C buttons yellow with an arrow, and Z/R/L as the long
 * grey shoulders they really are, which says "not a face button" faster than any colour could.
 *
 * The one place radius is allowed. docs/design/README.md sets radius 0 everywhere "except baked
 * button glyphs", and this is that exception -- the colours are baked into the sprites rather
 * than taken from the theme, because a blue A is blue on every N64 regardless of what palette
 * the menu is wearing. @p colour and @p shape only matter to the fallback that draws a glyph
 * the sprite table does not know; every glyph the screens currently pass is in the table.
 *
 * A round sprite is 26 px on a #UI_BTN_D (20 px) layout cell and overhangs it 3 px each way;
 * shoulders are 38 px wide and hang rightward from @p x. ui_hint() accounts for both.
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
