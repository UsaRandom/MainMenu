/**
 * @file parental.c
 * @brief The policy behind the padlock. See parental.h for what this is and is not.
 * @ingroup menu
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "library/cache.h"
#include "library/library.h"
#include "menu/ini_parser.h"
#include "menu/parental.h"
#include "menu/paths.h"
#include "ui/theme.h"

#define PARENTAL_FILE       "parental.ini"

/** Five seconds a failure, ten minutes at the ceiling. Both numbers are in parental.h. */
#define PENALTY_PER_FAIL_S  5.0f
#define PENALTY_MAX_S       600.0f

/* One character each, so ui_button() can draw them on its discs without a glyph table. The same
 * arrow set the detail sheet already uses for C-right. */
static const char *GLYPH[PBTN_COUNT] = { "^", "v", "<", ">" };

static parental_t state = {
    .code = "",
    .failures = 0,
    /* A window that would be a bedtime rule if one were ever switched on. */
    .hours_enabled = false,
    .hour_from = 8,
    .hour_to = 20,
};

static char  file_path[300];
static float wait_left;

parental_t *parental_state (void) {
    return &state;
}

const char *parental_glyph (int btn) {
    return (btn >= 0 && btn < PBTN_COUNT) ? GLYPH[btn] : "?";
}

uint16_t parental_colour (int btn) {
    (void)btn;
    return BTN_C_COLOR;
}

/* ------------------------------------------------------------------ the file */

/** @brief What the current failure count costs a fresh attempt. */
static float penalty (void) {
    float s = (float)state.failures * PENALTY_PER_FAIL_S;
    return (s > PENALTY_MAX_S) ? PENALTY_MAX_S : s;
}

void parental_load (const char *storage_prefix) {
    menu_path(file_path, sizeof(file_path), storage_prefix, PARENTAL_FILE);

    /* Deliberately NOT cache_load(). A cache with a version mismatch is deleted and rebuilt from
     * the card, and there is nothing on the card to rebuild a code from -- so this file going
     * through that layer would mean the next routine format bump quietly unlocked every locked
     * game on every card. See parental.h. */
    ini_t *ini = ini_try_load(file_path);

    snprintf(state.code, sizeof(state.code), "%s",
             ini_get_string(ini, "parental", "code", ""));

    int failures = ini_get_int(ini, "parental", "failures", 0);
    /* Clamped rather than trusted: a hand-edited file is the expected way to attack this, and a
     * negative or absurd count would either wipe out the backoff or wedge the pad forever. The
     * cap is where the penalty saturates anyway, so nothing above it can mean anything. */
    if (failures < 0) {
        failures = 0;
    }
    if (failures > (int)(PENALTY_MAX_S / PENALTY_PER_FAIL_S)) {
        failures = (int)(PENALTY_MAX_S / PENALTY_PER_FAIL_S);
    }
    state.failures = (uint32_t)failures;

    state.hours_enabled = ini_get_bool(ini, "parental", "hours_enabled", false);
    state.hour_from = ini_get_int(ini, "parental", "hour_from", 8);
    state.hour_to = ini_get_int(ini, "parental", "hour_to", 20);
    /* Clamped on the way in, not only on the way out. An out-of-range hour would otherwise reach
     * localtime() comparisons that quietly allow everything. */
    if (state.hour_from < 0 || state.hour_from > 23) {
        state.hour_from = 8;
    }
    if (state.hour_to < 0 || state.hour_to > 23) {
        state.hour_to = 20;
    }

    ini_free(ini);

    /* Armed at boot, not at the first press: resetting the console has to cost the full wait the
     * stored failures earned, or resetting is the way round the wait. */
    wait_left = penalty();
}

void parental_save (void) {
    ini_t *ini = ini_create();

    ini_set_string(ini, "parental", "code", state.code);
    ini_set_int(ini, "parental", "failures", (int)state.failures);
    ini_set_bool(ini, "parental", "hours_enabled", state.hours_enabled);
    ini_set_int(ini, "parental", "hour_from", state.hour_from);
    ini_set_int(ini, "parental", "hour_to", state.hour_to);

    if (!ini_save(ini, file_path)) {
        debugf("[PARENTAL] Failed to save to %s\n", file_path);
    }

    ini_free(ini);
}

/* ------------------------------------------------------------------ the code */

bool parental_code_set (void) {
    return state.code[0] != '\0';
}

/**
 * @brief The stored form of @p digits.
 *
 * Salted with a fixed string so the hash of a code is not the hash of the same six characters
 * anywhere else -- `cache_hash64` is also what keys playstate records, and two unrelated fields
 * that produce identical bytes for related inputs is the kind of coincidence that becomes a bug
 * when someone later greps for a value.
 */
static void code_hash (const uint8_t *digits, char *out, int cap) {
    char plain[PARENTAL_CODE_LEN + 8];
    int n = snprintf(plain, sizeof(plain), "pc:");
    for (int i = 0; i < PARENTAL_CODE_LEN; i++) {
        plain[n++] = (char)('0' + (digits[i] % PBTN_COUNT));
    }
    plain[n] = '\0';
    snprintf(out, cap, "%08lX%08lX",
             (unsigned long)(cache_hash64(plain) >> 32),
             (unsigned long)(cache_hash64(plain) & 0xFFFFFFFFu));
}

