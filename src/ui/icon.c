/**
 * @file icon.c
 * @brief See icon.h for why this is a cache and not a decoder.
 * @ingroup ui
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "svg64/svg64.h"
#include "ui/icon.h"

#define PACK_PATH   "rom:/icons.pack"
#define META_PATH   "rom:/icons.meta"

/**
 * @brief Entries per size class.
 *
 * The picker shows 45 cells at 40 px, so 64 covers a page with nineteen entries spare -- enough
 * that paging away and back still finds some of what it left. That spare margin only means
 * anything because the cache is associative: while it was direct-mapped, a page of 45 occupied
 * about 32 slots and the rest never got to exist at all. See find().
 *
 * At 3,200 bytes an entry that is 204,800 bytes, which is the largest single allocation this
 * program makes -- and it is affordable only because the M64 has the Expansion Pak built in.
 *
 * Overridable with `TUNE=-DCACHE_40=n`, which is how the claim above is checked rather than
 * asserted: build with 32 and the FRAME line's `icons=` column never falls to zero on the picker,
 * because a page of 45 cannot be held in 32 entries however they are indexed.
 *
 * The 60 px class holds one preview and one behind it, because that is all anything draws.
 */
#ifndef CACHE_40
#define CACHE_40    64
#endif
#define CACHE_60    2

/** Bytes of scratch svg64 needs. Sized for the larger class and shared; see icon.h. */
#define SCRATCH_BYTES  SVG64_SCRATCH_BYTES(ICON_LARGE, ICON_LARGE)

/** Never rasterise more than this in one frame however much budget is left. */
#define PUMP_MAX    3

typedef struct {
    surface_t surf;
    uint16_t  index;    /**< which icon is in it, or ICON_NONE if empty */
    uint16_t  ink;
    uint16_t  paper;
    uint32_t  used;     /**< stamp of the last touch; see pick_victim() */
    bool      valid;
} entry_t;

typedef struct {
    entry_t *entries;
    int      n;
    int      size;      /**< pixels, square */
    /* One pending request, the most recent. See icon_request(). */
    uint16_t want;
    uint16_t want_ink;
    uint16_t want_paper;
} cache_t;

static cache_t cache_small;
static cache_t cache_large;

/* ------------------------------------------------------------------ the pack -- */

typedef struct {
    uint32_t data_off, data_len, name_off, name_len;
} pack_entry_t;

static FILE *pack_fp;
static pack_entry_t *pack_index;
static int pack_count;

/** Read once at open, reused for every decode. Never freed; the menu holds it for its life. */
static uint8_t *scratch;
static char *svg_buf;
static size_t svg_cap;

/* --------------------------------------------------------------- the metadata -- */

typedef struct {
    uint16_t first, count, name_off, pad;
} meta_cat_t;

static uint8_t *meta_blob;
static meta_cat_t *meta_cats;
static int meta_cat_n;
static const uint16_t *meta_order;
static int meta_order_n;
static const char *meta_strtab;
static uint16_t meta_starter[ICON_STARTERS];

/** Bumped with the layout. v2 added the starter block; nothing shipped v1. */
#define META_VERSION  2

