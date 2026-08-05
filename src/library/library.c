/**
 * @file library.c
 * @brief The game index the grid draws from.
 * @ingroup library
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "library.h"
#include "menu/path.h"

#define LIB_INITIAL_CAP 64

/** Five, not four. The scan root moved from `/roms` to `/`, so everything on the card is one
 *  level further down than it was; four here would have quietly shortened the reach of a card
 *  organised exactly as before. */
#define SCAN_MAX_DEPTH  5

/**
 * @brief Directory names the scan will not walk into, at any depth.
 *
 * The scan starts at the card root now, so it has to be told what is not content. Everything here
 * is a directory that either belongs to the menu or belongs to the computer the card was last
 * plugged into, and walking any of them costs time and finds nothing.
 *
 * - `mainmenu`, `menu` -- ours, and its old name. `mainmenu/cache` alone can hold hundreds of
 *   files, none of which is a game.
 * - `metadata` -- an art pack, which is four levels of single-letter directories and one PNG per
 *   game, every one of them named `boxart_front.png`. Walking it would push several hundred
 *   identically-named images into the loose-art table, where the name is the key.
 * - `emulators` -- and this one is not an optimisation. `neon64bu.rom` is a core, and `.rom` is an
 *   N64 extension, so a card with its cores at `/emulators` would list the NES emulator in the N64
 *   tab as a game. Rooting the scan at `/` is what created that; under `/roms` the cores were
 *   never in reach.
 * - `saves` -- created beside every ROM the moment one is launched, so this is the exclusion that
 *   fires most often. `.sav` is not a ROM extension so nothing would be indexed from it either
 *   way; skipping the directory saves the walk.
 * - `System Volume Information` -- Windows. The dotted litter (`.Spotlight-V100`, `.Trashes`,
 *   `.fseventsd`, and AppleDouble `._*` files, which carry the real file's extension and would
 *   otherwise index as a second copy of every game) is already skipped by the leading-dot test in
 *   scan_dir().
 */
static const char *SCAN_SKIP[] = {
    "mainmenu", "menu", "metadata", "emulators", "saves", "System Volume Information",
};

/**
 * @brief Files that are never games, however much their extension says otherwise.
 *
 * A flashcart menu lives at the card root under a fixed name, and `sc64menu.n64` ends in `.n64`.
 * Scanning from `/` put the menu's own ROM in reach for the first time, so without this the first
 * thing a user sees in their N64 tab is this program, offering to boot itself. The other two names
 * are the ED64 and 64drive equivalents: this fork does not support either cart, but a card that
 * has been used with one still has the file sitting at its root.
 */
static const char *SCAN_SKIP_FILES[] = {
    "sc64menu.n64", "menu.bin", "OS64.v64", "OS64P.v64",
};

static const char *TAB_LABELS[TAB_COUNT] = {
    "RECENT", "FAVORITES",
    "N64", "NES", "SNES", "GB", "GBC", "SMS",
};

const char *library_tab_label (tab_t tab) {
    return (tab >= 0 && tab < TAB_COUNT) ? TAB_LABELS[tab] : "?";
}

library_t *library_init (void) {
    library_t *lib = calloc(1, sizeof(library_t));
    if (lib == NULL) {
        return NULL;
    }
    lib->records = calloc(LIB_INITIAL_CAP, sizeof(lib_record_t));
    if (lib->records == NULL) {
        free(lib);
        return NULL;
    }
    lib->capacity = LIB_INITIAL_CAP;
    return lib;
}

void library_free (library_t *lib) {
    if (lib == NULL) {
        return;
    }
    for (int i = 0; i < lib->count; i++) {
        free(lib->records[i].path);
        free(lib->records[i].title);
        free(lib->records[i].art_file);
    }
    for (int i = 0; i < lib->art_count; i++) {
        free(lib->art[i].key);
        free(lib->art[i].path);
    }
    free(lib->art);
    free(lib->records);
    free(lib);
}

/** @brief Lowercase @p name with its extension stripped, into a fresh allocation. */
static char *art_key_from_name (const char *name) {
    const char *dot = strrchr(name, '.');
    size_t n = (dot != NULL) ? (size_t)(dot - name) : strlen(name);
    if (n == 0) {
        return NULL;
    }
    char *key = malloc(n + 1);
    if (key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        key[i] = (char)tolower((unsigned char)name[i]);
    }
    key[n] = '\0';
    return key;
}

