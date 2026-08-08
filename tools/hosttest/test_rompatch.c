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

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
