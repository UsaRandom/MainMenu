/**
 * @file romcrc.h
 * @brief The checksum IPL3 computes over the first megabyte of a game, and why we need it.
 * @ingroup menu
 *
 * Every retail IPL3 checksums `ROM[0x1000 .. 0x101000]` after loading it and compares the answer
 * against CRC1 and CRC2 in the header at 0x10 and 0x14. Disagree and the console stops dead --
 * black screen, no logo, no sound. So a menu that wants to change two words of a game's code
 * before booting it has to recompute both, or the game will not run at all.
 *
 * That is what this is for. AUDIT 1as: the cheat hook has spent this whole investigation being
 * installed by code emitted into RDRAM and executed microseconds before the handoff, where
 * nothing can be observed and four different failures look identical. Patching the cartridge
 * image instead moves the whole operation into the menu, where there is a filesystem, a log and a
 * screen -- and where the patch can be read back and checked before anything boots.
 *
 * ## The algorithm is not ours and cannot be guessed
 *
 * Six accumulators over 262,144 words, seeded by the CIC, with three different final mixes. It is
 * the bootcode's, it has no specification, and a plausible-looking implementation that is subtly
 * wrong produces a ROM that does not boot. So it is checked two ways:
 *
 *   - tools/hosttest/test_romcrc.c pins this C against tools/romcrc.py over a synthetic image, so
 *     the two implementations cannot drift apart quietly.
 *   - At runtime, before any patch, romcrc_verify() computes the checksum of the UNMODIFIED image
 *     and compares it with the header. Disagreement means this code does not understand this ROM,
 *     and the correct response is to leave the ROM alone rather than to patch it and hope.
 *
 * The second is what makes it safe to ship. Measured on the 24 N64 ROMs on the reference card:
 * 23 reproduce their stored CRC exactly, across CIC 6101, 6102, 6103 and 6105. The twenty-fourth
 * is Pokemon Stadium, whose CRC1 matches and whose CRC2 does not -- a header that disagreed with
 * its own contents before we touched it. Under the rule above it simply does not get patched.
 */

#ifndef ROMCRC_H__
#define ROMCRC_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot/cic.h"

/** @brief Where the checksummed window starts, and how long it is. IPL3 loads exactly this. */
#define ROMCRC_START  (0x1000)
#define ROMCRC_LENGTH (0x100000)

/**
 * @brief Read @p len bytes at @p offset of the ROM into @p dst. False if it could not.
 *
 * A callback rather than a pointer because the two callers read from completely different places:
 * the menu DMAs from cartridge SDRAM over the PI, and the host test freads from a file. Both feed
 * the same accumulator loop, which is the point -- there is only one copy of the arithmetic.
 *
 * Offsets are always 4-byte aligned and lengths always a multiple of 4. @p dst is filled with
 * NATIVE-ENDIAN 32-bit words: a cartridge read on this big-endian target is a straight copy, and
 * the host test byte-swaps as it reads. The arithmetic below is defined on the values the console
 * sees, so getting this wrong on the host would make the two implementations disagree about a ROM
 * neither of them had misread.
 */
typedef bool (*romcrc_read_t) (void *ctx, uint32_t offset, void *dst, size_t len);

/**
 * @brief Compute CRC1 and CRC2 the way @p cic_type's bootcode will.
 *
 * @param read     How to get at the ROM. Called with offsets in [0, 0x101000).
 * @param ctx      Passed through to @p read.
 * @param cic_type Which bootcode. Only 6101/6102/7102, 6103, 6105 and 6106 have known seeds;
 *                 anything else returns false rather than guessing.
 * @param crc1     Out: the word belonging at ROM offset 0x10.
 * @param crc2     Out: the word belonging at ROM offset 0x14.
 * @return false if the CIC is not one we know, or if any read failed.
 */
bool romcrc_compute (romcrc_read_t read, void *ctx, cic_type_t cic_type,
                     uint32_t *crc1, uint32_t *crc2);

/** Word index into the 6105 bootcode window that boot/cheats.c overwrites. See below. */
#define ROMCRC_X105_NOP_WORD ((0x798 - 0x750) / 4)

/**
 * @brief The same, but computed the way IPL3 will see it after cheats have patched the bootcode.
 *
 * CIC 6105 is the odd one: its checksum mixes in 256 bytes of the bootcode at offset 0x750 rather
 * than the running accumulator. At runtime IPL3 reads that window out of its own copy in RSP
 * DMEM -- and `cheats_patch_ipl3()` writes a nop into DMEM word 486, which is byte 0x798, which is
 * **inside that window**. So on a 6105 game with cheats installed, the console computes a
 * different checksum from the one this file computes off the cartridge, and the difference is
 * exactly one word.
 *
 * That does not matter while nobody rewrites the game, because the check is either disabled by
 * that same nop or passes against the header it was shipped with. It matters enormously the
 * moment the header is recomputed, which is the first thing rompatch.c does.
 *
 * @p ipl3_nop_486 replays the nop, so the answer is what the console will get rather than what
 * the file says. It is a no-op for every CIC but 6105.
 */
bool romcrc_compute_ex (romcrc_read_t read, void *ctx, cic_type_t cic_type, bool ipl3_nop_486,
                        uint32_t *crc1, uint32_t *crc2);

/**
 * @brief Does the ROM's stored checksum match what its own bootcode will compute?
 *
 * The positive control for everything else in this file. Run it before patching: a mismatch means
 * either this code does not understand this ROM or the ROM was already broken, and in both cases
 * the honest move is to change nothing.
 *
 * @param stored1 Out, optional: the CRC1 read from the header.
 * @param stored2 Out, optional: the CRC2 read from the header.
 */
bool romcrc_verify (romcrc_read_t read, void *ctx, cic_type_t cic_type,
                    uint32_t *stored1, uint32_t *stored2);

#endif /* ROMCRC_H__ */
