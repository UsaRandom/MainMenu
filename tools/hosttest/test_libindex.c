/* The library index: round trip, and exactly what does and does not invalidate it.
 *
 * libindex.c had no coverage of any kind -- AUDIT lists it, with playstate.c and cheatstate.c, as
 * verified only by compile-time size assertions. It is also the file that decides whether a boot
 * costs 0.72 seconds or 14.4, and the next change to it is an incremental revalidation that
 * rebuilds only the directories that moved. That change cannot be made safely against no test:
 * its failure mode is not a crash, it is a library that is quietly missing a game, written back
 * to the card, and still wrong on the next boot.
 *
 * So this pins the behaviour that exists first. Every check here passes against the version of
 * libindex.c that predates incremental revalidation, on purpose -- a test written at the same time
 * as the change it guards proves only that they agree with each other.
 *
 * The staleness matrix is the valuable half. Several of these cases were argued about from the
 * source during a hardware session and got answered wrongly twice: a saves/ folder appearing was
 * believed to invalidate the index (it does not), and the signature was believed to have degraded
 * to entry-count-only because a 3.4 MB ROM change did not invalidate it (it had not -- the two
 * builds were byte-identical in size). Both are now questions a test answers in a second.
 */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdbool.h>

#include "cache.h"
#include "library.h"
#include "libindex.h"

static int checks = 0;

#define CHECK(cond, what) do {                                                  \
    checks++;                                                                   \
    if (!(cond)) {                                                              \
        fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));      \
        exit(1);                                                                \
    }                                                                           \
} while (0)

/* --- what library.c needs that the index never reaches ------------------------------------ */

path_t *path_create (const char *p) { (void)p; return NULL; }
void path_free (path_t *p) { (void)p; }
rom_err_t rom_config_load (path_t *p, rom_info_t *ri) { (void)p; (void)ri; return 0; }
void rom_info_free_meta (rom_info_t *ri) { (void)ri; }
rom_save_type_t rom_info_get_save_type (rom_info_t *ri) { (void)ri; return 0; }
void rom_info_set_dir_listing (const char *const *n, int c) { (void)n; (void)c; }

/* Not in fs_probe.c on purpose -- the shim's header says each test decides what "create a
 * directory" means for it. cache_init() calls this to make mainmenu/cache, so it has to build
 * intermediate components. */
bool directory_create (char *path) {
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    return false;                 /* false is success here; see utils/fs.h */
}

/* directory_exists() comes from fs_probe.c; only directory_create() is per-test. */

/* ------------------------------------------------------------------------------------------ */

static char root[512];

static void mk (const char *rel) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    mkdir(p, 0755);
}

static void put (const char *rel, int bytes) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    FILE *f = fopen(p, "wb");
    assert(f != NULL);
    for (int i = 0; i < bytes; i++) {
        fputc('z', f);
    }
    fclose(f);
}

static void rm (const char *rel) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    remove(p);
}

/** @brief Delete a directory and everything directly in it. One level is all these trees need. */
static void rmdir_all (const char *rel) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    DIR *d = opendir(p);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') {
                continue;
            }
            char f[1000];
            snprintf(f, sizeof(f), "%s/%s", p, e->d_name);
            remove(f);
        }
        closedir(d);
    }
    rmdir(p);
}

/** @brief A tree with two letter folders, art beside the ROMs, and both skipped directories. */
static void build_tree (void) {
    mk("");
    mk("roms");
    mk("roms/A");
    put("roms/A/Alpha.z64", 2048);
    put("roms/A/Alpha.png", 300);
    put("roms/A/Ant.z64", 4096);
    mk("roms/B");
    put("roms/B/Bravo.z64", 8192);
    /* Both of these are on library.c's skip list and must be invisible to the signature. */
    mk("mainmenu");
    put("mainmenu/config.ini", 64);
    mk("roms/A/saves");
    put("roms/A/saves/Alpha.sav", 512);
}

static library_t *fresh_scan (void) {
    library_t *lib = library_init();
    assert(lib != NULL);
    library_scan(lib, root, "/", NULL);
    library_resolve_loose_art(lib);
    return lib;
}

/** @brief Scan, save, then load into a second library. @return the loaded one, or NULL on miss. */
static library_t *save_then_load (void) {
    library_t *a = fresh_scan();
    CHECK(libindex_save(a, root, "/"), "index saves");
    int n = a->count;
    library_free(a);

    library_t *b = library_init();
    assert(b != NULL);
    if (!libindex_load(b, root, "/", NULL, NULL)) {
        library_free(b);
        return NULL;
    }
    CHECK(b->count == n, "loaded index holds the same number of titles as the scan");
    return b;
}

