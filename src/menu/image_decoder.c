/**
 * @file image_decoder.c
 * @brief PNG Decoder component implementation
 * @ingroup ui_components
 */

#include <stdio.h>
#include <libdragon.h>
#include <libspng/spng/spng.h>
#include <picojpeg/picojpeg.h>
#include "image_decoder.h"
#include "utils/fs.h"

/** @brief PNG File Information Structure. */
typedef struct {
    FILE *f; /**< File pointer */
    spng_ctx *ctx; /**< SPNG context */
    struct spng_ihdr ihdr; /**< SPNG image header */
    surface_t *image; /**< Image surface */
    uint8_t *row_buffer; /**< Row buffer */
    char *io_buffer; /**< stdio buffer for f; see the setvbuf call in image_decoder_start() */
    int decoded_rows; /**< Number of decoded rows */
    img_callback_t *callback; /**< Callback function */
    void *callback_data; /**< Callback data */

    /* Streaming scaler. When dst_w is non-zero the decoder box-filters each source row
     * straight into a dst_w x dst_h surface instead of allocating one the size of the file.
     * See image_decoder_start_scaled(). */
    int dst_w, dst_h;
    int crop_x, crop_y, crop_w, crop_h;  /**< source rect that covers the destination */
    uint16_t *colrun;                    /**< source pixels folded into each dest column */
    uint32_t *acc;                       /**< dst_w x RGB accumulator for one dest row */
    uint32_t acc_n;                      /**< source rows folded into the current dest row */
    int acc_row;                         /**< which destination row acc is building */
    int flushed_row;                     /**< last destination row written, -1 for none */

    /* JPEG. picojpeg keeps its own file-scope state, which is safe here only because exactly one
     * decode is ever in flight -- the same reason the PNG side gets away with a single static
     * decoder. It emits whole MCUs rather than rows, so a band of one MCU row is assembled and
     * then fed to the scaler a row at a time, which keeps the budget's unit of work the same
     * size for both formats. */
    bool jpeg;
    pjpeg_image_info_t ji;
    uint8_t *band;        /**< band_h rows of eff_w RGB triples */
    int band_h;           /**< rows per MCU row: MCUHeight, or an eighth of it when reduced */
    int band_row;         /**< next band row to hand over; == band_h means refill */
    int eff_w, eff_h;     /**< source size the scaler sees, after any reduction */
    int src_y;            /**< source rows handed to the scaler so far */
    bool reduce;          /**< decoding DC-only, at an eighth scale */
} image_decoder_t;

/* One decode's stdio buffer. 16 KB spans several IDAT chunks of a 1000 px scan, so spng's
 * small reads are served from RAM instead of becoming filesystem calls. Freed with the decode,
 * and there is only ever one decode in flight. */
#define IMG_IO_BUFFER   16384

static image_decoder_t *decoder;

/* Defined below, but the JPEG path above it is the other caller. */
static void scaler_add_row (int src_y, const uint8_t *rgb);

/**
 * @brief Deinitialize the PNG decoder.
 * 
 * @param free_image Flag indicating whether to free the image.
 */
static void image_decoder_deinit (bool free_image) {
    if (decoder != NULL) {
        fclose(decoder->f);          /* before io_buffer: fclose still writes through it */
        if (decoder->io_buffer != NULL) {
            free(decoder->io_buffer);
        }
        if (decoder->ctx != NULL) {
            spng_ctx_free(decoder->ctx);
        }
        if ((decoder->image != NULL) && free_image) {
            surface_free(decoder->image);
            free(decoder->image);
        }
        if (decoder->row_buffer != NULL) {
            free(decoder->row_buffer);
        }
        if (decoder->acc != NULL) {
            free(decoder->acc);
        }
        if (decoder->colrun != NULL) {
            free(decoder->colrun);
        }
        if (decoder->band != NULL) {
            free(decoder->band);
        }
        free(decoder);
        decoder = NULL;
    }
}

/**
 * @brief Start decoding a PNG file.
 * 
 * @param path Path to the PNG file.
 * @param max_width Maximum width of the image.
 * @param max_height Maximum height of the image.
 * @param callback Callback function to be called upon completion.
 * @param callback_data Data to be passed to the callback function.
 * @return img_err_t Error code.
 */
/**
 * @brief Choose the source rect that covers a dst_w x dst_h destination, and record it.
 *
 * Crop rule is from docs/design/README.md section 7: scale to cover, never letterbox and never
 * squash; horizontal crop centred; vertical crop anchored 40 % from the top, which is what keeps
 * the logo on a portrait box scan -- and a quarter of the real corpus is portrait.
 *
 * Integer maths throughout: sw/sh against dst_w/dst_h by cross-multiplication, so there is no
 * float rounding to argue about at the edges.
 */
