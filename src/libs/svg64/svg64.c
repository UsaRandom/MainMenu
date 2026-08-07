/**
 * @file svg64.c
 * @brief Document scan, viewBox fitting, and the svg64_render() entry point.
 *
 * The "XML parser" here is a tag scanner, and calling it anything grander would
 * oversell it. It walks the document looking for two things: the `<svg>`
 * element's viewBox, and every `<path>` element's `d` and `fill`. Elements it
 * does not recognise are skipped whole, attributes it does not recognise are
 * ignored, and there is no tree, no namespace handling and no entity decoding.
 *
 * That is a real limitation and it is worth being precise about which one. The
 * scanner ignores nesting, so it would mis-render a document that relied on
 * `<g>` for inherited fills or transforms. It does not do so for the target
 * corpus, where every file is a flat list of `<path>` elements carrying their
 * own fill -- but point it at a general SVG and it will quietly draw the wrong
 * picture rather than refuse. See docs/LIMITS.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "svg64_internal.h"

static svg64_stats_t g_stats;

const char *svg64_strerror(svg64_err_t err)
{
    switch (err) {
    case SVG64_OK:            return "ok";
    case SVG64_ERR_BADARG:    return "bad argument";
    case SVG64_ERR_NOSCRATCH: return "scratch buffer too small";
    case SVG64_ERR_NOPATH:    return "document contains nothing to draw";
    case SVG64_ERR_SYNTAX:    return "malformed path data";
    }
    return "unknown error";
}

void svg64_get_stats(svg64_stats_t *out)
{
    if (out) *out = g_stats;
}

/* ------------------------------------------------------------- tag scanner -- */

static inline bool is_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

typedef struct {
    const char *name_s, *name_e;
    const char *attr_s, *attr_e;
} sv_tag_t;

/**
 * @brief Advance to the next element open tag.
 *
 * Comments, processing instructions, doctypes and close tags are skipped.
 * Quoted attribute values are respected when looking for the closing '>', so a
 * '>' inside a path's `d` cannot truncate the tag.
 */
static bool next_tag(const char **pp, const char *end, sv_tag_t *t)
{
    const char *p = *pp;

    for (;;) {
        while (p < end && *p != '<') p++;
        if (p >= end) return false;
        p++;

        if (p < end && *p == '!') {
            if (end - p >= 3 && p[1] == '-' && p[2] == '-') {
                p += 3;
                while (p + 2 < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) p++;
                p = (p + 2 < end) ? p + 3 : end;
            } else {
                while (p < end && *p != '>') p++;
                if (p < end) p++;
            }
            continue;
        }
        if (p < end && (*p == '?' || *p == '/')) {
            while (p < end && *p != '>') p++;
            if (p < end) p++;
            continue;
        }
        break;
    }

    t->name_s = p;
    while (p < end && !is_ws(*p) && *p != '>' && *p != '/') p++;
    t->name_e = p;

    t->attr_s = p;
    while (p < end && *p != '>') {
        if (*p == '"' || *p == '\'') {
            const char q = *p++;
            while (p < end && *p != q) p++;
            if (p < end) p++;
        } else {
            p++;
        }
    }
    t->attr_e = p;
    if (p < end) p++;

    *pp = p;
    return true;
}

static bool tag_is(const sv_tag_t *t, const char *name)
{
    const size_t n = strlen(name);
    return (size_t)(t->name_e - t->name_s) == n &&
           memcmp(t->name_s, name, n) == 0;
}

