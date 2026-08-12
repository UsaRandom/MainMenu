/* The loose-art table: sort, deduplicate, bsearch.
 *
 * This code had no coverage anywhere when the table went from a linear walk to a sorted bsearch.
 * The walk was O(n^2) on both halves -- art_push() compared against every entry already held, and
 * library_find_art() compared against every entry on every lookup -- which a 278-cover card made
 * expensive enough to notice on the boot path.
 *
 * The risky part of that change is not the search. It is that deduplication moved from push time
 * to sort time, and "first seen wins, so the shallowest wins" is a promise about ORDER that a
 * sort is free to destroy. qsort is not stable, so the survivor is pinned by an explicit seq
 * field instead. That is what most of this file is about.
 *
 * The scan is driven over a real directory tree rather than by calling art_push() directly (it is
 * static, and reaching it would mean testing a copy rather than the thing that ships). So these
 * checks also cover traversal order, the leading-dot skip and the skip-directory list, which is
 * where "shallowest" is actually decided.
 */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "library.h"

static int checks = 0;

#define CHECK(cond, what) do {                                                  \
    checks++;                                                                   \
    if (!(cond)) {                                                              \
        fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));      \
        exit(1);                                                                \
    }                                                                           \
} while (0)

/* --- the five symbols library.c needs that the art table never reaches -------------------- */

path_t *path_create (const char *p) { (void)p; return NULL; }
void path_free (path_t *p) { (void)p; }

/* A tree of .png files reaches none of these: rom_config_load() is called only for a name that
 * classify() accepted as a ROM. If one of these ever fires, the test tree has grown a ROM and the
 * check below about art_count is measuring something else. */
static int rom_calls = 0;
rom_err_t rom_config_load (path_t *p, rom_info_t *ri) { (void)p; (void)ri; rom_calls++; return 0; }
void rom_info_free_meta (rom_info_t *ri) { (void)ri; }
rom_save_type_t rom_info_get_save_type (rom_info_t *ri) { (void)ri; rom_calls++; return 0; }

/* Installed by scan_dir() around each directory. The stub records the last listing it was handed
 * so the checks below can prove the scan clears it before recursing -- a child answering "is there
 * a sidecar" from its parent's names would find the wrong file, or miss the right one. */
static int listing_count_seen = -1;
static int listing_cleared = 0;
static int listing_max = 0;
void rom_info_set_dir_listing (const char *const *names, int count) {
    if (names == NULL) { listing_cleared++; }
    if (names != NULL && count > listing_max) { listing_max = count; }
    listing_count_seen = (names != NULL) ? count : 0;
}

/* ------------------------------------------------------------------------------------------ */

static char root[512];

static void mkdirp (const char *rel) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    mkdir(p, 0755);
}

static void touch (const char *rel) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    FILE *f = fopen(p, "wb");
    assert(f != NULL);
    fputc('x', f);
    fclose(f);
}

/* Enough duplicate keys, at enough depth, that qsort has a real chance to reorder equal elements.
 * With only one duplicate in a six-entry array most qsort implementations fall back to insertion
 * sort and stay stable by accident -- which made a mutation that deletes the seq tiebreak survive
 * the first version of this test. */
#define DUPES 24

static void build_tree (void) {
    mkdirp("");
    /* Same key at two depths. The shallow one is written second on purpose: if the survivor were
     * decided by file creation order or by whatever order readdir happens to return, this would
     * pass by luck. It has to be decided by scan depth. */
    mkdirp("deep");
    touch("deep/Dupe.png");
    touch("Dupe.png");

    touch("Alpha.png");
    touch("beta.PNG");          /* extension case */
    touch("GAMMA.Jpeg");        /* different extension, and name case */
    touch("delta.jpg");

    /* Skipped: leading dot, and a directory on the skip list. Neither may reach the table. */
    touch(".hidden.png");
    mkdirp("metadata");
    touch("metadata/boxart_front.png");
    mkdirp("saves");
    touch("saves/Alpha.png");

    /* A block of keys that each exist twice, once at the root and once a level down. */
    mkdirp("dup2");
    for (int i = 0; i < DUPES; i++) {
        char n[64];
        snprintf(n, sizeof(n), "Multi%02d.png", i);
        touch(n);
        char sub[80];
        snprintf(sub, sizeof(sub), "dup2/%s", n);
        touch(sub);
    }
}

static void expect_art (library_t *lib, const char *name, const char *want_suffix) {
    const char *got = library_find_art(lib, name);
    char msg[256];
    snprintf(msg, sizeof(msg), "find_art(\"%s\")", name);
    CHECK(got != NULL, msg);
    size_t gl = strlen(got), wl = strlen(want_suffix);
    CHECK(gl >= wl && strcmp(got + (gl - wl), want_suffix) == 0, msg);
}

