/**
 * @file draw.c
 * @brief Primitive drawing helpers. See draw.h.
 * @ingroup ui
 */

#include <math.h>
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

void ui_text_font (menu_font_type_t font, int x, int y, int w, rdpq_align_t align,
                   int style, const char *s) {
    if (s == NULL) {
        return;
    }
    char escaped[ESCAPE_MAX];
    s = escape_markup(s, escaped, sizeof(escaped));
    rdpq_text_printn(&(rdpq_textparms_t){ .width = w, .align = align,
                                          .style_id = style, .disable_aa_fix = true },
                     font, x, y, s, strlen(s));
}

int ui_text_wrap (menu_font_type_t font, int x, int y, int w, int style, const char *s) {
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    char escaped[ESCAPE_MAX];
    s = escape_markup(s, escaped, sizeof(escaped));
    rdpq_textmetrics_t m = rdpq_text_printn(
        &(rdpq_textparms_t){ .width = w, .wrap = WRAP_WORD,
                             .style_id = style, .disable_aa_fix = true },
        font, x, y, s, strlen(s));
    /* advance_y is a float and the caller is stepping integer pixel rows. Rounding up rather
     * than truncating: a paragraph that reports one pixel short of its real height overlaps the
     * next one by a pixel every time, and over a screen of text that accumulates into a visible
     * crush. One pixel of extra air does not. */
    return (int)(m.advance_y + 0.999f);
}

int ui_text_wrap_url (menu_font_type_t font, int x, int y, int w, int style, const char *s) {
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    char escaped[ESCAPE_MAX];
    s = escape_markup(s, escaped, sizeof(escaped));
    rdpq_textmetrics_t m = rdpq_text_printn(
        &(rdpq_textparms_t){ .width = w, .wrap = WRAP_CHAR,
                             .style_id = style, .disable_aa_fix = true },
        font, x, y, s, strlen(s));
    return (int)(m.advance_y + 0.999f);
}

int ui_text_width (menu_font_type_t font, const char *s) {
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    char escaped[ESCAPE_MAX];
    s = escape_markup(s, escaped, sizeof(escaped));
    /* Laid out and freed rather than drawn. rdpq_text_printn would give the same advance_x, but
     * only by putting the string on screen -- and this is called to decide where something else
     * goes, sometimes before the text itself is drawn. */
    rdpq_paragraph_t *par = rdpq_paragraph_build(
        &(rdpq_textparms_t){ .disable_aa_fix = true }, font, s, &(int){ (int)strlen(s) });
    if (par == NULL) {
        return 0;
    }
    int w = (int)(par->advance_x + 0.999f);
    rdpq_paragraph_free(par);
    return w;
}

void ui_label (int x, int y, int w, rdpq_align_t align, int style, const char *s) {
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text(x, y, w, align, style, s);
}

#define MARQUEE_PAUSE    0.80f
#define MARQUEE_PX_S     40.0f
#define MARQUEE_ASCENT   22
#define MARQUEE_DESCENT  8

void ui_text_marquee (int x, int y, int w, rdpq_align_t fit_align, int style,
                      const char *s, float clock) {
    if (s == NULL || s[0] == '\0' || w <= 0) {
        return;
    }

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);

    int tw = ui_text_width(FNT_DEFAULT, s);
    if (tw <= w) {
        ui_text(x, y, w, fit_align, style, s);
        return;
    }

    float max_off = (float)(tw - w);
    float travel = max_off / MARQUEE_PX_S;
    float cycle = MARQUEE_PAUSE + travel + MARQUEE_PAUSE;
    float t = fmodf(clock, cycle);
    if (t < 0.0f) {
        t += cycle;
    }

    int off;
    if (t < MARQUEE_PAUSE) {
        off = 0;
    } else if (t < MARQUEE_PAUSE + travel) {
        off = (int)((t - MARQUEE_PAUSE) * MARQUEE_PX_S);
        if (off > (int)max_off) {
            off = (int)max_off;
        }
    } else {
        off = (int)max_off;
    }

    rdpq_set_scissor(x, y - MARQUEE_ASCENT, x + w, y + MARQUEE_DESCENT);
    ui_text(x - off, y, tw + 8, ALIGN_LEFT, style, s);
    rdpq_set_scissor(0, 0, SCREEN_W, SCREEN_H);
}

