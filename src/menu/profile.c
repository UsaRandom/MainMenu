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
#include "menu/cart_load.h"
#include "menu/ini_parser.h"
#include "ui/icon.h"
#include "ui/theme.h"
#include "menu/paths.h"
#include "menu/profile.h"
#include "utils/fs.h"

#define PROFILE_FILE    "profiles.ini"

/** @brief Longest theme name, plus room. theme_by_name() falls back to Midnight on anything else. */
#define THEME_CAP       16

typedef struct {
    char     name[PROFILE_NAME_CAP];
    char     theme[THEME_CAP];
    uint16_t icon;      /**< pack index, or ICON_NONE */
    uint8_t  colour;    /**< the plate, an index into SWATCH */
    uint8_t  ink;       /**< the artwork on it, also an index into SWATCH */
    bool     used;      /**< slot 0 is always true; see profile_remove() */
} profile_t;

static profile_t roster[PROFILE_MAX];
static int  active;

/**
 * @brief The colours a profile can be. Both the plate and the artwork pick from this one list.
 *
 * The one place in this program where a colour is not a theme colour, and it is deliberate.
 * CLAUDE.md's rule is that no #define colour survives anywhere, because a theme that cannot
 * repaint the UI is not a theme. A profile's colours are not UI: they are how somebody finds
 * their own slot in a grid of ten, and a theme change that recoloured everybody's badge would
 * have changed who they are. So these are fixed, and the theme owns the chrome around them.
 *
 * The handoff's eight, plus the two neutrals it used as ink -- minus one. The eighth swatch was
 * `#E6E6DE`, a bone white, and it sat two places along from the `#F7F7FF` neutral. In RGBA5551
 * those quantise to (28,28,27) and (30,30,31): three levels apart out of 32, on a palette whose
 * whole job is to be told apart at a glance from across a room. Two near-identical whites is one
 * white and one bug report, so the bone one is gone and the pure one stayed.
 *
 * The table originally paired each swatch with a fixed ink -- light plates took the dark neutral,
 * dark plates took the light one -- which is a sensible default and a poor rule: it meant a
 * profile had one colour, and there was no way to ask for white-on-red rather than the red-on-white
 * the table had decided. The pairing survives as #profile_default_ink, which is what an existing
 * card and a new slot both get.
 */
static const uint16_t SWATCH[PROFILE_COLOURS] = {
    RGBA5551(0xDE, 0x21, 0x31),   /* 0 red    -- dark */
    RGBA5551(0xF7, 0xB5, 0x21),   /* 1 amber  -- light */
    RGBA5551(0x42, 0xBD, 0x63),   /* 2 green  -- light */
    RGBA5551(0x29, 0x63, 0xCE),   /* 3 blue   -- dark */
    RGBA5551(0x9C, 0x42, 0xCE),   /* 4 purple -- dark */
    RGBA5551(0x21, 0xB5, 0xC5),   /* 5 cyan   -- light */
    RGBA5551(0xF7, 0x7B, 0xAD),   /* 6 pink   -- light */
    RGBA5551(0x10, 0x10, 0x19),   /* 7 black  -- the dark neutral */
    RGBA5551(0xF7, 0xF7, 0xFF),   /* 8 white  -- the light neutral */
};

/**
 * @brief What each swatch is called, for the row that says which one is chosen.
 *
 * A ring around a rectangle says *that* one is selected; it does not say which one that is when
 * the two either side are also blue-ish and the room is lit. The name is the unambiguous answer,
 * and it costs nine short strings.
 */
static const char *const SWATCH_NAME[PROFILE_COLOURS] = {
    "Red", "Amber", "Green", "Blue", "Purple", "Cyan", "Pink", "Black", "White",
};

/** Which swatches are dark enough to need light artwork on them. Bit N = swatch N. */
#define DARK_PLATES  ((1u << 0) | (1u << 3) | (1u << 4) | (1u << 7))

int profile_default_ink (int plate) {
    if (plate < 0 || plate >= PROFILE_COLOURS) {
        plate = 0;
    }
    return (DARK_PLATES & (1u << plate)) ? PROFILE_COLOUR_PAPER : PROFILE_COLOUR_INK;
}static char file_path[300];
static char storage[240];

