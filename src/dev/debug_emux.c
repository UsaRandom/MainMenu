/**
 * @file debug_emux.c
 * @brief Framebuffer capture for the ares harness.
 * @ingroup dev
 */

#ifdef DEV_HARNESS

#include <libdragon.h>
#include <stdlib.h>

#include "debug_emux.h"
#include "menu/memprofile.h"

/* Sized for what this build's DBG_FBDUMP_SCALE will actually ask for, but never smaller than
 * the 320x240 that scales 2 and 4 have always used. Both halves matter. Without the first, a
 * full-resolution build (FBSCALE=1, for screenshots) is refused by the bounds check below and
 * silently produces no frames. Without the second, a scale-4 build would shrink the scratch
 * from 150 KB to 38 KB and move the heap under every allocation measurement in AUDIT.md, so a
 * capture tool would look like it had changed the thing it was measuring.
 *
 * Allocated once and kept, because allocating per dump would perturb the heap between frames
 * and make a memory regression look like a rendering one. */
#define DBG_FB_WANTED ((640 / DBG_FBDUMP_SCALE) * (480 / DBG_FBDUMP_SCALE))
#define DBG_FB_MAX_PIXELS (DBG_FB_WANTED > (320 * 240) ? DBG_FB_WANTED : (320 * 240))

/**
 * @brief Pixels to reserve: the padded figure above, or exactly what the scale needs on 4 MB.
 *
 * The 320x240 floor is there so a scale-4 build and a scale-1 build put the same hole in the heap
 * and an allocation measurement means the same thing in both -- see the note above, and keep it.
 * It cannot survive the small profile: 153,600 bytes against 143,144 free at the end of an idle
 * run means the dump fails, and a harness that cannot capture is a 4 MB build nobody can look at.
 * At scale 4 the real requirement is 38,400.
 *
 * Nothing in AUDIT.md is measured on a 4 MB console, so there is no comparability to protect here,
 * and any future measurement there starts from this number rather than from the padded one.
 */
static size_t scratch_pixels (void) {
    return mem_small() ? (size_t)DBG_FB_WANTED : (size_t)DBG_FB_MAX_PIXELS;
}

static uint16_t *scratch = NULL;

bool dbg_emux_present (void) {
    /* Subcode 1, not 0. The EMUX_FEAT1_* constants name the *second* bitmask: ares reports
     * opcodes 0x20-0x3F there (XHEXDUMP 0x27 lands on bit 0x7, XIOCTL 0x2C on bit 0xC), while
     * subcode 0 covers opcodes 0x00-0x1F and is empty on every host that exists. Asking for 0
     * returns zero and reports the harness absent while hexdump and exit demonstrably work --
     * a detector that fails closed on a working system is worse than no detector. */
    uint32_t feat = emux_detect(1);
    bool ok = (feat & EMUX_FEAT1_HEXDUMP) && (feat & EMUX_FEAT1_IOCTL);
    debugf("EMUX %s (FEAT1 0x%08lX)\n",
           ok ? "present" : "ABSENT -- is Homebrew Mode on?", (unsigned long)feat);
    return ok;
}

void dbg_fbdump (surface_t *fb, int scale) {
    if (fb == NULL) {
        debugf("FBDUMP skipped: no surface\n");
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    int w = fb->width / scale;
    int h = fb->height / scale;

    /* Refuse rather than truncate. A silently shortened dump still decodes to an image, just a
     * wrong one, and that reads as a rendering fault instead of a harness limit. */
    if ((size_t)(w * h) > scratch_pixels()) {
        debugf("FBDUMP skipped: %dx%d at scale %d exceeds the %u-pixel scratch\n",
               fb->width, fb->height, scale, (unsigned)scratch_pixels());
        return;
    }

    if (scratch == NULL && (scratch = malloc(scratch_pixels() * sizeof(uint16_t))) == NULL) {
        debugf("FBDUMP skipped: scratch allocation failed\n");
        return;
    }

    /* The RDP writes through its own path, so the CPU's view of the framebuffer is stale
     * without this. Skipping it yields a dump of whatever the previous frame left in cache --
     * which is subtly wrong in a way that looks like a one-frame render lag. */
    rspq_wait();
    data_cache_hit_invalidate(fb->buffer, fb->stride * fb->height);

    const uint8_t *src = (const uint8_t *)fb->buffer;
    for (int y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)(src + (size_t)(y * scale) * fb->stride);
        uint16_t *dst = &scratch[y * w];
        for (int x = 0; x < w; x++) {
            dst[x] = row[x * scale];
        }
    }

    debugf("FBDUMP w=%d h=%d scale=%d fmt=rgba5551\n", w, h, scale);
    emux_hexdump((const uint8_t *)scratch, (int)(w * h * sizeof(uint16_t)));
    debugf("FBEND\n");
}

#endif /* DEV_HARNESS */
