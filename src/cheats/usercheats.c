/**
 * @file usercheats.c
 * @brief Storage for hand-entered cheats. See usercheats.h for the format and why it is fixed.
 * @ingroup cheats
 */

#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "cheats/usercheats.h"
#include "library/cache.h"

#define USERCHEATS_FILE "usercheats.dat"

/** @brief Everything one hand-entered cheat is. 108 bytes; see the assertion below. */
typedef struct __attribute__((packed)) {
    uint64_t key;                                  /**< playstate_key() of the game */
    char     name[USERCHEAT_NAME_CAP];
    uint16_t line_count;
    uint16_t reserved;
    struct __attribute__((packed)) {
        uint32_t address;
        uint32_t value;
    } lines[USERCHEAT_MAX_LINES];
} uc_record_t;

_Static_assert(sizeof(uc_record_t) == 8 + USERCHEAT_NAME_CAP + 4 + 8 * USERCHEAT_MAX_LINES,
               "usercheat record must not gain padding");

/** Ceiling on the table. Not a memory limit -- 256 records is 27 KB -- but a bound on what a
 *  corrupt or hand-edited length field can make this allocate. */
#define UC_MAX 256

static uc_record_t *table;
static int          count;
static bool         dirty;

void usercheats_load (void) {
    void *buf = NULL;
    uint32_t bytes = 0;

    usercheats_free();

    if (!cache_load(USERCHEATS_FILE, USERCHEATS_MAGIC, &buf, &bytes)) {
        return;
    }

    int n = (int)(bytes / sizeof(uc_record_t));
    if (n > UC_MAX) {
        n = UC_MAX;
    }
    if (n <= 0) {
        free(buf);
        return;
    }

    table = malloc((size_t)UC_MAX * sizeof(uc_record_t));
    if (table == NULL) {
        free(buf);
        return;
    }
    memcpy(table, buf, (size_t)n * sizeof(uc_record_t));
    free(buf);

    /* Clamp every record on the way in. line_count drives a loop over a fixed array and a stored
     * value of 60,000 would walk off the end of it -- the file is on a card the user can edit and
     * a corrupt row must be dull, not exciting. */
    count = 0;
    for (int i = 0; i < n; i++) {
        if (table[i].line_count == 0 || table[i].line_count > USERCHEAT_MAX_LINES) {
            continue;
        }
        table[i].name[USERCHEAT_NAME_CAP - 1] = '\0';
        if (count != i) {
            table[count] = table[i];
        }
        count++;
    }

    dirty = false;
    debugf("USERCHEATS loaded %d records (%d rejected)\n", count, n - count);
}

void usercheats_free (void) {
    free(table);
    table = NULL;
    count = 0;
    dirty = false;
}

bool usercheats_dirty (void) {
    return dirty;
}

bool usercheats_save (void) {
    if (!cache_writable()) {
        return false;
    }
    if (count == 0) {
        cache_drop(USERCHEATS_FILE);
        dirty = false;
        return true;
    }
    bool ok = cache_store(USERCHEATS_FILE, USERCHEATS_MAGIC, table,
                          (uint32_t)((size_t)count * sizeof(uc_record_t)));
    if (ok) {
        dirty = false;
    }
    return ok;
}

int usercheats_count (uint64_t game_key) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (table[i].key == game_key) {
            n++;
        }
    }
    return n;
}

/** @brief Bytes the user groups' names currently occupy in @p set->user_strtab. */
static size_t user_chars (const cheatset_t *set) {
    size_t n = 0;
    for (int i = set->user_first; i < set->group_count; i++) {
        n += strlen(set->groups[i].name) + 1;
    }
    return n;
}

/**
 * @brief Grow @p set by @p groups groups, @p codes codes and @p chars name bytes.
 *
 * `*free_off` receives the offset in user_strtab where the new names go.
 *
 * The realloc has a trap in it: `cheat_group_t::name` is a pointer, and every existing user
 * group's name points into the OLD user_strtab. Moving that allocation without repointing them
 * leaves the list drawing freed memory, which under ares renders as plausible garbage rather than
 * crashing. `user_first` exists so the repointing loop knows where the user groups start -- the
 * database ones point into a different allocation entirely, and comparing pointers between two
 * unrelated allocations to tell them apart is not something C promises to answer.
 */
