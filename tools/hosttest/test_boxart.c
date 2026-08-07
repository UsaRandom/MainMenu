/**
 * @file test_boxart.c
 * @brief The two pure decisions behind a tile's shape: reading a size, and snapping it.
 *
 * Both are silent when wrong, which is why they are here rather than trusted.
 *
 * `image_probe_size` walks a JPEG's marker chain, and the failure mode of getting that wrong is
 * not a crash -- it is returning an error, which makes the record fall back to its system's box
 * shape. Every JPEG cover on a card would then be shaped by a table instead of by itself, and the
 * grid would look completely reasonable. The one measurement that would show it is a crop
 * percentage nobody computes at runtime.
 *
 * `boxart_snap` is arithmetic with a boundary, and a boundary in the wrong place moves a whole
 * class of covers into the wrong bucket. The interesting cases are the two boundaries themselves,
 * which are in log space and are not where a linear midpoint would put them.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/boxart.h"
#include "menu/image_decoder.h"
#include "ui/theme.h"

static int checks, failures;

static void check (bool ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static const char *testdir (void) {
    const char *d = getenv("TESTDIR");
    return d ? d : "build/hosttest/boxartdir";
}

static void path_for (char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s", testdir(), name);
}

/* ------------------------------------------------------------------ file builders */

static void put (const char *name, const void *bytes, size_t n) {
    char path[512];
    path_for(path, sizeof(path), name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("  FAIL  cannot write %s\n", path);
        failures++;
        return;
    }
    fwrite(bytes, 1, n, f);
    fclose(f);
}

/** @brief A PNG that is nothing but a signature and an IHDR. The probe reads no further. */
static void put_png (const char *name, uint32_t w, uint32_t h) {
    uint8_t b[33] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
        0, 0, 0, 13, 'I', 'H', 'D', 'R',
    };
    b[16] = (uint8_t)(w >> 24); b[17] = (uint8_t)(w >> 16);
    b[18] = (uint8_t)(w >> 8);  b[19] = (uint8_t)w;
    b[20] = (uint8_t)(h >> 24); b[21] = (uint8_t)(h >> 16);
    b[22] = (uint8_t)(h >> 8);  b[23] = (uint8_t)h;
    put(name, b, sizeof(b));
}

/**
 * @brief A JPEG marker chain with @p lead segments of junk before the SOF.
 *
 * The junk matters. A probe that assumed the SOF came first would pass on a file written by one
 * encoder and fail on a file written by another, and real covers carry JFIF, EXIF, comment and
 * quantisation segments ahead of it -- often several kilobytes of them.
 */
static void put_jpeg (const char *name, uint16_t w, uint16_t h, int lead,
                      bool progressive, bool restarts)
{
    uint8_t b[4096];
    size_t n = 0;
    b[n++] = 0xFF; b[n++] = 0xD8;                   /* SOI */

    if (restarts) {
        /* Standalone markers, which carry no length. Reading two bytes after one of these as a
         * segment length walks the parser into the middle of the file. */
        b[n++] = 0xFF; b[n++] = 0xD0;
        b[n++] = 0xFF; b[n++] = 0x01;               /* TEM */
    }
    for (int i = 0; i < lead; i++) {
        int payload = 16 + i * 7;
        b[n++] = 0xFF; b[n++] = (uint8_t)(0xE0 + (i & 0x0F));
        b[n++] = (uint8_t)((payload + 2) >> 8); b[n++] = (uint8_t)(payload + 2);
        for (int j = 0; j < payload; j++) {
            b[n++] = (uint8_t)(j * 13 + i);
        }
    }
    /* SOF0 baseline or SOF2 progressive. Both carry the dimensions in the same place. */
    b[n++] = 0xFF; b[n++] = progressive ? 0xC2 : 0xC0;
    b[n++] = 0; b[n++] = 11;                        /* length */
    b[n++] = 8;                                     /* precision */
    b[n++] = (uint8_t)(h >> 8); b[n++] = (uint8_t)h;
    b[n++] = (uint8_t)(w >> 8); b[n++] = (uint8_t)w;
    b[n++] = 1; b[n++] = 1; b[n++] = 0x11; b[n++] = 0;
    b[n++] = 0xFF; b[n++] = 0xDA;                   /* SOS: the probe must already be done */
    put(name, b, n);
}

/* ------------------------------------------------------------------ tests */

