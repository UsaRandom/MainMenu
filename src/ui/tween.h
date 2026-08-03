/**
 * @file tween.h
 * @brief Easing and frame-rate-independent smoothing.
 * @ingroup ui
 *
 * Durations and control points come from docs/design/README.md section 5. They are specified
 * in SECONDS, never frames, because the frame rate is still an open question -- see AUDIT.md.
 * Anything here that took a frame count would silently change feel when the video mode changes
 * and would corrupt the very A/B that decides the video mode.
 */

#ifndef UI_TWEEN_H__
#define UI_TWEEN_H__

#include <stdbool.h>

/**
 * @brief Solve a CSS-style cubic-bezier for @p t in [0,1].
 *
 * Control points are (x1,y1) and (x2,y2); the curve runs (0,0) to (1,1). Five Newton
 * iterations, which is the right accuracy for something evaluated a few dozen times a frame.
 */
float ease_bezier (float t, float x1, float y1, float x2, float y2);

/**
 * @brief Exponential approach to @p target, independent of frame rate.
 *
 * The reference mockup used `cur += (target - cur) * 0.4f` once per frame, which silently
 * changes feel with the frame rate. This is the continuous form:
 *
 *     cur + (target - cur) * (1 - exp(-rate * dt))
 *
 * A per-frame factor f at hz converts as `rate = -ln(1 - f) * hz`; the reference's 0.4 at 60 Hz
 * is 30.6, so SMOOTH_RATE_SCROLL is 30.
 */
float smooth_towards (float cur, float target, float rate, float dt);

/** @brief Clamp @p v into [lo,hi]. */
float clampf (float v, float lo, float hi);

/** @brief Linear interpolation. */
static inline float lerpf (float a, float b, float t) { return a + (b - a) * t; }

/** @brief A one-shot timed transition. */
typedef struct {
    float elapsed;
    float duration;
    bool  running;
} tween_t;

/** @brief Arm a tween for @p duration seconds. */
void tween_start (tween_t *tw, float duration);

/** @brief Advance by @p dt. Returns true while still running. */
bool tween_step (tween_t *tw, float dt);

/** @brief Progress in [0,1]; 1 when idle or finished. */
float tween_t01 (const tween_t *tw);

/* Motion constants, docs/design/README.md section 5. */
#define DUR_CURSOR_STEP     0.10f
#define DUR_TILE_GROW       0.14f
#define DUR_GRID_SCROLL     0.13f
#define DUR_AMBIENT_REKEY   0.22f
#define DUR_TAB_CHANGE      0.18f
#define DUR_SHEET_OPEN      0.20f
#define DUR_SHEET_CLOSE     0.14f
#define DUR_LAUNCH_EXPAND   0.26f
#define DUR_BOOT_CURTAIN    0.34f
#define DUR_TILE_ARRIVAL    0.12f
/** Scale an arriving tile starts at, section 5's motion table. */
#define TILE_ARRIVE_SCALE   0.86f

/** @brief Selection outline breathing, 0.45 Hz -- a 2.2 s loop. */
#define SEL_PULSE_HZ        0.45f

/* cubic-bezier control points, in the order ease_bezier() takes them. */
#define EASE_CURSOR_STEP    0.22f, 1.00f, 0.36f, 1.00f
#define EASE_TILE_GROW      0.34f, 1.32f, 0.64f, 1.00f   /* overshoots, then settles */
#define EASE_GRID_SCROLL    0.16f, 1.00f, 0.30f, 1.00f
#define EASE_TAB_CHANGE     0.40f, 0.00f, 0.20f, 1.00f
#define EASE_SHEET_OPEN     0.20f, 0.90f, 0.30f, 1.08f
#define EASE_SHEET_CLOSE    0.40f, 0.00f, 1.00f, 1.00f
#define EASE_LAUNCH_EXPAND  0.62f, 0.00f, 0.86f, 0.35f
#define EASE_BOOT_CURTAIN   0.70f, 0.00f, 0.20f, 1.00f

/** @brief Smoothing rate for grid scroll. See smooth_towards(). */
#define SMOOTH_RATE_SCROLL  30.0f

#endif /* UI_TWEEN_H__ */
