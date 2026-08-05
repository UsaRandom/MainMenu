/**
 * @file profile.c
 * @brief Who is playing. See profile.h for what a profile is and where the split falls.
 * @ingroup menu
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "cheats/cheatstate.h"
#include "library/cache.h"
#include "library/locks.h"
#include "library/playstate.h"
#include "menu/ini_parser.h"
#include "menu/paths.h"
#include "menu/profile.h"
#include "utils/fs.h"

#define PROFILE_FILE    "profiles.ini"

/** @brief Longest theme name, plus room. theme_by_name() falls back to Midnight on anything else. */
#define THEME_CAP       16

typedef struct {
    char name[PROFILE_NAME_CAP];
    char theme[THEME_CAP];
} profile_t;

static profile_t roster[PROFILE_MAX];
static int  count = 1;
static int  active;
static char file_path[300];
static char storage[240];

/** The "Player N" fallback, rebuilt on demand rather than stored. Storing it would mean a user who
 *  never named themselves has a name on disk, and then renaming profile 2 to nothing would leave
 *  "Player 2" written down as if they had chosen it. One buffer: no caller holds two at once. */
static char fallback[PROFILE_NAME_CAP];

int profile_count (void) {
    return count;
}

int profile_active (void) {
    return active;
}

static bool in_range (int index) {
    return (index >= 0 && index < count);
}

const char *profile_name_raw (int index) {
    return in_range(index) ? roster[index].name : "";
}

const char *profile_name (int index) {
    if (!in_range(index)) {
        return "?";
    }
    if (roster[index].name[0] != '\0') {
        return roster[index].name;
    }
    snprintf(fallback, sizeof(fallback), "Player %d", index + 1);
    return fallback;
}

void profile_set_name (int index, const char *name) {
    if (!in_range(index)) {
        return;
    }
    snprintf(roster[index].name, sizeof(roster[index].name), "%s", name ? name : "");

    /* Trailing spaces are what the name editor produces every time somebody backs a character off
     * the end, and a name of nothing but spaces is not empty by strcmp but is blank on screen --
     * so it would suppress the "Player N" fallback and draw an empty chip. Trimmed here rather
     * than in the editor, because profile_set_name() is the only way in. */
    char *s = roster[index].name;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') {
        s[--n] = '\0';
    }
}

const char *profile_theme (int index) {
    return in_range(index) ? roster[index].theme : "";
}

void profile_set_theme (int index, const char *name) {
    if (!in_range(index)) {
        return;
    }
    snprintf(roster[index].theme, sizeof(roster[index].theme), "%s", name ? name : "");
}

/* ------------------------------------------------------------------ paths */

const char *profile_save_subdir (void) {
    /* Rebuilt into a static rather than a table of ten strings, because it is asked for once per
     * launch. Profile 1 gets NULL and writes where every card already writes. */
    static char sub[8];
    if (active == 0) {
        return NULL;
    }
    snprintf(sub, sizeof(sub), "p%d", active + 1);
    return sub;
}

void profile_cache_name (char *out, size_t cap, const char *name) {
    if (active == 0) {
        snprintf(out, cap, "%s", name);
    } else {
        snprintf(out, cap, "p%d/%s", active + 1, name);
    }
}

/** @brief Make sure `cache/pN` exists, since cache_store() only opens files. */
static void ensure_cache_dir (void) {
    if (active == 0 || !cache_writable()) {
        return;
    }
    char sub[8];
    snprintf(sub, sizeof(sub), "p%d", active + 1);

    char dir[300];
    cache_path(dir, sizeof(dir), sub);
    directory_create(dir);
}

/* ------------------------------------------------------------------ the file */

void profile_load (const char *storage_prefix) {
    snprintf(storage, sizeof(storage), "%s", storage_prefix);
    menu_path(file_path, sizeof(file_path), storage_prefix, PROFILE_FILE);

    /* Deliberately not cache_load(), for the same reason parental.ini is not: a cache with a
     * version mismatch is deleted and rebuilt from the card, and there is nothing on the card to
     * rebuild a roster of names from. A format bump must not silently merge a family back into
     * one profile -- and it would, because profile 1 owns the unsuffixed paths, so everyone would
     * come back as each other. */
    ini_t *ini = ini_try_load(file_path);

    count = ini_get_int(ini, "profiles", "count", 1);
    if (count < 1) {
        count = 1;
    }
    if (count > PROFILE_MAX) {
        count = PROFILE_MAX;
    }

    for (int i = 0; i < count; i++) {
        char section[8];
        snprintf(section, sizeof(section), "p%d", i + 1);
        profile_set_name(i, ini_get_string(ini, section, "name", ""));
        snprintf(roster[i].theme, sizeof(roster[i].theme), "%s",
                 ini_get_string(ini, section, "theme", ""));
    }

    active = ini_get_int(ini, "profiles", "active", 0);
    /* Clamped, not trusted. An index past the roster would send every path through `pN/` for a
     * profile that does not exist -- favourites into a folder nothing ever reads back. */
    if (active < 0 || active >= count) {
        active = 0;
    }

    ini_free(ini);

    ensure_cache_dir();
    debugf("PROFILE %d of %d active (%s)\n", active + 1, count, profile_name(active));
}

