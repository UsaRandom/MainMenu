/**
 * @file path.c
 * @brief SVG path grammar, and flattening of curves into line segments.
 *
 * The whole of the `d` attribute grammar is here: every command letter that
 * appears anywhere in the target corpus, which turns out to be all of them
 * (`M L H V C S Q T A Z`, absolute and relative). Arcs are not an optional
 * extra -- they occur over ten thousand times across the icon set -- so the
 * full endpoint-to-centre conversion is implemented rather than approximated.
 *
 * Two things here are worth calling out.
 *
 * Numbers are parsed straight to 16.16 fixed point. There is no strtod and no
 * float in the number path at all: an SVG coordinate is a bounded decimal, and
 * assembling it digit by digit into an integer is both faster than a general
 * float parser and exactly reproducible between the host tests and the
 * console.
 *
 * Curves are flattened with a closed-form segment count rather than by
 * recursive subdivision. The usual approach -- split, test flatness, recurse --
 * spends most of its time on the flatness tests and needs a stack. Instead the
 * standard error bound for a polyline approximation of a Bezier gives the
 * required segment count directly from the curve's second difference, so each
 * curve costs one integer square root and then a straight run of evaluations.
 * Critically the count is computed in *device* space, so an icon drawn at 64x64
 * flattens to roughly an eighth of the segments it would need at 512x512.
 *
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include "svg64_internal.h"

/** Flatness tolerance, in device pixels. A fifth of a pixel is comfortably
 *  below what 8-bit coverage can express, so curves are smooth to the limit of
 *  the output format without tessellating past the point of visible return. */
#define SV_TOL_FX     (FX_ONE / 5)

/** Ceiling on segments per curve. Reached only by pathological input; the
 *  corpus never comes close at 64x64. */
#define SV_MAX_SEGS   96

/* ---------------------------------------------------------------- scanning -- */

static inline bool is_sep(char ch)
{
    return ch == ' ' || ch == ',' || ch == '\n' || ch == '\r' ||
           ch == '\t' || ch == '\f';
}

static inline void skip_sep(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && is_sep(*p)) p++;
    *pp = p;
}

/**
 * @brief Read one SVG number into 16.16 fixed point.
 *
 * Handles sign, integer and fraction parts, and exponents. The corpus contains
 * no scientific notation, but it costs a few lines to accept and silently
 * mangling such a file would be worse than the code.
 */
bool sv_parse_num(const char **pp, const char *end, fx_t *out)
{
    const char *p = *pp;
    int sign = 1;
    bool any = false;

    if (p < end && (*p == '+' || *p == '-')) {
        if (*p == '-') sign = -1;
        p++;
    }

    int64_t ipart = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        if (ipart < 100000) ipart = ipart * 10 + (*p - '0');  /* saturate junk */
        p++;
        any = true;
    }

    int64_t fnum = 0, fden = 1;
    if (p < end && *p == '.') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') {
            if (fden < 100000000) { fnum = fnum * 10 + (*p - '0'); fden *= 10; }
            p++;
            any = true;
        }
    }

    if (!any) return false;

    int exp = 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *save = p;
        int esign = 1, e = 0;
        p++;
        if (p < end && (*p == '+' || *p == '-')) {
            if (*p == '-') esign = -1;
            p++;
        }
        if (p < end && *p >= '0' && *p <= '9') {
            while (p < end && *p >= '0' && *p <= '9') {
                if (e < 100) e = e * 10 + (*p - '0');
                p++;
            }
            exp = esign * e;
        } else {
            p = save;  /* A trailing 'e' that was not an exponent. */
        }
    }

    int64_t v = (ipart << FX_BITS) + (((fnum << FX_BITS) + fden / 2) / fden);
    for (; exp > 0 && v < ((int64_t)1 << 44); exp--) v *= 10;
    for (; exp < 0 && v > 0; exp++) v /= 10;
    if (v > INT32_MAX) v = INT32_MAX;

    *out = (fx_t)(sign * v);
    *pp = p;
    return true;
}

/** @brief Read a number, tolerating leading separators. */
static bool next_num(const char **pp, const char *end, fx_t *out)
{
    skip_sep(pp, end);
    return sv_parse_num(pp, end, out);
}

