/**
 * @file test_cheatinstall.c
 * @brief Answer "would the cheat engine actually install for this ROM?" without a console.
 *
 * The cheat engine hooks itself by overwriting one instruction in the game's IPL3, and it locates
 * that instruction from a per-CIC table of word offsets in `cheats_patch_ipl3()`. Before writing,
 * it checks that the word it is about to clobber really is `jr $t1` -- because a libdragon IPL3
 * can be brute-force signed with any retail CIC seed, so the CIC type alone does not prove the
 * layout. If that check fails the engine is never hooked and cheats do nothing.
 *
 * On hardware none of this is observable. It runs inside `boot()`, after the filesystem and the
 * display are torn down, microseconds before jumping into the game -- there is nothing left to
 * print to, and this cart's USB does not work. But the check depends on nothing except the ROM's
 * own bytes, so it can be answered exactly, here, from a file.
 *
 * The real cic.c is compiled in rather than reimplemented. A Python transcription of
 * `cic_calculate_ipl3_checksum` would be a second implementation to keep in step, and the one
 * thing this tool must never do is disagree with the console about which CIC a ROM has.
 *
 *     cc -std=c11 -Isrc -Isrc/boot tools/hosttest/test_cheatinstall.c src/boot/cic.c \
 *        -o build/hosttest/test_cheatinstall && build/hosttest/test_cheatinstall ROM...
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/cic.h"

/** IPL3 occupies words 16..1023 of the ROM; boot.c copies exactly that range into DMEM, index
 *  for index, so a DMEM word offset is also a ROM word offset. */
#define IPL3_BYTE_OFF   0x40
#define IPL3_BYTES      0xFC0
#define ROM_WORDS       1024

/** The same table as cheats_patch_ipl3(). Duplicated deliberately: that function is static, and
 *  copying six numbers is better than making the boot path non-static for a test. If it ever
 *  changes, this file has to change with it -- which is what the mismatch check below is for. */
static int patch_offset_for (cic_type_t t) {
    switch (t) {
        case CIC_5101: return 476;
        case CIC_6101:
        case CIC_7102: return 466;
        case CIC_x102: return 475;
        case CIC_x103: return 472;
        case CIC_x105: return 499;
        case CIC_x106: return 488;
        default:       return -1;
    }
}

static const char *cic_name (cic_type_t t) {
    switch (t) {
        case CIC_5101: return "5101";
        case CIC_6101: return "6101";
        case CIC_7102: return "7102";
        case CIC_x102: return "6102/7101";
        case CIC_x103: return "6103/7103";
        case CIC_x105: return "6105/7105";
        case CIC_x106: return "6106/7106";
        case CIC_5167: return "5167";
        case CIC_8301: return "8301";
        case CIC_8302: return "8302";
        case CIC_8303: return "8303";
        case CIC_8401: return "8401";
        case CIC_8501: return "8501";
        default:       return "UNKNOWN";
    }
}

static uint32_t be32 (const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int main (int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s ROM...\n", argv[0]);
        return 2;
    }

    /* Encoded by hand, NOT via vr4300_asm.h's I_JR().
     *
     * Those macros build the word through a bitfield union, and C does not specify how bitfields
     * are packed -- on this x86 host I_JR(REG_T1) evaluates to 0x20000240, which is not a MIPS
     * instruction at all. Compiled for mips64-elf it is correct; compiled here it is nonsense,
     * and the first run of this tool duly reported that both ROMs would fail. The printed
     * constant below is the control that caught it: jr $t1 is SPECIAL | rs<<21 | JR, and rs for
     * $t1 is 9, so it can only be 0x01200008. */
    const uint32_t JR_T1 = (9u << 21) | 8u;
    printf("jr $t1 = %08x (hand-encoded; the bitfield macros do not pack portably)\n\n", JR_T1);

    int bad = 0;
    for (int a = 1; a < argc; a++) {
        uint8_t head[ROM_WORDS * 4];
        FILE *f = fopen(argv[a], "rb");
        if (f == NULL) {
            printf("%-44s  CANNOT OPEN\n", argv[a]);
            bad++;
            continue;
        }
        size_t got = fread(head, 1, sizeof(head), f);
        fclose(f);
        if (got < sizeof(head)) {
            printf("%-44s  SHORT (%zu bytes)\n", argv[a], got);
            bad++;
            continue;
        }

        /* Only big-endian .z64 is handled. A byte-swapped ROM would checksum to nothing and
         * report UNKNOWN, which would look like a CIC problem rather than a format one. */
        uint32_t magic = be32(head);
        const char *slash = strrchr(argv[a], '/');
        const char *name = slash ? slash + 1 : argv[a];

        if (magic != 0x80371240) {
            printf("%-44s  not big-endian z64 (magic %08x)\n", name, magic);
            bad++;
            continue;
        }

        cic_type_t cic = cic_detect(head + IPL3_BYTE_OFF);
        int off = patch_offset_for(cic);

        printf("%-44s  CIC %-9s ", name, cic_name(cic));
        if (off < 0) {
            printf("no patch offset -> cheats correctly SKIPPED\n");
            continue;
        }

        uint32_t word = be32(head + off * 4);
        /* x106's IPL3 is partially scrambled and cheats_patch_ipl3 un-scrambles before comparing.
         * Not reproduced here: none of these ROMs is x106, and a wrong answer is worse than an
         * absent one. Flagged rather than guessed. */
        if (cic == CIC_x106) {
            printf("word[%d]=%08x  x106 is scrambled -- NOT CHECKED\n", off, word);
            continue;
        }

        printf("word[%d]=%08x  %s\n", off, word,
               (word == JR_T1) ? "== jr $t1 -> engine WOULD hook"
                               : "!= jr $t1 -> engine would NOT hook, and says nothing");
        if (word != JR_T1) {
            bad++;
        }
    }

    printf("\n%d of %d ROMs would fail to hook the cheat engine\n", bad, argc - 1);
    return 0;
}
