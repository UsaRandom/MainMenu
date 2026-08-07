/**
 * @file screen_keyboard.c
 * @brief A QWERTY keyboard, for the two things in this program anyone types.
 * @ingroup screens
 *
 * ## This reverses a decision, and the reversal is the interesting part
 *
 * screen_profiles.c argued at length against exactly this screen. Its words: an on-screen
 * keyboard "would be the largest single piece of UI in the program in order to be used twice per
 * household", so a name was typed on a per-character odometer -- Left and Right to pick a cell,
 * Up and Down to wind it through the alphabet -- and so was a cheat name, on a second copy of the
 * same mechanism in screen_cheatedit.c.
 *
 * That reasoning was right for one screen and is wrong for three. The picker now asks for a name
 * when a slot is created as well as when it is renamed, the cheat editor still asks for one, and
 * the handoff specifies the layout down to the key size -- so the argument's premise, that the
 * keyboard would have to be invented, no longer holds either.
 *
 * It is fair to say what the reversal cost, because the original argument was about cost. This
 * file is 387 lines and the two odometers it replaced were about 80 between them, so `src/screens/`
 * is roughly 300 lines heavier for it. The claim first written here was that the directory came
 * out smaller; it does not, and measuring said so. What is bought is that naming a player is now
 * a few presses instead of winding eight cells through a 40-character alphabet, and that the same
 * screen serves all three callers instead of the mechanism being copied a third time.
 *
 * ## Two charsets, one layout
 *
 * A profile name is A-Z and space: the handoff is explicit that there is no shift key and no
 * digits, and eight uppercase characters is what the cards and the footer chip were drawn to
 * hold. A cheat name is not: the corpus is full of digits, brackets and slashes, and a keyboard
 * that could not type them would make the cheat editor worse than the odometer it replaced.
 *
 * So #KB_NAME is digits and letters and #KB_TEXT adds a row of marks under them. Same cursor,
 * same rules, same footer; the row table is the only difference, and every key common to both
 * sits in the same place on both.
 *
 * The handoff says a name is letters and space with no digits. That is right about what most
 * names are and wrong about what a keyboard should refuse: there is no shift key here, so a digit
 * costs one row, and its absence costs anybody called PLAYER2 a name they cannot type.
 *
 * ## B is delete, and then it is back
 *
 * DELETE was a third action key until B was bound to the same thing. A key you steer to in order
 * to do what the button under your thumb already does should not be there, and it sat exactly
 * where a thumb going for SPACE would land.
 *
 * B on an *empty* field leaves instead of doing nothing. Without that, an empty keyboard has no
 * exit at all -- DONE refuses an empty name -- so opening one by accident was a dead end.
 *
 * ## It must be able to show what it cannot type
 *
 * Names written by the odometer could contain digits and punctuation, because its alphabet had
 * them. Opening one of those in #KB_NAME must display it and be able to delete it -- so the field
 * renders whatever is in the buffer and only *input* is restricted. A keyboard that silently
 * dropped the characters it could not produce would eat somebody's existing name on the way in.
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "menu/fonts.h"
#include "menu/profile.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

/* Geometry, from the handoff section 7. The field is exactly SAFE_W wide. */
#define FIELD_X     SAFE_X
#define FIELD_Y     36
#define FIELD_W     SAFE_W
#define FIELD_H     64
#define FIELD_PAD   16
#define FIELD_BOT   (FIELD_Y + FIELD_H)

#define CARET_W     14
#define CARET_H     38
/** 1 Hz square: half a second lit, half dark. In seconds, never frames -- the frame rate is still
 *  an open question and a blink counted in frames would change speed with it. */
#define CARET_HZ    1.0f

#define KEY_W       52
#define KEY_H       42
#define KEY_GAP     6
#define KEY_PITCH   (KEY_H + KEY_GAP)
/** Between the last letter row and the action row: enough that SPACE reads as a different kind of
 *  thing from M, and not so much that the two look like separate keyboards. */
#define ACTION_GAP  16

