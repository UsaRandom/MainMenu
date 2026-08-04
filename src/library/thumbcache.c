/**
 * @file thumbcache.c
 * @brief Resident title-card art for the grid.
 * @ingroup library
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "menu/image_decoder.h"
#include "thumbcache.h"
#include "ui/theme.h"
#include "utils/fs.h"

#define METADATA_DIR    "menu/metadata"
#define ART_FILE        "boxart_front.png"

/* The asset spec asks authors for 280 x 196, but the corpus people actually use ranges from
 * 112 px to 1020 px wide and a quarter of it is portrait, so the decoder scales and
 * cover-crops on the way in rather than requiring any particular source size. See
 * image_decoder_start_scaled() and docs/AUDIT.md. */

typedef struct {
    uint16_t rom_id;
    bool     used;
    uint32_t last_wanted;   /**< frame counter, for eviction */
    surface_t *art;
} slot_t;

struct thumbcache_s {
    const char *storage;
    slot_t slots[THUMB_SLOTS];
    uint32_t clock;

    int decoding_slot;      /**< -1 when nothing is being decoded */
    /** No record is startable; the per-frame walk is skipped until something clears this. */
    bool idle;
    uint16_t decoding_id;

    /** Whether menu/metadata exists at all: -1 unknown, 0 absent, 1 present. Probed once.
     *  Without this a card carrying no art pack pays three filesystem probes per title to
     *  discover three times over that a directory it does not have is still not there --
     *  1,500 stats on a 500-title library, all of them answerable by one. */
    int8_t metadata_dir;

    int resident;
    uint32_t decoded_count;
    uint32_t decoded_us;

    /* Records asked for this frame that are not resident yet. The decoder works through these
     * before it prefetches anything, because otherwise it walks the library from index 0 and a
     * user who scrolls to the end of a 500-title card waits for 500 decodes of tiles that are
     * nowhere near the screen. */
    uint16_t wanted[THUMB_SLOTS * 2];
    int wanted_n;
};

/* Only one decode can be in flight because image_decoder is a single global instance. That is
 * left alone deliberately: the job here is inherently serial and a decoder pool would add
 * concurrency to something bounded by CPU, not by latency. */
static thumbcache_t *active;

thumbcache_t *thumbcache_init (const char *storage_prefix) {
    thumbcache_t *tc = calloc(1, sizeof(thumbcache_t));
    if (tc == NULL) {
        return NULL;
    }
    tc->storage = storage_prefix;
    tc->decoding_slot = -1;
    tc->metadata_dir = -1;      /* unknown until the first record needs it */
    for (int i = 0; i < THUMB_SLOTS; i++) {
        tc->slots[i].rom_id = 0xFFFF;
    }
    active = tc;
    return tc;
}

void thumbcache_free (thumbcache_t *tc) {
    if (tc == NULL) {
        return;
    }
    if (tc->decoding_slot >= 0) {
        image_decoder_abort();
    }
    for (int i = 0; i < THUMB_SLOTS; i++) {
        if (tc->slots[i].art != NULL) {
            surface_free(tc->slots[i].art);
            free(tc->slots[i].art);
        }
    }
    if (active == tc) {
        active = NULL;
    }
    free(tc);
}

int thumbcache_resident (const thumbcache_t *tc) {
    return tc->resident;
}

void thumbcache_begin_frame (thumbcache_t *tc) {
    tc->clock++;
    tc->wanted_n = 0;
}

static int find_slot (thumbcache_t *tc, uint16_t rom_id) {
    for (int i = 0; i < THUMB_SLOTS; i++) {
        if (tc->slots[i].used && tc->slots[i].rom_id == rom_id) {
            return i;
        }
    }
    return -1;
}

surface_t *thumbcache_get (thumbcache_t *tc, library_t *lib, uint16_t rom_id) {
    (void)lib;
    int s = find_slot(tc, rom_id);
    if (s >= 0) {
        tc->slots[s].last_wanted = tc->clock;
        return tc->slots[s].art;
    }

    /* Not resident: remember that the screen wanted it, so the decoder does this one next. */
    if (tc->wanted_n < (int)(sizeof(tc->wanted) / sizeof(tc->wanted[0]))) {
        for (int i = 0; i < tc->wanted_n; i++) {
            if (tc->wanted[i] == rom_id) {
                return NULL;
            }
        }
        tc->wanted[tc->wanted_n++] = rom_id;
        tc->idle = false;              /* something on screen is missing; there is work to do */
    }
    return NULL;
}