static uint32_t be32 (const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16 (const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* ------------------------------------------------------------------- opening -- */

static bool open_pack (void) {
    pack_fp = fopen(PACK_PATH, "rb");
    if (pack_fp == NULL) {
        debugf("icon: no %s; the picker will have nothing to show\n", PACK_PATH);
        return false;
    }

    uint8_t hdr[16];
    if (fread(hdr, 1, sizeof(hdr), pack_fp) != sizeof(hdr) || memcmp(hdr, "SVGP", 4) != 0) {
        debugf("icon: %s is not an icon pack\n", PACK_PATH);
        return false;
    }
    if (be32(hdr + 4) != 1) {
        /* Same discipline as every other versioned file here: a mismatch is refused rather than
         * guessed at. There is nothing to delete and rebuild -- the pack is in the cartridge --
         * so refusing means the picker is empty, which is visible and safe. */
        debugf("icon: %s is version %lu, this build reads 1\n",
               PACK_PATH, (unsigned long)be32(hdr + 4));
        return false;
    }
    pack_count = (int)be32(hdr + 8);
    if (pack_count <= 0 || pack_count > 0xFFFF) {
        pack_count = 0;
        return false;
    }

    size_t bytes = (size_t)pack_count * 16;
    uint8_t *raw = malloc(bytes);
    pack_index = malloc((size_t)pack_count * sizeof(pack_entry_t));
    if (raw == NULL || pack_index == NULL || fread(raw, 1, bytes, pack_fp) != bytes) {
        free(raw);
        free(pack_index);
        pack_index = NULL;
        pack_count = 0;
        return false;
    }
    /* Byte-swapped once, here, rather than on every seek. 3,894 entries is 62 KB resident --
     * the names blob is deliberately NOT loaded with it, because only the icon under the cursor
     * ever needs its name and that is one short read when the cursor moves. Holding all of them
     * would be another 78 KB for a string shown one at a time. */
    size_t largest = 0;
    for (int i = 0; i < pack_count; i++) {
        pack_index[i].data_off = be32(raw + i * 16);
        pack_index[i].data_len = be32(raw + i * 16 + 4);
        pack_index[i].name_off = be32(raw + i * 16 + 8);
        pack_index[i].name_len = be32(raw + i * 16 + 12);
        if (pack_index[i].data_len > largest) {
            largest = pack_index[i].data_len;
        }
    }
    free(raw);

    /* One buffer, sized for the biggest icon in this pack, allocated once. The corpus's worst
     * file is 23,919 bytes; sizing from the pack rather than from a constant means a corpus with
     * a bigger one does not overrun, and a capped build does not reserve for icons it lacks. */
    svg_cap = largest + 1;
    svg_buf = malloc(svg_cap);
    /* malloc_uncached is wrong here -- this is CPU scratch that svg64 hammers, not something the
     * RDP reads -- so a plain aligned allocation. libdragon's malloc is 8-byte aligned already;
     * the assert states the requirement svg64 documents rather than trusting that. */
    scratch = malloc(SCRATCH_BYTES);
    assertf(((uintptr_t)scratch & 7) == 0, "svg64 scratch must be 8-byte aligned");
    if (svg_buf == NULL || scratch == NULL) {
        debugf("icon: no room for the decode buffers (%u + %u bytes)\n",
               (unsigned)svg_cap, (unsigned)SCRATCH_BYTES);
        return false;
    }

    debugf("icon: %d icons, index %u bytes, largest %u, scratch %u\n",
           pack_count, (unsigned)bytes, (unsigned)largest, (unsigned)SCRATCH_BYTES);
    return true;
}

static bool open_meta (void) {
    FILE *f = fopen(META_PATH, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 16 || len > (1 << 20)) {
        fclose(f);
        return false;
    }
    meta_blob = malloc((size_t)len);
    if (meta_blob == NULL || fread(meta_blob, 1, (size_t)len, f) != (size_t)len) {
        free(meta_blob);
        meta_blob = NULL;
        fclose(f);
        return false;
    }
    fclose(f);

    if (memcmp(meta_blob, "ICNM", 4) != 0 || be32(meta_blob + 4) != META_VERSION) {
        free(meta_blob);
        meta_blob = NULL;
        return false;
    }
    meta_cat_n = (int)be32(meta_blob + 8);
    meta_order_n = (int)be32(meta_blob + 12);

    size_t starters_at = 16;
    size_t cats_at = starters_at + (size_t)ICON_STARTERS * 2;
    size_t need = cats_at + (size_t)meta_cat_n * 8 + (size_t)meta_order_n * 2;
    if (meta_cat_n <= 0 || meta_order_n <= 0 || need > (size_t)len) {
        free(meta_blob);
        meta_blob = NULL;
        meta_cat_n = meta_order_n = 0;
        return false;
    }

    /* Swapped in place: the blob is ours and nothing else reads it big-endian afterwards. */
    meta_cats = malloc((size_t)meta_cat_n * sizeof(meta_cat_t));
    if (meta_cats == NULL) {
        free(meta_blob);
        meta_blob = NULL;
        meta_cat_n = meta_order_n = 0;
        return false;
    }
    for (int i = 0; i < ICON_STARTERS; i++) {
        meta_starter[i] = be16(meta_blob + starters_at + (size_t)i * 2);
    }
    for (int i = 0; i < meta_cat_n; i++) {
        const uint8_t *p = meta_blob + cats_at + i * 8;
        meta_cats[i].first    = be16(p);
        meta_cats[i].count    = be16(p + 2);
        meta_cats[i].name_off = be16(p + 4);
    }
    uint16_t *order = (uint16_t *)(meta_blob + cats_at + (size_t)meta_cat_n * 8);
    for (int i = 0; i < meta_order_n; i++) {
        order[i] = be16((const uint8_t *)&order[i]);
    }
    meta_order = order;
    meta_strtab = (const char *)(meta_blob + need);
    return true;
}

static bool cache_alloc (cache_t *c, int n, int size) {
    c->entries = calloc((size_t)n, sizeof(entry_t));
    if (c->entries == NULL) {
        return false;
    }
    c->n = n;
    c->size = size;
    c->want = ICON_NONE;
    for (int i = 0; i < n; i++) {
        c->entries[i].index = ICON_NONE;
        /* One allocation per entry, at init, so a decode never allocates. surface_make_linear
         * gives the 8-byte alignment both svg64 and the RDP need from a texture. */
        c->entries[i].surf = surface_alloc(FMT_RGBA16, size, size);
        if (c->entries[i].surf.buffer == NULL) {
            return false;
        }
    }
    return true;
}

void icon_init (void) {
    if (!open_pack()) {
        return;
    }
    (void)open_meta();

    if (!cache_alloc(&cache_small, CACHE_40, ICON_SMALL) ||
        !cache_alloc(&cache_large, CACHE_60, ICON_LARGE)) {
        debugf("icon: cache allocation failed; icons are disabled\n");
        pack_count = 0;
    }
    debugf("icon: caches %d x %d px and %d x %d px = %u bytes\n",
           CACHE_40, ICON_SMALL, CACHE_60, ICON_LARGE,
           (unsigned)(CACHE_40 * ICON_SMALL * ICON_SMALL * 2 +
                      CACHE_60 * ICON_LARGE * ICON_LARGE * 2));
}

int icon_count (void) {
    return pack_count;
}

/* -------------------------------------------------------------------- lookup -- */

static cache_t *class_for (int size) {
    if (size == ICON_LARGE) {
        return (cache_large.n > 0) ? &cache_large : NULL;
    }
    return (cache_small.n > 0) ? &cache_small : NULL;
}

/**
 * @brief Monotonic touch counter. Wraps, which is why every comparison is a signed difference.
 *
 * Bumped on every hit and every request, so about 90 times a frame while the picker is open --
 * 2,700 a second, or eighteen days to wrap. It wraps anyway, and `(int32_t)(a - b) < 0` is correct
 * across the wrap where `a < b` is not.
 */
static uint32_t clock_stamp;

static void touch (entry_t *e) {
    e->used = ++clock_stamp;
}

/**
 * @brief The entry holding exactly (@p index, @p ink, @p paper), or NULL.
 *
 * Fully associative, and it has to be. This was direct-mapped on `index % n`, on the stated
 * assumption that "the access pattern is a grid cursor walking a contiguous run of indices". That
 * assumption is wrong: the picker walks a *category*, which is a list of scattered pack indices
 * baked by tools/mkiconmeta.py, not a run. Measured over the shipped metadata, 87 of the 100
 * category pages collide, nine cells per page on average and eighteen on the worst -- Travel page
 * 1 puts 45 icons into 30 slots. Colliding cells can never all be resident, so each one evicts the
 * other the moment it is decoded and the page never fills: icons missing, a different subset
 * missing every time the cursor moves, and cells visibly blinking.
 *
 * A linear scan of 64 entries costs 64 comparisons against a 6,377 us decode. There was never
 * anything to save here.
 */
static entry_t *find (cache_t *c, uint16_t index, uint16_t ink, uint16_t paper) {
    for (int i = 0; i < c->n; i++) {
        entry_t *e = &c->entries[i];
        if (e->valid && e->index == index && e->ink == ink && e->paper == paper) {
            return e;
        }
    }
    return NULL;
}

/** @brief An empty entry, or the least recently touched one. */
static entry_t *pick_victim (cache_t *c) {
    entry_t *best = &c->entries[0];
    for (int i = 0; i < c->n; i++) {
        entry_t *e = &c->entries[i];
        if (!e->valid) {
            return e;
        }
        if ((int32_t)(e->used - best->used) < 0) {
            best = e;
        }
    }
    return best;
}

const surface_t *icon_get (uint16_t index, int size, uint16_t ink, uint16_t paper) {
    cache_t *c = class_for(size);
    if (c == NULL || index >= pack_count) {
        return NULL;
    }
    entry_t *e = find(c, index, ink, paper);
    if (e == NULL) {
        return NULL;
    }
    touch(e);
    return &e->surf;
}

void icon_request (uint16_t index, int size, uint16_t ink, uint16_t paper) {
    cache_t *c = class_for(size);
    if (c == NULL || index >= pack_count) {
        return;
    }
    entry_t *e = find(c, index, ink, paper);
    if (e != NULL) {
        /* Touched even though there is nothing to do. The screen asks for its whole page every
         * frame, so this is what keeps a cell the cursor has not visited from being evicted for one
         * it has -- without it the LRU order would be draw order and the page would still churn. */
        touch(e);
        return;
    }
    c->want = index;
    c->want_ink = ink;
    c->want_paper = paper;
}

/* ------------------------------------------------------------------ decoding -- */

/** RGBA5551 to the RGBA8888 svg64 recolours in. */
static uint32_t to_rgba32 (uint16_t c) {
    uint32_t r = (c >> 11) & 0x1F;
    uint32_t g = (c >> 6) & 0x1F;
    uint32_t b = (c >> 1) & 0x1F;
    /* 5 bits to 8 by replicating the high bits, so 0x1F maps to 0xFF rather than to 0xF8 --
     * without it white artwork comes out three per cent grey and the picker looks dirty. */
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (r << 24) | (g << 16) | (b << 8) | 0xFF;
}

static bool decode (cache_t *c, uint16_t index, uint16_t ink, uint16_t paper) {
    if (index >= pack_count || pack_fp == NULL) {
        return false;
    }
    const pack_entry_t *pe = &pack_index[index];
    if (pe->data_len == 0 || pe->data_len >= svg_cap) {
        return false;
    }
    if (fseek(pack_fp, (long)pe->data_off, SEEK_SET) != 0) {
        return false;
    }
    if (fread(svg_buf, 1, pe->data_len, pack_fp) != pe->data_len) {
        return false;
    }

    /* Invalidated before the rasteriser is pointed at it, not after. svg64 writes into these
     * pixels, so a failed render leaves the previous occupant's image half overwritten -- and if
     * the entry were still marked valid, icon_get() would hand that out as the previous icon. */
    entry_t *e = pick_victim(c);
    e->valid = false;

    svg64_surface_t dst = {
        .pixels = e->surf.buffer,
        .width  = c->size,
        .height = c->size,
        .stride = e->surf.stride,
        .format = SVG64_FMT_RGBA16,
    };
    /* The corpus convention is a black backdrop path plus white artwork, so recolour maps those
     * two onto paper and ink by luma. The surface is cleared to paper as well, opaquely, which is
     * what lets a cell blit with no per-pixel blending at all: antialiased edges resolve against
     * the real background during rasterisation rather than against a transparent one. */
    svg64_opts_t opts = {
        .recolor = true,
        .dark    = to_rgba32(paper),
        .light   = to_rgba32(ink),
        .clear   = to_rgba32(paper),
    };
    if (svg64_render(svg_buf, pe->data_len, &dst, &opts, scratch, SCRATCH_BYTES) != SVG64_OK) {
        return false;
    }

    /* The RDP reads this as a texture and the CPU just wrote it, so the lines have to reach RAM
     * before it is used. Missing this shows as an icon that is stale or partly the previous
     * occupant of the slot, intermittently -- which looks like a cache-indexing bug and is not. */
    data_cache_hit_writeback(e->surf.buffer, (uint32_t)e->surf.stride * (uint32_t)c->size);

    e->index = index;
    e->ink = ink;
    e->paper = paper;
    e->valid = true;
    touch(e);
    return true;
}

static int pump_class (cache_t *c, uint32_t start, uint32_t budget, int done) {
    while (c != NULL && c->want != ICON_NONE && done < PUMP_MAX) {
        if (TICKS_DISTANCE(start, TICKS_READ()) >= (int32_t)budget) {
            break;
        }
        uint16_t index = c->want;
        c->want = ICON_NONE;
        if (decode(c, index, c->want_ink, c->want_paper)) {
            done++;
        }
    }
    return done;
}

int icon_pump (uint32_t budget_ticks) {
    if (pack_count == 0) {
        return 0;
    }
    uint32_t start = TICKS_READ();
    int done = 0;
    /* The preview first. It is the bigger of the two and the one being looked at directly; a
     * page of 40 px cells arriving before the thing the cursor is on would be backwards. */
    done = pump_class(&cache_large, start, budget_ticks, done);
    done = pump_class(&cache_small, start, budget_ticks, done);
    return done;
}

void icon_name (uint16_t index, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (index >= pack_count || pack_fp == NULL) {
        return;
    }
    const pack_entry_t *pe = &pack_index[index];
    size_t n = pe->name_len;
    if (n >= cap) {
        n = cap - 1;
    }
    if (fseek(pack_fp, (long)pe->name_off, SEEK_SET) != 0) {
        return;
    }
    if (fread(out, 1, n, pack_fp) != n) {
        out[0] = '\0';
        return;
    }
    out[n] = '\0';
}

/* ---------------------------------------------------------------- categories -- */

int icon_cat_count (void) {
    return meta_cat_n;
}

const char *icon_cat_name (int cat) {
    if (meta_blob == NULL || cat < 0 || cat >= meta_cat_n) {
        return "";
    }
    return meta_strtab + meta_cats[cat].name_off;
}

int icon_cat_size (int cat) {
    if (meta_blob == NULL || cat < 0 || cat >= meta_cat_n) {
        return 0;
    }
    return meta_cats[cat].count;
}

uint16_t icon_cat_at (int cat, int nth) {
    if (meta_blob == NULL || cat < 0 || cat >= meta_cat_n) {
        return ICON_NONE;
    }
    if (nth < 0 || nth >= meta_cats[cat].count) {
        return ICON_NONE;
    }
    int at = meta_cats[cat].first + nth;
    if (at < 0 || at >= meta_order_n) {
        return ICON_NONE;
    }
    return meta_order[at];
}

uint16_t icon_starter (int slot) {
    if (meta_blob == NULL || slot < 0 || slot >= ICON_STARTERS) {
        return ICON_NONE;
    }
    uint16_t idx = meta_starter[slot];
    return (idx < pack_count) ? idx : ICON_NONE;
}
