/**
 * @file launchlog.c
 * @brief See launchlog.h for why a file on the card is the diagnostic channel here.
 * @ingroup menu
 */

#include <stdarg.h>
#include <stdio.h>
#include <libdragon.h>

#include "menu/launchlog.h"
#include <sys/stat.h>

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

static FILE *log_file;

void launchlog_begin (void) {
    if (!ready || log_file != NULL) {
        return;
    }

    /* No cache_writable() gate, and that is a correction. This channel gated itself on the cache
     * probe's boot-time verdict, which coupled the one writer designed to be independent (see the
     * header: plain stdio, best-effort, never load-bearing) to the verdict of a different
     * subsystem. On the M64 that verdict -- or the writes behind it -- has produced zero
     * successful cache-family writes across the whole diagnostic era, while the settings writer,
     * which never asks the probe, went on writing config.ini fine. Whatever is wrong there, the
     * coupling meant this log died WITH it instead of reporting it. A failed fopen below is
     * already handled; asking permission first added nothing but a way to be silenced. */

    /* The folder may not exist on a card that has never been written to. cache_init() creates it
     * as a side effect of making /mainmenu/cache, but relying on that ordering is exactly the
     * mistake profile_save() made and the host test caught. */
    directory_create(dir_path);

    /* Append, not truncate.
     *
     * A card round trip costs minutes and now carries several launches -- three modes of the same
     * experiment, one after another. Truncating meant only the last one survived, and the run
     * whose log mattered was invariably not the last. The file is the only thing that comes back
     * from a console with no screen left to print on, so it keeps all of them.
     *
     * Capped, because a log nobody trims is a card nobody can write to. 256 KB is a few hundred
     * launches; past that the oldest go and the newest are what anybody wants anyway. */
    struct stat st;
    if (stat(file_path, &st) == 0 && st.st_size > 256 * 1024) {
        remove(file_path);
    }
    log_file = fopen(file_path, "ab");
    if (log_file != NULL) {
        fputs("\n---------------- launch ----------------\n", log_file);
    } else {
        /* Fall back to truncating, and say so in the file itself. The card's launch.log has not
         * gained a single append-era banner since the append rewrite landed, and one candidate
         * is fopen("a") itself failing on this newlib/FatFs stack -- a question no emulator can
         * answer, because ares has no writable storage at all. If the fallback line ever shows
         * up on a card, that is the answer, bought for free. */
        log_file = fopen(file_path, "wb");
        if (log_file != NULL) {
            fputs("---------------- launch (append-mode open failed; log truncated) ----------------\n",
                  log_file);
        }
    }
}

bool launchlog_open (void) {
    return log_file != NULL;
}

void launchlog_line (const char *fmt, ...) {
    if (log_file == NULL) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
}

void launchlog_end (void) {
    if (log_file == NULL) {
        return;
    }
    fclose(log_file);
    log_file = NULL;
}
