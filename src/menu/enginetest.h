/**
 * @file enginetest.h
 * @brief Does this console implement the watch exception the cheat engine hooks with?
 * @ingroup menu
 *
 * ## The question nothing else can answer
 *
 * Everything the menu can check about a cheat says yes. `cheatcheck_rom()` runs the real
 * `cic_detect()` and the real IPL3 layout check; `cheatdb_emit()` reports the words it wrote;
 * `/mainmenu/launch.log` records all of it. On two hardware runs every one of those said "will
 * hook", and the cheats did nothing in game.
 *
 * That is because the interesting half happens where the menu is not. `cheats_install()` patches
 * one instruction of the game's IPL3 to jump into a patcher; the patcher copies an exception
 * handler into RDRAM and **arms a watch exception on physical address 0x180**; and the engine
 * only ever runs when the game writes its own exception handler there and traps. All of it is
 * after `boot()` -- display closed, filesystem unmounted, and this program does not come back.
 *
 * WatchLo and WatchHi are among the most obscure registers the VR4300 has. The ModRetro M64 is a
 * clone console. **An FPGA CPU core that omitted the watch exception would break exactly this and
 * nothing else anybody would notice** -- which is a hypothesis, and hypotheses are cheap. This
 * makes it testable.
 *
 * ## Why this is not the obvious test
 *
 * The obvious test was to boot a copy of the menu with a cheat aimed at a variable inside it, and
 * have the copy report whether the value arrived. It cannot work, and the reason is worth keeping:
 * **the one ROM whose insides we control is the one ROM the engine cannot patch.** This is a
 * libdragon ROM, and libdragon ships its own IPL3 rather than a retail libultra one. The engine
 * refuses anything it does not recognise -- `cheats_ipl3_layout_ok()` checks for `jr $t1` at the
 * CIC's patch offset, and `sc64menu.n64` has `27bd0050` there, an `addiu $sp, $sp, 0x50`. The
 * detail sheet says "Not supported for this game", correctly, about the menu itself. Measured with
 * tools/hosttest/test_cheatinstall.c, which runs the production `cic_detect()`.
 *
 * ## So ask the CPU directly
 *
 * No cheat, no second ROM, no launch. Arm a watch on a variable we own, store to it, and see
 * whether the exception fires -- the same mechanism the engine depends on, exercised in three
 * instructions while the menu is still alive to report the answer.
 *
 * Three outcomes, and they have three different meanings:
 *
 *   - the register does not read back what was written -> not implemented; no cheat can ever run
 *   - it reads back, but a store to the watched address does not trap -> same conclusion, and it
 *     narrows where the omission is
 *   - it traps -> the mechanism works, and a cheat that does nothing is a wrong code or the fault
 *     is somewhere else entirely, which is worth knowing just as much
 *
 * ## The interlock
 *
 * A watch exception that fires and cannot be disarmed is an infinite exception loop, which on a
 * boot path is a console that will not start. The handler clears WatchLo before doing anything
 * else, so that should not happen -- but "should not" is not a thing to gamble somebody's evening
 * on, so the test leaves a marker file on the card while it runs and removes it afterwards. If the
 * marker is still there at the next boot the test did not finish, and it is never attempted again.
 */

#ifndef MENU_ENGINETEST_H__
#define MENU_ENGINETEST_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What the watch test concluded. */
typedef enum {
    WATCH_UNTESTED = 0,   /**< not run -- no storage to place the interlock, or not reached */
    WATCH_NO_REGISTER,    /**< WatchLo did not read back what was written to it */
    WATCH_NO_EXCEPTION,   /**< the register holds, but no store to the watched address trapped */
    WATCH_UNCACHED_ONLY,  /**< only an uncached store trapped -- not the case the engine needs */
    WATCH_WORKING,        /**< it trapped: the mechanism the cheat engine needs is present */
    WATCH_SKIPPED,        /**< a previous attempt left its marker behind, so this one is refused */
    WATCH_NO_HANDLER,     /**< the positive control failed, so nothing here can be believed */
} watchtest_t;

/**
 * @brief Run the test once, at boot. Needs the storage prefix for the interlock file.
 *
 * Cheap and self-contained: two COP0 writes, one store, one COP0 write to disarm. The exception
 * handler is registered for the duration and the previous one is restored.
 */
void enginetest_run (const char *storage_prefix);

/** @brief What it concluded. */
watchtest_t enginetest_watch (void);

/** @brief A short verdict for the Settings screen. Never NULL. */
const char *enginetest_text (void);

/** @brief The raw numbers behind it, for the launch log: what was armed, what read back, whether
 *         the control fired, whether the watch fired. */
void enginetest_detail (char *out, size_t cap);

#endif /* MENU_ENGINETEST_H__ */
