/**
 * @file hooktest.h
 * @brief End-to-end proof that the preamble hook works, run against the real emitted code.
 * @ingroup dev
 *
 * The cheat engine's history in this project is a sequence of things that looked installed and
 * never ran (AUDIT.md 2.4, 1af). This test exists so the preamble hook cannot join them: it
 * calls the production cheats_emit(), then EXECUTES the emitted patcher against a synthetic
 * game image -- a planted __osExceptionPreamble aimed at a fake __osException -- and then
 * executes the patched preamble the way an exception vector would, asserting that every cheat
 * value landed and that control came out the far side through the fake __osException.
 *
 * Two scenarios, because a hook test that cannot take the other branch proves nothing:
 *
 *   1. The image contains a well-formed preamble. The scan must find it, must rewrite it, must
 *      leave 0x80000180 and the watch register alone, and the executed chain
 *      preamble -> engine -> cheats -> original lui/addiu -> fake __osException -> back
 *      must deliver every write.
 *   2. The image's preamble is broken by one word. The scan must run its full megabyte, give up,
 *      and take the Datel path -- observable as `j engine` written over 0x80000180 and
 *      WatchLo reading back armed. The test then disarms the watch and restores the vector
 *      before interrupts are re-enabled, because that vector is libdragon's and the menu is
 *      still alive.
 *
 * The scan window is [$t1, $t1 + 1 MB) of whatever $t1 points at, so the synthetic image is a
 * full megabyte of zeroed .bss -- scanning a smaller buffer would put live heap inside the
 * window, and one pattern-shaped coincidence in a decoded PNG would flake the fallback scenario
 * red. A megabyte of .bss exists only in DEV_HARNESS builds and only here.
 */

#ifndef DEV_HOOKTEST_H__
#define DEV_HOOKTEST_H__

#ifdef DEV_HARNESS

void hooktest_run (void);

#else

static inline void hooktest_run (void) { }

#endif

#endif /* DEV_HOOKTEST_H__ */
