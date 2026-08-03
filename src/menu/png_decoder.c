/**
 * @file png_decoder.c
 * @brief PNG Decoder component implementation
 * @ingroup ui_components
 */

#include <stdio.h>
#include <libdragon.h>
#include <libspng/spng/spng.h>
#include "png_decoder.h"
#include "utils/fs.h"

/** @brief PNG File Information Structure. */
typedef struct {
    FILE *f; /**< File pointer */
    spng_ctx *ctx; /**< SPNG context */
    struct spng_ihdr ihdr; /**< SPNG image header */
    surface_t *image; /**< Image surface */
    uint8_t *row_buffer; /**< Row buffer */
    char *io_buffer; /**< stdio buffer for f; see the setvbuf call in png_decoder_start() */
    int decoded_rows; /**< Number of decoded rows */
    png_callback_t *callback; /**< Callback function */
    void *callback_data; /**< Callback data */

    /* Streaming scaler. When dst_w is non-zero the decoder box-filters each source row
     * straight into a dst_w x dst_h surface instead of allocating one the size of the file.
     * See png_decoder_start_scaled(). */
    int dst_w, dst_h;
    int crop_x, crop_y, crop_w, crop_h;  /**< source rect that covers the destination */
    uint32_t *acc;                       /**< dst_w * 3 channel accumulator for one dest row */
    uint32_t acc_n;                      /**< source rows folded into the current dest row */
    int acc_row;                         /**< which destination row acc is building */
} png_decoder_t;

/* One decode's stdio buffer. 16 KB spans several IDAT chunks of a 1000 px scan, so spng's
 * small reads are served from RAM instead of becoming filesystem calls. Freed with the decode,
 * and there is only ever one decode in flight. */
#define PNG_IO_BUFFER   16384

static png_decoder_t *decoder;

/**
 * @brief Deinitialize the PNG decoder.
 * 
 * @param free_image Flag indicating whether to free the image.
 */
