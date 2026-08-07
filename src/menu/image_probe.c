/**
 * @file image_probe.c
 * @brief An image's dimensions, without a decoder.
 * @ingroup menu
 *
 * Its own file rather than a function in image_decoder.c, for two reasons that pull the same way.
 *
 * It shares nothing with the decoder: no spng context, no picojpeg state, no row buffer, no
 * global instance. Living next to them invited the assumption that it could reuse decoder_open(),
 * which would claim the single decoder and evict a decode in flight.
 *
 * And it is the piece most worth testing on a host. Walking a JPEG marker chain is fiddly, and
 * getting it wrong is silent: the probe returns an error, the record falls back to its system's
 * box shape, and every JPEG cover on the card is shaped by a table instead of by itself. Nothing
 * looks broken. tools/hosttest/test_boxart.c compiles this file natively and feeds it a dozen
 * files including a truncated one; that is only affordable because there is nothing else in here.
 */

#include <stdio.h>

#include "image_decoder.h"

/**
 * @brief Read an image's dimensions without decoding it, or opening a decoder.
 *
 * Deliberately not routed through decoder_open(). That allocates a row buffer and, for a JPEG,
 * runs pjpeg_decode_init() -- which reads far enough into the file to build Huffman tables -- and
 * it also claims the single global decoder instance, so probing would evict a decode in flight.
 * This is an fopen, a read of at most a few hundred bytes, and an fclose.
 *
 * The point of it is that a tile's shape is now taken from the source's aspect (library/boxart.h)
 * and the shape has to be known BEFORE the decode, because it is the decode's destination size.
 * The answer is persisted in library.idx, so this runs once per record per index rather than once
 * per boot.
 *
 * The format is sniffed from the magic, not the extension, for the same reason decoder_open()
 * does it: the card that prompted JPEG support carries fifteen `.jpeg` and one `.jpg`, and a PNG
 * called `.jpg` should still work.
 */
img_err_t image_probe_size (const char *path, int *w, int *h) {
    if (path == NULL || w == NULL || h == NULL) {
        return IMG_ERR_INT;
    }
    *w = *h = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return IMG_ERR_NO_FILE;
    }

    unsigned char b[24];
    img_err_t err = IMG_ERR_BAD_FILE;

    if (fread(b, 1, 2, f) != 2) {
        fclose(f);
        return IMG_ERR_BAD_FILE;
    }

    if (b[0] == 0xFF && b[1] == 0xD8) {
        /* JPEG: walk the marker chain to the first SOFn, which carries the dimensions.
         *
         * SOF0/1/2 are baseline, extended and progressive. Progressive is included on purpose
         * even though picojpeg cannot decode one -- knowing the shape of a file we are about to
         * fail on is free, and refusing to read it here would make a progressive cover fall back
         * to its system's default shape for a reason unrelated to its shape.
         *
         * Skipped: the standalone markers, which carry no length field. D0-D7 are restart
         * markers and 01 is TEM; treating either as a segment reads its next two bytes as a
         * length and walks off into the entropy-coded data. */
        while (fread(b, 1, 2, f) == 2) {
            if (b[0] != 0xFF) {
                break;                          /* lost sync; not a marker chain any more */
            }
            unsigned char m = b[1];
            if (m == 0xFF) {
                fseek(f, -1, SEEK_CUR);         /* fill byte; the next byte is the marker */
                continue;
            }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
                continue;                       /* standalone, no payload */
            }
            if (m == 0xD9 || m == 0xDA) {
                break;                          /* end of image, or start of scan: too far */
            }
            if (fread(b, 1, 2, f) != 2) {
                break;
            }
            int len = (b[0] << 8) | b[1];
            if (len < 2) {
                break;
            }
            /* SOF0..SOF15 except the four that are not frame headers (C4 DHT, C8 JPG,
             * CC DAC). Any of them carries precision, height, width in that order. */
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                if (len >= 7 && fread(b, 1, 5, f) == 5) {
                    *h = (b[1] << 8) | b[2];
                    *w = (b[3] << 8) | b[4];
                    err = (*w > 0 && *h > 0) ? IMG_OK : IMG_ERR_BAD_FILE;
                }
                break;
            }
            if (fseek(f, len - 2, SEEK_CUR) != 0) {
                break;
            }
        }
        fclose(f);
        return err;
    }

    /* PNG: the IHDR is the first chunk and is at a fixed offset, so there is no chain to walk.
     * Signature (8) + length (4) + type (4), then width and height. */
    rewind(f);
    if (fread(b, 1, 24, f) == 24 &&
        b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' &&
        b[12] == 'I' && b[13] == 'H' && b[14] == 'D' && b[15] == 'R') {
        *w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
        *h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
        err = (*w > 0 && *h > 0) ? IMG_OK : IMG_ERR_BAD_FILE;
    }
    fclose(f);
    return err;
}
