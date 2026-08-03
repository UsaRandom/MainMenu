/**
 * @file cheatdb.c
 * @brief Read-only cheat database. See cheatdb.h.
 * @ingroup cheats
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "cheats/cheatdb.h"
#include "menu/path.h"

#define CHEATDB_LOCATION    "/menu"
#define CHEATDB_FILE        "cheats.db"

/** @brief On-disk header. Fixed 64 bytes; fields are big-endian, as everything on this machine is. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format_ver;
    uint16_t _pad;
    uint32_t game_count;
    uint32_t index_off;
    uint32_t blob_off;
    uint32_t strtab_off;
    uint32_t strtab_size;
    uint32_t crc32;
    uint8_t  reserved[36];
} cheatdb_header_t;

/** @brief One index row, 20 bytes packed, sorted by check_code so it can be binary-searched. */
typedef struct __attribute__((packed)) {
    uint64_t check_code;
    char     game_code[4];
    uint8_t  version;
    uint8_t  flags;
    uint16_t cheat_count;
    uint32_t cheats_off;    /**< from blob_off */
} cheatdb_index_t;

/** @brief Per-game blob header row, 8 bytes. */
typedef struct __attribute__((packed)) {
    uint32_t name_off;      /**< from strtab_off */
    uint16_t code_first;    /**< index into this game's own code array */
    uint16_t code_count;
} cheatdb_group_t;

static FILE             *db_file;
static cheatdb_header_t  db_head;
static cheatdb_index_t  *db_index;

bool cheatdb_available (void) {
    return db_file != NULL && db_index != NULL;
}

int cheatdb_game_count (void) {
    return cheatdb_available() ? (int)db_head.game_count : 0;
}

void cheatdb_close (void) {
    if (db_file != NULL) {
        fclose(db_file);
        db_file = NULL;
    }
    free(db_index);
    db_index = NULL;
    memset(&db_head, 0, sizeof(db_head));
}

bool cheatdb_open (const char *storage_prefix) {
    cheatdb_close();

    path_t *p = path_init(storage_prefix, CHEATDB_LOCATION);
    path_push(p, CHEATDB_FILE);
    db_file = fopen(path_get(p), "rb");
    path_free(p);

    if (db_file == NULL) {
        return false;         /* no database on this card; not an error */
    }

    if (fread(&db_head, sizeof(db_head), 1, db_file) != 1) {
        cheatdb_close();
        return false;
    }

    /* Magic and version checked before anything is trusted, and a mismatch is a hard reject
     * rather than a migration -- same discipline as every other cache file here. A truncated or
     * foreign file must not be able to make us allocate game_count * 24 bytes of garbage. */
    if (db_head.magic != CHEATDB_MAGIC || db_head.format_ver != CHEATDB_FORMAT_VER) {
        debugf("cheatdb: magic/version mismatch (%08lx v%u), ignoring\n",
               (unsigned long)db_head.magic, db_head.format_ver);
        cheatdb_close();
        return false;
    }
    if (db_head.game_count == 0 || db_head.game_count > 20000) {
        cheatdb_close();
        return false;
    }

    size_t index_bytes = (size_t)db_head.game_count * sizeof(cheatdb_index_t);
    db_index = malloc(index_bytes);
    if (db_index == NULL) {
        cheatdb_close();
        return false;
    }

    if (fseek(db_file, db_head.index_off, SEEK_SET) != 0 ||
        fread(db_index, 1, index_bytes, db_file) != index_bytes) {
        cheatdb_close();
        return false;
    }

    debugf("cheatdb: %lu games\n", (unsigned long)db_head.game_count);
    return true;
}

/**
 * @brief Find a row, strongest key first.
 *
 * check_code is the whole ROM's checksum and cannot collide by accident. The (game_code, version)
 * and (game_code, any) fallbacks exist because the upstream corpus is keyed by filename, so a
 * converter cannot always supply a check code -- see tools/mkcheatdb.py.
 */
static const cheatdb_index_t *find_row (uint64_t check_code, const char *game_code, uint8_t version) {
    if (!cheatdb_available()) {
        return NULL;
    }

    if (check_code != 0) {
        int lo = 0, hi = (int)db_head.game_count - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint64_t v = db_index[mid].check_code;
            if (v == check_code) {
                return &db_index[mid];
            }
            if (v < check_code) lo = mid + 1; else hi = mid - 1;
        }
    }

    if (game_code == NULL) {
        return NULL;
    }

    /* Linear from here: the index is sorted by check_code, so a game-code lookup has no order to
     * exploit. 350 rows is a few microseconds and it only runs on a miss. */
    const cheatdb_index_t *any = NULL;
    for (uint32_t i = 0; i < db_head.game_count; i++) {
        /* A '?' in the region position is a wildcard, because the upstream database's MATCH_ID
         * rows match any region and the converter carries that through rather than inventing a
         * region byte. Without this, every non-USA release misses. */
        const char *stored = db_index[i].game_code;
        int keylen = (stored[3] == '?') ? 3 : 4;
        if (memcmp(stored, game_code, keylen) != 0) {
            continue;
        }
        if (db_index[i].version == version) {
            return &db_index[i];
        }
        if (db_index[i].version == CHEATDB_ANY_VERSION && any == NULL) {
            any = &db_index[i];
        }
    }
    return any;
}

