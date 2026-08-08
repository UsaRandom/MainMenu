/**
 * @file diagreport.h
 * @brief A page of text for a person to photograph, on the one screen that cannot lose it.
 * @ingroup menu
 *
 * ## Why this exists
 *
 * The cheat investigation has now lost five hardware runs to its own reporting, not to the thing
 * being measured:
 *
 *   - the in-game bar needed the engine to run, which was the question;
 *   - the handoff flash needed a display `boot.c` had blanked, and its ten-second hold made every
 *     launch look like a black screen (AUDIT 1aq, 1ar);
 *   - `launch.log` and the cartridge dumps came back missing, twice, with no way to tell whether
 *     they had even been attempted;
 *   - and the report page bolted onto `screen_launch.c` failed three separate ways -- drawn to a
 *     surface `draw_fade_into()` had already shown, dismissed on its first frame by an edge that
 *     an extra mid-frame `joypad_poll()` had manufactured, and never once looked at under ares
 *     because the emulator path returned before drawing it.
 *
 * Every one of those was a channel invented for the occasion. So this one is not: the lines go to
 * `screen_fault.c`, which is a render-only screen with no update function, no input handling and
 * no way to be dismissed, whose file comment already says "designed to be read off a phone
 * photo". It draws on the theme background in the theme's own text colour, so contrast is
 * guaranteed by the same rule every other screen relies on.
 *
 * The console stops there. That is the point -- a diagnostic that continues is a diagnostic that
 * can be missed.
 */

#ifndef DIAGREPORT_H__
#define DIAGREPORT_H__

/** @brief How many lines the fault screen has room for at 24 px with a heading. */
#define DIAG_REPORT_MAX 12

/** @brief Append a line. Silently ignored past #DIAG_REPORT_MAX, because a diagnostic must not
 *         be the thing that crashes. Also mirrored to launchlog, when that happens to work. */
void diag_reportf (const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** @brief How many lines are waiting; zero means the fault screen is a real fault. */
int diag_report_count (void);

/** @brief Line @p i, or "" out of range. */
const char *diag_report_line (int i);

#endif /* DIAGREPORT_H__ */
