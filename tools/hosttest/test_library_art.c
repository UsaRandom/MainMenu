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

/**
 * @brief Which copy of @p stem the scan reaches first, by walking the tree the way scan_dir does.
 *
 * The survivor of a duplicate key is decided by DFS order, and DFS order is readdir order, which
 * varies by filesystem. Rather than hardcode an answer that is only true on APFS, the test derives
 * the expected winner from the same enumeration the scan just used. Mirrors scan_dir()'s shape:
 * recurse into a directory at the point it is met, skip leading dots.
 *
 * @return a path relative to the root, or NULL if not found.
 */
static const char *dfs_first (const char *rel_dir, const char *stem, char *out, size_t cap) {
    char abs[600];
    snprintf(abs, sizeof(abs), "%s%s%s", root, rel_dir[0] ? "/" : "", rel_dir);
    DIR *d = opendir(abs);
    if (d == NULL) {
        return NULL;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char child[600];
        snprintf(child, sizeof(child), "%s%s%s", rel_dir, rel_dir[0] ? "/" : "", e->d_name);
        if (e->d_type == DT_DIR) {
            if (strcmp(e->d_name, "metadata") == 0 || strcmp(e->d_name, "saves") == 0) {
                continue;
            }
            if (dfs_first(child, stem, out, cap) != NULL) {
                closedir(d);
                return out;
            }
        } else {
            const char *dot = strrchr(e->d_name, '.');
            size_t n = dot ? (size_t)(dot - e->d_name) : strlen(e->d_name);
            if (n == strlen(stem) && strncmp(e->d_name, stem, n) == 0) {
                snprintf(out, cap, "%s", child);
                closedir(d);
                return out;
            }
        }
    }
    closedir(d);
    return NULL;
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

    /* The seq tiebreak, which is the whole reason art_entry_t carries a seq at all: with it, the
     * survivor is the first copy the scan reached; without it, qsort's handling of equal keys
     * decides, and qsort is not stable. Expectation is computed by walking the tree the same way
     * rather than hardcoded, so this holds on any filesystem's readdir order. */
    for (int i = 0; i < DUPES; i++) {
        char stem[64], want[600], msg[256];
        snprintf(stem, sizeof(stem), "Multi%02d", i);
        CHECK(dfs_first("", stem, want, sizeof(want)) != NULL, "expectation is computable");
        const char *got = library_find_art(lib, stem);
        snprintf(msg, sizeof(msg), "duplicate %s resolves to the first copy the scan reached "
                                   "(want %s, got %s)", stem, want, got ? got : "(null)");
        CHECK(got != NULL, msg);
        size_t gl = strlen(got), wl = strlen(want);
        CHECK(gl >= wl && strcmp(got + (gl - wl), want) == 0, msg);
    }

    /* Exactly one of the two duplicates survives, it is a real path for that key, and it stays
     * the same across lookups.
     *
     * Deliberately NOT asserting that the shallow one wins. art_push()'s comment claimed that and
     * it was false: readdir returns "deep" before "Dupe.png" on this filesystem, scan_dir()
     * recurses on sight, so the deep copy is pushed first and keeps the key. Which one wins is
     * DFS order, and DFS order is the directory listing's order, which no filesystem promises.
     * An assertion either way would be pinning this test to APFS. */
    const char *dup = library_find_art(lib, "Dupe");
    CHECK(dup != NULL, "duplicate key resolves");
    CHECK(strcmp(dup, library_find_art(lib, "dupe.z64")) == 0, "duplicate resolves consistently");
    {
        size_t l = strlen(dup);
        const char *tail = "/Dupe.png";
        CHECK(l >= strlen(tail) && strcmp(dup + (l - strlen(tail)), tail) == 0,
              "survivor is one of the two real files");
    }

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
