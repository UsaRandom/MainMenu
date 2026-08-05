/**
 * @file test_cheatdb.c
 * @brief Host-side test for src/cheats/cheatdb.c against a real generated database.
 *
 * The reader was rewritten when the on-disk format went from 1 to 2: names moved out of one
 * string table shared by the whole database and into each game's own blob, so a load is one seek
 * and one read of a few kilobytes instead of a seek, three reads and 769,488 bytes of string
 * table. That change is entirely offset arithmetic, which is the class of mistake that produces
 * plausible output -- a name one byte short, a code array read from the group table, a game whose
 * last cheat is somebody else's first.
 *
 * Nothing on the console can see any of that. The detail sheet would show a list of names and a
 * count, and both would look exactly as they look now.
 *
 * So: mkcheatdb.py writes the file, cheatdb_expect.py reads it back from the spec, and this reads
 * it a third time through the production C. The three parsers are independent, which is the only
 * arrangement in which agreement means something -- a writer and a reader sharing a header can be
 * wrong together all day and round-trip perfectly. Compare against AUDIT.md's two recorded cases
 * of a harness that measured the wrong thing.
 *
 * Needs build/cheats.db, which needs the corpus. Absent, this skips loudly rather than passing.
 *
 *     tools/hosttest/run.sh
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats/cheatdb.h"

/* ------------------------------------------------------------------ harness */

static int failures;
static int checks;