/** @brief The filename part of @p path, without directories. */
static const char *basename_of (const char *path) {
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

/**
 * @brief Find this record's art, and return its size in bytes, or -1 if there is none.
 *
 * Five places are consulted, and the order is chosen so the ones that cost nothing come first.
 * The two index lookups are pure memory -- the scan already noticed every PNG in the tree -- so
 * they are free, while each metadata candidate is a filesystem probe on a cold FatFs.
 *
 *   1. a loose PNG named for the game code, anywhere under the scanned root  (NGEE.png)
 *   2. a loose PNG named for the ROM itself                 (Super Mario World (U) [!].png)
 *   3. menu/metadata/N/G/E/E/boxart_front.png               upstream's layout, unchanged
 *   4. menu/metadata/N/G/E/boxart_front.png                 upstream's region-agnostic fallback
 *   5. menu/metadata/NGEE.png                               flat, for hand-dropped art
 *
 * A loose file outranks the metadata tree deliberately. The tree is a bulk pack somebody
 * downloaded; a PNG the user put next to their ROM is a decision, and a decision should win.
 *
 * Rule 2 is the only one that does anything for NES, SNES, GB, GBC or SMS titles. They have no
 * N64 game code -- nothing sets one -- so every other rule here is dead for them, and before
 * this they could never show art at all no matter what was on the card.
 *
 * The winner is remembered in rec->art_file, so this walk happens once per record rather than
 * once per pass.
 */
static int64_t art_resolve (thumbcache_t *tc, const library_t *lib, lib_record_t *rec,
                            char *out, size_t cap) {
    if (rec->art_file != NULL) {
        snprintf(out, cap, "%s", rec->art_file);
        thumb_statcalls++;
        return file_get_size(out);
    }

    const char *hit = NULL;
    bool has_code = (rec->game_code[0] != '\0');

    if (has_code) {
        hit = library_find_art(lib, rec->game_code);
    }
    if (hit == NULL && rec->path != NULL) {
        hit = library_find_art(lib, basename_of(rec->path));
    }
    if (hit != NULL) {
        snprintf(out, cap, "%s", hit);
        thumb_statcalls++;
        int64_t bytes = file_get_size(out);
        if (bytes >= 0) {
            rec->art_file = strdup(out);
            debugf("ART %s: loose %s\n", rec->game_code[0] ? rec->game_code : "----", out);
            return bytes;
        }
    }

    if (!has_code) {
        debugf("ART ----: none for %s\n", rec->path ? basename_of(rec->path) : "?");
        return -1;
    }

    if (tc->metadata_dir < 0) {
        char probe[512];
        snprintf(probe, sizeof(probe), "%s%s", tc->storage, METADATA_DIR);
        dir_t d;
        tc->metadata_dir = (dir_findfirst(probe, &d) == 0) ? 1 : 0;
        debugf("ART: metadata dir %s\n", tc->metadata_dir ? "present" : "absent");
    }
    if (tc->metadata_dir == 0) {
        return -1;
    }

    const char *c = rec->game_code;
    for (int candidate = 0; candidate < 3; candidate++) {
        switch (candidate) {
            case 0:
                snprintf(out, cap, "%s%s/%c/%c/%c/%c/%s", tc->storage, METADATA_DIR,
                         c[0], c[1], c[2], c[3], ART_FILE);
                break;
            case 1:
                snprintf(out, cap, "%s%s/%c/%c/%c/%s", tc->storage, METADATA_DIR,
                         c[0], c[1], c[2], ART_FILE);
                break;
            default:
                snprintf(out, cap, "%s%s/%s.png", tc->storage, METADATA_DIR, c);
                break;
        }
        thumb_statcalls++;
        int64_t bytes = file_get_size(out);
        if (bytes >= 0) {
            rec->art_file = strdup(out);
            debugf("ART %s: metadata rule %d, %s\n", c, candidate, out);
            return bytes;
        }
    }

    debugf("ART %s: none\n", c);
    return -1;
}

/**
 * @brief The tile's representative colour, for the ambient wash behind the grid.
 *
 * A mean over every pixel, biased toward saturated ones. A flat mean is the wrong answer for box
 * art: most cards are a large dark or white field around a small vivid logo, and averaging that
 * gives grey every time -- the same grey for every game in the library, which is worse than not
 * having the feature.
 *
 * Weighting by (max channel - min channel) makes the logo win over the background without
 * needing a histogram or a median cut. It is one pass over 13,720 pixels with no allocation,
 * which is what lets it run inside the decode callback rather than as another budgeted job.
 */
static uint16_t dominant_colour (const surface_t *s) {
    const uint16_t *px = (const uint16_t *)s->buffer;
    int stride = s->stride / 2;

    uint32_t wr = 0, wg = 0, wb = 0, total = 0;

    /* Every fourth pixel in both axes: 858 samples instead of 13,720. The answer is a single
     * RGBA5551 colour used for a 15 %-alpha wash, so sixteen times the work buys nothing you
     * could see. */
    for (int y = 0; y < s->height; y += 4) {
        for (int x = 0; x < s->width; x += 4) {
            uint16_t c = px[y * stride + x];
            uint32_t r = (c >> 11) & 0x1F;
            uint32_t g = (c >> 6) & 0x1F;
            uint32_t b = (c >> 1) & 0x1F;

            uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            uint32_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            uint32_t w = (mx - mn) + 1;      /* +1 so a fully grey card still contributes */

            wr += r * w;
            wg += g * w;
            wb += b * w;
            total += w;
        }
    }

    if (total == 0) {
        return 0;
    }
    uint32_t r = wr / total, g = wg / total, b = wb / total;
    return (uint16_t)((r << 11) | (g << 6) | (b << 1) | 1);
}

static void decode_done (img_err_t err, surface_t *decoded, void *data) {
    thumbcache_t *tc = active;
    lib_record_t *rec = data;

    if (tc == NULL || tc->decoding_slot < 0) {
        if (decoded) { surface_free(decoded); free(decoded); }
        return;
    }

    slot_t *slot = &tc->slots[tc->decoding_slot];

    if (err != IMG_OK || decoded == NULL) {
        rec->art_state = ART_NONE;
        slot->used = false;
        tc->decoding_slot = -1;
        if (decoded) { surface_free(decoded); free(decoded); }
        return;
    }

    rec->dominant = dominant_colour(decoded);
    rec->art_age = 0.0f;          /* arms the arrival pop; see screen_grid draw_tile */

    /* The scaler already produced exactly TILE_W x TILE_H, so the surface is taken as-is. */
    slot->art = decoded;
    slot->used = true;
    rec->art_state = ART_READY;
    tc->resident++;
    tc->decoding_slot = -1;
    if ((tc->decoded_count % 8) == 0) {
        debugf("THUMB %lu decoded, %lu us total, %lu us each, resident=%d\n",
               (unsigned long)tc->decoded_count, (unsigned long)tc->decoded_us,
               (unsigned long)(tc->decoded_us / tc->decoded_count), tc->resident);
    }
}

/** @brief Pick a slot for @p rom_id: a free one, else the least-recently-wanted. */
static int claim_slot (thumbcache_t *tc, library_t *lib, uint16_t rom_id) {
    int best = -1;
    uint32_t oldest = 0xFFFFFFFF;

    for (int i = 0; i < THUMB_SLOTS; i++) {
        if (!tc->slots[i].used && tc->slots[i].art == NULL) {
            return i;
        }
    }
    for (int i = 0; i < THUMB_SLOTS; i++) {
        /* Never evict what is wanted this frame, or a working set larger than the pool would
         * thrash: every tile would evict the one drawn just before it and nothing would ever
         * stay resident. */
        if (tc->slots[i].last_wanted >= tc->clock) {
            continue;
        }
        if (tc->slots[i].last_wanted < oldest) {
            oldest = tc->slots[i].last_wanted;
            best = i;
        }
    }
    if (best >= 0 && tc->slots[best].art != NULL) {
        /* Put the evicted record back in the queue. Without this it keeps ART_READY while its
         * surface is gone, so thumbcache_get() misses, adds it to wanted[], and thumbcache_run()
         * skips it for not being ART_PENDING -- an evicted tile could never come back, and with
         * 56 records against 20 slots that meant scrolling away and back left permanent blanks.
         *
         * ART_PENDING rather than ART_COSTLY even if it was costly before: the size probe is one
         * stat and re-running it is cheaper than carrying a second "was costly" bit that can go
         * stale when the file changes underneath us. */
        uint16_t evicted = tc->slots[best].rom_id;
        if (lib != NULL && evicted < lib->count) {
            lib->records[evicted].art_state = ART_PENDING;
        }
        surface_free(tc->slots[best].art);
        free(tc->slots[best].art);
        tc->slots[best].art = NULL;
        tc->slots[best].used = false;
        tc->resident--;
        tc->idle = false;                  /* there is work again */
    }
    (void)rom_id;
    return best;
}

/* Above this, a card is decoded only once every cheaper one is done. 400 KB is drawn from the
 * corpus rather than picked: the stratified sample runs 8 KB to 3.4 MB, and 400 KB separates the
 * ordinary 680x498 and 1000x684 scans -- a second or two each -- from the handful of oversized
 * ones that cost tens of seconds. It bounds how long the queue can be blocked, nothing more. */
#define THUMB_CHEAP_BYTES   (400 * 1024)

/* Split so the two costs can be told apart. bg_us said 9,000-12,000 us per frame; whether that
 * is rows of PNG or the walk that looks for the next image to start is not a detail, because
 * only one of them is real work. */
uint32_t thumb_rows_us = 0, thumb_scan_us = 0, thumb_starts = 0, thumb_statcalls = 0;

bool thumbcache_run (thumbcache_t *tc, library_t *lib, uint32_t budget_us) {
    if (tc->decoding_slot >= 0) {
        uint32_t t0 = TICKS_READ();
        image_decoder_poll_budget(budget_us);
        uint32_t took = TIMER_MICROS(TICKS_SINCE(t0));
        tc->decoded_us += took;
        thumb_rows_us += took;
        return true;
    }
    /* Nothing pending anywhere: skip the walk entirely.
     *
     * Measured on a settled 14-minute session: the spin loop calls background() 208 times per
     * displayed frame, and each call walked all four passes over the whole library finding
     * nothing -- about 46,600 record visits and 9,409 us per frame of pure waste. It cost no
     * frames, because there was slack to waste, which is exactly why it went unnoticed.
     *
     * The flag is cleared by anything that can create work: a non-resident tile being asked for,
     * or an eviction putting a record back in the queue. */
    if (tc->idle && tc->wanted_n == 0) {
        return false;
    }

    uint32_t scan_t0 = TICKS_READ();

    /* Nothing in flight. Four passes, in order: cheap-and-visible, cheap-and-anywhere,
     * expensive-and-visible, expensive-and-anywhere.
     *
     * The size split is not a nicety. The corpus contains a 2118 x 1457 card, and inflate is
     * sequential so there is no way to decode less of it than all of it: 1,457 rows at ~26,000 us
     * each is 38 SECONDS for one tile. The decoder is single-instance, so while that one card
     * decodes no other tile can, and measured on this fixture the entire grid sat empty for 900
     * frames because the second visible tile happened to be that card.
     *
     * Before the OOM fix in image_decoder.c this was invisible: the card asked for a 6.17 MB
     * surface, failed instantly and was marked ART_NONE, so it cost nothing and showed nothing.
     * Making it decodable is what exposed that it monopolises the decoder. Doing the cheap ones
     * first turns "no art for 38 seconds" into "eleven tiles quickly and one that lands late". */
    for (int pass = 0; pass < 4; pass++) {
        /* Visibility outranks cost: a tile on screen always beats one that is not, and the size
         * split only breaks ties within a visibility class. Ordering these the other way round
         * -- both cheap passes before either costly one -- filled four visible tiles and then
         * went off to prefetch art the user could not see, leaving eight visible tiles blank. */
        bool visible_only = (pass < 2);
        bool allow_costly = ((pass & 1) != 0);
        int n = visible_only ? tc->wanted_n : lib->count;
        for (int k = 0; k < n; k++) {
            int i = visible_only ? (int)tc->wanted[k] : k;
            if (i < 0 || i >= lib->count) {
                continue;
            }
            lib_record_t *rec = &lib->records[i];
            /* Cheap passes take untried cards; costly passes take the ones a cheap pass already
             * measured and set aside. Probing size by state rather than by re-stat'ing matters:
             * the first version re-ran file_get_size on every candidate on every pass, which cost
             * 180 filesystem probes and 6,437 us per frame and produced 100 ms frames -- worse
             * than the problem it was added to fix. */
            uint8_t want_state = allow_costly ? ART_COSTLY : ART_PENDING;
            if (rec->art_state != want_state) {
                continue;
            }

            char path[512];
            int64_t art_bytes = art_resolve(tc, lib, rec, path, sizeof(path));
            if (art_bytes < 0) {
                rec->art_state = ART_NONE;    /* no art for this game; settled, never retried */
                continue;
            }
            if (!allow_costly && art_bytes > THUMB_CHEAP_BYTES) {
                rec->art_state = ART_COSTLY;  /* measured once; a later pass will take it */
                continue;
            }

            int slot = claim_slot(tc, lib, (uint16_t)i);
            if (slot < 0) {
                thumb_scan_us += TIMER_MICROS(TICKS_SINCE(scan_t0));
                return false;       /* pool full of tiles wanted right now; try next frame */
            }

            img_err_t perr = image_decoder_start_scaled(path, TILE_W, TILE_H, decode_done, rec);
            if (perr != IMG_OK) {
                debugf("THUMB %d: image_decoder_start_scaled(%s) = %d\n", i, path, (int)perr);
                rec->art_state = ART_NONE;
                continue;
            }

            tc->slots[slot].rom_id = (uint16_t)i;
            tc->slots[slot].last_wanted = tc->clock;
            tc->decoding_slot = slot;
            tc->decoding_id = (uint16_t)i;
            rec->art_state = ART_DECODING;
            tc->decoded_count++;
            thumb_starts++;
            thumb_scan_us += TIMER_MICROS(TICKS_SINCE(scan_t0));
            return true;
        }
    }

    /* Four full passes found nothing startable. Stay quiet until something changes. */
    tc->idle = true;
    thumb_scan_us += TIMER_MICROS(TICKS_SINCE(scan_t0));
    return false;
}