static img_err_t scaler_plan (image_decoder_t *d, int sw, int sh, int dst_w, int dst_h) {
    if (sw * dst_h > dst_w * sh) {
        d->crop_h = sh;                       /* source is wider: crop left and right */
        d->crop_w = (sh * dst_w) / dst_h;
        d->crop_x = (sw - d->crop_w) / 2;     /* centred */
        d->crop_y = 0;
    } else {
        d->crop_w = sw;                       /* source is taller: crop top and bottom */
        d->crop_h = (sw * dst_h) / dst_w;
        d->crop_x = 0;
        d->crop_y = ((sh - d->crop_h) * 2) / 5;  /* 40 % from the top, keeps the logo */
    }
    if (d->crop_w < 1) d->crop_w = 1;
    if (d->crop_h < 1) d->crop_h = 1;

    d->dst_w = dst_w;
    d->dst_h = dst_h;
    d->acc_row = -1;
    d->flushed_row = -1;

    /* Everything below is allocated exactly once, here. It used to be allocated in two places --
     * jpeg_open() and image_decoder_start_scaled() -- and the second overwrote the first, leaking
     * dst_w * 3 words on every JPEG decoded. One owner, one allocation. */
    d->acc    = calloc((size_t)dst_w * 3, sizeof(uint32_t));
    d->colrun = calloc((size_t)dst_w, sizeof(uint16_t));
    if (d->acc == NULL || d->colrun == NULL) {
        return IMG_ERR_OUT_OF_MEM;
    }

    /* How many source pixels each destination column folds in. This is the same for every source
     * row, so it is counted once here instead of being rebuilt per row: the old inner loop
     * carried the bucket arithmetic (a compare, an add and a conditional subtract per SOURCE
     * pixel, 2,081 of them on the widest card in the corpus) and a running per-column count. Both
     * are now a table lookup, and the accumulator lost its fourth lane with the count.
     *
     * Sums to crop_w by construction, so a row is consumed exactly as far as the crop reaches. */
    int dx = 0, dx_num = 0;
    for (int sx = 0; sx < d->crop_w && dx < dst_w; sx++) {
        d->colrun[dx]++;
        dx_num += dst_w;
        while (dx_num >= d->crop_w) {
            dx_num -= d->crop_w;
            dx++;
        }
    }

    return IMG_OK;
}

/** @brief Feed picojpeg from the already-open FILE. */
static unsigned char jpeg_need_bytes (unsigned char *buf, unsigned char n,
                                      unsigned char *actually_read, void *data) {
    size_t got = fread(buf, 1, n, (FILE *)data);
    *actually_read = (unsigned char)got;
    return 0;
}

/**
 * @brief Open a JPEG and size everything the scaler will need.
 *
 * The interesting decision here is @c reduce. picojpeg can keep only each block's DC
 * coefficient, which yields one pixel per 8x8 block and skips the AC dequantisation, the IDCT
 * and the chroma upsampling for every pixel of the image. That is close to free, so it is taken
 * whenever an eighth of the source still roughly covers the destination.
 *
 * "Roughly" is #JPEG_REDUCE_SLACK_NUM/DEN and it is the whole point of the rule. picojpeg offers
 * 1:1 or 1:8 and nothing in between, so a source just under 8x the tile used to fall off a cliff
 * into a full decode: a 1020 x 747 cover -- that size is in the corpus -- decodes 762,000 pixels
 * to fill a 13,720 pixel tile, against 12,000 pixels at DC-only. Demanding that an eighth cover
 * the tile EXACTLY bought a 1.09x upscale at roughly sixty times the cost.
 *
 * What is being upscaled matters to whether that is acceptable. DC-only is not a thumbnail
 * pulled out of the file, it is the exact 8x8 box average of every block, so a 1.14x upscale of
 * it is not the blurry mess the crop rule warns about -- that warning is about small art, and it
 * still holds: at more than 1/8 of the slack the full decode is taken.
 *
 * Deciding needs the dimensions, and the dimensions come from the header, so the header is read
 * twice: init, measure, rewind, init again asking for DC only. That second parse is a few
 * hundred bytes of markers against an image-sized saving.
 */
/** @brief How far below the destination an eighth-scale decode may land and still be taken.
 *
 * 7/8, i.e. at most a 1.14x upscale. Chosen, not measured: it is the smallest slack that clears
 * the 1020 x 747 cover (128 x 93 reduced against a 140 x 98 tile, 1.09x) with margin, and it is
 * small enough that the result is still denser than the tile in the dimension that binds after
 * the cover-crop. Widening it further trades picture for speed and wants a look, not a constant. */
