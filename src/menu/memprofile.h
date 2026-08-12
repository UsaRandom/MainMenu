/**
 * @file memprofile.h
 * @brief One decision -- how much RDRAM is there -- and everything that follows from it.
 * @ingroup menu
 *
 * The M64 has the Expansion Pak built in, so for the console this is written for the answer is
 * always 8 MB and every number below is the one it has always used. A stock N64 has 4 MB, and on
 * 4 MB this program did not start: `display_init()` asserted on its third framebuffer before a
 * frame was ever drawn.
 *
 * ### The rule
 *
 * **Nothing here may change anything when the pak is present.** Every function returns exactly
 * the constant that used to be hard-coded when mem_small() is false, so an 8 MB build is the same
 * build it was -- same buffers, same caches, same art. The small profile is a separate set of
 * numbers reached only by a console that cannot run the other one.
 *
 * ### Where the 4 MB budget goes
 *
 * All measured under ares with the Expansion Pak off -- `FOURMB=1 ARES_SETTINGS=... regress.sh`,
 * see tools/regress.sh -- and not one of them guessed. The heap is **2,221,744 bytes** where an
 * 8 MB console gets 6,416,048, because the ~1.97 MB outside the heap (IPL3, text, data, bss,
 * stack) is the same on both.
 *
 *     stage           full profile      small profile
 *     ------------------------------------------------
 *     icons              249,808            81,528
 *     music               54,576            54,576
 *     framebuffers     1,843,264         1,228,848
 *     fonts            1,364,624           186,096
 *
 * The body font is the whole story: **1,284,208 bytes of the 1,364,624**, which is more than the
 * two framebuffers it would have to share a 2.2 MB heap with. It does not fit with everything
 * else stripped out, and it does not fit with nothing else at all. 2,187 of its 2,697 characters
 * are CJK ideographs. See the FirpleBody4M rule in the Makefile.
 *
 * With those four changed, a **499-title** library measures a peak of 2,126,472 against the
 * 2,220,448 heap of that build -- 93,976 spare, 4.2 %. 499 is what DESIGN.md sizes for, and it
 * is measured rather than extrapolated: the 48- and 115-title fixtures put the growth at 373
 * bytes of peak per title, which predicted 2,136,549, and running the real thing said 2,126,472.
 */

#ifndef MENU_MEMPROFILE_H__
#define MENU_MEMPROFILE_H__

#include <stdbool.h>

/**
 * @brief Is this a console without an Expansion Pak?
 *
 * Asked once and cached, because it is on the path of things that run per frame and
 * get_memory_size() is a hardware probe. Safe to call before anything is initialised -- there is
 * nothing to initialise.
 */
bool mem_small (void);

/** @brief Total RDRAM, for the diagnostics that report it. */
unsigned mem_total_bytes (void);

/**
 * @brief Framebuffers to allocate. 3 with a pak, 2 without.
 *
 * The third costs 614,400 bytes and buys the window background() runs in: with two,
 * display_try_get() returns NULL whenever the RDP has not drained and the CPU spins instead of
 * decoding. Upstream runs two because it has to fit in 4 MB, which is exactly the situation the
 * small profile is in. Losing it costs fill rate, not function.
 */
int mem_fb_count (void);

/**
 * @brief How many thumbnail slots may hold a decoded surface at once.
 *
 * The slot ARRAY is THUMB_SLOTS either way and costs nothing to leave at full size -- a slot is a
 * few bytes until something lands in it. This bounds the surfaces, which are 27,440 to 45,360
 * bytes each and are the whole cost.
 *
 * At 36 the pool is a screenful plus two rows of prefetch either side. The small profile cannot
 * afford a screenful: sixteen landscape tiles is 439,040 bytes against a 282,793-byte budget that
 * also has to hold the icons and the library. See memprofile.c for what it settles on and why
 * that is not simply "as many as fit".
 */
int mem_thumb_slots (void);

/** @brief May the icon cache hold its full complement? The small profile keeps a smaller one. */
int mem_icon_cache_cells (void);

/**
 * @brief Print the heap at a named point in the boot.
 *
 * The 4 MB budget was argued from arithmetic before this existed and the arithmetic was wrong in
 * both directions -- it is what a stage costs in the order the stages actually run that decides
 * whether the next one has room. Cheap, and only ever debugf.
 */
void mem_report (const char *stage);

#endif /* MENU_MEMPROFILE_H__ */
