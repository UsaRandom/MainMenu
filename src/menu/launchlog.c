/**
 * @file launchlog.c
 * @brief See launchlog.h for why a file on the card is the diagnostic channel here.
 * @ingroup menu
 */

#include <stdarg.h>
#include <stdio.h>
#include <libdragon.h>

#include "library/cache.h"
#include "menu/launchlog.h"
#include "menu/paths.h"
#include "utils/fs.h"

#define LAUNCHLOG_FILE  "launch.log"

static char file_path[300];
static char dir_path[300];
static bool ready;

void launchlog_init (const char *storage_prefix) {
    menu_path(file_path, sizeof(file_path), storage_prefix, LAUNCHLOG_FILE);
    menu_path(dir_path, sizeof(dir_path), storage_prefix, NULL);
    ready = true;
}

void launchlog_write (const char *fmt, ...) {
    if (!ready || !cache_writable()) {
        return;
    }

    /* The folder may not exist on a card that has never been written to. cache_init() creates it
     * as a side effect of making /mainmenu/cache, but relying on that ordering is exactly the
     * mistake profile_save() made and the host test caught. */
    directory_create(dir_path);

    FILE *f = fopen(file_path, "wb");
    if (f == NULL) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fclose(f);
}
