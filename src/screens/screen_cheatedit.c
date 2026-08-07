/**
 * @file screen_cheatedit.c
 * @brief Typing in a cheat with a controller.
 * @ingroup screens
 *
 * Reached with R from the cheats list for a new cheat, or with Z to edit the one under the cursor.
 * The shipped database covers a few hundred N64 titles and nothing else, so for a homebrew ROM, an
 * emulated-system title, or a code published after the corpus was built, this is the only way in.
 *
 * **Editing works on shipped cheats too**, which is the reason Z exists: a published cheat is
 * often only useful once a value is changed, and `cheats.db` is read-only. Saving always writes a
 * user cheat, and a user cheat takes over any group of the same name rather than appearing beside
 * it. See usercheats.h. What cannot be represented here -- more than USERCHEAT_MAX_LINES lines, or
 * a name longer than the store holds -- is refused by the list rather than truncated by this
 * screen; see screen_cheatedit_can_edit().
 *
 * ## One mode, not two
 *
 * The obvious design is a hex keypad you steer a cursor around, plus a separate keyboard for the
 * name. That is two grids, two modes, and a lot of travel per character. Instead everything on
 * this screen is **one strip of cells** -- the name's characters and then each line's twelve
 * nibbles -- and the controls are the same wherever the cursor is:
 *
 *     left/right   move a cell, wrapping from the end of one row to the start of the next
 *     up/down      change the character or nibble under the cursor
 *     L/R          jump a whole row, because walking twelve nibbles to reach the next line is not
 *     A            add a line          Z  remove one          Start  save          B  cancel
 *
 * It is the arcade high-score idiom, which is the one text-entry pattern a console player already
 * knows, and it means there is no mode to be in the wrong one of.
 *
 * ## Whole groups, always
 *
 * The lines entered here are one named GROUP, and the cheats list can only toggle the group. That
 * is the same constraint the shipped database is built around and it is a correctness one, not a
 * simplification: a `D0` conditional and the write it guards are indivisible, and a published
 * two-line code that could be half-enabled would patch an address nobody asked for. See
 * cheatdb.h and AUDIT.md 2.2.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "cheats/cheatdb.h"
#include "cheats/usercheats.h"
#include "library/playstate.h"
#include "menu/fonts.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

#define LIST_X      SAFE_X
#define NAME_Y      120
#define LINES_Y     176
#define ROW_H       32
#define CELL_W      20                       /**< one editable cell, wide enough to box a glyph */
/* How many characters can be TYPED, which is a screen-width question and nothing else: 23 cells
 * is 460 px against SAFE_W's 608. It is deliberately not USERCHEAT_NAME_CAP, which is how long a
 * name can be STORED. Measured against the libretro corpus, only 80.5% of cheat names fit in 23
 * characters but 99.4% fit in 63, and a cheat that cannot be opened is worse than one whose name
 * cannot be retyped -- so a longer name is shown as a label and only its codes are editable. */
#define NAME_CELLS  23
#define HEX_CELLS   12                       /**< 8 address nibbles + 4 value nibbles */

/** The name alphabet, in the order up/down walks it. Space first so a short name is the default. */

static char    name[USERCHEAT_NAME_CAP];
static uint8_t nibbles[USERCHEAT_MAX_LINES][HEX_CELLS];
static int     line_count;
static int     cursor;                       /**< 0..name_cells-1 is the name, then 12 per line */
/**
 * @brief Cursor positions the name occupies. Always 1.
 *
 * It was NAME_CELLS -- twenty-three, one per character -- because the name was typed on a
 * per-character odometer sharing this screen's single cursor with the hex nibbles. The keyboard
 * replaced that, so the whole name is one stop: A on it opens screen_keyboard.c with KB_TEXT,
 * which is the charset with digits and marks in it, because cheat names in the corpus are full
 * of both.
 *
 * Kept as a named variable rather than folded away, because cursor_row(), cursor_cell() and
 * total_cells() are all written in terms of it and a literal 1 in three places is how the name
 * row and the hex rows would stop agreeing about where the boundary is.
 *
 * It was also 0 for a name too long to fit the cell strip, which made that name uneditable. The
 * keyboard has no such limit, so that case is gone.
 */
