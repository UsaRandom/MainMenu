/**
 * @file enginetest.h
 * @brief Does the Datel cheat engine actually run on this console?
 * @ingroup menu
 *
 * ## The question nothing else can answer
 *
 * Everything the menu can check about a cheat says yes. `cheatcheck_rom()` runs the real
 * `cic_detect()` and the real IPL3 layout check; `cheatdb_emit()` reports the words it wrote;
 * `/mainmenu/launch.log` records all of it. On the first hardware run every one of those said
 * "will hook", and the cheats did nothing in game.
 *
 * That is because the interesting half happens where the menu is not. `cheats_install()` patches
 * one instruction in the game's IPL3 to jump into a patcher, the patcher copies an exception
 * handler into RDRAM and arms a **watch exception** on physical address 0x180, and the engine
 * only ever runs when the *game* writes its own exception handler there and traps. All of that
 * is after `boot()`: the display is closed, the filesystem is unmounted, and this program does
 * not come back. There is nothing to draw on and nothing to write to.
 *
 * So the failure is invisible, and the list of things it could be is long: the CIC, the patch
 * offset, the engine assembly, `skip_rdram_reset`, the flashcart, or -- the one this cannot rule
 * out from a desk -- a clone console whose CPU does not implement the watch exception at all.
 * The VR4300's WatchLo/WatchHi are among the most obscure registers it has, and an FPGA core that
 * omitted them would break exactly this and nothing else anybody would notice.
 *
 * ## Making the menu the test subject
 *
 * The one ROM whose insides we control is this one. So: a halfword of our own `.data`, its
 * address printed on the Settings screen, and a cheat the user types by hand that writes a known
 * value to it. Boot a copy of the menu with that cheat ticked, and if the engine is running it
 * writes the value on every exception -- of which a menu drawing frames has hundreds a second.
 * The second copy then reports what it found.
 *
 * A definite answer either way, on the user's own hardware, with nothing but the card:
 *
 *   1. Settings shows the address and the exact code to enter.
 *   2. Copy `sc64menu.n64` to `roms/n64/enginetest.n64` -- the scan skips the menu at the card
 *      root by name, so a copy under another name is what makes it appear in the library.
 *   3. Open it, press Z, type the code, tick it, launch.
 *   4. In the copy that boots, Settings says whether the engine wrote.
 *
 * If it says yes, the engine works and a cheat that does nothing is a wrong code or a wrong game.
 * If it says no, nothing downstream of the IPL3 patch is running and no cheat will ever work on
 * this console.
 */

#ifndef MENU_ENGINETEST_H__
#define MENU_ENGINETEST_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What the test cheat writes. Distinctive, so a stray write cannot be mistaken for it. */
#define ENGINETEST_VALUE  0xBEEF

/** @brief Where to aim the test cheat. Moves with every build; that is why it is displayed. */
uint32_t enginetest_address (void);

/** @brief The Datel line to type, e.g. "810A1234 BEEF". A 16-bit write, so the halfword lands. */
void enginetest_code (char *out, size_t cap);

/**
 * @brief Has the engine ever written the probe since this boot?
 *
 * Latched rather than sampled: the engine runs from the exception handler, and an exception can
 * happen between any two instructions here. Reading the raw halfword would be a race that comes
 * out false often enough to be believed.
 */
bool enginetest_seen (void);

/** @brief Check the probe. Call once a frame; costs one load and a compare. */
void enginetest_poll (void);

#endif /* MENU_ENGINETEST_H__ */
