/**
 * @file screen_credits.c
 * @brief Everything this program owes to someone else, in one scrolling column.
 * @ingroup screens
 *
 * Nobody is made to read this and everybody can. That is the whole design: one column of text,
 * Up and Down to scroll, B to leave. There is no navigation inside it, no sections to pick from
 * and nothing to agree to, because none of those would make a licence more read and all of them
 * would make it look like a thing standing between the player and a game.
 *
 * ## The text is data, and that is the interesting part
 *
 * Every other screen draws string literals, so a character the font cannot render is visible to
 * a build-time check over the source. This one renders `rom:/credits.txt`, baked from
 * docs/CREDITS.md by tools/mkcredits.py -- which the source check cannot see into. So the baker
 * carries the check instead, and refuses to write the file when a character falls outside
 * assets/fonts/charset.txt. Verified by pasting a smart quote and an em dash into the Markdown:
 * both are genuinely absent from the charset, both were caught, and nothing was written.
 *
 * The alternative was 150 lines of string literals in this file, which is the same text in a
 * place where it would be edited by whoever was changing the screen rather than by whoever was
 * changing what is owed.
 *
 * ## Why the heights are measured rather than assumed
 *
 * Paragraphs wrap at #SAFE_W, so a block is one to six lines depending on how long a sentence
 * somebody wrote. A fixed row pitch would either crush long paragraphs together or leave a
 * screenful of air after short ones.
 *
 * #ui_text_wrap returns the pen advance, so the height of a block is known the moment it is
 * drawn -- and scrolling only ever needs the height of blocks that have already been on screen.
 * Down needs the height of the block being scrolled off, which was drawn last frame; up needs
 * the height of the block coming back, which was drawn before it left. So the cache fills
 * itself in the order it is needed and nothing has to be measured ahead of time. That matters
 * because measuring ahead of time would mean laying out 150 paragraphs at enter(), on a screen
 * whose entire job is to appear immediately.
 *
 * The position bar is by block index rather than by pixel, because total pixel height is exactly
 * the thing this scheme never computes. Over 150 blocks the difference is not visible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "menu/fonts.h"
#include "menu/sound.h"
#include "screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

#define CREDITS_PATH    "rom:/credits.txt"

#define HEAD_H          64
#define CONTENT_Y       (HEAD_H + 16)
#define CONTENT_BOT     FOOTER_Y
#define CONTENT_H       (CONTENT_BOT - CONTENT_Y)
#define TEXT_X          SAFE_X
#define TEXT_W          (SAFE_W - POSBAR_W - 12)

/** How far one press of Up or Down moves. Held, the repeat in input.c makes this a smooth run. */
#define SCROLL_STEP     24
/** L and R, for somebody looking for one section in a page of them. */
#define SCROLL_PAGE     (CONTENT_H - 40)

/** Air under a block, by tag. A heading needs to belong to what follows it, not to what it
 *  follows, so it gets more above than below -- which the baker already arranges by emitting a
 *  blank line before every section. */
#define GAP_TEXT        6
#define GAP_TIGHT       2

/**
 * @brief One line of the baked file: a tag byte and the text after it.
 *
 * Pointers into #blob rather than copies. The file is under 5 KB and is read once; copying it
 * into 150 allocations would cost more than the file.
 */
typedef struct {
    char tag;
    const char *text;
    int16_t height;   /**< pen advance when last drawn, or -1 if it never has been */
} block_t;

static char *blob;
static block_t *blocks;
static int nblocks;

/** Index of the block at the top of the window, and how many pixels of it are above the window. */
static int top;
static int top_px;

static void free_all (void) {
    free(blob);
    free(blocks);
    blob = NULL;
    blocks = NULL;
    nblocks = 0;
}

/**
 * @brief Read the baked file and index its lines.
 *
 * A failure here is not fatal and must not be: the screen draws its own apology and B still
 * works. It would take a corrupt DFS to get here, but a licence screen that hangs the console is
 * a worse outcome than one that says it could not load.
 */