/**
 * @brief Read an arc flag.
 *
 * Flags are a single '0' or '1' and, unlike every other operand, are allowed to
 * run straight into the next number with no separator: `a1 1 0 011 1` is legal
 * and means flags 0 and 1 followed by the coordinate 1. Reading them with the
 * general number parser would swallow `011` whole, so they get their own
 * single-character reader.
 */
static bool next_flag(const char **pp, const char *end, int *out)
{
    skip_sep(pp, end);
    const char *p = *pp;
    if (p < end && (*p == '0' || *p == '1')) {
        *out = *p - '0';
        *pp = p + 1;
        return true;
    }
    return false;
}

/* -------------------------------------------------------------- primitives -- */

static void emit_dev(sv_ctx_t *c, fx_t dx, fx_t dy)
{
    sv_raster_line(&c->r, c->dx, c->dy, dx, dy);
    c->dx = dx;
    c->dy = dy;
    c->nseg++;
    c->open = true;
}

/** @brief Close the subpath in progress.
 *
 * Filling treats every subpath as closed whether or not it ends in a 'Z', so
 * this runs on 'Z', on each new 'M', and once when the path data ends. */
static void close_subpath(sv_ctx_t *c)
{
    if (c->open && (c->dx != c->dsx || c->dy != c->dsy)) {
        sv_raster_line(&c->r, c->dx, c->dy, c->dsx, c->dsy);
        c->nseg++;  /* A closing edge is a segment like any other. */
    }
    c->dx = c->dsx;
    c->dy = c->dsy;
    c->open = false;
}

static void move_to(sv_ctx_t *c, fx_t ux, fx_t uy)
{
    close_subpath(c);
    c->cx = c->sx = ux;
    c->cy = c->sy = uy;
    sv_to_dev(c, ux, uy, &c->dx, &c->dy);
    c->dsx = c->dx;
    c->dsy = c->dy;
}

static void line_to(sv_ctx_t *c, fx_t ux, fx_t uy)
{
    fx_t dx, dy;
    sv_to_dev(c, ux, uy, &dx, &dy);
    emit_dev(c, dx, dy);
    c->cx = ux;
    c->cy = uy;
}

/**
 * @brief Segment count for a curve whose second difference bound is @p dd.
 *
 * From the standard bound: a polyline of n segments approximates a Bezier to
 * within max|B''| / (8n^2). Rearranged for n against SV_TOL_FX, with the
 * numerator and denominator of the max|B''| coefficient passed in, since it is
 * 6*dd for a cubic and 2*dd for a quadratic.
 */
static int seg_count(fx_t dd, int num, int den)
{
    if (dd <= 0) return 1;
    uint32_t ratio = (uint32_t)(((int64_t)dd * num) /
                                ((int64_t)SV_TOL_FX * den));
    int n = (int)isqrt32(ratio) + 1;
    return n > SV_MAX_SEGS ? SV_MAX_SEGS : n;
}

/** @brief Flatten a cubic given in *device* space, starting at the cursor. */
static void flatten_cubic_dev(sv_ctx_t *c, fx_t x1, fx_t y1,
                              fx_t x2, fx_t y2, fx_t x3, fx_t y3)
{
    const fx_t x0 = c->dx, y0 = c->dy;

    const fx_t ax = x0 - 2 * x1 + x2, ay = y0 - 2 * y1 + y2;
    const fx_t bx = x1 - 2 * x2 + x3, by = y1 - 2 * y2 + y3;
    const fx_t d1 = fx_abs(ax) + fx_abs(ay);
    const fx_t d2 = fx_abs(bx) + fx_abs(by);
    const int  n  = seg_count(d1 > d2 ? d1 : d2, 6, 8);

    /* Monomial form, so each point costs three multiplies per axis via Horner
     * instead of the twelve a de Casteljau evaluation would need. */
    const fx_t c3x = -x0 + 3 * x1 - 3 * x2 + x3;
    const fx_t c2x = 3 * x0 - 6 * x1 + 3 * x2;
    const fx_t c1x = -3 * x0 + 3 * x1;
    const fx_t c3y = -y0 + 3 * y1 - 3 * y2 + y3;
    const fx_t c2y = 3 * y0 - 6 * y1 + 3 * y2;
    const fx_t c1y = -3 * y0 + 3 * y1;

    for (int i = 1; i < n; i++) {
        const fx_t t = (fx_t)(((int64_t)i << FX_BITS) / n);
        emit_dev(c,
                 fx_mul(fx_mul(fx_mul(c3x, t) + c2x, t) + c1x, t) + x0,
                 fx_mul(fx_mul(fx_mul(c3y, t) + c2y, t) + c1y, t) + y0);
    }
    /* The endpoint is emitted exactly rather than evaluated at t = 1, so
     * consecutive curves in a subpath meet with no rounding gap between them. */
    emit_dev(c, x3, y3);
    c->ncurve++;
}

