/* Host-test shim. Only what thumbstore.c actually touches on a surface.
 *
 * Field ORDER differs from libdragon's real surface_t and that is fine -- thumbstore.c reaches
 * for members by name and never memcpys a surface or casts one. What matters is that `stride` is
 * independent of `width`, because the padded-stride path is one of the things being tested. */
#ifndef HOSTTEST_SURFACE_H
#define HOSTTEST_SURFACE_H

#include <stdint.h>

/* Widths match libdragon's surface.h exactly -- stride is uint16_t there, not uint32_t. Getting
 * that wrong here would be a shim that compiles code the target does not: the signedness of
 * `surf->stride == row_bytes` in slot_io() depends on it. */
typedef struct {
    void    *buffer;
    uint16_t width;
    uint16_t height;
    uint16_t stride;    /**< bytes per row; may exceed width * 2 */
} surface_t;

#endif
