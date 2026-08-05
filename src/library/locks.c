/**
 * @file locks.c
 * @brief The shared padlock list. See locks.h for why it is not in playstate.dat.
 * @ingroup library
 */

#include <stdlib.h>
#include <libdragon.h>

#include "cache.h"
#include "locks.h"
#include "playstate.h"

#define LOCKS_FILE  "locks.dat"

/* Bare keys, no record struct: there is nothing to say about a locked game except that it is one.
 * Asserted anyway, on the same principle as every other on-disk size here -- a file whose stride
 * is wrong still passes magic, version and CRC, and then reads back as a list of games nobody
 * locked. */
_Static_assert(sizeof(uint64_t) == 8, "locks records must stay 8 bytes");

static bool dirty;

void locks_touch (void) {
    dirty = true;
}

bool locks_dirty (void) {
    return dirty;
}

void locks_load (library_t *lib) {
    void *buf = NULL;
    uint32_t bytes = 0;

    if (!cache_load(LOCKS_FILE, LOCKS_MAGIC, &buf, &bytes)) {
        /* No file. Either nothing has ever been locked, or this is a card written before this
         * file existed and playstate_load() has already put the locks onto the records. Those two
         * are told apart by looking, and only the second one needs a write. */
        dirty = false;
        for (int i = 0; i < lib->count; i++) {
            if (lib->records[i].flags & LIBF_LOCKED) {
                dirty = true;
                debugf("LOCKS none on disk, but the library has locked games: migrating\n");
                break;
            }
        }
        return;
    }

    int n = (int)(bytes / sizeof(uint64_t));
    const uint64_t *keys = buf;
    int applied = 0;

    /* Same shape and the same reasoning as playstate_load(): 500 titles against at most a few
     * hundred locks is a few hundred thousand 64-bit compares, once, at boot. */
    for (int i = 0; i < lib->count; i++) {
        uint64_t key = playstate_key(&lib->records[i]);
        for (int j = 0; j < n; j++) {
            if (keys[j] == key) {
                lib->records[i].flags |= LIBF_LOCKED;
                applied++;
                break;
            }
        }
    }

    free(buf);
    dirty = false;
    debugf("LOCKS loaded %d keys, %d matched the library\n", n, applied);
}

bool locks_save (const library_t *lib) {
    if (!cache_writable()) {
        return false;
    }

    int n = 0;
    for (int i = 0; i < lib->count; i++) {
        if (lib->records[i].flags & LIBF_LOCKED) {
            n++;
        }
    }

    if (n == 0) {
        /* Nothing locked. Drop the file rather than leave one behind, or the next boot loads a
         * list that re-locks games the user just cleared -- the same trap playstate_save() has. */
        cache_drop(LOCKS_FILE);
        dirty = false;
        return true;
    }

    uint64_t *keys = calloc(n, sizeof(uint64_t));
    if (keys == NULL) {
        return false;
    }

    int w = 0;
    for (int i = 0; i < lib->count && w < n; i++) {
        if (lib->records[i].flags & LIBF_LOCKED) {
            keys[w++] = playstate_key(&lib->records[i]);
        }
    }

    bool ok = cache_store(LOCKS_FILE, LOCKS_MAGIC, keys, (uint32_t)(w * sizeof(uint64_t)));
    free(keys);

    if (ok) {
        dirty = false;
    }
    return ok;
}
