/**
 * @file theme.c
 * @brief Built-in themes.
 * @ingroup ui
 *
 * Values transcribed from docs/design/README.md section 2. The handoff also lists the packed
 * RGBA5551 word for each colour; RGBA5551() recomputes it from the hex rather than pasting the
 * word, so a transcription slip shows up as a wrong colour on screen instead of silently
 * agreeing with a mistyped constant.
 *
 * Two rules the palettes encode, worth not breaking when adding a theme:
 *   1. Surfaces differ by at least two ladder steps in every channel. That is what stops a
 *      #2A2A2A / #2C2C2C collapse once the framebuffer quantises.
 *   2. No badge is distinguished by colour alone. Phosphor exists in the set to enforce this --
 *      under it, favourite and save are near-identical greens, so the marks have to be told
 *      apart by silhouette and corner. Colour is confirmation, never the message.
 */

#include <string.h>

#include "menu/fonts.h"
#include "theme.h"

void theme_apply (const theme_t *th) {
    if (th == NULL) {
        return;
    }
    /* The font styles are the half of a theme that is not drawn by this module. They were
     * registered once at load in fixed colours, so every label stayed white however dark or
     * light the surfaces under it became -- which made `cartridge`, the one light theme,
     * unreadable from the moment it was added. Anything that changes app->theme must come
     * through here. */
    fonts_set_palette(th->text, th->text_dim, th->text_accent);
}

const theme_t THEME_MIDNIGHT = {
    .name          = "midnight",
    .bg            = RGBA5551(0x10, 0x10, 0x19),
    .bg_alt        = RGBA5551(0x19, 0x19, 0x29),
    .panel         = RGBA5551(0x21, 0x21, 0x3A),
    .panel_alt     = RGBA5551(0x31, 0x31, 0x4A),
    .text          = RGBA5551(0xF7, 0xF7, 0xFF),
    .text_dim      = RGBA5551(0x9C, 0x9C, 0xAD),
    .text_accent   = RGBA5551(0xF7, 0xB5, 0x21),
    .tab_active    = RGBA5551(0xF7, 0xF7, 0xFF),
    .tab_inactive  = RGBA5551(0x73, 0x73, 0x84),
    .tab_underline = RGBA5551(0xF7, 0xB5, 0x21),
    .sel_outline   = RGBA5551(0xFF, 0xFF, 0xFF),
    .sel_shadow    = RGBA5551(0x00, 0x00, 0x00), .sel_shadow_a = 143,  /* 56% */
    .badge_fav     = RGBA5551(0xF7, 0xB5, 0x21),
    .badge_save    = RGBA5551(0x42, 0xBD, 0x63),
    .overlay       = RGBA5551(0x08, 0x08, 0x10), .overlay_a = 199,     /* 78% */
    .tile_dim_a    = 87,                                               /* 34% */
};

const theme_t THEME_CARTRIDGE = {
    .name          = "cartridge",
    .bg            = RGBA5551(0xBD, 0xBD, 0xB5),
    .bg_alt        = RGBA5551(0xAD, 0xAD, 0xA5),
    .panel         = RGBA5551(0xE6, 0xE6, 0xDE),
    .panel_alt     = RGBA5551(0xF7, 0xF7, 0xEF),
    .text          = RGBA5551(0x19, 0x19, 0x19),
    .text_dim      = RGBA5551(0x5A, 0x5A, 0x5A),
    .text_accent   = RGBA5551(0xC5, 0x10, 0x29),
    .tab_active    = RGBA5551(0x19, 0x19, 0x19),
    .tab_inactive  = RGBA5551(0x73, 0x73, 0x73),
    .tab_underline = RGBA5551(0xC5, 0x10, 0x29),
    .sel_outline   = RGBA5551(0x19, 0x19, 0x19),
    .sel_shadow    = RGBA5551(0x00, 0x00, 0x00), .sel_shadow_a = 102,  /* 40% */
    .badge_fav     = RGBA5551(0xE6, 0x8C, 0x10),
    .badge_save    = RGBA5551(0x29, 0x84, 0x4A),
    .overlay       = RGBA5551(0xF7, 0xF7, 0xEF), .overlay_a = 204,     /* 80% */
    .tile_dim_a    = 87,
};