#define JPEG_REDUCE_SLACK_NUM   7
#define JPEG_REDUCE_SLACK_DEN   8
/** @brief Map a picojpeg init failure, keeping "cannot" apart from "broken". */
static img_err_t jpeg_init_err (unsigned char e) {
    if (e == PJPG_UNSUPPORTED_MODE) {
        /* Progressive. Measured on the vendored picojpeg: a progressive file fails here at both
         * reduce settings and a baseline one decodes clean, colour or grayscale. Reporting it as
         * a bad file sent the user looking for a corrupt download. */
        debugf("JPEG: progressive, which picojpeg cannot decode -- re-save as baseline\n");
        return IMG_ERR_UNSUPPORTED;
    }
    return IMG_ERR_BAD_FILE;
}

static img_err_t jpeg_open (image_decoder_t *d, int dst_w, int dst_h) {
    rewind(d->f);
    unsigned char perr = pjpeg_decode_init(&d->ji, jpeg_need_bytes, d->f, 0);
    if (perr != 0) {
        return jpeg_init_err(perr);
    }

    int rw = (d->ji.m_width + 7) / 8;
    int rh = (d->ji.m_height + 7) / 8;
    if (dst_w > 0 && dst_h > 0 &&
        rw * JPEG_REDUCE_SLACK_DEN >= dst_w * JPEG_REDUCE_SLACK_NUM &&
        rh * JPEG_REDUCE_SLACK_DEN >= dst_h * JPEG_REDUCE_SLACK_NUM) {
        rewind(d->f);
        perr = pjpeg_decode_init(&d->ji, jpeg_need_bytes, d->f, 1);
        if (perr != 0) {
            return jpeg_init_err(perr);
        }
        d->reduce = true;
    }

    d->eff_w  = d->reduce ? rw : d->ji.m_width;
    d->eff_h  = d->reduce ? rh : d->ji.m_height;
    d->band_h = d->reduce ? (d->ji.m_MCUHeight / 8) : d->ji.m_MCUHeight;
    if (d->eff_w < 1 || d->eff_h < 1 || d->band_h < 1) {
        return IMG_ERR_BAD_FILE;
    }

    /* An unscaled caller still goes through the scaler, at 1:1. Keeping one path costs a copy
     * per row and removes a whole second set of edge cases nothing exercises. */
    if (dst_w <= 0 || dst_h <= 0) {
        dst_w = d->eff_w;
        dst_h = d->eff_h;
    }

    d->image = calloc(1, sizeof(surface_t));
    if (d->image == NULL) {
        return IMG_ERR_OUT_OF_MEM;
    }
    img_err_t serr = scaler_plan(d, d->eff_w, d->eff_h, dst_w, dst_h);
    if (serr != IMG_OK) {
        return serr;
    }
    *d->image = surface_alloc(FMT_RGBA16, dst_w, dst_h);
    if (d->image->buffer == NULL) {
        return IMG_ERR_OUT_OF_MEM;
    }
    /* Cleared for the same reason the PNG path clears: a destination row that no source row maps
     * to is never written by the scaler, and a truncated file stops partway down and delivers
     * what it has. Uncleared, both of those were whatever the heap last held -- and decode_done()
     * writes the result to thumbs.pak, so the garbage would have outlived the boot. */
    memset(d->image->buffer, 0, (size_t)d->image->stride * dst_h);

    d->band = malloc((size_t)d->band_h * d->eff_w * 3);
    if (d->band == NULL) {
        return IMG_ERR_OUT_OF_MEM;
    }

    d->band_row = d->band_h;     /* empty, so the first poll fills it */
    d->jpeg     = true;
    debugf("JPEG %dx%d %s -> %dx%d (band %d)\n", d->ji.m_width, d->ji.m_height,
           d->reduce ? "DC-only/8" : "full", dst_w, dst_h, d->band_h);
    return IMG_OK;
}

/**
 * @brief Decode one MCU row into the band buffer.
 *
 * picojpeg hands back each MCU as one to four 8x8 blocks of separate R, G and B planes, laid out
 * row-major within the MCU. Scattering them into a linear RGB band here is what lets the scaler
 * stay the single shared piece of code it is.
 */
