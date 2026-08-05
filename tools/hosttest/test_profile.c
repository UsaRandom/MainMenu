/**
 * @file test_profile.c
 * @brief Host-side test for src/menu/profile.c and src/library/locks.c.
 *
 * Same gap as test_cache.c, and worse here. Under ares the storage prefix is the ROM's own
 * read-only DFS, so `profile_save()` writes nothing, `locks_save()` returns before serialising a
 * byte, and the regression suite can only prove the screens degrade politely. That leaves the two
 * things this feature can get wrong in ways nobody would notice until a card was involved:
 *
 * 1. **A profile writing to the wrong path.** `profile_cache_name()` and `profile_save_subdir()`
 *    decide which `playstate.dat` is read and which folder a launch drops a `.sav` into. Getting
 *    profile 1's case wrong -- a `p1/` prefix where there should be none -- would strand every
 *    save on every card that existed before this feature, and it would look exactly like a
 *    working menu with an empty save folder.
 *
 * 2. **The renumbering on delete.** Removing a profile shifts every one above it down a slot, and
 *    the slot names the folder. If the active index does not follow correctly, the console comes
 *    back as somebody else.
 *
 * Neither of those is visible in a frame, so neither is reachable from the regression suite at
 * all. They are pure functions of state, which is exactly what a host test is for.
 *
 * locks.c is here too because it is a new on-disk format, and the same reasoning as test_cache.c
 * applies to it: the round trip is the whole point and ares cannot execute the writing half.
 *
 *     cc -std=c11 -Wall -Wextra -Werror -Itools/hosttest/shim -Isrc -Isrc/library \
 *        tools/hosttest/test_profile.c src/library/cache.c src/library/locks.c \
 *        src/library/playstate.c src/menu/profile.c src/menu/ini_parser.c src/menu/paths.c \
 *        tools/hosttest/shim/fs_probe.c \
 *        -o build/hosttest/test_profile && build/hosttest/test_profile
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/cache.h"
#include "library/library.h"
#include "library/locks.h"
#include "menu/profile.h"

/* ------------------------------------------------------------------ shims */

bool directory_create (char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return mkdir(tmp, 0777) == 0;
}

/* cheatstate.c is not linked in: it drags in cheatdb.h and the whole cheat set model for three
 * functions whose per-profile behaviour is one call to profile_cache_name(), which is asserted
 * directly below. Stubbed and counted instead, so that what IS tested here is the thing that
 * matters about them at this seam -- that switching profile flushes the outgoing selection before
 * it reloads, rather than after, when the old one is already gone. */
static int cs_loads;
static int cs_saves;
static bool cs_is_dirty;

bool cheatstate_dirty (void) { return cs_is_dirty; }
bool cheatstate_save (void) { cs_saves++; cs_is_dirty = false; return true; }
void cheatstate_load (void) { cs_loads++; }

/* ------------------------------------------------------------------ harness */

static int failures;
static int checks;

static void check (bool ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

static void check_str (const char *got, const char *want, const char *what) {
    bool ok = (strcmp(got, want) == 0);
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s (got \"%s\", wanted \"%s\")\n", what, got, want);
    } else {
        printf("  ok    %s\n", what);
    }
}

static char root[512];

/** @brief Wipe and recreate the storage root, so each section starts from a bare card. */
static void fresh_storage (void) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* Nothing there to remove is the normal first case. */
    }
    mkdir(root, 0777);
}

/* ------------------------------------------------------------------ a fake library */

#define GAMES 6

static lib_record_t records[GAMES];
static library_t lib = { .records = records, .count = GAMES };

static void build_library (void) {
    memset(records, 0, sizeof(records));
    for (int i = 0; i < GAMES; i++) {
        /* Real check codes, because playstate_key() prefers them and falls back to hashing the
         * filename only when one is absent. The last two records deliberately have none, so the
         * fallback path -- every emulated-system ROM in the real product, since only N64 headers
         * are parsed -- is covered, and so is its collision behaviour: they share a filename. */
        records[i].check_code = (i >= GAMES - 2) ? 0 : (0x1122334400000000ull + (uint64_t)i);
        records[i].path = (char *)"/roms/n64/game.z64";
    }
}

