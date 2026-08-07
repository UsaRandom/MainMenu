/**
 * @file raster.c
 * @brief Signed-area coverage rasteriser.
 *
 * The algorithm is the one Raph Levien's font-rs popularised, ported to 16.16
 * fixed point. It is worth understanding why it was chosen over the more
 * obvious sorted-edge scanline fill, because the reason is the whole
 * performance story of this library.
 *
 * A classic scanline filler needs the edges of a path up front: it sorts them,
 * walks an active edge table, and for antialiasing repeats the whole thing
 * several times per output row at sub-scanline offsets. That means storing an
 * edge list (a few thousand per icon), sorting it, and paying for N vertical
 * samples of approximate coverage.
 *
 * This one instead treats each line segment as a contribution of *signed area*
 * to the cells it passes through, accumulated into a buffer of deltas. Once
 * every segment has been added, a single prefix sum across each row turns the
 * deltas into the exact, analytically-integrated area covered by the path at
 * every pixel. Three consequences follow, and all three matter on a 93 MHz
 * R4300i:
 *
 *   - Segments are consumed as they are produced. Nothing needs to be retained,
 *     so the flattener streams straight in and there is no edge buffer and no
 *     sort. Memory is one accumulator, sized by the *output*, not the input:
 *     a 23 KB icon and a 151 byte icon cost the same 16.9 KB at 64x64.
 *   - The antialiasing is exact rather than sampled, in one pass. There is no
 *     supersampling factor to trade quality against time, which matters here
 *     because the corpus is 512x512 art shown at 64x64 -- an 8x reduction that
 *     sampled coverage renders visibly crunchy.
 *   - The inner loop is adds. The only division is one reciprocal per span.
 *
 * The cost is that it computes a signed winding *number* as a real value, so
 * the nonzero rule falls out naturally (clamp the absolute value) while
 * even-odd would need a triangle wave. Since no file in the target corpus sets
 * fill-rule, only nonzero is implemented.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "svg64_internal.h"

/* ------------------------------------------------------------------ helpers -- */

