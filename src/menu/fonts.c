#include <libdragon.h>

#include "fonts.h"
#include "utils/fs.h"

/* Kept so the styles can be re-registered when the theme changes. rdpq_text_register_font takes
 * ownership for drawing but there is no way to ask it for the pointer back, and rdpq_font_style
 * needs the font, not the registered id. */
static rdpq_font_t *body_font = NULL;
static rdpq_font_t *boot_font = NULL;

/* The styles that do NOT follow the theme, and why each one does not:
 *   GREEN/BLUE/RED  semantic, and only RED is used -- the fault screen's heading, over that
 *                   screen's own full-bleed dark plate rather than over a theme surface.
 *   ONBTN           the letter inside a controller-colour disc. Button colours are deliberately
 *                   not themed (see theme.h), so the glyph on top must not be either. */
static void register_fixed_styles (rdpq_font_t *f) {
    rdpq_font_style(f, STL_GREEN, &((rdpq_fontstyle_t) { .color = RGBA32(0x70, 0xFF, 0x70, 0xFF) }));
    rdpq_font_style(f, STL_BLUE,  &((rdpq_fontstyle_t) { .color = RGBA32(0x70, 0xBC, 0xFF, 0xFF) }));
    rdpq_font_style(f, STL_RED,   &((rdpq_fontstyle_t) { .color = RGBA32(0xFF, 0x40, 0x40, 0xFF) }));
    rdpq_font_style(f, STL_ONLIGHT, &((rdpq_fontstyle_t) { .color = RGBA32(0x10, 0x10, 0x19, 0xFF) }));
    rdpq_font_style(f, STL_ONBTN, &((rdpq_fontstyle_t) { .color = RGBA32(0xFF, 0xFF, 0xFF, 0xFF) }));
}


static rdpq_font_t *small_font;
static rdpq_font_t *key_font;
static rdpq_font_t *field_font;

static void load_default_font (char *custom_font_path) {
    char *font_path = "rom:/Firple-Bold.font64";

    if (custom_font_path && file_exists(custom_font_path)) {
        font_path = custom_font_path;
    }

    body_font = rdpq_font_load(font_path);
    register_fixed_styles(body_font);
    rdpq_text_register_font(FNT_DEFAULT, body_font);
}


/* Separate page rather than a scaled default: rdpq scales glyph quads, and 15 px stretched to 32
 * is visibly soft next to everything else on a screen whose whole job is to look deliberate. */
static void load_boot_font (void) {
    const char *path = "rom:/FirpleBoot.font64";
    if (!file_exists((char *)path)) {
        return;                 /* boot plate falls back to the body font; not fatal */
    }
    boot_font = rdpq_font_load(path);
    register_fixed_styles(boot_font);
    rdpq_text_register_font(FNT_BOOT, boot_font);
}

/** One of the restricted-charset faces. Optional, like the boot font: a build whose filesystem
 *  lost one falls back to the body font at the wrong size rather than failing to start. */
static rdpq_font_t *load_ui_font (const char *path, menu_font_type_t id) {
    if (!file_exists((char *)path)) {
        return NULL;
    }
    rdpq_font_t *f = rdpq_font_load(path);
    register_fixed_styles(f);
    rdpq_text_register_font(id, f);
    return f;
}

void fonts_init (char *custom_font_path) {
    load_default_font(custom_font_path);
    load_boot_font();
    small_font = load_ui_font("rom:/FirpleSmall.font64", FNT_SMALL);
    key_font   = load_ui_font("rom:/FirpleKey.font64",   FNT_KEY);
    field_font = load_ui_font("rom:/FirpleField.font64", FNT_FIELD);
    /* White until a theme says otherwise, so a font drawn before theme_apply() is still legible
     * on the dark plate the boot screen paints. */
    fonts_set_palette(0xFFFF, 0xA5A5, 0xFFFF);
}

void fonts_set_palette (uint16_t text, uint16_t dim, uint16_t accent) {
    color_t t = color_from_packed16(text);
    color_t d = color_from_packed16(dim);
    color_t a = color_from_packed16(accent);

    rdpq_font_t *fonts[] = { body_font, boot_font, small_font, key_font, field_font };
    for (unsigned i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        if (fonts[i] == NULL) {
            continue;           /* the boot font is optional */
        }
        rdpq_font_style(fonts[i], STL_DEFAULT, &((rdpq_fontstyle_t) { .color = t }));
        rdpq_font_style(fonts[i], STL_GRAY,    &((rdpq_fontstyle_t) { .color = d }));
        /* Both accents map to the same token. They are used for the position counter and for
         * the "not in the database" note, which are the same role -- something the eye should
         * find without it shouting. Keeping two names is harmless; keeping two colours meant
         * one of them was always wrong under some theme. */
        rdpq_font_style(fonts[i], STL_YELLOW,  &((rdpq_fontstyle_t) { .color = a }));
        rdpq_font_style(fonts[i], STL_ORANGE,  &((rdpq_fontstyle_t) { .color = a }));
    }
}
