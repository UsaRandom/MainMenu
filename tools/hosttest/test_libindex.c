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
    if (!libindex_load(b, root, "/")) {
        library_free(b);
        return NULL;
    }
    CHECK(b->count == n, "loaded index holds the same number of titles as the scan");
    return b;
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

    library_t *loaded = library_init();
    CHECK(libindex_load(loaded, root, "/"), "an untouched card loads its index");
    CHECK(loaded->count == scanned->count, "same title count");

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

    /* --- what MUST invalidate ------------------------------------------------------------- */
    /* Each case re-saves first, so it is testing its own change and not the one before it. */

    library_t *base = fresh_scan();
    CHECK(libindex_save(base, root, "/"), "baseline saved");
    library_free(base);
    put("roms/A/Cobra.z64", 1024);           /* a game added */
    l = library_init();
    CHECK(!libindex_load(l, root, "/"), "adding a ROM invalidates");
    library_free(l);

    base = fresh_scan(); libindex_save(base, root, "/"); library_free(base);
    rm("roms/A/Cobra.z64");                  /* a game removed */
    l = library_init();
    CHECK(!libindex_load(l, root, "/"), "deleting a ROM invalidates");
    library_free(l);

    base = fresh_scan(); libindex_save(base, root, "/"); library_free(base);
    put("roms/A/Alpha2.png", 300);           /* art added */
    l = library_init();
    CHECK(!libindex_load(l, root, "/"), "adding art invalidates, so new covers are picked up");
    library_free(l);
    rm("roms/A/Alpha2.png");

    /* The size case, and the reason it is here: a session concluded from hardware that size_sum
     * had silently stopped working, because replacing the menu ROM did not invalidate the index.
     * It had not stopped working -- the two ROMs were the same padded size. Same entry count,
     * different bytes, and the index must notice. */
    base = fresh_scan(); libindex_save(base, root, "/"); library_free(base);
    put("roms/B/Bravo.z64", 8192 + 512);
    l = library_init();
    CHECK(!libindex_load(l, root, "/"), "a file changing SIZE invalidates, at the same entry count");
    library_free(l);

    /* An EMPTY file: entry count +1, size_sum unchanged. The only signal is the entry count, so
     * this is the one case that tests it on its own -- adding a ROM moves both fields, and a
     * mutation removing the entry-count compare survived the whole file until this existed. */
    base = fresh_scan(); libindex_save(base, root, "/"); library_free(base);
    put("roms/A/notes.txt", 0);
    l = library_init();
    CHECK(!libindex_load(l, root, "/"), "a zero-byte file invalidates on entry count alone");
    library_free(l);
    rm("roms/A/notes.txt");

    base = fresh_scan(); libindex_save(base, root, "/"); library_free(base);
    mk("roms/C");                            /* a directory added */
    put("roms/C/Charlie.z64", 2048);
    l = library_init();
    CHECK(!libindex_load(l, root, "/"), "adding a directory invalidates");
    library_free(l);

    /* --- a card with no index at all ------------------------------------------------------ */
    char idx[700];
    snprintf(idx, sizeof(idx), "%s/mainmenu/cache/library.idx", root);
    remove(idx);
    l = library_init();
    CHECK(!libindex_load(l, root, "/"), "a missing index is a miss, not a crash");
    library_free(l);

    printf("  %d checks passed\n", checks);
    return 0;
}