static int     name_cells = 1;
/** Set while the keyboard has the screen, so enter() knows not to reset everything. */
static bool    pending_name_edit;
static const char *error;
static bool    editing;                      /**< opened on an existing cheat rather than a blank */

/** Armed by screen_cheatedit_open() and consumed by enter(). See screens.h. */
static const cheat_group_t *pending_group;
static const cheat_code_t  *pending_codes;

bool screen_cheatedit_can_edit (const cheat_group_t *g) {
    /* The name has to survive being stored, because storing it under a different name is what
     * turns an edit into a duplicate. Everything else about it is presentation. */
    return g != NULL
        && g->count >= 1 && g->count <= USERCHEAT_MAX_LINES
        && strlen(g->name) < USERCHEAT_NAME_CAP;
}

void screen_cheatedit_open (const cheat_group_t *g, const cheat_code_t *codes) {
    pending_group = g;
    pending_codes = codes;
}

/** @brief Which row the cursor is on: -1 for the name, otherwise the line index. */
static int cursor_row (void) {
    return (cursor < name_cells) ? -1 : (cursor - name_cells) / HEX_CELLS;
}

static int cursor_cell (void) {
    return (cursor < name_cells) ? cursor : (cursor - name_cells) % HEX_CELLS;
}

static int total_cells (void) {
    return name_cells + line_count * HEX_CELLS;
}

static void cheatedit_enter (app_t *app) {
    (void)app;
    /* Coming back from the keyboard, which wrote straight into `name`. Everything else on this
     * screen -- the nibbles, the line count, which group is being edited -- has to survive that
     * round trip, so nothing below runs. */
    if (pending_name_edit) {
        pending_name_edit = false;
        return;
    }

    memset(nibbles, 0, sizeof(nibbles));
    cursor = 0;
    error = NULL;
    editing = (pending_group != NULL);

    if (editing) {
        snprintf(name, sizeof(name), "%s", pending_group->name);
        /* A name that will not fit the cell strip becomes a label. Truncating it to fit would
         * change it, and a changed name is a new cheat rather than a replacement -- the list would
         * end up holding the original and the edit under two nearly identical names. */
        line_count = pending_group->count;
        for (int r = 0; r < line_count; r++) {
            const cheat_code_t *c = &pending_codes[pending_group->first + r];
            for (int i = 0; i < 8; i++) {
                nibbles[r][i] = (uint8_t)((c->address >> (28 - 4 * i)) & 0xF);
            }
            for (int i = 0; i < 4; i++) {
                nibbles[r][8 + i] = (uint8_t)((c->value >> (12 - 4 * i)) & 0xF);
            }
        }
    } else {
        /* A default name rather than an empty one. An unnamed group would draw as a blank row in
         * the cheats list, and the user has to press up sixteen times from space to reach A. */
        snprintf(name, sizeof(name), "NEW CHEAT");
        line_count = 1;
    }

    /* Consumed, so a later entry that forgets to arm gets a blank cheat rather than the last one
     * edited -- and so nothing holds a pointer into groups[] across a realloc. */
    pending_group = NULL;
    pending_codes = NULL;
}

/** @brief Pack a row of nibbles into the address/value pair the engine wants. */
static void pack_line (int row, cheat_code_t *out) {
    uint32_t addr = 0, val = 0;
    for (int i = 0; i < 8; i++) {
        addr = (addr << 4) | nibbles[row][i];
    }
    for (int i = 8; i < HEX_CELLS; i++) {
        val = (val << 4) | nibbles[row][i];
    }
    out->address = addr;
    out->value = val;
}

/** @brief Is every nibble on @p row zero? Used to refuse saving a line nobody filled in. */
static bool row_blank (int row) {
    for (int i = 0; i < HEX_CELLS; i++) {
        if (nibbles[row][i] != 0) {
            return false;
        }
    }
    return true;
}

