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
    uint32_t *acc;                       /**< dst_w * 3 channel accumulator for one dest row */
    uint32_t acc_n;                      /**< source rows folded into the current dest row */
    int acc_row;                         /**< which destination row acc is building */

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
static void scaler_plan_crop (image_decoder_t *d, int sw, int sh, int dst_w, int dst_h) {
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
 * whenever an eighth of the source is still at least the destination size -- below that the tile
 * would be upscaled from a thumbnail, which the crop rule calls blurry and the asset spec calls
 * a bug.
 *
 * Deciding needs the dimensions, and the dimensions come from the header, so the header is read
 * twice: init, measure, rewind, init again asking for DC only. That second parse is a few
 * hundred bytes of markers against an image-sized saving.
 */
static img_err_t jpeg_open (image_decoder_t *d, int dst_w, int dst_h) {
    rewind(d->f);
    if (pjpeg_decode_init(&d->ji, jpeg_need_bytes, d->f, 0) != 0) {
        return IMG_ERR_BAD_FILE;
    }

    int rw = (d->ji.m_width + 7) / 8;
    int rh = (d->ji.m_height + 7) / 8;
    if (dst_w > 0 && dst_h > 0 && rw >= dst_w && rh >= dst_h) {
        rewind(d->f);
        if (pjpeg_decode_init(&d->ji, jpeg_need_bytes, d->f, 1) != 0) {
            return IMG_ERR_BAD_FILE;
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
    scaler_plan_crop(d, d->eff_w, d->eff_h, dst_w, dst_h);
    *d->image = surface_alloc(FMT_RGBA16, dst_w, dst_h);
    if (d->image->buffer == NULL) {
        return IMG_ERR_OUT_OF_MEM;
    }

    d->band = malloc((size_t)d->band_h * d->eff_w * 3);
    d->acc  = calloc((size_t)dst_w * 4, sizeof(uint32_t));
    if (d->band == NULL || d->acc == NULL) {
        return IMG_ERR_OUT_OF_MEM;
    }

    d->dst_w    = dst_w;
    d->dst_h    = dst_h;
    d->acc_row  = -1;
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

    if (d->src_y >= d->eff_h) {
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

    size_t image_size;

    if (spng_decoded_image_size(decoder->ctx, SPNG_FMT_RGB8, &image_size) != SPNG_OK) {
        image_decoder_deinit(false);
        return IMG_ERR_BAD_FILE;
    }

    if (spng_decode_image(decoder->ctx, NULL, image_size, SPNG_FMT_RGB8, SPNG_DECODE_PROGRESSIVE) != SPNG_OK) {
        image_decoder_deinit(false);
        return IMG_ERR_BAD_FILE;
    }

    if (spng_get_ihdr(decoder->ctx, &decoder->ihdr) != SPNG_OK) {
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
        scaler_plan_crop(decoder, sw, sh, dst_w, dst_h);
        *decoder->image = surface_alloc(FMT_RGBA16, dst_w, dst_h);
        if (decoder->image->buffer != NULL) {
            /* Cleared because the tile is drawn while it is still filling, so undecoded rows are
             * on screen and must be background rather than whatever the heap last held. */
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
     * contains. 4096 covers every scan in it with room to spare. */
    img_err_t err = decoder_open(path, 4096, 4096, callback, callback_data, dst_w, dst_h);
    if (err != IMG_OK) {
        return err;
    }

    image_decoder_t *d = decoder;

    d->acc = calloc((size_t)dst_w * 4, sizeof(uint32_t));
    if (d->acc == NULL) {
        image_decoder_abort();
        return IMG_ERR_OUT_OF_MEM;
    }

    d->dst_w = dst_w;
    d->dst_h = dst_h;
    d->acc_row = -1;

    return IMG_OK;
}

/**
 * @brief Fold one decoded source row into the scaler's accumulator.
 *
 * Horizontal box filter into acc, flushed to the destination once every source row belonging
 * to the current destination row has arrived. Per-column counts are kept explicitly rather
 * than derived, because when upscaling some destination columns receive no source pixel at
 * all and inferring that from "the accumulator is still zero" would corrupt genuinely black
 * pixels into a copy of their neighbour.
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
        memset(d->acc, 0, d->dst_w * 4 * sizeof(uint32_t));
    }

    /* dx advanced incrementally rather than recomputed as (sx * dst_w) / crop_w. That division
     * ran once per SOURCE pixel -- 2,118 of them on the widest card in the corpus -- and the
     * VR4300's integer divide is ~37 cycles and does not pipeline. Measured at 15 % of row cost;
     * the other 85 % is inside spng_decode_row and no scheduling change touches it. */
    int dx = 0, dx_num = 0;
    const uint8_t *px = &rgb[d->crop_x * 3];
    for (int sx = 0; sx < d->crop_w; sx++, px += 3) {
        if (dx >= d->dst_w) {
            break;
        }
        uint32_t *a = &d->acc[dx * 4];
        a[0] += px[0];
        a[1] += px[1];
        a[2] += px[2];
        a[3] += 1;

        dx_num += d->dst_w;
        while (dx_num >= d->crop_w) {
            dx_num -= d->crop_w;
            dx++;
        }
    }

    /* Flush once the next source row belongs to a different destination row. */
    int next_dst = ((src_y + 1 - d->crop_y) * d->dst_h) / d->crop_h;
    bool last_row = (src_y + 1 >= d->crop_y + d->crop_h);
    if (next_dst == dst_row && !last_row) {
        return;
    }

    uint16_t *out = (uint16_t *)((uint8_t *)d->image->buffer + dst_row * d->image->stride);
    uint16_t prev = 0;
    bool have_prev = false;

    for (int x = 0; x < d->dst_w; x++) {
        uint32_t *a = &d->acc[x * 4];
        if (a[3] == 0) {
            /* Upscaling: this column got no source pixel. Replicate the last real one --
             * nearest neighbour, which the asset spec asks for on undersized art because
             * blocky reads as a choice and blurry reads as a bug. */
            out[x] = have_prev ? prev : 0x0001;
            continue;
        }
        uint32_t r = a[0] / a[3];
        uint32_t g = a[1] / a[3];
        uint32_t b = a[2] / a[3];
        prev = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
        have_prev = true;
        out[x] = prev;
    }

    /* A leading run of empty columns cannot be filled forwards, so fill it backwards from the
     * first real pixel. Without this an upscaled card starts with a black stripe. */
    for (int x = 0; x < d->dst_w; x++) {
        if (d->acc[x * 4 + 3] != 0) {
            for (int k = 0; k < x; k++) {
                out[k] = out[x];
            }
            break;
        }
    }
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
