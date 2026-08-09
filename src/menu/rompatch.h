/**
 * @file rompatch.h
 * @brief Install the cheat engine's hook by editing the cartridge image, not by emitting code.
 * @ingroup menu
 *
 * ## How a cheat runs on this console
 *
 * Not the way the Datel engine does it. That one arms the VR4300's watch exception on the game's
 * exception vector, and the M64's CPU does not implement watch -- measured, with a positive
 * control, in AUDIT 1af. Nor by staging an engine in RDRAM before the handoff: this console does
 * not carry RDRAM across it, and the IPL3 DMEM patch that would install a hook does not take
 * either. Eight hardware rounds went into placements that all failed for one of those two reasons
 * (AUDIT 2n through 2x).
 *
 * What works is to stop making the engine arrive at run time. IPL3 copies `ROM[0x1000, 0x101000)`
 * into RDRAM at the entry address before the game's first instruction, so an engine written into
 * that window is installed by the console itself, from the cartridge, by the same DMA that loads
 * the game. Nothing has to survive anything.
 *
 * Three edits, all inside that window, all made to cartridge SDRAM at launch while there is still
 * a log to write to and a read-back to check against:
 *
 *   1. the engine into runs of inter-function alignment padding in the game's own boot code;
 *   2. the two words at the top of `__osExceptionPreamble` repointed at its RDRAM address, so
 *      `osInitialize` copies our hook onto the exception vectors for us;
 *   3. CRC1/CRC2 recomputed, because every retail IPL3 checksums that window and a stale header
 *      is a console that stops dead before the logo. See romcrc.h.
 *
 * ## The two rules that make it safe
 *
 * **Recompute the unmodified image's checksum first, and if it does not match the header, patch
 * nothing.** A ROM this code does not fully understand is a ROM it must not touch. Measured: 23 of
 * 24 on the reference card pass, the exception being a Pokemon Stadium whose header disagreed with
 * its own contents before anyone touched it.
 *
 * **A run of zeros is not automatically padding.** Zero *fields* inside a pointer table look
 * identical from the outside, and an engine written into one black-screens the game. See
 * rompatch_run_is_padding(), which is the single decision this whole file turns on.
 */

#ifndef ROMPATCH_H__
#define ROMPATCH_H__

#include <stdbool.h>
#include <stdint.h>

#include "boot/cic.h"
#include "romcrc.h"

/** @brief How many words of engine the cartridge will hold. Three per plain write, seven per
 *  conditional and the write it guards, four for the tail. */
#define ENGINE_MAX_WORDS 128

/** @brief What the scan found, and what was done about it. Every field ends up in launch.log. */
typedef struct {
    bool     attempted;     /**< The menu tried; false means cheats were off or there is no cart */
    bool     crc_ok;        /**< The unmodified image agreed with its own header (the gate) */
    bool     found;         /**< A preamble that passes every test was located */
    bool     written;       /**< The patch and the new checksum are in the cartridge */
    bool     verified;      /**< ...and read back as what we wrote */
    bool     reverified;    /**< ...and the whole image agrees with its new checksum */

    uint32_t entry;         /**< Where IPL3 will load the image, CIC shift applied */
    uint32_t rom_offset;    /**< Byte offset of the preamble in the cartridge */
    uint32_t ram_address;   /**< Where that lands in RDRAM */
    uint32_t target;        /**< The `__osException` address the preamble computes */
    uint32_t word0, word1;  /**< The two words as found, which the engine tail must replay */
    int      candidates;    /**< Preamble-shaped runs seen; more than one is worth knowing */
    int      rejected;      /**< ...of which this many failed the address test */

    uint32_t old_crc1, old_crc2;
    uint32_t new_crc1, new_crc2;

    uint32_t engine_rom_off;    /**< ROM offset of the padding run the engine starts in */
    uint32_t engine_ram;        /**< ...and where IPL3's load will put it in RDRAM */
    uint32_t engine_run_words;  /**< ...and how many words of engine were written in total */
    int      engine_segments;   /**< ...across this many padding runs, chained by `j` */
    int      engine_codes;      /**< ...carrying this many cheat lines */

    /** Exactly what was written over the preamble, so the log carries the instructions
     *  themselves. Hand-decodable from a photograph, which is how the inline stub's encoding
     *  gets checked on a console with no debugger. */
    uint32_t patch[2];
    int      patch_words;
} rompatch_result_t;

