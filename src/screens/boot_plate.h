/**
 * @file boot_plate.h
 * @brief The boot screen, drawn over the first screen there is. See boot_plate.c.
 * @ingroup screens
 */

#ifndef SCREENS_BOOT_PLATE_H__
#define SCREENS_BOOT_PLATE_H__

#include <stdbool.h>

/**
 * @brief Arm the plate, once per power-on. Later calls do nothing.
 *
 * Called from the enter() of whichever screen is first, which is the grid on a card with one
 * player and the picker on a card with several. It was a struct owned by the grid, on the
 * reasoning in boot_plate.c that the plate must reveal something already live rather than hand
 * over to a cold screen -- and that is still the reasoning, but "something already live" turned
 * out not to always be the grid. A card with four players showed the picker first, cold and with
 * no plate at all, and only played the boot animation afterwards over the grid.
 *
 * One plate, armed once, so whichever screen gets there first is the one it lifts off. Nothing
 * replays it: the grid arming it after the picker already has is a no-op.
 */
void boot_plate_arm (void);

/**
 * @brief Advance. Returns true while the plate is still covering anything.
 *
 * @param ready  Caller's judgement that the screen underneath is worth revealing. Consulted only
 *               after the minimum hold, and overridden by the maximum one.
 */
bool boot_plate_step (float dt, bool ready);

/**
 * @brief Advance the clock during blocking work, without ever releasing.
 *
 * For the stretch before the main loop exists. The library scan is a single blocking call of
 * about 11.5 ms per ROM -- 3.2 s on a 278-title card, and it runs on every boot on a console
 * where cache_writable() is false -- and until this existed the screen showed nothing at all for
 * the whole of it, then the plate began its own 2.5 s hold afterwards. The two were sequential,
 * so the plate hid none of the cost it was there to hide.
 *
 * Clamped to the minimum hold rather than left to run, because the ceiling in boot_plate_step()
 * would otherwise fire mid-scan and lift the curtain onto a grid that does not exist yet. Arriving
 * at the main loop already at the threshold is the point: the first step() with a ready screen
 * releases immediately, so a scan that takes longer than the hold costs nothing beyond itself.
 */
void boot_plate_hold (float dt);

/**
 * @brief True while the plate is holding still and background work is free.
 *
 * False during the mark's rise and during the curtain, which are the two animated stretches and
 * the two where a 20 ms decode would show as a stutter. Also false before it is armed and after
 * it is over, so this is the whole answer to "may I spend a frame on boot work".
 */
bool boot_plate_working (void);

/** @brief True once the plate has finished, or if it was never armed. */
bool boot_plate_done (void);

/**
 * @brief Microseconds of cover decoding a frame may spend while the plate is up.
 *
 * Lives with the plate rather than with the grid, because the plate's hold is the whole reason
 * there is a boot budget at all: nothing is animating, no input is accepted, and the only thing
 * the frame rate governs is a mark standing still, so this is the one place in the program where
 * dropping a field is free. It moved here when the plate stopped belonging to the grid -- the
 * picker spends the same budget on the same work, and the number must be one number.
 */
#ifndef DECODE_BUDGET_BOOT_US
#define DECODE_BUDGET_BOOT_US   14000
#endif

/** @brief Draw over whatever is already in the framebuffer. No-op once done. */
void boot_plate_draw (const char *version, int title_count);

#endif /* SCREENS_BOOT_PLATE_H__ */