const theme_t THEME_PHOSPHOR = {
    .name          = "phosphor",
    .bg            = RGBA5551(0x00, 0x10, 0x08),
    .bg_alt        = RGBA5551(0x00, 0x21, 0x10),
    .panel         = RGBA5551(0x00, 0x31, 0x19),
    .panel_alt     = RGBA5551(0x00, 0x4A, 0x29),
    .text          = RGBA5551(0x8C, 0xF7, 0xA5),
    .text_dim      = RGBA5551(0x42, 0xA5, 0x5A),
    .text_accent   = RGBA5551(0xE6, 0xF7, 0x8C),
    .tab_active    = RGBA5551(0x8C, 0xF7, 0xA5),
    .tab_inactive  = RGBA5551(0x31, 0x94, 0x52),
    .tab_underline = RGBA5551(0xE6, 0xF7, 0x8C),
    .sel_outline   = RGBA5551(0xDE, 0xFF, 0xE6),
    .sel_shadow    = RGBA5551(0x00, 0x00, 0x00), .sel_shadow_a = 153,  /* 60% */
    .badge_fav     = RGBA5551(0xE6, 0xF7, 0x8C),
    .badge_save    = RGBA5551(0x52, 0xDE, 0x7B),
    .overlay       = RGBA5551(0x00, 0x10, 0x08), .overlay_a = 209,     /* 82% */
    .tile_dim_a    = 102,                                              /* 40% */
};

/* Violet surfaces with a pink accent. Every channel value below is a rung of the 5-bit ladder --
 * round(i * 255/31) -- so the hex here and the packed word agree exactly, as in the three above.
 * The accent is pink rather than the amber midnight uses, because amber on violet is the same
 * warm-on-cool pairing midnight already has and the two themes would read as one recoloured. */
const theme_t THEME_PURPLE = {
    .name          = "purple",
    .bg            = RGBA5551(0x19, 0x08, 0x29),
    .bg_alt        = RGBA5551(0x29, 0x19, 0x42),
    .panel         = RGBA5551(0x3A, 0x21, 0x5A),
    .panel_alt     = RGBA5551(0x52, 0x31, 0x7B),
    .text          = RGBA5551(0xF7, 0xEF, 0xFF),
    .text_dim      = RGBA5551(0xA5, 0x8C, 0xBD),
    .text_accent   = RGBA5551(0xFF, 0x7B, 0xD6),
    .tab_active    = RGBA5551(0xF7, 0xEF, 0xFF),
    .tab_inactive  = RGBA5551(0x73, 0x5A, 0x8C),
    .tab_underline = RGBA5551(0xFF, 0x7B, 0xD6),
    .sel_outline   = RGBA5551(0xFF, 0xFF, 0xFF),
    .sel_shadow    = RGBA5551(0x00, 0x00, 0x00), .sel_shadow_a = 143,  /* 56% */
    .badge_fav     = RGBA5551(0xFF, 0x7B, 0xD6),
    .badge_save    = RGBA5551(0x42, 0xD6, 0x7B),
    .overlay       = RGBA5551(0x10, 0x00, 0x21), .overlay_a = 199,     /* 78% */
    .tile_dim_a    = 87,                                               /* 34% */
};

/* Deep maroon with a gold accent. The accent cannot be red: on these surfaces a red underline
 * and a red favourite badge would be the one thing on screen the eye cannot separate from the
 * panel behind them, which is the failure rule 2 at the top of this file is about. */
const theme_t THEME_RED = {
    .name          = "red",
    .bg            = RGBA5551(0x19, 0x08, 0x08),
    .bg_alt        = RGBA5551(0x31, 0x10, 0x10),
    .panel         = RGBA5551(0x4A, 0x19, 0x19),
    .panel_alt     = RGBA5551(0x6B, 0x29, 0x29),
    .text          = RGBA5551(0xFF, 0xEF, 0xEF),
    .text_dim      = RGBA5551(0xBD, 0x8C, 0x8C),
    .text_accent   = RGBA5551(0xFF, 0xD6, 0x52),
    .tab_active    = RGBA5551(0xFF, 0xEF, 0xEF),
    .tab_inactive  = RGBA5551(0x8C, 0x5A, 0x5A),
    .tab_underline = RGBA5551(0xFF, 0xD6, 0x52),
    .sel_outline   = RGBA5551(0xFF, 0xFF, 0xFF),
    .sel_shadow    = RGBA5551(0x00, 0x00, 0x00), .sel_shadow_a = 143,
    .badge_fav     = RGBA5551(0xFF, 0xD6, 0x52),
    .badge_save    = RGBA5551(0x42, 0xD6, 0x6B),
    .overlay       = RGBA5551(0x10, 0x00, 0x00), .overlay_a = 199,
    .tile_dim_a    = 87,
};

static const theme_t *const THEMES[] = {
    &THEME_MIDNIGHT,
    &THEME_CARTRIDGE,
    &THEME_PHOSPHOR,
    &THEME_PURPLE,
    &THEME_RED,
};

#define THEME_N ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

int theme_count (void) {
    return THEME_N;
}

const theme_t *theme_at (int index) {
    if (index < 0 || index >= THEME_N) {
        return &THEME_MIDNIGHT;
    }
    return THEMES[index];
}

const theme_t *theme_by_name (const char *name) {
    if (name != NULL) {
        for (int i = 0; i < THEME_N; i++) {
            if (strcasecmp(THEMES[i]->name, name) == 0) {
                return THEMES[i];
            }
        }
    }
    return &THEME_MIDNIGHT;
}