static bool grow_set (cheatset_t *set, int groups, int codes, int chars, size_t *free_off) {
    size_t old_chars = (set->user_strtab != NULL) ? user_chars(set) : 0;

    cheat_group_t *g = realloc(set->groups, (size_t)(set->group_count + groups) * sizeof(*g));
    if (g == NULL) {
        return false;
    }
    set->groups = g;

    cheat_code_t *c = realloc(set->codes, (size_t)(set->code_count + codes) * sizeof(*c));
    if (c == NULL) {
        return false;                /* set->groups is bigger than group_count says; harmless */
    }
    set->codes = c;

    char *s = realloc(set->user_strtab, old_chars + (size_t)chars);
    if (s == NULL) {
        return false;
    }
    size_t off = 0;
    for (int i = set->user_first; i < set->group_count; i++) {
        set->groups[i].name = &s[off];
        off += strlen(&s[off]) + 1;
    }
    set->user_strtab = s;
    *free_off = old_chars;
    return true;
}

/** @brief Copy one stored record onto the end of @p set. Caller has already grown it. */
static void append_group (cheatset_t *set, const uc_record_t *r, size_t *off) {
    size_t len = strlen(r->name) + 1;
    memcpy(&set->user_strtab[*off], r->name, len);
    set->groups[set->group_count].name    = &set->user_strtab[*off];
    set->groups[set->group_count].first   = (uint16_t)set->code_count;
    set->groups[set->group_count].count   = r->line_count;
    set->groups[set->group_count].enabled = false;
    *off += len;

    for (int j = 0; j < r->line_count; j++) {
        set->codes[set->code_count + j].address = r->lines[j].address;
        set->codes[set->code_count + j].value   = r->lines[j].value;
    }
    set->group_count++;
    set->code_count += r->line_count;
}

int usercheats_apply (cheatset_t *set, uint64_t game_key) {
    int mine = 0, chars = 0, lines = 0;
    for (int i = 0; i < count; i++) {
        if (table[i].key == game_key) {
            mine++;
            lines += table[i].line_count;
            chars += (int)strlen(table[i].name) + 1;
        }
    }
    if (mine == 0) {
        return 0;
    }

    size_t off = 0;
    if (!grow_set(set, mine, lines, chars, &off)) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (table[i].key == game_key) {
            append_group(set, &table[i], &off);
        }
    }
    return mine;
}

bool usercheats_add (cheatset_t *set, uint64_t game_key, const char *name,
                     const cheat_code_t *lines, int line_count) {
    if (line_count <= 0 || line_count > USERCHEAT_MAX_LINES || name == NULL || lines == NULL) {
        return false;
    }
    if (count >= UC_MAX) {
        return false;
    }

    if (table == NULL) {
        table = calloc(UC_MAX, sizeof(uc_record_t));
        if (table == NULL) {
            return false;
        }
    }

    uc_record_t *r = &table[count];
    memset(r, 0, sizeof(*r));
    r->key = game_key;
    strncpy(r->name, name, USERCHEAT_NAME_CAP - 1);
    r->line_count = (uint16_t)line_count;
    for (int i = 0; i < line_count; i++) {
        r->lines[i].address = lines[i].address;
        r->lines[i].value   = lines[i].value;
    }

    /* Into the live set as well, so the list the user is looking at gains the row now. Rebuilding
     * from cheatdb_load() instead would be simpler and would throw away every tick they have made
     * since the sheet opened. */
    size_t off = 0;
    if (!grow_set(set, 1, line_count, (int)strlen(r->name) + 1, &off)) {
        return false;
    }
    append_group(set, r, &off);

    count++;
    dirty = true;
    return true;
}