static void curve_to(sv_ctx_t *c, fx_t ux1, fx_t uy1, fx_t ux2, fx_t uy2,
                     fx_t ux3, fx_t uy3)
{
    fx_t x1, y1, x2, y2, x3, y3;
    sv_to_dev(c, ux1, uy1, &x1, &y1);
    sv_to_dev(c, ux2, uy2, &x2, &y2);
    sv_to_dev(c, ux3, uy3, &x3, &y3);
    flatten_cubic_dev(c, x1, y1, x2, y2, x3, y3);
    c->cx = ux3;
    c->cy = uy3;
}

static void quad_to(sv_ctx_t *c, fx_t ux1, fx_t uy1, fx_t ux2, fx_t uy2)
{
    fx_t x1, y1, x2, y2;
    sv_to_dev(c, ux1, uy1, &x1, &y1);
    sv_to_dev(c, ux2, uy2, &x2, &y2);

    const fx_t x0 = c->dx, y0 = c->dy;
    const fx_t dd = fx_abs(x0 - 2 * x1 + x2) + fx_abs(y0 - 2 * y1 + y2);
    const int  n  = seg_count(dd, 2, 8);

    const fx_t c2x = x0 - 2 * x1 + x2, c1x = -2 * x0 + 2 * x1;
    const fx_t c2y = y0 - 2 * y1 + y2, c1y = -2 * y0 + 2 * y1;

    for (int i = 1; i < n; i++) {
        const fx_t t = (fx_t)(((int64_t)i << FX_BITS) / n);
        emit_dev(c,
                 fx_mul(fx_mul(c2x, t) + c1x, t) + x0,
                 fx_mul(fx_mul(c2y, t) + c1y, t) + y0);
    }
    emit_dev(c, x2, y2);
    c->ncurve++;

    c->cx = ux2;
    c->cy = uy2;
}

/* -------------------------------------------------------------------- arcs -- */

static inline float fx2f(fx_t v) { return (float)v * (1.0f / 65536.0f); }

static inline fx_t f2fx(float v)
{
    if (v >  30000.0f) v =  30000.0f;
    if (v < -30000.0f) v = -30000.0f;
    return (fx_t)(v * 65536.0f);
}

/**
 * @brief Flatten an elliptical arc by converting it to cubics.
 *
 * This is the endpoint-to-centre parameterisation from the SVG specification's
 * implementation notes (F.6.5), followed by the usual split into segments of at
 * most a quarter turn, each approximated by a cubic with control arms of
 * 4/3 * tan(dtheta/4).
 *
 * It is the one place in the library that uses floating point. That is a
 * deliberate boundary rather than an oversight: the conversion needs two
 * transcendental calls and a square root, it runs a handful of times per icon,
 * and it never touches a loop over pixels. Reimplementing it in fixed point
 * would trade real accuracy for no measurable time.
 */