/** @brief Scan the tree as it stands now and write that as the index the next load will meet. */
static void baseline (void) {
    library_t *a = fresh_scan();
    CHECK(libindex_save(a, root, "/"), "baseline index written");
    library_free(a);
}

/** @brief Load, reporting what the load did. @return the library, or NULL if it refused. */
static library_t *load_reporting (libindex_result_t *res) {
    library_t *l = library_init();
    assert(l != NULL);
    if (!libindex_load(l, root, "/", NULL, res)) {
        library_free(l);
        return NULL;
    }
    return l;
}

/**
 * @brief Assert that @p got holds exactly what a full scan of the same tree would have.
 *
 * This is the check the whole incremental path stands on, and it is deliberately not a count. A
 * repair that keeps the wrong records still produces a plausible number of titles -- the failure
 * this file exists to catch is a library that is subtly, quietly not the library the card
 * describes, written back over the good one. So every record is compared field by field, in
 * order, against a scan that read the whole card.
 *
 * art_state and art_age are not compared, and that is not an omission: neither is persisted, on
 * purpose. ART_READY names a slot in a RAM pool that does not exist at load time, and a cached
 * record must not replay the arrival animation. See libindex.c.
 */
static void same_as_full_scan (library_t *got, const char *what) {
    library_t *want = fresh_scan();
    char msg[256];

    snprintf(msg, sizeof(msg), "%s: title count matches a full scan", what);
    CHECK(got->count == want->count, msg);

    for (int i = 0; i < got->count && i < want->count; i++) {
        const lib_record_t *g = &got->records[i], *w = &want->records[i];
        snprintf(msg, sizeof(msg), "%s: record %d is the same game a scan found", what, i);
        CHECK(strcmp(g->path ? g->path : "", w->path ? w->path : "") == 0, msg);
        CHECK(strcmp(g->title ? g->title : "", w->title ? w->title : "") == 0, msg);
        CHECK(g->system == w->system, msg);
        CHECK(g->check_code == w->check_code, msg);
        CHECK(g->save_type == w->save_type, msg);
        CHECK(g->feat == w->feat, msg);
        CHECK(g->flags == w->flags, msg);
        CHECK(g->art_kind == w->art_kind, msg);
        snprintf(msg, sizeof(msg), "%s: record %d resolves the same art a scan does", what, i);
        CHECK(strcmp(g->art_file ? g->art_file : "", w->art_file ? w->art_file : "") == 0, msg);
    }
    library_free(want);
}

