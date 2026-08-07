/**
 * @file svg64_internal.h
 * @brief Shared types for the fused parse/flatten/rasterise pipeline.
 *
 * Everything here is 16.16 signed fixed point. The choice is not about the
 * R4300i lacking an FPU -- it has a decent one -- but about determinism: the
 * host test harness and the console must agree bit for bit, so that a
 * regression caught by `make test` is the same regression that would have
 * shown up on hardware. Floating point appears in exactly one place, the
 * elliptical-arc setup in path.c, where it runs a handful of times per icon
 * and never inside a loop over pixels.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SVG64_INTERNAL_H
#define SVG64_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "svg64.h"

/* -------------------------------------------------------------- fixed point -- */

typedef int32_t fx_t;

#define FX_BITS  16
#define FX_ONE   (1 << FX_BITS)
#define FX_HALF  (FX_ONE >> 1)

/** @brief Multiply two 16.16 values. The 64-bit intermediate is free on MIPS64. */
static inline fx_t fx_mul(fx_t a, fx_t b)
{
    return (fx_t)(((int64_t)a * (int64_t)b) >> FX_BITS);
}

/** @brief Divide two 16.16 values. Callers must ensure @p b is nonzero. */
static inline fx_t fx_div(fx_t a, fx_t b)
{
    return (fx_t)((((int64_t)a) << FX_BITS) / (int64_t)b);
}

static inline fx_t fx_abs(fx_t a) { return a < 0 ? -a : a; }
static inline fx_t fx_min(fx_t a, fx_t b) { return a < b ? a : b; }
static inline fx_t fx_max(fx_t a, fx_t b) { return a > b ? a : b; }

/** @brief floor() to an integer pixel index. */
static inline int32_t fx_floori(fx_t a) { return a >> FX_BITS; }

/** @brief Integer square root, for the curve subdivision counts in path.c.
 *
 * Restoring binary square root: two bits of radicand per iteration. The root is
 * accumulated at twice its value (hence the += 2 and the final >> 1), which is
 * what lets the trial subtraction be a compare against `root` rather than a
 * multiply. Exact for the whole uint32 range -- floor(sqrt(v)) with no drift.
 */
static inline uint32_t isqrt32(uint32_t v)
{
    uint32_t rem = 0, root = 0;
    for (int i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (v >> 30);
        v <<= 2;
        if (root < rem) {
            rem -= root + 1;
            root += 2;
        }
    }
    return root >> 1;
}

/* --------------------------------------------------------------- rasteriser -- */

/**
 * @brief Coverage accumulator.
 *
 * @ref acc holds signed area *deltas*, not coverage. A row is resolved by
 * running a prefix sum across it; the running total at pixel x is the signed
 * winding-weighted coverage there. See raster.c for the derivation.
 *
 * @ref astride is @ref w + 2 rather than @ref w: the span code writes one cell
 * past the right edge of a span, and a span may legitimately end at x == w.
 * Paying two cells per row buys an inner loop with no bounds check in it.
 */
typedef struct {
    fx_t *acc;
    int   w, h;
    int   astride;
    int   ymin, ymax;  /**< Half-open range of rows touched since the last clear. */
} sv_raster_t;

void sv_raster_reset(sv_raster_t *r);
void sv_raster_line(sv_raster_t *r, fx_t x0, fx_t y0, fx_t x1, fx_t y1);
void sv_raster_composite(sv_raster_t *r, const svg64_surface_t *dst, uint32_t rgba);
void sv_raster_unpremultiply(const svg64_surface_t *dst, int y0, int y1);

/* ------------------------------------------------------------------ context -- */

/**
 * @brief State threaded through one path's parse-flatten-rasterise pass.
 *
 * The SVG path grammar is defined in user space (viewBox units) because
 * relative commands accumulate there, so @ref cx / @ref cy and the control
 * point reflection state stay in user space. Coordinates cross into device
 * space only at the moment a curve is about to be flattened, since the
 * subdivision count depends on how big the curve is *on screen*.
 */
typedef struct {
    sv_raster_t r;

    /* user -> device: dev = user * scale + off */
    fx_t scale, offx, offy;

    /* Path state, user space. */
    fx_t cx, cy;      /**< Current point. */
    fx_t sx, sy;      /**< Start of the current subpath, for 'Z'. */
    fx_t rcx, rcy;    /**< Reflected control point source for 'S' / 'T'. */
    char prev_cmd;    /**< Last command letter, for the 'S'/'T' smoothing rules. */

    /* Device-space cursor, kept in step with cx/cy so lines need no transform. */
    fx_t dx, dy;
    fx_t dsx, dsy;    /**< Device-space subpath start. */
    bool open;        /**< A subpath is in progress and needs closing. */

    uint32_t nseg, ncurve;
} sv_ctx_t;

/** @brief Transform a user-space coordinate pair into device space. */
static inline void sv_to_dev(const sv_ctx_t *c, fx_t ux, fx_t uy, fx_t *ox, fx_t *oy)
{
    *ox = fx_mul(ux, c->scale) + c->offx;
    *oy = fx_mul(uy, c->scale) + c->offy;
}

/** @brief Parse and rasterise one path's `d` attribute. */
svg64_err_t sv_path_run(sv_ctx_t *c, const char *d, const char *end);

/** @brief Read one SVG number into 16.16 fixed point, advancing @p pp.
 *
 * Exposed because the viewBox scan in svg64.c needs the identical number
 * grammar; there is no second implementation to drift out of step. */
bool sv_parse_num(const char **pp, const char *end, fx_t *out);

#endif /* SVG64_INTERNAL_H */
