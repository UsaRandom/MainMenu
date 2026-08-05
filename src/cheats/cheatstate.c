/**
 * @file cheatstate.c
 * @brief Persistent cheat selections. See cheatstate.h for why the key is the name.
 * @ingroup cheats
 */

#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "cheatstate.h"
#include "library/cache.h"
#include "menu/profile.h"

#define CHEATSTATE_FILE "cheatstate.dat"

/** @brief The file this profile writes: `cheatstate.dat`, or `p3/cheatstate.dat`.
 *
 *  Per profile because a cheat is a choice about how you want to play, not a fact about the card
 *  -- one player leaving infinite lives on must not turn them on for everybody else. The database
 *  the names come from, `cheats.db`, and any cheats the user has written themselves stay shared;
 *  see profile.h for the whole split. */
static void cs_file (char *out, size_t cap) {
    profile_cache_name(out, cap, CHEATSTATE_FILE);
}

/** @brief One enabled group. 16 bytes. */
typedef struct __attribute__((packed)) {
    uint64_t game_key;
    uint32_t name_hash;
    uint32_t reserved;
} cs_record_t;

/* Every on-disk struct's size is asserted, because the one bug this whole family of files
 * cannot survive is silent padding. A compiler that inserts two bytes somewhere writes a
 * file that passes its own magic, version and CRC and is then read back with every field
 * after the padding shifted -- which looks like corrupt data from a working card. __packed
 * should prevent it; this is what proves it did. */
_Static_assert(sizeof(cs_record_t) == 16, "cheatstate record must stay 16 bytes");

static cs_record_t *rows;
static int row_count;
static int row_cap;
static bool dirty;

bool cheatstate_dirty (void) {
    return dirty;
}

void cheatstate_free (void) {
    free(rows);
    rows = NULL;
    row_count = row_cap = 0;
    dirty = false;
}

void cheatstate_load (void) {
    cheatstate_free();

    void *buf = NULL;
    uint32_t bytes = 0;
    char file[64];
    cs_file(file, sizeof(file));

    if (!cache_load(file, CHEATSTATE_MAGIC, &buf, &bytes)) {
        return;
    }

    row_count = row_cap = (int)(bytes / sizeof(cs_record_t));
    rows = buf;             /* the payload IS the array; no copy, no second allocation */
    dirty = false;
    debugf("CHEATSTATE loaded %d enabled cheats\n", row_count);
}

int cheatstate_apply (cheatset_t *set, uint64_t game_key) {
    if (set == NULL || rows == NULL) {
        return 0;
    }

    int on = 0;
    for (int g = 0; g < set->group_count; g++) {
        if (set->groups[g].name == NULL) {
            continue;
        }
        uint32_t h = cache_hash32(set->groups[g].name);
        for (int i = 0; i < row_count; i++) {
            if (rows[i].game_key == game_key && rows[i].name_hash == h) {
                set->groups[g].enabled = true;
                on++;
                break;
            }
        }
    }
    return on;
}

/** @brief Drop every row belonging to @p game_key, preserving the order of the rest. */
static void forget_game (uint64_t game_key) {
    int w = 0;
    for (int i = 0; i < row_count; i++) {
        if (rows[i].game_key == game_key) {
            continue;
        }
        if (w != i) {
            rows[w] = rows[i];
        }
        w++;
    }
    if (w != row_count) {
        row_count = w;
        dirty = true;
    }
}

static bool ensure_room (int extra) {
    if (row_count + extra <= row_cap) {
        return true;
    }
    int want = row_cap ? row_cap * 2 : 32;
    while (want < row_count + extra) {
        want *= 2;
    }
    cs_record_t *bigger = realloc(rows, (size_t)want * sizeof(cs_record_t));
    if (bigger == NULL) {
        return false;
    }
    rows = bigger;
    row_cap = want;
    return true;
}

void cheatstate_capture (const cheatset_t *set, uint64_t game_key) {
    if (set == NULL) {
        return;
    }

    int on = 0;
    for (int g = 0; g < set->group_count; g++) {
        if (set->groups[g].enabled && set->groups[g].name != NULL) {
            on++;
        }
    }

    /* Secure the room BEFORE dropping the old rows. The other order -- forget, then grow -- turns
     * a failed realloc into permanent data loss: the game's existing selections are already gone
     * and the replacements never arrive, so the next save writes a file that has quietly
     * forgotten them. Reserving first over-counts by however many rows forget_game() is about to
     * free, which costs a few bytes and cannot lose anything. */
    if (on > 0 && !ensure_room(on)) {
        return;
    }

    /* Replace rather than merge. The screen that just closed showed the complete set of groups
     * for this game, so what it has ticked IS the answer -- merging would make un-ticking a cheat
     * impossible to persist, which is a bug that only shows up on the second boot. */
    forget_game(game_key);

    if (on == 0) {
        return;                 /* everything un-ticked; the forget above IS the change */
    }

    for (int g = 0; g < set->group_count; g++) {
        if (!set->groups[g].enabled || set->groups[g].name == NULL) {
            continue;
        }
        rows[row_count].game_key  = game_key;
        rows[row_count].name_hash = cache_hash32(set->groups[g].name);
        rows[row_count].reserved  = 0;
        row_count++;
    }
    dirty = true;
}

bool cheatstate_save (void) {
    if (!cache_writable()) {
        return false;
    }
    char file[64];
    cs_file(file, sizeof(file));

    if (row_count == 0) {
        cache_drop(file);
        dirty = false;
        return true;
    }
    bool ok = cache_store(file, CHEATSTATE_MAGIC, rows,
                          (uint32_t)(row_count * sizeof(cs_record_t)));
    if (ok) {
        dirty = false;
    }
    return ok;
}
