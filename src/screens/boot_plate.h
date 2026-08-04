/**
 * @file boot_plate.h
 * @brief The boot screen, drawn over the grid. See boot_plate.c.
 * @ingroup screens
 */

#ifndef SCREENS_BOOT_PLATE_H__
#define SCREENS_BOOT_PLATE_H__

#include <stdbool.h>

/** @brief Boot animation state. Lives in the grid, which draws it over itself. */
typedef struct {
    float t;
    float curtain_at;   /**< t at which the hold was released; meaningless until released */
    bool  released;
    bool  done;
} boot_plate_t;

/** @brief Arm the plate at t=0. */
void boot_plate_reset (boot_plate_t *bp);

/**
 * @brief Advance. Returns true while the plate is still covering anything.
 *
 * @param ready  Caller's judgement that the screen underneath is worth revealing. Consulted only
 *               after the minimum hold, and overridden by the maximum one.
 */
bool boot_plate_step (boot_plate_t *bp, float dt, bool ready);

/**
 * @brief True while the plate is holding still and background work is free.
 *
 * False during the mark's rise and during the curtain, which are the two animated stretches and
 * the two where a 20 ms decode would show as a stutter.
 */
bool boot_plate_working (const boot_plate_t *bp);

/** @brief Draw over whatever is already in the framebuffer. No-op once done. */
void boot_plate_draw (const boot_plate_t *bp, const char *version, int title_count);

#endif /* SCREENS_BOOT_PLATE_H__ */
