/**
 * @file debug_emux.h
 * @brief Framebuffer capture and run control through the EMUX emulator extensions.
 * @ingroup dev
 *
 * libdragon ships <emux.h>, which already implements the EMUX opcode encoding and wraps the
 * useful ones as inline functions -- emux_hexdump(), emux_ioctl_exit(), emux_detect(), and a
 * profiler exposing cycle, cache, RDP and RDRAM-traffic counters. Use those. An earlier
 * revision of this file hand-rolled the same instruction words and collided with the official
 * macros at compile time; there is nothing here worth reimplementing.
 *
 * What this header adds is the two project-specific pieces: a self-describing framebuffer dump
 * the host scripts can parse, and a detection assert.
 *
 * Everything compiles to nothing without DEV_HARNESS. A real VR4300 takes a reserved-
 * instruction exception on these opcodes, so a ROM built with it must never ship.
 *
 * Note that plain logging needs none of this. debug_init_isviewer() is already called on the
 * emulator path, so debugf() reaches ares' stdout on its own.
 */

#ifndef DEBUG_EMUX_H__
#define DEBUG_EMUX_H__

#include <stdbool.h>
#include <stdint.h>
#include <surface.h>
#include <time.h>

/* Divisor applied to both axes when a script dumps a frame. 4 gives 160x120 from 640x480:
 * roughly 250 KB of hex per frame instead of 4 MB, which is the difference between a
 * 600-frame run being routine and being unusable. Build with -DDBG_FBDUMP_SCALE=1 for a
 * full-resolution frame worth looking at closely. */
#ifndef DBG_FBDUMP_SCALE
#define DBG_FBDUMP_SCALE 4
#endif

/* Wall clock a harness build reports, in place of the RTC. 1996-06-23, the N64's Japanese
 * launch. Any fixed value would do; what matters is that it does not advance, because the
 * browser footer prints it and a live clock changes every frame hash on every screen. */
#define DEV_FROZEN_TIME ((time_t)835488000)

#ifdef DEV_HARNESS

#include <emux.h>

/**
 * @brief Report whether an EMUX host is listening, and log the answer.
 *
 * Worth calling once at startup. Without it, a run with Homebrew Mode off produces a log with
 * no dumps in it, which is indistinguishable from a ROM that crashed before reaching the
 * instrumentation. One line at boot turns a confusing afternoon into an obvious one.
 */
bool dbg_emux_present (void);

/**
 * @brief Point-sample a framebuffer into the log, bracketed by parseable markers.
 *
 * The dump is self-describing (`FBDUMP w=.. h=.. scale=.. fmt=rgba5551`) so one log survives a
 * change of resolution or colour depth. A host script with a hardcoded width silently
 * misreads every frame after such a change, and the resulting images look like a rendering
 * bug rather than a harness bug.
 *
 * Point-sampling rather than averaging is deliberate: averaging smooths away exactly the
 * single-pixel shimmer a contact sheet exists to catch.
 */
void dbg_fbdump (surface_t *fb, int scale);

/** @brief Ask the host to quit, so a headless run ends on its own instead of on a timeout. */
#define DBG_EXIT() emux_ioctl_exit()

#else

/* Inline functions rather than macros: a macro expanding to `(false)` is an unused-value
 * error under -Werror wherever the result is discarded, which is every call site that only
 * wants the log line. Release builds must compile without the harness. */
static inline bool dbg_emux_present (void) { return false; }
static inline void dbg_fbdump (surface_t *fb, int scale) { (void)fb; (void)scale; }

#define DBG_EXIT()              ((void)0)

#endif /* DEV_HARNESS */

#endif /* DEBUG_EMUX_H__ */
