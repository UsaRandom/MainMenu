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

/* icon.c is not linked in: it is rdpq and libdragon all the way down. profile.c calls exactly one
 * function out of it -- icon_starter(), the per-slot default face -- so that is stubbed to return
 * the slot number, which makes "slot 4 gets starter 4" checkable and keeps the tests independent
 * of whatever the real corpus happens to contain. */
uint16_t icon_starter (int slot) { return (uint16_t)slot; }

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
    printf("\n-- removal\n");

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

    /* This section used to assert the opposite of every line below it. Removing a profile closed
     * the gap above it, so the checks here were "everything above shifts down a slot" and "the
     * active profile follows its own data down" -- both of which were true, and both of which
     * described the bug rather than a feature: the slot number names `saves/pN/`, so the shift
     * handed one player's saved games to another. The four checks that failed when the behaviour
     * changed are the four rewritten here.
     *
     * Active sits above the hole and must not move at all. */
    profile_activate(3, &lib);
    check(profile_remove(1), "removing profile 2 succeeds");
    check(profile_count() == 3, "the roster is one shorter");
    check(!profile_slot_used(1), "the slot it left is empty, not closed up");
    check_str(profile_name(2), "CAI", "nothing above it moved");
    check(profile_active() == 3, "and the active profile did not move either");
    check_str(profile_name(profile_active()), "DEE", "so it is still the same person");

    /* Active is the one deleted. There is nothing left to be, so it falls back to profile 1 --
     * the one slot guaranteed to exist. */
    profile_activate(2, &lib);
    check(profile_remove(2), "removing the active profile succeeds");
    check(profile_active() == 0, "and lands on profile 1");
    check(profile_slot_used(3), "the profile above the deleted one is still there");
    check_str(profile_name(3), "DEE", "under its own name and its own slot number");

    check(profile_remove(3), "removing the last extra profile succeeds");
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

/* ------------------------------------------------- format 2: slots do not move */

/**
 * Deleting a profile used to close the gap above it, and the slot number names `saves/pN/` -- so
 * deleting player 2 handed player 3's folder to player 2. This is the test that the shift is gone.
 * It cannot be reached from ares at all: profile_save() writes nothing there, so the roster never
 * survives to be re-read.
 */
static void test_stable_slots (void) {
    printf("\nstable slots\n");
    fresh_storage();
    profile_load(root);

    check(profile_add() == 1, "the second profile takes slot 2");
    check(profile_add() == 2, "the third takes slot 3");
    profile_set_name(1, "BEE");
    profile_set_name(2, "CEE");
    profile_save();

    check(profile_remove(1), "the middle profile is removed");
    check(!profile_slot_used(1), "its slot is now empty");
    check(profile_slot_used(2), "the slot above it still exists");
    check_str(profile_name(2), "CEE", "and still belongs to the same person");
    check(profile_count() == 2, "two profiles remain");

    /* The whole point, restated as the thing a user would notice. */
    profile_activate(2, NULL);
    check_str(profile_save_subdir(), "p3", "who keeps writing to saves/p3");

    check(profile_add() == 1, "a new profile fills the hole rather than appending");
    check_str(profile_name(1), "Player 2", "and comes up unnamed");

    /* Round trip, because the used flags are the new thing on disk. */
    profile_save();
    profile_load(root);
    check(profile_count() == 3, "three profiles after a reload");
    check_str(profile_name(2), "CEE", "the untouched profile survived the write");
}

/**
 * A card written by the format that had no version key. Slots 0..count-1 are in use and nothing
 * needs renumbering, which is exactly why the old layout could be adopted rather than converted --
 * and this is the check that adopting it does not move anybody.
 */
static void test_migration (void) {
    printf("\nreading a format 1 roster\n");
    fresh_storage();

    char path[600];
    snprintf(path, sizeof(path), "%smainmenu", root);
    directory_create(path);
    snprintf(path, sizeof(path), "%smainmenu/profiles.ini", root);
    FILE *f = fopen(path, "wb");
    check(f != NULL, "a version 1 profiles.ini can be written for the test");
    if (f != NULL) {
        /* No version key, no used flags, no icon, no colour. This is what every card that has
         * ever run this menu holds. */
        fputs("[profiles]\ncount=3\nactive=2\n"
              "[p1]\nname=ANN\n[p2]\nname=BEE\n[p3]\nname=CEE\n", f);
        fclose(f);
    }

    profile_load(root);
    check(profile_count() == 3, "all three profiles are found");
    check_str(profile_name(0), "ANN", "slot 1 keeps its name");
    check_str(profile_name(1), "BEE", "slot 2 keeps its name");
    check_str(profile_name(2), "CEE", "slot 3 keeps its name");
    check(profile_active() == 2, "the active profile is still the active profile");
    check_str(profile_save_subdir(), "p3", "and still writes to the same folder");
    check(profile_icon(1) == 1, "a profile with no icon gets its slot's default");
    check(!profile_slot_used(3), "nothing above the third is invented");

    /* Rewritten in format 2, and the names must survive that too -- this is the write that every
     * existing card gets on its first boot after the upgrade. */
    profile_save();
    profile_load(root);
    check_str(profile_name(1), "BEE", "the rewrite in the new format kept the middle name");
    check(profile_active() == 2, "and the active profile");
}

