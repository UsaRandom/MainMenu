/**
 * @file cardstat.c
 * @brief See cardstat.h.
 * @ingroup menu
 */

#include <stdio.h>
#include <string.h>

#include "cardstat.h"
#include "cheats/cheatdb.h"
#include "flashcart/flashcart.h"
#include "library/cache.h"
#include "menu/cart_load.h"
#include "menu/launchlog.h"
#include "menu/memprofile.h"
#include "menu/profile.h"

static const char *storage;
static const library_t *lib;

static unsigned cores_present;
static bool     have_boot;
static bool     idx_hit;
static bool     idx_repaired;
static uint32_t idx_us;
static uint32_t scan_us;
static unsigned gap_us;
static unsigned slack_us;

#define CORE_NES  (1u << 0)
#define CORE_SNES (1u << 1)
#define CORE_GB   (1u << 2)
#define CORE_GBC  (1u << 3)
#define CORE_SMS  (1u << 4)

static bool on_card (void) {
    return storage != NULL && strncmp(storage, "sd:", 3) == 0;
}

static bool starved (void) {
    return have_boot && slack_us > 0 && gap_us > slack_us;
}

static unsigned missing_cores (void) {
    if (lib == NULL) {
        return 0;
    }
    unsigned need = 0;
    for (int i = 0; i < lib->count; i++) {
        switch (lib->records[i].system) {
            case SYS_NES:  need |= CORE_NES;  break;
            case SYS_SNES: need |= CORE_SNES; break;
            case SYS_GB:   need |= CORE_GB;   break;
            case SYS_GBC:  need |= CORE_GBC;  break;
            case SYS_SMS:  need |= CORE_SMS;  break;
            default: break;
        }
    }
    return need & ~cores_present;
}

static const struct { unsigned bit; const char *name; } CORE_TAB[] = {
    { CORE_NES,  "NES" },
    { CORE_SNES, "SNES" },
    { CORE_GB,   "GB" },
    { CORE_GBC,  "GBC" },
    { CORE_SMS,  "SMS" },
};

static void fmt_bits (char *out, size_t cap, unsigned bits) {
    out[0] = '\0';
    for (unsigned i = 0; i < sizeof(CORE_TAB) / sizeof(CORE_TAB[0]); i++) {
        if (bits & CORE_TAB[i].bit) {
            size_t n = strlen(out);
            snprintf(out + n, cap - n, "%s%s", n ? ", " : "", CORE_TAB[i].name);
        }
    }
    if (out[0] == '\0') {
        snprintf(out, cap, "None");
    }
}

void cardstat_bind (const char *storage_prefix, const library_t *library) {
    storage = storage_prefix;
    lib = library;
}

void cardstat_probe_cores (void) {
    cores_present = 0;
    if (storage == NULL) {
        return;
    }
    if (cart_load_emu_present(storage, CART_LOAD_EMU_TYPE_NES)) {
        cores_present |= CORE_NES;
    }
    if (cart_load_emu_present(storage, CART_LOAD_EMU_TYPE_SNES)) {
        cores_present |= CORE_SNES;
    }
    if (cart_load_emu_present(storage, CART_LOAD_EMU_TYPE_GAMEBOY)) {
        cores_present |= CORE_GB;
    }
    if (cart_load_emu_present(storage, CART_LOAD_EMU_TYPE_GAMEBOY_COLOR)) {
        cores_present |= CORE_GBC;
    }
    if (cart_load_emu_present(storage, CART_LOAD_EMU_TYPE_SEGA_GENERIC_8BIT)) {
        cores_present |= CORE_SMS;
    }
}

void cardstat_set_boot (bool hit, bool repaired,
                        uint32_t i_us, uint32_t s_us,
                        unsigned gap, unsigned slack) {
    have_boot = true;
    idx_hit = hit;
    idx_repaired = repaired;
    idx_us = i_us;
    scan_us = s_us;
    gap_us = gap;
    slack_us = slack;
}

int cardstat_plate (char out[][72], int cap) {
    int n = 0;
    if (cap <= 0) {
        return 0;
    }

    if (on_card() && !cache_writable() && n < cap) {
        snprintf(out[n++], 72, "Card is read-only. Every start will be slow.");
    }
    if (mem_small() && n < cap) {
        snprintf(out[n++], 72, "No Expansion Pak. Art cache is smaller.");
    }
    if (lib != NULL && lib->dir_busiest >= CARDSTAT_BUSY_WARN && n < cap) {
        snprintf(out[n++], 72, "%d files in one folder. Split it to speed this up.",
                 lib->dir_busiest);
    }
    unsigned miss = missing_cores();
    if (miss && n < cap) {
        char names[40];
        fmt_bits(names, sizeof(names), miss);
        snprintf(out[n++], 72, "%s need files in emulators.", names);
    }
    if (starved() && n < cap) {
        snprintf(out[n++], 72, "Music stalled at start.");
    }
    return n > 2 ? 2 : n;
}

