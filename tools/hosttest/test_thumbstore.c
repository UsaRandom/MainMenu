/**
 * @file test_thumbstore.c
 * @brief Host-side round-trip test for src/library/thumbstore.c.
 *
 * The atlas is the least-tested file in the project and the one with the most arithmetic in it.
 * Under ares `cache_writable()` is false, so `thumbstore_open()` finds no pak, cannot create one,
 * and returns with `pak == NULL` -- after which `thumbstore_put()` and `thumbstore_fetch()` both
 * return on their first line. **Every slot offset, every stride branch, the padding that keeps
 * the file a whole number of slots, and the header rewrite that keeps `slot_count` honest have
 * therefore never executed anywhere.** The first machine to run them was going to be the console.
 *
 * So run them here. thumbstore.c needs `surface_t`, three timing macros and `debugf` from
 * libdragon, all shimmed, plus the two column widths and TILE_H_MAX from the real ui/theme.h.
 * The file itself is compiled unmodified.
 *
 * What this covers: create, append, close, reopen, fetch, the size-changed miss, padded strides,
 * multi-slot offset arithmetic, a corrupt header, an index pointing past the end of the pak, and
 * the read-only path. What it cannot: FatFs cluster behaviour, PI DMA alignment, and whether 22
 * MB/s holds. Those are hardware questions and they stay hardware questions.
 *
 *     tools/hosttest/run.sh
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/cache.h"
#include "menu/paths.h"
#include "library/thumbstore.h"
#include "ui/theme.h"

/* The shape most of this suite uses: the NTSC N64 box at 109 x 155, which lives in the narrow
 * grid column. A tile is a box shape now and BOTH of its dimensions vary -- square and landscape
 * art earn the wider 140 px column -- so there is no one tile size to test against. The square
 * case below is 140 x 140 and is a genuinely different width, not just a different height. */
#define TILE_W      TILE_W_NARROW
#define TILE_H      155
#define SQ_W        TILE_W_WIDE
#define SQ_H        140
#define SLOT_BYTES  49152

/* ------------------------------------------------------------------ shims */

bool directory_create (char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return mkdir(tmp, 0777) == 0;
}

/* ------------------------------------------------------------------ harness */

static int failures;
static int checks;

static void check (bool ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

static const char *testdir (void) {
    const char *d = getenv("TESTDIR");
    return d ? d : "build/hosttest/thumbdir";
}

/* ------------------------------------------------------------------ surfaces */

/**
 * @brief A TILE_W x TILE_H surface whose pixels are a function of @p seed and their position.
 *
 * Content-addressed on purpose: every pixel is distinct, so a slot read back from the wrong
 * offset, sheared by a stride bug, or truncated by a short read cannot accidentally compare
 * equal. A surface of one flat colour would pass all three.
 */
static surface_t make_tile_wh (uint16_t seed, uint16_t stride, int width, int height) {
    surface_t s = { .width = width, .height = height, .stride = stride };
    s.buffer = calloc(1, (size_t)stride * height);
    for (int y = 0; y < height; y++) {
        uint16_t *row = (uint16_t *)((uint8_t *)s.buffer + (size_t)y * stride);
        for (int x = 0; x < width; x++) {
            row[x] = (uint16_t)(seed * 7919u + (uint32_t)y * width + x);
        }
    }
    return s;
}

static surface_t make_tile (uint16_t seed, uint16_t stride) {
    return make_tile_wh(seed, stride, TILE_W, TILE_H);
}

/** @brief Compare only the used part of each row, so padding bytes are not part of the claim. */
static bool tiles_equal (const surface_t *a, const surface_t *b) {
    if (a->width != b->width || a->height != b->height) {
        return false;
    }
    for (int y = 0; y < a->height; y++) {
        const uint8_t *pa = (const uint8_t *)a->buffer + (size_t)y * a->stride;
        const uint8_t *pb = (const uint8_t *)b->buffer + (size_t)y * b->stride;
        if (memcmp(pa, pb, TILE_W * 2) != 0) {
            return false;
        }
    }
    return true;
}

static long file_size (const char *rel) {
    char path[512];
    snprintf(path, sizeof(path), "%s" MENU_DIR "/cache/%s", testdir(), rel);
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void rm_rf (const char *dir) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* first run: nothing to remove */ }
}

/* ------------------------------------------------------------------ tests */

