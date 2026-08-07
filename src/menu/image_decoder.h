/**
 * @file image_decoder.h
 * @brief PNG decoder
 * @ingroup menu 
 */

#ifndef IMAGE_DECODER_H__
#define IMAGE_DECODER_H__

#include <surface.h>

/** 
 * @brief PNG decoder errors 
 * 
 * Enumeration for different types of errors that can occur in the PNG decoder.
 */
typedef enum {
    IMG_OK,               /**< No error */
    IMG_ERR_INT,          /**< Internal error */
    IMG_ERR_BUSY,         /**< Decoder is busy */
    IMG_ERR_OUT_OF_MEM,   /**< Out of memory error */
    IMG_ERR_NO_FILE,      /**< No file found error */
    IMG_ERR_BAD_FILE,     /**< Bad file error */
    /** A well-formed image in a form this decoder cannot read, as opposed to a broken one.
     *  Today that means exactly one thing: a progressive JPEG. picojpeg has no progressive
     *  support by design -- see PJPG_UNSUPPORTED_MODE in picojpeg.h -- so the file is rejected
     *  at init. Kept separate from IMG_ERR_BAD_FILE because the two want different answers from
     *  a user: one file needs converting, the other is damaged. */
    IMG_ERR_UNSUPPORTED,
} img_err_t;

/**
 * @brief PNG decoder callback type.
 * 
 * This typedef defines the callback function type used by the PNG decoder.
 * 
 * @param err Error code indicating the result of the decoding process.
 * @param decoded_image Pointer to the decoded image surface.
 * @param callback_data User-defined data passed to the callback function.
 */
typedef void img_callback_t (img_err_t err, surface_t *decoded_image, void *callback_data);

/**
 * @brief Start the PNG decoding process.
 * 
 * This function starts the PNG decoding process for the specified file.
 * 
 * @param path Path to the PNG file.
 * @param max_width Maximum width of the decoded image.
 * @param max_height Maximum height of the decoded image.
 * @param callback Callback function to be called when decoding is complete.
 * @param callback_data User-defined data to be passed to the callback function.
 * @return img_err_t Error code indicating the result of the start operation.
 */
img_err_t image_decoder_start (char *path, int max_width, int max_height, img_callback_t *callback, void *callback_data);

/**
 * @brief Read @p path's dimensions without decoding it.
 *
 * An fopen, a few hundred bytes and an fclose -- no decoder state, no allocation, and it does not
 * claim the single global decoder, so it is safe to call while a decode is in flight. PNG and
 * JPEG, sniffed from the magic rather than the extension.
 *
 * Exists because a tile's shape is taken from its cover's aspect (see library/boxart.h) and the
 * shape is the decode's destination size, so it has to be known first.
 *
 * @return IMG_OK with @p w and @p h set, or an error with both zeroed.
 */
img_err_t image_probe_size (const char *path, int *w, int *h);

/**
 * @brief Decode a PNG straight into a @p dst_w x @p dst_h surface, scaled and cover-cropped.
 *
 * Accepts any source size and aspect, which the real art corpus requires: it ranges from
 * 112 px to 1020 px wide and a quarter of it is portrait. Scales to cover and crops -- never
 * letterboxes, never squashes -- with the horizontal crop centred and the vertical crop
 * anchored 40 % from the top so a portrait box scan keeps its logo.
 *
 * Allocates only the destination surface plus one accumulator row: a 1020 x 747 scan costs
 * 27 KB rather than 1.5 MB. This is now true of the code as well as of the intent -- it used to
 * allocate the file-sized surface first and free it, which put a 2118 x 1457 card in the corpus
 * at 6.17 MB and failed it outright with IMG_ERR_OUT_OF_MEM.
 */
img_err_t image_decoder_start_scaled (char *path, int dst_w, int dst_h,
                                    img_callback_t *callback, void *callback_data);

/**
 * @brief Decode rows until @p budget_us microseconds have been spent.
 *
 * image_decoder_poll() manages exactly one pixel row per call. That is the right shape for an
 * interactive screen decoding one image, and hopeless for filling a grid: a 196-row card needs
 * 196 main-loop iterations, so a 20-tile working set takes nearly four thousand frames to
 * appear. This spends a stated slice of the frame instead, so the caller can trade fill rate
 * against how fast art lands and can say which it chose.
 *
 * Always manages at least one row, so a budget of zero still makes progress rather than
 * deadlocking the job that is waiting on it.
 */
void image_decoder_poll_budget (uint32_t budget_us);

/** Rows decoded, and the worst single row, since these were last zeroed. */
extern uint32_t img_rows_done, img_worst_row_us;
/** Row cost split: time inside spng_decode_row vs inside the box-filter scaler. */
extern uint32_t img_entropy_us, img_scale_us;

/**
 * @brief Abort the PNG decoding process.
 *
 * This function aborts the ongoing PNG decoding process.
 */
void image_decoder_abort (void);

/**
 * @brief Get the progress of the PNG decoding process.
 * 
 * This function returns the current progress of the PNG decoding process as a percentage.
 * 
 * @return float Current progress of the decoding process (0.0 to 100.0).
 */
float image_decoder_get_progress (void);

/**
 * @brief Poll the PNG decoder.
 * 
 * This function polls the PNG decoder to handle any ongoing decoding tasks.
 */
void image_decoder_poll (void);

#endif /* IMAGE_DECODER_H__ */
