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
    bool  done;
} boot_plate_t;

/** @brief Arm the plate at t=0. */
void boot_plate_reset (boot_plate_t *bp);

/** @brief Advance. Returns true while the plate is still covering anything. */
bool boot_plate_step (boot_plate_t *bp, float dt);

/** @brief Draw over whatever is already in the framebuffer. No-op once done. */
void boot_plate_draw (const boot_plate_t *bp, const char *version, int title_count);

#endif /* SCREENS_BOOT_PLATE_H__ */
