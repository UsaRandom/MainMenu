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

int mem_thumb_slots (void) {
    /* Four, and the number is a measurement rather than a preference.
     *
     * Eight was tried first and fitted the 48-title fixture with about 70,000 bytes to spare,
     * which was the wrong thing to measure: the library costs 373 bytes of peak per title, so the
     * same build on the 499-title card DESIGN.md sizes for would have wanted roughly 160,000 more
     * than it had. At four, that card measures a peak of 2,126,472 against a 2,220,448 heap --
     * 93,976 spare, and measured on the real fixture rather than extrapolated to it.
     *
     * A screenful is twelve to sixteen tiles, so the pool cannot hold one and tiles WILL be
     * evicted while scrolling. That is affordable in a way it was not before: on a card with an
     * atlas the display path no longer decodes, it fetches, and a re-fetch is one seek and about
     * 27 KB. The thrash AUDIT 1u warns about is DECODE thrash, which this profile never does.
     *
     * Zero was considered and rejected. It does not merely turn art off, it turns the grid into a
     * wall of plates on a console whose owner can see perfectly well that other people get box
     * art. Four of a twelve-tile screen is a grid that fills in as you look at it. */
    return mem_small() ? 4 : 36;
}

int mem_icon_cache_cells (void) {
    return mem_small() ? 8 : 64;
}

void mem_report (const char *stage) {
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    debugf("MEM %-14s used %8d  free %8d  of %d\n", stage, hs.used, hs.total - hs.used, hs.total);
}