static void png_decoder_deinit (bool free_image) {
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
 * @return png_err_t Error code.
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
static void scaler_plan_crop (png_decoder_t *d, int sw, int sh, int dst_w, int dst_h) {
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

/* dst_w/dst_h of 0 means "surface the size of the file"; non-zero means the streaming scaler
 * writes straight into a surface of that size and the file-sized one is never allocated.
 *
 * Taking the destination size HERE rather than fixing it up afterwards is the whole point. The
 * scaled path used to call png_decoder_start(), let it allocate a surface the size of the file,
 * then free that and allocate the small one -- so a 2118 x 1457 card in the corpus asked for
 * 6.17 MB on an 8 MB machine that already holds 1.84 MB of framebuffers, and failed with
 * PNG_ERR_OUT_OF_MEM before decoding a single row. The header comment promising 27 KB described
 * the intent of the code below it and not what it did. */
static png_err_t decoder_open (char *path, int max_width, int max_height,
                               png_callback_t *callback, void *callback_data,
                               int dst_w, int dst_h) {
    if (decoder != NULL) {
        return PNG_ERR_BUSY;
    }

    decoder = calloc(1, sizeof(png_decoder_t));
    if (decoder == NULL) {
        return PNG_ERR_OUT_OF_MEM;
    }

    if ((decoder->f = fopen(path, "rb")) == NULL) {
        png_decoder_deinit(false);
        return PNG_ERR_NO_FILE;
    }

    /* Upstream ran this file UNBUFFERED (setbuf(f, NULL)), which makes every read spng issues --
     * and it issues small ones, chunk headers and IDAT fragments -- a separate trip through the
     * filesystem. Measured: worst single decoded row 17,578 us, which is one whole 60 Hz field
     * to decode one row of one image, and it made the frame budget in thumbcache_run decorative
     * because the smallest unit of work it can stop on cost more than the entire frame.
     *
     * See docs/AUDIT.md for the before and after. Buffer costs PNG_IO_BUFFER bytes for the life
     * of one decode, which upstream could not spare in 4 MB and the M64 plainly can. */
    decoder->io_buffer = malloc(PNG_IO_BUFFER);
    if (decoder->io_buffer != NULL) {
        setvbuf(decoder->f, decoder->io_buffer, _IOFBF, PNG_IO_BUFFER);
    }

    if ((decoder->ctx = spng_ctx_new(SPNG_CTX_IGNORE_ADLER32)) == NULL) {
        png_decoder_deinit(false);
        return PNG_ERR_OUT_OF_MEM;
    }

    if (spng_set_crc_action(decoder->ctx, SPNG_CRC_USE, SPNG_CRC_USE) != SPNG_OK) {
        png_decoder_deinit(false);
        return PNG_ERR_INT;
    }

    if (spng_set_image_limits(decoder->ctx, max_width, max_height) != SPNG_OK) {
        png_decoder_deinit(false);
        return PNG_ERR_INT;
    }

    if (spng_set_png_file(decoder->ctx, decoder->f) != SPNG_OK) {
        png_decoder_deinit(false);
        return PNG_ERR_INT;
    }

    size_t image_size;

    if (spng_decoded_image_size(decoder->ctx, SPNG_FMT_RGB8, &image_size) != SPNG_OK) {
        png_decoder_deinit(false);
        return PNG_ERR_BAD_FILE;
    }

    if (spng_decode_image(decoder->ctx, NULL, image_size, SPNG_FMT_RGB8, SPNG_DECODE_PROGRESSIVE) != SPNG_OK) {
        png_decoder_deinit(false);
        return PNG_ERR_BAD_FILE;
    }

    if (spng_get_ihdr(decoder->ctx, &decoder->ihdr) != SPNG_OK) {
        png_decoder_deinit(false);
        return PNG_ERR_BAD_FILE;
    }

    decoder->image = calloc(1, sizeof(surface_t));
    if (decoder->image == NULL) {
        png_decoder_deinit(false);
        return PNG_ERR_OUT_OF_MEM;
    }

    int sw = (int)decoder->ihdr.width;
    int sh = (int)decoder->ihdr.height;

    if (dst_w > 0 && dst_h > 0) {
        if (sw <= 0 || sh <= 0) {
            png_decoder_deinit(false);
            return PNG_ERR_BAD_FILE;
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
        png_decoder_deinit(true);
        return PNG_ERR_OUT_OF_MEM;
    }

    if ((decoder->row_buffer = malloc(decoder->ihdr.width * 3)) == NULL) {
        png_decoder_deinit(true);
        return PNG_ERR_OUT_OF_MEM;
    }

    decoder->decoded_rows = 0;

    decoder->callback = callback;
    decoder->callback_data = callback_data;

    return PNG_OK;
}

png_err_t png_decoder_start (char *path, int max_width, int max_height,
                             png_callback_t *callback, void *callback_data) {
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
png_err_t png_decoder_start_scaled (char *path, int dst_w, int dst_h,
                                    png_callback_t *callback, void *callback_data) {
    if (dst_w <= 0 || dst_h <= 0) {
        return PNG_ERR_INT;
    }

    /* Limits are generous rather than dst-sized: the point is to accept what the corpus actually
     * contains. 4096 covers every scan in it with room to spare. */
    png_err_t err = decoder_open(path, 4096, 4096, callback, callback_data, dst_w, dst_h);
    if (err != PNG_OK) {
        return err;
    }

    png_decoder_t *d = decoder;

    d->acc = calloc((size_t)dst_w * 4, sizeof(uint32_t));
    if (d->acc == NULL) {
        png_decoder_abort();
        return PNG_ERR_OUT_OF_MEM;
    }

    d->dst_w = dst_w;
    d->dst_h = dst_h;
    d->acc_row = -1;

    return PNG_OK;
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
static void scaler_add_row (int src_y) {
    png_decoder_t *d = decoder;

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
    const uint8_t *px = &d->row_buffer[d->crop_x * 3];
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
void png_decoder_abort (void) {
    png_decoder_deinit(true);
}

/**
 * @brief Get the progress of the PNG decoding process.
 * 
 * @return float Progress as a percentage.
 */
float png_decoder_get_progress (void) {
    if (!decoder) {
        return 0.0f;
    }

    return (float) (decoder->decoded_rows) / (decoder->ihdr.height);
}

/**
 * @brief Poll the PNG decoder to process the next row.
 */
void png_decoder_poll (void) {
    if (!decoder) {
        return;
    }

    enum spng_errno err;
    struct spng_row_info row_info;

    if ((err = spng_get_row_info(decoder->ctx, &row_info)) != SPNG_OK) {
        decoder->callback(PNG_ERR_BAD_FILE, NULL, decoder->callback_data);
        png_decoder_deinit(true);
        return;
    }

    uint32_t inf_t0 = TICKS_READ();
    err = spng_decode_row(decoder->ctx, decoder->row_buffer, decoder->ihdr.width * 3);
    png_inflate_us += TIMER_MICROS(TICKS_SINCE(inf_t0));

    if (err == SPNG_OK || err == SPNG_EOI) {
        decoder->decoded_rows += 1;

        if (decoder->dst_w) {
            uint32_t st = TICKS_READ();
            scaler_add_row((int)row_info.row_num);
            png_scale_us += TIMER_MICROS(TICKS_SINCE(st));
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
        decoder->callback(PNG_OK, decoder->image, decoder->callback_data);
        png_decoder_deinit(false);
    } else if (err != SPNG_OK) {
        decoder->callback(PNG_ERR_BAD_FILE, NULL, decoder->callback_data);
        png_decoder_deinit(true);
    }
}

/* One decoded row is the smallest unit of work the budget can stop on, so if a single row costs
 * more than the whole frame budget the budget is decorative. Counted rather than assumed: the
 * corpus runs to 2118 px wide and a row of that is a different animal to a row of 112. */
uint32_t png_rows_done = 0;
uint32_t png_worst_row_us = 0;
uint32_t png_inflate_us = 0, png_scale_us = 0;

/**
 * @brief Decode rows until the budget is spent. See png_decoder.h.
 */
void png_decoder_poll_budget (uint32_t budget_us) {
    if (!decoder) {
        return;
    }

    uint32_t start = TICKS_READ();
    do {
        uint32_t row_start = TICKS_READ();
        png_decoder_poll();
        uint32_t row_us = TIMER_MICROS(TICKS_SINCE(row_start));
        png_rows_done++;
        if (row_us > png_worst_row_us) {
            png_worst_row_us = row_us;
        }
        // png_decoder_poll() clears `decoder` on completion or error, so this doubles as the
        // termination check for a finished image.
        if (!decoder) {
            return;
        }
    } while (TIMER_MICROS(TICKS_SINCE(start)) < budget_us);
}