int ui_text_scroll_x (int box_x, int box_w, int caret_px, int caret_w) {
    int need = caret_px + caret_w;
    if (need <= box_w) {
        return box_x;
    }
    return box_x - (need - box_w);
}

/* Controller buttons drawn as the controller wears them: A blue, B green, START red, the
 * C buttons yellow with a dark arrow, and the shoulders long and grey with the letter cut in.
 * Three layers each -- a shared near-black shell, a coloured face, a glyph -- from span tables
 * adapted out of garfbargle/n64-game-template (MIT), engine/src/ui.c. The template draws them
 * at 320x240; this canvas is 640x480, so every span is doubled, which keeps the pixel-art
 * edges instead of the smear a fractional scale would make of a 13 px disc.
 *
 * Spans are inclusive pixel runs at 1x: {x0, y0, x1, y1}. */
typedef struct { uint8_t x0, y0, x1, y1; } btn_span_t;

static const btn_span_t BTN_ROUND_SHELL[] = {
    {4, 0, 8, 0}, {2, 1, 10, 1}, {1, 2, 11, 3}, {0, 4, 12, 8},
    {1, 9, 11, 10}, {2, 11, 10, 11}, {4, 12, 8, 12}
};
static const btn_span_t BTN_ROUND_FACE[] = {
    {4, 1, 8, 1}, {2, 2, 10, 3}, {1, 4, 11, 8}, {2, 9, 10, 10}, {4, 11, 8, 11}
};
static const btn_span_t BTN_WIDE_SHELL[] = {
    {2, 0, 16, 0}, {1, 1, 17, 1}, {0, 2, 18, 8}, {1, 9, 17, 9}, {2, 10, 16, 10}
};
static const btn_span_t BTN_WIDE_FACE[] = {
    {2, 1, 16, 1}, {1, 2, 17, 8}, {2, 9, 16, 9}
};

/* Five-by-seven letters, matching the template's own UI font proportions. */
static const btn_span_t GLYPH_A[] = {
    {1, 0, 3, 0}, {0, 1, 0, 2}, {4, 1, 4, 2}, {0, 3, 4, 3}, {0, 4, 0, 6}, {4, 4, 4, 6}
};
static const btn_span_t GLYPH_B[] = {
    {0, 0, 3, 0}, {0, 1, 0, 2}, {4, 1, 4, 2}, {0, 3, 3, 3}, {0, 4, 0, 5}, {4, 4, 4, 5},
    {0, 6, 3, 6}
};
static const btn_span_t GLYPH_S[] = {
    {1, 0, 4, 0}, {0, 1, 0, 2}, {1, 3, 3, 3}, {4, 4, 4, 5}, {0, 6, 3, 6}
};
static const btn_span_t GLYPH_L[] = {
    {0, 0, 0, 5}, {0, 6, 4, 6}
};
static const btn_span_t GLYPH_R[] = {
    {0, 0, 3, 0}, {0, 1, 0, 2}, {4, 1, 4, 2}, {0, 3, 3, 3}, {0, 4, 0, 6}, {3, 4, 3, 4},
    {4, 5, 4, 6}
};
static const btn_span_t GLYPH_Z[] = {
    {0, 0, 4, 0}, {4, 1, 4, 1}, {3, 2, 3, 2}, {2, 3, 2, 3}, {1, 4, 1, 4}, {0, 5, 0, 5},
    {0, 6, 4, 6}
};

/* Arrows with a two-row base, so they stay solid triangles rather than thin darts. */
static const btn_span_t GLYPH_UP[] = {
    {3, 0, 3, 0}, {2, 1, 4, 1}, {1, 2, 5, 2}, {0, 3, 6, 4}
};
static const btn_span_t GLYPH_DOWN[] = {
    {0, 0, 6, 1}, {1, 2, 5, 2}, {2, 3, 4, 3}, {3, 4, 3, 4}
};
static const btn_span_t GLYPH_LEFT[] = {
    {3, 0, 4, 0}, {2, 1, 4, 1}, {1, 2, 4, 2}, {0, 3, 4, 3}, {1, 4, 4, 4}, {2, 5, 4, 5},
    {3, 6, 4, 6}
};
static const btn_span_t GLYPH_RIGHT[] = {
    {0, 0, 1, 0}, {0, 1, 2, 1}, {0, 2, 3, 2}, {0, 3, 4, 3}, {0, 4, 3, 4}, {0, 5, 2, 5},
    {0, 6, 1, 6}
};

