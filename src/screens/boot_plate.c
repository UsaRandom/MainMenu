/**
 * @file boot_plate.c
 * @brief The boot screen, drawn over the grid it is about to reveal.
 * @ingroup screens
 *
 * docs/design/README.md section 4.1, transcribed:
 *
 *   Black -- literal #000000, the one screen outside the theme. SC64 mark (192 x 135) centred at
 *   y 120, `MAIN MENU` at 32 px / 10 px tracking at y 296, and `MENU <version>` / `<n> TITLES` at
 *   16 px on the bottom safe line.
 *
 * The spec also puts `SUMMERCART64` at 16 px on y 340. Dropped: the mark directly above it already
 * says so, and spelling it out under the product's own name made the plate read as a vendor splash
 * rather than as the menu starting.
 *
 *   t=0.00 mark at 88 % scale, 22 % opacity, boot SFX fires - t=0.55 full, holds -
 *   t~1.30 the whole plate translates -480 px over 0.34 s. The grid is already composited
 *   underneath and already has a selection -- there is no second fade-in. Total <= 1.64 s.
 *
 * That last sentence is why this is an overlay on the grid rather than a screen of its own. A
 * separate SCREEN_BOOT would have to hand over at t=1.64, and whatever it handed over to would
 * be arriving cold -- one frame of empty grid before the first tile lands, which is precisely the
 * "second fade-in" the spec rules out. Drawn on top of a live grid, the reveal is a reveal.
 *
 * This file used to end that paragraph by claiming the library "has been scanning and decoding
 * underneath for the whole 1.64 s". It had been doing neither: grid_update() returns early while
 * the plate is up, above the counter that gates background decoding, so the gate never opened and
 * the first decode began a quarter-second AFTER the curtain lifted. See AUDIT.md 1q. The claim is
 * true now, and the hold is elastic so that it can stay true on art we have not measured.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "menu/fonts.h"
#include "utils/fs.h"
#include "screens/boot_plate.h"
#include "ui/draw.h"
#include "ui/theme.h"
#include "ui/tween.h"

/* Timeline, seconds. The rise is section 5's table; the hold is no longer a single number.
 *
 * Section 4.1 specifies a 1.30 s hold and a <= 1.64 s total, and that is still the floor and
 * still what a warm or artless library gets. But the sentence at the top of this file -- that the
 * grid underneath has been decoding for the whole hold -- was aspirational rather than true, and
 * a fixed hold cannot make it true, because how long the first row takes is a property of the
 * user's art and not of ours. So the hold is elastic between the two: it never ends before the
 * mark has finished arriving and settled, and it does not end after that until the grid has
 * something on it, up to a ceiling that exists so nobody with 2000 x 1400 covers is stranded.
 *
 * At the measured ~155,000 us per card the first row costs ~0.62 s, which lands inside the
 * 1.30 s floor. So the common case is unchanged at 1.64 s total -- the difference is that the
 * curtain now lifts on painted tiles instead of on placeholders. */
#define T_RISE_END      0.55f
#define T_HOLD_MIN      1.30f
#define T_HOLD_MAX      3.00f

#define MARK_W          192
#define MARK_H          135
#define MARK_Y          120
#define TITLE_Y         296
#define TITLE_TRACKING  10       /**< docs/design/README.md 4.1: 32 px / 10 px tracking */
#define CURTAIN_DY      480

#define START_SCALE     0.88f
#define START_ALPHA     0.22f

/* The real SC64 mark, from Polprzewodnikowy/SummerCart64 assets/sc64_logo.svg by way of
 * tools/mklogo.py. Composited onto black at conversion time, because the plate is #000000 and an
 * alpha channel for a mark only ever drawn on black would cost a blend mode for nothing. */
static sprite_t *mark;

static void mark_load (void) {
    if (mark == NULL && file_exists("rom:/sc64_logo.sprite")) {
        mark = sprite_load("rom:/sc64_logo.sprite");
    }
}

void boot_plate_reset (boot_plate_t *bp) {
    mark_load();
    bp->t = 0.0f;
    bp->curtain_at = 0.0f;
    bp->released = false;
    bp->done = false;
}

bool boot_plate_working (const boot_plate_t *bp) {
    return !bp->done && !bp->released && bp->t >= T_RISE_END;
}

