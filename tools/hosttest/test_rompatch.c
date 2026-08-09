/*
 * Exercise the code that decides which two words of somebody's game get rewritten.
 *
 * rompatch_find() is the most dangerous function in this repository: it picks a site in a
 * cartridge image and the caller then writes over it. It cannot be run under ares, because ares
 * has no cartridge -- flashcart_is_dummy() is true and the whole path is skipped. So it runs here,
 * against images built to contain exactly the things that have gone wrong on real ROMs.
 *
 * With a ROM directory in ROMDIR it also runs over real games and cross-checks against
 * tools/preamblescan.py's published answers for the reference card. That part skips itself when
 * the directory is absent, loudly, because the suite may not assume anyone's ROMs are present.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu/rompatch.h"

#define IMAGE_WORDS ((ROMCRC_START + ROMCRC_LENGTH) / 4)
#define ENTRY       (0x80000400u)
#define RAM_TOP     (0x80800000u)

static int checks;
static int failures;

static void check (bool ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static bool read_image (void *ctx, uint32_t offset, void *dst, size_t len) {
    const uint32_t *rom = ctx;
    if ((offset % 4) || (len % 4) || (offset + len > ROMCRC_START + ROMCRC_LENGTH)) {
        return false;
    }
    memcpy(dst, &rom[offset / 4], len);
    return true;
}

/* The four words as libultra emits them, computing @p target into $k0. Written out by hand rather
 * than through vr4300_asm.h, which cannot be used here: it assembles through a bitfield union and
 * bitfield order follows the target's endianness, so on this host the macros produce nonsense.
 * That is the same reason tools/preamblescan.py carries literals -- and the reason those literals
 * are pinned to the real encodings by src/dev/hooktest.c, on MIPS. */
static void plant (uint32_t *rom, uint32_t rom_offset, uint32_t target) {
    uint32_t hi = (target >> 16) & 0xFFFF;
    uint32_t lo = target & 0xFFFF;
    if (lo & 0x8000) {
        hi = (hi + 1) & 0xFFFF;                 /* addiu sign-extends, so %hi is pre-biased */
    }
    rom[rom_offset / 4 + 0] = 0x3C1A0000u | hi;         /* lui   $k0, %hi   */
    rom[rom_offset / 4 + 1] = 0x275A0000u | lo;         /* addiu $k0, $k0   */
    rom[rom_offset / 4 + 2] = 0x03400008u;              /* jr    $k0        */
    rom[rom_offset / 4 + 3] = 0x00000000u;              /* nop              */
}

static uint32_t *blank_image (void) {
    return calloc(IMAGE_WORDS, 4);
}

/** @brief ROM offset of the word that lands at RDRAM @p ram, given ENTRY. */
static uint32_t rom_at (uint32_t ram) {
    return ROMCRC_START + (ram - ENTRY);
}

