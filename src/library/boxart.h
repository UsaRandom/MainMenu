/**
 * @file boxart.h
 * @brief What shape a box is, per system.
 * @ingroup library
 *
 * ## The problem
 *
 * The grid drew every game into a 140 x 98 landscape tile, which is the shape of the reference
 * mockup's title card and the shape of no box that has ever existed. Real front faces, measured
 * off box-protector inside dimensions (which run 1-2 mm over the box itself):
 *
 * | system | front face | aspect W:H |
 * |---|---|---|
 * | NES | 127 x 180 mm | 0.706 |
 * | SNES | 127 x 181 mm | 0.702 |
 * | N64 | 127 x 181 mm | 0.702 |
 * | Master System | 128 x 179 mm | 0.715 |
 * | Game Boy | 126 x 126 mm | 1.000 |
 * | Game Boy Color | 126 x 126 mm | 1.000 |
 *
 * Practically two shapes: portrait for the cartridge boxes -- NES, SNES, N64 and SMS share
 * protectors, and Genesis shares the SMS size -- and square for the handhelds. The decoder scales
 * to cover and crops rather than letterboxing (docs/design/README.md section 7), so a 0.702 box
 * in a 1.4286 tile lost **51 % of its own height**, top and bottom, on every cover on the card.
 *
 * ## The shape comes from the cover, and the table is the fallback
 *
 * A per-system table is right about the box and wrong about the file, and the file is what gets
 * drawn. Covers arrive from scrapers that do not normalise dimensions: some scans are tight, some
 * carry margin, some are padded square, and plenty of "covers" are title screens or cartridge
 * photos. So the shape of a tile is read off its own art and snapped to one of #ART_SHAPES, and
 * the table below is what an artless -- or not-yet-probed -- record gets.
 *
 * Measured over tools/mksample.py's 115-cover card (AUDIT.md 1ak), as mean source area cropped:
 *
 * |                          | realistic mix | hostile mix |
 * |---|---:|---:|
 * | per-system table         | 8.1 % | 20.3 % |
 * | snap to two shapes       | 5.0 % | 11.0 % |
 * | **snap to three**        | **2.3 %** | **2.9 %** |
 * | snap to four             | 2.2 % | 2.4 % |
 *
 * Three, and exactly three. Two is not enough because the worst case is a landscape cover cut
 * into a portrait tile and neither portrait nor square is anywhere near 1.43. A fourth -- a
 * dedicated PAL-tall bucket at 0.62 -- moves the mean by a tenth of a percent and does not remove
 * a single cover from the "loses more than 10 %" column.
 *
 * This is also what makes the region table optional rather than necessary: a PAL box photographed
 * as a PAL box snaps to portrait on its own, and a card that mixes regions needs no setting at
 * all. The setting survives as the escape hatch -- #boxart_region_current 0 is "Automatic" and
 * anything else forces the table -- because there is no measurement yet of how often the snap
 * gets it wrong on a real pack.
 *
 * ## What this file decides, and what it does not
 *
 * Only the aspect. Every number below is read as a ratio, never as a length: a region whose boxes
 * are "narrower" is the same picture as one whose boxes are taller, which is the correct reading
 * -- what reaches the screen is a shape, not a millimetre.
 *
 * The ratio then picks a grid column. There are two of them, four tiles across or five, and the
 * rule that chooses is in ui/theme.h: **the fewest columns that still show two whole rows**. A
 * portrait box is 155 px tall in a 109 px column; a square or landscape one fits a 140 px column
 * and gets it. That is the difference between a landscape cover at 109 x 76 and the same cover at
 * 140 x 98 -- 66 % more pixels for art that is otherwise identical.
 *
 * A tab is laid out on the tallest shape it holds, and shorter art is centred in that cell rather
 * than stretched or cropped. Because a taller shape never gets a wider column, the tallest shape
 * in a tab also has the narrowest column in it, so every other shape in the tab is drawn smaller
 * than the size it was cached at. Nothing is ever cropped to fit a neighbour and nothing is ever
 * upscaled out of the atlas.
 *
 * ## Why the regional tables are not in this file
 *
 * The measurements above are NTSC/US retail and are the built-in table. PAL NES boxes are taller
 * and thinner, PAL SMS stock changed mid-generation, and Japanese SFC, N64 and GB boxes are all
 * smaller than their US counterparts. Those numbers are not ours to invent -- inventing them
 * would put five fabricated aspects into a program whose whole style is that a claim carries a
 * measurement -- so they live in `menu/boxart.ini` on the card, in named sections, and the
 * Settings screen offers whichever sections the file defines.
 *
 * That also answers "certain sections use PAL" without a per-system setting: a section is a whole
 * table, so a card can define one that is NTSC for N64 and PAL for NES and pick it.
 *
 *     [pal]
 *     nes  = 135x190
 *     snes = 127x181
 *     gb   = 126x126
 *
 * Keys are the system names the tabs use, lower-cased. A key the section omits keeps its built-in
 * value, so a section only has to say what it changes.
 */