static bool jpeg_fill_band (image_decoder_t *d) {
    int bx_n = d->ji.m_MCUWidth / 8;
    int by_n = d->ji.m_MCUHeight / 8;
    bool grey = (d->ji.m_comps == 1);

    memset(d->band, 0, (size_t)d->band_h * d->eff_w * 3);

    for (int mx = 0; mx < d->ji.m_MCUSPerRow; mx++) {
        if (pjpeg_decode_mcu() != 0) {
            return false;
        }
        for (int by = 0; by < by_n; by++) {
            for (int bx = 0; bx < bx_n; bx++) {
                int blk = (by * bx_n + bx) * 64;

                if (d->reduce) {
                    int px = mx * bx_n + bx;
                    if (px >= d->eff_w || by >= d->band_h) {
                        continue;
                    }
                    uint8_t *o = &d->band[((size_t)by * d->eff_w + px) * 3];
                    o[0] = d->ji.m_pMCUBufR[blk];
                    o[1] = grey ? o[0] : d->ji.m_pMCUBufG[blk];
                    o[2] = grey ? o[0] : d->ji.m_pMCUBufB[blk];
                    continue;
                }

                for (int y = 0; y < 8; y++) {
                    int py = by * 8 + y;
                    if (py >= d->band_h) {
                        break;
                    }
                    for (int x = 0; x < 8; x++) {
                        int px = mx * d->ji.m_MCUWidth + bx * 8 + x;
                        if (px >= d->eff_w) {
                            break;
                        }
                        int s = blk + y * 8 + x;
                        uint8_t *o = &d->band[((size_t)py * d->eff_w + px) * 3];
                        o[0] = d->ji.m_pMCUBufR[s];
                        o[1] = grey ? o[0] : d->ji.m_pMCUBufG[s];
                        o[2] = grey ? o[0] : d->ji.m_pMCUBufB[s];
                    }
                }
            }
        }
    }
    return true;
}

/** @brief One source row per call, so the budget's unit of work matches the PNG path's. */
static void jpeg_poll (void) {
    image_decoder_t *d = decoder;

    if (d->band_row >= d->band_h) {
        uint32_t t0 = TICKS_READ();
        bool ok = jpeg_fill_band(d);
        img_entropy_us += TIMER_MICROS(TICKS_SINCE(t0));
        if (!ok) {
            /* Truncated or malformed past this point. Rows already scaled are real, so a partial
             * image is delivered rather than thrown away -- a card that is three-quarters there
             * beats a placeholder, and the alternative discards work already paid for. */
            if (d->src_y > 0) {
                d->callback(IMG_OK, d->image, d->callback_data);
                image_decoder_deinit(false);
            } else {
                d->callback(IMG_ERR_BAD_FILE, NULL, d->callback_data);
                image_decoder_deinit(true);
            }
            return;
        }
        d->band_row = 0;
    }

    uint32_t st = TICKS_READ();
    scaler_add_row(d->src_y, &d->band[(size_t)d->band_row * d->eff_w * 3]);
    img_scale_us += TIMER_MICROS(TICKS_SINCE(st));

    d->band_row++;
    d->src_y++;
    d->decoded_rows++;

    /* Stop at the bottom of the crop, not at the bottom of the file. Every row below it is
     * discarded by scaler_add_row() anyway, and on this path that is a whole MCU row of Huffman
     * decode, IDCT and chroma upsampling per band spent on pixels nothing will ever read. */
    if (d->src_y >= d->eff_h || d->src_y >= d->crop_y + d->crop_h) {
        d->callback(IMG_OK, d->image, d->callback_data);
        image_decoder_deinit(false);
    }
}

/* dst_w/dst_h of 0 means "surface the size of the file"; non-zero means the streaming scaler
 * writes straight into a surface of that size and the file-sized one is never allocated.
 *
 * Taking the destination size HERE rather than fixing it up afterwards is the whole point. The
 * scaled path used to call image_decoder_start(), let it allocate a surface the size of the file,
 * then free that and allocate the small one -- so a 2118 x 1457 card in the corpus asked for
 * 6.17 MB on an 8 MB machine that already holds 1.84 MB of framebuffers, and failed with
 * IMG_ERR_OUT_OF_MEM before decoding a single row. The header comment promising 27 KB described
 * the intent of the code below it and not what it did. */
