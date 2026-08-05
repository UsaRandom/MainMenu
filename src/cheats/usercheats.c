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

/**
 * @brief Case-insensitive name compare, for deciding whether two cheats are the same cheat.
 *
 * Case-insensitive because the editor's alphabet is uppercase only. A shipped cheat called
 * "Infinite health" that somebody retypes the name of comes back as "INFINITE HEALTH", and an
 * exact compare would leave both in the list -- the original and the edit, side by side, which is
 * the thing overriding exists to avoid.
 */
static bool same_name (const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) {
            return false;
        }
    }
    return *a == *b;
}

/** @brief Index of the group in @p set called @p name, or -1. */
static int find_group (const cheatset_t *set, const char *name) {
    for (int i = 0; i < set->group_count; i++) {
        if (same_name(set->groups[i].name, name)) {
            return i;
        }
    }
    return -1;
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
    /* `cheat_group_t::first` is a uint16_t index into codes[], so the array cannot be allowed past
     * 65,535 entries -- past it the truncation is silent and the group points at the wrong codes.
     * Reachable only in principle (the largest game in the corpus is four figures of cheats, a few
     * lines each) but the failure would be a cheat that patches an address nobody chose. */
    if (set->code_count + codes > 65535) {
        return false;
    }

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

    /* Skipped entirely when there is nothing to hold, because overriding a shipped cheat asks for
     * zero name bytes and realloc(NULL, 0) is allowed to hand back NULL -- which this function
     * cannot tell from being out of memory, and would report as a failed edit. */
    size_t want = old_chars + (size_t)chars;
    if (want > 0) {
        char *s = realloc(set->user_strtab, want);
        if (s == NULL) {
            return false;
        }
        size_t off = 0;
        for (int i = set->user_first; i < set->group_count; i++) {
            set->groups[i].name = &s[off];
            off += strlen(&s[off]) + 1;
        }
        set->user_strtab = s;
    }
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

/**
 * @brief Point an existing group at @p r's lines instead of the ones it shipped with.
 *
 * The replaced codes are left where they are rather than compacted out. Nothing walks codes[]
 * except through a group's first/count, so orphaned entries are invisible; compacting them would
 * mean renumbering `first` on every later group for a handful of bytes.
 */
static void override_group (cheatset_t *set, const uc_record_t *r, int gi) {
    set->groups[gi].first = (uint16_t)set->code_count;
    set->groups[gi].count = r->line_count;
    for (int j = 0; j < r->line_count; j++) {
        set->codes[set->code_count + j].address = r->lines[j].address;
        set->codes[set->code_count + j].value   = r->lines[j].value;
    }
    set->code_count += r->line_count;
    /* The NAME is deliberately left pointing at the database's string table. It is what the list
     * draws and what cheatstate hashes, so keeping it means an edited cheat is still remembered as
     * the same cheat -- and it is why the editor can be entered on a shipped group at all. */
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

    /* Sized as if every record were new. Overrides need the codes but neither a group slot nor
     * name bytes, so this over-allocates by a few dozen bytes when a game has edited cheats --
     * cheaper than a second pass that has to model the appends it has not made yet. */
    size_t off = 0;
    if (!grow_set(set, mine, lines, chars, &off)) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (table[i].key != game_key) {
            continue;
        }
        int gi = find_group(set, table[i].name);
        if (gi >= 0) {
            override_group(set, &table[i], gi);
        } else {
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

    if (table == NULL) {
        table = calloc(UC_MAX, sizeof(uc_record_t));
        if (table == NULL) {
            return false;
        }
    }

    /* Truncated first, because the truncated form is what gets stored and therefore what has to
     * be matched against -- otherwise a long name would file a second record every time it was
     * edited, each one indistinguishable from the last on disk. */
    char stored[USERCHEAT_NAME_CAP];
    snprintf(stored, sizeof(stored), "%s", name);

    /* Saving over a cheat of the same name replaces it rather than filing a second one. Without
     * this, editing the same cheat twice leaves two records; usercheats_apply() would apply both,
     * the second silently winning, and the first would be invisible and impossible to remove. */
    int slot = -1;
    for (int i = 0; i < count; i++) {
        if (table[i].key == game_key && same_name(table[i].name, stored)) {
            slot = i;
            break;
        }
    }
    bool fresh = (slot < 0);
    if (fresh) {
        if (count >= UC_MAX) {
            return false;
        }
        slot = count;
    }

    /* The live set is grown before the record is written, so a failed allocation leaves the table
     * as it was rather than holding an edit the list does not show. */
    int gi = find_group(set, stored);
    size_t off = 0;
    if (!grow_set(set, gi >= 0 ? 0 : 1, line_count,
                  gi >= 0 ? 0 : (int)strlen(stored) + 1, &off)) {
        return false;
    }

    uc_record_t *r = &table[slot];
    memset(r, 0, sizeof(*r));
    r->key = game_key;
    memcpy(r->name, stored, strlen(stored) + 1);
    r->line_count = (uint16_t)line_count;
    for (int i = 0; i < line_count; i++) {
        r->lines[i].address = lines[i].address;
        r->lines[i].value   = lines[i].value;
    }

    /* Into the live set as well, so the list the user is looking at changes now. Rebuilding from
     * cheatdb_load() instead would be simpler and would throw away every tick they have made since
     * the sheet opened. */
    if (gi >= 0) {
        override_group(set, r, gi);
    } else {
        append_group(set, r, &off);
    }

    if (fresh) {
        count++;
    }
    dirty = true;
    return true;
}
