/* Host-test shim: the two probes paths.c takes as function pointers.
 *
 * Shared rather than repeated in each test file, because every suite here links paths.c now --
 * cache_init() calls menu_path() -- while none of them exercises the three-place search that uses
 * these. They exist to satisfy the linker and to behave correctly if a later test does reach them.
 *
 * directory_create() is deliberately NOT here. Each test file has its own, because what "create a
 * directory" should do differs between them, and a shared one would be the wrong one somewhere.
 */

#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

bool file_exists (char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

bool directory_exists (char *path) {
    struct stat st;
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

#include <dirent.h>
#include <stdio.h>
#include <string.h>

int directory_erase (const char *path, void (*tick)(void)) {
    DIR *d = opendir(path);
    if (d == NULL) {
        return 0;
    }
    int removed = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (tick != NULL) {
            tick();
        }
        if (e->d_name[0] == '.') {
            continue;
        }
        char file[512];
        snprintf(file, sizeof(file), "%s/%s", path, e->d_name);
        if (remove(file) == 0) {
            removed++;
        }
    }
    closedir(d);
    return removed;
}