static void check (bool ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void checkf (bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void checkf (bool ok, const char *fmt, ...) {
    checks++;
    if (!ok) {
        failures++;
        va_list ap;
        va_start(ap, fmt);
        printf("  FAIL  ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
    }
}

/* ------------------------------------------------------------------ expectations */

typedef struct {
    char     code[8];
    unsigned version;
    unsigned groups;
    unsigned codes;
    char     first_name[160];
    uint32_t a0, v0, aN, vN;
} expect_t;

int main (void) {
    const char *storage = getenv("TESTDIR");
    const char *expect_path = getenv("EXPECT");
    if (storage == NULL || expect_path == NULL) {
        fprintf(stderr, "set TESTDIR and EXPECT\n");
        return 2;
    }

    FILE *ef = fopen(expect_path, "r");
    if (ef == NULL) {
        fprintf(stderr, "cannot read %s\n", expect_path);
        return 2;
    }

    if (!cheatdb_open(storage)) {
        printf("  FAIL  cheatdb_open(%s)\n", storage);
        return 1;
    }

    int described = 0;
    char line[512];
    while (fgets(line, sizeof(line), ef) != NULL) {
        expect_t e;
        memset(&e, 0, sizeof(e));
        /* %159[^\t] rather than %s: cheat names contain spaces, and a name read with %s stops at
         * the first one -- which would silently compare "Infinite" against "Infinite Health" and
         * report a mismatch that is the harness's fault, not the reader's. */
        if (sscanf(line, "%7[^\t]\t%u\t%u\t%u\t%159[^\t]\t%x\t%x\t%x\t%x",
                   e.code, &e.version, &e.groups, &e.codes, e.first_name,
                   &e.a0, &e.v0, &e.aN, &e.vN) != 9) {
            continue;
        }
        described++;

        /* Looked up the way the menu looks a game up: by game code and version, since the corpus
         * supplies no check codes. A '?' in the region slot is the reader's wildcard, so a real
         * ROM's fourth character is substituted here to exercise that path rather than sidestep
         * it -- 'E' is arbitrary and any letter must match equally. */
        char code[5];
        snprintf(code, sizeof(code), "%s", e.code);
        if (code[3] == '?') {
            code[3] = 'E';
        }
        uint8_t ver = (e.version == CHEATDB_ANY_VERSION) ? 7 : (uint8_t)e.version;

        cheatset_t set;
        if (!cheatdb_load(0, code, ver, &set)) {
            checkf(false, "%s v%u did not load", e.code, e.version);
            continue;
        }

        checkf((unsigned)set.group_count == e.groups,
               "%s group count: %d, expected %u", e.code, set.group_count, e.groups);
        checkf((unsigned)set.code_count == e.codes,
               "%s code count: %d, expected %u", e.code, set.code_count, e.codes);
        checkf(set.user_first == set.group_count,
               "%s: every group should be a database group, %d of %d were",
               e.code, set.user_first, set.group_count);

        if (set.group_count > 0) {
            checkf(set.groups[0].name != NULL && strcmp(set.groups[0].name, e.first_name) == 0,
                   "%s first name: \"%s\", expected \"%s\"",
                   e.code, set.groups[0].name ? set.groups[0].name : "(null)", e.first_name);
        }
        if (set.code_count > 0) {
            checkf(set.codes[0].address == e.a0 && set.codes[0].value == e.v0,
                   "%s first code: %08x %08x, expected %08x %08x",
                   e.code, set.codes[0].address, set.codes[0].value, e.a0, e.v0);
            /* The LAST line matters as much as the first. A blob read one row short, or a codes
             * offset computed from the wrong group count, still gets code 0 right. */
            const cheat_code_t *last = &set.codes[set.code_count - 1];
            checkf(last->address == e.aN && last->value == e.vN,
                   "%s last code: %08x %08x, expected %08x %08x",
                   e.code, last->address, last->value, e.aN, e.vN);
        }

        /* Every name has to lie inside the blob and be terminated. A group whose name_off ran off
         * the end used to read into the next game's string table and print somebody else's cheat;
         * now it points into this game's own blob, so the bound is the blob and this is what
         * checks it. */
        bool names_ok = true;
        for (int i = 0; i < set.group_count; i++) {
            const char *n = set.groups[i].name;
            if (n == NULL || n[0] == '\0' || strlen(n) > 127) {
                names_ok = false;
                break;
            }
            if ((unsigned)(set.groups[i].first + set.groups[i].count) > (unsigned)set.code_count) {
                names_ok = false;
                break;
            }
        }
        checkf(names_ok, "%s: a group name was empty or a group ran past the code array", e.code);

        /* Emitting one group must produce exactly that group's lines and nothing else. This is
         * the property the whole group model exists for, checked against a real database rather
         * than a constructed one. */
        if (set.group_count > 0) {
            set.groups[0].enabled = true;
            uint32_t out[4096];
            size_t w = cheatdb_emit(&set, out, sizeof(out) / sizeof(out[0]));
            size_t want = (size_t)set.groups[0].count * 2 + 2;
            if (want <= sizeof(out) / sizeof(out[0])) {
                checkf(w == want, "%s emit: %u words, expected %u",
                       e.code, (unsigned)w, (unsigned)want);
                checkf(w >= 2 && out[w - 2] == 0 && out[w - 1] == 0,
                       "%s emit: missing the terminating zero pair", e.code);
                checkf(w < 2 || (out[0] == set.codes[set.groups[0].first].address &&
                                 out[1] == set.codes[set.groups[0].first].value),
                       "%s emit: first pair is not the group's first line", e.code);
            }
        }

        cheatdb_free(&set);
        check(set.groups == NULL && set.codes == NULL && set.strtab == NULL,
              "cheatdb_free zeroes the set");
    }
    fclose(ef);

    checkf(described > 0, "the expectation file described no games");
    checkf(cheatdb_game_count() == described,
           "game count: %d, expectation file describes %d", cheatdb_game_count(), described);

    /* A game code nothing has must miss rather than returning the nearest row. The linear
     * fallback walks every entry comparing three or four characters, and "returns the last one it
     * looked at" is a real way to write that loop wrong. */
    cheatset_t none;
    check(!cheatdb_load(0, "ZZZZ", 0, &none), "an unknown game code misses");
    check(!cheatdb_load(0, NULL, 0, &none), "a NULL game code misses");

    cheatdb_close();
    check(!cheatdb_available(), "cheatdb_available is false after close");

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
