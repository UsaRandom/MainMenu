/**
 * @file cardstat.h
 * @brief What is wrong with this card, in words a person can act on.
 * @ingroup menu
 *
 * launch.log already records the numbers. This is the other half: the boot plate and Settings
 * say the same facts in English, because a log on the card is not something you can read from
 * the sofa.
 *
 * A clean 8 MB card that can be written produces no plate lines at all. The plate is a splash,
 * not a diagnostic dump. The rest lives behind Settings ▸ System info, which is the page you
 * photograph when something is wrong.
 *
 * Read-only is only a warning on real storage (`sd:`). Under ares the prefix is `rom:` and
 * that is not a locked card, it is the emulator; saying "every start will be slow" there is
 * a lie about a situation the user cannot fix.
 */

#ifndef MENU_CARDSTAT_H__
#define MENU_CARDSTAT_H__

#include "library/library.h"

/** @brief Files in one folder before Settings and the plate mention it.
 *
 * The card that prompted this had 556 entries in one directory and cost about 0.7 s per title.
 * Splitting by initial letter took that to about 21. Eighty is four times a tidy letter folder
 * and well below the flat-shelf case, so it fires when splitting would actually help. */
#define CARDSTAT_BUSY_WARN  80

/** @brief Remember where the card is and which library is filling.
 *
 * Call once both exist. The library pointer is borrowed; busiest-folder updates during the
 * scan are visible on the next plate paint without another call. */
void cardstat_bind (const char *storage, const library_t *lib);

/** @brief Probe the six cores. Cheap: three stats each, once. */
void cardstat_probe_cores (void);

/** @brief What the index and the mixer did this boot. Call after sound_worst_gap_us(). */
void cardstat_set_boot (bool idx_hit, bool repaired,
                        uint32_t idx_us, uint32_t scan_us,
                        unsigned gap_us, unsigned slack_us);

/** @brief Warning lines for the plate, most urgent first. 0 if nothing is wrong. */
int cardstat_plate (char out[][72], int cap);

/** @brief One Settings row. */
typedef struct {
    char label[20];
    char value[72];
} cardstat_row_t;

/** @brief Every fact the System info screen shows. At least Card. */
int cardstat_info (cardstat_row_t *out, int cap);

/** @brief The same facts, one launch.log line each. */
void cardstat_log (void);

#endif /* MENU_CARDSTAT_H__ */