/** @brief (a * b) / 255 for a, b in 0..255, exactly, without a divide. */
static inline int mul255(int a, int b)
{
    int t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

void sv_raster_reset(sv_raster_t *r)
{
    if (r->ymin < r->ymax) {
        memset(r->acc + (size_t)r->ymin * r->astride, 0,
               (size_t)(r->ymax - r->ymin) * (size_t)r->astride * sizeof(fx_t));
    }
    r->ymin = r->h;
    r->ymax = 0;
}

/* --------------------------------------------------------------------- line -- */

/**
 * @brief Accumulate one line segment's signed area into the delta buffer.
 *
 * Coordinates are device-space pixels in 16.16. The segment is clipped
 * vertically (rows outside the surface are skipped) and clamped horizontally.
 * Clamping rather than rejecting is deliberate: a segment that runs off the
 * left edge still has to deposit its full winding contribution at x == 0, or
 * everything to its right in that row would be left unfilled.
 */
void sv_raster_line(sv_raster_t *r, fx_t x0, fx_t y0, fx_t x1, fx_t y1)
{
    if (y0 == y1) return;  /* Horizontal edges contribute no winding. */

    int dir = 1;
    if (y0 > y1) {
        fx_t t;
        dir = -1;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }

    const fx_t hfx = (fx_t)(r->h << FX_BITS);
    if (y1 <= 0 || y0 >= hfx) return;

    /* Slope is held in 64 bits because a near-horizontal segment can produce a
     * value far outside 16.16. Clamping it is safe: such a segment spans less
     * than 1/65536 of a row, so the area it deposits is orders of magnitude
     * below the 1/255 coverage quantum whichever x it is attributed to. */
    int64_t dxdy;
    {
        const int64_t LIM = (int64_t)1 << 40;
        dxdy = (((int64_t)(x1 - x0)) << FX_BITS) / (int64_t)(y1 - y0);
        if (dxdy >  LIM) dxdy =  LIM;
        if (dxdy < -LIM) dxdy = -LIM;
    }

    fx_t x = x0;
    if (y0 < 0) {
        /* Advance the cursor to where the segment crosses y == 0. */
        x += (fx_t)((dxdy * (int64_t)(-y0)) >> FX_BITS);
        y0 = 0;
    }

    int ystart = y0 >> FX_BITS;
    int yend   = (int)((y1 + (FX_ONE - 1)) >> FX_BITS);
    if (yend > r->h) yend = r->h;
    if (ystart >= yend) return;

    if (ystart < r->ymin) r->ymin = ystart;
    if (yend   > r->ymax) r->ymax = yend;

    const fx_t wfx = (fx_t)(r->w << FX_BITS);
    fx_t *row = r->acc + (size_t)ystart * (size_t)r->astride;

    for (int y = ystart; y < yend; y++, row += r->astride) {
        const fx_t ytop = (fx_t)(y << FX_BITS);
        const fx_t lo = fx_max(ytop, y0);
        const fx_t hi = fx_min(ytop + FX_ONE, y1);
        const fx_t dy = hi - lo;

        const fx_t xnext = x + (fx_t)((dxdy * (int64_t)dy) >> FX_BITS);
        const fx_t d = (dir > 0) ? dy : -dy;

        /* Span covered within this row, ordered and clipped to the surface. */
        fx_t xa = x, xb = xnext;
        if (xa > xb) { fx_t t = xa; xa = xb; xb = t; }
        if (xa < 0)    xa = 0;
        if (xb < 0)    xb = 0;
        if (xa > wfx)  xa = wfx;
        if (xb > wfx)  xb = wfx;

        const int  x0i     = xa >> FX_BITS;
        const fx_t x0floor = (fx_t)(x0i << FX_BITS);
        const int  x1i     = (int)((xb + (FX_ONE - 1)) >> FX_BITS);
        const fx_t x1ceil  = (fx_t)(x1i << FX_BITS);

        /* A span can straddle a pixel boundary while being arbitrarily narrow,
         * which would make the trapezoid branch's 1/(xb-xa) overflow 16.16 --
         * at a width of 2/65536 px the reciprocal is exactly INT32_MAX + 1, and
         * the resulting negative `s` would scatter garbage across the row.
         * Below a thousandth of a pixel the midpoint split is used instead: it
         * deposits the same total `d`, and its error is three orders of
         * magnitude under the 1/255 coverage quantum. */
        if (x1i <= x0i + 1 || xb - xa < (FX_ONE / 1024)) {
            /* The span sits within a single pixel: split d by where its
             * midpoint falls, which is exact for a segment this short. */
            const fx_t xmf = ((xa + xb) >> 1) - x0floor;
            row[x0i]     += d - fx_mul(d, xmf);
            row[x0i + 1] += fx_mul(d, xmf);
        } else {
            /* The span crosses pixel boundaries. Area is distributed as a
             * trapezoid: a partial cell at each end and a uniform d*s per cell
             * across the middle. */
            const fx_t s   = fx_div(FX_ONE, xb - xa);
            const fx_t x0f = xa - x0floor;
            const fx_t x1f = xb - x1ceil + FX_ONE;
            const fx_t a1  = fx_mul(s, FX_ONE - x0f);
            const fx_t am  = fx_mul(fx_mul(s, fx_mul(x1f, x1f)), FX_HALF);

            row[x0i] += fx_mul(d, a1);

            if (x1i == x0i + 2) {
                row[x0i + 1] += fx_mul(d, FX_ONE - a1 - am);
            } else {
                const fx_t a2 = fx_mul(s, FX_ONE + FX_HALF - x0f);
                const fx_t ds = fx_mul(d, s);
                row[x0i + 1] += fx_mul(d, a2 - a1);
                for (int xi = x0i + 2; xi < x1i - 1; xi++)
                    row[xi] += ds;
                const fx_t a3 = a2 + (fx_t)((x1i - x0i - 3) * s);
                row[x1i - 1] += fx_mul(d, FX_ONE - a3 - am);
            }
            row[x1i] += fx_mul(d, am);
        }

        x = xnext;
    }
}

/* ---------------------------------------------------------------- composite -- */

/**
 * @brief Resolve the accumulator to coverage and blend @p rgba into @p dst.
 *
 * The prefix sum and the blend share one pass, so each cell is read once.
 *
 * RGBA32 accumulates *premultiplied*, which is what makes antialiased edges
 * come out right over a transparent background; sv_raster_unpremultiply()
 * converts back afterwards. RGBA16 skips all of that: with a single alpha bit
 * there is no partial transparency to preserve, so its edge pixels blend
 * against whatever is already in the surface and the bit records whether the
 * pixel is at least half covered.
 */
void sv_raster_composite(sv_raster_t *r, const svg64_surface_t *dst, uint32_t rgba)
{
    const int sr = (int)((rgba >> 24) & 0xFF);
    const int sg = (int)((rgba >> 16) & 0xFF);
    const int sb = (int)((rgba >>  8) & 0xFF);
    const int sa = (int)( rgba        & 0xFF);
    if (sa == 0) return;

    const int y0 = r->ymin < 0 ? 0 : r->ymin;
    const int y1 = r->ymax > dst->height ? dst->height : r->ymax;
    const int w  = r->w < dst->width ? r->w : dst->width;

    /* Fully-covered opaque pixels are the overwhelming majority: the interior
     * of every glyph, and in the two-tone corpus an entire backdrop path that
     * covers the cell. Blending those against the destination is wasted work --
     * the result is just the source colour -- so the packed source pixel is
     * computed once here and stored directly when coverage is total. Without
     * this the backdrop path alone costs a full read-modify-write per pixel. */
    const bool src_opaque = (sa == 255);
    const uint16_t src16 = (uint16_t)(((sr >> 3) << 11) | ((sg >> 3) << 6) |
                                      ((sb >> 3) << 1) | 1);

    for (int y = y0; y < y1; y++) {
        const fx_t *cell = r->acc + (size_t)y * (size_t)r->astride;
        uint8_t *rowp = (uint8_t *)dst->pixels + (size_t)y * (size_t)dst->stride;
        fx_t acc = 0;

        for (int x = 0; x < w; x++) {
            acc += cell[x];

            /* 16.16 winding -> 0..255 coverage. Nonzero rule: magnitude only,
             * saturated, so a doubly-wound region is simply solid. */
            int cov = (int)(fx_abs(acc) >> 8);
            if (cov > 255) cov = 255;
            if (cov == 0) continue;

            if (cov == 255 && src_opaque) {
                switch (dst->format) {
                case SVG64_FMT_I8:
                    rowp[x] = 255;
                    break;
                case SVG64_FMT_RGBA16:
                    *(uint16_t *)(rowp + (size_t)x * 2) = src16;
                    break;
                case SVG64_FMT_RGBA32:
                default: {
                    uint8_t *p = rowp + (size_t)x * 4;
                    p[0] = (uint8_t)sr; p[1] = (uint8_t)sg;
                    p[2] = (uint8_t)sb; p[3] = 255;
                    break;
                }
                }
                continue;
            }

            const int a = mul255(cov, sa);
            if (a == 0) continue;
            const int ia = 255 - a;

            switch (dst->format) {
            case SVG64_FMT_I8: {
                uint8_t *p = rowp + x;
                *p = (uint8_t)(a + mul255(*p, ia));
                break;
            }
            case SVG64_FMT_RGBA16: {
                uint16_t *p = (uint16_t *)(rowp + (size_t)x * 2);
                const uint16_t v = *p;
                /* 5-5-5-1, expanded to 8 bits by replicating the high bits so
                 * that 31 maps to 255 rather than 248. */
                int dr = ((v >> 11) & 0x1F), dg = ((v >> 6) & 0x1F), db = ((v >> 1) & 0x1F);
                dr = (dr << 3) | (dr >> 2);
                dg = (dg << 3) | (dg >> 2);
                db = (db << 3) | (db >> 2);
                const int nr = mul255(sr, a) + mul255(dr, ia);
                const int ng = mul255(sg, a) + mul255(dg, ia);
                const int nb = mul255(sb, a) + mul255(db, ia);
                const int na = (a >= 128) ? 1 : (v & 1);
                *p = (uint16_t)(((nr >> 3) << 11) | ((ng >> 3) << 6) |
                                ((nb >> 3) << 1) | na);
                break;
            }
            case SVG64_FMT_RGBA32:
            default: {
                uint8_t *p = rowp + (size_t)x * 4;
                p[0] = (uint8_t)(mul255(sr, a) + mul255(p[0], ia));
                p[1] = (uint8_t)(mul255(sg, a) + mul255(p[1], ia));
                p[2] = (uint8_t)(mul255(sb, a) + mul255(p[2], ia));
                p[3] = (uint8_t)(a + mul255(p[3], ia));
                break;
            }
            }
        }
    }
}

/**
 * @brief Convert premultiplied RGBA32 back to straight alpha, in place.
 *
 * Only called for SVG64_FMT_RGBA32, once per render rather than once per path.
 * The reciprocal table turns the per-pixel divide into a multiply and a shift;
 * fully opaque and fully transparent pixels, which are the overwhelming
 * majority, skip it entirely.
 */
void sv_raster_unpremultiply(const svg64_surface_t *dst, int y0, int y1)
{
    static uint16_t recip[256];
    static bool built = false;

    if (dst->format != SVG64_FMT_RGBA32) return;

    if (!built) {
        for (int i = 1; i < 256; i++)
            recip[i] = (uint16_t)((255 * 256 + i / 2) / i);
        built = true;
    }

    if (y0 < 0) y0 = 0;
    if (y1 > dst->height) y1 = dst->height;

    for (int y = y0; y < y1; y++) {
        uint8_t *p = (uint8_t *)dst->pixels + (size_t)y * (size_t)dst->stride;
        for (int x = 0; x < dst->width; x++, p += 4) {
            const int a = p[3];
            if (a == 0 || a == 255) continue;
            const int k = recip[a];
            int r = (p[0] * k) >> 8, g = (p[1] * k) >> 8, b = (p[2] * k) >> 8;
            p[0] = (uint8_t)(r > 255 ? 255 : r);
            p[1] = (uint8_t)(g > 255 ? 255 : g);
            p[2] = (uint8_t)(b > 255 ? 255 : b);
        }
    }
}