/** @brief Look up one attribute by exact name within a tag's attribute span. */
static bool find_attr(const sv_tag_t *t, const char *name,
                      const char **vs, const char **ve)
{
    const size_t nlen = strlen(name);
    const char *p = t->attr_s, *end = t->attr_e;

    while (p < end) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;

        const char *ns = p;
        while (p < end && *p != '=' && !is_ws(*p)) p++;
        const char *ne = p;
        if (ne == ns) { p++; continue; }  /* Never spin on a stray character. */

        while (p < end && is_ws(*p)) p++;
        if (p >= end || *p != '=') continue;  /* Valueless attribute. */
        p++;
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;

        const char *v_s, *v_e;
        if (*p == '"' || *p == '\'') {
            const char q = *p++;
            v_s = p;
            while (p < end && *p != q) p++;
            v_e = p;
            if (p < end) p++;
        } else {
            v_s = p;
            while (p < end && !is_ws(*p)) p++;
            v_e = p;
        }

        if ((size_t)(ne - ns) == nlen && memcmp(ns, name, nlen) == 0) {
            *vs = v_s;
            *ve = v_e;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ colours -- */

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/**
 * @brief Resolve a `fill` value to RGBA8888.
 *
 * Covers what the corpus and its neighbours actually use: `#rgb`, `#rrggbb`,
 * `none`, and the two colour keywords that show up in hand-edited icons.
 * Anything else falls back to opaque black, which is also SVG's default fill,
 * so an unrecognised value renders as a plain shape rather than disappearing.
 */
static uint32_t parse_color(const char *s, const char *e, bool *is_none)
{
    *is_none = false;
    while (s < e && is_ws(*s)) s++;
    while (e > s && is_ws(e[-1])) e--;

    const size_t n = (size_t)(e - s);

    if (n == 4 && memcmp(s, "none", 4) == 0) { *is_none = true; return 0; }
    if (n == 5 && memcmp(s, "white", 5) == 0) return 0xFFFFFFFFu;
    if (n == 5 && memcmp(s, "black", 5) == 0) return 0x000000FFu;

    if (n >= 4 && s[0] == '#') {
        int v[6], got = 0;
        for (size_t i = 1; i < n && got < 6; i++) {
            const int h = hexval(s[i]);
            if (h < 0) break;
            v[got++] = h;
        }
        if (got >= 6)
            return (uint32_t)((v[0] << 28) | (v[1] << 24) | (v[2] << 20) |
                              (v[3] << 16) | (v[4] << 12) | (v[5] << 8)) | 0xFFu;
        if (got >= 3)
            return (uint32_t)((v[0] << 28) | (v[0] << 24) | (v[1] << 20) |
                              (v[1] << 16) | (v[2] << 12) | (v[2] << 8)) | 0xFFu;
    }

    return 0x000000FFu;
}

/* -------------------------------------------------------------- surface ops -- */

static inline int mul255(int a, int b)
{
    const int t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

static void clear_surface(const svg64_surface_t *dst, uint32_t rgba)
{
    const int r = (int)((rgba >> 24) & 0xFF);
    const int g = (int)((rgba >> 16) & 0xFF);
    const int b = (int)((rgba >>  8) & 0xFF);
    const int a = (int)( rgba        & 0xFF);

    for (int y = 0; y < dst->height; y++) {
        uint8_t *row = (uint8_t *)dst->pixels + (size_t)y * (size_t)dst->stride;
        switch (dst->format) {
        case SVG64_FMT_I8:
            memset(row, a, (size_t)dst->width);
            break;
        case SVG64_FMT_RGBA16: {
            const uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) |
                                          ((b >> 3) << 1) | (a >= 128 ? 1 : 0));
            uint16_t *p = (uint16_t *)row;
            for (int x = 0; x < dst->width; x++) p[x] = v;
            break;
        }
        case SVG64_FMT_RGBA32:
        default: {
            /* Stored premultiplied to match how paths composite; the final
             * pass in svg64_render() converts the whole surface back. */
            uint8_t *p = row;
            const uint8_t pr = (uint8_t)mul255(r, a);
            const uint8_t pg = (uint8_t)mul255(g, a);
            const uint8_t pb = (uint8_t)mul255(b, a);
            for (int x = 0; x < dst->width; x++, p += 4) {
                p[0] = pr; p[1] = pg; p[2] = pb; p[3] = (uint8_t)a;
            }
            break;
        }
        }
    }
}

static int bytes_per_pixel(svg64_fmt_t f)
{
    switch (f) {
    case SVG64_FMT_I8:     return 1;
    case SVG64_FMT_RGBA16: return 2;
    default:               return 4;
    }
}

/* ------------------------------------------------------------------- render -- */

/** @brief Read a viewBox attribute's four numbers. */
static bool parse_viewbox(const char *s, const char *e, fx_t out[4])
{
    const char *p = s;
    for (int i = 0; i < 4; i++) {
        while (p < e && (is_ws(*p) || *p == ',')) p++;
        if (!sv_parse_num(&p, e, &out[i])) return false;
    }
    return out[2] > 0 && out[3] > 0;
}

svg64_err_t svg64_render(const char *svg, size_t len,
                         const svg64_surface_t *dst,
                         const svg64_opts_t *opts,
                         void *scratch, size_t scratch_len)
{
    static const svg64_opts_t defaults = {
        .recolor = false, .dark = 0, .light = 0xFFFFFFFFu,
        .clear = 0, .no_clear = false,
    };
    if (!opts) opts = &defaults;

    if (!svg || !dst || !dst->pixels || !scratch) return SVG64_ERR_BADARG;
    if (dst->width <= 0 || dst->height <= 0)       return SVG64_ERR_BADARG;
    if (dst->stride < dst->width * bytes_per_pixel(dst->format))
        return SVG64_ERR_BADARG;
    if (scratch_len < SVG64_SCRATCH_BYTES(dst->width, dst->height))
        return SVG64_ERR_NOSCRATCH;
    /* The accumulator is indexed as 32-bit cells. On MIPS a misaligned load
     * faults rather than merely running slowly, so this is worth rejecting up
     * front instead of crashing somewhere inside the inner loop. */
    if (((uintptr_t)scratch & 3u) != 0) return SVG64_ERR_BADARG;

    memset(&g_stats, 0, sizeof g_stats);

    sv_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.r.acc     = (fx_t *)scratch;
    ctx.r.w       = dst->width;
    ctx.r.h       = dst->height;
    ctx.r.astride = dst->width + 2;
    ctx.r.ymin    = dst->height;
    ctx.r.ymax    = 0;

    /* The accumulator is caller-provided memory of unknown contents, and
     * sv_raster_reset() only clears the rows a path actually touched, so the
     * one full clear has to happen here. */
    memset(ctx.r.acc, 0, SVG64_SCRATCH_BYTES(dst->width, dst->height));

    if (!opts->no_clear) clear_surface(dst, opts->clear);

    /* Default transform, replaced once a viewBox turns up. */
    ctx.scale = FX_ONE;
    ctx.offx  = 0;
    ctx.offy  = 0;

    const char *p = svg, *end = svg + len;
    sv_tag_t tag;
    uint32_t drawn = 0;

    while (next_tag(&p, end, &tag)) {
        const char *vs, *ve;

        if (tag_is(&tag, "svg")) {
            fx_t vb[4];
            if (find_attr(&tag, "viewBox", &vs, &ve) && parse_viewbox(vs, ve, vb)) {
                /* preserveAspectRatio="xMidYMid meet": uniform scale, centred.
                 * The corpus is square in a square cell so this is a no-op
                 * there, but it keeps non-square surfaces honest. */
                const fx_t sx = fx_div((fx_t)(dst->width  << FX_BITS), vb[2]);
                const fx_t sy = fx_div((fx_t)(dst->height << FX_BITS), vb[3]);
                ctx.scale = sx < sy ? sx : sy;
                ctx.offx  = (((fx_t)(dst->width  << FX_BITS) -
                              fx_mul(vb[2], ctx.scale)) >> 1) -
                            fx_mul(vb[0], ctx.scale);
                ctx.offy  = (((fx_t)(dst->height << FX_BITS) -
                              fx_mul(vb[3], ctx.scale)) >> 1) -
                            fx_mul(vb[1], ctx.scale);
            }
            continue;
        }

        if (!tag_is(&tag, "path")) continue;
        if (!find_attr(&tag, "d", &vs, &ve)) continue;

        uint32_t rgba = 0x000000FFu;  /* SVG's default fill is opaque black. */
        const char *fs, *fe;
        if (find_attr(&tag, "fill", &fs, &fe)) {
            bool none = false;
            rgba = parse_color(fs, fe, &none);
            if (none) continue;
        }

        if (opts->recolor) {
            const int r = (int)((rgba >> 24) & 0xFF);
            const int g = (int)((rgba >> 16) & 0xFF);
            const int b = (int)((rgba >>  8) & 0xFF);
            const int luma = (r * 77 + g * 151 + b * 28) >> 8;
            rgba = (luma < 128) ? opts->dark : opts->light;
        }

        const svg64_err_t err = sv_path_run(&ctx, vs, ve);
        if (err != SVG64_OK) {
            /* Leave a blank cell rather than a half-drawn one: a corrupt file
             * should look absent, not look like a different icon. */
            if (!opts->no_clear) clear_surface(dst, opts->clear);
            memset(ctx.r.acc, 0, SVG64_SCRATCH_BYTES(dst->width, dst->height));
            return err;
        }

        sv_raster_composite(&ctx.r, dst, rgba);
        sv_raster_reset(&ctx.r);
        drawn++;
    }

    if (dst->format == SVG64_FMT_RGBA32)
        sv_raster_unpremultiply(dst, 0, dst->height);

    g_stats.paths    = drawn;
    g_stats.segments = ctx.nseg;
    g_stats.curves   = ctx.ncurve;

    return drawn ? SVG64_OK : SVG64_ERR_NOPATH;
}
