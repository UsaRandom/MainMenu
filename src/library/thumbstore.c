/**
 * @file thumbstore.c
 * @brief The decoded-art atlas. See thumbstore.h for why the slot is 49,152 bytes.
 * @ingroup library
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "cache.h"
#include "thumbstore.h"
#include "ui/theme.h"

#define PAK_FILE    "thumbs.pak"
#define IDX_FILE    "thumbs.idx"

/* Three 16 KB FAT clusters. See the header comment -- cluster alignment is the whole point of
 * the number, and the count follows from the biggest tile a slot has to hold.
 *
 * It was two clusters when every tile was 140 x 98. Tiles are per-system box shapes now and the
 * ceiling is TILE_W x TILE_H_MAX = 109 x 176 = 38,368 bytes, which does not fit in 32,768. The
 * alternative was to trim the tile until it did -- 107 x 152 would have -- and distorting the art
 * to suit a filesystem is the wrong way round. The cost is disk: 31 % of each slot is padding
 * against the old 16 %, so a 500-title card holds 24.6 MB of atlas instead of 16.4 MB. */
#define SLOT_BYTES  49152

/** The most pixels a slot may be asked to carry. */
#define TILE_BYTES_MAX  (TILE_W * TILE_H_MAX * 2)

/** Hard cap, so a corrupt index cannot make us seek to a nonsense offset in a 16 MB file. */
#define MAX_SLOTS   2048

/** @brief 32 bytes at the very front of the pak, inside slot 0's reserved space. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format_ver;
    uint16_t tile_w;        /**< every tile's width; the column, so it never varies */
    uint16_t tile_h;        /**< the CEILING, not the tile height -- see ti_record_t */
    uint16_t pixfmt;        /**< 0 = RGBA16 */
    uint32_t slot_bytes;
    uint32_t slot_count;
    uint8_t  reserved[12];
} pak_header_t;

/** @brief One index row. 20 bytes.
 *
 *  It carries the tile's own height because a slot no longer implies one: a Game Boy cover is
 *  109 x 109 in the same 48 KB slot an N64 cover fills 109 x 155 of. Reading a slot back at the
 *  wrong height does not fail, it shears the picture -- so the height is stored rather than
 *  re-derived from the record's system, which can change under the atlas when a ROM is moved
 *  between folders. */
typedef struct __attribute__((packed)) {
    uint64_t src_hash;
    uint32_t src_size;
    uint16_t dominant;
    uint16_t slot;
    uint16_t tile_w;
    uint16_t tile_h;
} ti_record_t;

/* Every on-disk struct's size is asserted, because the one bug this whole family of files
 * cannot survive is silent padding. A compiler that inserts two bytes somewhere writes a
 * file that passes its own magic, version and CRC and is then read back with every field
 * after the padding shifted -- which looks like corrupt data from a working card. __packed
 * should prevent it; this is what proves it did. */
_Static_assert(sizeof(pak_header_t) == 32, "pak header must stay 32 bytes");
_Static_assert(sizeof(ti_record_t) == 20, "thumb index record must stay 20 bytes");
_Static_assert(TILE_BYTES_MAX <= SLOT_BYTES, "the tallest tile must fit in its slot");
_Static_assert(SLOT_BYTES % 16384 == 0, "a slot must be a whole number of FAT clusters");

static FILE *pak;
static bool  pak_writable;
static ti_record_t *rows;
static int   row_count, row_cap;
static uint32_t slot_count;     /**< slots physically present in the pak */
static bool  idx_dirty;

int thumbstore_count (void) {
    return row_count;
}

bool thumbstore_available (void) {
    return pak != NULL;
}

static long slot_offset (uint32_t slot) {
    /* Slot 0's space is the header's, so data starts one slot in and every slot stays aligned to
     * the cluster. Costs 32 KB of card. */
    return (long)((slot + 1) * SLOT_BYTES);
}

/* ------------------------------------------------------------------ open */

