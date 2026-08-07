/**
 * @file svg64.h
 * @brief Anti-aliased SVG icon rasteriser for the Nintendo 64.
 *
 * svg64 renders a *deliberately small* subset of SVG: filled `<path>` elements,
 * nonzero winding, flat colours. That is the whole feature set. It is not a
 * general SVG renderer and will not become one -- see docs/LIMITS.md for the
 * list of what it ignores and why ignoring it is what makes it fast.
 *
 * The pipeline is fused and allocation-free. Parsing, curve flattening and
 * rasterisation happen in a single pass: path data is turned into line segments
 * that are fed straight into a coverage accumulator, so there is no retained
 * document, no edge list, and no malloc. The only memory svg64 needs is a
 * scratch buffer the caller supplies, sized by SVG64_SCRATCH_BYTES().
 *
 * @code
 * #include <svg64.h>
 *
 * static uint8_t scratch[SVG64_SCRATCH_BYTES(64, 64)];
 * uint16_t pixels[64 * 64];
 *
 * svg64_surface_t dst = {
 *     .pixels = pixels, .width = 64, .height = 64,
 *     .stride = 64 * 2, .format = SVG64_FMT_RGBA16,
 * };
 * svg64_render(svg_text, svg_len, &dst, NULL, scratch, sizeof scratch);
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SVG64_H
#define SVG64_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ errors -- */

typedef enum {
    SVG64_OK = 0,
    SVG64_ERR_BADARG,     /**< NULL pointer, or a surface with a bad size/stride. */
    SVG64_ERR_NOSCRATCH,  /**< Scratch buffer smaller than SVG64_SCRATCH_BYTES(). */
    SVG64_ERR_NOPATH,     /**< Parsed fine but the document drew nothing. */
    SVG64_ERR_SYNTAX,     /**< Malformed path data; nothing was drawn. */
} svg64_err_t;

/** @brief Human-readable form of an svg64_err_t, for logging. */
const char *svg64_strerror(svg64_err_t err);

/* ---------------------------------------------------------------- surfaces -- */

typedef enum {
    /** 8-bit coverage only. Colour is discarded; every path contributes its
     *  alpha. Useful as an RDP I8 texture you then tint at blit time. */
    SVG64_FMT_I8 = 0,
    /** 16-bit 5-5-5-1, the N64's native framebuffer/texture format. The single
     *  alpha bit is set where coverage is at least 50%. */
    SVG64_FMT_RGBA16,
    /** 32-bit 8-8-8-8, R in the high byte. Full alpha; the reference format. */
    SVG64_FMT_RGBA32,
} svg64_fmt_t;

typedef struct {
    void        *pixels;  /**< Destination. Must be 8-byte aligned for RDP use. */
    int          width;   /**< Pixels across. */
    int          height;  /**< Pixels down. */
    int          stride;  /**< *Bytes* per row, >= width * bytes-per-pixel. */
    svg64_fmt_t  format;
} svg64_surface_t;

/* ----------------------------------------------------------------- options -- */

/**
 * @brief Per-render knobs. Pass NULL for the defaults, which reproduce the
 *        file's own colours on a transparent background.
 *
 * The recolour fields exist because the target corpus (game-icons.net) is
 * strictly two-tone: every file is a black backdrop path plus a white glyph
 * path. Rather than make callers post-process, svg64 can substitute colours as
 * it composites. Fills are bucketed by luma: anything below mid-grey takes
 * @ref dark, anything at or above takes @ref light.
 */
typedef struct {
    /** Substitute @ref dark / @ref light for the authored fill colours. */
    bool     recolor;
    uint32_t dark;   /**< RGBA8888 replacing dark fills.  Default 0x00000000. */
    uint32_t light;  /**< RGBA8888 replacing light fills. Default 0xFFFFFFFF. */

    /** Clear the surface to this RGBA8888 before drawing. Default 0 (transparent).
     *  Set the alpha to 0 for a transparent background. */
    uint32_t clear;

    /** Skip clearing and composite over whatever is already in the surface.
     *  @ref clear is ignored when this is set. */
    bool     no_clear;
} svg64_opts_t;

/* ----------------------------------------------------------------- scratch -- */

/**
 * @brief Scratch bytes needed to render a @p w by @p h surface.
 *
 * This is the coverage accumulator: one 32-bit signed cell per pixel plus two
 * guard columns per row, which absorb the span-end writes the rasteriser makes
 * at x+1 without needing a bounds test in the inner loop.
 *
 * At 64x64 this is 16.9 KB. It scales linearly with area, so it is worth
 * rendering large images in horizontal bands if memory is tight.
 */
#define SVG64_SCRATCH_BYTES(w, h)  ((size_t)((w) + 2) * (size_t)(h) * 4u)

/* ------------------------------------------------------------------- entry -- */

/**
 * @brief Parse and rasterise an SVG document into @p dst.
 *
 * The document is read once, front to back; @p svg is not retained and may be
 * freed or overwritten as soon as this returns. Nothing is allocated: all
 * working memory comes from @p scratch.
 *
 * On SVG64_ERR_SYNTAX the surface is left cleared (or untouched under
 * @ref svg64_opts_t::no_clear) rather than half-drawn, so a corrupt file
 * renders as a blank cell instead of garbage.
 *
 * @param svg          Document text. Need not be NUL-terminated.
 * @param len          Bytes of @p svg.
 * @param dst          Destination surface.
 * @param opts         Options, or NULL for the defaults.
 * @param scratch      Caller-owned scratch, 8-byte aligned.
 * @param scratch_len  Bytes of @p scratch; see SVG64_SCRATCH_BYTES().
 */
svg64_err_t svg64_render(const char *svg, size_t len,
                         const svg64_surface_t *dst,
                         const svg64_opts_t *opts,
                         void *scratch, size_t scratch_len);

/* ------------------------------------------------------------------- stats -- */

/**
 * @brief Counters from the most recent svg64_render() on this thread.
 *
 * Present so the demo ROM can report what a render actually cost without the
 * library taking a dependency on libdragon's timers. Purely diagnostic.
 */
typedef struct {
    uint32_t paths;     /**< `<path>` elements drawn. */
    uint32_t segments;  /**< Line segments the flattener produced. */
    uint32_t curves;    /**< Curve and arc commands flattened. */
} svg64_stats_t;

/** @brief Fetch the counters described by svg64_stats_t. */
void svg64_get_stats(svg64_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SVG64_H */
