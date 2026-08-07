#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "library/boxart.h"
#include "library/library.h"
#include "menu/ini_parser.h"
#include "menu/paths.h"
#include "ui/theme.h"

#define BOXART_FILE "boxart.ini"

/** @brief The built-in table, in millimetres, from the measurements in boxart.h. */
typedef struct {
    uint16_t mm_w, mm_h;
} mm_t;

/* Indexed by system_t. The two duplicated pairs are deliberate: NES/SNES/N64/SMS are within 2 %
 * of each other and could be one entry, but a table with a row per system is a table somebody can
 * correct one row of. */
static const mm_t BUILTIN[SYS_COUNT] = {
    [SYS_N64]  = { 127, 181 },
    [SYS_NES]  = { 127, 180 },
    [SYS_SNES] = { 127, 181 },
    [SYS_GB]   = { 126, 126 },
    [SYS_GBC]  = { 126, 126 },
    [SYS_SMS]  = { 128, 179 },
};

/**
 * @brief The three shapes a tile may be, as aspects. See boxart.h for why three.
 *
 * Heights are derived rather than written, so a change to TILE_W cannot leave one of them at a
 * stale pixel count: at 109 they come out 155, 109 and 76.
 */
static const float SHAPE_ASPECT[ART_SHAPES] = {
    [ART_PORTRAIT]  = 127.0f / 181.0f,      /* 0.7017 */
    [ART_SQUARE]    = 1.0f,
    [ART_LANDSCAPE] = 1.4286f,
};

static char        names[BOXART_REGIONS_MAX][BOXART_NAME_CAP];
static int         name_count;
static int         current;
static art_shape_t shapes[SYS_COUNT];
static int         tallest;

/** The parsed file, kept open for the life of the program because switching region in Settings
 *  has to re-read it and re-reading is a card access on a screen that is otherwise silent. It is
 *  a few hundred bytes. */
static ini_t *file;

