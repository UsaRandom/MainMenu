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

/* ------------------------------------------------------------------ the handoff flash
 *
 * The bar above answers "did the ENGINE run". It came back no on the console (AUDIT 1ao) with the
 * self-test saying PAINTED, which is a real fact and a dead end: the engine not running is
 * consistent with the patcher never executing, with the scan missing, with the scan hitting and
 * the game ignoring the bytes we rewrote, and with the engine being overwritten. Four stories, one
 * symptom, and no way to tell them apart from inside the game.
 *
 * So the patcher reports on ITSELF, before it hands over. It has one advantage the engine does not:
 * it owns the machine. Nothing else is running, nothing will touch the video interface until the
 * game's own osViSetMode, and there are megabytes of RDRAM nobody has any use for.
 *
 * TWO channels, because the first attempt had one and it came back empty in a way that could not
 * be read. That version painted 320x240 and programmed all thirteen VI timing registers from
 * scratch, because boot.c blanks the display on its way past (VI->H_LIMITS = 0). The console said
 * "no flash", which is what a patcher that never ran looks like AND what an HDMI sink that never
 * re-acquired sync looks like. A one-channel instrument whose silence has two readings is not an
 * instrument.
 *
 *   1. TIME. The patcher holds for BEACON_FLASH_HOLD_TICKS before jumping into the game, and the
 *      miss path holds that long a second time first. So the wall clock between the menu
 *      going away and the game's first frame says which branch ran, and it depends on nothing but
 *      the CPU clock -- no video, no memory the game cares about, no cooperation from anything:
 *
 *          ~2 s    the patcher never executed
 *          ~10 s   the preamble scan found libultra's handler and rewrote it
 *          ~20 s   the scan came up empty and the Datel watch was armed instead
 *
 *   2. COLOUR, for the same three cases: no flash, green, red. This no longer programs the VI at
 *      all. boot.c leaves the display alone while the flash is armed, so the menu's own 640x480
 *      mode is still running and still locked, and the patcher only has to point VI_ORIGIN at a
 *      frame it painted. Nothing resyncs; the picture just changes.
 *
 * And across the middle of the picture, twenty-four blocks: bit 23 down to bit 0 of
 * (match - game entry), white for one and black for zero, read left to right as six hex digits.
 * On a hit that is where in the loaded megabyte the preamble was; tools/preamblescan.py predicts
 * 0x0021f0 for Ocarina of Time. On a miss the scan pointer has run the whole window, so it reads
 * 0x100000 -- a full megabyte, which is the number that means "looked everywhere".
 *
 * The cost is ten to twenty seconds before every cheat launch, which is why this rides on the same
 * `[menu] cheat_beacon` switch as the bar and is off by default. */
#define BEACON_FLASH_PHYS       (0x00600000)    /* physical: dead menu heap by the time this runs */

/* The menu's own geometry, because the flash inherits the menu's live VI mode rather than setting
 * one. 640 x 480 x 2 = 614,400 bytes, ending at 0x00696000 -- clear of the patcher at 0x00700000.
 * If the menu's resolution ever changes, these change with it or the picture is skewed. */
#define BEACON_FLASH_W          640
#define BEACON_FLASH_H          480
#define BEACON_FLASH_BYTES      (BEACON_FLASH_W * BEACON_FLASH_H * 2)

#define BEACON_FLASH_BITS       24              /* of (match - entry), MSB first */
#define BEACON_FLASH_BIT_ROW    200             /* top row of the block band */
#define BEACON_FLASH_BIT_H      80              /* how many rows the band is copied down */
/* The blocks are painted with word stores, so the margin, the width and above all the pitch are
 * counted in pairs of pixels. A 13-pixel pitch advanced the pointer by 26 bytes and the second
 * block's first store took a misaligned-write exception -- ares caught it, which is the one place
 * emitted code of this kind can still be caught. cheats.c asserts all of it at compile time now.
 * 32 + 24 * 24 = 608, so the band is centred in 640 with an 8-pixel gap between blocks. */
#define BEACON_FLASH_BIT_X      32              /* left margin, pixels */
#define BEACON_FLASH_BIT_W      16              /* block width, pixels */
#define BEACON_FLASH_BIT_PITCH  24              /* block start to block start, pixels */

/* COP0 Count runs at half the 93.75 MHz core clock. Ten seconds is chosen to be unmistakable
 * against a normal handoff, which is a second or two: nobody has to time it with anything, and
 * "the same as always" versus "ages" versus "twice that again" is a reading that survives being
 * done by eye. */
#define BEACON_FLASH_HOLD_TICKS  (46875000u * 10u)

/**
 * @brief Is the handoff flash armed? boot.c asks, because it must not blank the display if so.
 *
 * The flash inherits whatever VI mode is running rather than programming one, so boot.c's
 * `VI->H_LIMITS = 0` would leave it painting into a display nobody can see. Skipping that one
 * write is the whole accommodation; the game reprograms the VI from scratch regardless.
 */
bool cheats_flash_armed (void);

/**
 * @brief Make the patcher flash a full screen at handoff, saying which way the scan went.
 *
 * @param enabled     Emit it at all.
 * @param fb_phys     Physical address of the framebuffer to paint, or 0 for #BEACON_FLASH_PHYS.
 *                    The dev harness overrides it; nothing else should.
 * @param hold_ticks  COP0 Count ticks to hold the screen, or 0 for #BEACON_FLASH_HOLD_TICKS. The
 *                    miss path waits this long twice, which is the timing channel.
 *
 * Call before cheats_install(), like cheats_set_beacon().
 */
void cheats_set_flash (bool enabled, uint32_t fb_phys, uint32_t hold_ticks);

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
 * @brief The hook is already in the cartridge, so do not scan for it or arm the watch.
 *
 * src/menu/rompatch.c rewrites the game's __osExceptionPreamble in the ROM image while the menu
 * is still up, where the change can be logged and read back. When it succeeds it hands the two
 * original words here, because the engine's tail replays them to reach the game's real
 * __osException -- and with them known in advance the patcher stops being anything more than a
 * memcpy: no scan, no branch, no watch.
 *
 * Call before cheats_install(). Passing false restores the runtime scan.
 *
 * @param word0 The `lui $k0, %hi(__osException)` found in the cartridge.
 * @param word1 The `addiu $k0, $k0, %lo(__osException)` that followed it.
 */
void cheats_set_rom_hook (bool enabled, uint32_t word0, uint32_t word1);

/** @brief Where the engine ends up: the default below, unless a 0xDE code moves it. */
#define CHEATS_DEFAULT_ENGINE_ADDRESS (0x807C5C00)

/**
 * @brief Where @p cheat_list's engine will live, so the ROM hook can be aimed at it.
 *
 * The menu has to write the hook before boot() runs, and a 0xFF "set store location" code can
 * move the engine, so the address cannot simply be assumed to be the default.
 */
uint32_t cheats_engine_address (uint32_t *cheat_list);

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