/**
 * @brief Locate the preamble in an image, without writing anything.
 *
 * Split out from the install so it can be run on the host against real ROM files, which is the
 * only way to exercise the part of this that decides which two words of somebody's game get
 * rewritten. tools/hosttest/test_rompatch.c plants both a real preamble and the exact bogus one
 * that Conker's Bad Fur Day carries.
 *
 * @param read     Reader over the image; see romcrc.h for the endianness contract.
 * @param entry    Where IPL3 will load it, CIC shift already applied.
 * @param ram_top  One past the last valid RDRAM address, for the target test.
 * @param out      Zeroed by the caller; `found` and the site fields are filled in.
 */
bool rompatch_find (romcrc_read_t read, void *ctx, uint32_t entry, uint32_t ram_top,
                    rompatch_result_t *out);

/** @brief The entry address IPL3 will actually use: two CICs load below the header's. */
uint32_t rompatch_entry_for (cic_type_t cic_type, uint32_t header_entry);

/**
 * @brief Put the cheat engine in the game's own boot segment and aim the preamble at it.
 *
 * The design that works, confirmed on an M64 (AUDIT 2y). Three edits, all inside the checksum
 * window: the engine goes into runs of inter-function alignment padding, the preamble's first two
 * words are repointed at its RDRAM address, and CRC1/CRC2 are recomputed over the result. IPL3's
 * own load then places the engine in RDRAM alongside the game, so nothing has to survive the
 * handoff, nothing executes over the PI, and no IPL3 or DMEM patch is involved -- which is also
 * why this path needs no 6105 nop: the header genuinely describes the image.
 *
 * Three instructions per cheat plus a four-word tail that replays the preamble's own words and
 * `jr $k0`, entering `__osException` with the register state it expects. The engine may be spread
 * over up to four padding runs, chained with `j`, which is what makes multi-line groups fit.
 *
 * Refuses -- leaving the cartridge byte for byte as it was -- if the checksum does not already
 * describe the image, if no preamble is found, if the selection does not fit, if the image has no
 * qualifying padding, or if any read-back disagrees. Nothing is written until placement has
 * succeeded, so there is no state in which a patched preamble points at a half-written engine.
 *
 * @param cheat_list `{address, value}` pairs terminated by `{0, 0}`, as cheatdb_emit() leaves
 *                   them. Pass it through rompatch_cheats_fit() first.
 */
bool rompatch_install_engine (cic_type_t cic_type, uint32_t header_entry,
                              const uint32_t *cheat_list, rompatch_result_t *out);

/**
 * @brief Is a run of zeros in the boot segment alignment padding, or zeros that mean something?
 *
 * Exposed because it is the single decision that separates a game that boots from a black screen,
 * and because the scan around it reads the cartridge over the PI and so cannot run on the host.
 * tools/hosttest/test_rompatch.c pins it against the words measured either side of the real runs
 * -- the ones an engine has been booted out of, and the ones that killed it.
 *
 * @param before2 The word two back from the run, @param before1 the word one back.
 * @param run_bytes Length of the run of zero words.
 */
bool rompatch_run_is_padding (uint32_t before2, uint32_t before1, uint32_t run_bytes);

/**
 * @brief Can this selection be carried at all, and how many lines is it?
 *
 * Carried: unconditional 8/16-bit writes (0x80/0x81/0xA0/0xA1) and the `D0`-`D3` conditionals,
 * each guarding the single write that follows it. Refused: repeaters (`0x50`, which emit three
 * instructions per iteration and up to 255 iterations), boot-time writes (`0xF0`/`0xF1`) and
 * every GS-button variant, since there is no button for this engine to read.
 *
 * A selection carrying any of those is refused **whole** rather than filtered, because a `D0` and
 * the write it guards are one indivisible thing and dropping half of a group is a bug this
 * project has already shipped once (AUDIT 2.2). Measured over the shipped database: 42,220 of
 * 42,898 groups are carried, up from 40,764 before conditionals.
 *
 * Answers before anything touches the cartridge, so the launch screen can say so.
 */
bool rompatch_cheats_fit (const uint32_t *cheat_list, int *lines_out);

#endif /* ROMPATCH_H__ */