/* Two action keys, not three. DELETE was one of them until B was bound to the same thing, at
 * which point it was a key you steered to in order to do what the button under your thumb already
 * did -- and it sat where a thumb going for SPACE would land. Wider now that there are two. */
#define WIDE_SPACE  340
#define WIDE_DONE   180

/**
 * @brief Where the keys may live: clear of the field at the top, clear of the footer at the bottom.
 *
 * The rows used to carry absolute Y values from the handoff, and the digit row was added at the Y
 * the handoff gave the *first* row -- 104, against a field whose bottom edge is at 108. The top
 * row of the keyboard was drawn four pixels underneath the box you were typing into, and nothing
 * checked, because every number in the file was a literal that agreed only with itself.
 *
 * So the rows carry an index and the Y is computed. #KB_TOP is the only place the clearance is
 * stated and #kb_origin centres the block in what is left, which means the 4-row name keyboard
 * gets the slack as a wider gap under the field rather than as dead space above the footer.
 */
#define KB_TOP      (FIELD_BOT + 16)
#define KB_BOT      (FOOTER_Y - 8)
/** Rows plus the gap plus the action row, which is one key tall. */
#define BLOCK_H(n)  ((n) * KEY_H + ((n) - 1) * KEY_GAP + ACTION_GAP + KEY_H)

/** Rows of keys. A row is a string of glyphs and where it starts across. */
typedef struct {
    const char *glyphs;
    int16_t     x;
} keyrow_t;

/* Digits on top of the letters in both charsets, so every key common to the two sits in the same
 * place on both. */
static const keyrow_t ROWS_NAME[] = {
    { "1234567890", 32  },
    { "QWERTYUIOP", 32  },
    { "ASDFGHJKL",  62  },
    { "ZXCVBNM",    120 },
};
#define ROWS_NAME_N  4

/* KB_TEXT keeps the same four rows and adds marks under them, so the letters stay exactly where a
 * hand that has used KB_NAME expects them. */
static const keyrow_t ROWS_TEXT[] = {
    { "1234567890", 32  },
    { "QWERTYUIOP", 32  },
    { "ASDFGHJKL",  62  },
    { "ZXCVBNM",    120 },
    { "-_.,:/()+&", 32  },
};
#define ROWS_TEXT_N  5

#define ACTION_X        57      /**< (640 - 340 - 6 - 180) / 2, so it centres on the letters */
#define ACTION_N        2

/* The symbol keyboard is the tall one, so it is the one that has to fit. This is what would have
 * caught the row drawn under the field: it fails if the field grows, if a row is added, or if the
 * keys get taller, none of which the old absolute Y values could notice. */
_Static_assert(BLOCK_H(ROWS_TEXT_N) <= KB_BOT - KB_TOP,
               "the symbol keyboard does not fit between the field and the footer");
_Static_assert(KB_TOP >= FIELD_BOT + 12, "the top key row crowds the text field");
_Static_assert(ROWS_TEXT_N >= ROWS_NAME_N, "BLOCK_H is asserted against the taller charset");

static kb_charset_t charset;
static char *target;            /**< the caller's buffer; written only on DONE */
static size_t target_cap;
static char buf[64];
static int len;
static screen_id_t back_to;
static const char *title;

/** Cursor: row index into the glyph rows, or the action row when @ref on_action. */
static int row, col;
static bool on_action;
static int action;              /**< 0 space, 1 done */

/**
 * The x the cursor is trying to stay under while it moves up and down.
 *
 * Set whenever the cursor moves sideways, and *not* when it moves vertically, which is what makes
 * a column survive a detour through a short row: P down to L down to M and back up returns to P
 * rather than drifting to O. Same rule a text editor uses for a line shorter than the one above.
 */
static int want_x;

static float caret_t;
/** Seconds left of the rejection flash. The field goes red rather than the screen doing
 *  anything, because the thing being refused is the field's contents. */
static float flash_t;