static img_err_t decoder_open (char *path, int max_width, int max_height,
                               img_callback_t *callback, void *callback_data,
                               int dst_w, int dst_h) {
    if (decoder != NULL) {
        return IMG_ERR_BUSY;
    }

    decoder = calloc(1, sizeof(image_decoder_t));
    if (decoder == NULL) {
        return IMG_ERR_OUT_OF_MEM;
    }

    if ((decoder->f = fopen(path, "rb")) == NULL) {
        image_decoder_deinit(false);
        return IMG_ERR_NO_FILE;
    }

    /* Upstream ran this file UNBUFFERED (setbuf(f, NULL)), which makes every read spng issues --
     * and it issues small ones, chunk headers and IDAT fragments -- a separate trip through the
     * filesystem. Measured: worst single decoded row 17,578 us, which is one whole 60 Hz field
     * to decode one row of one image, and it made the frame budget in thumbcache_run decorative
     * because the smallest unit of work it can stop on cost more than the entire frame.
     *
     * See docs/AUDIT.md for the before and after. Buffer costs IMG_IO_BUFFER bytes for the life
     * of one decode, which upstream could not spare in 4 MB and the M64 plainly can. */
    decoder->io_buffer = malloc(IMG_IO_BUFFER);
    if (decoder->io_buffer != NULL) {
        setvbuf(decoder->f, decoder->io_buffer, _IOFBF, IMG_IO_BUFFER);
    }

    /* Sniff the magic rather than trusting the extension. Art arrives named however whoever
     * made it felt at the time -- the card that prompted JPEG support carries fifteen .jpeg and
     * one .jpg -- and a PNG called .jpg should still draw. */
    unsigned char magic[2] = { 0, 0 };
    if (fread(magic, 1, 2, decoder->f) != 2) {
        image_decoder_deinit(false);
        return IMG_ERR_BAD_FILE;
    }
    rewind(decoder->f);

    if (magic[0] == 0xFF && magic[1] == 0xD8) {
        img_err_t jerr = jpeg_open(decoder, dst_w, dst_h);
        if (jerr != IMG_OK) {
            image_decoder_deinit(true);
            return jerr;
        }
        decoder->callback = callback;
        decoder->callback_data = callback_data;
        return IMG_OK;
    }

    if ((decoder->ctx = spng_ctx_new(SPNG_CTX_IGNORE_ADLER32)) == NULL) {
        image_decoder_deinit(false);
        return IMG_ERR_OUT_OF_MEM;
    }

    if (spng_set_crc_action(decoder->ctx, SPNG_CRC_USE, SPNG_CRC_USE) != SPNG_OK) {
        image_decoder_deinit(false);
        return IMG_ERR_INT;
    }

    if (spng_set_image_limits(decoder->ctx, max_width, max_height) != SPNG_OK) {
        image_decoder_deinit(false);
        return IMG_ERR_INT;
    }

    if (spng_set_png_file(decoder->ctx, decoder->f) != SPNG_OK) {
        image_decoder_deinit(false);
        return IMG_ERR_INT;
    }

    /* Read before the decode is set up, because it decides whether there will be one. */
    if (spng_get_ihdr(decoder->ctx, &decoder->ihdr) != SPNG_OK) {
        image_decoder_deinit(false);
        return IMG_ERR_BAD_FILE;
    }

    /* Adam7, refused for the same reason progressive JPEG is: this decoder consumes whole rows in
     * order and an interlaced PNG delivers neither.
     *
     * libspng's progressive API scatters each pass into the caller's buffer at strided offsets
     * (spng.c, spng_decode_row) and leaves the rest of the row untouched, so a "row" handed to
     * scaler_add_row() is between an eighth and a half real pixels with the PREVIOUS row's bytes
     * in the gaps. row_num also arrives out of order -- 0, 8, 16 ... then 4, 12 -- which the
     * accumulator reads as a new destination row every time and flushes over its own output. The
     * seven passes total 1.875x the image height, so image_decoder_get_progress() passes 1.0 as
     * well. It did not fail, it drew nonsense; a placeholder and a line in the log is better. */
    if (decoder->ihdr.interlace_method != 0) {
        debugf("PNG: interlaced (Adam7), which this decoder cannot consume -- re-save without interlacing\n");
        image_decoder_deinit(false);
        return IMG_ERR_UNSUPPORTED;
    }

    size_t image_size;

    if (spng_decoded_image_size(decoder->ctx, SPNG_FMT_RGB8, &image_size) != SPNG_OK) {
        image_decoder_deinit(false);
        return IMG_ERR_BAD_FILE;
    }

    if (spng_decode_image(decoder->ctx, NULL, image_size, SPNG_FMT_RGB8, SPNG_DECODE_PROGRESSIVE) != SPNG_OK) {
        image_decoder_deinit(false);
        return IMG_ERR_BAD_FILE;
    }

    decoder->image = calloc(1, sizeof(surface_t));
    if (decoder->image == NULL) {
        image_decoder_deinit(false);
        return IMG_ERR_OUT_OF_MEM;
    }

    int sw = (int)decoder->ihdr.width;
    int sh = (int)decoder->ihdr.height;

    if (dst_w > 0 && dst_h > 0) {
        if (sw <= 0 || sh <= 0) {
            image_decoder_deinit(false);
            return IMG_ERR_BAD_FILE;
        }
        img_err_t serr = scaler_plan(decoder, sw, sh, dst_w, dst_h);
        if (serr != IMG_OK) {
            image_decoder_deinit(false);
            return serr;
        }
        *decoder->image = surface_alloc(FMT_RGBA16, dst_w, dst_h);
        if (decoder->image->buffer != NULL) {
            /* Cleared as a backstop, and the comment that used to be here said why wrongly: it
             * claimed the tile is drawn while it is still filling, which decode_done() has not
             * done since it started publishing the surface only on completion. What the clear is
             * actually for is a decode that stops early -- a truncated file, or a destination row
             * the scaler's gap fill somehow never reaches -- landing on background rather than on
             * whatever the heap last held. */
            memset(decoder->image->buffer, 0, (size_t)decoder->image->stride * dst_h);
        }
    } else {
        *decoder->image = surface_alloc(FMT_RGBA16, sw, sh);
    }
    if (decoder->image->buffer == NULL) {
        image_decoder_deinit(true);
        return IMG_ERR_OUT_OF_MEM;
    }

    if ((decoder->row_buffer = malloc(decoder->ihdr.width * 3)) == NULL) {
        image_decoder_deinit(true);
        return IMG_ERR_OUT_OF_MEM;
    }

    decoder->decoded_rows = 0;

    decoder->callback = callback;
    decoder->callback_data = callback_data;

    return IMG_OK;
}

