/**
 * @file screen_fault.c
 * @brief Unrecoverable fault. Designed to be read off a phone photo.
 * @ingroup screens
 *
 * docs/design/README.md section 4.10. Everything is 20 px or larger, the diagnostic block is
 * fixed-position and fixed-order so a support reader scans it the same way every time, and
 * there are no footer hints because there is nothing to press.
 */

#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "menu/sound.h"
#include "menu/diagreport.h"
#include "menu/fonts.h"
#include "screens.h"
#include "ui/theme.h"

static void fill_rect (int x, int y, int w, int h, uint16_t c) {
    rdpq_set_mode_fill(color_from_packed16(c));
    rdpq_fill_rectangle(x, y, x + w, y + h);
}

static void line (int x, int y, int w, rdpq_align_t align, int style, const char *s) {
    rdpq_text_printn(&(rdpq_textparms_t){ .width = w, .align = align, .disable_aa_fix = true },
                     FNT_DEFAULT, x, y, s, strlen(s));
}

static void fault_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;

    rdpq_attach(fb, NULL);
    rdpq_set_mode_fill(color_from_packed16(th->bg));
    rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H);

    /* The one red on the screen, bleeding edge to edge so it survives a photo at an angle. */
    fill_rect(0, 0, SCREEN_W, 8, BTN_START_COLOR);

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);

    /* The same screen serves the cheat diagnostic, and deliberately so. It is render-only: no
     * update function, no input, nothing that can dismiss it -- which after five diagnostics lost
     * to their own reporting is the property that matters most. See diagreport.h. */
    if (diag_report_count() > 0) {
        line(SAFE_X, 40, SAFE_W, ALIGN_LEFT, STL_RED, "C H E A T   D I A G N O S T I C");
        int y = 92;
        for (int i = 0; i < diag_report_count(); i++) {
            line(SAFE_X, y, SAFE_W, ALIGN_LEFT, STL_DEFAULT, diag_report_line(i));
            y += 26;
        }
        line(SAFE_X, 440, SAFE_W, ALIGN_LEFT, STL_GRAY,
             "Photograph this, then power off. The game was not booted.");
        rdpq_detach_show();
        return;
    }

    line(SAFE_X, 40, SAFE_W, ALIGN_LEFT, STL_RED, "F A U L T");
    line(SAFE_X, 84, SAFE_W, ALIGN_LEFT, STL_DEFAULT,
         app->fault_message ? app->fault_message : "Unknown fault.");

    line(SAFE_X, 150, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "The menu cannot continue.");
    line(SAFE_X, 176, SAFE_W, ALIGN_LEFT, STL_DEFAULT,
         "Power off, check the cartridge is seated, and try again.");

    fill_rect(SAFE_X, 216, SAFE_W, 160, th->panel);
    line(SAFE_X + 12, 248, 180, ALIGN_LEFT, STL_GRAY,  "STORAGE");
    line(SAFE_X + 200, 248, 380, ALIGN_LEFT, STL_DEFAULT,
         app->storage ? app->storage : "(none)");
    line(SAFE_X + 12, 280, 180, ALIGN_LEFT, STL_GRAY,  "EXPANSION PAK");
    line(SAFE_X + 200, 280, 380, ALIGN_LEFT, STL_DEFAULT,
         is_memory_expanded() ? "PRESENT" : "ABSENT");
    line(SAFE_X + 12, 312, 180, ALIGN_LEFT, STL_GRAY,  "TITLES");
    line(SAFE_X + 200, 312, 380, ALIGN_LEFT, STL_DEFAULT,
         app->lib ? "SCANNED" : "NOT SCANNED");

    line(SAFE_X, 412, SAFE_W, ALIGN_LEFT, STL_DEFAULT,
         "Photograph this screen when asking for help.");

    rdpq_detach_show();
}

const screen_t SCREEN_FAULT_DEF = {
    .id     = SCREEN_FAULT,
    .render = fault_render,
};

void screens_register (const screen_t **table) {
    table[SCREEN_GRID]   = &SCREEN_GRID_DEF;
    table[SCREEN_DETAIL] = &SCREEN_DETAIL_DEF;
    table[SCREEN_CHEATS] = &SCREEN_CHEATS_DEF;
    table[SCREEN_CHEATEDIT] = &SCREEN_CHEATEDIT_DEF;
    table[SCREEN_SETTINGS] = &SCREEN_SETTINGS_DEF;
    table[SCREEN_PARENTAL] = &SCREEN_PARENTAL_DEF;
    table[SCREEN_LOCKS]  = &SCREEN_LOCKS_DEF;
    table[SCREEN_PROFILES] = &SCREEN_PROFILES_DEF;
    table[SCREEN_CLOCK]  = &SCREEN_CLOCK_DEF;
    table[SCREEN_CODE]   = &SCREEN_CODE_DEF;
    table[SCREEN_CREDITS] = &SCREEN_CREDITS_DEF;
    table[SCREEN_KEYBOARD] = &SCREEN_KEYBOARD_DEF;
    table[SCREEN_APPEARANCE] = &SCREEN_APPEARANCE_DEF;
    table[SCREEN_LAUNCH] = &SCREEN_LAUNCH_DEF;
    table[SCREEN_FAULT]  = &SCREEN_FAULT_DEF;
}