static void test_probe (void) {
    char path[512];
    int w = -1, h = -1;

    printf("\nreading a size without decoding\n");

    put_png("p.png", 218, 310);
    path_for(path, sizeof(path), "p.png");
    check(image_probe_size(path, &w, &h) == IMG_OK && w == 218 && h == 310,
          "a PNG's IHDR gives its dimensions");

    put_png("big.png", 2118, 1457);
    path_for(path, sizeof(path), "big.png");
    check(image_probe_size(path, &w, &h) == IMG_OK && w == 2118 && h == 1457,
          "and a four-digit one is not truncated");

    put_jpeg("simple.jpg", 640, 448, 0, false, false);
    path_for(path, sizeof(path), "simple.jpg");
    check(image_probe_size(path, &w, &h) == IMG_OK && w == 640 && h == 448,
          "a JPEG with SOF0 immediately after SOI");

    put_jpeg("real.jpg", 500, 700, 6, false, false);
    path_for(path, sizeof(path), "real.jpg");
    check(image_probe_size(path, &w, &h) == IMG_OK && w == 500 && h == 700,
          "a JPEG behind six application segments");

    put_jpeg("prog.jpg", 300, 300, 3, true, false);
    path_for(path, sizeof(path), "prog.jpg");
    check(image_probe_size(path, &w, &h) == IMG_OK && w == 300 && h == 300,
          "a progressive JPEG, which the decoder cannot read but the probe can measure");

    put_jpeg("standalone.jpg", 128, 176, 2, false, true);
    path_for(path, sizeof(path), "standalone.jpg");
    check(image_probe_size(path, &w, &h) == IMG_OK && w == 128 && h == 176,
          "standalone markers are skipped rather than read as segments");

    /* A PNG called .jpg. The magic decides, not the name -- the card that prompted JPEG support
     * carries fifteen .jpeg and one .jpg, and nothing says the extensions are honest. */
    put_png("liar.jpg", 109, 155);
    path_for(path, sizeof(path), "liar.jpg");
    check(image_probe_size(path, &w, &h) == IMG_OK && w == 109 && h == 155,
          "a PNG named .jpg is measured as a PNG");

    printf("\nrefusing what it cannot read\n");

    put("empty.png", "", 0);
    path_for(path, sizeof(path), "empty.png");
    check(image_probe_size(path, &w, &h) != IMG_OK && w == 0 && h == 0,
          "an empty file fails and zeroes its outputs");

    /* Half a download. This is the case the whole probe has to survive: it must not hang, must
     * not read off the end, and must report failure so the record stays unprobed and is retried
     * once the rest of the file arrives. */
    put_jpeg("cut.jpg", 400, 560, 4, false, false);
    {
        char p2[512];
        path_for(p2, sizeof(p2), "cut.jpg");
        FILE *f = fopen(p2, "rb");
        uint8_t buf[4096];
        size_t got = f ? fread(buf, 1, sizeof(buf), f) : 0;
        if (f) fclose(f);
        put("cut.jpg", buf, got / 3);
    }
    path_for(path, sizeof(path), "cut.jpg");
    check(image_probe_size(path, &w, &h) != IMG_OK, "a truncated JPEG fails rather than guessing");

    put("junk.png", "not an image at all", 19);
    path_for(path, sizeof(path), "junk.png");
    check(image_probe_size(path, &w, &h) != IMG_OK, "a file that is neither is refused");

    path_for(path, sizeof(path), "absent.png");
    check(image_probe_size(path, &w, &h) == IMG_ERR_NO_FILE, "a missing file says so");
    check(image_probe_size(NULL, &w, &h) == IMG_ERR_INT, "a NULL path is an argument error");
}

