/* Display titles: collision, custom skip, filename tags kept.
 *
 * library_finish() is the whole of the behaviour. A pretty-name database is not used, because
 * hacks share the original header and would stay identical. The filename is the unique string
 * the user already has, and (U) [!] stay on it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ht_alloc.h"
#include "library.h"

int title_allocs = 0;

void *ht_malloc (size_t n) {
    title_allocs++;
    return malloc(n);
}

void ht_free (void *p) {
    free(p);
}

char *ht_strdup (const char *s) {
    size_t n;
    char *p;

    title_allocs++;
    if (s == NULL) {
        return NULL;
    }
    n = strlen(s) + 1;
    p = malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

static int checks = 0;

#define CHECK(cond, what) do {                                                  \
    checks++;                                                                   \
    if (!(cond)) {                                                              \
        fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));      \
        exit(1);                                                                \
    }                                                                           \
} while (0)

path_t *path_create (const char *p) { (void)p; return NULL; }
void path_free (path_t *p) { (void)p; }
rom_err_t rom_config_load (path_t *p, rom_info_t *ri) { (void)p; (void)ri; return 0; }
rom_err_t rom_config_set_display_name (path_t *p, const char *n) { (void)p; (void)n; return 0; }
void rom_info_free_meta (rom_info_t *ri) { (void)ri; }
rom_save_type_t rom_info_get_save_type (rom_info_t *ri) { (void)ri; return 0; }
void rom_info_set_dir_listing (const char *const *n, int c) { (void)n; (void)c; }

static lib_record_t *add (library_t *lib, const char *path, const char *given, uint16_t flags) {
    lib_record_t *r = library_push(lib);
    CHECK(r != NULL, "push");
    memset(r, 0, sizeof(*r));
    r->path = strdup(path);
    r->given = given ? strdup(given) : NULL;
    r->flags = flags;
    return r;
}

static const lib_record_t *by_path (const library_t *lib, const char *path) {
    int i = library_find_path(lib, path);
    CHECK(i >= 0, path);
    return &lib->records[i];
}

int main (void) {
    char *fn = library_title_from_path("/roms/Zelda - MQ (U) [!].z64");
    CHECK(fn != NULL, "filename from path");
    CHECK(strcmp(fn, "Zelda - MQ (U) [!]") == 0, "tags stay, extension goes");
    free(fn);

    fn = library_title_from_path("no-dir.n64");
    CHECK(fn != NULL && strcmp(fn, "no-dir") == 0, "basename only");
    free(fn);

    /* The scan root is "sd:/". Child joins stack a slash, on purpose: that is the spelling
     * already hashed into library.idx and thumbs.pak. Collapsing it is a card-wide cache miss.
     * The two walks share this helper so they cannot disagree with each other either. */
    char joined[64];
    library_join_child(joined, sizeof(joined), "sd:/roms", "game.z64");
    CHECK(strcmp(joined, "sd:/roms/game.z64") == 0, "join without trailing slash");
    library_join_child(joined, sizeof(joined), "sd:/", "roms");
    CHECK(strcmp(joined, "sd://roms") == 0, "sd:/ plus child keeps the doubled slash");
    library_join_child(joined, sizeof(joined), "sd:/", "cover.png");
    CHECK(strcmp(joined, "sd://cover.png") == 0, "root file keeps it too");

    library_t *lib = library_init();
    CHECK(lib != NULL, "init");

    add(lib, "/roms/Mario 64.z64", "SUPER MARIO 64", 0);
    library_finish(lib);
    CHECK(strcmp(by_path(lib, "/roms/Mario 64.z64")->title, "SUPER MARIO 64") == 0,
          "unique header stays");
    const char *mario_title = by_path(lib, "/roms/Mario 64.z64")->title;
    library_finish(lib);
    CHECK(by_path(lib, "/roms/Mario 64.z64")->title == mario_title,
          "a second finish keeps the same allocation");

    add(lib, "/hacks/Zelda - MQ (U) [!].z64", "THE LEGEND OF ZELDA", 0);
    add(lib, "/hacks/Zelda - Randomizer.z64", "THE LEGEND OF ZELDA", 0);
    add(lib, "/hacks/Zelda - Master Quest.z64", "THE LEGEND OF ZELDA", 0);
    library_finish(lib);

    CHECK(strcmp(by_path(lib, "/hacks/Zelda - MQ (U) [!].z64")->title,
                 "Zelda - MQ (U) [!]") == 0,
          "colliding header becomes filename, tags intact");
    CHECK(strcmp(by_path(lib, "/hacks/Zelda - Randomizer.z64")->title,
                 "Zelda - Randomizer") == 0,
          "second collide is its own filename");
    CHECK(strcmp(by_path(lib, "/roms/Mario 64.z64")->title, "SUPER MARIO 64") == 0,
          "unrelated unique header is untouched");
    CHECK(by_path(lib, "/roms/Mario 64.z64")->title == mario_title,
          "unrelated unique header is not reallocated when others collide");

    library_free(lib);
    lib = library_init();

    add(lib, "/a/Zelda - MQ.z64", "THE LEGEND OF ZELDA", 0);
    add(lib, "/a/Zelda - Rando.z64", "THE LEGEND OF ZELDA", 0);
    add(lib, "/a/Zelda - Named.z64", "Master Quest", LIBF_CUSTOM_TITLE);
    library_finish(lib);

    CHECK(strcmp(by_path(lib, "/a/Zelda - Named.z64")->title, "Master Quest") == 0,
          "custom name is not overwritten by collision");
    CHECK(strcmp(by_path(lib, "/a/Zelda - MQ.z64")->title, "Zelda - MQ") == 0,
          "two remaining headers still collide");
    CHECK(strcmp(by_path(lib, "/a/Zelda - Rando.z64")->title, "Zelda - Rando") == 0,
          "the other leftover still collides");

    library_free(lib);
    lib = library_init();

    add(lib, "/b/Zelda - MQ.z64", "THE LEGEND OF ZELDA", 0);
    add(lib, "/b/Zelda - Named.z64", "Master Quest", LIBF_CUSTOM_TITLE);
    library_finish(lib);

    CHECK(strcmp(by_path(lib, "/b/Zelda - MQ.z64")->title, "THE LEGEND OF ZELDA") == 0,
          "one leftover unique header stays the header");
    CHECK(strcmp(by_path(lib, "/b/Zelda - Named.z64")->title, "Master Quest") == 0,
          "custom still wins when it is the only other copy");

    library_free(lib);
    lib = library_init();

    /* A rename qsorts. The index a caller was holding is then a different game -- look up by
     * path, which is what screen_detail and the art pool have to do after C-up. */
    add(lib, "/z.z64", "Zelda", 0);
    add(lib, "/a.z64", "Mario", 0);
    library_finish(lib);
    CHECK(library_find_path(lib, "/a.z64") == 0, "sorted by title, A first");
    CHECK(library_find_path(lib, "/z.z64") == 1, "Z after A");
    const char *mario_kept = by_path(lib, "/a.z64")->title;
    int z = library_find_path(lib, "/z.z64");
    free(lib->records[z].given);
    lib->records[z].given = strdup("Alpha");
    lib->records[z].flags |= LIBF_CUSTOM_TITLE;
    library_finish(lib);
    CHECK(library_find_path(lib, "/z.z64") == 0, "renamed game moved; look up by path");
    CHECK(library_find_path(lib, "/a.z64") == 1, "the other game slid down");
    CHECK(strcmp(lib->records[0].title, "Alpha") == 0, "display follows the new given");
    CHECK(by_path(lib, "/a.z64")->title == mario_kept,
          "the game that was not renamed keeps its title allocation");

    /* Naming one of two colliding headers leaves the other unique, so its display must change
     * from the filename back to the header -- that one allocation is the one a rename still
     * owes. The pointer-stability checks above would pass if finish never updated anyone. */
    library_free(lib);
    lib = library_init();
    add(lib, "/c/Zelda - MQ.z64", "THE LEGEND OF ZELDA", 0);
    add(lib, "/c/Zelda - Rando.z64", "THE LEGEND OF ZELDA", 0);
    library_finish(lib);
    CHECK(strcmp(by_path(lib, "/c/Zelda - MQ.z64")->title, "Zelda - MQ") == 0,
          "pair collides before the rename");
    int mq = library_find_path(lib, "/c/Zelda - MQ.z64");
    free(lib->records[mq].given);
    lib->records[mq].given = strdup("Master Quest");
    lib->records[mq].flags |= LIBF_CUSTOM_TITLE;
    library_finish(lib);
    CHECK(strcmp(by_path(lib, "/c/Zelda - MQ.z64")->title, "Master Quest") == 0,
          "the renamed one shows the typed name");
    CHECK(strcmp(by_path(lib, "/c/Zelda - Rando.z64")->title, "THE LEGEND OF ZELDA") == 0,
          "the leftover unique header returns to the header");

    /* Pointer equality cannot catch a free+strdup of the same length: the heap hands the
     * same address back. Counting allocations can. A second finish of 32 unique titles used
     * to malloc 32 times; it must now malloc none, and renaming one of them must malloc one. */
    library_free(lib);
    lib = library_init();
    {
        char path[32], given[16];
        const char *held[32];
        int i, before, z;

        for (i = 0; i < 32; i++) {
            snprintf(path, sizeof(path), "/roms/g%02d.z64", i);
            snprintf(given, sizeof(given), "GAME %02d", i);
            add(lib, path, given, 0);
        }
        library_finish(lib);
        for (i = 0; i < 32; i++) {
            snprintf(path, sizeof(path), "/roms/g%02d.z64", i);
            held[i] = by_path(lib, path)->title;
        }
        before = title_allocs;
        library_finish(lib);
        CHECK(title_allocs == before, "a second finish of 32 unique titles allocates nothing");
        for (i = 0; i < 32; i++) {
            snprintf(path, sizeof(path), "/roms/g%02d.z64", i);
            CHECK(by_path(lib, path)->title == held[i], "and keeps every allocation");
        }

        z = library_find_path(lib, "/roms/g31.z64");
        CHECK(z >= 0, "last of the 32 is present");
        free(lib->records[z].given);
        lib->records[z].given = strdup("Alpha");
        lib->records[z].flags |= LIBF_CUSTOM_TITLE;
        before = title_allocs;
        library_finish(lib);
        CHECK(title_allocs == before + 1, "renaming one of 32 allocates one title, not 32");
        CHECK(strcmp(by_path(lib, "/roms/g31.z64")->title, "Alpha") == 0,
              "the renamed one of 32 shows the new name");
        CHECK(by_path(lib, "/roms/g00.z64")->title == held[0],
              "the other 31 keep their allocations");
    }

    library_free(lib);

    printf("  %d checks\n", checks);
    return 0;
}