/**
 * @brief Take the stdio buffer off @p f, immediately after opening it and before anything else.
 *
 * Unbuffered, on purpose. Every access to this file is a whole ~34,000-byte tile into a 64-byte
 * aligned uncached surface, which is precisely the shape libcart's bulk PI DMA path wants. A
 * stdio buffer in the middle would turn that into a read into a cached scratch buffer plus a
 * memcpy, giving up the alignment and paying for the copy -- on the one file where the whole
 * design is about the transfer being direct.
 *
 * Called the instant the handle exists because setvbuf() is only defined before any other
 * operation on the stream (C99 7.19.5.6). It used to run after the header had already been read,
 * which left newlib reconciling a discarded read-ahead buffer against the file offset. Every
 * access here happens to seek first, so it was survivable -- but "survivable undefined behaviour
 * on the read path of the art cache" is not a thing to carry onto new hardware.
 */
static void pak_unbuffered (FILE *f) {
    setvbuf(f, NULL, _IONBF, 0);
}

/** @brief Create a fresh pak with a valid header. Returns the open handle, or NULL. */
static FILE *pak_create (const char *path) {
    FILE *f = fopen(path, "wb+");
    if (f == NULL) {
        return NULL;
    }
    pak_unbuffered(f);
    /* The header is written into a full zeroed slot rather than as 32 bytes, so the first real
     * slot starts where slot_offset() says it does even before anything has been appended. */
    void *pad = calloc(1, SLOT_BYTES);
    if (pad == NULL) {
        fclose(f);
        return NULL;
    }
    pak_header_t *h = pad;
    h->magic      = THUMBSTORE_MAGIC;
    h->format_ver = MENU_CACHE_FORMAT_VER;
    h->tile_w     = TILE_W;
    h->tile_h     = TILE_H_MAX;
    h->pixfmt     = 0;
    h->slot_bytes = SLOT_BYTES;
    h->slot_count = 0;

    bool ok = (fwrite(pad, 1, SLOT_BYTES, f) == SLOT_BYTES);
    free(pad);
    if (!ok) {
        fclose(f);
        remove(path);
        return NULL;
    }
    fflush(f);
    return f;
}

void thumbstore_open (void) {
    char path[300];
    cache_path(path, sizeof(path), PAK_FILE);

    slot_count = 0;
    idx_dirty = false;
    pak_writable = false;

    /* Open read-write when we can, so appending does not need a reopen. "rb+" fails when the file
     * is absent, which is the first-run case and is handled by creating it. */
    pak = cache_writable() ? fopen(path, "rb+") : fopen(path, "rb");

    if (pak != NULL) {
        pak_unbuffered(pak);
        pak_header_t h;
        bool bad = (fread(&h, 1, sizeof(h), pak) != sizeof(h)) ||
                   h.magic      != THUMBSTORE_MAGIC ||
                   h.format_ver != MENU_CACHE_FORMAT_VER ||
                   h.tile_w     != TILE_W ||
                   h.tile_h     != TILE_H_MAX ||
                   h.slot_bytes != SLOT_BYTES ||
                   h.slot_count  > MAX_SLOTS;
        if (bad) {
            /* Asserting the geometry as well as the version is deliberate: changing TILE_W and
             * forgetting to bump MENU_CACHE_FORMAT_VER is a mistake someone will make, and the
             * symptom would be every tile drawn from misaligned bytes rather than a clean miss. */
            debugf("THUMBSTORE %s: header rejected -- rebuilding\n", PAK_FILE);
            fclose(pak);
            pak = NULL;
            cache_drop(PAK_FILE);
            cache_drop(IDX_FILE);
        } else {
            slot_count = h.slot_count;
        }
    }

    if (pak == NULL && cache_writable()) {
        pak = pak_create(path);
    }

    if (pak == NULL) {
        debugf("THUMBSTORE unavailable (%s) -- art will be decoded every boot\n", cache_status());
        return;
    }
    pak_writable = cache_writable();

    /* The index only means anything against the pak we just accepted. If it claims slots the pak
     * does not have, it is from a pak that has since been rebuilt; drop it rather than read
     * uninitialised space. */
    void *buf = NULL;
    uint32_t bytes = 0;
    if (cache_load(IDX_FILE, THUMBSTORE_MAGIC, &buf, &bytes)) {
        int n = (int)(bytes / sizeof(ti_record_t));
        ti_record_t *r = buf;
        int keep = 0;
        for (int i = 0; i < n; i++) {
            if (r[i].slot < slot_count) {
                r[keep++] = r[i];
            }
        }
        if (keep != n) {
            debugf("THUMBSTORE index: dropped %d rows past the end of the pak\n", n - keep);
            idx_dirty = true;
        }
        rows = r;
        row_count = keep;
        row_cap = n;
    }

    debugf("THUMBSTORE open: %lu slots on card, %d indexed, %s\n",
           (unsigned long)slot_count, row_count, pak_writable ? "writable" : "read-only");
}

