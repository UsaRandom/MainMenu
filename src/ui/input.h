/**
 * @file input.h
 * @brief Joypad state with time-based repeat.
 * @ingroup ui
 *
 * Replaces upstream's actions.c. Two substantive changes:
 *
 * 1. Repeat is measured in SECONDS, not frames. Upstream arms an 8-frame delay and then repeats
 *    every frame (actions.c:6,44-89), which at its 30 fps cap is 0.27 s to first repeat and then
 *    30 steps a second. Uncapped at 60 fps that same code would repeat 60 times a second, so the
 *    feel of the menu would depend on the video mode.
 * 2. Repeat accelerates. The handoff's sound spec wants the cursor pitch to rise on the 4th and
 *    8th step of a held repeat, "a scroll should sound like it is accelerating" -- which only
 *    means anything if the scroll actually is.
 */

#ifndef UI_INPUT_H__
#define UI_INPUT_H__

#include <stdbool.h>
#include <stdint.h>
#include <libdragon.h>

/** @brief Edge-triggered buttons, one bit each. */
typedef enum {
    BTN_A     = (1 << 0),
    BTN_B     = (1 << 1),
    BTN_START = (1 << 2),
    BTN_L     = (1 << 3),
    BTN_R     = (1 << 4),
    BTN_Z     = (1 << 5),
    /** C-right, read as a button rather than as a direction. See input_poll() for what that
     *  costs: the C-pad is otherwise the fast-scroll pad, so this one direction is no longer
     *  available for it. */
    BTN_CRIGHT = (1 << 6),
} button_t;

typedef struct {
    /* Edge-triggered: true only on the frame a step is accepted, including repeats. */
    bool up, down, left, right;
    bool fast;              /**< C-pad direction: same step, faster repeat */
    uint32_t pressed;       /**< button_t bitmask, edge-triggered */

    uint32_t repeat_index;  /**< 0 on the initial press, then 1,2,3... while held */

    /* internal */
    joypad_8way_t held;
    float since_change;
    float until_repeat;
} input_t;

/** @brief Reset to a known state. */
void input_init (input_t *in);

/** @brief Poll the pads and resolve edges and repeats for a frame of @p dt seconds. */
void input_poll (input_t *in, float dt);

/** @brief True on the frame @p b transitions to pressed. */
static inline bool input_pressed (const input_t *in, button_t b) {
    return (in->pressed & b) != 0;
}

/** @brief True if any direction stepped this frame. */
static inline bool input_stepped (const input_t *in) {
    return in->up || in->down || in->left || in->right;
}

/* Held-direction repeat. 0.34 s before the first repeat is long enough that a single tap never
 * double-steps and short enough that holding does not feel stuck; it then accelerates from
 * 8 to 20 steps a second over the first eight repeats. A flat rate either crawls through a
 * 500-title library or overshoots by three rows on a short list. */
#define REPEAT_DELAY_S      0.34f
#define REPEAT_SLOW_S       (1.0f / 8.0f)
#define REPEAT_FAST_S       (1.0f / 20.0f)
#define REPEAT_RAMP_STEPS   8

/** @brief C-pad repeat, deliberately quicker: it exists to cross a long library. */
#define REPEAT_FAST_DELAY_S 0.20f
#define REPEAT_FAST_RATE_S  (1.0f / 30.0f)

#endif /* UI_INPUT_H__ */
