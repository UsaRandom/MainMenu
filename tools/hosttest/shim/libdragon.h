/* Host-test shim. Only what cache.c and thumbstore.c actually use.
 *
 * The timing macros collapse to zero rather than reading a host clock. thumbstore.c uses them
 * only to log how long a slot took, and a test that measured host I/O would be measuring the
 * wrong machine anyway -- the numbers that matter come off the cart. */
#ifndef HOSTTEST_LIBDRAGON_H
#define HOSTTEST_LIBDRAGON_H

#include <stdio.h>
#include <stdint.h>

#include "surface.h"


/* Directory walking, over POSIX.
 *
 * library.c's scan is the only user, and it reads d_name and d_type and nothing else. Iteration
 * state lives inside the dir_t rather than in a static, because scan_dir() recurses into a
 * subdirectory part-way through iterating its parent, with a separate dir_t per stack frame --
 * a single shared cursor would resume the parent's walk wherever the child's left off, and the
 * scan would silently miss files. That is the same contract libdragon's own dir_t has.
 */
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

/* DT_DIR and DT_REG come from <dirent.h> and are used as-is: redefining them to our own values
 * only fought the system header, and library.c cares about the comparison, not the constant. */

typedef struct { char d_name[256]; int d_type; uint32_t d_size; void *_it; char _dir[512]; } dir_t;

static inline int hosttest_dir_step (dir_t *out) {
    struct dirent *e;
    while ((e = readdir((DIR *)out->_it)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) { continue; }
        snprintf(out->d_name, sizeof(out->d_name), "%s", e->d_name);
        out->d_type = e->d_type;
        /* libindex sums d_size across a directory to notice a file whose CONTENT changed at an
         * unchanged entry count. A shim that left this zero would make every signature agree on
         * size and silently pass the one staleness case worth testing -- which it did, once. */
        out->d_size = 0;
        if (e->d_type != DT_DIR) {
            char full[800];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", out->_dir, e->d_name);
            if (stat(full, &st) == 0) { out->d_size = (uint32_t)st.st_size; }
        }
        return 0;
    }
    closedir((DIR *)out->_it);
    out->_it = NULL;
    return -1;
}

static inline int dir_findfirst (const char *path, dir_t *out) {
    snprintf(out->_dir, sizeof(out->_dir), "%s", path);
    out->_it = opendir(path);
    if (out->_it == NULL) { return -1; }
    return hosttest_dir_step(out);
}

static inline int dir_findnext (const char *path, dir_t *out) {
    (void)path;
    if (out->_it == NULL) { return -1; }
    return hosttest_dir_step(out);
}

#define debugf(...) fprintf(stderr, "    [rom] " __VA_ARGS__)

#define TICKS_READ()        ((uint32_t)0)
#define TICKS_SINCE(t)      ((uint32_t)((void)(t), 0))
#define TIMER_MICROS(t)     ((uint32_t)((void)(t), 0))

#endif