static void save (app_t *app) {
    /* A blank line is not a cheat, and 00000000 0000 is a write to address zero rather than a
     * harmless no-op -- boot/cheats.c would happily assemble it. Refused rather than dropped,
     * because silently discarding what someone just typed is worse than saying no. */
    for (int i = 0; i < line_count; i++) {
        if (row_blank(i)) {
            error = "Every line needs a code";
            sound_play_effect(SFX_ERROR);
            return;
        }
    }

    cheat_code_t lines[USERCHEAT_MAX_LINES];
    for (int i = 0; i < line_count; i++) {
        pack_line(i, &lines[i]);
    }

    /* Trailing spaces would survive into the list and into the cheatstate name hash, so a name
     * that gained a space on the way past would be remembered as a different cheat. */
    char trimmed[USERCHEAT_NAME_CAP];
    snprintf(trimmed, sizeof(trimmed), "%s", name);
    int end = (int)strlen(trimmed);
    while (end > 0 && trimmed[end - 1] == ' ') {
        trimmed[--end] = '\0';
    }
    if (end == 0) {
        error = "Give it a name";
        sound_play_effect(SFX_ERROR);
        return;
    }

    if (app->launch.rom_id < 0) {
        /* No record means no key, and key 0 would file this cheat against every future game that
         * also failed to produce one. Not reachable from the grid, which always sets rom_id; the
         * cost of being wrong about that is a cheat attached to the wrong title. */
        error = "No game selected";
        sound_play_effect(SFX_ERROR);
        return;
    }
    uint64_t key = playstate_key(&app->lib->records[app->launch.rom_id]);
    if (!usercheats_add(&app->cheats, key, trimmed, lines, line_count)) {
        error = "No room for another cheat";
        sound_play_effect(SFX_ERROR);
        return;
    }

    sound_play_effect(SFX_ENTER);
    app_goto(app, SCREEN_CHEATS);
}

static void cheatedit_update (app_t *app, float dt) {
    (void)dt;
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B)) {
        sound_play_effect(SFX_EXIT);
        app_goto(app, SCREEN_CHEATS);
        return;
    }

    if (input_pressed(in, BTN_START)) {
        save(app);
        return;
    }

    if (input_pressed(in, BTN_A) && cursor < name_cells) {
        pending_name_edit = true;
        screen_keyboard_ask(KB_TEXT, "Name this cheat", name,
                            name, sizeof(name), SCREEN_CHEATEDIT);
        app_goto(app, SCREEN_KEYBOARD);
        return;
    }

    if (input_pressed(in, BTN_A) && line_count < USERCHEAT_MAX_LINES) {
        line_count++;
        cursor = name_cells + (line_count - 1) * HEX_CELLS;
        error = NULL;
        sound_play_effect(SFX_SETTING);
        return;
    }

    if (input_pressed(in, BTN_Z) && line_count > 1) {
        int row = cursor_row();
        if (row < 0) {
            row = line_count - 1;
        }
        for (int i = row; i < line_count - 1; i++) {
            memcpy(nibbles[i], nibbles[i + 1], sizeof(nibbles[0]));
        }
        memset(nibbles[line_count - 1], 0, sizeof(nibbles[0]));
        line_count--;
        if (cursor >= total_cells()) {
            cursor = total_cells() - 1;
        }
        error = NULL;
        sound_play_effect(SFX_EXIT);
        return;
    }

    int n = total_cells();
    if (in->right) {
        cursor = (cursor + 1) % n;
        sound_play_effect(SFX_CURSOR);
    }
    if (in->left) {
        cursor = (cursor + n - 1) % n;
        sound_play_effect(SFX_CURSOR);
    }
    if (input_pressed(in, BTN_R)) {
        cursor = (cursor < name_cells) ? name_cells
               : name_cells + ((cursor_row() + 1) % line_count) * HEX_CELLS;
        sound_play_effect(SFX_CURSOR);
    }
    if (input_pressed(in, BTN_L)) {
        cursor = (cursor < name_cells) ? name_cells + (line_count - 1) * HEX_CELLS : 0;
        sound_play_effect(SFX_CURSOR);
    }

    int step = (in->up ? 1 : 0) - (in->down ? 1 : 0);
    if (step != 0) {
        error = NULL;
        if (cursor < name_cells) {
            /* Nothing. Up and Down wind a hex nibble; the name is typed on the keyboard, which A
             * opens. Winding a name one letter at a time is what this screen used to do and what
             * the keyboard exists to stop. */
        } else {
            int row = cursor_row(), cell = cursor_cell();
            nibbles[row][cell] = (uint8_t)((nibbles[row][cell] + step + 16) % 16);
        }
        sound_play_effect(SFX_SETTING);
    }
}