img_err_t image_decoder_start (char *path, int max_width, int max_height,
                             img_callback_t *callback, void *callback_data) {
    return decoder_open(path, max_width, max_height, callback, callback_data, 0, 0);
}

/**
 * @brief Decode a PNG straight into a dst_w x dst_h surface, cover-cropped.
 *
 * The real corpus people use -- n64-tools/n64-flashcart-menu-metadata -- is not the asset spec.
 * Measured over a 40-card stratified sample: sizes from 112 px to 1020 px wide, 11 of 40 in
 * PORTRAIT (Japanese box scans at 112x158), and 25 of 40 more than 0.05 off the 1.4286 aspect
 * the spec asks authors for. A decoder that only accepts 280 x 196 rejects essentially all of
 * it, and one that allocates a surface the size of the file needs 1.5 MB of intermediate for a
 * 1020 x 747 scan and then throws almost all of it away.
 *
 * So: pick the largest source rect with the destination's aspect, box-filter it down one row at
 * a time, and never hold more than the destination plus one accumulator row.
 *
 * Crop rule is from docs/design/README.md section 7: scale to cover, never letterbox and never
 * squash; horizontal crop centred; vertical crop anchored 40 % from the top, which is what
 * keeps the logo on a portrait box scan.
 */
img_err_t image_decoder_start_scaled (char *path, int dst_w, int dst_h,
                                    img_callback_t *callback, void *callback_data) {
    if (dst_w <= 0 || dst_h <= 0) {
        return IMG_ERR_INT;
    }

    /* Limits are generous rather than dst-sized: the point is to accept what the corpus actually
     * contains. 4096 covers every scan in it with room to spare.
     *
     * Everything the scaler needs is set up inside decoder_open(), by scaler_plan(). It used to be
     * set up again out here, which on the JPEG path silently leaked the buffer jpeg_open() had
     * already allocated. */
    return decoder_open(path, 4096, 4096, callback, callback_data, dst_w, dst_h);
}

/** @brief The start of destination row @p y. */
static inline uint16_t *scaler_row (image_decoder_t *d, int y) {
    return (uint16_t *)((uint8_t *)d->image->buffer + (size_t)y * d->image->stride);
}

/**
 * @brief Write the accumulated destination row, and any rows above it nothing landed on.
 *
 * @p last_row says this is the final source row of the crop, so the rows BELOW this one are the
 * scaler's last chance too.
 */