int main (void) {
    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/", testdir());
    rm_rf(testdir());

    /* -------------------------------------------------- create */
    printf("\ncreate\n");
    cache_init(prefix);
    check(cache_writable(), "the test directory is writable");

    thumbstore_open();
    check(thumbstore_available(), "a fresh atlas is created and available");
    check(thumbstore_count() == 0, "a fresh atlas holds no tiles");
    /* Slot 0's space is the header's, so an empty pak is exactly one slot long. Getting this
     * wrong shifts every subsequent slot and reads garbage. */
    check(file_size("thumbs.pak") == SLOT_BYTES, "an empty pak is exactly one slot long");

    /* -------------------------------------------------- append and read back warm */
    printf("\nappend\n");
    surface_t a = make_tile(1, TILE_W * 2);
    thumbstore_put("sd:/art/alpha.png", 12345, &a, 0xABCD);
    check(thumbstore_count() == 1, "the tile is indexed");
    check(file_size("thumbs.pak") == 2 * SLOT_BYTES,
          "the pak grew by exactly one slot, padding included");

    surface_t got = make_tile(99, TILE_W * 2);
    uint16_t dom = 0;
    check(thumbstore_fetch("sd:/art/alpha.png", 12345, &got, &dom), "the tile is found");
    check(tiles_equal(&a, &got), "every pixel survives the round trip");
    check(dom == 0xABCD, "the dominant colour survives with it");

    /* -------------------------------------------------- invalidation */
    printf("\ninvalidation\n");
    check(!thumbstore_fetch("sd:/art/alpha.png", 999, &got, &dom),
          "a source file of a different size misses");
    check(!thumbstore_fetch("sd:/art/beta.png", 12345, &got, &dom),
          "a different source path misses");

    /* thumbstore_has() must agree with thumbstore_fetch() on every one of those, because the
     * caller uses it to decide whether to allocate a 27,440-byte surface at all. A has() that
     * says yes where fetch() says no puts the allocate-and-free back; one that says no where
     * fetch() would have said yes loses the tile silently and the grid draws a placeholder over
     * art that is sitting on the card. Same three keys, same three answers. */
    check(thumbstore_has("sd:/art/alpha.png", 12345), "has() agrees the tile is there");
    check(!thumbstore_has("sd:/art/alpha.png", 999), "has() agrees a resized source misses");
    check(!thumbstore_has("sd:/art/beta.png", 12345), "has() agrees a different path misses");
    check(!thumbstore_has(NULL, 12345), "has() survives a NULL path");
    check(!thumbstore_has("sd:/art/alpha.png", 0), "has() refuses a zero size");

    /* -------------------------------------------------- more slots, then a cold reopen */
    printf("\nmulti-slot\n");
    surface_t b = make_tile(2, TILE_W * 2);
    surface_t c = make_tile(3, TILE_W * 2);
    thumbstore_put("sd:/art/beta.png",  2222, &b, 0x1111);
    thumbstore_put("sd:/art/gamma.png", 3333, &c, 0x2222);
    check(thumbstore_count() == 3, "three tiles indexed");
    check(file_size("thumbs.pak") == 4 * SLOT_BYTES, "the pak is header plus three slots");

    thumbstore_close();

    printf("\ncold reopen -- the warm-boot path\n");
    thumbstore_open();
    check(thumbstore_available(), "the existing atlas reopens");
    check(thumbstore_count() == 3, "the index survived the close");

    /* Fetched in an order the writer never used. If slot_offset() were wrong by a constant, the
     * first tile would still round-trip and the third would not. */
    check(thumbstore_fetch("sd:/art/gamma.png", 3333, &got, &dom) &&
          tiles_equal(&c, &got) && dom == 0x2222, "slot 2 reads back after a cold open");
    check(thumbstore_fetch("sd:/art/alpha.png", 12345, &got, &dom) &&
          tiles_equal(&a, &got) && dom == 0xABCD, "slot 0 reads back after a cold open");
    check(thumbstore_fetch("sd:/art/beta.png", 2222, &got, &dom) &&
          tiles_equal(&b, &got) && dom == 0x1111, "slot 1 reads back after a cold open");

    /* -------------------------------------------------- padded stride */
    printf("\npadded stride\n");
    /* surface_alloc() is free to pad, and 109 RGBA16 pixels happen to need none today. "Happens
     * to" is not a contract; a shear here would read as a corrupt decoder, not a stride bug. */
    surface_t wide = make_tile(4, TILE_W * 2 + 32);
    thumbstore_put("sd:/art/wide.png", 4444, &wide, 0x3333);
    surface_t wide_got = make_tile(98, TILE_W * 2 + 32);
    check(thumbstore_fetch("sd:/art/wide.png", 4444, &wide_got, &dom) &&
          tiles_equal(&wide, &wide_got), "a padded stride round-trips without shearing");
    /* And across the two layouts, which is what happens when one boot pads and the next does not. */
    surface_t tight_got = make_tile(97, TILE_W * 2);
    check(thumbstore_fetch("sd:/art/wide.png", 4444, &tight_got, &dom) &&
          tiles_equal(&wide, &tight_got), "a padded write is readable into a tight surface");
    check(file_size("thumbs.pak") == 5 * SLOT_BYTES, "a padded tile still occupies one slot");

    /* -------------------------------------------------- two shapes in one atlas */
    printf("\nbox shapes\n");
    /* A slot no longer implies a tile size: a Game Boy cover is 140 x 140 in the same 48 KB slot
     * an N64 cover fills 109 x 155 of. The index carries each tile's own dimensions, and this is
     * what says so -- without it the square tile reads back sheared, at the wrong stride AND the
     * wrong row count, which is a picture rather than an error and would have shipped.
     *
     * Both dimensions differ here on purpose. When this was written the two shapes shared a width
     * and only the height was in question, so a fetch that compared heights alone passed; the
     * wide column made the width vary too and that version would not have noticed. */
    surface_t sq = make_tile_wh(6, SQ_W * 2, SQ_W, SQ_H);
    thumbstore_put("sd:/art/square.png", 6666, &sq, 0x4444);
    surface_t sq_got = make_tile_wh(96, SQ_W * 2, SQ_W, SQ_H);
    check(thumbstore_fetch("sd:/art/square.png", 6666, &sq_got, &dom) &&
          tiles_equal(&sq, &sq_got) && dom == 0x4444,
          "a square tile round-trips beside portrait ones");
    check(file_size("thumbs.pak") == 6 * SLOT_BYTES,
          "a square tile occupies the same one slot as a tall one");

    /* The shape is part of the key, not a hint. Switching the box art region reshapes every tile
     * while the atlas still holds the old ones, and a read at the wrong height does not fail --
     * so a mismatch has to be turned into a miss here, where the dimensions are known. */
    surface_t tall_got = make_tile(95, TILE_W * 2);
    check(!thumbstore_fetch("sd:/art/square.png", 6666, &tall_got, &dom),
          "fetching a square tile into a portrait surface is a miss, not a sheared read");
    check(!thumbstore_fetch("sd:/art/alpha.png", 12345, &sq_got, &dom),
          "and the other way round");
    /* Still there for whoever asks at the right shape, because a region can be switched back. */
    check(thumbstore_fetch("sd:/art/square.png", 6666, &sq_got, &dom) && tiles_equal(&sq, &sq_got),
          "a shape miss does not evict the tile");

    /* Same height, different width -- which is the case the three built-in shapes cannot produce
     * and a boxart.ini can. A 0.78-aspect box is too tall for the wide column, so it lands at
     * 109 x 140 beside a square cover's 140 x 140.
     *
     * Written because mutating the fetch to compare heights alone left this suite entirely green:
     * every other pair here differs in both dimensions, so the width comparison was load-bearing
     * and unexercised. Reading 140 px of source into a 109 px surface is a shear, not a failure. */
    surface_t narrow_got = make_tile_wh(94, TILE_W_NARROW * 2, TILE_W_NARROW, SQ_H);
    check(!thumbstore_fetch("sd:/art/square.png", 6666, &narrow_got, &dom),
          "a same-height tile of a different width is a miss too");

    thumbstore_close();

    /* -------------------------------------------------- a corrupt header */
    printf("\nrejection\n");
    {
        char path[512];
        snprintf(path, sizeof(path), "%s" MENU_DIR "/cache/thumbs.pak", testdir());
        FILE *f = fopen(path, "rb+");
        check(f != NULL, "the pak can be reopened to damage it");
        uint32_t junk = 0xDEADBEEF;
        if (f) { fwrite(&junk, 1, 4, f); fclose(f); }
    }
    thumbstore_open();
    check(thumbstore_available(), "a rejected pak is replaced, not left broken");
    check(thumbstore_count() == 0, "the index goes with it");
    check(file_size("thumbs.pak") == SLOT_BYTES, "the replacement is a fresh empty pak");
    check(file_size("thumbs.idx") == -1, "the stale index is deleted rather than kept");
    check(!thumbstore_fetch("sd:/art/alpha.png", 12345, &got, &dom),
          "nothing survives a format rejection");
    thumbstore_close();

    /* -------------------------------------------------- read-only storage, the ares case */
    printf("\nread-only storage\n");
    {
        char ro[512];
        snprintf(ro, sizeof(ro), "%s-ro", testdir());
        rm_rf(ro);
        mkdir(ro, 0555);
        char roprefix[520];
        snprintf(roprefix, sizeof(roprefix), "%s/", ro);
        cache_init(roprefix);
        check(!cache_writable(), "read-only storage probes as unwritable");

        thumbstore_open();
        check(!thumbstore_available(), "no atlas is invented on a card that cannot hold one");
        surface_t d = make_tile(5, TILE_W * 2);
        thumbstore_put("sd:/art/delta.png", 5555, &d, 0x4444);   /* must not fault */
        check(thumbstore_count() == 0, "put is refused rather than attempted");
        check(!thumbstore_fetch("sd:/art/delta.png", 5555, &got, &dom), "fetch is a quiet miss");
        check(!thumbstore_has("sd:/art/delta.png", 5555), "has() is a quiet miss with no pak");
        thumbstore_flush();                                       /* must not fault */
        thumbstore_close();
        free(d.buffer);
        chmod(ro, 0755);
        rm_rf(ro);
    }

    free(a.buffer); free(b.buffer); free(c.buffer);
    free(wide.buffer); free(wide_got.buffer); free(tight_got.buffer); free(got.buffer);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