bool cheatdb_has (uint64_t check_code, const char *game_code, uint8_t version) {
    return find_row(check_code, game_code, version) != NULL;
}

void cheatdb_free (cheatset_t *set) {
    if (set == NULL) {
        return;
    }
    free(set->groups);
    free(set->codes);
    free(set->strtab);
    memset(set, 0, sizeof(*set));
}

bool cheatdb_load (uint64_t check_code, const char *game_code, uint8_t version, cheatset_t *out) {
    memset(out, 0, sizeof(*out));

    const cheatdb_index_t *row = find_row(check_code, game_code, version);
    if (row == NULL || row->cheat_count == 0) {
        return false;
    }

    uint32_t n = row->cheat_count;
    cheatdb_group_t *raw = malloc((size_t)n * sizeof(cheatdb_group_t));
    if (raw == NULL) {
        return false;
    }

    if (fseek(db_file, db_head.blob_off + row->cheats_off, SEEK_SET) != 0 ||
        fread(raw, sizeof(cheatdb_group_t), n, db_file) != n) {
        free(raw);
        return false;
    }

    /* Codes follow the group table for this game, contiguously. total is the end of the last
     * group rather than a stored count, so a truncated blob cannot make us read past it. */
    uint32_t total_codes = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t end = (uint32_t)raw[i].code_first + raw[i].code_count;
        if (raw[i].code_count == 0 || end < raw[i].code_first) {
            free(raw);
            return false;                        /* malformed: empty or wrapping group */
        }
        if (end > total_codes) {
            total_codes = end;
        }
    }

    cheat_code_t *codes = malloc((size_t)total_codes * sizeof(cheat_code_t));
    if (codes == NULL) {
        free(raw);
        return false;
    }
    if (fread(codes, sizeof(cheat_code_t), total_codes, db_file) != total_codes) {
        free(raw);
        free(codes);
        return false;
    }

    char *strtab = malloc(db_head.strtab_size);
    if (strtab == NULL) {
        free(raw);
        free(codes);
        return false;
    }
    if (fseek(db_file, db_head.strtab_off, SEEK_SET) != 0 ||
        fread(strtab, 1, db_head.strtab_size, db_file) != db_head.strtab_size) {
        free(raw);
        free(codes);
        free(strtab);
        return false;
    }
    strtab[db_head.strtab_size - 1] = '\0';      /* so a bad offset cannot run off the end */

    cheat_group_t *groups = calloc(n, sizeof(cheat_group_t));
    if (groups == NULL) {
        free(raw);
        free(codes);
        free(strtab);
        return false;
    }

    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = raw[i].name_off;
        groups[i].name    = (off < db_head.strtab_size) ? &strtab[off] : "(unnamed)";
        groups[i].first   = raw[i].code_first;
        groups[i].count   = raw[i].code_count;
        groups[i].enabled = false;
    }
    free(raw);

    out->groups      = groups;
    out->group_count = (int)n;
    out->codes       = codes;
    out->code_count  = (int)total_codes;
    out->strtab      = strtab;
    return true;
}

int cheatdb_total_lines (const cheatset_t *set) {
    int total = 0;
    for (int i = 0; i < set->group_count; i++) {
        total += set->groups[i].count;
    }
    return total;
}

size_t cheatdb_emit (const cheatset_t *set, uint32_t *out, size_t out_cap) {
    size_t w = 0;

    for (int g = 0; g < set->group_count; g++) {
        const cheat_group_t *grp = &set->groups[g];
        if (!grp->enabled) {
            continue;
        }

        /* All of a group or none of it. Checking the whole group's worth of space before writing
         * any of it is the entire point: stopping halfway through would leave a conditional
         * without its write, which is the exact failure this model exists to prevent -- the
         * engine would pair the dangling D0 with whatever line came next. */
        if (w + (size_t)grp->count * 2 + 2 > out_cap) {
            debugf("cheatdb: emit buffer full, dropping group '%s' and the rest\n", grp->name);
            break;
        }

        for (int i = 0; i < grp->count; i++) {
            const cheat_code_t *c = &set->codes[grp->first + i];
            out[w++] = c->address;
            out[w++] = c->value;
        }
    }

    if (w == 0) {
        return 0;
    }

    out[w++] = 0;
    out[w++] = 0;
    return w;
}
