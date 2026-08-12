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
#include "menu/paths.h"
#include "utils/fs.h"

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

/** @brief One index row, 24 bytes packed, sorted by check_code so it can be binary-searched. */
typedef struct __attribute__((packed)) {
    uint64_t check_code;
    char     game_code[4];
    uint8_t  version;
    uint8_t  flags;
    uint16_t cheat_count;
    uint32_t cheats_off;    /**< from blob_off */
    uint32_t blob_size;     /**< group rows + codes + names, so a load is one read */
} cheatdb_index_t;

/** @brief Per-game blob header row, 8 bytes. */
typedef struct __attribute__((packed)) {
    uint32_t name_off;      /**< from the start of this game's own blob */
    uint16_t code_first;    /**< index into this game's own code array */
    uint16_t code_count;
} cheatdb_group_t;

/* Every number in this file is big-endian, because the machine that reads it is.
 *
 * That was left implicit -- the structs above were read straight off disk and the fields came out
 * right because mips64-elf is big-endian and nothing else ever ran this code. Which is true, and
 * is also why the reader could not be tested: tools/hosttest runs the real cheatdb.c natively,
 * and on a little-endian host a raw struct read turns 'M64C' into 'C64M' and format 2 into 512.
 * The suite's first run said exactly that.
 *
 * So the byte order is stated. On the console CHEATDB_SWAP is 0, the loops below are constant-
 * folded away, and the generated code is what it was. Off it, the same file parses.
 */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CHEATDB_SWAP 1
#else
#define CHEATDB_SWAP 0
#endif

