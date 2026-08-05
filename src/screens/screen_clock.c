/**
 * @file screen_clock.c
 * @brief Setting the clock the cartridge keeps.
 * @ingroup screens
 *
 * Reached from Settings. Until this existed there was no way to set the time at all, which made
 * three things quietly wrong rather than visibly missing: `last_played` on the detail sheet, the
 * parental schedule, and anything that ever wants to say how long ago something happened.
 *
 * ## Not rtc_set()
 *
 * The pinned libdragon deprecates `rtc_get`, `rtc_set` and `rtc_is_writable` in favour of the ISO
 * C ones -- `rtc_init()` hooks the clock into newlib, so `time()` reads it and `settimeofday()`
 * writes it. `rtc_is_writable()` is worth naming specifically: it is `static inline bool
 * rtc_is_writable(void) { return true; }`, an unconditional yes carrying a deprecation warning
 * that says "just assume it's always writable". Asking it whether a write will work answers
 * nothing, so this screen **reads the clock back** after setting it instead.
 *
 * ## What a set does and does not survive
 *
 * `rtc_get_source()` says where the time comes from. `RTC_SOURCE_NONE` is libdragon's software
 * clock: settable, correct until the console is switched off, and gone afterwards. That is the
 * case under ares, and it is likely the case on any cart whose RTC is not wired up -- so the
 * screen says which one it is rather than letting a parent set a bedtime that evaporates.
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <libdragon.h>

#include "app.h"
#include "menu/fonts.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

#define LIST_X      SAFE_X
#define FIELDS_Y    170
#define FIELD_H     40
#define CLOCK_SEED_YEAR 2026

typedef enum {
    F_YEAR = 0,
    F_MONTH,
    F_DAY,
    F_HOUR,
    F_MINUTE,
    F_COUNT,
} field_t;

static const char *MONTHS[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/** Field x positions, so the row reads as one date and time rather than five boxes. */
static const int FIELD_X[F_COUNT] = { 0, 90, 170, 260, 330 };
static const int FIELD_W[F_COUNT] = { 76, 66, 66, 56, 56 };

static struct tm edit;
static int  cursor;
static const char *error;

/** @brief Days in @p month (0-11) of @p year, Gregorian. */
static int days_in_month (int year, int month) {
    static const int N[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 1) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return N[month];
}

/**
 * @brief Pull the day back into the month it is in.
 *
 * 31 January with the month stepped to February is 31 February, and mktime() would silently
 * resolve that to 2 or 3 March -- so the screen would accept a date and the clock would hold a
 * different one. Clamping here means what is displayed is what gets set.
 */
static void clamp_day (void) {
    int max = days_in_month(edit.tm_year + 1900, edit.tm_mon);
    if (edit.tm_mday > max) {
        edit.tm_mday = max;
    }
}

static void clock_enter (app_t *app) {
    (void)app;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    /* A clock that has never been set reads as the epoch, and offering 1970 as the starting point
     * means the first thing anyone does is hold up for fifty presses. The seed year is arbitrary
     * and will drift out of date; that is fine, because the only thing being bought is a starting
     * point near enough that nobody has to hold a button. It is not read anywhere else. */
    if (t == NULL || t->tm_year + 1900 < 2000) {
        struct tm seed = { 0 };
        seed.tm_year = CLOCK_SEED_YEAR - 1900;
        seed.tm_mon  = 0;
        seed.tm_mday = 1;
        seed.tm_hour = 12;
        edit = seed;
    } else {
        edit = *t;
    }
    edit.tm_sec = 0;
    edit.tm_isdst = -1;
    cursor = 0;
    error = NULL;
}

static void step_field (int delta) {
    switch ((field_t)cursor) {
        case F_YEAR:
            edit.tm_year += delta;
            /* 2000..2099. The lower bound is not fussiness: the software clock's range starts at
             * 1970 and a parent who overshoots downwards should hit a wall rather than wrap into
             * dates the hardware cannot hold. */
            if (edit.tm_year < 100) edit.tm_year = 100;
            if (edit.tm_year > 199) edit.tm_year = 199;
            clamp_day();
            break;
        case F_MONTH:
            edit.tm_mon = (edit.tm_mon + delta + 12) % 12;
            clamp_day();
            break;
        case F_DAY: {
            int max = days_in_month(edit.tm_year + 1900, edit.tm_mon);
            edit.tm_mday = ((edit.tm_mday - 1 + delta + max) % max) + 1;
            break;
        }
        case F_HOUR:
            edit.tm_hour = (edit.tm_hour + delta + 24) % 24;
            break;
        case F_MINUTE:
            edit.tm_min = (edit.tm_min + delta + 60) % 60;
            break;
        default:
            break;
    }
}

