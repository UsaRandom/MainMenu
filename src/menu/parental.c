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
#include "menu/parental.h"
#include "ui/theme.h"

/* One character each, so ui_button() can draw them on its discs without a glyph table. The C
 * directions borrow the arrow set the detail sheet already uses for C-right. */
static const char *GLYPH[PBTN_COUNT] = { "A", "Z", "L", "R", "^", "v", "<", ">" };

const char *parental_glyph (int btn) {
    return (btn >= 0 && btn < PBTN_COUNT) ? GLYPH[btn] : "?";
}

uint16_t parental_colour (int btn) {
    switch (btn) {
        case PBTN_A:                return BTN_A_COLOR;
        case PBTN_Z:                return BTN_Z_COLOR;
        case PBTN_L: case PBTN_R:   return BTN_Z_COLOR;   /* shoulders are the shell grey */
        default:                    return BTN_C_COLOR;
    }
}

bool parental_code_set (const settings_t *s) {
    return s->parental_code != NULL && s->parental_code[0] != '\0';
}

/**
 * @brief The stored form of @p digits.
 *
 * Salted with a fixed string so the hash of a code is not the hash of the same four characters
 * anywhere else in the file -- `cache_hash64` is also what keys playstate records, and two
 * unrelated fields that produce identical bytes for related inputs is the kind of coincidence
 * that becomes a bug when someone later greps for a value.
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

void parental_code_store (settings_t *s, const uint8_t *digits) {
    free(s->parental_code);
    if (digits == NULL) {
        s->parental_code = strdup("");
        return;
    }
    char hash[24];
    code_hash(digits, hash, sizeof(hash));
    s->parental_code = strdup(hash);
}

bool parental_code_matches (const settings_t *s, const uint8_t *digits) {
    if (!parental_code_set(s)) {
        return true;
    }
    char hash[24];
    code_hash(digits, hash, sizeof(hash));
    return strcmp(hash, s->parental_code) == 0;
}

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

parental_verdict_t parental_check (const settings_t *s, uint16_t flags, time_t now) {
    if (!parental_code_set(s)) {
        return PARENTAL_ALLOW;
    }

    if (flags & LIBF_LOCKED) {
        return PARENTAL_GAME_LOCKED;
    }

    if (s->parental_hours_enabled && parental_clock_ok(now)) {
        struct tm *lt = localtime(&now);
        if (lt != NULL &&
            !parental_hour_allowed(lt->tm_hour, s->parental_hour_from, s->parental_hour_to)) {
            return PARENTAL_OUTSIDE_HOURS;
        }
    }

    return PARENTAL_ALLOW;
}

void parental_hour_text (int hour, char *buf, int cap) {
    hour = ((hour % 24) + 24) % 24;
    int h12 = hour % 12;
    snprintf(buf, cap, "%d %s", h12 == 0 ? 12 : h12, hour < 12 ? "am" : "pm");
}

void parental_window_text (const settings_t *s, char *buf, int cap) {
    if (!s->parental_hours_enabled) {
        snprintf(buf, cap, "Any time");
        return;
    }
    char a[8], b[8];
    parental_hour_text(s->parental_hour_from, a, sizeof(a));
    parental_hour_text(s->parental_hour_to, b, sizeof(b));
    snprintf(buf, cap, "%s to %s", a, b);
}
