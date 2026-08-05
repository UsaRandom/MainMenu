/**
 * @file paths.c
 * @brief See paths.h for why there are two kinds of path here rather than one.
 * @ingroup menu
 */

#include <stdio.h>
#include <string.h>

#include "menu/paths.h"
#include "utils/fs.h"

/** @brief `<storage><dir>/<leaf>`, with the doubled slash the prefix would otherwise leave. */
static void join (char *out, size_t cap, const char *storage, const char *dir, const char *leaf) {
    size_t n = strlen(storage);
    while (n > 0 && storage[n - 1] == '/') {
        n--;
    }
    if (leaf == NULL || leaf[0] == '\0') {
        snprintf(out, cap, "%.*s%s", (int)n, storage, dir);
    } else {
        snprintf(out, cap, "%.*s%s/%s", (int)n, storage, dir, leaf);
    }
}

void menu_path (char *out, size_t cap, const char *storage, const char *sub) {
    join(out, cap, storage, MENU_DIR, sub);
}

/** @brief The probe, parameterised by what counts as "there". */
static bool find (char *out, size_t cap, const char *storage, const char *leaf,
                  bool (*present) (char *)) {
    /* Our folder first, so a card that has been organised the way this menu suggests never pays
     * for the other two probes, and so a stale copy at the root cannot shadow the current one. */
    join(out, cap, storage, MENU_DIR, leaf);
    if (present(out)) {
        return true;
    }

    char candidate[300];

    /* The root, which is where a zip emptied onto the card lands. */
    join(candidate, sizeof(candidate), storage, "", leaf);
    if (present(candidate)) {
        snprintf(out, cap, "%s", candidate);
        return true;
    }

    /* Last, and read-only: this is where an existing card built for the old layout has it. */
    join(candidate, sizeof(candidate), storage, MENU_DIR_LEGACY, leaf);
    if (present(candidate)) {
        snprintf(out, cap, "%s", candidate);
        return true;
    }

    /* @p out is left holding the first candidate on purpose -- a caller that wants to say where
     * it looked can name the place the file is supposed to go. */
    return false;
}

bool menu_find_file (char *out, size_t cap, const char *storage, const char *leaf) {
    return find(out, cap, storage, leaf, file_exists);
}

bool menu_find_dir (char *out, size_t cap, const char *storage, const char *leaf) {
    return find(out, cap, storage, leaf, directory_exists);
}