static void scaler_flush (image_decoder_t *d, int dst_row, bool last_row) {
    uint16_t *out = scaler_row(d, dst_row);
    uint32_t rows = d->acc_n;
    uint16_t prev = 0;
    bool have_prev = false;

    /* Two divisors, remembered. The count folded into a column is colrun[x] * rows, colrun takes
     * at most two values across a row and rows is fixed for the flush, so in practice this sees
     * two distinct divisors and alternates between them.
     *
     * The reciprocal is what the memo is protecting. Dividing three channels per column cost
     * three VR4300 divides -- ~37 cycles each, and they do not pipeline -- which is 3 x 19,600 of
     * them for a full tile. One divide per distinct divisor, then a multiply-high per channel,
     * turns that into ~2 divides per row.
     *
     * Exact, not approximate: with inv = ceil(2^32/n) the error term is a*(inv*n - 2^32)/2^32,
     * which stays below 1 while a*(n-1) < 2^32. Here a <= 255*n and n = colrun * rows is bounded
     * by the 4096 px image limit against the smallest tile (TILE_W_NARROW 109, TILE_H_MIN 64), so
     * n <= 38 * 65 and 255*n*(n-1) tops out around 1.55e9 against 4.29e9. Checked rather than
     * argued: an off-tree harness compared it against plain division over every divisor to 2,600
     * and 123 million dividends, and the equivalent of this loop against the divide-per-channel
     * version it replaces over 125 source/tile pairs, byte for byte.
     *
     * n == 1 is out on its own because ceil(2^32/1) does not fit in the word that holds it -- it
     * wrapped to zero and blacked out every such column, which is how the harness earned its
     * keep. It is also the common case rather than a corner: art close to tile size folds one
     * source pixel per column, 107 of 109 columns on a 112 px cover, so the branch pays for
     * itself twice over. */
    uint32_t d0 = 0, i0 = 0, d1 = 0, i1 = 0;

    for (int x = 0; x < d->dst_w; x++) {
        uint32_t n = (uint32_t)d->colrun[x] * rows;
        if (n == 0) {
            /* Upscaling: this column got no source pixel. Replicate the last real one --
             * nearest neighbour, which the asset spec asks for on undersized art because
             * blocky reads as a choice and blurry reads as a bug. */
            out[x] = have_prev ? prev : 0x0001;
            continue;
        }
        const uint32_t *a = &d->acc[x * 3];
        uint32_t r, g, b;
        if (n == 1) {
            r = a[0]; g = a[1]; b = a[2];
        } else {
            if (n != d0) {
                if (n == d1) {
                    uint32_t td = d0, ti = i0;
                    d0 = d1; i0 = i1; d1 = td; i1 = ti;
                } else {
                    d1 = d0; i1 = i0;
                    d0 = n;  i0 = 0xFFFFFFFFu / n + 1;
                }
            }
            r = (uint32_t)(((uint64_t)a[0] * i0) >> 32);
            g = (uint32_t)(((uint64_t)a[1] * i0) >> 32);
            b = (uint32_t)(((uint64_t)a[2] * i0) >> 32);
        }
        prev = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
        have_prev = true;
        out[x] = prev;
    }

    /* A leading run of empty columns cannot be filled forwards, so fill it backwards from the
     * first real pixel. Without this an upscaled card starts with a black stripe. */
    for (int x = 0; x < d->dst_w; x++) {
        if (d->colrun[x] != 0) {
            for (int k = 0; k < x; k++) {
                out[k] = out[x];
            }
            break;
        }
    }

    /* And the same thing one axis over, which was missing entirely. When the crop is shorter than
     * the destination, consecutive source rows land on destination rows more than one apart and
     * the rows between them were never written by anybody: a 126 x 126 cover into a 140 x 140
     * tile left 14 of 140 rows, a 96 x 96 one left 44. They came out as cleared background on the
     * PNG path and as heap contents on the JPEG path, and thumbstore_put() then cached the result.
     *
     * Filling from the row above is not a guess, it is the mapping read backwards: a destination
     * row r takes source row crop_y + floor(r * crop_h / dst_h), and for every row in a gap that
     * is the same source row that produced the row above it. Same nearest-neighbour rule the
     * empty columns get, for the same reason. */
    size_t bytes = (size_t)d->dst_w * sizeof(uint16_t);
    if (d->flushed_row >= 0) {
        for (int y = d->flushed_row + 1; y < dst_row; y++) {
            memcpy(scaler_row(d, y), scaler_row(d, d->flushed_row), bytes);
        }
    }
    d->flushed_row = dst_row;

    if (last_row) {
        for (int y = dst_row + 1; y < d->dst_h; y++) {
            memcpy(scaler_row(d, y), out, bytes);
        }
    }
}

/**
 * @brief Fold one decoded source row into the scaler's accumulator.
 *
 * Horizontal box filter into acc, flushed to the destination once every source row belonging
 * to the current destination row has arrived. How many source pixels each destination column
 * folds in is read from the table scaler_plan() built rather than counted here: it is the same
 * for every row, and when upscaling some columns receive no source pixel at all, which has to
 * stay distinguishable from "the accumulator is still zero" or genuinely black pixels would be
 * corrupted into a copy of their neighbour.
 */
static void scaler_add_row (int src_y, const uint8_t *rgb) {
    image_decoder_t *d = decoder;

    if (src_y < d->crop_y || src_y >= d->crop_y + d->crop_h) {
        return;                                  /* cropped away vertically */
    }

    int dst_row = ((src_y - d->crop_y) * d->dst_h) / d->crop_h;
    if (dst_row >= d->dst_h) {
        return;
    }

    if (dst_row != d->acc_row) {
        d->acc_row = dst_row;
        d->acc_n = 0;
        memset(d->acc, 0, (size_t)d->dst_w * 3 * sizeof(uint32_t));
    }

    /* The bucket arithmetic that used to live in here -- a compare, an add and a conditional
     * subtract per SOURCE pixel, plus a running count into a fourth accumulator lane -- is a
     * colrun[] lookup now. What is left is the three adds that are the actual work, and the
     * accumulator is three lanes instead of four, which is 1.6 KB of an 8 KB data cache rather
     * than 2.2 KB for a full-width tile. */
    const uint8_t *px = &rgb[d->crop_x * 3];
    uint32_t *a = d->acc;
    const uint16_t *run = d->colrun;

    for (int x = 0; x < d->dst_w; x++, a += 3) {
        uint32_t r = 0, g = 0, b = 0;
        for (int n = run[x]; n > 0; n--, px += 3) {
            r += px[0];
            g += px[1];
            b += px[2];
        }
        a[0] += r;
        a[1] += g;
        a[2] += b;
    }
    d->acc_n++;

    /* Flush once the next source row belongs to a different destination row. */
    int next_dst = ((src_y + 1 - d->crop_y) * d->dst_h) / d->crop_h;
    bool last_row = (src_y + 1 >= d->crop_y + d->crop_h);
    if (next_dst == dst_row && !last_row) {
        return;
    }

    scaler_flush(d, dst_row, last_row);
}

