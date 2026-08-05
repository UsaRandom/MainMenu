/**
 * @file draw.c
 * @brief Primitive drawing helpers. See draw.h.
 * @ingroup ui
 */

#include <string.h>
#include <libdragon.h>

#include "ui/draw.h"
#include "ui/theme.h"

void ui_fill (int x, int y, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) {
        return;
    }
    rdpq_set_mode_fill(color_from_packed16(c));
    rdpq_fill_rectangle(x, y, x + w, y + h);
}

void ui_wash (int x, int y, int w, int h, uint16_t c, uint8_t alpha) {
    if (w <= 0 || h <= 0) {
        return;
    }
    color_t col = color_from_packed16(c);
    col.a = alpha;
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(col);
    rdpq_fill_rectangle(x, y, x + w, y + h);
}

void ui_border (int x, int y, int w, int h, int t, uint16_t c) {
    ui_fill(x, y, w, t, c);
    ui_fill(x, y + h - t, w, t, c);
    ui_fill(x, y + t, t, h - 2 * t, c);
    ui_fill(x + w - t, y + t, t, h - 2 * t, c);
}

void ui_padlock (int x, int y, int w, int h, uint16_t c) {
    /* The shackle is drawn two pixels taller than half and then overdrawn by the body, so the
     * gap in the middle of the U closes cleanly instead of leaving a one-pixel seam that reads
     * as a broken glyph at badge size. */
    int shackle_w = (w * 5) / 8;
    int shackle_h = h / 2;
    ui_border(x + (w - shackle_w) / 2, y, shackle_w, shackle_h + 2, 2, c);
    ui_fill(x, y + shackle_h, w, h - shackle_h, c);
}

/**
 * @brief Longest string ui_text() will escape. Anything past this is cut.
 *
 * Titles are laid out into a fixed-width box and were already being clipped long before this;
 * 256 is comfortably past the point where a name stops being readable on a 592 px row.
 */
#define ESCAPE_MAX 256

/**
 * @brief Copy @p s into @p out, doubling the two characters rdpq treats as markup.
 *
 * `$XX` selects a font and `^XX` selects a style in rdpq_text, and a bad pair is not ignored --
 * `__rdpq_paragraph_build` calls `assertf(!error, "invalid style id ...")`, which on this target
 * is a hard assertion into the inspector. So **any string containing a bare `$` or `^` takes the
 * menu down**, and the strings this draws include ROM filenames, ROM header titles and cheat
 * names from a corpus nobody here wrote. A game called `Foo^Bar.z64` would have crashed the grid
 * the moment its tile scrolled into view.
 *
 * Found by drawing a caret as a button glyph on the code pad, which asserted immediately. It is
 * escaped here, at the one place all text goes through, rather than sanitised at every source --
 * nothing in this tree uses the markup deliberately, so there is nothing to preserve, and a fix
 * at the sources would have to be repeated for every new source.
 *
 * @return the escaped string, which is @p out unless nothing needed escaping.
 */
static const char *escape_markup (const char *s, char *out, size_t cap) {
    const char *p = s;
    while (*p != '\0' && *p != '$' && *p != '^') {
        p++;
    }
    if (*p == '\0') {
        return s;
    }

    size_t w = 0;
    for (const char *q = s; *q != '\0' && w + 2 < cap; q++) {
        if (*q == '$' || *q == '^') {
            out[w++] = *q;
        }
        out[w++] = *q;
    }
    out[w] = '\0';
    return out;
}

void ui_text (int x, int y, int w, rdpq_align_t align, int style, const char *s) {
    if (s == NULL) {
        return;
    }
    char escaped[ESCAPE_MAX];
    s = escape_markup(s, escaped, sizeof(escaped));
    /* style_id was missing here, so every caller's STL_GRAY / STL_YELLOW rendered white and the
     * dimmed secondary text the spec asks for was never dim. fonts.c registers all seven styles
     * at init; nothing was using them. */
    rdpq_text_printn(&(rdpq_textparms_t){ .width = w, .align = align,
                                          .style_id = style, .disable_aa_fix = true },
                     FNT_DEFAULT, x, y, s, strlen(s));
}

void ui_label (int x, int y, int w, rdpq_align_t align, int style, const char *s) {
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text(x, y, w, align, style, s);
}

/* Half-widths of a 20 px disc, top half only; the bottom mirrors. Baked rather than computed
 * because it never changes and a sqrt per scanline per button per frame is silly.
 *
 * Runs of equal half-width are drawn as one taller rectangle, which takes a disc from 20 fills
 * to 11. Four hints in a footer is then 44 small fills rather than 80.
 */
static const uint8_t DISC_HW[UI_BTN_D / 2] = { 3, 5, 7, 8, 8, 9, 9, 10, 10, 10 };

/* The Z trigger, as a tall rectangle: narrower than the face-button discs and the same height, so
 * it reads as taller than it is wide without needing to be bigger than anything around it. */
#define TALL_W          14

void ui_button (int x, int y, const char *glyph, uint16_t colour, ui_btn_shape_t shape) {
    int cx = x + UI_BTN_D / 2;

    if (shape == UI_BTN_TALL) {
        ui_fill(cx - TALL_W / 2, y, TALL_W, UI_BTN_D, colour);
    } else {
        for (int i = 0; i < UI_BTN_D / 2; ) {
            int hw = DISC_HW[i];
            int run = 1;
            while (i + run < UI_BTN_D / 2 && DISC_HW[i + run] == hw) {
                run++;
            }
            /* Top half and its mirror in one pass, so the two stay symmetric by construction. */
            ui_fill(cx - hw, y + i, hw * 2, run, colour);
            ui_fill(cx - hw, y + UI_BTN_D - i - run, hw * 2, run, colour);
            i += run;
        }
    }

    /* Baseline sits low in the disc because the font's ascent is most of its box; centring on
     * the geometric middle puts the letter visibly high.
     *
     * The caret is the exception and needs another 4 px. Every other glyph on a button is a
     * capital or an arrow that fills the cap height downwards; `^` is the one whose ink sits
     * entirely at the TOP of the cap box, so the baseline that centres a letter pushes it out
     * through the top of the disc. It looked like a clipped sprite on the code pad, which is the
     * only screen that draws one. */
    int baseline = y + UI_BTN_D - 5 + ((glyph != NULL && glyph[0] == '^') ? 4 : 0);
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text(x, baseline, UI_BTN_D, ALIGN_CENTER, STL_ONBTN, glyph);
}

int ui_hint (int x, int y, const char *glyph, uint16_t colour, ui_btn_shape_t shape,
             const char *label) {
    ui_button(x, y, glyph, colour, shape);
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text(x + UI_BTN_D + 6, y + UI_BTN_D - 5, 200, ALIGN_LEFT, STL_GRAY, label);
    /* 12 px a glyph, matching the metric the detail sheet uses for its row wrapping. Both
     * follow the body font: at size 20 Firple-Bold is near enough monospace for layout. */
    return x + UI_BTN_D + 6 + (int)strlen(label) * 12 + 20;
}