/** @brief Remember a loose PNG. Duplicates keep the first seen, so the shallowest wins. */
static void art_push (library_t *lib, const char *name, const char *full_path) {
    char *key = art_key_from_name(name);
    if (key == NULL) {
        return;
    }
    for (int i = 0; i < lib->art_count; i++) {
        if (strcmp(lib->art[i].key, key) == 0) {
            free(key);
            return;
        }
    }
    if (lib->art_count == lib->art_capacity) {
        int cap = (lib->art_capacity == 0) ? 32 : lib->art_capacity * 2;
        art_entry_t *grown = realloc(lib->art, cap * sizeof(art_entry_t));
        if (grown == NULL) {
            free(key);
            return;
        }
        lib->art = grown;
        lib->art_capacity = cap;
    }
    char *path = strdup(full_path);
    if (path == NULL) {
        free(key);
        return;
    }
    lib->art[lib->art_count].key = key;
    lib->art[lib->art_count].path = path;
    lib->art_count++;
}

const char *library_find_art (const library_t *lib, const char *name) {
    if (lib == NULL || name == NULL || name[0] == '\0') {
        return NULL;
    }
    char *key = art_key_from_name(name);
    if (key == NULL) {
        return NULL;
    }
    const char *found = NULL;
    for (int i = 0; i < lib->art_count; i++) {
        if (strcmp(lib->art[i].key, key) == 0) {
            found = lib->art[i].path;
            break;
        }
    }
    free(key);
    return found;
}

/** @brief True when @p name looks like art: .png, .jpg or .jpeg, case-insensitively. */
static bool is_image (const char *name) {
    size_t n = strlen(name);
    return (n > 4 && strcasecmp(name + n - 4, ".png") == 0) ||
           (n > 4 && strcasecmp(name + n - 4, ".jpg") == 0) ||
           (n > 5 && strcasecmp(name + n - 5, ".jpeg") == 0);
}

/** @brief Grow by doubling. Upstream's browser reallocs once per entry; at 500 files that is
 *  500 reallocs and the fragmentation to match. */
void library_touch (library_t *lib) {
    if (lib != NULL) {
        lib->dirty = true;
    }
}

lib_record_t *library_push (library_t *lib) {
    if (lib->count == lib->capacity) {
        int cap = lib->capacity * 2;
        lib_record_t *grown = realloc(lib->records, cap * sizeof(lib_record_t));
        if (grown == NULL) {
            return NULL;
        }
        memset(&grown[lib->capacity], 0, (cap - lib->capacity) * sizeof(lib_record_t));
        lib->records = grown;
        lib->capacity = cap;
    }
    return &lib->records[lib->count++];
}

static const struct { const char *ext; uint8_t system; } EXT_SYSTEM[] = {
    { "z64", SYS_N64 }, { "n64", SYS_N64 }, { "v64", SYS_N64 }, { "rom", SYS_N64 },
    { "nes", SYS_NES },
    { "sfc", SYS_SNES }, { "smc", SYS_SNES },
    { "gb",  SYS_GB  },
    { "gbc", SYS_GBC },
    { "sms", SYS_SMS }, { "gg", SYS_SMS }, { "sg", SYS_SMS },
};

/** @brief Classify by extension. Returns false for anything not a ROM. */
static bool classify (const char *name, uint8_t *system) {
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot[1] == '\0') {
        return false;
    }
    char ext[8];
    size_t n = strlen(dot + 1);
    if (n >= sizeof(ext)) {
        return false;
    }
    for (size_t i = 0; i <= n; i++) {
        ext[i] = tolower((unsigned char)dot[1 + i]);
    }
    for (size_t i = 0; i < sizeof(EXT_SYSTEM) / sizeof(EXT_SYSTEM[0]); i++) {
        if (strcmp(ext, EXT_SYSTEM[i].ext) == 0) {
            *system = EXT_SYSTEM[i].system;
            return true;
        }
    }
    return false;
}

/** @brief Filename without directory or extension, as a fallback display title. */
static char *title_from_filename (const char *name) {
    const char *dot = strrchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    char *t = malloc(n + 1);
    if (t == NULL) {
        return NULL;
    }
    memcpy(t, name, n);
    t[n] = '\0';
    return t;
}