static bool load (void) {
    FILE *f = fopen(CREDITS_PATH, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (1 << 20)) {
        fclose(f);
        return false;
    }

    blob = malloc((size_t)len + 1);
    if (blob == NULL) {
        fclose(f);
        return false;
    }
    size_t got = fread(blob, 1, (size_t)len, f);
    fclose(f);
    blob[got] = '\0';

    /* Count first, then index. Two passes over 5 KB against one realloc-per-line, which is the
     * churn the file browser this menu replaced was built out of. */
    int n = 0;
    for (size_t i = 0; i < got; i++) {
        if (blob[i] == '\n') {
            n++;
        }
    }
    if (n == 0) {
        free_all();
        return false;
    }

    blocks = malloc((size_t)n * sizeof(block_t));
    if (blocks == NULL) {
        free_all();
        return false;
    }

    int w = 0;
    char *p = blob;
    while (*p != '\0' && w < n) {
        char *nl = strchr(p, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        blocks[w].tag = p[0];
        blocks[w].text = (p[0] != '\0') ? p + 1 : "";
        blocks[w].height = -1;
        w++;
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    nblocks = w;
    return true;
}

static void credits_enter (app_t *app) {
    (void)app;
    top = 0;
    top_px = 0;
    if (blocks == NULL) {
        (void)load();
    }
}

static void credits_leave (app_t *app) {
    (void)app;
    free_all();
}

/** @brief Height of a block, or a guess if it has not been drawn yet. */
static int height_of (int i) {
    if (i < 0 || i >= nblocks) {
        return 0;
    }
    if (blocks[i].height >= 0) {
        return blocks[i].height;
    }
    /* Only reachable for a block below the window, which the scroll never steps onto without
     * drawing it first. One body line is the right guess if it ever is. */
    return 20;
}

/**
 * @brief Pixels of content from block @p from to the end, or -1 if any of it is unmeasured.
 *
 * The tail is only fully known once every block in it has been on screen, which is exactly the
 * state reaching the end puts it in. Returning -1 rather than a guess is what keeps the clamp
 * honest: an estimate here would stop the scroll short of the real end, and there would be text
 * nobody could reach with no indication that there was.
 */
static int tail_height (int from) {
    int total = 0;
    for (int i = from; i < nblocks; i++) {
        if (blocks[i].height < 0) {
            return -1;
        }
        total += blocks[i].height;
    }
    return total;
}

static void scroll_by (int dy) {
    top_px += dy;
    while (top_px >= height_of(top) && top < nblocks - 1) {
        top_px -= height_of(top);
        top++;
    }
    while (top_px < 0 && top > 0) {
        top--;
        top_px += height_of(top);
    }
    if (top_px < 0) {
        top_px = 0;
    }

    /* Stop when the last line reaches the bottom of the window, not when the last block reaches
     * the top of it. The first version did the latter, because total height is the one number
     * this scheme never computes -- and the result was that scrolling to the end left a single
     * paragraph at the top of an otherwise empty screen, which reads as the text having been
     * lost rather than as having been finished.
     *
     * By the time this matters every block below has been drawn at least once, so the tail is
     * measured rather than estimated. Until then tail_height() says so and the clamp does not
     * fire, which costs nothing: you cannot be at the end of text you have not scrolled through.
     */
    int tail = tail_height(top);
    if (tail >= 0 && tail - top_px < CONTENT_H) {
        int over = CONTENT_H - (tail - top_px);
        top_px -= over;
        while (top_px < 0 && top > 0) {
            top--;
            top_px += height_of(top);
        }
        if (top_px < 0) {
            top_px = 0;
        }
    }

    if (top >= nblocks - 1) {
        top = nblocks - 1;
        if (top_px > 0) {
            top_px = 0;
        }
    }
}

static void credits_update (app_t *app, float dt) {
    (void)dt;
    const input_t *in = &app->input;

    if (input_pressed(in, BTN_B)) {
        sound_play_effect(SFX_EXIT);
        app_goto(app, SCREEN_SETTINGS);
        return;
    }
    if (nblocks == 0) {
        return;
    }

    if (in->up)   scroll_by(-SCROLL_STEP);
    if (in->down) scroll_by(SCROLL_STEP);
    if (input_pressed(in, BTN_L)) scroll_by(-SCROLL_PAGE);
    if (input_pressed(in, BTN_R)) scroll_by(SCROLL_PAGE);
}

/** @brief Draw one block at @p y and say how tall it turned out. */
static int draw_block (const theme_t *th, const block_t *b, int y) {
    (void)th;
    switch (b->tag) {
        case 'H':
            return ui_text_wrap(FNT_DEFAULT, TEXT_X, y + 20, TEXT_W, STL_YELLOW, b->text)
                   + 20 + GAP_TEXT;
        case 'S':
            return ui_text_wrap(FNT_DEFAULT, TEXT_X, y + 18, TEXT_W, STL_ORANGE, b->text)
                   + 18 + GAP_TIGHT;
        case 'B':
            /* Indented, and the dash is drawn rather than baked into the string so a wrapped
             * bullet's second line lines up with the first line's text instead of with the
             * dash. */
            ui_text(TEXT_X + 8, y + 16, 12, ALIGN_LEFT, STL_GRAY, "-");
            return ui_text_wrap(FNT_DEFAULT, TEXT_X + 24, y + 16, TEXT_W - 24,
                                STL_DEFAULT, b->text) + 16 + GAP_TIGHT;
        case 'U':
            /* Broken mid-string rather than word-wrapped: a URL is one word and WRAP_WORD
             * ellipsises what it cannot fit, which lost the tail of the fork's source address.
             * See ui_text_wrap_url(). */
            return ui_text_wrap_url(FNT_DEFAULT, TEXT_X + 8, y + 16, TEXT_W - 8,
                                    STL_ORANGE, b->text) + 16 + GAP_TEXT;
        case 'T':
            return ui_text_wrap(FNT_DEFAULT, TEXT_X, y + 16, TEXT_W, STL_DEFAULT, b->text)
                   + 16 + GAP_TEXT;
        default:
            /* A blank line. Half a body line of air, which is what separates paragraphs. */
            return 10;
    }
}

static void credits_render (app_t *app, surface_t *fb) {
    const theme_t *th = app->theme;

    rdpq_attach(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

    ui_fill(0, 0, SCREEN_W, HEAD_H, th->panel);
    ui_fill(0, HEAD_H - ACCENT_BAR, SCREEN_W, ACCENT_BAR, th->tab_underline);
    ui_label(SAFE_X, 36, SAFE_W, ALIGN_LEFT, STL_DEFAULT, "Credits and licences");

    if (nblocks == 0) {
        ui_label(TEXT_X, CONTENT_Y + 40, TEXT_W, ALIGN_CENTER, STL_GRAY,
                 "The credits file could not be read from this cartridge.");
    } else {
        /* Scissored, because a block at either end is drawn partly outside the window and would
         * otherwise run over the header and the footer. */
        rdpq_set_scissor(0, CONTENT_Y, SCREEN_W, CONTENT_BOT);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);

        int y = CONTENT_Y - top_px;
        for (int i = top; i < nblocks && y < CONTENT_BOT; i++) {
            int h = draw_block(th, &blocks[i], y);
            /* The measurement, cached where the scroll will look for it. See the header. */
            blocks[i].height = (int16_t)h;
            y += h;
        }
        rdpq_set_scissor(0, 0, SCREEN_W, SCREEN_H);

        int thumb = (CONTENT_H * 8) / (nblocks > 8 ? nblocks : 8);
        if (thumb < POSBAR_THUMB_MIN) {
            thumb = POSBAR_THUMB_MIN;
        }
        int travel = CONTENT_H - thumb;
        int pos = (nblocks > 1) ? (travel * top) / (nblocks - 1) : 0;
        ui_fill(POSBAR_X, CONTENT_Y, POSBAR_W, CONTENT_H, th->panel);
        ui_fill(POSBAR_X, CONTENT_Y + pos, POSBAR_W, thumb, th->text_dim);
    }

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    (void)ui_hint(SAFE_X, FOOTER_Y + 14, "^", BTN_C_COLOR, UI_BTN_DISC, "Scroll");
    ui_button(SAFE_X + SAFE_W - UI_BTN_D, FOOTER_Y + 14, "B", BTN_B_COLOR, UI_BTN_DISC);
    ui_label(SAFE_X, FOOTER_Y + 14 + UI_BTN_D - 5, SAFE_W - UI_BTN_D - 6, ALIGN_RIGHT,
             STL_GRAY, "Back");

    rdpq_detach_show();
}

const screen_t SCREEN_CREDITS_DEF = {
    .id     = SCREEN_CREDITS,
    .enter  = credits_enter,
    .leave  = credits_leave,
    .update = credits_update,
    .render = credits_render,
};