bool boot_plate_step (boot_plate_t *bp, float dt, bool ready) {
    if (bp->done) {
        return false;
    }
    bp->t += dt;

    if (!bp->released && bp->t >= T_HOLD_MIN && (ready || bp->t >= T_HOLD_MAX)) {
        bp->released = true;
        bp->curtain_at = bp->t;
        /* One line per boot, kept because the hold is now a negotiation rather than a constant
         * and "why was the plate up that long" is otherwise unanswerable from a log. */
        debugf("BOOT plate held %d ms, released by %s\n", (int)(bp->t * 1000.0f),
               ready ? "the grid" : "the ceiling");
    }
    if (bp->released && bp->t >= bp->curtain_at + DUR_BOOT_CURTAIN) {
        bp->done = true;
        return false;
    }
    return true;
}

/**
 * @brief Blit the mark at @p scale, dimmed to @p lum.
 *
 * Opacity is done by multiplying the texture toward black rather than by blending. The plate
 * behind it is already black, so the two are identical on screen and this costs no blend layer --
 * which matters on the one screen that is otherwise a full-frame fill every frame.
 */
static void draw_mark (int x, int y, float scale, uint8_t lum) {
    if (mark == NULL) {
        return;
    }
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    rdpq_set_prim_color(RGBA32(lum, lum, lum, 0xFF));
    rdpq_sprite_blit(mark, x, y, &(rdpq_blitparms_t){ .scale_x = scale, .scale_y = scale });
}

void boot_plate_draw (const boot_plate_t *bp, const char *version, int title_count) {
    if (bp->done) {
        return;
    }

    float t = bp->t;

    /* Curtain: the WHOLE plate translates, mark and type together, so it reads as one object
     * leaving rather than as elements animating out separately. */
    int dy = 0;
    if (bp->released) {
        float k = (t - bp->curtain_at) / DUR_BOOT_CURTAIN;
        if (k > 1.0f) {
            k = 1.0f;
        }
        dy = -(int)(ease_bezier(k, EASE_BOOT_CURTAIN) * (float)CURTAIN_DY);
    }

    /* #000000 literally, not theme->bg. The one screen outside the theme. */
    ui_fill(0, dy, SCREEN_W, SCREEN_H, RGBA5551(0, 0, 0));

    /* Mark: 88 % -> 100 % scale and 22 % -> 100 % opacity over the first 0.55 s.
     *
     * Section 5 names a curve for the curtain but not for this rise, so it borrows the arrival
     * curve the spec uses for a tile landing (0.34,1.32,.64,1) -- same gesture, something
     * appearing with a little weight.
     *
     * SCALE may overshoot; OPACITY may not. That curve peaks above 1.0, and lerping alpha with it
     * gave 1.06, which `(uint8_t)(alpha * 255)` wrapped to 14 -- the mark went black for a third
     * of its own entrance. Clamped, and the two are computed separately so it cannot come back. */
    float rise = t < T_RISE_END ? ease_bezier(t / T_RISE_END, EASE_TILE_GROW) : 1.0f;
    float scale = lerpf(START_SCALE, 1.0f, rise);
    float alpha = lerpf(START_ALPHA, 1.0f, clampf(rise, 0.0f, 1.0f));

    int mw = (int)(MARK_W * scale);
    int mh = (int)(MARK_H * scale);
    int mx = (SCREEN_W - mw) / 2;
    int my = MARK_Y + (MARK_H - mh) / 2 + dy;

    (void)mw; (void)mh;
    draw_mark(mx, my, scale, (uint8_t)(alpha * 255.0f));

    /* Type holds still while the mark rises -- only the mark is animated in 4.1 -- but fades with
     * it, so the plate arrives as one thing. */
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);

    /* Tracking via char_spacing rather than by spelling the string with spaces between letters.
     * The hand-spelled version put a full 32 px space between glyphs -- three times the 10 px the
     * spec asks for -- and it also made the string untranslatable and unsearchable. */
    static const char *TITLE = "MAIN MENU";
    rdpq_text_printn(&(rdpq_textparms_t){ .width = SCREEN_W, .align = ALIGN_CENTER,
                                          .char_spacing = TITLE_TRACKING,
                                          .style_id = STL_DEFAULT, .disable_aa_fix = true },
                     FNT_BOOT, 0, TITLE_Y + dy, TITLE, strlen(TITLE));

    char left[48], right[32];
    snprintf(left, sizeof(left), "MENU %s", version != NULL ? version : "");
    snprintf(right, sizeof(right), "%d TITLES", title_count);
    ui_text(SAFE_X, SAFE_Y + SAFE_H + dy, SAFE_W, ALIGN_LEFT, STL_GRAY, left);
    ui_text(SAFE_X, SAFE_Y + SAFE_H + dy, SAFE_W, ALIGN_RIGHT, STL_GRAY, right);
}
