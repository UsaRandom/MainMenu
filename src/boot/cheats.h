/**
 * @file cheats.h
 * @brief Header file for cheat installation functions.
 * @ingroup boot
 */

#ifndef CHEATS_H__
#define CHEATS_H__

#include <stdbool.h>
#include <stdint.h>
#include "cic.h"

/**
 * @brief Word offset into the IPL3 that the engine hooks itself into, or -1 if unsupported.
 *
 * Exposed so the menu can answer "will cheats actually work for this game?" while the filesystem
 * and the display still exist. The install itself runs inside boot(), after everything has been
 * torn down and microseconds before jumping into the game, where there is nothing left to report
 * a failure to. See src/menu/cheatcheck.h.
 */
int cheats_ipl3_patch_offset (cic_type_t cic_type);

/**
 * @brief Is @p word_at_offset the instruction the engine expects to overwrite?
 *
 * @p word_at_offset is the IPL3 word at #cheats_ipl3_patch_offset. Handles the x106 descrambling.
 * False means the engine cannot hook this ROM and cheats will not run.
 */
bool cheats_ipl3_layout_ok (cic_type_t cic_type, uint32_t word_at_offset);

/* ------------------------------------------------------------------ the beacon
 *
 * Everything from here to the game's first frame happens after the menu is gone: no display, no
 * filesystem, and on this cart no USB either. AUDIT 1ag is what that costs -- the handler hook
 * went green end to end under ares on executed emitted code, did nothing on the console, and
 * left four candidate explanations that could not be told apart. A day, and no fact.
 *
 * So the engine reports on itself, through the one output device that is guaranteed to exist and
 * guaranteed to be pointed at something: the video interface. VI_ORIGIN always holds the address
 * of whatever the GAME is currently displaying, so the engine reads it and writes a bar of solid
 * colour into the top of that framebuffer. Ten instructions plus a run of stores, no knowledge of
 * the game, no channel, no logs. If the engine executes inside Ocarina of Time, a coloured bar
 * appears over Ocarina of Time. If it does not, there is no bar. That is the whole question, and
 * it is answered in one launch.
 *
 * The colour says which way the patcher went, because the patcher writes it and the engine only
 * displays it:
 *
 *   green   the preamble scan found libultra's handler and the game installed our hook
 *   red     the scan missed and the Datel watch hook was armed instead -- so a red bar means the
 *           watch DID fire, which on this console would itself be news
 *   none    the engine never executed -- but only trust that once the launch log's beacon
 *           self-test says PAINTED, which is the positive control for the instrument itself
 *
 * Off unless `[menu] cheat_beacon = true` is in config.ini on the card. It draws over the game,
 * which is the point, and nobody should meet it by accident.
 */
#define VI_ORIGIN_ADDRESS   (0xA4400004)


/* Where in the framebuffer the bar goes, and how much of it there is.
 *
 * The first version put 1 KB at offset zero and the console reported no bar. That result was
 * worthless, because 1 KB at offset zero is **0.8 to 1.6 pixel rows at the very top of the
 * buffer** -- 320x240 at 16 bpp has 640-byte rows, 640x480 has 1,280 -- and the top rows of an N64
 * framebuffer are exactly what overscan eats. A bar nobody can see and a bar that was never drawn
 * look identical, which is the one thing an instrument may not do.
 *
 * 64,000 bytes in lands in the middle of every geometry a game plausibly uses: row 100 of 240 at
 * 320x240x16, row 50 of 480 at 640x480x16, row 50 of 240 at 320x240x32. 8 KB covers 12 rows of the
 * first, 6 of the second. Nothing that size in the middle of the screen is subtle.
 *
 * The cost is real and is accepted: 2,048 uncached stores run on every exception the engine sees.
 * If that visibly slows the game down, that is another signal and not a problem. */
#define BEACON_OFFSET_BYTES 64000
#define BEACON_WORDS        2048
#define BEACON_GREEN        0x07C107C1u     /* RGBA5551 (0,31,0) twice */
#define BEACON_RED          0xF801F801u     /* RGBA5551 (31,0,0) twice */

/* VI_ORIGIN below 64 KB is not a framebuffer -- it is VI_ORIGIN before the game set it, and
 * writing there would land in the exception vectors or in the low memory a 0x20 code clears. One
 * `srl` by this much and a branch on zero rejects both that and an origin of zero.
 *
 * It was a one-megabyte floor first, which is wrong in the direction that matters. hooktest's
 * beacon scenario refused to run and said why: this whole ROM is 824 KB, so its own .bss sits
 * below the line -- and a small game's framebuffer can sit there too. A floor that suppresses the
 * beacon is worse than no floor at all, because a bar that never appears is exactly how "the
 * engine never ran" looks. 64 KB clears every real framebuffer and still catches an unset one. */
#define BEACON_MIN_ORIGIN_SHIFT  16

/**
 * @brief Make the engine paint a bar over the running game, so we can see whether it ran.
 *
 * A diagnostic, off by default, turned on with `[menu] cheat_beacon = true` in config.ini. See
 * the long comment at the top of cheats.c for what the colours mean and why the video interface
 * is the only reporting channel available past this point.
 *
 * Call before cheats_install(); it changes what gets emitted, not what happens afterwards.
 */
void cheats_set_beacon (bool enabled);

/**
 * @brief Assemble the patcher and engine into RDRAM without touching the IPL3.
 *
 * Everything cheats_install() does except the one-word IPL3 patch that arms it. Split out so the
 * dev harness can execute the emitted patcher against a synthetic game image and prove the
 * preamble hook end-to-end under ares -- the IPL3 patch is the single step that cannot happen
 * inside the menu, because the menu's own IPL3 is libdragon's and the layout check refuses it.
 *
 * @param cheat_list The cheat list, terminated by a zero pair.
 * @return The patcher's entry point (jump here with $t1 holding the game entry point), or NULL
 *         if the list was NULL or would overflow the engine or patcher regions.
 */
uint32_t *cheats_emit(uint32_t *cheat_list);

/**
 * @brief Installs cheats based on the CIC type.
 *
 * This function installs the cheats provided in the cheat list based on the
 * specified CIC type.
 *
 * @param cic_type The type of CIC (Copy Protection Chip) used.
 * @param cheat_list A pointer to an array of cheats to be installed.
 * @return true if the cheats were successfully installed, false otherwise.
 */
bool cheats_install(cic_type_t cic_type, uint32_t *cheat_list);

#endif // CHEATS_H__
