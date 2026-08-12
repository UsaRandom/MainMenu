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

#include "boxart.h"
#include "library.h"
#include "menu/path.h"

#define LIB_INITIAL_CAP 64

#define SCAN_MAX_DEPTH  LIBRARY_SCAN_MAX_DEPTH

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

void library_clear (library_t *lib) {
    if (lib == NULL) {
        return;
    }
    for (int i = 0; i < lib->count; i++) {
        free(lib->records[i].path);
        free(lib->records[i].title);
        free(lib->records[i].art_file);
    }
    memset(lib->records, 0, (size_t)lib->capacity * sizeof(lib_record_t));
    lib->count = 0;

    for (int i = 0; i < lib->art_count; i++) {
        free(lib->art[i].key);
        free(lib->art[i].path);
    }
    free(lib->art);
    lib->art = NULL;
    lib->art_count = 0;
    lib->art_capacity = 0;
    lib->art_sorted = false;
}

void library_free (library_t *lib) {
    if (lib == NULL) {
        return;
    }
    library_clear(lib);
    free(lib->records);
    free(lib);
}

/** @brief Longest art key we will build on the stack. Longer names fall back to the heap. */
#define ART_KEY_CAP 256

/**
 * @brief Lowercase @p name with its extension stripped, into @p out.
 *
 * Exists so a lookup costs no allocation. library_find_art() used to build its key with
 * art_key_from_name(), which mallocs -- two lookups per record across a 278-title card is 556
 * malloc/free pairs on the boot path, for keys that never outlive the comparison.
 *
 * @return false if the name is empty or does not fit, in which case @p out is untouched.
 */
static bool art_key_into (char *out, size_t cap, const char *name) {
    const char *dot = strrchr(name, '.');
    size_t n = (dot != NULL) ? (size_t)(dot - name) : strlen(name);
    if (n == 0 || n >= cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)tolower((unsigned char)name[i]);
    }
    out[n] = '\0';
    return true;
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

/**
 * @brief Remember a loose PNG. Duplicates keep the first one the scan reached.
 *
 * This used to say "so the shallowest wins", and that was never true. scan_dir() recurses into a
 * subdirectory at the point it meets it in the directory listing, so a duplicate one level down
 * is pushed before a sibling file that readdir happens to return later -- which is exactly what
 * `tools/hosttest/test_library_art.c` found on the first tree it built. The rule is DFS order,
 * and it coincides with "shallowest" only when the listing puts the file before the directory.
 * Behaviour is unchanged; only the description was wrong.
 *
 * Appends unconditionally. It used to scan the whole table for a duplicate on every push, which
 * is O(n^2) across the scan -- about 38,000 strcmp on the 278-cover card that prompted this, and
 * growing as the square. Deduplication moved into art_sort(), which is already touching every
 * entry and can drop a duplicate for the cost of one comparison against its neighbour.
 *
 * The "first seen wins" rule survives the move because each entry records its scan order in
 * ::art_entry_t::seq and the sort breaks ties on it. Without that, qsort being unstable would
 * make which duplicate survives depend on the input order -- the kind of bug that reproduces on
 * one card and not another.
 *
 * Public since incremental revalidation, which fills this table from the signature walk rather
 * than from a scan. See library.h.
 */