/**
 * @brief Abort the PNG decoding process.
 */
void image_decoder_abort (void) {
    image_decoder_deinit(true);
}

/**
 * @brief Get the progress of the PNG decoding process.
 * 
 * @return float Progress as a percentage.
 */
float image_decoder_get_progress (void) {
    if (!decoder) {
        return 0.0f;
    }

    int total = decoder->jpeg ? decoder->eff_h : (int)decoder->ihdr.height;
    return (total > 0) ? ((float)decoder->decoded_rows / total) : 0.0f;
}

/**
 * @brief Poll the PNG decoder to process the next row.
 */
void image_decoder_poll (void) {
    if (!decoder) {
        return;
    }

    if (decoder->jpeg) {
        jpeg_poll();
        return;
    }

    enum spng_errno err;
    struct spng_row_info row_info;

    if ((err = spng_get_row_info(decoder->ctx, &row_info)) != SPNG_OK) {
        decoder->callback(IMG_ERR_BAD_FILE, NULL, decoder->callback_data);
        image_decoder_deinit(true);
        return;
    }

    uint32_t inf_t0 = TICKS_READ();
    err = spng_decode_row(decoder->ctx, decoder->row_buffer, decoder->ihdr.width * 3);
    img_entropy_us += TIMER_MICROS(TICKS_SINCE(inf_t0));

    if (err == SPNG_OK || err == SPNG_EOI) {
        decoder->decoded_rows += 1;

        if (decoder->dst_w) {
            uint32_t st = TICKS_READ();
            scaler_add_row((int)row_info.row_num, decoder->row_buffer);
            img_scale_us += TIMER_MICROS(TICKS_SINCE(st));

            /* Nothing below the crop is ever read, so stop inflating at its last row rather than
             * at the end of the file. Worth 3-5 % of the rows on the large cards, and it costs a
             * comparison per row.
             *
             * Safe to leave the stream unfinished: scaler_add_row() flushes the last destination
             * row on exactly this row, deinit frees the spng context mid-decode as it already
             * does on every error path, and the image is complete by construction. */
            if ((int)row_info.row_num + 1 >= decoder->crop_y + decoder->crop_h) {
                decoder->callback(IMG_OK, decoder->image, decoder->callback_data);
                image_decoder_deinit(false);
                return;
            }
            goto row_done;
        }

        uint16_t *image_buffer = decoder->image->buffer + (row_info.row_num * decoder->image->stride);
        for (int i = 0; i < decoder->ihdr.width * 3; i += 3) {
            uint8_t r = decoder->row_buffer[i + 0] >> 3;
            uint8_t g = decoder->row_buffer[i + 1] >> 3;
            uint8_t b = decoder->row_buffer[i + 2] >> 3;
            *image_buffer++ = (r << 11) | (g << 6) | (b << 1) | 1;
        }
    }

row_done:
    if (err == SPNG_EOI) {
        decoder->callback(IMG_OK, decoder->image, decoder->callback_data);
        image_decoder_deinit(false);
    } else if (err != SPNG_OK) {
        decoder->callback(IMG_ERR_BAD_FILE, NULL, decoder->callback_data);
        image_decoder_deinit(true);
    }
}

/* One decoded row is the smallest unit of work the budget can stop on, so if a single row costs
 * more than the whole frame budget the budget is decorative. Counted rather than assumed: the
 * corpus runs to 2118 px wide and a row of that is a different animal to a row of 112. */
uint32_t img_rows_done = 0;
uint32_t img_worst_row_us = 0;
uint32_t img_entropy_us = 0, img_scale_us = 0;

/**
 * @brief Decode rows until the budget is spent. See image_decoder.h.
 */
void image_decoder_poll_budget (uint32_t budget_us) {
    if (!decoder) {
        return;
    }

    uint32_t start = TICKS_READ();
    do {
        uint32_t row_start = TICKS_READ();
        image_decoder_poll();
        uint32_t row_us = TIMER_MICROS(TICKS_SINCE(row_start));
        img_rows_done++;
        if (row_us > img_worst_row_us) {
            img_worst_row_us = row_us;
        }
        // image_decoder_poll() clears `decoder` on completion or error, so this doubles as the
        // termination check for a finished image.
        if (!decoder) {
            return;
        }
    } while (TIMER_MICROS(TICKS_SINCE(start)) < budget_us);
}
