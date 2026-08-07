/**
 * @file icon.h
 * @brief Several thousand icons, rasterised on the console, cached at two sizes.
 * @ingroup ui
 *
 * A profile is a name and a face. The faces come from game-icons.net -- 3,894 of them after the
 * IP review, as SVG text in `rom:/icons.pack` -- and svg64 turns one into pixels at the moment it
 * is needed. There is no sprite atlas and no offline conversion, which is the whole reason the
 * corpus can be this large: 3,894 pre-rendered 40 px tiles would be 12.5 MB of RGBA16 that had to
 * be regenerated for every theme, against 6.26 MB of text that is theme-independent.
 *
 * ## Everything here exists because rasterising costs about 5 ms
 *
 * svg64 measures 4.8 ms for a 40x40 icon in ares, with a worst case of 31.4 ms on one pathological
 * file out of 4,204 (svg64/docs/PERF.md). A 45-cell page of the picker is therefore about 216 ms
 * if it is filled in one frame, which is thirteen missed fields and a visible stall.
 *
 * So nothing here rasterises during render(). #icon_get is a cache lookup that either has the
 * pixels or does not, and #icon_pump does the work from background(), where the CPU is otherwise
 * waiting for the RDP to drain. A cell whose icon has not arrived draws a placeholder and pops in
 * a frame or two later, which is the same contract the thumbnail streamer already has with the
 * grid.
 *
 * ## Budgeted by time, not by count
 *
 * The obvious policy is three icons a frame, which is what svg64's own demo does and what holds
 * it at 60 VPS. It is not enough on its own here: three copies of the 31 ms icon is 94 ms, and
 * "at most three" would have said that was fine. #icon_pump takes a tick budget and stops when it
 * is spent, with three as a ceiling rather than a target.
 *
 * ## Two sizes, and only two
 *
 * 40 px for slot cards and grid cells, 60 px for the edit preview. The layout uses no others, and
 * a size class is a whole cache, so adding a third would be a third arena rather than a constant.
 *
 * A 40 px RGBA16 icon is 3,200 bytes and a 60 px one is 7,200. Both caches are fully associative
 * with least-recently-touched eviction. They were direct-mapped on the icon index, on the
 * assumption that the picker walks a contiguous run -- it does not, and icon.c's find() records
 * what that cost.
 *
 * ## The pack can be smaller than the profile that refers to it
 *
 * `make ICON_LIMIT=200` builds a 200-icon pack. A profile saved against a full build then holds
 * an index the pack does not have, and so does a profile from a card older than whatever gets
 * excluded next. #icon_get returns NULL for those and the caller draws its placeholder; the
 * stored index is never rewritten, so a full build shows the real icon again. Losing somebody's
 * chosen face because a developer capped a build would be the worse failure by a distance.
 */

#ifndef UI_ICON_H__
#define UI_ICON_H__

#include <stdbool.h>
#include <stdint.h>
#include <libdragon.h>

/** @brief The two sizes anything draws an icon at. Whole pixels; see the handoff, section 4. */
#define ICON_SMALL      40
#define ICON_LARGE      60

/** @brief An index no icon has, for "this profile has not chosen one". */
#define ICON_NONE       0xFFFF

/**
 * @brief Microseconds a frame may spend rasterising, from the shared pump in app.c.
 *
 * One 40 px icon costs about 4,800 us in ares. Six thousand therefore buys one ordinary icon per
 * frame and lets a slow one finish rather than leaving it half done, which the budget cannot do
 * anyway -- svg64_render() is not interruptible, so this bounds when the *next* one starts, not
 * how long the current one runs. That is why it is a ceiling on starting work and not a promise
 * about frame time, and why the worst icon in the corpus can still cost 31 ms whatever is set
 * here. Overridable with TUNE=-DICON_BUDGET_US=... for a sweep.
 */
#ifndef ICON_BUDGET_US
#define ICON_BUDGET_US  6000
#endif

/**
 * @brief Open the pack and the category index. Safe to call when neither exists.
 *
 * Failure is not fatal and must not be: a build with no pack still has profiles, and they still
 * need names and colours. #icon_count() returns 0 and every #icon_get returns NULL.
 */
void icon_init (void);

/** @brief Icons in the pack, or 0 if there is none. */
int icon_count (void);

/**
 * @brief The rasterised icon, or NULL if it is not ready yet.
 *
 * Never rasterises. A NULL means either "not decoded yet, ask again next frame" or "no such
 * icon", and the caller draws the same placeholder for both -- the distinction matters to
 * icon_pump() and to nobody else.
 *
 * @param index  pack index, or #ICON_NONE
 * @param size   #ICON_SMALL or #ICON_LARGE
 * @param ink    the colour the artwork is drawn in, RGBA5551
 * @param paper  the colour behind it, RGBA5551
 */
const surface_t *icon_get (uint16_t index, int size, uint16_t ink, uint16_t paper);

/**
 * @brief Ask for @p index to be decoded soon. Cheap, and safe to call every frame.
 *
 * The queue is one deep per size class and holds the most recent request, because the thing worth
 * decoding is whatever the cursor is on now -- not whatever it passed over while the stick was
 * held. Scrolling a page therefore costs one decode at the end rather than 45 along the way.
 */
void icon_request (uint16_t index, int size, uint16_t ink, uint16_t paper);

/**
 * @brief Do queued decoding until @p budget_ticks is spent. Call from background().
 *
 * @return how many icons were decoded, for the frame-time log.
 */
int icon_pump (uint32_t budget_ticks);

/** @brief The icon's name from the pack, into @p out. Empty if there is no such icon. */
void icon_name (uint16_t index, char *out, size_t cap);

/** @brief Default faces baked into the metadata, one per profile slot. */
#define ICON_STARTERS   10

/**
 * @brief The default face for profile slot @p slot, or #ICON_NONE if there is none.
 *
 * A new player should not be a blank plate, and ten of them should not be the same plate. The
 * indices are baked by tools/mkiconmeta.py against the pack that actually shipped, so a capped
 * build substitutes something it does have rather than pointing ten slots at icon 0.
 */
uint16_t icon_starter (int slot);

/* ------------------------------------------------------------------ categories -- */

/** @brief Categories in the index, or 0 if there is no metadata. */
int icon_cat_count (void);

/** @brief Display name of category @p cat. Never NULL. */
const char *icon_cat_name (int cat);

/** @brief How many icons category @p cat holds. */
int icon_cat_size (int cat);

/**
 * @brief The @p nth icon of category @p cat, as a pack index.
 *
 * Categories are contiguous runs in one baked array, so this is an array read and not a search.
 * Returns #ICON_NONE if either argument is out of range.
 */
uint16_t icon_cat_at (int cat, int nth);

#endif /* UI_ICON_H__ */