static void test_appearance (void) {
    printf("\nicon and colour\n");
    fresh_storage();
    profile_load(root);
    (void)profile_add();

    /* Named, not numbered. These were the literals 8 and 9, which were the two neutrals until the
     * palette lost its duplicate white and became nine colours -- at which point 9 was out of
     * range and profile_set_ink() ignored it, so five checks went red. Correctly red: the tests
     * were asserting the palette's size in passing, and this is what they meant to say. */
    const int WHITE = PROFILE_COLOUR_PAPER;
    const int BLACK = PROFILE_COLOUR_INK;

    profile_set_icon(0, 100);
    profile_set_plate(0, 3);
    profile_set_ink(0, WHITE);
    profile_set_icon(1, 100);
    profile_set_plate(1, 3);
    profile_set_ink(1, WHITE);

    check(profile_appearance_owner(100, 3, WHITE, 1) == 0, "a duplicate appearance names its owner");
    /* The plate and the artwork are separate choices now, so two profiles sharing a sprite AND a
     * plate are still distinguishable if the artwork on them differs. That is the whole reason
     * the uniqueness key grew a third field. */
    check(profile_appearance_owner(100, 3, BLACK, 1) == -1, "the same sprite and plate in other ink is free");
    check(profile_appearance_owner(100, 4, WHITE, 1) == -1, "the same sprite on another plate is free");
    check(profile_appearance_owner(100, 3, WHITE, 0) == 1, "the check ignores the slot being edited");
    /* Ten unconfigured slots must not all collide with each other, or the first edit is
     * impossible. */
    check(profile_appearance_owner(0xFFFF, 3, WHITE, 5) == -1, "an unchosen icon never collides");

    profile_set_plate(0, 99);
    check(profile_plate(0) == 3, "an out-of-range plate is ignored, not clamped");
    profile_set_ink(0, -1);
    check(profile_ink(0) == WHITE, "and so is an out-of-range ink");
    /* One past the end, which is the value a screen that still thinks the palette is ten long
     * would hand over. Nothing on screen would show a wrong colour -- profile_colour_fill() folds
     * it to red -- so the refusal has to be here or it is nowhere. */
    profile_set_ink(0, PROFILE_COLOURS);
    check(profile_ink(0) == WHITE, "an ink one past the palette is refused");
    check(profile_colour_name(PROFILE_COLOURS - 1)[0] != '\0', "every palette entry is named");

    /* The pairing that used to be hardcoded in the swatch table is now the default only. Dark
     * plates take the light neutral; light plates take the dark one. */
    check(profile_default_ink(0) == PROFILE_COLOUR_PAPER, "a dark plate defaults to light artwork");
    check(profile_default_ink(1) == PROFILE_COLOUR_INK, "a light plate defaults to dark artwork");

    profile_save();
    profile_load(root);
    check(profile_icon(1) == 100, "the icon survives a round trip");
    check(profile_plate(1) == 3, "so does the plate");
    check(profile_ink(1) == WHITE, "and so does the ink");
}

/**
 * A card written before the plate and the artwork could differ -- which is every card. The ink
 * key is absent, and reading it must reproduce exactly what the old fixed pairing drew, or every
 * profile on every existing card changes colour on the first boot after the upgrade.
 */
static void test_ink_default (void) {
    printf("\nreading a roster with no ink key\n");
    fresh_storage();

    char path[600];
    snprintf(path, sizeof(path), "%smainmenu", root);
    directory_create(path);
    snprintf(path, sizeof(path), "%smainmenu/profiles.ini", root);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fputs("[profiles]\nversion=2\ncount=2\nactive=0\n"
              "[p1]\nused=1\nname=ANN\nicon=5\ncolour=0\n"    /* red, a dark plate */
              "[p2]\nused=1\nname=BEE\nicon=6\ncolour=1\n",   /* amber, a light one */
              f);
        fclose(f);
    }

    profile_load(root);
    check(profile_plate(0) == 0, "the plate is read as written");
    check(profile_ink(0) == PROFILE_COLOUR_PAPER, "a dark plate gets light artwork");
    check(profile_ink(1) == PROFILE_COLOUR_INK, "a light plate gets dark artwork");

    profile_save();
    profile_load(root);
    check(profile_ink(0) == PROFILE_COLOUR_PAPER, "and the default is written down, not re-derived");
}

/**
 * The destructive path, against real files. This is the one thing in the program that cannot be
 * undone, and ares cannot execute a byte of it.
 */
/**
 * Ten slots, ten different default names.
 *
 * "Player 10" is nine characters and the fallback buffer was #PROFILE_NAME_CAP -- nine bytes,
 * eight characters and a terminator -- so snprintf truncated it to "Player 1". Slot 10 and slot 1
 * showed the same name on the same screen, on a screen whose entire job is telling ten people
 * apart. The cap is right for a *typed* name, which the keyboard limits to eight; the fallback is
 * neither typed nor stored and had no business sharing it.
 */