static void arc_to(sv_ctx_t *c, fx_t rxf, fx_t ryf, fx_t rotf,
                   int large, int sweep, fx_t ux2, fx_t uy2)
{
    const float x1 = fx2f(c->cx), y1 = fx2f(c->cy);
    const float x2 = fx2f(ux2),   y2 = fx2f(uy2);

    float rx = fabsf(fx2f(rxf)), ry = fabsf(fx2f(ryf));

    /* Degenerate radii mean a straight line, per the specification. */
    if (rx < 1e-6f || ry < 1e-6f ||
        (fabsf(x2 - x1) < 1e-6f && fabsf(y2 - y1) < 1e-6f)) {
        line_to(c, ux2, uy2);
        return;
    }

    const float phi = fx2f(rotf) * (float)(M_PI / 180.0);
    const float cp = cosf(phi), sp = sinf(phi);

    const float dx2 = (x1 - x2) * 0.5f, dy2 = (y1 - y2) * 0.5f;
    const float x1p =  cp * dx2 + sp * dy2;
    const float y1p = -sp * dx2 + cp * dy2;

    /* Scale the radii up if they are too small to span the endpoints. */
    float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f) {
        const float s = sqrtf(lambda);
        rx *= s;
        ry *= s;
    }

    const float rx2 = rx * rx, ry2 = ry * ry;
    const float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    float num = rx2 * ry2 - den;
    if (num < 0.0f) num = 0.0f;
    float co = (den > 0.0f) ? sqrtf(num / den) : 0.0f;
    if (large == sweep) co = -co;

    const float cxp =  co * rx * y1p / ry;
    const float cyp = -co * ry * x1p / rx;
    const float ccx = cp * cxp - sp * cyp + (x1 + x2) * 0.5f;
    const float ccy = sp * cxp + cp * cyp + (y1 + y2) * 0.5f;

    const float sx = (x1p - cxp) / rx, sy = (y1p - cyp) / ry;
    const float ex = (-x1p - cxp) / rx, ey = (-y1p - cyp) / ry;

    const float theta1 = atan2f(sy, sx);
    float dtheta = atan2f(ey, ex) - theta1;
    if (!sweep && dtheta > 0.0f) dtheta -= (float)(2.0 * M_PI);
    if ( sweep && dtheta < 0.0f) dtheta += (float)(2.0 * M_PI);

    int nseg = (int)ceilf(fabsf(dtheta) / (float)(M_PI / 2.0));
    if (nseg < 1) nseg = 1;
    if (nseg > 4) nseg = 4;

    const float delta = dtheta / (float)nseg;
    const float k = 4.0f / 3.0f * tanf(delta * 0.25f);

    float t = theta1;
    float px = x1, py = y1;
    /* Tangent at the segment start, in the ellipse's rotated frame. */
    float ctc = cosf(t), sts = sinf(t);
    float dpx = cp * (-rx * sts) - sp * (ry * ctc);
    float dpy = sp * (-rx * sts) + cp * (ry * ctc);

    for (int i = 0; i < nseg; i++) {
        const float t2 = t + delta;
        const float ct2 = cosf(t2), st2 = sinf(t2);

        const float qx = ccx + cp * (rx * ct2) - sp * (ry * st2);
        const float qy = ccy + sp * (rx * ct2) + cp * (ry * st2);
        const float dqx = cp * (-rx * st2) - sp * (ry * ct2);
        const float dqy = sp * (-rx * st2) + cp * (ry * ct2);

        curve_to(c,
                 f2fx(px + k * dpx), f2fx(py + k * dpy),
                 f2fx(qx - k * dqx), f2fx(qy - k * dqy),
                 f2fx(qx),           f2fx(qy));

        t = t2;
        px = qx;  py = qy;
        dpx = dqx; dpy = dqy;
    }

    /* Land exactly on the requested endpoint; the cubic chain above is an
     * approximation and must not be allowed to drift the current point. */
    c->cx = ux2;
    c->cy = uy2;
}

/* ------------------------------------------------------------------ grammar -- */

