/**
 * @file playstate.c
 * @brief Persistent favourites and play history. See playstate.h for why it is a separate file.
 * @ingroup library
 */

#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "cache.h"
#include "playstate.h"

#define PLAYSTATE_FILE  "playstate.dat"

/** @brief One remembered game. 24 bytes, naturally aligned, no padding to guess at. */
typedef struct __attribute__((packed)) {
    uint64_t key;
    uint32_t last_played;
    uint32_t play_count;
    uint16_t flags;         /**< the persistable subset of lib_flags_t */
    uint16_t reserved;
    uint32_t reserved2;
} ps_record_t;

/** Which record flags are the user's rather than the card's. LIBF_HAS_SAVE is derived from a file
 *  on disk and LIBF_NO_MATCH from the database, so persisting either would let a stale cache
 *  contradict what a rescan just found. The favourite and the parental lock are both genuinely
 *  the user's, and both must survive a rescan -- which is exactly what keying on the ROM header
 *  rather than the path buys. Moving a locked game to another folder must not unlock it. */
#define PS_PERSIST_FLAGS  (LIBF_FAVORITE | LIBF_LOCKED)

/* Every on-disk struct's size is asserted, because the one bug this whole family of files
 * cannot survive is silent padding. A compiler that inserts two bytes somewhere writes a
 * file that passes its own magic, version and CRC and is then read back with every field
 * after the padding shifted -- which looks like corrupt data from a working card. __packed
 * should prevent it; this is what proves it did. */
_Static_assert(sizeof(ps_record_t) == 24, "playstate record must stay 24 bytes");

static bool dirty;

void playstate_touch (void) {
    dirty = true;
}

bool playstate_dirty (void) {
    return dirty;
}

uint64_t playstate_key (const lib_record_t *rec) {
    if (rec->check_code != 0) {
        return rec->check_code;
    }
    /* No usable header -- every SNES, NES, GB and SMS title, because only N64 headers are
     * parsed. Hash the filename rather than the full path, so moving a game between folders
     * still finds its history. Two identically-named ROMs in different folders will collide and
     * share a favourite; that is a better failure than losing the favourite on every move. */
    const char *p = rec->path ? rec->path : "";
    const char *slash = strrchr(p, '/');
    return cache_hash64(slash ? slash + 1 : p);
}

void playstate_load (library_t *lib) {
    void *buf = NULL;
    uint32_t bytes = 0;

    if (!cache_load(PLAYSTATE_FILE, PLAYSTATE_MAGIC, &buf, &bytes)) {
        return;
    }

    int n = (int)(bytes / sizeof(ps_record_t));
    const ps_record_t *recs = buf;
    int applied = 0;

    /* O(n*m) against 500 titles and at most a few hundred remembered games is a few hundred
     * thousand comparisons of a 64-bit integer -- microseconds, once, at boot. A hash table here
     * would be more code than it saves. */
    for (int i = 0; i < lib->count; i++) {
        uint64_t key = playstate_key(&lib->records[i]);
        for (int j = 0; j < n; j++) {
            if (recs[j].key != key) {
                continue;
            }
            lib->records[i].last_played = recs[j].last_played;
            lib->records[i].play_count  = recs[j].play_count;
            lib->records[i].flags      |= (recs[j].flags & PS_PERSIST_FLAGS);
            applied++;
            break;
        }
    }

    free(buf);
    dirty = false;
    debugf("PLAYSTATE loaded %d records, %d matched the library\n", n, applied);
}

bool playstate_save (const library_t *lib) {
    if (!cache_writable()) {
        return false;
    }

    int n = 0;
    for (int i = 0; i < lib->count; i++) {
        const lib_record_t *r = &lib->records[i];
        if ((r->flags & PS_PERSIST_FLAGS) || r->last_played != 0 || r->play_count != 0) {
            n++;
        }
    }

    if (n == 0) {
        /* Nothing worth remembering. Remove any older file rather than leave one that would be
         * loaded back on the next boot and re-favourite something the user just cleared. */
        cache_drop(PLAYSTATE_FILE);
        dirty = false;
        return true;
    }

    ps_record_t *recs = calloc(n, sizeof(ps_record_t));
    if (recs == NULL) {
        return false;
    }

    int w = 0;
    for (int i = 0; i < lib->count && w < n; i++) {
        const lib_record_t *r = &lib->records[i];
        if (!(r->flags & PS_PERSIST_FLAGS) && r->last_played == 0 && r->play_count == 0) {
            continue;
        }
        recs[w].key         = playstate_key(r);
        recs[w].last_played = r->last_played;
        recs[w].play_count  = r->play_count;
        recs[w].flags       = (uint16_t)(r->flags & PS_PERSIST_FLAGS);
        w++;
    }

    bool ok = cache_store(PLAYSTATE_FILE, PLAYSTATE_MAGIC, recs, (uint32_t)(w * sizeof(ps_record_t)));
    free(recs);

    if (ok) {
        dirty = false;
    }
    return ok;
}

void playstate_played (lib_record_t *rec, uint32_t now) {
    rec->last_played = now;
    rec->play_count++;
    dirty = true;
}
