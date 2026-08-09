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

/** @brief How many words of engine the cartridge will hold, before its padding is even looked at.
 *
 *  Costed by rompatch_body_words() plus four per conditional in front of the line it guards, plus
 *  four for the tail. Raising it buys almost nothing: over the keyed corpus, 128 carries 157,795
 *  groups and 256 carries 157,872 -- 77 more out of 177,579. What actually decides is how much dead
 *  padding the game has, and that is a per-ROM number this constant knows nothing about. Measured
 *  over the fifteen-ROM shelf: Star Wars 15 words, Banjo-Kazooie 24, Ocarina 38, Donkey Kong 64
 *  141, Mario Party 1,128. So on most games place_engine() is the binding constraint and this is
 *  slack; the cap is here to bound the stack arrays, not to express a policy. */
#define ENGINE_MAX_WORDS 128

/** @brief What one line costs, where it is not simply three instructions.
 *
 *  A repeater is a loop rather than `count` copies of the write. Datel's own engine unrolls -- 3
 *  words per iteration, and the corpus has counts up to 254, so 762 words for one cheat. As a loop
 *  it is a fixed twelve, thirteen when the value increments as well as the address. That is the
 *  whole reason repeaters are expressible at all: no game on the reference shelf has 762 words of
 *  padding, and half have under 45. */
#define REPEAT_WORDS_FLAT 12
#define REPEAT_WORDS_INCR 13

/** @brief Disabling the Expansion Pak is two stores through one base, plus the constant. */
#define EXPANSION_OFF_WORDS 4

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
 * @brief Do these four words begin libultra's `__osException`?
 *
 * What a preamble candidate's target is checked against, and the reason a preamble is now
 * identified by where it points rather than by how far. Exposed so the host suite can pin it
 * against the four words measured at the target of every real candidate on the reference shelf --
 * and against the dispatcher stub in Mario Party 3 that sits at the +16 the old rule trusted.
 */
bool rompatch_is_exception (const uint32_t *words);

/**
 * @brief What the one line at the head of @p list costs, and how many list entries it eats.
 *
 * The body of an atom: the line a run of conditionals guards, or a line standing on its own.
 * Everything the engine can express is here and nowhere else, so the fit rule, the emitter and
 * tools/rompatch.py cannot drift apart on what is supported.
 *
 * | line                        | words | why |
 * |-----------------------------|-------|-----|
 * | `80/81/A0/A1` write         | 3     | lui, ori, sb/sh |
 * | `F0/F1` boot write          | 3     | the same three; see below |
 * | `50` repeater + its write   | 12/13 | a loop, not `count` copies |
 * | `EE` disable Expansion Pak  | 4     | two stores of 4 MB over osMemSize |
 * | anything else               | 0     | cannot be expressed |
 *
 * `F0`/`F1` are Datel's write-once-at-boot, and this engine has no boot pass -- it runs on every
 * exception. Emitting them as ordinary writes is a deliberate difference and it is safe because of
 * what they are: 448 of the corpus's 726 boot writes store `0x2400`, the top half of
 * `addiu $zero, $zero, x`, and 541 of them are word-aligned. They are instruction patches turning a
 * check into a no-op, so writing the same constant over the same dead instruction repeatedly is
 * idempotent. It also makes the twenty groups that mix a boot write with real cheats work.
 *
 * @param list  positioned at the line in question; may point at the `{0,0}` terminator
 * @param lines receives how many cheat lines were consumed -- 1, or 2 for a repeater
 * @return the word count, or 0 when this engine cannot express the line
 */
uint32_t rompatch_body_words (const uint32_t *list, int *lines);

/**
 * @brief What one indivisible block costs: a run of conditionals plus the body they guard.
 *
 * Four words per test in front of rompatch_body_words(). Both callers go through this rather than
 * counting the conditionals themselves, because the two of them getting different answers about
 * where a run ends is a branch into somebody else's boot code.
 *
 * A 16-bit access to an odd address is refused here as well, and it is the one refusal that is
 * about the console rather than the cheat: `sh`/`lhu` off an odd address takes an Address Error at
 * the exception vector with EXL set, which vectors straight back into this engine and locks the
 * machine. 1,964 of the corpus's 149,687 16-bit writes name one.
 *
 * @param lines     receives the cheat lines consumed, always at least 1 so a caller can advance
 * @param tests_out receives how many conditionals were in front
 * @return the word count, or 0 when the block cannot be expressed
 */
uint32_t rompatch_atom_words (const uint32_t *list, int *lines, int *tests_out);

/**
 * @brief Where conditional @p k of @p tests, guarding a @p body-word body, branches to on failure.
 *
 * In words past the branch's delay slot, which is what a MIPS branch immediate counts. Out here
 * rather than inline in emit_engine() for one reason: it is the most dangerous number in this
 * file. Off by one and the branch lands on the store it was meant to skip; off by more and it
 * lands in whatever the game keeps after the padding, executed on every exception forever. The
 * host suite can pin an arithmetic function and cannot pin one that needs a cartridge.
 */
int rompatch_test_branch (int tests, int k, uint32_t body);

/**
 * @brief Can this selection be carried at all, and how many lines is it?
 *
 * Carried: everything rompatch_body_words() prices, with any number of `D0`-`D3` conditionals
 * stacked in front of the line they guard at four words each.
 *
 * Refused: every GS-button variant (`0x88`, `0x89`, `0xA8`, `0xA9`, `0xD8`-`0xDB`), because the
 * console has no GameShark button and nothing in RDRAM stands in for one; a conditional or repeater
 * with nothing after it to guard; and the specials that Datel's own engine accepts and then emits
 * nothing for (`0xCC`, `0xDE`, `0xFF`).
 *
 * A selection carrying any of those is refused **whole** rather than filtered, because a `D0` and
 * the write it guards are one indivisible thing and dropping half of a group is a bug this
 * project has already shipped once (AUDIT 2.2).
 *
 * Answers before anything touches the cartridge, so the launch screen can say so. It does not
 * answer whether the *game* has room -- that is place_engine(), and on most ROMs it is the tighter
 * of the two. See ENGINE_MAX_WORDS.
 */
bool rompatch_cheats_fit (const uint32_t *cheat_list, int *lines_out);

#endif /* ROMPATCH_H__ */