svg64_err_t sv_path_run(sv_ctx_t *c, const char *d, const char *end)
{
    const char *p = d;
    char cmd = 0;

    c->cx = c->cy = c->sx = c->sy = 0;
    c->rcx = c->rcy = 0;
    c->prev_cmd = 0;
    c->open = false;
    sv_to_dev(c, 0, 0, &c->dx, &c->dy);
    c->dsx = c->dx;
    c->dsy = c->dy;

    for (;;) {
        skip_sep(&p, end);
        if (p >= end) break;

        const char ch = *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            cmd = ch;
            p++;
        } else if (cmd == 0) {
            return SVG64_ERR_SYNTAX;
        } else if (cmd == 'Z' || cmd == 'z') {
            /* 'Z' takes no operands, so a number here would loop forever. */
            return SVG64_ERR_SYNTAX;
        } else if (cmd == 'M') {
            cmd = 'L';  /* Extra coordinate pairs after a moveto are linetos. */
        } else if (cmd == 'm') {
            cmd = 'l';
        }

        const bool rel = (cmd >= 'a' && cmd <= 'z');
        const fx_t ox = rel ? c->cx : 0;
        const fx_t oy = rel ? c->cy : 0;
        fx_t a, b, e, f, g, h;

        switch (cmd) {
        case 'M': case 'm':
            if (!next_num(&p, end, &a) || !next_num(&p, end, &b))
                return SVG64_ERR_SYNTAX;
            move_to(c, ox + a, oy + b);
            break;

        case 'L': case 'l':
            if (!next_num(&p, end, &a) || !next_num(&p, end, &b))
                return SVG64_ERR_SYNTAX;
            line_to(c, ox + a, oy + b);
            break;

        case 'H': case 'h':
            if (!next_num(&p, end, &a)) return SVG64_ERR_SYNTAX;
            line_to(c, ox + a, c->cy);
            break;

        case 'V': case 'v':
            if (!next_num(&p, end, &a)) return SVG64_ERR_SYNTAX;
            line_to(c, c->cx, oy + a);
            break;

        case 'C': case 'c':
            if (!next_num(&p, end, &a) || !next_num(&p, end, &b) ||
                !next_num(&p, end, &e) || !next_num(&p, end, &f) ||
                !next_num(&p, end, &g) || !next_num(&p, end, &h))
                return SVG64_ERR_SYNTAX;
            c->rcx = ox + e;
            c->rcy = oy + f;
            curve_to(c, ox + a, oy + b, ox + e, oy + f, ox + g, oy + h);
            break;

        case 'S': case 's': {
            if (!next_num(&p, end, &e) || !next_num(&p, end, &f) ||
                !next_num(&p, end, &g) || !next_num(&p, end, &h))
                return SVG64_ERR_SYNTAX;
            /* The implied first control point is the previous one mirrored
             * through the current point -- but only if the previous command was
             * itself a cubic, otherwise it coincides with the current point. */
            fx_t rx = c->cx, ry = c->cy;
            if (c->prev_cmd == 'C' || c->prev_cmd == 'S') {
                rx = 2 * c->cx - c->rcx;
                ry = 2 * c->cy - c->rcy;
            }
            c->rcx = ox + e;
            c->rcy = oy + f;
            curve_to(c, rx, ry, ox + e, oy + f, ox + g, oy + h);
            break;
        }

        case 'Q': case 'q':
            if (!next_num(&p, end, &a) || !next_num(&p, end, &b) ||
                !next_num(&p, end, &g) || !next_num(&p, end, &h))
                return SVG64_ERR_SYNTAX;
            c->rcx = ox + a;
            c->rcy = oy + b;
            quad_to(c, ox + a, oy + b, ox + g, oy + h);
            break;

        case 'T': case 't': {
            if (!next_num(&p, end, &g) || !next_num(&p, end, &h))
                return SVG64_ERR_SYNTAX;
            fx_t rx = c->cx, ry = c->cy;
            if (c->prev_cmd == 'Q' || c->prev_cmd == 'T') {
                rx = 2 * c->cx - c->rcx;
                ry = 2 * c->cy - c->rcy;
            }
            c->rcx = rx;
            c->rcy = ry;
            quad_to(c, rx, ry, ox + g, oy + h);
            break;
        }

        case 'A': case 'a': {
            int large, sweep;
            if (!next_num(&p, end, &a) || !next_num(&p, end, &b) ||
                !next_num(&p, end, &e) ||
                !next_flag(&p, end, &large) || !next_flag(&p, end, &sweep) ||
                !next_num(&p, end, &g) || !next_num(&p, end, &h))
                return SVG64_ERR_SYNTAX;
            arc_to(c, a, b, e, large, sweep, ox + g, oy + h);
            break;
        }

        case 'Z': case 'z':
            close_subpath(c);
            c->cx = c->sx;
            c->cy = c->sy;
            break;

        default:
            return SVG64_ERR_SYNTAX;
        }

        c->prev_cmd = (cmd >= 'a' && cmd <= 'z') ? (char)(cmd - 32) : cmd;
    }

    close_subpath(c);
    return SVG64_OK;
}