int main (void) {
    /* An empty image must find nothing rather than settle for the first run of zeros. A nop is
     * three quarters of the pattern. */
    {
        uint32_t *rom = blank_image();
        rompatch_result_t r = {0};
        check(!rompatch_find(read_image, rom, ENTRY, RAM_TOP, &r), "an empty image has no preamble");
        check(r.candidates == 0, "and nothing that even looks like one");
        free(rom);
    }

    /* The ordinary case, well inside the window. */
    {
        uint32_t *rom = blank_image();
        uint32_t site = ENTRY + 0x21F0;                  /* Ocarina's, per preamblescan.py */
        plant(rom, rom_at(site), site + 16);

        rompatch_result_t r = {0};
        check(rompatch_find(read_image, rom, ENTRY, RAM_TOP, &r), "a well-formed preamble is found");
        check(r.ram_address == site, "at the right RDRAM address");
        check(r.rom_offset == rom_at(site), "and the right cartridge offset");
        check(r.target == site + 16, "with __osException sixteen bytes on");
        check(r.word0 == rom[rom_at(site) / 4] && r.word1 == rom[rom_at(site) / 4 + 1],
              "and the two displaced words reported verbatim");
        check(r.rejected == 0, "nothing rejected");
        free(rom);
    }

    /* Conker's Bad Fur Day and GoldenEye 007 both match a run of data whose reconstructed target
     * is not RDRAM. Without the address test the patcher rewrites two words of live game code at
     * a coincidence -- one in twelve of the ROMs on the reference card. Both exact values. */
    {
        const uint32_t bogus[] = { 0x100071E0u, 0x700101A0u };
        for (size_t i = 0; i < 2; i++) {
            uint32_t *rom = blank_image();
            uint32_t site = ENTRY + 0x1000;
            plant(rom, rom_at(site), bogus[i]);

            rompatch_result_t r = {0};
            char what[80];
            snprintf(what, sizeof(what), "a target of %08x is not an address and is walked past",
                     bogus[i]);
            check(!rompatch_find(read_image, rom, ENTRY, RAM_TOP, &r), what);
            check(r.candidates == 1 && r.rejected == 1, "and is counted as seen and rejected");
            free(rom);
        }
    }

    /* A target inside RDRAM but not adjacent. preamblescan.py calls these "odd" and found five on
     * the reference card; they are jump tables and the like, not preambles. */
    {
        uint32_t *rom = blank_image();
        uint32_t site = ENTRY + 0x1000;
        plant(rom, rom_at(site), site + 0x4000);

        rompatch_result_t r = {0};
        check(!rompatch_find(read_image, rom, ENTRY, RAM_TOP, &r),
              "a KSEG0 target that is not exactly +16 is rejected too");
        free(rom);
    }

    /* A target past the top of this machine's RDRAM. 0x80800000 is the whole 8 MB of an M64. */
    {
        uint32_t *rom = blank_image();
        uint32_t site = ENTRY + 0x1000;
        plant(rom, rom_at(site), 0x80900000u);

        rompatch_result_t r = {0};
        check(!rompatch_find(read_image, rom, ENTRY, RAM_TOP, &r),
              "a target above the top of RDRAM is rejected");
        free(rom);
    }

    /* A bogus one first and a real one later: the scan must keep going rather than stop at the
     * first thing shaped like a match. This is the behaviour that saves Conker. */
    {
        uint32_t *rom = blank_image();
        uint32_t bad = ENTRY + 0x1000;
        uint32_t good = ENTRY + 0x30000;
        plant(rom, rom_at(bad), 0x100071E0u);
        plant(rom, rom_at(good), good + 16);

        rompatch_result_t r = {0};
        check(rompatch_find(read_image, rom, ENTRY, RAM_TOP, &r),
              "a rejected match does not stop the scan");
        check(r.ram_address == good, "and the real one after it is taken");
        check(r.candidates == 2 && r.rejected == 1, "with both counted");
        free(rom);
    }

    /* The scan reads in 8 KB chunks with a three-word overlap. A preamble straddling a boundary
     * is the one case that overlap exists for, and it is exactly the kind of thing that is right
     * until someone tunes the chunk size. Every straddle position, not just one. */
    {
        for (int off = -3; off <= 0; off++) {
            uint32_t *rom = blank_image();
            /* (2048 - 3) words per step is the stride; put the site just before the first
             * boundary, at each of the four alignments that span it. */
            uint32_t site = ENTRY + (uint32_t)(((2048 - 3) + off) * 4);
            plant(rom, rom_at(site), site + 16);

            rompatch_result_t r = {0};
            char what[80];
            snprintf(what, sizeof(what), "a preamble straddling a chunk boundary by %d words "
                     "is still seen whole", -off);
            check(rompatch_find(read_image, rom, ENTRY, RAM_TOP, &r) && r.ram_address == site,
                  what);
            free(rom);
        }
    }

    /* The last four words of the window are still in it. An off-by-one at the end would be
     * invisible on every real ROM and wrong on exactly one. */
    {
        uint32_t *rom = blank_image();
        uint32_t site = ENTRY + ROMCRC_LENGTH - 16;
        plant(rom, rom_at(site), site + 16);

        rompatch_result_t r = {0};
        check(rompatch_find(read_image, rom, ENTRY, RAM_TOP, &r) && r.ram_address == site,
              "a preamble in the last four words of the window is found");
        free(rom);
    }

    /* The CIC entry shift. Five ROMs on the reference card first read as preambles a megabyte
     * from their target because tools/preamblescan.py did not apply it; rom_info.c has had the
     * same two numbers all along. */
    check(rompatch_entry_for(CIC_x102, 0x80000400) == 0x80000400, "6102 loads where the header says");
    check(rompatch_entry_for(CIC_x103, 0x80100400) == 0x80000400, "6103 loads a megabyte below");
    check(rompatch_entry_for(CIC_x106, 0x80200400) == 0x80000400, "6106 loads two megabytes below");
    check(rompatch_entry_for(CIC_x105, 0x80000400) == 0x80000400, "6105 loads where the header says");

    /* Which runs of zeros the engine may be written into. Every row is measured from a real ROM,
     * and the two groups are not hypothetical: an engine chained across the rejected runs
     * black-screened Ocarina, and the accepted ones have booted and run it. See AUDIT 2y.
     *
     * The `jr $zero` row is the whole reason this is a named function rather than two lines
     * inlined in the scan. 0x00000008 is a well-formed `jr $zero`, no compiler emits one, and it
     * is an extremely common data value -- accepting it made a table of {pointer, length} records
     * look exactly like the end of a function. */
    check(rompatch_run_is_padding(0x01400008, 0x27BD7750, 44),
          "Ocarina rom+001034: jr $t2, delay slot, then padding");
    check(rompatch_run_is_padding(0x03E00008, 0xA02B902F, 108),
          "Ocarina rom+004174: jr $ra, delay slot, then padding");
    check(rompatch_run_is_padding(0x27BD0028, 0x03E00008, 108),
          "Donkey Kong 64 rom+005c04: jr $ra whose nop delay slot the run swallowed");
    check(!rompatch_run_is_padding(0x80006D30, 0x00000008, 32),
          "Ocarina rom+0073e0: a {pointer, length} record, not a function -- jr $zero is data");
    check(!rompatch_run_is_padding(0x00000000, 0x00000001, 44),
          "Ocarina rom+007404: zeros inside a table are not padding");
    check(!rompatch_run_is_padding(0x03E00008, 0x00000000, 9204),
          "Ocarina rom+04125c: 9 KB of zeros is a data void, whatever precedes it (AUDIT 2w)");
    check(!rompatch_run_is_padding(0x03E00008, 0x00000000, 16),
          "a four-word hole is too small to be worth the risk");
    check(rompatch_run_is_padding(0x00000000, 0x08001A03, 64),
          "a `j` tail call ends a function too");
    check(!rompatch_run_is_padding(0x00000000, 0x08000000, 64),
          "...but `j 0` is a zero word wearing an opcode");

    /* Which selections the engine will carry. All or nothing across the whole selection, because
     * a D0 and the write it guards are one indivisible thing and half a group is worse than none
     * (AUDIT 2.2). Ocarina's "Infinite Magic" is the case this was written for: four lines whose
     * first is a conditional, refused outright until the engine learned to branch. */
    {
        int lines = 0;
        const uint32_t plain[]   = { 0x8111ACAE, 0x0140, 0, 0 };
        const uint32_t magic[]   = { 0xD011ACB9, 0x0008, 0x8011ACBA, 0x0001,
                                     0x8011ACBC, 0x0001, 0x8011ACB3, 0x0060, 0, 0 };
        const uint32_t dangling[] = { 0x8111ACAE, 0x0140, 0xD011ACB9, 0x0008, 0, 0 };
        const uint32_t stacked[]  = { 0xD011ACB9, 0x0008, 0xD011ACBA, 0x0001, 0, 0 };
        const uint32_t gsbutton[] = { 0xD811ACB9, 0x0008, 0x8011ACBA, 0x0001, 0, 0 };
        const uint32_t gswrite[]  = { 0x8811ACBA, 0x0001, 0, 0 };
        const uint32_t repeater[] = { 0x50000502, 0x0000, 0x8111ACBA, 0x0001, 0, 0 };
        const uint32_t repincr[]  = { 0x50000502, 0x0001, 0x8111ACBA, 0x0001, 0, 0 };
        const uint32_t rep0[]     = { 0x50000002, 0x0000, 0x8111ACBA, 0x0001, 0, 0 };
        const uint32_t repdangle[] = { 0x50000502, 0x0000, 0, 0 };
        const uint32_t bootwrite[] = { 0xF1000318, 0x0000, 0, 0 };
        const uint32_t expoff[]   = { 0xEE000000, 0x0000, 0, 0 };
        const uint32_t setentry[] = { 0xDE000000, 0x0000, 0, 0 };
        const uint32_t twotests[] = { 0xD011ACB9, 0x0008, 0xD011ACBA, 0x0001,
                                      0x8011ACBC, 0x0001, 0, 0 };
        const uint32_t condrep[]  = { 0xD011ACB9, 0x0008, 0x50000502, 0x0000,
                                      0x8111ACBA, 0x0001, 0, 0 };

        check(rompatch_cheats_fit(plain, &lines) && lines == 1,
              "one plain 16-bit write fits, and counts as one line");
        check(rompatch_cheats_fit(magic, &lines) && lines == 4,
              "Ocarina's Infinite Magic: a conditional and three writes, four lines, carried");
        check(!rompatch_cheats_fit(dangling, NULL),
              "a conditional with nothing after it is refused, not silently dropped");
        check(!rompatch_cheats_fit(stacked, NULL),
              "a run of conditionals with no body after it is refused");
        check(!rompatch_cheats_fit(gsbutton, NULL),
              "a GS-button conditional is refused -- there is no button to read");
        check(!rompatch_cheats_fit(gswrite, NULL), "nor a GS-button write");
        check(!rompatch_cheats_fit(NULL, NULL), "no selection at all is not a selection that fits");

        check(rompatch_cheats_fit(repeater, &lines) && lines == 2,
              "a repeater and the write it multiplies: two lines, carried as a loop");
        check(rompatch_cheats_fit(repincr, NULL), "and again when the value increments too");
        check(!rompatch_cheats_fit(rep0, NULL),
              "a repeater counting zero times is refused -- the loop counter would underflow to 2^32");
        check(!rompatch_cheats_fit(repdangle, NULL), "a repeater with no write after it is refused");
        check(rompatch_cheats_fit(bootwrite, &lines) && lines == 1,
              "a boot-time write is carried, as the ordinary write it is");
        check(rompatch_cheats_fit(expoff, NULL), "disabling the Expansion Pak is carried");
        check(!rompatch_cheats_fit(setentry, NULL),
              "0xDE is a special that emits nothing, not a GS-button conditional");
        check(rompatch_cheats_fit(twotests, &lines) && lines == 3,
              "two conditionals stack in front of one write");
        check(rompatch_cheats_fit(condrep, &lines) && lines == 3,
              "and a conditional can guard a repeater");

        /* The hang, which is the one refusal that is about the console and not about the cheat.
         * `sh`/`lhu` off an odd address takes an Address Error at the exception vector with EXL
         * set, which vectors straight back in and locks the machine. 1,964 of the corpus's 149,687
         * 16-bit writes name one, and every engine before this one carried them. */
        {
            const uint32_t oddw[] = { 0x8108CD69, 0x0041, 0, 0 };
            const uint32_t oddb[] = { 0x8008CD69, 0x0041, 0, 0 };
            const uint32_t oddc[] = { 0xD111ACB9, 0x0008, 0x8011ACBA, 0x0001, 0, 0 };
            const uint32_t oddrep[] = { 0x50000501, 0x0000, 0x8111ACBA, 0x0001, 0, 0 };
            check(!rompatch_cheats_fit(oddw, NULL),
                  "a 16-bit write to an odd address is refused: it would hang, not misbehave");
            check(rompatch_cheats_fit(oddb, NULL), "...while the 8-bit write beside it is fine");
            check(!rompatch_cheats_fit(oddc, NULL), "an `lhu` off an odd address hangs the same way");
            check(!rompatch_cheats_fit(oddrep, NULL),
                  "and a repeater stepping a 16-bit write by an odd 1 goes odd on iteration two");
        }

        /* The budget, from both sides. A plain write is three words and the tail is four, so
         * (128 - 4) / 3 = 41 writes fit and 42 do not. Getting this off by one would overrun
         * engine[] in install(), which is a stack array. */
        static uint32_t many[2 * 64 + 2];
        for (int n = 41; n <= 42; n++) {
            for (int i = 0; i < n; i++) {
                many[i * 2] = 0x8011AD00u + (uint32_t)i;
                many[i * 2 + 1] = 0x0009;
            }
            many[n * 2] = 0;
            many[n * 2 + 1] = 0;
            bool got = rompatch_cheats_fit(many, NULL);
            check(got == (n == 41), (n == 41) ? "41 plain writes fit the engine budget"
                                              : "42 do not, and are refused before anything is written");
        }

        /* And the same from the conditional side, which is the accounting that is easy to get
         * wrong: a guarded pair is SEVEN words, not three. (128 - 4) / 7 is 17. Charging three
         * would let eighteen pairs through and overrun engine[] by two words. */
        for (int n = 17; n <= 18; n++) {
            for (int i = 0; i < n; i++) {
                many[i * 4 + 0] = 0xD011ACB9u;
                many[i * 4 + 1] = 0x0008;
                many[i * 4 + 2] = 0x8011AD00u + (uint32_t)i;
                many[i * 4 + 3] = 0x0001;
            }
            many[n * 4] = 0;
            many[n * 4 + 1] = 0;
            bool got = rompatch_cheats_fit(many, NULL);
            check(got == (n == 17), (n == 17) ? "17 guarded pairs fit, at seven words each"
                                              : "18 do not -- a pair is seven words, not three");
        }

        /* The repeater's price, from both sides. Twelve words whatever the count, which is the
         * whole reason it is a loop: at Datel's three-per-iteration a count of 254 is 762 words
         * and no game on the reference shelf has that much padding. (128 - 4) / 12 = 10. */
        for (int n = 10; n <= 11; n++) {
            for (int i = 0; i < n; i++) {
                many[i * 4 + 0] = 0x5000FE02u;          /* 254 iterations, step 2 */
                many[i * 4 + 1] = 0x0000;
                many[i * 4 + 2] = 0x8111AD00u + (uint32_t)(i * 2);
                many[i * 4 + 3] = 0x0001;
            }
            many[n * 4] = 0;
            many[n * 4 + 1] = 0;
            bool got = rompatch_cheats_fit(many, NULL);
            check(got == (n == 10),
                  (n == 10) ? "10 repeaters fit, at twelve words each whatever the count"
                            : "11 do not, and 254 iterations still costs twelve");
        }

        /* What the pricing function says, line by line, so a change to the emitter that forgets to
         * change the price cannot pass. emit_engine() checks the two against each other at run
         * time and writes nothing when they disagree; this checks the numbers themselves. */
        {
            int eaten = 0, tests = 0;
            check(rompatch_body_words(plain, &eaten) == 3 && eaten == 1, "a plain write is 3 words");
            check(rompatch_body_words(repeater, &eaten) == 12 && eaten == 2,
                  "a flat repeater is 12 words and eats both its lines");
            check(rompatch_body_words(repincr, &eaten) == 13,
                  "one that increments the value as well is 13");
            check(rompatch_body_words(expoff, &eaten) == 4, "disabling the Expansion Pak is 4");
            check(rompatch_atom_words(twotests, &eaten, &tests) == 11 && tests == 2 && eaten == 3,
                  "two tests over one write is 4 + 4 + 3 words in one indivisible atom");
            check(rompatch_atom_words(condrep, &eaten, &tests) == 16 && tests == 1 && eaten == 3,
                  "one test over a repeater is 4 + 12, and the branch has to clear all of it");
        }

        /* Where each conditional branches to when it fails, in words past its delay slot. The
         * numbers, not the formula: this is the one place an error puts the console into somebody
         * else's boot code on every exception, and it is checked against hand-counted blocks.
         *
         *   1 test, 3-word body:  [lui lbu ori bne] [lui ori sh] -- branch at 3, delay 4,
         *                         target 7, so +3. This is the number src/boot/cheats.c uses.
         *   2 tests:              [0..3][4..7][8..10] -- both branch to 11, so +7 and +3.
         *   1 test, 12-word body: [0..3][4..15] -- target 16, so +12. */
        check(rompatch_test_branch(1, 0, 3) == 3, "one test over one write branches +3");
        check(rompatch_test_branch(2, 0, 3) == 7, "the first of two branches +7");
        check(rompatch_test_branch(2, 1, 3) == 3, "and the second +3, to the same word");
        check(rompatch_test_branch(5, 0, 3) == 19, "the first of five branches +19");
        check(rompatch_test_branch(5, 4, 3) == 3, "and the last of five, +3");
        check(rompatch_test_branch(1, 0, 12) == 12, "one test over a repeater loop clears all 12");
        check(rompatch_test_branch(1, 0, 4) == 4, "and over the Expansion Pak special, all 4");
    }

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