typedef struct {
    const btn_span_t *shell, *face, *glyph;
    uint8_t shell_n, face_n, glyph_n;
    uint8_t glyph_x, glyph_y;       /* at 1x, from the sprite's own corner */
    uint8_t w, h;                   /* at 1x */
    uint16_t face_colour, glyph_colour;
} btn_style_t;

/* One shell colour under every button, so a footer's row reads as one family. The faces are the
 * controller's own colours, same as the BTN_*_COLOR constants these sprites replaced -- the
 * theme never touches them, because a blue A is blue on every N64. */
#define BTN_SHELL_COLOUR    RGBA5551(10, 10, 12)
#define BTN_LETTER_WHITE    RGBA5551(238, 240, 245)
#define BTN_ARROW_DARK      RGBA5551(38, 30, 6)
#define BTN_SHOULDER_GREY   RGBA5551(168, 170, 175)
#define BTN_SHOULDER_DARK   RGBA5551(32, 33, 36)

#define ROUND(g, gx, gy, face_c, glyph_c) \
    { BTN_ROUND_SHELL, BTN_ROUND_FACE, g, 7, 5, sizeof(g) / sizeof(btn_span_t), \
      gx, gy, 13, 13, face_c, glyph_c }
#define WIDE(g, face_c, glyph_c) \
    { BTN_WIDE_SHELL, BTN_WIDE_FACE, g, 5, 3, sizeof(g) / sizeof(btn_span_t), \
      7, 2, 19, 11, face_c, glyph_c }

static const btn_style_t BTN_STYLE_A     = ROUND(GLYPH_A, 4, 3, RGBA5551(52, 104, 198), BTN_LETTER_WHITE);
static const btn_style_t BTN_STYLE_B     = ROUND(GLYPH_B, 4, 3, RGBA5551(56, 158, 84), BTN_LETTER_WHITE);
static const btn_style_t BTN_STYLE_START = ROUND(GLYPH_S, 4, 3, RGBA5551(198, 52, 48), BTN_LETTER_WHITE);
static const btn_style_t BTN_STYLE_C_U   = ROUND(GLYPH_UP, 3, 4, BTN_C_COLOR, BTN_ARROW_DARK);
static const btn_style_t BTN_STYLE_C_D   = ROUND(GLYPH_DOWN, 3, 4, BTN_C_COLOR, BTN_ARROW_DARK);
static const btn_style_t BTN_STYLE_C_L   = ROUND(GLYPH_LEFT, 4, 3, BTN_C_COLOR, BTN_ARROW_DARK);
static const btn_style_t BTN_STYLE_C_R   = ROUND(GLYPH_RIGHT, 4, 3, BTN_C_COLOR, BTN_ARROW_DARK);
static const btn_style_t BTN_STYLE_L     = WIDE(GLYPH_L, BTN_SHOULDER_GREY, BTN_SHOULDER_DARK);
static const btn_style_t BTN_STYLE_R     = WIDE(GLYPH_R, BTN_SHOULDER_GREY, BTN_SHOULDER_DARK);
static const btn_style_t BTN_STYLE_Z     = WIDE(GLYPH_Z, BTN_SHOULDER_GREY, BTN_SHOULDER_DARK);

/* Which sprite a call means, from the glyph the call sites already pass. The shape parameter
 * stops mattering for anything the table knows: Z arrives as UI_BTN_TALL and comes out a
 * shoulder, which is a better answer to the same question the tall rectangle was answering. */
static const btn_style_t *btn_style_for (const char *glyph) {
    if (glyph == NULL || glyph[0] == '\0' || glyph[1] != '\0') {
        return NULL;
    }
    switch (glyph[0]) {
        case 'A': return &BTN_STYLE_A;
        case 'B': return &BTN_STYLE_B;
        case 'S': return &BTN_STYLE_START;
        case '^': return &BTN_STYLE_C_U;
        case 'v': return &BTN_STYLE_C_D;
        case '<': return &BTN_STYLE_C_L;
        case '>': return &BTN_STYLE_C_R;
        case 'L': return &BTN_STYLE_L;
        case 'R': return &BTN_STYLE_R;
        case 'Z': return &BTN_STYLE_Z;
        default:  return NULL;
    }
}