static int locked_count (void) {
    int n = 0;
    for (int i = 0; i < GAMES; i++) {
        if (records[i].flags & LIBF_LOCKED) {
            n++;
        }
    }
    return n;
}

static void clear_locks (void) {
    for (int i = 0; i < GAMES; i++) {
        records[i].flags &= (uint16_t)~LIBF_LOCKED;
    }
}

/* ------------------------------------------------------------------ paths */

static void test_paths (void) {
    char buf[64];

    printf("\n-- paths\n");

    fresh_storage();
    profile_load(root);
    check(profile_count() == 1, "a card with no profiles.ini has exactly one profile");
    check(profile_active() == 0, "and it is the active one");

    /* The single most consequential assertion in this file. Profile 1 must produce the paths a
     * card written before this feature existed already uses -- no suffix, no subdirectory. A
     * regression here does not crash and does not look wrong; it silently stops finding every
     * save and every favourite on every existing card. */
    profile_cache_name(buf, sizeof(buf), "playstate.dat");
    check_str(buf, "playstate.dat", "profile 1's cache file is unsuffixed");
    check(profile_save_subdir() == NULL, "profile 1 has no saves subdirectory");

    check(profile_add() == 1, "adding a second profile returns index 1");
    check(profile_add() == 2, "and a third returns index 2");
    check(profile_count() == 3, "the roster now holds three");

    profile_activate(2, &lib);
    profile_cache_name(buf, sizeof(buf), "playstate.dat");
    /* Index 2 is player 3, and the folder is named for the player, not the index. A card opened
     * on a computer has to read the way the screen does. */
    check_str(buf, "p3/playstate.dat", "profile 3's cache file nests under p3/");
    check_str(profile_save_subdir(), "p3", "and its saves go in saves/p3");

    profile_activate(0, &lib);
    check(profile_save_subdir() == NULL, "switching back to profile 1 drops the suffix again");

    /* Order, not just occurrence. Everything profile_activate() reloads lands on top of the
     * outgoing profile's state, so an unwritten selection at that moment is gone -- and switching
     * profile is exactly when somebody stops touching the menu, which is to say when their last
     * change is most likely to be one press old. */
    cs_loads = cs_saves = 0;
    cs_is_dirty = true;
    profile_activate(1, &lib);
    check(cs_saves == 1, "switching profile flushes the outgoing cheat selection");
    check(cs_loads == 1, "and reloads the incoming one");
    profile_activate(0, &lib);
}

/* ------------------------------------------------------------------ the roster file */

static void test_roster (void) {
    printf("\n-- roster\n");

    fresh_storage();
    profile_load(root);
    profile_add();
    profile_add();
    profile_set_name(0, "ANA");
    profile_set_name(1, "BEN");
    profile_set_name(2, "CAI");
    profile_activate(1, &lib);
    profile_save();

    /* Reload from the file rather than trusting the in-memory copy: the point of the test is the
     * serialisation, and a getter that returns what a setter just stored proves nothing. */
    profile_load(root);
    check(profile_count() == 3, "three profiles survive a reload");
    check(profile_active() == 1, "and so does which one was active");
    check_str(profile_name(0), "ANA", "name 1 round-trips");
    check_str(profile_name(2), "CAI", "name 3 round-trips");

    profile_set_name(1, "");
    check_str(profile_name(1), "Player 2", "an empty name reads as its slot");
    check_str(profile_name_raw(1), "", "but is still empty underneath, so renaming starts blank");

    /* The name editor pads every cell to a space so the whole width is editable, which means a
     * name the user shortened comes back with a tail of spaces. Untrimmed it is non-empty by
     * strcmp and blank on screen, so it would suppress the "Player N" fallback and draw nothing. */
    profile_set_name(1, "DEE     ");
    check_str(profile_name(1), "DEE", "trailing spaces are trimmed off a name");
    profile_set_name(1, "    ");
    check_str(profile_name(1), "Player 2", "a name of nothing but spaces is an empty name");

    profile_set_name(1, "ABCDEFGHIJKLMNOPQRST");
    check(strlen(profile_name(1)) == PROFILE_NAME_CAP - 1, "an over-long name is truncated, not overrun");
}

/* ------------------------------------------------------------------ removal */