static void save (app_t *app) {
    struct tm t = edit;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t want = mktime(&t);
    if (want == (time_t)-1) {
        error = "That is not a date";
        sound_play_effect(SFX_ERROR);
        return;
    }

    rtc_range_t range = rtc_get_supported_range();
    if (want < range.min || want > range.max) {
        error = "Outside what this clock holds";
        sound_play_effect(SFX_ERROR);
        return;
    }

    struct timeval tv = { .tv_sec = want, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    /* Read back rather than trust the return, because the only writability check libdragon offers
     * is a deprecated inline `return true`. A joybus RTC that is present but not accepting writes
     * would otherwise leave this screen looking like it had worked. Two seconds of tolerance: the
     * clock is running while this happens. */
    time_t got = time(NULL);
    if (got < want - 2 || got > want + 2) {
        error = "The clock did not take it";
        sound_play_effect(SFX_ERROR);
        return;
    }

    sound_play_effect(SFX_ENTER);
    app_goto(app, SCREEN_SETTINGS);
}

static void clock_update (app_t *app, float dt) {
    (void)dt;
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B)) {
        sound_play_effect(SFX_EXIT);
        app_goto(app, SCREEN_SETTINGS);
        return;
    }

    if (input_pressed(in, BTN_START) || input_pressed(in, BTN_A)) {
        save(app);
        return;
    }

    if (in->right && cursor < F_COUNT - 1) {
        cursor++;
        sound_play_effect(SFX_CURSOR);
    }
    if (in->left && cursor > 0) {
        cursor--;
        sound_play_effect(SFX_CURSOR);
    }

    int step = (in->up ? 1 : 0) - (in->down ? 1 : 0);
    if (step != 0) {
        step_field(step);
        error = NULL;
        sound_play_effect(SFX_SETTING);
    }
}

/** @brief The value in @p f, as it should read on screen. */
static void field_text (field_t f, char *out, size_t cap) {
    switch (f) {
        case F_YEAR:   snprintf(out, cap, "%d", edit.tm_year + 1900); break;
        case F_MONTH:  snprintf(out, cap, "%s", MONTHS[edit.tm_mon]); break;
        case F_DAY:    snprintf(out, cap, "%d", edit.tm_mday); break;
        case F_HOUR:   snprintf(out, cap, "%02d", edit.tm_hour); break;
        case F_MINUTE: snprintf(out, cap, "%02d", edit.tm_min); break;
        default:       snprintf(out, cap, "?"); break;
    }
}

static void clock_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    char buf[32];

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "Clock");
    if (error != NULL) {
        ui_label(SAFE_X, 36, SAFE_W, ALIGN_RIGHT, STL_YELLOW, error);
    }

    for (int i = 0; i < F_COUNT; i++) {
        int x = LIST_X + FIELD_X[i];
        int w = FIELD_W[i];
        if (i == cursor) {
            ui_fill(x, FIELDS_Y - 28, w, FIELD_H, th->panel_alt);
            ui_fill(x, FIELDS_Y - 28, w, ACCENT_BAR, th->tab_underline);
        }
        field_text((field_t)i, buf, sizeof(buf));
        ui_label(x, FIELDS_Y, w, ALIGN_CENTER, i == cursor ? STL_DEFAULT : STL_GRAY, buf);
    }

    /* The colon belongs to the reader, not to a field: without it the two numbers on the right
     * are just two numbers. Drawn between the hour and minute boxes rather than inside either. */
    ui_label(LIST_X + FIELD_X[F_HOUR] + FIELD_W[F_HOUR], FIELDS_Y, 14, ALIGN_CENTER,
             STL_GRAY, ":");

    /* The cartridge, not the console. A stock N64 has no clock at all -- the battery-backed one
     * libdragon reports over Joybus is on the flashcart, which is what keeps the time when the
     * power goes. Saying "the console's clock" pointed the reader at the wrong piece of hardware
     * to go and check when the time comes back wrong. */
    ui_label(LIST_X, FIELDS_Y + 60, SAFE_W, ALIGN_LEFT, STL_GRAY,
             rtc_get_source() == RTC_SOURCE_NONE
                 ? "No clock here: the time is lost at power off."
                 : "Kept by the cartridge's clock.");

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    int hx = ui_hint(SAFE_X, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC, "Set");
    (void)hx;
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, "Cancel");

    rdpq_detach_show();
}

const screen_t SCREEN_CLOCK_DEF = {
    .id     = SCREEN_CLOCK,
    .enter  = clock_enter,
    .update = clock_update,
    .render = clock_render,
};