void thumbstore_close (void) {
    thumbstore_flush();
    if (pak != NULL) {
        fclose(pak);
        pak = NULL;
    }
    free(rows);
    rows = NULL;
    row_count = row_cap = 0;
}

/* ------------------------------------------------------------------ lookup */

static const ti_record_t *find_row (uint64_t hash, uint32_t size) {
    for (int i = 0; i < row_count; i++) {
        if (rows[i].src_hash == hash && rows[i].src_size == size) {
            return &rows[i];
        }
    }
    return NULL;
}

/**
 * @brief Move @p bytes between the pak and @p pixels, honouring a padded surface stride.
 *
 * surface_alloc() is free to pad the stride, and a 109-pixel RGBA16 row is 218 bytes which
 * happens to need no padding today. "Happens to" is not a contract, so the padded case is handled
 * rather than asserted -- getting it wrong would shear every tile by a few pixels per row, which
 * reads as a corrupt decoder rather than as a stride bug.
 *
 * The height comes off the surface rather than off a constant, because a slot holds whatever
 * shape its system's box is. A slot is always SLOT_BYTES long regardless; the rest is padding.
 */
static bool slot_io (uint32_t slot, surface_t *surf, bool write) {
    if (fseek(pak, slot_offset(slot), SEEK_SET) != 0) {
        return false;
    }
    uint8_t *pixels = surf->buffer;
    int row_bytes = surf->width * 2;
    size_t tile_bytes = (size_t)row_bytes * surf->height;

    if (tile_bytes > SLOT_BYTES) {
        return false;                   /* cannot happen: TILE_H_MAX is asserted against the slot */
    }
    if (surf->stride == row_bytes) {
        size_t n = write ? fwrite(pixels, 1, tile_bytes, pak)
                         : fread (pixels, 1, tile_bytes, pak);
        return n == tile_bytes;
    }
    for (int y = 0; y < surf->height; y++) {
        uint8_t *line = pixels + (size_t)y * surf->stride;
        size_t n = write ? fwrite(line, 1, row_bytes, pak)
                         : fread (line, 1, row_bytes, pak);
        if (n != (size_t)row_bytes) {
            return false;
        }
    }
    return true;
}

bool thumbstore_has (const char *src_path, int64_t src_size) {
    if (pak == NULL || src_path == NULL || src_size <= 0) {
        return false;
    }
    return find_row(cache_hash64(src_path), (uint32_t)src_size) != NULL;
}

bool thumbstore_fetch (const char *src_path, int64_t src_size, surface_t *dst, uint16_t *dominant) {
    if (pak == NULL || src_path == NULL || src_size <= 0 || dst == NULL) {
        return false;
    }

    const ti_record_t *row = find_row(cache_hash64(src_path), (uint32_t)src_size);
    if (row == NULL) {
        return false;
    }

    /* A hit at the wrong shape is a miss, not a read.
     *
     * The tile in the slot was cut for whatever box shape was in force when it was decoded, and
     * that can change under a warm atlas -- somebody switches the region in Settings, or a ROM
     * moves from the NES folder to the SNES one. Reading 109 x 109 of pixels into a 109 x 155
     * surface does not fail; it fills two thirds of the tile with the picture and the rest with
     * whatever was in the slot's padding. Refusing sends it back to the decoder, which costs one
     * decode and produces the right picture. */
    if (row->tile_w != dst->width || row->tile_h != dst->height) {
        return false;
    }

    uint32_t t0 = TICKS_READ();
    if (!slot_io(row->slot, dst, false)) {
        debugf("THUMBSTORE slot %u unreadable\n", row->slot);
        return false;
    }
    if (dominant != NULL) {
        *dominant = row->dominant;
    }

    /* No cache writeback here, and that is deliberate rather than an omission.
     *
     * The first version flushed the surface after filling it, on the reasoning that the RDP reads
     * RDRAM directly and would otherwise sample a stale line. But surface_alloc() allocates with
     * malloc_uncached_aligned() (libdragon surface.c:47), so the buffer is already uncached: the
     * writes went straight to RDRAM and there is no line to flush. Calling it anyway is what
     * produces ares' "CACHE access to non-cacheable address" warning, and on hardware it is a
     * pointless pass over 27 KB.
     *
     * The same allocation is why this path is fast on the cart. 64-byte alignment clears the
     * 8-byte test in libcart's sc_card_rd_dram(), so the read takes the bulk PI DMA path rather
     * than the per-512-byte bounce-buffer fallback -- see AUDIT.md 1q on that cliff. */

    debugf("THUMBSTORE hit slot %u in %lu us\n", row->slot,
           (unsigned long)TIMER_MICROS(TICKS_SINCE(t0)));
    return true;
}

