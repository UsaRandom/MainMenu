#include <libdragon.h>

#include "fonts.h"
#include "utils/fs.h"
#include "menu/memprofile.h"

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
    /* The body face is the largest allocation this program makes: measured at 1,284,208 bytes of
     * RDRAM, which is more than the two framebuffers a 4 MB console has to fit beside it in a
     * 2,221,744-byte heap. It does not fit there with everything else removed, so a console with
     * no Expansion Pak reads the same typeface at the same size with the CJK dropped -- 339
     * characters instead of 2,697, and 37,492 bytes on the card instead of 681,040.
     *
     * The cost is real and it is stated rather than hidden: a title with Japanese in it draws its
     * Latin part and holes where the rest was. That is why this is a fallback for a console that
     * cannot start otherwise, and never a default -- with a pak, the path below is byte for byte
     * what it always was. */
    char *font_path = mem_small() ? "rom:/FirpleBody4M.font64" : "rom:/Firple-Bold.font64";

    /* A custom font still wins on either profile. On the small one that is a loaded gun -- a
     * full-charset face put there by hand is an assert in asset_load and a console that does not
     * boot -- but refusing a file the owner deliberately installed is worse, and nothing in this
     * build ever passes one: app.c calls fonts_init(NULL). */
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
    mem_report("f:body");
    load_boot_font();
    mem_report("f:boot");
    small_font = load_ui_font("rom:/FirpleSmall.font64", FNT_SMALL);
    mem_report("f:small");
    key_font   = load_ui_font("rom:/FirpleKey.font64",   FNT_KEY);
    mem_report("f:key");
    field_font = load_ui_font("rom:/FirpleField.font64", FNT_FIELD);
    mem_report("f:field");
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