static void test_removal (void) {
    printf("\n-- removal and renumbering\n");

    fresh_storage();
    profile_load(root);
    profile_add();
    profile_add();
    profile_add();
    profile_set_name(0, "ANA");
    profile_set_name(1, "BEN");
    profile_set_name(2, "CAI");
    profile_set_name(3, "DEE");

    check(!profile_remove(0), "profile 1 cannot be removed");
    check(profile_count() == 4, "and the roster is untouched by the attempt");

    /* Active sits above the hole. It has to move down with its own data, or the console comes
     * back as whoever inherited the slot number. */
    profile_activate(3, &lib);
    check(profile_remove(1), "removing profile 2 succeeds");
    check(profile_count() == 3, "the roster closes the gap");
    check_str(profile_name(1), "CAI", "and everything above shifts down a slot");
    check(profile_active() == 2, "the active profile follows its own data down");
    check_str(profile_name(profile_active()), "DEE", "so it is still the same person");

    /* Active is the one deleted. There is nothing to follow, so it lands on profile 1 rather than
     * on whoever moved into the number. */
    profile_activate(1, &lib);
    check(profile_remove(1), "removing the active profile succeeds");
    check(profile_active() == 0, "and lands on profile 1, not on its replacement");

    check(profile_remove(1), "removing the last extra profile succeeds");
    check(!profile_remove(0), "and the roster cannot be emptied");
    check(profile_count() == 1, "one profile always remains");
}

/* ------------------------------------------------------------------ locks */

static void test_locks (void) {
    printf("\n-- locks round trip\n");

    fresh_storage();
    profile_load(root);
    cache_init(root);
    check(cache_writable(), "the test directory is writable, so the write half actually runs");

    build_library();
    records[0].flags |= LIBF_LOCKED;
    records[3].flags |= LIBF_LOCKED;
    records[5].flags |= LIBF_LOCKED;      /* the no-check-code record; keyed on its filename */

    check(locks_save(&lib), "three locks are written");
    clear_locks();
    check(locked_count() == 0, "the flags are cleared in memory");

    locks_load(&lib);
    /* Four, not three. Three keys were written, but the last two records share a filename and
     * neither has a check code, so the one key covers both -- see the collision check below. */
    check(locked_count() == 4, "all three keys come back, covering four records");
    check((records[0].flags & LIBF_LOCKED) != 0, "the first locked game is the same one");
    check((records[3].flags & LIBF_LOCKED) != 0, "so is the second");
    check((records[1].flags & LIBF_LOCKED) == 0, "and an unlocked game stays unlocked");
    check(!locks_dirty(), "a fresh load is not dirty");

    /* Records 5 and 4 both hash the same filename, so a lock on either applies to both. That is a
     * real property of playstate_key()'s fallback and it is asserted here rather than left to be
     * discovered: two identically-named ROMs in different folders share their lock. Better than
     * losing the lock whenever a file moves, which is what keying on the path would do. */
    check((records[4].flags & LIBF_LOCKED) != 0,
          "a lock keyed on a filename covers every record with that filename");

    /* Nothing locked must remove the file, not leave a stale one behind -- otherwise the next
     * boot re-locks games the parent just cleared. */
    clear_locks();
    check(locks_save(&lib), "saving with nothing locked succeeds");
    locks_load(&lib);
    check(locked_count() == 0, "and nothing comes back");

    printf("\n-- locks migration from playstate\n");

    /* A card written before locks.dat existed: no file, but playstate_load() has already put the
     * flags onto the records. locks_load() has to notice and mark itself dirty, or the locks stay
     * in a per-profile file forever and switching profile unlocks the card. */
    fresh_storage();
    cache_init(root);
    build_library();
    records[2].flags |= LIBF_LOCKED;
    locks_load(&lib);
    check(locks_dirty(), "no locks.dat plus locked records means a migration is pending");
    check(locked_count() == 1, "and the lock already on the record is left alone");

    fresh_storage();
    cache_init(root);
    build_library();
    locks_load(&lib);
    check(!locks_dirty(), "no locks.dat and nothing locked is simply a card with no locks");
}

int main (void) {
    const char *dir = getenv("TESTDIR");
    snprintf(root, sizeof(root), "%s/", dir ? dir : "build/hosttest/profiledir");

    printf("profile and locks round trip, storage at %s\n", root);

    test_paths();
    test_roster();
    test_removal();
    test_locks();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