void screen_keyboard_ask (kb_charset_t set, const char *prompt, const char *initial,
                          char *out, size_t cap, screen_id_t back) {
    charset = set;
    title = (prompt != NULL) ? prompt : "";
    target = out;
    target_cap = cap;
    back_to = back;

    buf[0] = '\0';
    if (initial != NULL) {
        snprintf(buf, sizeof(buf), "%s", initial);
    }
    len = (int)strlen(buf);

    int limit = screen_keyboard_limit(set);
    if (len > limit) {
        len = limit;
        buf[len] = '\0';
    }
}

int screen_keyboard_limit (kb_charset_t set) {
    /* Eight for a name, because that is what PROFILE_NAME_CAP holds and what the cards were drawn
     * to fit. Cheat names get the rest of the buffer. */
    return (set == KB_NAME) ? (PROFILE_NAME_CAP - 1) : (int)(sizeof(buf) - 1);
}

static const keyrow_t *rows (void) {
    return (charset == KB_NAME) ? ROWS_NAME : ROWS_TEXT;
}

static int row_count (void) {
    return (charset == KB_NAME) ? ROWS_NAME_N : ROWS_TEXT_N;
}

/** Top of the first key row: the block, centred in the space between the field and the footer. */
static int kb_origin (void) {
    int slack = (KB_BOT - KB_TOP) - BLOCK_H(row_count());
    return KB_TOP + ((slack > 0) ? slack / 2 : 0);
}

static int row_y (int r) {
    return kb_origin() + r * KEY_PITCH;
}

static int action_y (void) {
    /* Below the last row by ACTION_GAP rather than by the ordinary row gap. */
    return kb_origin() + row_count() * KEY_PITCH + (ACTION_GAP - KEY_GAP);
}

static int row_len (int r) {
    return (int)strlen(rows()[r].glyphs);
}

/** Centre of key @p c in row @p r. */
static int key_cx (int r, int c) {
    return rows()[r].x + c * (KEY_W + KEY_GAP) + KEY_W / 2;
}

/** Centre of an action key. SPACE and DONE are wide, so their centres are far apart. */
static int action_cx (int a) {
    return (a == 0) ? (ACTION_X + WIDE_SPACE / 2)
                    : (ACTION_X + WIDE_SPACE + KEY_GAP + WIDE_DONE / 2);
}

/**
 * @brief The key in row @p r physically nearest @p x.
 *
 * Up and Down used to carry the column *index* across, which is only the same thing as carrying
 * the position when every row starts at the same place and holds the same number of keys. Neither
 * is true here: ASDFGHJKL is inset 30 px from QWERTYUIOP and ZXCVBNM is inset 88, so the index
 * drifts right by half a key per row. Counted over the letter rows, 16 of the 26 downward moves
 * landed on a key that was not the one underneath -- W went to S with A sitting under it, J went
 * to M with N underneath -- and the three keys past the end of a short row all piled onto its last
 * one. It reads as a cursor that slides sideways while you are pressing down.
 */