static void test_default_names (void) {
    printf("\nten slots, ten default names\n");
    fresh_storage();
    profile_load(root);
    for (int i = 1; i < PROFILE_MAX; i++) {
        (void)profile_add_at(i);
    }
    check(profile_count() == PROFILE_MAX, "all ten slots are occupied");

    static char seen[PROFILE_MAX][32];
    for (int i = 0; i < PROFILE_MAX; i++) {
        snprintf(seen[i], sizeof(seen[i]), "%s", profile_name(i));
    }
    check_str(seen[PROFILE_MAX - 1], "Player 10", "the tenth slot is called Player 10");

    int clashes = 0;
    for (int i = 0; i < PROFILE_MAX; i++) {
        for (int j = i + 1; j < PROFILE_MAX; j++) {
            if (strcmp(seen[i], seen[j]) == 0) {
                clashes++;
            }
        }
    }
    check(clashes == 0, "no two slots default to the same name");

    /* The rail reserves room for the longest of them, so the longest is worth pinning: nine
     * characters, and everything that draws a name is laid out against that. */
    size_t longest = 0;
    for (int i = 0; i < PROFILE_MAX; i++) {
        if (strlen(seen[i]) > longest) {
            longest = strlen(seen[i]);
        }
    }
    check(longest == 9, "the longest default name is nine characters");
}

static void test_save_erasure (void) {
    printf("\ndeleting a profile's saves\n");
    fresh_storage();
    profile_load(root);
    (void)profile_add();     /* slot 1 -> saves/p2 */

    /* Two ROM directories, each with a save for profile 2 and one for profile 1 that must not be
     * touched. The library is what says where to look, so it is built the same shape the scanner
     * produces. */
    char d[600];
    static lib_record_t recs[2];
    static char p0[600], p1[600];
    snprintf(d, sizeof(d), "%sroms/n64", root);        directory_create(d);
    snprintf(d, sizeof(d), "%sroms/n64/saves", root);  directory_create(d);
    snprintf(d, sizeof(d), "%sroms/n64/saves/p2", root); directory_create(d);
    snprintf(d, sizeof(d), "%sroms/snes", root);       directory_create(d);
    snprintf(d, sizeof(d), "%sroms/snes/saves", root); directory_create(d);
    snprintf(d, sizeof(d), "%sroms/snes/saves/p2", root); directory_create(d);

    const char *files[] = {
        "roms/n64/saves/p2/One.sav", "roms/n64/saves/p2/Two.sav",
        "roms/snes/saves/p2/Three.srm",
        "roms/n64/saves/Mine.sav",          /* profile 1's, and it must survive */
    };
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(d, sizeof(d), "%s%s", root, files[i]);
        FILE *f = fopen(d, "wb");
        if (f != NULL) { fputs("x", f); fclose(f); }
    }

    snprintf(p0, sizeof(p0), "%sroms/n64/Game.z64", root);
    snprintf(p1, sizeof(p1), "%sroms/snes/Other.sfc", root);
    recs[0].path = p0;
    recs[1].path = p1;
    library_t lib = { .records = recs, .count = 2 };

    check(profile_erase_saves(1, &lib, true) == 3, "the dry run counts three saves");
    snprintf(d, sizeof(d), "%sroms/n64/saves/p2/One.sav", root);
    check(access(d, F_OK) == 0, "and the dry run deleted nothing");

    check(profile_erase_saves(1, &lib, false) == 3, "the real run reports three");
    check(access(d, F_OK) != 0, "the save is gone");
    snprintf(d, sizeof(d), "%sroms/snes/saves/p2/Three.srm", root);
    check(access(d, F_OK) != 0, "including the one in the other directory");
    snprintf(d, sizeof(d), "%sroms/n64/saves/Mine.sav", root);
    check(access(d, F_OK) == 0, "profile 1's save is untouched");

    /* Player one owns the unsuffixed saves/ -- on a card older than this feature, every save on
     * it. This must refuse at the source and not merely be unreachable from the UI.
     *
     * Two things protect it and only one of them is the guard. The guard refuses slot 0 outright;
     * underneath that, slot 0's folder name would be `p1`, and nothing has ever written to
     * `saves/p1/`. That matters for how this is tested: relaxing the guard alone leaves the mutant
     * passing, because it then looks in a directory that does not exist. The mutation in
     * tools/hosttest/run.sh therefore also makes the path agree with profile_save_subdir(), which
     * is the bug somebody would really write -- and with both edits these three checks go red. */
    check(profile_erase_saves(0, &lib, true) == 0, "profile 1's saves cannot be counted");
    check(profile_erase_saves(0, &lib, false) == 0, "or erased");
    check(access(d, F_OK) == 0, "and are still there afterwards");
}

int main (void) {
    const char *dir = getenv("TESTDIR");
    snprintf(root, sizeof(root), "%s/", dir ? dir : "build/hosttest/profiledir");

    printf("profile and locks round trip, storage at %s\n", root);

    test_paths();
    test_roster();
    test_removal();
    test_stable_slots();
    test_migration();
    test_appearance();
    test_ink_default();
    test_default_names();
    test_save_erasure();
    test_locks();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