void profile_save (void) {
    /* The folder, first. profiles.ini is the only file the menu writes that is not a cache and
     * not reached through cache.c, so nothing else creates its directory -- it worked at all only
     * because cache_init() happens to create `/mainmenu/cache`, and therefore `/mainmenu`, before
     * any screen can reach this. That is an ordering dependency, not a guarantee, and the host
     * test caught it by calling profile_save() without cache_init(): the roster silently failed
     * to persist and every name came back as "Player N". */
    char dir[300];
    menu_path(dir, sizeof(dir), storage, NULL);
    directory_create(dir);

    ini_t *ini = ini_create();

    ini_set_int(ini, "profiles", "count", count);
    ini_set_int(ini, "profiles", "active", active);

    for (int i = 0; i < count; i++) {
        char section[8];
        snprintf(section, sizeof(section), "p%d", i + 1);
        ini_set_string(ini, section, "name", roster[i].name);
        ini_set_string(ini, section, "theme", roster[i].theme);
    }

    ini_save(ini, file_path);
    ini_free(ini);
}

/* ------------------------------------------------------------------ the roster */

int profile_add (void) {
    if (count >= PROFILE_MAX) {
        return -1;
    }
    int index = count++;
    roster[index].name[0] = '\0';
    roster[index].theme[0] = '\0';
    profile_save();
    return index;
}

bool profile_remove (int index) {
    /* Profile 1 owns the unsuffixed paths -- `cache/playstate.dat` and `<romdir>/saves/` -- and
     * something has to. Removing it would either strand those files or promote profile 2 into
     * them, which is the one renumbering that silently hands somebody else's saves to a player. */
    if (index <= 0 || index >= count || count <= 1) {
        return false;
    }

    /* The profile's own bookkeeping goes; its saves do not. See profile.h on why walking the card
     * to find them is not worth the one irreversible mistake it could make. */
    int was = active;
    active = index;
    char name[64];
    profile_cache_name(name, sizeof(name), "playstate.dat");
    cache_drop(name);
    profile_cache_name(name, sizeof(name), "cheatstate.dat");
    cache_drop(name);
    active = was;

    for (int i = index; i < count - 1; i++) {
        roster[i] = roster[i + 1];
    }
    count--;

    /* Everything above the hole moved down a slot, and the slot number is what names the folders.
     * The active profile has to follow its own data, and a player who was the one deleted lands
     * back on profile 1 rather than on whoever inherited their number. */
    if (active == index) {
        active = 0;
    } else if (active > index) {
        active--;
    }

    profile_save();
    return true;
}

void profile_activate (int index, library_t *lib) {
    if (!in_range(index)) {
        return;
    }

    /* Flush before switching, not after. Everything below reloads over the top of the outgoing
     * profile's state, so anything unwritten at this moment is gone -- and switching profile is
     * exactly the moment a player stops touching the menu, which is to say the moment their
     * favourites are most likely to be one press old and unsaved. */
    if (lib != NULL && playstate_dirty()) {
        playstate_save(lib);
    }
    if (cheatstate_dirty()) {
        cheatstate_save();
    }

    active = index;
    ensure_cache_dir();
    profile_save();

    if (lib != NULL) {
        /* Cleared, then reloaded. playstate_load() ORs its flags in and assigns its counters, so
         * without the clear a favourite would survive into a profile that never set it -- the
         * previous player's list would accumulate into everybody else's. LIBF_LOCKED is not
         * touched: it is the parental lock, it is shared, and profile switching must never be a
         * way around it. See locks.h. */
        for (int i = 0; i < lib->count; i++) {
            lib->records[i].flags &= ~LIBF_FAVORITE;
            lib->records[i].last_played = 0;
            lib->records[i].play_count = 0;
        }
        playstate_load(lib);
    }

    cheatstate_load();
    debugf("PROFILE switched to %d (%s)\n", active + 1, profile_name(active));
}
