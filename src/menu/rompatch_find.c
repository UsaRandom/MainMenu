/**
 * @file rompatch_find.c
 * @brief The half of rompatch that decides WHERE to patch, kept free of libdragon on purpose.
 * @ingroup menu
 *
 * Separated from rompatch.c so tools/hosttest/test_rompatch.c can compile and run it natively.
 * That matters more here than anywhere else in this codebase: this is the code that picks two
 * words of somebody's game to overwrite, ares cannot exercise it at all -- there is no cartridge
 * under ares, so the whole path is skipped -- and the failure mode on a real console is a game
 * that boots into corruption with no explanation.
 *
 * See rompatch.h for what the patch is for.
 */

#include <string.h>

#include "rompatch.h"

/* Read in 8 KB bites. Over the PI that is one DMA per chunk instead of one per word; on the host
 * it is just a buffer. The three-word overlap below is sized against this. */
#define SCAN_WORDS (2048)

/* The four instructions osInitialize copies onto every exception vector. Only the two address
 * halves vary between games, which is what makes it findable at all:
 *
 *     lui   $k0, %hi(__osException)
 *     addiu $k0, $k0, %lo(__osException)
 *     jr    $k0
 *     nop
 *
 * Literals rather than vr4300_asm.h's macros, for the same reason tools/preamblescan.py carries
 * literals: that header assembles instructions through a bitfield union, and bitfield allocation
 * order follows the target's endianness, so on the little-endian host this file must also compile
 * for, I_JR(REG_K0) comes out 0x20000680 instead of 0x03400008. src/dev/hooktest.c pins these
 * four values against the macros in a MIPS build, so a drift is a red check rather than a silent
 * disagreement about which bytes of a game to overwrite. */
#define PREAMBLE_LUI_HI16   (0x3C1Au)
#define PREAMBLE_ADDIU_HI16 (0x275Au)
#define PREAMBLE_JR_K0      (0x03400008u)
#define PREAMBLE_NOP        (0x00000000u)

/* IPL3 does not always load to the entry point in the header: two CICs shift it. Same two numbers
 * as rom_info.c's fix_boot_address(), duplicated because that function takes a rom_cic_type_t and
 * this file has a cic_type_t. Getting it wrong does not look like an error -- it looks like a
 * preamble whose target is a megabyte away from it, which is exactly how five of the twenty-four
 * ROMs on the reference card first read out of tools/preamblescan.py. */
/* What a run of zeros has to be to hold engine code. Above MAX it is a data void rather than
 * padding -- the 9,204-byte run inside Ocarina's compressed assets that AUDIT 2w wrote a
 * trampoline into is the case this excludes; below MIN it is not worth the risk. */
#define GAP_MIN_BYTES  32u
#define GAP_MAX_BYTES  0x400u

/**
 * @brief `jr rs` or a `j` tail call -- how a MIPS function ends, and what padding follows.
 *
 * `jr $zero` is excluded, and that one exclusion is the difference between this rule working and
 * not. The word 0x00000008 *is* a well-formed `jr $zero`, no compiler emits one, and it is a very
 * common data value: accepting it let the scan mistake Ocarina's table of `{pointer, length}`
 * records at 0x800067c8 for the end of a function.
 */
static bool is_return (uint32_t w) {
    if ((w & 0xFC1FFFFFu) == 0x08u) {
        return ((w >> 21) & 0x1Fu) != 0;
    }
    return (w >> 26) == 0x02u && (w & 0x03FFFFFFu) != 0;
}

bool rompatch_run_is_padding (uint32_t before2, uint32_t before1, uint32_t run_bytes) {
    if (run_bytes < GAP_MIN_BYTES || run_bytes > GAP_MAX_BYTES) {
        return false;
    }
    return is_return(before1) || is_return(before2);
}

uint32_t rompatch_entry_for (cic_type_t cic_type, uint32_t header_entry) {
    switch (cic_type) {
        case CIC_x103: return header_entry - 0x100000;
        case CIC_x106: return header_entry - 0x200000;
        default:       return header_entry;
    }
}

/** @brief The reconstructed target of a preamble at @p words, or 0 if it is not one. */
static uint32_t preamble_target (const uint32_t *w) {
    if ((w[0] >> 16) != PREAMBLE_LUI_HI16)   { return 0; }
    if ((w[1] >> 16) != PREAMBLE_ADDIU_HI16) { return 0; }
    if (w[2] != PREAMBLE_JR_K0)              { return 0; }
    if (w[3] != PREAMBLE_NOP)                { return 0; }

    /* %lo is sign-extended by addiu, which is why the compiler pre-biases %hi. Reconstructing it
     * the same way gives the address the game will actually jump to. */
    int32_t lo = (int16_t)(w[1] & 0xFFFF);
    return ((w[0] & 0xFFFF) << 16) + (uint32_t)lo;
}

/**
 * @brief Walk the loaded megabyte for a preamble, taking the first that survives every test.
 *
 * The address test is not cosmetic. tools/preamblescan.py ran this pattern over the 24 N64 ROMs
 * on the reference card and two of them -- Conker's Bad Fur Day and GoldenEye 007 -- match a run
 * of data whose reconstructed target is 0x100071e0 and 0x700101a0. Neither is RDRAM. Without the
 * test those two get two words of live game code rewritten at a coincidence. One in twelve.
 */
bool rompatch_find (romcrc_read_t read, void *ctx, uint32_t entry, uint32_t ram_top,
                    rompatch_result_t *out) {
    uint32_t buf[SCAN_WORDS];

    /* Overlapping windows by three words, so a preamble that straddles a chunk boundary is still
     * seen whole. Cheaper than the alternative and impossible to get subtly wrong. */
    for (uint32_t at = 0; at < ROMCRC_LENGTH; at += (SCAN_WORDS - 3) * 4) {
        uint32_t want = SCAN_WORDS * 4;
        if (at + want > ROMCRC_LENGTH) {
            want = ROMCRC_LENGTH - at;
        }
        if (want < 16) {
            break;
        }
        if (!read(ctx, ROMCRC_START + at, buf, want)) {
            return false;
        }

        for (uint32_t i = 0; i + 4 <= want / 4; i++) {
            uint32_t target = preamble_target(&buf[i]);
            if (target == 0) {
                continue;
            }
            out->candidates++;

            /* The target has to be an address, and it has to be the address libultra links:
             * __osException sits immediately after the stub that jumps to it, so a genuine match
             * points exactly sixteen bytes forward. */
            uint32_t ram = entry + at + i * 4;
            if (target < 0x80000000u || target >= ram_top || target != ram + 16) {
                out->rejected++;
                continue;
            }

            out->found = true;
            out->rom_offset = ROMCRC_START + at + i * 4;
            out->ram_address = ram;
            out->target = target;
            out->word0 = buf[i];
            out->word1 = buf[i + 1];
            return true;
        }
    }

    return false;
}

