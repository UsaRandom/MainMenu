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
#include "menu/paths.h"
#include "thumbcache.h"
#include "thumbstore.h"
#include "ui/theme.h"
#include "utils/fs.h"

/* Build with -DTHUMB_AB_PREFIX=1 to restore the pre-fix cache policy: prefetch evicts, and a
 * record with no art is wanted every frame. It exists so a regression run can produce a real
 * "before" against the same fixture -- the last full run predates an art refetch, so diffing
 * against it would attribute the corpus change to this one. Never set in any build that ships;
 * see AUDIT.md 1u for what the two behaviours cost. */
#ifndef THUMB_AB_PREFIX
#define THUMB_AB_PREFIX 0
#endif

#define METADATA_DIR    "metadata"
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
    /** Source path and size of the decode in flight. decode_done() needs both to key the atlas
     *  write, and by then art_resolve() has long returned. */
    char     decoding_src[512];
    int64_t  decoding_bytes;
    /** No record is startable; the per-frame walk is skipped until something clears this. */
    bool idle;
    uint16_t decoding_id;

    /** Whether an art pack exists at all: -1 unknown, 0 absent, 1 present. Probed once.
     *  Without this a card carrying no art pack pays three filesystem probes per title to
     *  discover three times over that a directory it does not have is still not there --
     *  1,500 stats on a 500-title library, all of them answerable by one. */
    int8_t metadata_dir;

    /** Which root the pack turned out to be under, resolved with the line above. Held rather
     *  than recomputed because it is on the path of every title that misses a loose image. */
    char metadata_root[300];

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

/** The library thumbcache_run() is working against. decode_done() is a callback from the image
 *  decoder and is handed only the record, but it needs the library to mark the index stale. */