int main (void) {
    const char *dir = getenv("TESTDIR");
    assert(dir != NULL);
    snprintf(root, sizeof(root), "%s", dir);
    build_tree();

    /* Before anything touches the index: cache_init() is what resolves mainmenu/cache and runs
     * the writability probe that libindex_save() gates on. Without it every save silently does
     * nothing, which is exactly the shape of failure this file exists to catch on a real card. */
    cache_init(root);
    CHECK(cache_writable(), "the test directory is writable, so the write half actually runs");

    /* --- round trip ----------------------------------------------------------------------- */
    library_t *scanned = fresh_scan();
    CHECK(scanned->count == 3, "three ROMs are indexed (art and saves are not titles)");

    CHECK(libindex_save(scanned, root, "/"), "save succeeds");

    libindex_result_t res;
    library_t *loaded = library_init();
    CHECK(libindex_load(loaded, root, "/", NULL, &res), "an untouched card loads its index");
    CHECK(loaded->count == scanned->count, "same title count");
    CHECK(!res.incremental, "an untouched card is believed as it stands, not repaired");
    CHECK(res.dirs_rescanned == 0, "and nothing is read off the card again");

    /* Field-for-field, because the index is a serialisation and the failure that matters is a
     * field landing in the wrong place -- which a count comparison cannot see. */
    for (int i = 0; i < scanned->count; i++) {
        const lib_record_t *s = &scanned->records[i], *l = &loaded->records[i];
        CHECK(strcmp(s->path, l->path) == 0, "path survives the round trip");
        CHECK(strcmp(s->title, l->title) == 0, "title survives");
        CHECK(s->system == l->system, "system survives");
        CHECK(s->check_code == l->check_code, "check code survives");
        CHECK(s->art_kind == l->art_kind, "art shape survives");
        bool both_null = (s->art_file == NULL) == (l->art_file == NULL);
        CHECK(both_null, "art presence survives");
        if (s->art_file != NULL) {
            CHECK(strcmp(s->art_file, l->art_file) == 0, "resolved art path survives");
        }
    }
    /* The art path is the expensive answer to reach, so prove one really is carried. */
    int with_art = 0;
    for (int i = 0; i < loaded->count; i++) {
        if (loaded->records[i].art_file != NULL) {
            with_art++;
        }
    }
    CHECK(with_art == 1, "the one cover in the tree is carried in the index");
    library_free(scanned);
    library_free(loaded);

    /* --- abandoning a half-built library ---------------------------------------------------
     *
     * The repair pushes records before it can be sure of finishing, and gives up by emptying the
     * library so the caller's full scan starts from nothing. The give-up itself is an allocation
     * failure, which nothing here can provoke -- so the primitive it leans on is tested directly
     * instead, including the thing that goes wrong if it only half works: a scan into a library
     * that was not really emptied lists every game twice. */
    library_t *reused = fresh_scan();
    CHECK(reused->count == 3, "a library with something in it");
    CHECK(reused->art_count > 0, "and a loose-art table with something in it");
    library_clear(reused);
    CHECK(reused->count == 0, "clearing empties the records");
    CHECK(reused->art_count == 0, "and the art table with them");
    library_scan(reused, root, "/", NULL);
    CHECK(reused->count == 3, "so a scan into the same library does not double its contents");
    library_free(reused);

    /* --- what must NOT invalidate --------------------------------------------------------- */
    library_t *l;

    put("mainmenu/config.ini", 4096);        /* the menu writing its own settings */
    l = save_then_load();
    CHECK(l != NULL, "a changed file under mainmenu/ does not invalidate");
    library_free(l);

    put("roms/A/saves/Alpha.sav", 32768);    /* a game saving */
    l = save_then_load();
    CHECK(l != NULL, "a save being written does not invalidate");
    library_free(l);

    mk("roms/B/saves");                      /* first save in a folder that had none */
    put("roms/B/saves/Bravo.sav", 512);
    l = save_then_load();
    CHECK(l != NULL, "a saves/ folder appearing does not invalidate");
    library_free(l);

    /* --- what MUST be noticed --------------------------------------------------------------
     *
     * These cases used to assert that libindex_load() REFUSED. It does not any more: noticing a
     * change and throwing the whole index away were one act, and incremental revalidation splits
     * them -- the signature still has to notice, and what follows is a repair rather than a
     * 14-second rescan of a card where one folder moved.
     *
     * So the assertion moved rather than weakened. `incremental` is exactly the old question: if
     * the signature had failed to notice, the load would return true with it false and a library
     * that is silently out of date. And each case additionally checks the repaired library against
     * a full scan of the same tree, which the old refusal-based test could not do at all.
     *
     * Each case re-saves first, so it is testing its own change and not the one before it. */

    baseline();
    put("roms/A/Cobra.z64", 1024);           /* a game added */
    l = load_reporting(&res);
    CHECK(l != NULL, "adding a ROM is repaired, not refused");
    CHECK(res.incremental, "adding a ROM is noticed");
    CHECK(res.dirs_rescanned == 1, "and only the folder it went into is read again");
    CHECK(res.records_kept == 1, "the game in the untouched folder comes out of the file");
    CHECK(res.records_scanned == 3, "the touched folder's three are read off the card");
    CHECK(l->count == 4, "the new game is in the library");
    same_as_full_scan(l, "ROM added");
    library_free(l);

    /* Straight after the repair, with nothing else touched: the repaired index must have been
     * written back. If it was not, this is a second repair rather than a plain hit -- and every
     * boot from here on would rescan the same folder for ever. */
    l = load_reporting(&res);
    CHECK(l != NULL, "the repaired index loads");
    CHECK(!res.incremental, "the repair was written back, so the next boot is a plain hit");
    CHECK(l->count == 4, "and it holds the repaired library");
    library_free(l);

    baseline();
    rm("roms/A/Cobra.z64");                  /* a game removed */
    l = load_reporting(&res);
    CHECK(res.incremental, "deleting a ROM is noticed");
    CHECK(l->count == 3, "and the game is gone from the library");
    same_as_full_scan(l, "ROM deleted");
    library_free(l);

    baseline();
    put("roms/A/Alpha2.png", 300);           /* art added */
    l = load_reporting(&res);
    CHECK(res.incremental, "adding art is noticed, so new covers are picked up");
    same_as_full_scan(l, "art added");
    library_free(l);
    rm("roms/A/Alpha2.png");

    /* The size case, and the reason it is here: a session concluded from hardware that size_sum
     * had silently stopped working, because replacing the menu ROM did not invalidate the index.
     * It had not stopped working -- the two ROMs were the same padded size. Same entry count,
     * different bytes, and the index must notice. */
    baseline();
    put("roms/B/Bravo.z64", 8192 + 512);
    l = load_reporting(&res);
    CHECK(res.incremental, "a file changing SIZE is noticed, at the same entry count");
    same_as_full_scan(l, "ROM replaced with a different build");
    library_free(l);

    /* An EMPTY file: entry count +1, size_sum unchanged. The only signal is the entry count, so
     * this is the one case that tests it on its own -- adding a ROM moves both fields, and a
     * mutation removing the entry-count compare survived the whole file until this existed. */
    baseline();
    put("roms/A/notes.txt", 0);
    l = load_reporting(&res);
    CHECK(res.incremental, "a zero-byte file is noticed on entry count alone");
    same_as_full_scan(l, "zero-byte file added");
    library_free(l);
    rm("roms/A/notes.txt");

    baseline();
    mk("roms/C");                            /* a directory added */
    put("roms/C/Charlie.z64", 2048);
    l = load_reporting(&res);
    CHECK(res.incremental, "adding a directory is noticed");
    /* Two, and this is the case that shows why every changed directory is scanned rather than
     * only the deepest: roms/ changed because it gained an entry, and roms/C is new. Scanning
     * roms/ alone would miss Charlie; scanning roms/ RECURSIVELY would re-index A and B, whose
     * records the index still holds and whose signatures still match. */
    CHECK(res.dirs_rescanned == 2, "the parent and the new directory, and nothing else");
    CHECK(l->count == 4, "the game in the new directory is indexed");
    same_as_full_scan(l, "directory added");
    library_free(l);

    baseline();
    rmdir_all("roms/C");                     /* the whole directory removed */
    l = load_reporting(&res);
    CHECK(res.incremental, "removing a directory is noticed");
    CHECK(l->count == 3, "its games leave the library with it");
    same_as_full_scan(l, "directory removed");
    library_free(l);

    /* A rename is a delete and an add at once, and it is the case where matching directories by
     * POSITION rather than by path hash quietly falls apart: every directory after the renamed one
     * shifts, so a positional compare declares the whole tail changed and rescans it. */
    baseline();
    mk("roms/Bee");
    put("roms/Bee/Bravo.z64", 8192 + 512);
    rm("roms/B/saves/Bravo.sav");
    rmdir_all("roms/B/saves");
    rmdir_all("roms/B");
    l = load_reporting(&res);
    CHECK(res.incremental, "renaming a directory is noticed");
    CHECK(res.records_kept == 2, "the folders that did not move keep their records");
    same_as_full_scan(l, "directory renamed");
    library_free(l);

    /* Art that does NOT live beside its ROM. The repair only reads the directories that moved, so
     * without the signature walk feeding the loose-art table this cover is invisible: the new game
     * would resolve to nothing, be written into the index as LIBF_ART_MISSING, and stay wrong on
     * every later boot because the directories would then match. */
    mk("roms/covers");
    put("roms/covers/Delta.png", 300);
    baseline();
    put("roms/A/Delta.z64", 4096);
    l = load_reporting(&res);
    CHECK(res.incremental, "the folder the game went into is noticed");
    CHECK(res.dirs_rescanned == 1, "and the folder holding the cover is not read again");
    const char *delta_art = NULL;
    for (int i = 0; i < l->count; i++) {
        if (l->records[i].title != NULL && strcmp(l->records[i].title, "Delta") == 0) {
            delta_art = l->records[i].art_file;
        }
    }
    CHECK(delta_art != NULL, "a cover kept in another directory is still found");
    same_as_full_scan(l, "art in a different directory");
    library_free(l);

    /* A title that has already recorded having no art, meeting a cover that turns up later in a
     * directory it does not live in. LIBF_ART_MISSING is the persisted "we looked and there was
     * nothing", and it is the whole reason a card with no art pack does not repeat the five-rule
     * search on every boot -- so a record that gains a path has to lose the flag in the same
     * breath. Written back with both, it loads next boot as ART_NONE holding a perfectly good art
     * path that nothing will ever decode. Straight after a scan the flag is always clear, which is
     * why this could not happen before a repair could run against records read from the file. */
    library_t *marked = fresh_scan();
    for (int i = 0; i < marked->count; i++) {
        if (strcmp(marked->records[i].title, "Ant") == 0) {
            marked->records[i].art_state = ART_NONE;
        }
    }
    CHECK(libindex_save(marked, root, "/"), "an index recording a title as having no art");
    library_free(marked);

    put("roms/covers/Ant.png", 300);
    l = load_reporting(&res);
    CHECK(res.incremental, "the folder the cover landed in is noticed");
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->records[i].title, "Ant") != 0) {
            continue;
        }
        CHECK(l->records[i].art_file != NULL, "the cover reaches the record that had none");
        CHECK((l->records[i].flags & LIBF_ART_MISSING) == 0, "and the no-art flag comes off");
        CHECK(l->records[i].art_state == ART_PENDING, "so the tile will actually decode it");
    }
    same_as_full_scan(l, "cover appears for a title recorded as having none");
    library_free(l);

    /* --- the bottom of the tree --------------------------------------------------------------
     *
     * The scan indexes LIBRARY_SCAN_MAX_DEPTH levels below the root and the signature walk stopped
     * one level further down, under a comment saying the two matched. Harmless while the answer
     * was yes-or-no: a change down there invalidated an index that could not have held anything
     * from there, and the rescan found nothing new. Incremental revalidation turns it into a
     * library that disagrees with the card -- the deep directory is handed to library_scan_dir()
     * and its games indexed, so a repaired card holds titles a full rescan of the same card does
     * not, and it writes that back.
     *
     * This is the case for it, and it earns its place: with the walk restored to depth 6 every
     * other check in this file still passes. */
    mk("deep");
    mk("deep/b");
    mk("deep/b/c");
    mk("deep/b/c/d");
    mk("deep/b/c/d/e");                      /* depth 5 -- the last level the scan indexes */
    mk("deep/b/c/d/e/f");                    /* depth 6 -- below it */
    put("deep/b/c/d/e/Edge.z64", 2048);
    put("deep/b/c/d/e/f/Under.z64", 2048);

    library_t *deep = fresh_scan();
    bool saw_edge = false, saw_under = false;
    for (int i = 0; i < deep->count; i++) {
        if (strcmp(deep->records[i].title, "Edge") == 0)  saw_edge = true;
        if (strcmp(deep->records[i].title, "Under") == 0) saw_under = true;
    }
    CHECK(saw_edge, "a scan reaches the deepest level it claims to");
    CHECK(!saw_under, "and stops there");
    library_free(deep);

    baseline();
    put("deep/b/c/d/e/f/Under2.z64", 2048);
    l = load_reporting(&res);
    CHECK(l != NULL, "a change below the scan's reach still loads");
    CHECK(!res.incremental, "and is not even noticed, because nothing down there is ever indexed");
    same_as_full_scan(l, "change below the scan's reach");
    library_free(l);

    rm("deep/b/c/d/e/f/Under.z64");
    rm("deep/b/c/d/e/f/Under2.z64");
    rm("deep/b/c/d/e/Edge.z64");
    rmdir_all("deep/b/c/d/e/f");
    rmdir_all("deep/b/c/d/e");
    rmdir_all("deep/b/c/d");
    rmdir_all("deep/b/c");
    rmdir_all("deep/b");
    rmdir_all("deep");

    /* --- a card with no index at all ------------------------------------------------------ */
    char idx[700];
    snprintf(idx, sizeof(idx), "%s/mainmenu/cache/library.idx", root);
    remove(idx);
    l = library_init();
    CHECK(!libindex_load(l, root, "/", NULL, NULL), "a missing index is a miss, not a crash");
    library_free(l);

    /* Nothing recognisable left. Two new directories rather than one, so the ROOT's entry count
     * moves as well -- with one the root would still match, the repair would keep nothing, rescan
     * everything and be right anyway. This is the case where there is no partial answer at all and
     * the caller has to be told to scan, rather than handed an empty library and believed. */
    baseline();
    mk("elsewhere");
    mk("alsohere");
    put("elsewhere/Echo.z64", 2048);
    rmdir_all("roms/A/saves");
    rmdir_all("roms/A");
    rmdir_all("roms/Bee");
    rmdir_all("roms/covers");
    rmdir_all("roms");
    l = library_init();
    CHECK(!libindex_load(l, root, "/", NULL, &res),
          "a card with nothing left to keep falls back to a full scan");
    CHECK(!res.incremental, "and says so");
    CHECK(l->count == 0, "leaving the library untouched for the scan to fill");
    library_free(l);

    printf("  %d checks passed\n", checks);
    return 0;
}
