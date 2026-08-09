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

/**
 * @brief A 16-bit access to an odd address, which on this engine is a hang and not a bad cheat.
 *
 * `sh` or `lhu` to an odd address raises an Address Error, and this code runs *at* the general
 * exception vector with EXL already set. A nested exception there does not update EPC and vectors
 * straight back to 0x80000180, into this engine, into the same store: the console locks solid and
 * only the power switch gets it back.
 *
 * It is not hypothetical, which is why the check is here rather than in a comment. Measured over
 * the keyed corpus: 1,964 of 149,687 16-bit writes name an odd address, spread over 1,179 groups
 * -- AeroGauge's "Name 1" is `8108CD69 8108CD6A 8108CD6B`, three consecutive bytes typed as
 * 16-bit writes. They were carried by every version of this engine before this one.
 *
 * @param word The raw address word, whose type byte's bit 0 must genuinely mean "16 bits wide".
 */
static bool halfword_misaligned (uint32_t word) {
    return (((word >> 24) & 0x01u) != 0) && ((word & 1u) != 0);
}

uint32_t rompatch_body_words (const uint32_t *list, int *lines) {
    if (list[0] == 0 && list[1] == 0) {
        return 0;                       /* the terminator is not a body */
    }
    uint32_t type = (list[0] >> 24) & 0xFFu;
    bool gs = (type & (1u << 3)) != 0;

    /* The GS-button bit is bit 3 of a *write* or *conditional* type byte and means nothing
     * anywhere else. Reading it out of 0xEE or 0xFF is a category error, and one this project
     * made: mkcheatdb reported 51 groups as "GS-button-only" that are nothing of the sort. */
    if (((type & 0xF0u) == 0x80u) || ((type & 0xF0u) == 0xA0u)
        || (type == 0xF0u) || (type == 0xF1u)) {
        /* Boot writes are the same three instructions as ordinary ones; see rompatch.h. */
        *lines = 1;
        if (gs && ((type & 0xF0u) != 0xF0u)) {
            return 0;
        }
        return halfword_misaligned(list[0]) ? 0u : 3u;
    }
    if (type == 0xEEu) {
        *lines = 1;
        return (uint32_t)EXPANSION_OFF_WORDS;
    }
    if (type == 0x50u) {
        /* A repeater is two entries: itself, and the write it multiplies. A count of zero would
         * emit nothing in Datel's unrolled engine and would underflow the loop counter into four
         * billion iterations in this one, so it is refused rather than special-cased. The step
         * has to keep the run aligned as well as the address it starts from. */
        uint32_t count = (list[0] >> 8) & 0xFFu;
        uint32_t step = list[0] & 0xFFu;
        uint32_t next = (list[2] >> 24) & 0xFFu;
        bool next_write = ((next & 0xF0u) == 0x80u) || ((next & 0xF0u) == 0xA0u);
        bool tail = (list[2] == 0 && list[3] == 0);
        if (count == 0 || tail || !next_write || (next & (1u << 3))) {
            return 0;
        }
        if (halfword_misaligned(list[2]) || (((next & 0x01u) != 0) && ((step & 1u) != 0))) {
            return 0;
        }
        *lines = 2;
        return ((int16_t)(list[1] & 0xFFFFu) != 0) ? (uint32_t)REPEAT_WORDS_INCR
                                                   : (uint32_t)REPEAT_WORDS_FLAT;
    }
    return 0;                           /* 0xCC, 0xDE, 0xFF and friends emit nothing at all */
}

int rompatch_test_branch (int tests, int k, uint32_t body) {
    /* Test k's branch is the fourth word of its own block, its delay slot the fifth. Everything
     * after it -- the remaining `tests - k - 1` blocks of four, and the body -- has to be cleared,
     * so the target is `4*(tests-k-1) + body` words past the delay slot.
     *
     * The delay slot is the *next* thing's first word rather than a `nop`, which always executes
     * and is always harmless: for the last test that is the body's `lui`, loading a register whose
     * store is being skipped anyway, and for any earlier test it is the next test's `lui`, doing
     * the same. Writing the obvious `nop` here would put every target one word short -- on the
     * store, executing exactly what the conditional exists to prevent. */
    return 4 * (tests - k - 1) + (int)body;
}

uint32_t rompatch_atom_words (const uint32_t *list, int *lines, int *tests_out) {
    /* Conditionals stack: `D0 D0 80` is "if both, write", and the corpus has runs of up to five.
     * Each is four words in front of a body they all branch past, so the run and its body are one
     * indivisible block -- priced here, and placed as one atom by place_engine(). `& 0xF8 == 0xD0`
     * is D0-D3 with the GS-button bit clear, so a `D8` ends the run and is then priced as a body,
     * which refuses it. */
    int tests = 0;
    while ((((list[2u * (size_t)tests] >> 24) & 0xF8u) == 0xD0u)) {
        if (halfword_misaligned(list[2u * (size_t)tests])) {
            break;                      /* an `lhu` off an odd address hangs exactly as `sh` does */
        }
        tests++;
    }

    int consumed = 0;
    uint32_t body = rompatch_body_words(&list[2u * (size_t)tests], &consumed);
    if (body == 0) {
        *lines = 1;
        *tests_out = 0;
        return 0;
    }
    *lines = tests + consumed;
    *tests_out = tests;
    return 4u * (uint32_t)tests + body;
}

bool rompatch_cheats_fit (const uint32_t *cheat_list, int *lines_out) {
    if (cheat_list == NULL) {
        return false;
    }
    int lines = 0;
    uint32_t words = 4;                 /* the tail */
    bool ok = true;

    for (size_t i = 0; !(cheat_list[i] == 0 && cheat_list[i + 1] == 0); ) {
        int consumed = 0, tests = 0;
        uint32_t atom = rompatch_atom_words(&cheat_list[i], &consumed, &tests);
        if (atom == 0) {
            /* Walk on one line rather than stopping, so lines_out still describes the whole
             * selection -- it is what the launch screen and the log report. */
            ok = false;
        }
        words += atom;
        lines += consumed;
        i += 2u * (size_t)consumed;
    }

    if (lines_out != NULL) {
        *lines_out = lines;
    }
    /* All or nothing, and deliberately not per line. A `D0` conditional and the write it guards
     * are one indivisible thing, so a selection carrying anything this engine cannot emit is
     * refused whole. AUDIT 2.2 is what per-line filtering did. */
    return lines > 0 && ok && words <= (uint32_t)ENGINE_MAX_WORDS;
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