static library_t *active_lib;

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
    int s = find_slot(tc, rom_id);
    if (s >= 0) {
        tc->slots[s].last_wanted = tc->clock;
        return tc->slots[s].art;
    }

    /* A record with no art can never become resident, so wanting it is a request that can never
     * be met -- and the want is what clears tc->idle. One artless tile on screen therefore kept
     * the whole four-pass walk armed for every background() call forever: measured at 13,418 us
     * per frame of scan and 421 filesystem probes a second on a settled grid whose every visible
     * tile was already either resident or known to have no art. The eleven emulated-system stubs
     * in the fixture are all artless, so a single screenful contained several. */
    if (!THUMB_AB_PREFIX &&
        lib != NULL && rom_id < lib->count && lib->records[rom_id].art_state == ART_NONE) {
        return NULL;
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
            library_touch((library_t *)lib);   /* worth writing back; see library_t::dirty */
            debugf("ART %s: loose %s\n", rec->game_code[0] ? rec->game_code : "----", out);
            return bytes;
        }
    }

    if (!has_code) {
        debugf("ART ----: none for %s\n", rec->path ? basename_of(rec->path) : "?");
        return -1;
    }

    if (tc->metadata_dir < 0) {
        /* Probed across the three roots once, then remembered: an art pack downloaded as a zip
         * and emptied onto the card lands at /metadata, and requiring it to be moved is the kind
         * of preparation this menu exists not to ask for. See menu/paths.h. */
        tc->metadata_dir = menu_find_dir(tc->metadata_root, sizeof(tc->metadata_root),
                                         tc->storage, METADATA_DIR) ? 1 : 0;
        debugf("ART: metadata dir %s (%s)\n",
               tc->metadata_dir ? "present" : "absent", tc->metadata_root);
    }
    if (tc->metadata_dir == 0) {
        return -1;
    }

    const char *c = rec->game_code;
    for (int candidate = 0; candidate < 3; candidate++) {
        switch (candidate) {
            case 0:
                snprintf(out, cap, "%s/%c/%c/%c/%c/%s", tc->metadata_root,
                         c[0], c[1], c[2], c[3], ART_FILE);
                break;
            case 1:
                snprintf(out, cap, "%s/%c/%c/%c/%s", tc->metadata_root,
                         c[0], c[1], c[2], ART_FILE);
                break;
            default:
                snprintf(out, cap, "%s/%s.png", tc->metadata_root, c);
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

    /* Pay the 32 KB write once so no future boot pays the 259,633 us decode again. Deliberately
     * synchronous and deliberately here: we have just spent a quarter of a second on this image,
     * so an 11 ms append is 4 % on the cold path and 100 % of the saving on every warm one. */
    thumbstore_put(tc->decoding_src, tc->decoding_bytes, decoded, rec->dominant);
    /* Publish every eight tiles. The pak is appended to as we go, but a tile the index has not
     * recorded is a tile nobody can find -- so a user who fills a cold grid and then power-cycles
     * without launching anything would throw away every decode. Eight bounds that loss to about
     * two seconds of work while keeping the index rewrite off the per-tile path. */
    if ((thumbstore_count() % 8) == 0) {
        thumbstore_flush();
    }

    /* dominant is derived from the decoded pixels and is worth carrying in the index too: it
     * drives the ambient wash, which would otherwise be flat until each tile finished decoding. */
    if (active_lib != NULL) {
        library_touch(active_lib);
    }

    tc->decoding_slot = -1;
    if ((tc->decoded_count % 8) == 0) {
        debugf("THUMB %lu decoded, %lu us total, %lu us each, resident=%d\n",
               (unsigned long)tc->decoded_count, (unsigned long)tc->decoded_us,
               (unsigned long)(tc->decoded_us / tc->decoded_count), tc->resident);
    }
}

/**
 * @brief Pick a slot for @p rom_id: a free one, else the least-recently-wanted.
 *
 * @p may_evict is false for the prefetch passes, and that is what makes the cache ever stop
 * working. Prefetch walks the whole library, and an eviction hands the evicted record back to
 * the queue as ART_PENDING -- so with more titles than slots there was always another candidate
 * and the pool decoded and evicted forever. `tools/inputs/idle.txt` measured it on the SD card's
 * 27-title library: 28 mallocs and 28 frees per 60 frames, 1,200 frames after the last input,
 * permanent background CPU and unbounded churn of 27 KB surfaces against a heap with no MMU
 * behind it. Every fixture before that one fitted inside the pool, so the gate had never been
 * able to go red. See AUDIT.md 1u.
 *
 * Visible tiles still evict, because the alternative is a blank tile under the cursor. Prefetch
 * only ever fills what is already free, so a full pool ends the walk instead of restarting it.
 */
static int claim_slot (thumbcache_t *tc, library_t *lib, uint16_t rom_id, bool may_evict) {
    int best = -1;
    uint32_t oldest = 0xFFFFFFFF;

    for (int i = 0; i < THUMB_SLOTS; i++) {
        if (!tc->slots[i].used && tc->slots[i].art == NULL) {
            return i;
        }
    }
    if (!may_evict && !THUMB_AB_PREFIX) {
        return -1;
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

/**
 * @brief The pool had no slot to give. Charge the scan and decide whether to go quiet.
 *
 * The two callers differ only in which pass they are in, and the distinction is the whole fix.
 * A visible pass that cannot claim means every slot is wanted *this frame*; the clock moves on,
 * so next frame it may well succeed and the walk must stay armed. A prefetch pass that cannot
 * claim means the pool is simply full, and since prefetch never evicts, no later pass and no
 * later frame can do better until something frees a slot -- which only an eviction or a new
 * want does, and both clear the flag.
 *
 * A cached-only pass never goes quiet either, whichever pass it is in: it declines to decode, so
 * "nothing more I can do" is a statement about this mode and not about the queue. Letting it set
 * the flag would silence the full run that follows the scroll and leave the grid unfilled until
 * the next want cleared it.
 */
static bool no_slot (thumbcache_t *tc, bool may_go_idle, uint32_t scan_t0) {
    if (may_go_idle) {
        tc->idle = true;
    }
    thumb_scan_us += TIMER_MICROS(TICKS_SINCE(scan_t0));
    return false;
}

/**
 * @brief One unit of work: poll a decode in flight, or start/fetch exactly one tile.
 *
 * @p cached_only restricts it to the atlas -- fetch what is already in thumbs.pak and never open
 * a PNG. That is the mode the grid uses while the cursor is moving; see thumbcache_run_cached().
 */
static bool run_once (thumbcache_t *tc, library_t *lib, uint32_t budget_us, bool cached_only) {
    if (tc->decoding_slot >= 0) {
        /* Cached mode does NOT advance the decode -- that is the work it exists to avoid -- and
         * it does not walk either. It returns, and the walk is the reason.
         *
         * The first version fell through here, on the reasoning that an atlas fetch goes nowhere
         * near the decoder so a scroll beginning mid-decode could still fill from thumbs.pak.
         * True, and ruinous: this early-out is what kept the four-pass walk from running during a
         * cold fill, and a cold fill is a decode in flight almost continuously. Removing it put a
         * full walk -- a filesystem probe per candidate -- into every background() call, and
         * background() runs once per frame plus once per spin iteration, of which AUDIT.md 1l
         * measured 208 per displayed frame. The card came back visibly slower at everything:
         * music, input, scrolling. See 1ae.
         *
         * Nothing is lost that was ever gained. A decode is in flight only while the cache is
         * cold, and a cold cache has nothing in the atlas to fetch. */
        if (cached_only) {
            return false;
        }
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

    active_lib = lib;

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
        /* Cached mode never leaves the wanted list. The two whole-library passes exist to decode
         * ahead of the screen, and this mode does not decode -- so on a large card they would be
         * a walk of every record, and the stat inside art_resolve() on each, to reach a decision
         * that was already made. The rows either side of the window are in wanted[] already;
         * screen_grid asks for them. */
        if (cached_only && !visible_only) {
            continue;
        }
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

            /* Cached mode only takes records whose art has already been located. art_resolve()
             * falls back to a five-rule search with up to three filesystem probes, and paying
             * that during a scroll -- for a record that by definition has never been decoded, so
             * cannot be in the atlas -- is the expensive half of a question whose answer is
             * always no. A warm card has every path in library.idx, which is the case this mode
             * is for. */
            if (cached_only && rec->art_file == NULL) {
                continue;
            }

            char path[512];
            int64_t art_bytes = art_resolve(tc, lib, rec, path, sizeof(path));
            if (art_bytes < 0) {
                rec->art_state = ART_NONE;    /* no art for this game; settled, never retried */
                library_touch(lib);           /* "there is none" is the expensive answer to cache */
                continue;
            }
            /* The atlas is checked BEFORE the cost gate, and that ordering is the point. A
             * cached tile is 27 KB off the card no matter how ruinous its source was to decode,
             * so the 2118 x 1457 card that monopolises the decoder for 38 seconds becomes an
             * ordinary tile the moment it has been decoded once. Deferring a cached tile to the
             * costly pass would throw that away for no reason. */
            /* Ask the index BEFORE claiming a slot or allocating anything.
             *
             * This block used to claim a slot, malloc a surface_t, surface_alloc 27,440 bytes,
             * attempt the fetch, and unwind all of it on a miss -- per candidate, per pass, per
             * call. On a cold card every candidate misses, so a walk of a screenful was a dozen
             * whole-tile allocations and frees that could never succeed. Worse, claim_slot() may
             * EVICT to hand back a slot, so a full pool would destroy a resident tile in order to
             * fail to fill the space with a tile that was never in the atlas.
             *
             * thumbstore_has() is a hash and a scan of the resident index. It answers the same
             * question for nothing. See AUDIT.md 1ae. */
            if (thumbstore_available() && thumbstore_has(path, art_bytes)) {
                int slot = claim_slot(tc, lib, (uint16_t)i, visible_only);
                if (slot < 0) {
                    return no_slot(tc, !visible_only && !cached_only, scan_t0);
                }
                surface_t *cached = malloc(sizeof(surface_t));
                if (cached != NULL) {
                    *cached = surface_alloc(FMT_RGBA16, TILE_W, TILE_H);
                    uint16_t dom = 0;
                    if (cached->buffer != NULL &&
                        thumbstore_fetch(path, art_bytes, cached, &dom)) {
                        tc->slots[slot].rom_id = (uint16_t)i;
                        tc->slots[slot].last_wanted = tc->clock;
                        tc->slots[slot].art = cached;
                        tc->slots[slot].used = true;
                        tc->resident++;
                        rec->dominant = dom;
                        rec->art_state = ART_READY;
                        rec->art_age = 0.0f;        /* still pops in; it is new to the screen */
                        thumb_scan_us += TIMER_MICROS(TICKS_SINCE(scan_t0));
                        return true;
                    }
                    surface_free(cached);
                    free(cached);
                }
                /* Miss. The slot was claimed but nothing was put in it, so release it before
                 * falling through -- otherwise a cold library leaks one slot per tile and the
                 * pool is exhausted after twenty misses. */
                tc->slots[slot].used = false;
                tc->slots[slot].rom_id = 0xFFFF;
            }

            /* Not in the atlas, so the only way to get it is to decode it -- which is exactly
             * what this mode exists not to do. Left ART_PENDING so the full run picks it up the
             * moment the cursor settles. */
            if (cached_only) {
                continue;
            }

            if (!allow_costly && art_bytes > THUMB_CHEAP_BYTES) {
                rec->art_state = ART_COSTLY;  /* measured once; a later pass will take it */
                continue;
            }

            int slot = claim_slot(tc, lib, (uint16_t)i, visible_only);
            if (slot < 0) {
                return no_slot(tc, !visible_only, scan_t0);
            }

            snprintf(tc->decoding_src, sizeof(tc->decoding_src), "%s", path);
            tc->decoding_bytes = art_bytes;
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

    /* Four full passes found nothing startable. Stay quiet until something changes -- unless this
     * was a cached-only pass, which declined to decode and therefore has not established that
     * there is nothing to do. */
    if (!cached_only) {
        tc->idle = true;
    }
    thumb_scan_us += TIMER_MICROS(TICKS_SINCE(scan_t0));
    return false;
}

bool thumbcache_run (thumbcache_t *tc, library_t *lib, uint32_t budget_us) {
    return run_once(tc, lib, budget_us, false);
}

bool thumbcache_run_cached (thumbcache_t *tc, library_t *lib, uint32_t budget_us) {
    if (!thumbstore_available()) {
        return false;
    }
    /* Keep going while there is budget, because one tile per call is not enough to keep up with
     * a scroll: a row is four tiles and the cursor crosses one about every twelve frames on a
     * held direction. One fetch per frame fills a row in four -- by which time the row after it
     * is on screen and the grid is permanently one row behind the eye. */
    bool any = false;
    uint32_t t0 = TICKS_READ();
    while (TIMER_MICROS(TICKS_SINCE(t0)) < budget_us) {
        if (!run_once(tc, lib, 0, true)) {
            break;
        }
        any = true;
    }
    return any;
}