#ifndef LIBRARY_BOXART_H__
#define LIBRARY_BOXART_H__

#include <stdbool.h>
#include <stdint.h>

/** @brief A tile, in pixels. Its width is a grid column: one of #TILE_W_WIDE or #TILE_W_NARROW. */
typedef struct {
    uint16_t w;
    uint16_t h;
} art_shape_t;

/** @brief The shapes a tile may be. Index order is the on-disk order; see @ref boxart_shape_at. */
typedef enum {
    ART_PORTRAIT = 0,   /**< 0.702 -- every cartridge box */
    ART_SQUARE,         /**< 1.000 -- Game Boy and Game Boy Color */
    ART_LANDSCAPE,      /**< 1.4286 -- title cards, screenshots, the old asset spec */
    ART_SHAPES,
} art_kind_t;

/** @brief "Not probed yet". Stored in a record's shape field and in library.idx. */
#define ART_KIND_UNKNOWN 0xFF

/**
 * @brief What a tile is when nothing has told us otherwise.
 *
 * Used under Automatic for a record with no cover, and for one whose cover has not been measured
 * yet. Not derived from the box measurements -- those are still what a *forced* table uses, and
 * they still say every cartridge box is portrait. This is a separate decision about the case
 * where there is no box to be right about.
 *
 * Landscape by request, and defensible on its own terms: a record that reaches this is either
 * artless, in which case the tile is a plate with a name on it and a wide plate holds more of a
 * name on one line, or it is a cover on its way in, in which case a short cell puts a whole
 * screenful of them in view rather than two rows.
 *
 * One constant, precisely so it is one edit back. What it costs is in the note on measure_cells()
 * in screen_grid.c.
 */
#define BOXART_FALLBACK_KIND ART_LANDSCAPE

/** @brief Longest section name kept, including the terminator. */
#define BOXART_NAME_CAP 16

/** @brief How many sections the picker will offer, the built-in table included. */
#define BOXART_REGIONS_MAX 8

/**
 * @brief Read `menu/boxart.ini` and select @p want, or the built-in table if it is absent.
 *
 * Safe to call with no card and with no file: the built-in table is always region 0 and is what
 * a fresh install gets.
 */
void boxart_init (const char *storage_prefix, const char *want);

/** @brief How many tables there are to choose between. At least one. */
int boxart_region_count (void);

/** @brief Name of table @p i, or "" out of range. Region 0 is the built-in, called "NTSC". */
const char *boxart_region_name (int i);

/** @brief Which table is in use. */
int boxart_region_current (void);

/** @brief Select table @p i and recompute every shape. Out-of-range indices are ignored. */
void boxart_set_region (int i);

/** @brief The tile for @p system, which is a @ref system_t. Unknown systems get the cartridge
 *         shape, because an unrecognised ROM on a card of cartridge games is one of those. */
art_shape_t boxart_shape (uint8_t system);

/** @brief The tallest shape in the current table. What an empty tab is pitched from. */
art_shape_t boxart_tallest (void);

/** @brief Tile for one of the three supported shapes. Out of range gives #ART_PORTRAIT. */
art_shape_t boxart_shape_at (int kind);

/**
 * @brief The largest rectangle of @p s's aspect that fits inside @p cell_w x @p cell_h.
 *
 * What a cell narrower than a cover's own column does with it, which is every shape but the
 * tallest in a mixed tab. Both dimensions are considered rather than just the width, so the
 * transient where a cover is measured after its tab was pitched -- and is briefly taller than the
 * row it lands in -- scales instead of squashing.
 */
art_shape_t boxart_fit_into (art_shape_t s, int cell_w, int cell_h);

/**
 * @brief Which of the three a source image of @p src_w x @p src_h is nearest.
 *
 * Nearest in log aspect rather than in linear aspect, because the three are not evenly spaced:
 * 0.702, 1.000 and 1.4286 are a geometric run, and a linear midpoint would put the boundary
 * between portrait and square at 0.851 -- which is 21 % away from portrait and 15 % from square,
 * so a cover exactly between them would be judged square. In log space the boundaries land at
 * 0.838 and 1.195, equidistant in the only sense that matters to a crop.
 *
 * @return an @ref art_kind_t, or #ART_KIND_UNKNOWN if the dimensions are not usable.
 */
uint8_t boxart_snap (int src_w, int src_h);

/** @brief Is the shape taken from the art, or forced from the region table? */
bool boxart_automatic (void);

#endif