/**
 * The "Player N" fallback, rebuilt on demand rather than stored. Storing it would mean a user who
 * never named themselves has a name on disk, and then renaming profile 2 to nothing would leave
 * "Player 2" written down as if they had chosen it. One buffer: no caller holds two at once.
 *
 * Deliberately **not** PROFILE_NAME_CAP. That is nine bytes -- eight characters and a terminator,
 * which is what the keyboard lets anybody type -- and "Player 10" is nine characters. snprintf
 * truncated it to "Player 1", so slot 10 and slot 1 showed the same name on the screen whose whole
 * job is telling ten people apart. The cap governs typed names; this is neither typed nor stored
 * and had no business sharing it. #PROFILE_LABEL_CAP is what the layout reserves room for.
 */
static char fallback[PROFILE_LABEL_CAP];

/** A slot that exists and is in use. Was `index < count`, which meant something different when
 *  the roster was contiguous; now the flags decide and a hole is out of range. */
static bool in_range (int index) {
    return (index >= 0 && index < PROFILE_MAX) && (index == 0 || roster[index].used);
}

/* Slot 0 always counts, even before anything has been written -- see profile.h. */
int profile_count (void) {
    int n = 0;
    for (int i = 0; i < PROFILE_MAX; i++) {
        if (roster[i].used) {
            n++;
        }
    }
    return (n > 0) ? n : 1;
}

bool profile_slot_used (int index) {
    if (index < 0 || index >= PROFILE_MAX) {
        return false;
    }
    return (index == 0) || roster[index].used;
}

uint16_t profile_icon (int index) {
    return in_range(index) ? roster[index].icon : ICON_NONE;
}

void profile_set_icon (int index, uint16_t icon) {
    if (in_range(index)) {
        roster[index].icon = icon;
    }
}

int profile_plate (int index) {
    return in_range(index) ? roster[index].colour : 0;
}

void profile_set_plate (int index, int colour) {
    if (in_range(index) && colour >= 0 && colour < PROFILE_COLOURS) {
        roster[index].colour = (uint8_t)colour;
    }
}

int profile_ink (int index) {
    return in_range(index) ? roster[index].ink : PROFILE_COLOUR_PAPER;
}

void profile_set_ink (int index, int colour) {
    if (in_range(index) && colour >= 0 && colour < PROFILE_COLOURS) {
        roster[index].ink = (uint8_t)colour;
    }
}

uint16_t profile_colour_fill (int colour) {
    if (colour < 0 || colour >= PROFILE_COLOURS) {
        colour = 0;
    }
    return SWATCH[colour];
}

const char *profile_colour_name (int colour) {
    if (colour < 0 || colour >= PROFILE_COLOURS) {
        return "";
    }
    return SWATCH_NAME[colour];
}

int profile_appearance_owner (uint16_t icon, int plate, int ink, int except) {
    /* An unchosen icon is not an appearance, so ten unconfigured slots do not all collide with
     * each other and make the first edit impossible. */
    if (icon == ICON_NONE) {
        return -1;
    }
    for (int i = 0; i < PROFILE_MAX; i++) {
        if (i == except || !profile_slot_used(i)) {
            continue;
        }
        if (roster[i].icon == icon && roster[i].colour == plate && roster[i].ink == ink) {
            return i;
        }
    }
    return -1;
}


