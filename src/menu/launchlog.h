/**
 * @file launchlog.h
 * @brief A few lines on the card about the last launch, because USB is not available.
 * @ingroup menu
 *
 * `debugf()` reaches a PC over the SC64's USB link, which is how everything in this program has
 * been diagnosed so far. That is not universally available: the cart in use here has a USB port
 * that does not enumerate, and even a working one cannot see the interesting part -- the cheat
 * engine installs inside `boot()`, after the display is closed and the filesystem is unmounted,
 * microseconds before jumping into the game.
 *
 * So the launch path writes what it did to `/mainmenu/launch.log` while it still can. The file is
 * short, overwritten each launch rather than appended, and reports exactly the things that cannot
 * be seen from the outside: which ROM, which CIC, whether the cheat engine could hook it, and how
 * many cheat words were emitted.
 *
 * ## Not a cache and not a feature
 *
 * It goes through `ini_save`-style plain stdio rather than `cache.c`, because it is not derived
 * data and has no format to version -- it is a paragraph of text for a person to read. Every
 * write is best-effort and its failure is ignored: a card that cannot be written must launch
 * games exactly as it does now. Nothing ever reads this file back.
 */

#ifndef MENU_LAUNCHLOG_H__
#define MENU_LAUNCHLOG_H__

/** @brief Remember where to write. Call once, at boot, with the storage prefix. */
void launchlog_init (const char *storage_prefix);

/**
 * @brief Start a fresh log, discarding the previous launch's. Best-effort; failure is silent.
 *
 * Open/append/close rather than one formatted call, because the interesting part of the report is
 * the list of code lines actually handed to the engine and that is a variable number of them. The
 * alternative was assembling the whole thing in a buffer first, which means choosing a buffer size
 * and then silently truncating the evidence at it.
 */
void launchlog_begin (void);

/** @brief One line. Newline is added. No-op unless launchlog_begin() opened a file. */
void launchlog_line (const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** @brief Flush and close. Safe to call when nothing was opened. */
void launchlog_end (void);

#endif /* MENU_LAUNCHLOG_H__ */
