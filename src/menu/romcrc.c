/**
 * @file romcrc.c
 * @brief See romcrc.h for what this is and why a wrong answer is a console that will not boot.
 * @ingroup menu
 */

#include <string.h>

#include "romcrc.h"

/* The bootcode's seeds. Not derivable from anything -- they are constants inside each IPL3, and
 * the only way to know them is to have read that IPL3 or to have checked the answer against real
 * ROMs. Both were done: 23 of the 24 N64 ROMs on the reference card reproduce their stored
 * checksum with these, across four of the five. */
#define SEED_6102 (0xF8CA4DDCu)     /* also 6101 and 7102 */
#define SEED_6103 (0xA3886759u)
#define SEED_6105 (0xDF26F436u)
#define SEED_6106 (0x1FEA617Au)

/* CIC 6105's bootcode mixes in a 256-byte window of ITSELF as it goes, at this offset, rather
 * than the running accumulator every other CIC uses. Cycled by the low byte of the position, so
 * each pass over the game re-reads the same 64 words in the same order. */
#define X105_TABLE_OFFSET (0x0750)
#define X105_TABLE_BYTES  (0x100)

/* One PI transfer per iteration would be 262,144 of them. This is the compromise between that and
 * holding the whole megabyte, and it is on the stack of a function the menu calls once. */
#define CHUNK_WORDS (512)

static uint32_t rotate_left (uint32_t value, uint32_t by) {
    by &= 31;
    return by ? ((value << by) | (value >> (32 - by))) : value;
}

static bool seed_for (cic_type_t cic_type, uint32_t *seed) {
    switch (cic_type) {
        case CIC_6101:
        case CIC_7102:
        case CIC_x102: *seed = SEED_6102; return true;
        case CIC_x103: *seed = SEED_6103; return true;
        case CIC_x105: *seed = SEED_6105; return true;
        case CIC_x106: *seed = SEED_6106; return true;
        default:       return false;
    }
}

bool romcrc_compute (romcrc_read_t read, void *ctx, cic_type_t cic_type,
                     uint32_t *crc1, uint32_t *crc2) {
    return romcrc_compute_ex(read, ctx, cic_type, false, crc1, crc2);
}

bool romcrc_compute_ex (romcrc_read_t read, void *ctx, cic_type_t cic_type, bool ipl3_nop_486,
                        uint32_t *crc1, uint32_t *crc2) {
    uint32_t seed;
    if (read == NULL || !seed_for(cic_type, &seed)) {
        return false;
    }

    /* Read once, outside the loop. It is the same 64 words every pass and the loop runs a quarter
     * of a million times. */
    uint32_t table[X105_TABLE_BYTES / 4];
    if (cic_type == CIC_x105) {
        if (!read(ctx, X105_TABLE_OFFSET, table, sizeof(table))) {
            return false;
        }
        if (ipl3_nop_486) {
            table[ROMCRC_X105_NOP_WORD] = 0;
        }
    }

    uint32_t t1 = seed, t2 = seed, t3 = seed, t4 = seed, t5 = seed, t6 = seed;
    uint32_t chunk[CHUNK_WORDS];

    for (uint32_t at = 0; at < ROMCRC_LENGTH; at += CHUNK_WORDS * 4) {
        if (!read(ctx, ROMCRC_START + at, chunk, sizeof(chunk))) {
            return false;
        }

        for (uint32_t i = 0; i < CHUNK_WORDS; i++) {
            uint32_t d = chunk[i];
            uint32_t r = rotate_left(d, d & 0x1F);

            /* t4 counts the carries out of t6, which is why the add has to be tested before it
             * happens rather than after. */
            if ((uint32_t)(t6 + d) < t6) {
                t4++;
            }
            t6 += d;
            t3 ^= d;
            t5 += r;
            t2 ^= (t2 > d) ? r : (t6 ^ d);

            if (cic_type == CIC_x105) {
                uint32_t byte_pos = (at + i * 4) & 0xFF;
                t1 += table[byte_pos / 4] ^ d;
            } else {
                t1 += t5 ^ d;
            }
        }
    }

    switch (cic_type) {
        case CIC_x103:
            *crc1 = (t6 ^ t4) + t3;
            *crc2 = (t5 ^ t2) + t1;
            break;
        case CIC_x106:
            *crc1 = (t6 * t4) + t3;
            *crc2 = (t5 * t2) + t1;
            break;
        default:
            *crc1 = t6 ^ t4 ^ t3;
            *crc2 = t5 ^ t2 ^ t1;
            break;
    }

    return true;
}

bool romcrc_verify (romcrc_read_t read, void *ctx, cic_type_t cic_type,
                    uint32_t *stored1, uint32_t *stored2) {
    uint32_t header[2];
    if (read == NULL || !read(ctx, 0x10, header, sizeof(header))) {
        return false;
    }
    if (stored1 != NULL) {
        *stored1 = header[0];
    }
    if (stored2 != NULL) {
        *stored2 = header[1];
    }

    uint32_t crc1, crc2;
    if (!romcrc_compute(read, ctx, cic_type, &crc1, &crc2)) {
        return false;
    }

    return (crc1 == header[0]) && (crc2 == header[1]);
}