int main (void) {
    const char *dir = getenv("TESTDIR");
    assert(dir != NULL);
    snprintf(root, sizeof(root), "%s", dir);
    build_tree();

    library_t *lib = library_init();
    CHECK(lib != NULL, "library_init");
    /* (storage_prefix, root), joined by library_join() -- the same split app.c uses, where the
     * prefix is the card and the root is "/". Passing the temp dir as both concatenates it. */
    library_scan(lib, root, "/", NULL);

    CHECK(rom_calls == 0, "a .png tree must not reach the ROM loader");

    /* Six files pushed, one of them a duplicate key. The scan does not deduplicate -- the sort
     * does, and the sort is lazy, so the table is still six entries long at this point. Asserting
     * five here was this test's own first failure, and the check is kept in both positions
     * because "deduplicates on lookup rather than on push" is the whole design and a silent
     * change back to eager dedupe should break something. */
    CHECK(lib->art_count == 6 + 2 * DUPES, "art_count before the first lookup: not yet deduplicated");

    /* Lookups are by bare name, any extension, any case. The first of these triggers the sort. */
    expect_art(lib, "Alpha.png", "/Alpha.png");
    expect_art(lib, "alpha", "/Alpha.png");
    expect_art(lib, "ALPHA.z64", "/Alpha.png");
    expect_art(lib, "beta.z64", "/beta.PNG");
    expect_art(lib, "gamma", "/GAMMA.Jpeg");
    expect_art(lib, "delta", "/delta.jpg");

    /* Five named keys plus the DUPES block, each collapsed to one. */
    CHECK(lib->art_count == 5 + DUPES, "art_count after the sort deduplicated");

    /* The seq tiebreak, which is the whole reason art_entry_t carries a seq at all: with it the
     * survivor is the first copy the scan pushed, and files-before-subdirectories makes that the
     * root copy every time. Without the tiebreak, qsort decides among equal keys and qsort is not
     * stable -- which is why there are 24 of these and not one. A single duplicate in a six-entry
     * array leaves most qsort implementations stable by accident, and the mutation survived. */
    for (int i = 0; i < DUPES; i++) {
        char stem[64], want[80], msg[256];
        snprintf(stem, sizeof(stem), "Multi%02d", i);
        snprintf(want, sizeof(want), "/%s.png", stem);
        const char *got = library_find_art(lib, stem);
        snprintf(msg, sizeof(msg), "duplicate %s resolves to the shallow copy (got %s)",
                 stem, got ? got : "(null)");
        CHECK(got != NULL, msg);
        CHECK(strstr(got, "/dup2/") == NULL, msg);
        size_t gl = strlen(got), wl = strlen(want);
        CHECK(gl >= wl && strcmp(got + (gl - wl), want) == 0, msg);
    }

    /* The listing is lent per directory and cleared before recursing. Without the clear, a
     * subdirectory's ROMs would be checked for sidecars against their parent's file names. */
    /* Lent at all, and with the root's real entry count. Skipping the loan changes no behaviour
     * -- sidecar_possible() answers "maybe" without one and every probe happens as before -- so
     * nothing else in this file can notice its absence. Only the cost changes, which is the whole
     * point of it, so the check has to be on the loan itself. */
    /* The root's own entries: Dupe/Alpha/beta/GAMMA/delta, the four subdirectories, and the
     * DUPES block. Each directory is lent separately, so the largest loan is the root's. */
    CHECK(listing_max == 5 + 4 + DUPES, "the directory listing is lent to rom_info, in full");
    CHECK(listing_cleared > 0, "scan_dir clears the lent listing before it recurses");
    CHECK(listing_count_seen == 0, "and the last thing it does with it is clear it");

    /* Exclusions. */
    CHECK(library_find_art(lib, "hidden") == NULL, "leading-dot art is not indexed");
    CHECK(library_find_art(lib, "boxart_front") == NULL, "metadata/ is not walked");

    /* Misses, including ones that bracket the sorted array at both ends -- a bsearch that gets
     * its bounds wrong tends to work in the middle and fail at the edges. */
    CHECK(library_find_art(lib, "aaaaaa") == NULL, "miss below the first key");
    CHECK(library_find_art(lib, "zzzzzz") == NULL, "miss above the last key");
    CHECK(library_find_art(lib, "cee") == NULL, "miss between two keys");
    CHECK(library_find_art(lib, "") == NULL, "empty name");
    CHECK(library_find_art(NULL, "alpha") == NULL, "null library");

    /* Every key present must be findable. A sort that loses an entry, or a compaction that
     * overwrites one, shows up here and nowhere else. */
    for (int i = 0; i < lib->art_count; i++) {
        CHECK(library_find_art(lib, lib->art[i].key) != NULL, "every key is findable");
    }

    /* Sorted and unique, which is the precondition the bsearch is entitled to assume. */
    for (int i = 1; i < lib->art_count; i++) {
        CHECK(strcmp(lib->art[i - 1].key, lib->art[i].key) < 0, "keys strictly ascending");
    }

    /* A name longer than the stack key buffer must miss rather than truncate into a false hit.
     * Truncation would make a 300-character name match a 255-character one. */
    char huge[600];
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    CHECK(library_find_art(lib, huge) == NULL, "over-long name is rejected, not truncated");

    library_free(lib);
    printf("  %d checks passed\n", checks);
    return 0;
}
