/**
 * @file paths.h
 * @brief The one folder the menu owns, and how everything else on the card is found.
 * @ingroup menu
 *
 * ## Two different questions
 *
 * "Where do I write my own state" and "where did the user put their cheat database" are not the
 * same question, and answering them with one constant is what made `/menu` mean both. State the
 * menu writes goes in exactly one place and is not looked for anywhere else -- if it is not there
 * it did not exist, and every one of those files rebuilds from the card. Content the *user* placed
 * is looked for in several, because a person emptying a zip onto a card should not have to know
 * which folder we prefer.
 *
 * ## Why the folder was renamed
 *
 * `/menu` said what it was for back when it was also where content lived. A folder the menu owns
 * outright should say whose it is, because the one time a user opens it on a computer is when they
 * are deleting something -- `parental.ini` to recover a forgotten code, or the whole folder to
 * make the menu forget everything it learned. `/mainmenu` next to their own folders is obvious;
 * `/menu` next to their own folders is one more thing they did not put there.
 *
 * Nothing migrates. Config and caches are all derivable from the card, so a card that had a
 * `/menu` folder simply rebuilds into `/mainmenu` on first boot and the old one is left alone --
 * it is the user's card and deleting folders on it is not ours to do. Content is still *read* from
 * `/menu`, third in the probe order, so an existing card keeps working with no action at all.
 */

#ifndef MENU_PATHS_H__
#define MENU_PATHS_H__

#include <stdbool.h>
#include <stddef.h>

/** @brief The folder the menu writes into. Everything here is the menu's, and is disposable. */
#define MENU_DIR            "/mainmenu"

/** @brief What that folder used to be called. Read for content, never written. */
#define MENU_DIR_LEGACY     "/menu"

/**
 * @brief Build `<storage>/mainmenu/<sub>` into @p out.
 *
 * @p sub may be several components ("cache/thumbs.pak") or NULL for the folder itself. The
 * storage prefix already ends in a slash, and concatenating naively gives `sd://mainmenu`, which
 * FatFs has never been asked to accept -- see AUDIT.md 1n. Stripped here rather than at each of
 * the six call sites that used to do it themselves, or not.
 */
void menu_path (char *out, size_t cap, const char *storage, const char *sub);

/**
 * @brief Find a file the user placed: `/mainmenu/<leaf>`, then `/<leaf>`, then `/menu/<leaf>`.
 *
 * Returns false with @p out holding the first candidate, so a caller that wants to report where
 * it looked has something to name. Three `fopen`s at worst, once at launch.
 */
bool menu_find_file (char *out, size_t cap, const char *storage, const char *leaf);

/** @brief The same probe for a directory. See menu_find_file(). */
bool menu_find_dir (char *out, size_t cap, const char *storage, const char *leaf);

#endif /* MENU_PATHS_H__ */
