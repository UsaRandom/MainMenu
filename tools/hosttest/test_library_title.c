/* Display titles: collision, custom skip, filename tags kept.
 *
 * library_finish() is the whole of the behaviour. A pretty-name database is not used, because
 * hacks share the original header and would stay identical. The filename is the unique string
 * the user already has, and (U) [!] stay on it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library.h"

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

    /* The scan root is often "sd:/" with a trailing separator. "%s/%s" stacked another and the
     * signature walk hashed a different string than the scan stored, so a warm boot looked like
     * a card that had moved. */
    char joined[64];
    library_join_child(joined, sizeof(joined), "sd:/roms", "game.z64");
    CHECK(strcmp(joined, "sd:/roms/game.z64") == 0, "join without trailing slash");
    library_join_child(joined, sizeof(joined), "sd:/", "roms");
    CHECK(strcmp(joined, "sd:/roms") == 0, "join with trailing slash does not stack");
    library_join_child(joined, sizeof(joined), "sd:/", "cover.png");
    CHECK(strcmp(joined, "sd:/cover.png") == 0, "root file with trailing slash");

    library_t *lib = library_init();
    CHECK(lib != NULL, "init");

    add(lib, "/roms/Mario 64.z64", "SUPER MARIO 64", 0);
    library_finish(lib);
    CHECK(strcmp(by_path(lib, "/roms/Mario 64.z64")->title, "SUPER MARIO 64") == 0,
          "unique header stays");

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
    int z = library_find_path(lib, "/z.z64");
    free(lib->records[z].given);
    lib->records[z].given = strdup("Alpha");
    lib->records[z].flags |= LIBF_CUSTOM_TITLE;
    library_finish(lib);
    CHECK(library_find_path(lib, "/z.z64") == 0, "renamed game moved; look up by path");
    CHECK(library_find_path(lib, "/a.z64") == 1, "the other game slid down");
    CHECK(strcmp(lib->records[0].title, "Alpha") == 0, "display follows the new given");

    library_free(lib);

    printf("  %d checks\n", checks);
    return 0;
}