static void test_snap (void) {
    printf("\nsnapping a source aspect to one of three\n");

    check(boxart_snap(127, 181) == ART_PORTRAIT, "an N64 box is portrait");
    check(boxart_snap(126, 126) == ART_SQUARE, "a Game Boy box is square");
    check(boxart_snap(280, 196) == ART_LANDSCAPE, "the old title-card spec is landscape");
    check(boxart_snap(640, 448) == ART_LANDSCAPE, "and so is a screenshot");
    check(boxart_snap(112, 158) == ART_PORTRAIT, "a 112x158 Japanese scan is portrait");
    check(boxart_snap(1020, 747) == ART_LANDSCAPE, "a 1020x747 scan is landscape");

    /* A scan with margin on all four sides is pulled towards square. It must not get there:
     * margin is the single most common thing wrong with a cover, and calling a margined portrait
     * box square would crop a third off it. */
    check(boxart_snap(139, 189) == ART_PORTRAIT, "a portrait box with 10 % margin is still portrait");

    printf("\nthe boundaries, which are in log space\n");
    /* Portrait 0.7017 and square 1.0 meet at sqrt(0.7017) = 0.8377, not at the linear midpoint
     * 0.8509. Anything between those two is judged differently by the two rules, so it is exactly
     * where a mistake would live. */
    check(boxart_snap(830, 1000) == ART_PORTRAIT, "0.830 is portrait");
    check(boxart_snap(845, 1000) == ART_SQUARE, "0.845 is square, which a linear midpoint denies");
    /* Square and landscape meet at sqrt(1.4286) = 1.1952, against a linear 1.2143. */
    check(boxart_snap(1190, 1000) == ART_SQUARE, "1.190 is square");
    check(boxart_snap(1200, 1000) == ART_LANDSCAPE, "1.200 is landscape, ditto");

    printf("\nextremes and nonsense\n");
    check(boxart_snap(1, 1000) == ART_PORTRAIT, "a sliver clamps to the tallest shape offered");
    check(boxart_snap(1000, 1) == ART_LANDSCAPE, "a banner clamps to the widest");
    check(boxart_snap(0, 100) == ART_KIND_UNKNOWN, "zero width is not a shape");
    check(boxart_snap(100, 0) == ART_KIND_UNKNOWN, "nor is zero height");
    check(boxart_snap(-5, 10) == ART_KIND_UNKNOWN, "nor is a negative");

    printf("\nthe shapes themselves\n");
    art_shape_t p = boxart_shape_at(ART_PORTRAIT);
    art_shape_t s = boxart_shape_at(ART_SQUARE);
    art_shape_t l = boxart_shape_at(ART_LANDSCAPE);
    check(p.h > s.h && s.h > l.h, "portrait is tallest, landscape shortest");
    check(s.h == s.w, "the square one is square");
    /* The atlas slot is sized from TILE_H_MAX, so a shape taller than it would be written past
     * the end of its slot. thumbstore asserts the ceiling; this asserts nothing reaches it. */
    check(p.h <= TILE_H_MAX && l.h >= TILE_H_MIN, "every shape is inside the bounds the atlas sized for");

    printf("\nwhich column each shape earns\n");
    /* The rule is "the fewest columns that still show two whole rows", and what makes it safe is
     * that it is MONOTONE: a taller shape never gets a wider column. A mixed tab is laid out on
     * its tallest shape, so if that were ever false one of the other shapes in the tab would have
     * to be upscaled out of the atlas -- which is the one thing caching at the drawn size exists
     * to avoid, and it would look like slightly soft art rather than like a bug. */
    check(p.w == TILE_W_NARROW, "portrait is too tall for the wide column and takes the narrow one");
    check(s.w == TILE_W_WIDE && l.w == TILE_W_WIDE, "square and landscape both fit the wide one");
    check(l.w == TILE_W_WIDE && l.h == 98, "a landscape tile is 140 x 98, as it was at four columns");
    for (int k = 1; k < ART_SHAPES; k++) {
        art_shape_t a = boxart_shape_at(k - 1), b = boxart_shape_at(k);
        check(a.h < b.h || a.w <= b.w, "a taller shape never gets a wider column");
    }

    /* Every snap result must be a drawable shape. A kind that indexed off the end would return
     * portrait from boxart_shape_at() and look almost right. */
    for (int k = 0; k < ART_SHAPES; k++) {
        art_shape_t sh = boxart_shape_at(k);
        check((sh.w == TILE_W_WIDE || sh.w == TILE_W_NARROW) &&
              sh.h >= TILE_H_MIN && sh.h <= TILE_H_MAX, "shape is within bounds");
        check(sh.w != TILE_W_WIDE || sh.h <= TILE_H_TWO_ROW,
              "a wide shape fits two rows, which is why it is wide");
    }

    printf("\nfitting a shape into a cell it did not choose\n");
    /* What a mixed tab does. A landscape cover in a portrait tab is drawn at the portrait column
     * width, and must keep its aspect while it shrinks. */
    art_shape_t in_narrow = boxart_fit_into(l, p.w, p.h);
    check(in_narrow.w == TILE_W_NARROW && in_narrow.h == 76,
          "a landscape cover in a portrait tab is 109 x 76");
    check(boxart_fit_into(s, p.w, p.h).h == TILE_W_NARROW, "and a square one is 109 x 109");
    /* The other direction is the transient: a cover measured after its tab was laid out can be
     * taller than the row it lands in. It must scale, not squash -- pinning the height and
     * leaving the width alone is what the grid used to do. */
    art_shape_t squeezed = boxart_fit_into(p, TILE_W_NARROW, 100);
    check(squeezed.h == 100 && squeezed.w == 70,
          "a tile too tall for its row narrows as well as shortens");
    check(boxart_fit_into(l, 0, 100).w == 0 && boxart_fit_into((art_shape_t){0, 0}, 10, 10).w == 0,
          "a nonsense cell or shape fits nothing");
}

int main (void) {
    printf("== box art shapes\n");
    test_probe();
    test_snap();
    printf("\n  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
