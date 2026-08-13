/**
 * @file memprofile.c
 * @brief The two profiles. See memprofile.h for the budget every small-profile number comes from.
 * @ingroup menu
 */

#include <libdragon.h>

#include "menu/memprofile.h"

/** 4 MB, in bytes. A console reporting less than this plus a margin has no Expansion Pak. */
#define RDRAM_4MB   (4 * 1024 * 1024)

static bool  probed;
static bool  small;
static unsigned total;

static void probe (void) {
    if (probed) {
        return;
    }
    total = (unsigned)get_memory_size();
    /* is_memory_expanded() is the same question and is what the rest of the tree already asks;
     * the size is taken as well because the fault screen and the boot record both print it, and
     * two ways of asking one thing is how they end up disagreeing. */
    small = (total <= RDRAM_4MB);
    probed = true;
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    debugf("MEMPROFILE %u bytes RDRAM -- %s profile -- heap %d total, %d used, %d free\n",
           total, small ? "SMALL" : "full", hs.total, hs.used, hs.total - hs.used);
}

bool mem_small (void) {
    probe();
    return small;
}

unsigned mem_total_bytes (void) {
    probe();
    return total;
}

int mem_fb_count (void) {
    return mem_small() ? 2 : 3;
}

/** The largest tile a slot can be asked to hold: a wide cover at 140 x 162, RGBA16 -- divided by
 *  the profile's art divisor in both axes, because that is the size tiles are actually STORED at.
 *  Sizing the pool against the full figure while storing quarter-size tiles is how the first
 *  version of this handed a 4 MB console three slots and left 232,072 bytes unused. */
static int worst_tile_bytes (void) {
    int d = mem_art_divisor();
    return (140 / d) * (162 / d) * 2;
}

/** Held back from the tile pool for everything that allocates after it. See mem_thumb_slots(). */
#define POOL_RESERVE      131072

int mem_thumb_slots (void) {
    /* Taken from what is actually free, not from a constant per profile.
     *
     * A constant has to be chosen for the worst library anybody might have, and then every smaller
     * library gets that worst case too. Measured on 4 MB: a 499-title card leaves 334,576 bytes
     * free at this point and a 289-title one leaves about 78,000 more, because the library costs
     * 373 bytes of peak per title. Fixing the pool at four -- which is what 499 titles affords --
     * meant a 289-title card showed four tiles of art on a twelve-tile screen while 78,000 bytes
     * of RAM sat unused. The number that matters is the room left over, so ask.
     *
     * Called once, from thumbcache_init(), which app.c runs after the library, playstate, locks and
     * the cheat database are all allocated -- so "free" here is the real remainder and not a
     * snapshot taken before the expensive things happened.
     *
     * POOL_RESERVE is for what allocates AFTER this: the decoder's destination surface, which is a
     * whole tile again, plus the thumbstore index growing as tiles are added and the scaler's
     * accumulator row. Not for libindex_save()'s payload at exit -- that one is a soft write which
     * returns false and skips the save if it cannot allocate, so it degrades rather than crashes.
     *
     * On 8 MB this arithmetic clamps to THUMB_SLOTS every time (about 2.5 MB is free here, which
     * is 55 slots' worth), so the full profile is the 36 it has always been.
     */
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    int free_now = hs.total - hs.used;

    int affordable = (free_now - POOL_RESERVE) / worst_tile_bytes();
    if (affordable > 36) {
        affordable = 36;
    }
    /* Three even if the arithmetic says fewer. Below three the grid stops being a grid with art in
     * it and becomes a grid with an accident in it, and a console that is this short of memory has
     * already failed at something more important than box art. */
    if (affordable < 3) {
        affordable = 3;
    }

    debugf("MEMPROFILE pool: %d slots from %d free (reserve %d, tile %d)\n",
           affordable, free_now, POOL_RESERVE, worst_tile_bytes());
    return affordable;
}

int mem_art_divisor (void) {
    return mem_small() ? 2 : 1;
}

int mem_icon_cache_cells (void) {
    return mem_small() ? 8 : 64;
}

void mem_report (const char *stage) {
    heap_stats_t hs;
    sys_get_heap_stats(&hs);

    /* Elapsed since the previous stage, as well as the heap.
     *
     * Added because a user reported the music stalling for about a second on the boot plate and
     * there was no way to say which call did it: boot is a dozen blocking calls in a row and only
     * the library scan was ever timed. A stage longer than sound_slack_us() -- eight buffers at
     * 16 kHz, measured at 316,059 us -- is a stage the audio cannot survive, so the number that
     * matters is per-stage wall clock and not the total. */
    static uint32_t prev_ticks;
    uint32_t now = TICKS_READ();
    unsigned long us = (prev_ticks != 0) ? (unsigned long)TIMER_MICROS(TICKS_DISTANCE(prev_ticks, now)) : 0;
    prev_ticks = now;

    debugf("MEM %-14s used %8d  free %8d  of %d  (+%lu us)\n",
           stage, hs.used, hs.total - hs.used, hs.total, us);
}