/** @brief Trim trailing spaces from a fixed-width ROM header title. */
static char *title_from_header (const char *raw, size_t len) {
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\0')) {
        len--;
    }
    if (len == 0) {
        return NULL;
    }
    char *t = malloc(len + 1);
    if (t == NULL) {
        return NULL;
    }
    memcpy(t, raw, len);
    t[len] = '\0';
    return t;
}

static void index_n64 (lib_record_t *rec, const char *full_path) {
    path_t *p = path_create(full_path);
    if (p == NULL) {
        rec->flags |= LIBF_NO_MATCH;
        return;
    }

    rom_info_t info;
    memset(&info, 0, sizeof(info));

    if (rom_config_load(p, &info) == ROM_OK) {
        rec->check_code = (uint64_t)info.check_code;
        memcpy(rec->game_code, info.game_code, 4);
        rec->game_code[4] = '\0';
        rec->version = info.version;
        rec->save_type = (uint8_t)rom_info_get_save_type(&info);

        uint16_t caps = 0;
        if (info.features.controller_pak)          caps |= CAP_CPAK;
        if (info.features.rumble_pak)              caps |= CAP_RPAK;
        if (info.features.transfer_pak)            caps |= CAP_TPAK;
        if (info.features.voice_recognition_unit)  caps |= CAP_VRU;
        if (info.features.real_time_clock)         caps |= CAP_RTC;
        if (info.features.disk_conversion ||
            info.features.combo_rom_disk_game)     caps |= CAP_64DD;
        if (info.features.expansion_pak == EXPANSION_PAK_REQUIRED)  caps |= CAP_EXP_REQ;
        if (info.features.expansion_pak == EXPANSION_PAK_SUGGESTED) caps |= CAP_EXP_SUGG;
        rec->feat = caps;

        /* Prefer the curated metadata name, then the 20-byte header title, then the filename.
         * The header title is uppercase and often padded, so it is a poor display string, but
         * it beats a filename that has been through a ROM renamer. */
        if (info.meta.name != NULL && info.meta.name[0] != '\0') {
            free(rec->title);
            rec->title = strdup(info.meta.name);
        } else {
            char *ht = title_from_header(info.title, sizeof(info.title));
            if (ht != NULL) {
                free(rec->title);
                rec->title = ht;
            }
        }

        if (rec->game_code[1] == 'E' && rec->game_code[2] == 'D') {
            rec->flags |= LIBF_HOMEBREW;
        }

        /* rom_config_load strdups its metadata strings; not freeing them leaks once per ROM,
         * which at 500 titles is the whole scan's memory. */
        rom_info_free_meta(&info);
    } else {
        rec->flags |= LIBF_NO_MATCH;
    }

    path_free(p);
}

bool library_scan_skipped (const char *name) {
    for (unsigned i = 0; i < sizeof(SCAN_SKIP) / sizeof(SCAN_SKIP[0]); i++) {
        if (strcasecmp(name, SCAN_SKIP[i]) == 0) {
            return true;
        }
    }
    return false;
}