/* ------------------------------------------------------------------ append */

static bool rows_room (void) {
    if (row_count < row_cap) {
        return true;
    }
    int want = row_cap ? row_cap * 2 : 64;
    ti_record_t *bigger = realloc(rows, (size_t)want * sizeof(ti_record_t));
    if (bigger == NULL) {
        return false;
    }
    rows = bigger;
    row_cap = want;
    return true;
}

void thumbstore_put (const char *src_path, int64_t src_size, const surface_t *art, uint16_t dominant) {
    if (pak == NULL || !pak_writable || src_path == NULL || src_size <= 0 || art == NULL) {
        return;
    }
    if (slot_count >= MAX_SLOTS || !rows_room()) {
        return;
    }
    if (art->width != TILE_W || art->height < TILE_H_MIN || art->height > TILE_H_MAX) {
        return;                     /* not a grid tile; the sheet's large art is not atlas material */
    }

    uint64_t hash = cache_hash64(src_path);
    if (find_row(hash, (uint32_t)src_size) != NULL) {
        return;                     /* already stored; the atlas only ever appends */
    }

    uint32_t slot = slot_count;
    uint32_t t0 = TICKS_READ();

    /* Cast away const: slot_io serves both directions and the write path does not modify the
     * surface. Duplicating the stride handling for a const variant is worse than this. */
    if (!slot_io(slot, (surface_t *)art, true)) {
        debugf("THUMBSTORE write of slot %lu failed\n", (unsigned long)slot);
        return;
    }

    /* Pad the slot out to its full length. Without this the file's length is not a multiple of
     * the slot size, and the next append lands at an offset the reader will not look at. */
    static const uint8_t zeros[512];
    uint32_t written = (uint32_t)art->width * (uint32_t)art->height * 2;
    for (uint32_t left = SLOT_BYTES - written; left > 0; ) {
        uint32_t n = left < sizeof(zeros) ? left : (uint32_t)sizeof(zeros);
        if (fwrite(zeros, 1, n, pak) != n) {
            break;
        }
        left -= n;
    }

    slot_count++;
    rows[row_count].src_hash = hash;
    rows[row_count].src_size = (uint32_t)src_size;
    rows[row_count].dominant = dominant;
    rows[row_count].slot     = (uint16_t)slot;
    rows[row_count].tile_w   = (uint16_t)art->width;
    rows[row_count].tile_h   = (uint16_t)art->height;
    row_count++;
    idx_dirty = true;

    /* Keep the header's slot_count honest as we go. If power is lost before the index is
     * flushed the tile is simply unreferenced -- wasted space, not a wrong tile. */
    pak_header_t h;
    if (fseek(pak, 0, SEEK_SET) == 0 && fread(&h, 1, sizeof(h), pak) == sizeof(h)) {
        h.slot_count = slot_count;
        if (fseek(pak, 0, SEEK_SET) == 0) {
            fwrite(&h, 1, sizeof(h), pak);
        }
    }
    fflush(pak);

    debugf("THUMBSTORE stored slot %lu in %lu us\n", (unsigned long)slot,
           (unsigned long)TIMER_MICROS(TICKS_SINCE(t0)));
}

void thumbstore_flush (void) {
    if (!idx_dirty) {
        return;
    }
    if (row_count == 0 || rows == NULL) {
        /* Dirty with nothing to say means every row was dropped for pointing past the end of the
         * pak. Removing the file is the write: returning early instead left that same unusable
         * index on the card to be loaded, rejected and re-dropped on every boot forever. */
        cache_drop(IDX_FILE);
        idx_dirty = false;
        return;
    }
    if (cache_store(IDX_FILE, THUMBSTORE_MAGIC, rows,
                    (uint32_t)(row_count * sizeof(ti_record_t)))) {
        idx_dirty = false;
    }
}