/** @brief Lower-case system name, which is what a key in the file is. */
static void system_key (int sys, char *out, size_t cap) {
    const char *s = library_tab_label((tab_t)(TAB_N64 + sys));
    size_t i = 0;
    for (; s[i] != '\0' && i + 1 < cap; i++) {
        out[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
    }
    out[i] = '\0';
}

/**
 * @brief Turn a "WxH" value into a tile height, or return 0 if it is not one.
 *
 * The width is the grid column and cannot move, so the pair is read purely as a ratio. Rounding
 * is to nearest rather than truncating: at 109 px a truncation costs up to a whole pixel of
 * height, which across a 155 px tile is a visible half-percent squash on one system and not the
 * next one along.
 */
static int height_from (const char *spec) {
    int w = 0, h = 0;
    if (spec == NULL || sscanf(spec, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
        return 0;
    }
    int px = (TILE_W * h + w / 2) / w;
    if (px < TILE_H_MIN || px > TILE_H_MAX) {
        /* Loud rather than silently clamped. A number outside this range is a typo or a unit
         * mix-up, and a tile quietly pinned to the ceiling looks like the file was ignored. */
        debugf("BOXART %s is %d px tall, outside %d..%d -- ignored\n",
               spec, px, TILE_H_MIN, TILE_H_MAX);
        return 0;
    }
    return px;
}

/* The first two entries are not sections.
 *
 *   0  Automatic  shapes come from each cover's own aspect; the built-in table is only the
 *                 fallback for a record with no art, or none probed yet.
 *   1  NTSC       force the built-in table for everything, which is what the menu did before
 *                 snapping existed.
 *
 * Sections from menu/boxart.ini follow, and each of those forces its own table. Automatic is
 * first because it is the answer for almost every card: a PAL box photographed as a PAL box
 * snaps to portrait without anybody being asked. */
#define REGION_AUTO   0
#define REGION_NTSC   1
#define REGION_FIXED  2         /**< first index that names a section */

/** @brief Recompute the fallback table from the built-in one, then let a chosen section override. */
static void resolve (void) {
    const char *section = (current >= REGION_FIXED) ? names[current] : NULL;

    tallest = 0;
    for (int sys = 0; sys < SYS_COUNT; sys++) {
        const mm_t *mm = &BUILTIN[sys];
        char spec[16];
        snprintf(spec, sizeof(spec), "%ux%u", mm->mm_w, mm->mm_h);
        int px = height_from(spec);

        if (section != NULL && file != NULL) {
            char key[16];
            system_key(sys, key, sizeof(key));
            int over = height_from(ini_get_string(file, section, key, NULL));
            if (over > 0) {
                px = over;
            }
        }

        shapes[sys].w = TILE_W;
        shapes[sys].h = (uint16_t)px;
        if (px > tallest) {
            tallest = px;
        }
    }
    debugf("BOXART %s, fallback table '%s': tallest %d px\n",
           boxart_automatic() ? "automatic" : "forced",
           boxart_region_name(current), tallest);
}

void boxart_init (const char *storage_prefix, const char *want) {
    snprintf(names[REGION_AUTO], BOXART_NAME_CAP, "Automatic");
    snprintf(names[REGION_NTSC], BOXART_NAME_CAP, "NTSC");
    name_count = REGION_FIXED;
    current = REGION_AUTO;

    if (storage_prefix != NULL) {
        char path[300];
        menu_path(path, sizeof(path), storage_prefix, BOXART_FILE);
        file = ini_try_load(path);
    }

    if (file != NULL) {
        int n = ini_section_count(file);
        for (int i = 0; i < n && name_count < BOXART_REGIONS_MAX; i++) {
            const char *s = ini_section_name(file, i);
            if (s == NULL || s[0] == '\0' || strlen(s) >= BOXART_NAME_CAP) {
                continue;               /* a name the picker could not print is not offered */
            }
            snprintf(names[name_count], BOXART_NAME_CAP, "%s", s);
            name_count++;
        }
    }

    if (want != NULL) {
        for (int i = 0; i < name_count; i++) {
            if (strcmp(names[i], want) == 0) {
                current = i;
                break;
            }
        }
    }
    resolve();
}

int boxart_region_count (void) {
    return name_count;
}

const char *boxart_region_name (int i) {
    return (i >= 0 && i < name_count) ? names[i] : "";
}

int boxart_region_current (void) {
    return current;
}

void boxart_set_region (int i) {
    if (i < 0 || i >= name_count || i == current) {
        return;
    }
    current = i;
    resolve();
}

art_shape_t boxart_shape_at (int kind) {
    if (kind < 0 || kind >= ART_SHAPES) {
        kind = ART_PORTRAIT;
    }
    int h = (int)((float)TILE_W / SHAPE_ASPECT[kind] + 0.5f);
    if (h < TILE_H_MIN) {
        h = TILE_H_MIN;
    } else if (h > TILE_H_MAX) {
        h = TILE_H_MAX;
    }
    return (art_shape_t){ .w = TILE_W, .h = (uint16_t)h };
}

uint8_t boxart_snap (int src_w, int src_h) {
    if (src_w <= 0 || src_h <= 0) {
        return ART_KIND_UNKNOWN;
    }

    /* Compared in log space. The three are a geometric run -- 0.70, 1.00, 1.43, each about 1.43x
     * the last -- so a linear nearest puts the portrait/square boundary at 0.851, which is 21 %
     * away from portrait and 15 % from square. A cover exactly between the two would be called
     * square, and it would be cropped more than the call that lost. Ratios are what a crop costs,
     * so ratios are what the comparison uses.
     *
     * No logf: the ratio of a candidate to the source is already the quantity, and comparing
     * max(a/b, b/a) across candidates orders them identically to |log(a/b)| without the libm
     * call. This runs once per record, but it also runs on the boot path. */
    float a = (float)src_w / (float)src_h;
    int best = ART_PORTRAIT;
    float best_d = 1e30f;
    for (int i = 0; i < ART_SHAPES; i++) {
        float r = a / SHAPE_ASPECT[i];
        if (r < 1.0f) {
            r = 1.0f / r;
        }
        if (r < best_d) {
            best_d = r;
            best = i;
        }
    }
    return (uint8_t)best;
}

bool boxart_automatic (void) {
    return current == 0;
}

art_shape_t boxart_shape (uint8_t system) {
    /* An unknown system takes N64's shape rather than a square or a default-initialised zero.
     * Zero would divide by nothing in the scaler and a square would make one odd ROM the tallest
     * thing in its tab; the cartridge shape is what an unrecognised file on this card is. */
    int sys = (system < SYS_COUNT) ? system : SYS_N64;
    return shapes[sys];
}

int boxart_tallest (void) {
    return tallest;
}