/** @brief Is @p name a file that is never a game? See SCAN_SKIP_FILES. */
static bool skipped_file (const char *name) {
    for (unsigned i = 0; i < sizeof(SCAN_SKIP_FILES) / sizeof(SCAN_SKIP_FILES[0]); i++) {
        if (strcasecmp(name, SCAN_SKIP_FILES[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int scan_dir (library_t *lib, const char *dir, int depth) {
    if (depth > SCAN_MAX_DEPTH) {
        return 0;
    }

    dir_t info;
    int added = 0;

    int result = dir_findfirst(dir, &info);
    while (result == 0) {
        if (info.d_name[0] == '.') {
            result = dir_findnext(dir, &info);
            continue;
        }

        char child[512];
        snprintf(child, sizeof(child), "%s/%s", dir, info.d_name);

        if (info.d_type == DT_DIR) {
            if (!library_scan_skipped(info.d_name)) {
                added += scan_dir(lib, child, depth + 1);
            }
        } else if (skipped_file(info.d_name)) {
            /* Nothing at all: not a game, and not art either. */
        } else if (is_image(info.d_name)) {
            /* Art sitting loose in the tree, whatever it is named or formatted as. Recorded here rather than
             * searched for later: this loop is already visiting every file, so the whole
             * feature costs one extension compare per entry. */
            art_push(lib, info.d_name, child);
        } else {
            uint8_t system;
            if (classify(info.d_name, &system)) {
                lib_record_t *rec = library_push(lib);
                if (rec != NULL) {
                    memset(rec, 0, sizeof(*rec));
                    rec->system = system;
                    rec->path = strdup(child);
                    rec->title = title_from_filename(info.d_name);
                    rec->art_state = ART_PENDING;

                    if (system == SYS_N64) {
                        index_n64(rec, child);
                    } else {
                        rec->flags |= LIBF_NO_MATCH;
                    }
                    added++;
                }
            }
        }

        result = dir_findnext(dir, &info);
    }

    return added;
}

static int compare_title (const void *a, const void *b) {
    const lib_record_t *x = a, *y = b;
    const char *xt = x->title ? x->title : "";
    const char *yt = y->title ? y->title : "";
    int c = strcasecmp(xt, yt);
    return c != 0 ? c : strcasecmp(x->path ? x->path : "", y->path ? y->path : "");
}

void library_join (char *out, size_t cap, const char *storage_prefix, const char *root) {
    size_t plen = strlen(storage_prefix);
    bool prefix_slash = (plen > 0 && storage_prefix[plen - 1] == '/');
    snprintf(out, cap, "%s%s", storage_prefix,
             (prefix_slash && root[0] == '/') ? root + 1 : root);
}

int library_scan (library_t *lib, const char *storage_prefix, const char *root) {
    char base[512];

    /* The prefix already ends in a slash ("rom:/", "sd:/") and every caller passes an absolute
     * root, so a plain concatenation produced "rom://roms" -- and that doubled separator was
     * copied into rec->path for every title, which is what the launch path later opens.
     *
     * DFS accepts it, which is why it survived: `rom://roms/snes/Star Relic.sfc` opened, sized
     * and launched correctly under ares. FatFs over the SC64 has never been asked, and a path
     * separator is a poor thing to discover on hardware. Joined properly instead. */
    library_join(base, sizeof(base), storage_prefix, root);

    uint32_t t0 = TICKS_READ();
    int added = scan_dir(lib, base, 0);
    uint32_t us = TIMER_MICROS(TICKS_SINCE(t0));

    qsort(lib->records, lib->count, sizeof(lib_record_t), compare_title);

    /* Reported per ROM as well as in total: the total is what a user waits, but the per-ROM
     * figure is the one that extrapolates to a 500-title card. */
    debugf("LIBRARY scanned %s: %d titles in %lu us (%lu us/rom)\n",
           base, added, (unsigned long)us,
           (unsigned long)(added ? us / (uint32_t)added : 0));
    return added;
}

int library_tab_view (const library_t *lib, tab_t tab, uint16_t *out, int cap) {
    int n = 0;

    for (int i = 0; i < lib->count && n < cap; i++) {
        const lib_record_t *r = &lib->records[i];
        bool keep = false;

        switch (tab) {
            case TAB_RECENT:      keep = r->last_played != 0; break;
            case TAB_FAVORITES:   keep = (r->flags & LIBF_FAVORITE) != 0; break;
            case TAB_N64:         keep = r->system == SYS_N64; break;
            case TAB_NES:         keep = r->system == SYS_NES; break;
            case TAB_SNES:        keep = r->system == SYS_SNES; break;
            case TAB_GB:          keep = r->system == SYS_GB; break;
            case TAB_GBC:         keep = r->system == SYS_GBC; break;
            case TAB_SMS:         keep = r->system == SYS_SMS; break;
            default: break;
        }

        if (keep) {
            out[n++] = (uint16_t)i;
        }
    }

    /* Records are already title-sorted, so every system tab and Favourites come out ordered for
     * free. Recent is the one tab whose order is not the title order: most recent first. */
    if (tab == TAB_RECENT) {
        for (int i = 1; i < n; i++) {
            uint16_t v = out[i];
            uint32_t kv = lib->records[v].last_played;
            int j = i - 1;
            while (j >= 0 && lib->records[out[j]].last_played < kv) {
                out[j + 1] = out[j];
                j--;
            }
            out[j + 1] = v;
        }
    }

    return n;
}