static void btn_spans (const btn_span_t *spans, int n, int x, int y, uint16_t c) {
    /* One mode-set for the whole layer, not one per span. Drawing every span through ui_fill()
     * re-issued the fill mode ~18 times per button -- roughly twice the attribute churn of the
     * discs these replaced -- and the template this is adapted from batches by colour for a
     * reason: rapid fill-colour changes are exactly the RDP attribute hazard its docs record on
     * real hardware, and the M64's reimplemented RDP is the strictest one this menu runs on.
     * Three mode-sets per button (shell, face, glyph) is fewer than the eleven the old disc
     * spent. */
    rdpq_set_mode_fill(color_from_packed16(c));
    for (int i = 0; i < n; i++) {
        rdpq_fill_rectangle(x + spans[i].x0 * 2, y + spans[i].y0 * 2,
                            x + (spans[i].x1 + 1) * 2, y + (spans[i].y1 + 1) * 2);
    }
}

/* The sprite's drawn width: wider than the UI_BTN_D layout cell for the shoulders, which is the
 * one case ui_hint has to move its label for. */
static int btn_sprite_w (const btn_style_t *style) {
    return style != NULL ? style->w * 2 : UI_BTN_D;
}

void ui_button (int x, int y, const char *glyph, uint16_t colour, ui_btn_shape_t shape) {
    const btn_style_t *style = btn_style_for(glyph);

    if (style != NULL) {
        /* Bottom-aligned to the UI_BTN_D cell, not centred: the grid footer lays its hints out
         * against the bottom of a 480-line screen, and centring a 26 px sprite on the 20 px cell
         * pushed 3 px of every button below the old glyph's bottom line -- which on the console
         * was 3 px past the edge of the panel. The cell's bottom edge is the one line every call
         * site already keeps on screen, so the sprite's extra height goes upward, where the
         * footer has padding. Shoulders hang from x: they are 18 px wider than the cell, and
         * centring them would push 9 px of sprite left into whatever the previous hint drew. */
        int sx = style->w == 13 ? x + (UI_BTN_D - 26) / 2 : x;
        int sy = y + UI_BTN_D - style->h * 2;

        btn_spans(style->shell, style->shell_n, sx, sy, BTN_SHELL_COLOUR);
        btn_spans(style->face, style->face_n, sx, sy, style->face_colour);
        btn_spans(style->glyph, style->glyph_n,
                  sx + style->glyph_x * 2, sy + style->glyph_y * 2, style->glyph_colour);
        return;
    }

    /* No sprite for this glyph: the flat disc these sprites replaced, kept for the odd caller
     * with a one-off character. Baseline sits low in the disc because the font's ascent is most
     * of its box; the caret is the one glyph whose ink sits at the TOP of the cap box and needs
     * pushing back down. */
    if (shape == UI_BTN_TALL) {
        ui_fill(x + UI_BTN_D / 2 - 7, y, 14, UI_BTN_D, colour);
    } else {
        ui_fill(x + 2, y + 2, UI_BTN_D - 4, UI_BTN_D - 4, colour);
        ui_fill(x + 5, y, UI_BTN_D - 10, UI_BTN_D, colour);
        ui_fill(x, y + 5, UI_BTN_D, UI_BTN_D - 10, colour);
    }
    int baseline = y + UI_BTN_D - 5 + ((glyph != NULL && glyph[0] == '^') ? 4 : 0);
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text(x, baseline, UI_BTN_D, ALIGN_CENTER, STL_ONBTN, glyph);
}

int ui_hint (int x, int y, const char *glyph, uint16_t colour, ui_btn_shape_t shape,
             const char *label) {
    int w = btn_sprite_w(btn_style_for(glyph));

    /* Round sprites keep the 20 px cell's metrics: their 3 px overhang eats gap, not label.
     * Shoulders really are wider, so their label and the next hint both move right with them. */
    if (w < UI_BTN_D) {
        w = UI_BTN_D;
    }
    ui_button(x, y, glyph, colour, shape);
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text(x + w + 6, y + UI_BTN_D - 5, 200, ALIGN_LEFT, STL_GRAY, label);
    /* 12 px a glyph, matching the metric the detail sheet uses for its row wrapping. Both
     * follow the body font: at size 20 Firple-Bold is near enough monospace for layout. */
    return x + w + 6 + (int)strlen(label) * 12 + 20;
}
