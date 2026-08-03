/**
 * @file input.c
 * @brief Joypad state with time-based repeat.
 * @ingroup ui
 */

#include <string.h>

#include "dev/inputscript.h"
#include "input.h"
#include "tween.h"

void input_init (input_t *in) {
    memset(in, 0, sizeof(*in));
    in->held = JOYPAD_8WAY_NONE;

    JOYPAD_PORT_FOREACH (port) {
        joypad_set_rumble_active(port, false);
    }
}

/** @brief Seconds until the next repeat, given how many have already fired. */
static float repeat_interval (uint32_t index, bool fast) {
    if (fast) {
        return REPEAT_FAST_RATE_S;
    }
    if (index >= REPEAT_RAMP_STEPS) {
        return REPEAT_FAST_S;
    }
    /* Linear ramp from slow to fast across the first REPEAT_RAMP_STEPS repeats. */
    float t = (float)index / (float)REPEAT_RAMP_STEPS;
    return lerpf(REPEAT_SLOW_S, REPEAT_FAST_S, t);
}

static void apply_direction (input_t *in, joypad_8way_t dir) {
    switch (dir) {
        case JOYPAD_8WAY_RIGHT:      in->right = true; break;
        case JOYPAD_8WAY_UP_RIGHT:   in->up = true; in->right = true; break;
        case JOYPAD_8WAY_UP:         in->up = true; break;
        case JOYPAD_8WAY_UP_LEFT:    in->up = true; in->left = true; break;
        case JOYPAD_8WAY_LEFT:       in->left = true; break;
        case JOYPAD_8WAY_DOWN_LEFT:  in->down = true; in->left = true; break;
        case JOYPAD_8WAY_DOWN:       in->down = true; break;
        case JOYPAD_8WAY_DOWN_RIGHT: in->down = true; in->right = true; break;
        default: break;
    }
}

void input_poll (input_t *in, float dt) {
    joypad_poll();
    inputscript_tick();

    in->up = in->down = in->left = in->right = false;
    in->fast = false;
    in->pressed = 0;

    joypad_8way_t dir = JOYPAD_8WAY_NONE;
    joypad_8way_t cdir = JOYPAD_8WAY_NONE;
    joypad_buttons_t pressed = {0};

    /* A scripted run replaces the physical pads but still goes through the repeat logic below,
     * so the harness exercises the real input path rather than a parallel one that could drift
     * away from it. */
    if (inputscript_active()) {
        dir = inputscript_direction(false);
        cdir = inputscript_direction(true);
        pressed.a     = inputscript_pressed(ISCRIPT_BTN_A);
        pressed.b     = inputscript_pressed(ISCRIPT_BTN_B);
        pressed.start = inputscript_pressed(ISCRIPT_BTN_START);
        pressed.l     = inputscript_pressed(ISCRIPT_BTN_L);
        pressed.r     = inputscript_pressed(ISCRIPT_BTN_R);
        pressed.z     = inputscript_pressed(ISCRIPT_BTN_Z);
    } else {
        JOYPAD_PORT_FOREACH (i) {
            dir = joypad_get_direction(i, JOYPAD_2D_DPAD | JOYPAD_2D_STICK);
            cdir = joypad_get_direction(i, JOYPAD_2D_C);
            if (dir != JOYPAD_8WAY_NONE || cdir != JOYPAD_8WAY_NONE) {
                break;
            }
        }
        JOYPAD_PORT_FOREACH (i) {
            joypad_buttons_t p = joypad_get_buttons_pressed(i);
            if (p.raw) {
                pressed = p;
                break;
            }
        }
    }

    bool fast = false;
    if (dir == JOYPAD_8WAY_NONE && cdir != JOYPAD_8WAY_NONE) {
        dir = cdir;
        fast = true;
    }
    in->fast = fast;

    if (dir == JOYPAD_8WAY_NONE) {
        in->held = JOYPAD_8WAY_NONE;
        in->repeat_index = 0;
        in->since_change = 0.0f;
        in->until_repeat = 0.0f;
    } else if (dir != in->held) {
        /* Fresh press: step immediately, then wait out the initial delay. Stepping on the same
         * frame the direction changes is what makes a single tap feel instant. */
        in->held = dir;
        in->repeat_index = 0;
        in->since_change = 0.0f;
        in->until_repeat = fast ? REPEAT_FAST_DELAY_S : REPEAT_DELAY_S;
        apply_direction(in, dir);
    } else {
        in->since_change += dt;
        in->until_repeat -= dt;
        if (in->until_repeat <= 0.0f) {
            in->repeat_index++;
            in->until_repeat += repeat_interval(in->repeat_index, fast);
            /* A long stall (an SD read, a shader compile under ares) can leave until_repeat
             * deeply negative. Without this the menu would fire a burst of steps to "catch up"
             * and jump several rows for one held press. */
            if (in->until_repeat < 0.0f) {
                in->until_repeat = repeat_interval(in->repeat_index, fast);
            }
            apply_direction(in, dir);
        }
    }

    if (pressed.a)     in->pressed |= BTN_A;
    if (pressed.b)     in->pressed |= BTN_B;
    if (pressed.start) in->pressed |= BTN_START;
    if (pressed.l)     in->pressed |= BTN_L;
    if (pressed.r)     in->pressed |= BTN_R;
    if (pressed.z)     in->pressed |= BTN_Z;
}