void parental_code_store (const uint8_t *digits) {
    if (digits == NULL) {
        state.code[0] = '\0';
    } else {
        code_hash(digits, state.code, sizeof(state.code));
    }
    /* Setting or clearing a code is also the moment to forgive whatever came before it: the
     * person doing it has just proved they are the parent. */
    state.failures = 0;
    wait_left = 0.0f;
    parental_save();
}

bool parental_code_matches (const uint8_t *digits) {
    if (!parental_code_set()) {
        return true;
    }
    char hash[24];
    code_hash(digits, hash, sizeof(hash));
    return strcmp(hash, state.code) == 0;
}

/* ------------------------------------------------------------------ the wait */

void parental_note_attempt (void) {
    if (state.failures < 0xFFFFFFFFu) {
        state.failures++;
    }
    /* Persisted here, before the caller compares anything. If this happened after the comparison,
     * a wrong guess followed by pulling the power would cost nothing at all. */
    parental_save();
    wait_left = penalty();
}

void parental_note_success (void) {
    state.failures = 0;
    wait_left = 0.0f;
    parental_save();
}

float parental_wait_left (void) {
    return wait_left;
}

void parental_wait_tick (float dt) {
    if (wait_left > 0.0f) {
        wait_left -= dt;
        if (wait_left < 0.0f) {
            wait_left = 0.0f;
        }
    }
}

void parental_wait_text (char *buf, int cap) {
    int s = (int)(wait_left + 0.999f);
    if (s >= 120) {
        snprintf(buf, cap, "%d minutes", (s + 59) / 60);
    } else if (s >= 60) {
        snprintf(buf, cap, "a minute");
    } else if (s == 1) {
        snprintf(buf, cap, "1 second");
    } else {
        snprintf(buf, cap, "%d seconds", s);
    }
}

/* ------------------------------------------------------------------ the schedule */

bool parental_hour_allowed (int hour, int from, int to) {
    /* A window with no width is not a window. Rather than blocking the whole day or allowing it
     * by accident, from == to means "no restriction", which is what a parent who set both ends
     * the same was reaching for. */
    if (from == to) {
        return true;
    }
    if (from < to) {
        return hour >= from && hour < to;
    }
    /* Wraps midnight: 20:00 to 07:00 is the bedtime rule, and it is the common case rather than
     * the exotic one. `hour >= from && hour < to` is false for every hour of the day here. */
    return hour >= from || hour < to;
}

bool parental_clock_ok (time_t now) {
    /* time(NULL) returns (time_t)-1 with no RTC, and 0 is the epoch, which no console has ever
     * genuinely been sitting at. Either means there is no clock to enforce against. */
    return now > 0;
}

parental_verdict_t parental_check (uint16_t flags, time_t now) {
    if (!parental_code_set()) {
        return PARENTAL_ALLOW;
    }

    if (flags & LIBF_LOCKED) {
        return PARENTAL_GAME_LOCKED;
    }

    if (state.hours_enabled && parental_clock_ok(now)) {
        struct tm *lt = localtime(&now);
        if (lt != NULL && !parental_hour_allowed(lt->tm_hour, state.hour_from, state.hour_to)) {
            return PARENTAL_OUTSIDE_HOURS;
        }
    }

    return PARENTAL_ALLOW;
}

long parental_seconds_until_open (time_t now) {
    if (!parental_code_set() || !state.hours_enabled || !parental_clock_ok(now)) {
        return -1;
    }
    struct tm *lt = localtime(&now);
    if (lt == NULL || parental_hour_allowed(lt->tm_hour, state.hour_from, state.hour_to)) {
        return -1;
    }

    /* Counted in whole hours from the top of the next one, plus the rest of this one. Doing it by
     * building a struct tm for the opening time and calling mktime() would be exact, and would
     * also have to decide what "the opening hour" means when the window wraps midnight and the
     * clock is at 23:59 -- for a line that reads "in 2h 15m" the arithmetic is not worth it. */
    long secs = (long)(3600 - (lt->tm_min * 60 + lt->tm_sec));
    for (int h = 1; h < 24; h++) {
        int hour = (lt->tm_hour + h) % 24;
        if (parental_hour_allowed(hour, state.hour_from, state.hour_to)) {
            return secs;
        }
        secs += 3600;
    }
    return -1;      /* no hour of the day is allowed; parental_hour_allowed cannot say this */
}

void parental_hour_text (int hour, char *buf, int cap) {
    hour = ((hour % 24) + 24) % 24;
    int h12 = hour % 12;
    snprintf(buf, cap, "%d %s", h12 == 0 ? 12 : h12, hour < 12 ? "am" : "pm");
}

void parental_window_text (char *buf, int cap) {
    if (!state.hours_enabled) {
        snprintf(buf, cap, "Any time");
        return;
    }
    char a[8], b[8];
    parental_hour_text(state.hour_from, a, sizeof(a));
    parental_hour_text(state.hour_to, b, sizeof(b));
    snprintf(buf, cap, "%s to %s", a, b);
}