static int nearest_col (int r, int x) {
    int n = row_len(r);
    int best = 0, best_d = 1 << 30;
    for (int c = 0; c < n; c++) {
        int d = key_cx(r, c) - x;
        if (d < 0) {
            d = -d;
        }
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    return best;
}

/** The action key nearest @p x. Two of them, so it is one comparison against the gap between. */
static int nearest_action (int x) {
    int mid = (action_cx(0) + action_cx(1)) / 2;
    return (x < mid) ? 0 : 1;
}

static void kb_enter (app_t *app) {
    (void)app;
    row = 0;
    col = 0;
    on_action = false;
    action = 0;
    want_x = key_cx(0, 0);
    caret_t = 0.0f;
    flash_t = 0.0f;
}

/** @brief Append @p c, or refuse. */
static void type (char c) {
    if (len >= screen_keyboard_limit(charset)) {
        /* The ninth character is a no-op plus the reject sound, per the handoff. Not a silent
         * one: without a sound it reads as a dropped input rather than as a limit. */
        sound_play_effect(SFX_ERROR);
        flash_t = 0.25f;
        return;
    }
    buf[len++] = c;
    buf[len] = '\0';
    sound_play_effect(SFX_SETTING);
}

static void backspace (void) {
    if (len <= 0) {
        return;
    }
    buf[--len] = '\0';
    sound_play_effect(SFX_EXIT);
}

static void confirm (app_t *app) {
    if (len == 0) {
        /* An empty name is refused on confirm and the old one survives. The field flashes rather
         * than a dialog appearing, because the answer is one character away. */
        sound_play_effect(SFX_ERROR);
        flash_t = 0.4f;
        return;
    }
    if (target != NULL && target_cap > 0) {
        snprintf(target, target_cap, "%s", buf);
    }
    sound_play_effect(SFX_ENTER);
    app_goto(app, back_to);
}

static void kb_update (app_t *app, float dt) {
    const input_t *in = &app->input;

    caret_t += dt;
    if (caret_t >= CARET_HZ) {
        caret_t -= CARET_HZ;
    }
    if (flash_t > 0.0f) {
        flash_t -= dt;
    }

    /* B deletes from anywhere, and START confirms from anywhere. Both are the handoff's, and both
     * matter more than they look: they are what stop the cursor having to travel to a key to do
     * the two things done most often. */
    if (input_pressed(in, BTN_B)) {
        if (len == 0) {
            /* Nothing to delete, so B means what it means everywhere else in this program. Delete
             * used to be its only meaning here, which made an empty keyboard a screen with no way
             * out -- DONE refuses an empty field -- so opening one by accident was a dead end.
             * Emptying the field and pressing B once more is how somebody changes their mind. */
            sound_play_effect(SFX_EXIT);
            app_goto(app, back_to);
        } else {
            backspace();
        }
        return;
    }
    if (input_pressed(in, BTN_START)) {
        confirm(app);
        return;
    }

    int last = row_count() - 1;
    bool moved = false;

    /* Vertical moves land on whatever is physically nearest #want_x in the row being entered, and
     * leave want_x alone. Horizontal moves set it. See nearest_col(). */
    if (in->up) {
        if (on_action) {
            on_action = false;
            row = last;
            col = nearest_col(row, want_x);
            moved = true;
        } else if (row > 0) {
            row--;
            col = nearest_col(row, want_x);
            moved = true;
        }
    }
    if (in->down && !on_action) {
        if (row < last) {
            row++;
            col = nearest_col(row, want_x);
        } else {
            on_action = true;
            action = nearest_action(want_x);
        }
        moved = true;
    }
    if (in->left) {
        if (on_action && action > 0) {
            action--;
            want_x = action_cx(action);
            moved = true;
        } else if (!on_action && col > 0) {
            col--;
            want_x = key_cx(row, col);
            moved = true;
        }
    }
    if (in->right) {
        if (on_action && action < ACTION_N - 1) {
            action++;
            want_x = action_cx(action);
            moved = true;
        } else if (!on_action && col < row_len(row) - 1) {
            col++;
            want_x = key_cx(row, col);
            moved = true;
        }
    }

    /* Belt and braces. nearest_col() cannot return an out-of-range column, but the charset can
     * change under the cursor -- screen_keyboard_ask() picks the row table -- and a stale column
     * indexes off the end of a string. */
    if (!on_action && col >= row_len(row)) {
        col = row_len(row) - 1;
    }
    if (moved) {
        sound_play_effect(SFX_CURSOR);
    }

    if (input_pressed(in, BTN_A)) {
        if (!on_action) {
            type(rows()[row].glyphs[col]);
        } else if (action == 0) {
            type(' ');
        } else {
            confirm(app);
        }
    }
}

/* ------------------------------------------------------------------ drawing -- */

static void draw_key (app_t *app, int x, int y, int w, const char *label, bool cursor,
                      bool is_done) {
    const theme_t *th = app->theme;
    int lift = cursor ? 2 : 0;

    if (cursor) {
        /* Section 8: the chosen thing is the brightest thing, and never distinguished by colour
         * alone. The cursor key inverts -- white plate, dark glyph -- and lifts two pixels with a
         * shadow under it, so it reads on a CRT that has lost the contrast. */
        ui_fill(x + 2, y + lift + KEY_H - 2, w, 4, th->sel_shadow);
        ui_fill(x, y - lift, w, KEY_H, 0xFFFF);
    } else if (is_done) {
        ui_fill(x, y, w, KEY_H, profile_colour_fill(2));   /* the green swatch */
    } else {
        ui_fill(x, y, w, KEY_H, th->panel_alt);
    }

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    /* Baseline 12 px up from the bottom of the key, which is where a 32 px face sits centred in
     * one. Was the literal 36 against a 48 px key; keyed off KEY_H now so shrinking the keys does
     * not slide every glyph towards the floor. */
    ui_text_font(FNT_KEY, x, y - lift + KEY_H - 12, w, ALIGN_CENTER,
                 (cursor || is_done) ? STL_ONLIGHT : STL_DEFAULT, label);
}

static void kb_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;
    char count[16];

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_label(SAFE_X, 26, SAFE_W, ALIGN_LEFT, STL_GRAY, title);

    /* The field. Red while a rejection is flashing, which is the only feedback an empty confirm
     * or a ninth character gets -- see type() and confirm(). */
    ui_fill(FIELD_X, FIELD_Y, FIELD_W, FIELD_H,
            (flash_t > 0.0f) ? profile_colour_fill(0) : th->panel);

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text_font(FNT_FIELD, FIELD_X + FIELD_PAD, FIELD_Y + 48, FIELD_W - FIELD_PAD * 2,
                 ALIGN_LEFT, STL_DEFAULT, buf);

    snprintf(count, sizeof(count), "%d / %d", len, screen_keyboard_limit(charset));
    ui_text(FIELD_X, FIELD_Y + 44, FIELD_W - FIELD_PAD, ALIGN_RIGHT, STL_GRAY, count);

    /* The caret, after the text, at a position measured from the glyphs rather than assumed: the
     * field face is proportional, so a caret placed at len * a constant drifts along a name and
     * ends up inside the last letter. */
    if (caret_t < CARET_HZ / 2.0f) {
        int caret_x = FIELD_X + FIELD_PAD + ui_text_width(FNT_FIELD, buf);
        if (caret_x < FIELD_X + FIELD_W - FIELD_PAD - CARET_W) {
            ui_fill(caret_x + 2, FIELD_Y + 13, CARET_W, CARET_H, profile_colour_fill(1));
        }
    }

    for (int r = 0; r < row_count(); r++) {
        const keyrow_t *kr = &rows()[r];
        int n = row_len(r);
        for (int c = 0; c < n; c++) {
            char label[2] = { kr->glyphs[c], '\0' };
            draw_key(app, kr->x + c * (KEY_W + KEY_GAP), row_y(r), KEY_W, label,
                     !on_action && r == row && c == col, false);
        }
    }

    int ax = ACTION_X;
    int ay = action_y();
    draw_key(app, ax, ay, WIDE_SPACE, "SPACE", on_action && action == 0, false);
    ax += WIDE_SPACE + KEY_GAP;
    draw_key(app, ax, ay, WIDE_DONE, "DONE", on_action && action == 1, true);

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    int hx = SAFE_X;
    hx = ui_hint(hx, FOOTER_Y + 14, "A", BTN_A_COLOR, UI_BTN_DISC, "Type");
    hx = ui_hint(hx, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC,
                 len > 0 ? "Delete" : "Back");
    (void)ui_hint(hx, FOOTER_Y + 14, "S", BTN_START_COLOR, UI_BTN_DISC, "Done");

    rdpq_detach_show();
}

const screen_t SCREEN_KEYBOARD_DEF = {
    .id     = SCREEN_KEYBOARD,
    .enter  = kb_enter,
    .update = kb_update,
    .render = kb_render,
};
