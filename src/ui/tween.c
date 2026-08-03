/**
 * @file tween.c
 * @brief Easing and frame-rate-independent smoothing.
 * @ingroup ui
 */

#include <math.h>

#include "tween.h"

float clampf (float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float ease_bezier (float t, float x1, float y1, float x2, float y2) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;

    /* Newton-solve x(u) = t for the curve parameter u, then evaluate y(u). The x polynomial is
     * expanded once so the loop is three multiply-adds and a divide. */
    const float f0 = 1.0f - 3.0f * x2 + 3.0f * x1;
    const float f1 = 3.0f * x2 - 6.0f * x1;
    const float f2 = 3.0f * x1;

    float u = t;
    for (int i = 0; i < 5; i++) {
        float u2 = u * u;
        float x = f0 * u2 * u + f1 * u2 + f2 * u;
        float dx = 3.0f * f0 * u2 + 2.0f * f1 * u + f2;
        if (dx > -1e-6f && dx < 1e-6f) {
            break;                      /* flat segment: another step would divide by ~0 */
        }
        u -= (x - t) / dx;
        u = clampf(u, 0.0f, 1.0f);
    }

    /* y(u) for a curve anchored at (0,0) and (1,1). Written as multiplies rather than pow():
     * the reference used pow(x, 2.0f) and pow(x, 3.0f) here, which are double-precision libm
     * calls evaluated per tween per frame, for values that are exactly u*u and u*u*u. */
    const float iu = 1.0f - u;
    const float u2 = u * u;
    return 3.0f * iu * iu * u * y1 + 3.0f * iu * u2 * y2 + u2 * u;
}

float smooth_towards (float cur, float target, float rate, float dt) {
    if (dt <= 0.0f) {
        return cur;
    }
    return cur + (target - cur) * (1.0f - expf(-rate * dt));
}

void tween_start (tween_t *tw, float duration) {
    tw->elapsed = 0.0f;
    tw->duration = duration > 0.0f ? duration : 0.0001f;
    tw->running = true;
}

bool tween_step (tween_t *tw, float dt) {
    if (!tw->running) {
        return false;
    }
    tw->elapsed += dt;
    if (tw->elapsed >= tw->duration) {
        tw->elapsed = tw->duration;
        tw->running = false;
    }
    return tw->running;
}

float tween_t01 (const tween_t *tw) {
    if (tw->duration <= 0.0f) {
        return 1.0f;
    }
    return clampf(tw->elapsed / tw->duration, 0.0f, 1.0f);
}