static inline uint16_t swap16 (uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t swap32 (uint32_t v) {
    return ((v >> 24) & 0x000000FFu) | ((v >> 8) & 0x0000FF00u) |
           ((v << 8) & 0x00FF0000u) | ((v << 24) & 0xFF000000u);
}

static inline uint64_t swap64 (uint64_t v) {
    return ((uint64_t)swap32((uint32_t)v) << 32) | swap32((uint32_t)(v >> 32));
}

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

    /* Probed rather than fixed: someone who downloaded a database has it in their Downloads
     * folder, not in a folder they have never heard of, and telling them to create one is a step
     * that buys nothing. See menu/paths.h. */
    char path[300];
    if (!menu_find_file(path, sizeof(path), storage_prefix, CHEATDB_FILE)) {
        /* Nothing on the card, so use the copy baked into the cartridge if this build has one.
         *
         * Card first, deliberately. The corpus is refetched and regenerated -- 1.1.0's keys are
         * per region, which is a breaking change to the format's contents -- and a user who drops
         * a newer cheats.db on their card must get that one rather than whatever was current when
         * their menu was built. So this is a fallback, never an override, and dropping the file on
         * the card is still all it takes to update.
         *
         * Absent in both places stays "not an error": a build made without the corpus present
         * ships no copy, and the cheats screen is simply empty, exactly as before. */
        snprintf(path, sizeof(path), "rom:/%s", CHEATDB_FILE);
        if (!file_exists(path)) {
            return false;     /* no database on this card and none in the ROM; not an error */
        }
        debugf("CHEATDB: no copy on the card, using the one in the cartridge\n");
    }

    db_file = fopen(path, "rb");
    if (db_file == NULL) {
        return false;
    }

    if (fread(&db_head, sizeof(db_head), 1, db_file) != 1) {
        cheatdb_close();
        return false;
    }
    if (CHEATDB_SWAP) {
        db_head.magic       = swap32(db_head.magic);
        db_head.format_ver  = swap16(db_head.format_ver);
        db_head.game_count  = swap32(db_head.game_count);
        db_head.index_off   = swap32(db_head.index_off);
        db_head.blob_off    = swap32(db_head.blob_off);
        db_head.strtab_off  = swap32(db_head.strtab_off);
        db_head.strtab_size = swap32(db_head.strtab_size);
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
    if (CHEATDB_SWAP) {
        for (uint32_t i = 0; i < db_head.game_count; i++) {
            db_index[i].check_code  = swap64(db_index[i].check_code);
            db_index[i].cheat_count = swap16(db_index[i].cheat_count);
            db_index[i].cheats_off  = swap32(db_index[i].cheats_off);
            db_index[i].blob_size   = swap32(db_index[i].blob_size);
        }
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
     * exploit. 390 rows is a few microseconds and it only runs on a miss.
     *
     * Most specific wins, not first-found, and that is a correction. Several games have both a
     * wildcard row and a region-specific one -- Super Mario 64 carries `NSM?` and `NSMJ`, and both
     * match a Japanese cartridge asking for NSMJ. Returning whichever sat earlier in the index
     * handed the Japanese release the merged all-regions row, which is the same class of mistake
     * as handing a V1.2 cartridge V1.0's addresses (AUDIT 2aa): a cheat that writes faithfully to
     * an address that means nothing in this binary.
     *
     * Rank, best first: an exact four-character code beats a three-character wildcard, and within
     * each, an exact version beats the ANY sentinel. */
    const cheatdb_index_t *best = NULL;
    int best_rank = 0;

    for (uint32_t i = 0; i < db_head.game_count; i++) {
        /* A '?' in the region position is a wildcard, because the upstream database's MATCH_ID
         * rows match any region and the converter carries that through rather than inventing a
         * region byte. Without this, every non-USA release misses. */
        const char *stored = db_index[i].game_code;
        bool wild = (stored[3] == '?');
        if (memcmp(stored, game_code, wild ? 3 : 4) != 0) {
            continue;
        }

        int rank;
        if (db_index[i].version == version) {
            rank = wild ? 3 : 4;
        } else if (db_index[i].version == CHEATDB_ANY_VERSION) {
            rank = wild ? 1 : 2;
        } else {
            continue;                   /* a different revision of the same game: not ours */
        }

        if (rank > best_rank) {
            best_rank = rank;
            best = &db_index[i];
        }
    }
    return best;
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
    free(set->user_strtab);
    memset(set, 0, sizeof(*set));
}

/**
 * @brief Load one game's blob: one seek, one read.
 *
 * Version 1 kept a single string table for the whole database and read **all** of it on every
 * load -- 769,488 bytes on the shipped file, allocated and freed each time a game sheet opened.
 * That was the blocking call behind the reported symptom "cheat db search seems to lag music when
 * opening details": three quarters of a megabyte off FatFs is far longer than the ~133 ms of
 * audio the mixer holds ahead of the DAC, so the music stopped for as long as it took, every
 * single time. Nothing about the read was needed -- a game uses a few hundred bytes of names.
 *
 * In version 2 the names live in the game's own blob, so this reads group rows, code lines and
 * names in one go: about 12 KB for the largest game in the corpus and under 1 KB for a typical
 * one. The blob is kept as `out->strtab` because every group name points into it.
 */
bool cheatdb_load (uint64_t check_code, const char *game_code, uint8_t version, cheatset_t *out) {
    memset(out, 0, sizeof(*out));

    const cheatdb_index_t *row = find_row(check_code, game_code, version);
    if (row == NULL || row->cheat_count == 0) {
        return false;
    }

    uint32_t n = row->cheat_count;
    uint32_t blob_size = row->blob_size;

    /* The group table alone must fit, or the loop below reads past the buffer. A file that fails
     * this is corrupt, and a corrupt file must be rejected rather than trusted with a length. */
    if (blob_size < (uint64_t)n * sizeof(cheatdb_group_t) || blob_size > (16u << 20)) {
        return false;
    }

    char *blob = malloc(blob_size);
    if (blob == NULL) {
        return false;
    }
    if (fseek(db_file, db_head.blob_off + row->cheats_off, SEEK_SET) != 0 ||
        fread(blob, 1, blob_size, db_file) != blob_size) {
        free(blob);
        return false;
    }
    blob[blob_size - 1] = '\0';      /* so a bad name offset cannot run off the end */

    cheatdb_group_t *raw = (cheatdb_group_t *)blob;
    if (CHEATDB_SWAP) {
        for (uint32_t i = 0; i < n; i++) {
            raw[i].name_off   = swap32(raw[i].name_off);
            raw[i].code_first = swap16(raw[i].code_first);
            raw[i].code_count = swap16(raw[i].code_count);
        }
    }

    /* Codes follow the group table for this game, contiguously. total is the end of the last
     * group rather than a stored count, so a truncated blob cannot make us read past it. */
    uint32_t total_codes = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t end = (uint32_t)raw[i].code_first + raw[i].code_count;
        if (raw[i].code_count == 0 || end < raw[i].code_first) {
            free(blob);
            return false;                        /* malformed: empty or wrapping group */
        }
        if (end > total_codes) {
            total_codes = end;
        }
    }

    size_t codes_off = (size_t)n * sizeof(cheatdb_group_t);
    if (codes_off + (size_t)total_codes * sizeof(cheat_code_t) > blob_size) {
        free(blob);
        return false;
    }

    /* Copied out rather than aliased into the blob: usercheats.c appends hand-entered lines to
     * this array and reallocs it, which it cannot do to a pointer into the middle of another
     * allocation. A few kilobytes; the read it replaces was three quarters of a megabyte. */
    cheat_code_t *codes = malloc((size_t)total_codes * sizeof(cheat_code_t));
    if (codes == NULL) {
        free(blob);
        return false;
    }
    memcpy(codes, &blob[codes_off], (size_t)total_codes * sizeof(cheat_code_t));
    if (CHEATDB_SWAP) {
        for (uint32_t i = 0; i < total_codes; i++) {
            codes[i].address = swap32(codes[i].address);
            codes[i].value   = swap32(codes[i].value);
        }
    }

    cheat_group_t *groups = calloc(n, sizeof(cheat_group_t));
    if (groups == NULL) {
        free(blob);
        free(codes);
        return false;
    }

    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = raw[i].name_off;
        groups[i].name    = (off < blob_size) ? &blob[off] : "(unnamed)";
        groups[i].first   = raw[i].code_first;
        groups[i].count   = raw[i].code_count;
        groups[i].enabled = false;
    }

    char *strtab = blob;
    out->groups      = groups;
    out->group_count = (int)n;
    out->codes       = codes;
    out->code_count  = (int)total_codes;
    out->strtab      = strtab;
    /* Everything loaded here came from the database, so anything appended after this point is a
     * user cheat. usercheats.c needs the boundary to repoint its own names across a realloc; on
     * the failure paths above the struct is left zeroed, where 0 == group_count says the same. */
    out->user_first  = (int)n;
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