void library_art_note (library_t *lib, const char *name, const char *full_path) {
    char *key = art_key_from_name(name);
    if (key == NULL) {
        return;
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
    lib->art[lib->art_count].seq = lib->art_count;
    lib->art_count++;
    lib->art_sorted = false;
}

/** @brief Order by key, then by scan order so the shallowest duplicate sorts first. */
static int art_cmp (const void *a, const void *b) {
    const art_entry_t *x = a, *y = b;
    int c = strcmp(x->key, y->key);
    if (c != 0) {
        return c;
    }
    return (x->seq > y->seq) - (x->seq < y->seq);
}

/**
 * @brief Sort the art table by key and drop duplicates, leaving it ready for bsearch.
 *
 * Called lazily from library_find_art() rather than at the end of the scan, because the scan is
 * not the only thing that fills the table -- and a table that is sorted at one call site and not
 * another is exactly how a bsearch starts returning misses for entries that are present.
 */
static void art_sort (library_t *lib) {
    if (lib->art_sorted || lib->art_count == 0) {
        lib->art_sorted = true;
        return;
    }
    qsort(lib->art, (size_t)lib->art_count, sizeof(art_entry_t), art_cmp);

    /* Compact in place, keeping the first of each run -- which the tiebreak above has already
     * made the lowest seq, so this is the same survivor the old push-time check would have
     * picked. */
    int w = 1;
    for (int r = 1; r < lib->art_count; r++) {
        if (strcmp(lib->art[r].key, lib->art[w - 1].key) == 0) {
            free(lib->art[r].key);
            free(lib->art[r].path);
            continue;
        }
        lib->art[w++] = lib->art[r];
    }
    lib->art_count = w;
    lib->art_sorted = true;
}

/** @brief The filename part of @p path, or the whole thing if it has no separator. */
static const char *art_basename (const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int library_resolve_loose_art (library_t *lib) {
    if (lib == NULL || lib->art_count == 0) {
        return 0;
    }

    int n = 0;
    for (int i = 0; i < lib->count; i++) {
        lib_record_t *r = &lib->records[i];
        if (r->art_file != NULL) {
            continue;
        }

        /* Game code first, then the ROM's filename -- the same order and the same two keys
         * thumbcache's art_resolve() uses, because a record resolved here and a record resolved
         * lazily must land on the same file. */
        const char *hit = NULL;
        if (r->game_code[0] != '\0') {
            hit = library_find_art(lib, r->game_code);
        }
        if (hit == NULL && r->path != NULL) {
            hit = library_find_art(lib, art_basename(r->path));
        }
        if (hit == NULL) {
            continue;
        }

        r->art_file = strdup(hit);
        if (r->art_file != NULL) {
            /* Both of these have to move with the path, and neither did before incremental
             * revalidation could run this against records loaded from the index. Such a record
             * arrives carrying LIBF_ART_MISSING -- the persisted "we looked and there was
             * nothing" -- and if a cover for it turns up in some other directory, giving it the
             * path while leaving the flag set writes an index whose record says both. Next boot
             * that record loads with art_state ART_NONE and a perfectly good art_file nothing
             * will ever decode. Straight after a scan the flag is always clear, which is why
             * this never mattered until now. */
            r->flags &= (uint16_t)~LIBF_ART_MISSING;
            r->art_state = ART_PENDING;
            n++;
        }
    }

    debugf("LIBRARY resolved art for %d of %d titles\n", n, lib->count);
    return n;
}

const char *library_find_art (const library_t *lib, const char *name) {
    if (lib == NULL || name == NULL || name[0] == '\0' || lib->art_count == 0) {
        return NULL;
    }

    char key[ART_KEY_CAP];
    if (!art_key_into(key, sizeof(key), name)) {
        return NULL;
    }

    /* Sorting is a mutation of a table the caller handed us as const, and the cast says so. The
     * const is about the *library* -- records, counts, what the scan found -- and the art table's
     * order is not part of that: every lookup returns the same path before and after. Same reason
     * and same shape as library_touch() in thumbcache's art_probe_shape(). */
    art_sort((library_t *)lib);

    int lo = 0, hi = lib->art_count - 1;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) / 2);
        int c = strcmp(lib->art[mid].key, key);
        if (c == 0) {
            return lib->art[mid].path;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

bool library_is_art_name (const char *name) {
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

/**
 * @brief Read a directory's entries out in full before touching anything in it.
 *
 * The scan used to interleave: read one entry, then immediately open the ROM it named and probe
 * three sidecars beside it, then ask for the next entry. Two things came of that, both bad.
 *
 * FatFs is built here with FF_FS_TINY, which means ONE 512-byte sector window per filesystem
 * rather than one per open file. So every probe's path walk evicted the sector the enumeration
 * was standing on, and the enumeration evicted it back. They thrashed each other for the whole
 * scan.
 *
 * And the probes themselves were answerable from what the enumeration had already seen. A
 * `<rom>.ini`, `<rom>.meta` or `<rom>.metadata.ini` lives in the directory being read, so once
 * the listing is in memory the question costs a string compare instead of a directory walk that
 * has to reach the end to prove absence.
 *
 * Recursion moves to the end, after every file here is done. That changes DFS order, and it
 * changes it toward what art_push() always claimed: with subdirectories visited last, the
 * SHALLOWEST duplicate cover really does win now, where before it was whichever the directory
 * listing happened to put first. The claim in library_art_note() is finally true; see the note
 * there and the duplicate checks in tools/hosttest/test_library_art.c.
 *
 * @param recurse  False indexes this directory and stops, which is what incremental revalidation
 *                 asks for: it has already been told, by the signature walk, exactly which
 *                 directories moved, and walking their children again would re-index directories
 *                 whose records the index still holds.
 */
static int scan_dir (library_t *lib, const char *dir, int depth,
                     library_scan_progress_t on_progress, bool recurse) {
    if (depth > SCAN_MAX_DEPTH) {
        return 0;
    }

    dir_t info;
    int added = 0;

    /* The listing, held only for this frame. Names rather than a fixed table because a directory
     * of several hundred covers is normal and a fixed cap would silently stop indexing past it. */
    char **names = NULL;
    bool *isdir = NULL;
    int n = 0, cap = 0;

    int result = dir_findfirst(dir, &info);
    while (result == 0) {
        if (info.d_name[0] != '.') {
            if (n == cap) {
                int want = cap ? cap * 2 : 32;
                char **gn = realloc(names, (size_t)want * sizeof(*gn));
                bool *gd = realloc(isdir, (size_t)want * sizeof(*gd));
                if (gn != NULL) { names = gn; }
                if (gd != NULL) { isdir = gd; }
                if (gn == NULL || gd == NULL) {
                    break;      /* out of memory: index what was read, do not lose it */
                }
                cap = want;
            }
            names[n] = strdup(info.d_name);
            if (names[n] == NULL) {
                break;
            }
            isdir[n] = (info.d_type == DT_DIR);
            n++;
        }
        result = dir_findnext(dir, &info);
    }

    /* Files first, so a subdirectory cannot get between a ROM and the sidecar sitting beside it,
     * and so the listing above is what answers the sidecar question. */
    rom_info_set_dir_listing((const char *const *)names, n);

    for (int i = 0; i < n; i++) {
        if (isdir[i]) {
            continue;
        }

        /* Per entry rather than per record added, so the readout keeps moving through a folder of
         * covers or a deep tree of nothing. lib->count rather than `added`, because `added` is
         * this directory's tally and the plate is showing the library's. */
        if (on_progress != NULL) {
            on_progress(lib->count);
        }

        char child[512];
        snprintf(child, sizeof(child), "%s/%s", dir, names[i]);

        if (skipped_file(names[i])) {
            /* Nothing at all: not a game, and not art either. */
        } else if (library_is_art_name(names[i])) {
            /* Art sitting loose in the tree, whatever it is named or formatted as. Recorded here
             * rather than searched for later: this loop is already visiting every file, so the
             * whole feature costs one extension compare per entry. */
            library_art_note(lib, names[i], child);
        } else {
            uint8_t system;
            if (classify(names[i], &system)) {
                lib_record_t *rec = library_push(lib);
                if (rec != NULL) {
                    memset(rec, 0, sizeof(*rec));
                    rec->system = system;
                    rec->path = strdup(child);
                    rec->title = title_from_filename(names[i]);
                    rec->art_state = ART_PENDING;
                    /* Not zero: zero is ART_PORTRAIT, and a record that has never been probed
                     * must be distinguishable from one whose cover really is portrait -- the
                     * first falls back to its system's shape, the second does not. */
                    rec->art_kind = ART_KIND_UNKNOWN;

                    if (system == SYS_N64) {
                        index_n64(rec, child);
                    } else {
                        rec->flags |= LIBF_NO_MATCH;
                    }
                    added++;
                }
            }
        }
    }

    /* Cleared before recursing: the child installs its own, and a stale parent listing would let
     * a child answer "is there a sidecar" from the wrong directory's names. */
    rom_info_set_dir_listing(NULL, 0);

    for (int i = 0; i < n && recurse; i++) {
        if (!isdir[i] || library_scan_skipped(names[i])) {
            continue;
        }
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", dir, names[i]);
        added += scan_dir(lib, child, depth + 1, on_progress, true);
    }

    for (int i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
    free(isdir);

    return added;
}

int library_scan_dir (library_t *lib, const char *dir, library_scan_progress_t on_progress) {
    /* Depth 0 rather than the directory's real depth in the tree, and it makes no difference:
     * depth only bounds recursion and there is none here. The caller is responsible for not
     * asking about a directory deeper than the scan would have reached -- see libindex.c, which
     * fingerprints one level further down than the scan indexes. */
    return scan_dir(lib, dir, 0, on_progress, false);
}

static int compare_title (const void *a, const void *b) {
    const lib_record_t *x = a, *y = b;
    const char *xt = x->title ? x->title : "";
    const char *yt = y->title ? y->title : "";
    int c = strcasecmp(xt, yt);
    return c != 0 ? c : strcasecmp(x->path ? x->path : "", y->path ? y->path : "");
}

void library_sort (library_t *lib) {
    qsort(lib->records, lib->count, sizeof(lib_record_t), compare_title);
}

void library_join (char *out, size_t cap, const char *storage_prefix, const char *root) {
    size_t plen = strlen(storage_prefix);
    bool prefix_slash = (plen > 0 && storage_prefix[plen - 1] == '/');
    snprintf(out, cap, "%s%s", storage_prefix,
             (prefix_slash && root[0] == '/') ? root + 1 : root);
}

int library_scan (library_t *lib, const char *storage_prefix, const char *root,
                  library_scan_progress_t on_progress) {
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
    int added = scan_dir(lib, base, 0, on_progress, true);
    uint32_t us = TIMER_MICROS(TICKS_SINCE(t0));

    library_sort(lib);

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