int profile_active (void) {
    return active;
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

    /* Version 1 had no version key and no holes: `count` profiles occupied slots 0..count-1, and
     * deleting one closed the gap by moving every profile above it down a slot. That renumbering
     * is what this format exists to stop, because the slot number names the folder on disk --
     * deleting player 2 made player 3's `saves/p3/` into player 2's, so the saves followed the
     * slot rather than the person.
     *
     * Reading version 1 needs no renumbering, which is the whole reason the old layout can be
     * adopted rather than converted: contiguous slots 0..count-1 ARE the new format with the
     * first `count` slots marked used. Nothing moves, so no card can lose a save by being read
     * by this build. The file is rewritten with a version key the next time anything changes.
     */
    int version = ini_get_int(ini, "profiles", "version", 1);
    int legacy_count = ini_get_int(ini, "profiles", "count", 1);
    if (legacy_count < 1) {
        legacy_count = 1;
    }
    if (legacy_count > PROFILE_MAX) {
        legacy_count = PROFILE_MAX;
    }

    for (int i = 0; i < PROFILE_MAX; i++) {
        char section[8];
        snprintf(section, sizeof(section), "p%d", i + 1);

        if (version >= PROFILE_VERSION) {
            roster[i].used = ini_get_int(ini, section, "used", 0) != 0;
        } else {
            roster[i].used = (i < legacy_count);
        }
        if (!roster[i].used) {
            continue;
        }
        profile_set_name(i, ini_get_string(ini, section, "name", ""));
        snprintf(roster[i].theme, sizeof(roster[i].theme), "%s",
                 ini_get_string(ini, section, "theme", ""));

        int icon = ini_get_int(ini, section, "icon", ICON_NONE);
        roster[i].icon = (icon >= 0 && icon < ICON_NONE) ? (uint16_t)icon : ICON_NONE;
        if (roster[i].icon == ICON_NONE) {
            /* Nobody starts as a blank plate. Every card that predates this reaches here with no
             * icon key at all, including the single unnamed profile on a card that predates
             * profiles -- so this is the path most consoles take, not an edge case.
             *
             * By slot, so the same slot always comes back the same face and a family recognises
             * their own row. Resolved from the metadata rather than hardcoded, because a capped
             * build does not contain the icon a hardcoded index would name. */
            roster[i].icon = icon_starter(i);
        }
        int colour = ini_get_int(ini, section, "colour", i % PROFILE_PLATES);
        roster[i].colour = (uint8_t)((colour >= 0 && colour < PROFILE_COLOURS)
                                     ? colour : (i % PROFILE_PLATES));
        /* Absent on every card written before the plate and the artwork could differ -- which is
         * all of them. The default is the pairing that used to be hardcoded in the swatch table,
         * so an upgraded card looks exactly as it did and the choice becomes available rather
         * than being made for the user. */
        int ink = ini_get_int(ini, section, "ink", -1);
        roster[i].ink = (uint8_t)((ink >= 0 && ink < PROFILE_COLOURS)
                                  ? ink : profile_default_ink(roster[i].colour));
    }

    /* There is always a player one. A file that somehow says otherwise is not believed: profile 1
     * owns the unsuffixed paths and a card with nobody in slot 0 would write every save into a
     * `pN/` folder while every card ever made keeps them at the top. */
    roster[0].used = true;

    active = ini_get_int(ini, "profiles", "active", 0);
    /* Clamped, not trusted. An index at an empty slot would send every path through `pN/` for a
     * profile that does not exist -- favourites into a folder nothing ever reads back. */
    if (active < 0 || active >= PROFILE_MAX || !roster[active].used) {
        active = 0;
    }

    ini_free(ini);

    ensure_cache_dir();
    debugf("PROFILE %d of %d active (%s), format v%d\n",
           active + 1, profile_count(), profile_name(active), version);
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

    ini_set_int(ini, "profiles", "version", PROFILE_VERSION);
    /* Still written, and still meaning what it meant: how many profiles exist. A version 1 build
     * reading this file gets a roster of the right size, with holes closed up -- which is wrong
     * about who owns which slot, but is the failure of an old build reading a new file and not
     * something this one can prevent. Omitting the key would make that build see one profile. */
    ini_set_int(ini, "profiles", "count", profile_count());
    ini_set_int(ini, "profiles", "active", active);

    for (int i = 0; i < PROFILE_MAX; i++) {
        char section[8];
        snprintf(section, sizeof(section), "p%d", i + 1);
        ini_set_int(ini, section, "used", roster[i].used ? 1 : 0);
        if (!roster[i].used) {
            continue;
        }
        ini_set_string(ini, section, "name", roster[i].name);
        ini_set_string(ini, section, "theme", roster[i].theme);
        ini_set_int(ini, section, "icon", (int)roster[i].icon);
        ini_set_int(ini, section, "colour", (int)roster[i].colour);
        ini_set_int(ini, section, "ink", (int)roster[i].ink);
    }

    ini_save(ini, file_path);
    ini_free(ini);
}

/* ------------------------------------------------------------------ the roster */