int cardstat_info (cardstat_row_t *out, int cap) {
    if (cap <= 0) {
        return 0;
    }
    int n = 0;

    snprintf(out[n].label, sizeof(out[n].label), "Card");
    if (!on_card()) {
        snprintf(out[n].value, sizeof(out[n].value), "Emulator, not a card");
    } else if (!cache_writable()) {
        snprintf(out[n].value, sizeof(out[n].value), "%s. Nothing will be saved",
                 cache_status());
    } else {
        snprintf(out[n].value, sizeof(out[n].value), "Writable");
    }
    n++;

    if (n < cap) {
        snprintf(out[n].label, sizeof(out[n].label), "Memory");
        snprintf(out[n].value, sizeof(out[n].value),
                 mem_small() ? "4 MB, no Expansion Pak" : "8 MB");
        n++;
    }

    if (n < cap) {
        snprintf(out[n].label, sizeof(out[n].label), "Titles");
        snprintf(out[n].value, sizeof(out[n].value), "%d", lib != NULL ? lib->count : 0);
        n++;
    }

    if (have_boot && n < cap) {
        snprintf(out[n].label, sizeof(out[n].label), "Library");
        if (!idx_hit) {
            if (scan_us >= 1000000) {
                snprintf(out[n].value, sizeof(out[n].value), "Scanned this start (%u s)",
                         (unsigned)(scan_us / 1000000));
            } else {
                snprintf(out[n].value, sizeof(out[n].value), "Scanned this start");
            }
        } else if (idx_repaired) {
            snprintf(out[n].value, sizeof(out[n].value), "Repaired this start");
        } else if (idx_us >= 1000000) {
            snprintf(out[n].value, sizeof(out[n].value), "Cached (%u s)",
                     (unsigned)(idx_us / 1000000));
        } else {
            snprintf(out[n].value, sizeof(out[n].value), "Cached");
        }
        n++;
    }

    if (lib != NULL && lib->dir_busiest > 0 && n < cap) {
        snprintf(out[n].label, sizeof(out[n].label), "Folder");
        if (lib->dir_busiest_name[0] != '\0') {
            snprintf(out[n].value, sizeof(out[n].value), "%d files in %s%s",
                     lib->dir_busiest, lib->dir_busiest_name,
                     lib->dir_busiest >= CARDSTAT_BUSY_WARN ? " (split it)" : "");
        } else {
            snprintf(out[n].value, sizeof(out[n].value), "%d files in one folder",
                     lib->dir_busiest);
        }
        n++;
    }

    if (n < cap) {
        unsigned miss = missing_cores();
        char names[40];
        snprintf(out[n].label, sizeof(out[n].label), "Emulators");
        if (miss) {
            fmt_bits(names, sizeof(names), miss);
            snprintf(out[n].value, sizeof(out[n].value), "%s missing", names);
        } else {
            fmt_bits(names, sizeof(names), cores_present);
            snprintf(out[n].value, sizeof(out[n].value), "%s", names);
        }
        n++;
    }

    if (n < cap) {
        snprintf(out[n].label, sizeof(out[n].label), "Cheats");
        if (cheatdb_available()) {
            snprintf(out[n].value, sizeof(out[n].value), "%d games in the database",
                     cheatdb_game_count());
        } else {
            snprintf(out[n].value, sizeof(out[n].value), "No cheats.db on the card");
        }
        n++;
    }

    if (have_boot && n < cap) {
        snprintf(out[n].label, sizeof(out[n].label), "Audio");
        if (starved()) {
            snprintf(out[n].value, sizeof(out[n].value), "Stalled at start (%u ms of %u ms)",
                     gap_us / 1000, slack_us / 1000);
        } else if (slack_us > 0) {
            snprintf(out[n].value, sizeof(out[n].value), "OK (%u ms of %u ms)",
                     gap_us / 1000, slack_us / 1000);
        } else {
            snprintf(out[n].value, sizeof(out[n].value), "OK");
        }
        n++;
    }

    if (n < cap) {
        snprintf(out[n].label, sizeof(out[n].label), "Players");
        snprintf(out[n].value, sizeof(out[n].value), "%d", profile_count());
        n++;
    }

    if (n < cap) {
        snprintf(out[n].label, sizeof(out[n].label), "Firmware");
        if (flashcart_is_dummy()) {
            snprintf(out[n].value, sizeof(out[n].value), "None (emulator)");
        } else {
            flashcart_firmware_version_t v = flashcart_get_firmware_version();
            snprintf(out[n].value, sizeof(out[n].value), "%u.%u.%lu",
                     (unsigned)v.major, (unsigned)v.minor, (unsigned long)v.revision);
        }
        n++;
    }

    return n;
}

void cardstat_log (void) {
    launchlog_line("card %s%s",
                   on_card() ? (cache_writable() ? "writable" : "READ-ONLY") : "emulator",
                   mem_small() ? ", 4 MB no Expansion Pak" : ", 8 MB");
    if (lib != NULL && lib->dir_busiest > 0) {
        launchlog_line("busiest folder %d entries (%s)",
                       lib->dir_busiest,
                       lib->dir_busiest_name[0] ? lib->dir_busiest_name : "?");
    }
    unsigned miss = missing_cores();
    if (miss) {
        char names[40];
        fmt_bits(names, sizeof(names), miss);
        launchlog_line("emulators missing: %s", names);
    }
}