/** @brief One row of boxed cells. @p sel is the cell to highlight, or -1. */
static void draw_cells (const theme_t *th, int x, int y, const char *glyphs, int n, int sel,
                        int gap_after) {
    char one[2] = { 0, 0 };
    for (int i = 0; i < n; i++) {
        int cx = x + i * CELL_W + (gap_after >= 0 && i >= gap_after ? 14 : 0);
        if (i == sel) {
            ui_fill(cx - 2, y - 20, CELL_W, 26, th->text_accent);
        }
        one[0] = glyphs[i];
        ui_label(cx, y, CELL_W, ALIGN_LEFT, i == sel ? STL_ONBTN : STL_DEFAULT, one);
    }
    ui_fill(x, y + 6, n * CELL_W + (gap_after >= 0 ? 14 : 0), HAIRLINE, th->panel_alt);
}

static void cheatedit_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    char glyphs[HEX_CELLS + 1];
    char buf[64];

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, 64, th->panel);
    ui_fill(0, 64 - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, editing ? "Edit cheat" : "New cheat");

    /* The message shares the header's right-hand slot with the line counter rather than sitting
     * under the rows. It used to be drawn at LINES_Y + 8*ROW_H + 8 = y 440, which is inside the
     * footer -- and the footer is filled afterwards, so every refusal to save was painted over
     * before it reached the screen. */
    if (error != NULL) {
        ui_label(SAFE_X, 36, SAFE_W, ALIGN_RIGHT, STL_YELLOW, error);
    } else {
        snprintf(buf, sizeof(buf), "%d of %d lines", line_count, USERCHEAT_MAX_LINES);
        ui_label(SAFE_X, 36, SAFE_W, ALIGN_RIGHT, STL_GRAY, buf);
    }

    ui_label(LIST_X, NAME_Y - 26, SAFE_W, ALIGN_LEFT, STL_GRAY, "Name");
    if (name_cells > 0) {
        bool here = (cursor < name_cells);
        if (here) {
            ui_fill(LIST_X - 4, NAME_Y - 20, NAME_CELLS * CELL_W + 8, 26, th->text_accent);
        }
        ui_label(LIST_X, NAME_Y, NAME_CELLS * CELL_W, ALIGN_LEFT,
                 here ? STL_ONBTN : STL_DEFAULT, name);
    } else {
        /* Too long for the strip, so it is shown rather than offered. Drawn dim to say that,
         * without a second sentence explaining it. */
        ui_label(LIST_X, NAME_Y, SAFE_W, ALIGN_LEFT, STL_GRAY, name);
        ui_fill(LIST_X, NAME_Y + 6, NAME_CELLS * CELL_W, HAIRLINE, th->panel_alt);
    }

    ui_label(LIST_X, LINES_Y - 26, SAFE_W, ALIGN_LEFT, STL_GRAY, "Address     Value");
    for (int r = 0; r < line_count; r++) {
        for (int i = 0; i < HEX_CELLS; i++) {
            glyphs[i] = "0123456789ABCDEF"[nibbles[r][i]];
        }
        glyphs[HEX_CELLS] = '\0';
        int sel = (cursor_row() == r) ? cursor_cell() : -1;
        draw_cells(th, LIST_X, LINES_Y + r * ROW_H, glyphs, HEX_CELLS, sel, 8);
    }

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    int hx = SAFE_X;
    hx = ui_hint(hx, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC, "Add line");
    hx = ui_hint(hx, FOOTER_Y + 14, "Z", BTN_Z_COLOR, UI_BTN_TALL, "Remove");
    hx = ui_hint(hx, FOOTER_Y + 14, "S", BTN_START_COLOR, UI_BTN_DISC, "Save");
    (void)hx;
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, "Cancel");

    rdpq_detach_show();
}

const screen_t SCREEN_CHEATEDIT_DEF = {
    .id     = SCREEN_CHEATEDIT,
    .enter  = cheatedit_enter,
    .update = cheatedit_update,
    .render = cheatedit_render,
};