bool profile_add_at (int slot) {
    if (slot < 0 || slot >= PROFILE_MAX || roster[slot].used) {
        return false;
    }
    roster[slot].used = true;
    roster[slot].name[0] = '\0';
    roster[slot].theme[0] = '\0';
    roster[slot].icon = icon_starter(slot);
    /* Only the hues, never the two neutrals: a new slot should look like a player, and a black or
     * white plate reads as an empty one. Both are still choosable. */
    roster[slot].colour = (uint8_t)(slot % PROFILE_PLATES);
    roster[slot].ink = (uint8_t)profile_default_ink(roster[slot].colour);
    profile_save();
    return true;
}

int profile_add (void) {
    /* The lowest free slot, not the next one along. Slots are stable now, so a roster with a hole
     * in the middle has a hole in the middle -- and filling it is what the picker's "+ Empty" card
     * is offering when it sits there. Appending past it would leave a gap nothing could ever use
     * and would run out of slots with four players on the card. */
    for (int i = 0; i < PROFILE_MAX; i++) {
        if (profile_add_at(i)) {
            return i;
        }
    }
    return -1;
}

bool profile_remove (int index) {
    /* Profile 1 owns the unsuffixed paths -- `cache/playstate.dat` and `<romdir>/saves/` -- and
     * something has to. Removing it would either strand those files or promote profile 2 into
     * them, which is the one renumbering that silently hands somebody else's saves to a player. */
    if (index <= 0 || index >= PROFILE_MAX || !roster[index].used) {
        return false;
    }

    /* The profile's own bookkeeping. Its saves are gone too, but by then: profile_erase_saves()
     * is a separate call the screen makes after confirming, so the irreversible half is explicit
     * at the call site rather than a side effect of removing a name from a list. */
    int was = active;
    active = index;
    char name[64];
    profile_cache_name(name, sizeof(name), "playstate.dat");
    cache_drop(name);
    profile_cache_name(name, sizeof(name), "cheatstate.dat");
    cache_drop(name);
    active = was;

    /* Cleared in place. Nothing above it moves, and that is the point of the format change: the
     * slot number names `saves/pN/` and `cache/pN/`, so closing the gap used to hand player 3's
     * folders to player 2. Now slot 2 stays empty until somebody fills it, and when they do they
     * get a folder with nothing in it, because profile_erase_saves() ran first. */
    memset(&roster[index], 0, sizeof(roster[index]));
    roster[index].icon = ICON_NONE;
    roster[index].ink = (uint8_t)PROFILE_COLOUR_PAPER;

    /* Only the deleted player moves, and only if it was them. Everyone else keeps their slot,
     * their folder and their saves. */
    if (active == index) {
        active = 0;
    }

    profile_save();
    return true;
}

int profile_erase_saves (int index, const library_t *lib, void (*tick)(void)) {
    if (index <= 0 || index >= PROFILE_MAX || lib == NULL) {
        return 0;
    }

    char sub[8];
    snprintf(sub, sizeof(sub), "p%d", index + 1);

    int removed = 0;
    for (int i = 0; i < lib->count; i++) {
        /* Once per record, before the probe: most records cost exactly one failed
         * dir_findfirst(), and on the console that lone miss is still a card round-trip. */
        if (tick != NULL) {
            tick();
        }
        const char *path = lib->records[i].path;
        if (path == NULL) {
            continue;
        }
        /* The directory the ROM is in. Saves live beside their game rather than in one place on
         * the card, so the set of places to look is the set of directories the library already
         * walked -- which is why this takes the library rather than walking the card again. A
         * second walk would be slow, and it would also find `saves/` trees under directories no
         * game is in, which is exactly the kind of guess not to make when deleting. */
        const char *slash = strrchr(path, '/');
        if (slash == NULL) {
            continue;
        }
        size_t dir_len = (size_t)(slash - path);

        char dir[300];
        if (dir_len + 1 + strlen(SAVE_DIRECTORY_NAME) + 1 + strlen(sub) + 1 > sizeof(dir)) {
            continue;
        }
        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';

        char folder[320];
        snprintf(folder, sizeof(folder), "%s/%s/%s", dir, SAVE_DIRECTORY_NAME, sub);

        /* Deduplicated by trying the same folder twice being harmless: the second attempt finds
         * nothing to delete and adds nothing to the count. Ten games in one directory means ten
         * attempts at one folder, which is cheaper than the set that would avoid them. */
        removed += directory_erase(folder, tick);
    }
    return removed;
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
