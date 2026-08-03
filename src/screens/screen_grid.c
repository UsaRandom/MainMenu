/**
 * @file screen_grid.c
 * @brief The library grid. The screen everything is judged on.
 * @ingroup screens
 *
 * Geometry and colour are transcribed from docs/design/README.md sections 1-4 by way of
 * ui/theme.h. Nothing here invents a number.
 *
 * The selection model is worth stating because it inverts what you would expect to draw: the
 * signal is not that the selected tile is decorated, it is that every OTHER tile is washed out.
 * The handoff calls this the one signal that survives a room's width. So the common case is a
 * blended quad per unselected tile, and the grow, shadow and outline are reinforcement.
 *
 * Art is not wired up yet -- every tile renders in its "no art" state, which is a fully
 * specified state rather than a placeholder, so the grid is legitimately the product with an
 * empty cache.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <libdragon.h>

#include "app.h"
#include "menu/fonts.h"
#include "menu/png_decoder.h"
#include "menu/sound.h"
#include "screens.h"
#include "screens/boot_plate.h"
#include "ui/draw.h"
#include "ui/theme.h"
#include "ui/tween.h"

#define MAX_VIEW 1024

static uint16_t view[MAX_VIEW];
static int view_count;

static tab_t tab = TAB_N64;
static int cursor;              /**< index into view[] */
static float scroll_y;          /**< pixels, content space; always rounded before use */
static float scroll_target;
static float pulse_phase;
static boot_plate_t boot_anim;   /* not `boot`: boot/boot.h already has a function by that name */
static bool boot_armed;
static uint16_t ambient;        /**< current wash colour, eased toward the selection's */
static uint16_t ambient_target;
static tween_t grow;
static int frames_since_move;

/* ------------------------------------------------------------------ small drawing helpers */


/* ------------------------------------------------------------------ model */

static int rows_total (void) {
    return (view_count + GRID_COLS - 1) / GRID_COLS;
}

static float scroll_max (void) {
    int content = rows_total() * ROW_PITCH - TILE_GAP;
    float m = (float)(content - GRID_H);
    return m > 0.0f ? m : 0.0f;
}

static void rebuild_view (app_t *app) {
    view_count = library_tab_view(app->lib, tab, view, MAX_VIEW);
    if (cursor >= view_count) {
        cursor = view_count > 0 ? view_count - 1 : 0;
    }
}

/** @brief Centre the selected row, clamped, snapped to whole pixels. */
static void retarget_scroll (void) {
    int row = cursor / GRID_COLS;
    float want = (float)(row * ROW_PITCH) - (float)(GRID_H - TILE_H) * 0.5f;
    scroll_target = roundf(clampf(want, 0.0f, scroll_max()));
}

/* ------------------------------------------------------------------ tiles */

/**
 * @brief Draw one tile in its "no art" state.
 *
 * bg_alt fill, 2 px panel_alt inner border, system code as a large watermark, title clipped
 * along the bottom. Still identifiable -- which is the point: a grid with a cold cache must be
 * navigable, not a wall of grey rectangles.
 */
static void draw_tile (app_t *app, const lib_record_t *rec, uint16_t rom_id,
                       int x, int y, int w, int h, bool selected) {
    const theme_t *th = app->theme;
    static const char *SYS_CODE[SYS_COUNT] = { "N64", "NES", "SNES", "GB", "GBC", "SMS" };
    const char *code = (rec->system < SYS_COUNT) ? SYS_CODE[rec->system] : "?";

    surface_t *art = thumbcache_get(app->thumbs, app->lib, rom_id);

    if (art != NULL) {
        /* Arrival: 0.12 s, scale 0.86 -> 1.00 on the spec's overshoot curve, silent. Section 6 is
         * explicit that 512 of these must make no sound. */
        float arrive = 1.0f;
        if (rec->art_age < DUR_TILE_ARRIVAL) {
            float k = ease_bezier(rec->art_age / DUR_TILE_ARRIVAL, EASE_TILE_GROW);
            arrive = lerpf(TILE_ARRIVE_SCALE, 1.0f, k);
        }
        int aw = (int)(w * arrive);
        int ah = (int)(h * arrive);
        int ax = x + (w - aw) / 2;
        int ay = y + (h - ah) / 2;

        /* Copy mode cannot scale, so an arriving tile goes through the standard pipeline for the
         * few frames it is animating and drops back to copy once it settles. */
        if (arrive < 1.0f) {
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_TEX);
        } else {
            rdpq_set_mode_copy(false);
        }
        rdpq_tex_blit(art, ax, ay, &(rdpq_blitparms_t){
            .scale_x = (float)aw / (float)TILE_W,
            .scale_y = (float)ah / (float)TILE_H,
        });
    } else if (rec->art_state == ART_NONE) {
        /* No art: bg_alt fill, 2 px inner border, the system code as a watermark, and the title
         * clipped along the bottom. Still identifiable, which is the point -- a cold or artless
         * library must stay navigable rather than becoming a wall of grey rectangles. */
        ui_fill(x, y, w, h, th->bg_alt);
        ui_border(x, y, w, h, 2, th->panel_alt);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
        ui_text(x, y + h / 2 - 18, w, ALIGN_CENTER, STL_GRAY, code);
        if (rec->title != NULL) {
            ui_text(x + 4, y + h - 16, w - 8, ALIGN_CENTER, STL_DEFAULT, rec->title);
        }
    } else {
        /* Loading: panel fill, the title immediately from the database, and a worm along the
         * bottom edge showing decode progress. Deliberately not a spinner -- the handoff bans
         * them for both of the product's long waits. */
        ui_fill(x, y, w, h, th->panel);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
        if (rec->title != NULL) {
            ui_text(x + 4, y + h / 2, w - 8, ALIGN_CENTER, STL_DEFAULT, rec->title);
        }
        if (rec->art_state == ART_DECODING) {
            int p = (int)(png_decoder_get_progress() * (float)w);
            ui_fill(x, y + h - 3, p, 3, th->text_accent);
        }
    }

    if (!selected) {
        /* The primary selection signal: everything that is NOT selected is washed toward bg.
         * These quads never overlap each other, so this stays one layer of blended fill. */
        ui_wash(x, y, w, h, th->bg, th->tile_dim_a);
    }
}

static void draw_badges (app_t *app, const lib_record_t *rec, int x, int y, int w) {
    const theme_t *th = app->theme;

    /* Two marks, two corners, so there is no precedence to resolve. Save is a game state and
     * owns the top-right; favourite is a user property and owns the top-left. Cheats
     * deliberately never appear here. */
    if (rec->flags & LIBF_HAS_SAVE) {
        int bx = x + w - BADGE_SLOT - BADGE_INSET;
        int by = y + BADGE_INSET;
        ui_fill(bx, by, BADGE_SLOT, BADGE_SLOT, th->badge_save);
        ui_fill(bx + 5, by + 4, BADGE_SLOT - 10, BADGE_SLOT - 8, th->text);
    }

    if ((rec->flags & LIBF_FAVORITE) && tab != TAB_FAVORITES) {
        /* Right triangle, hypotenuse from top-right to bottom-left. Distinguished from the
         * save badge by silhouette, not by colour -- under the Phosphor theme the two are
         * near-identical greens by design. */
        for (int i = 0; i < FAV_TRIANGLE; i++) {
            ui_fill(x, y + i, FAV_TRIANGLE - i, 1, th->badge_fav);
        }
    }
}

/**
 * @brief Hand the selected game to @p screen.
 *
 * The path is cloned rather than borrowed because the library owns its strings and a launch
 * outlives the record's residency in a tab view -- switching tabs rebuilds view[] and the index
 * the launch was started from stops meaning anything.
 *
 * rom_config_load() is re-run here rather than cached per record: it fills a rom_info_t of a few
 * hundred bytes, which is not worth holding 500 of, and at 20,700 us for one ROM it is invisible
 * against the seconds a load takes.
 */
static void grid_open (app_t *app, screen_id_t screen) {
    const lib_record_t *rec = &app->lib->records[view[cursor]];
    if (rec->path == NULL) {
        return;
    }

    if (app->launch.rom_path != NULL) {
        path_free(app->launch.rom_path);
    }
    app->launch.rom_path = path_create(rec->path);
    app->launch.rom_id = (int)view[cursor];
    memset(&app->launch.rom_info, 0, sizeof(app->launch.rom_info));
    rom_config_load(app->launch.rom_path, &app->launch.rom_info);

    app_goto(app, screen);
}

/* ------------------------------------------------------------------ chrome */

/**
 * @brief Glyph for a virtual tab, drawn when it is not the active one.
 *
 * Favourites is a corner triangle, matching the badge used on a favourited tile so the two teach
 * each other. Recent is a clock. Most Played is a rising bar chart. All three are built from
 * fills rather than sprites -- three shapes did not justify an asset pipeline.
 */
static void draw_tab_icon (int x, int y, tab_t t, uint16_t c) {
    switch (t) {
        case TAB_FAVORITES:
            for (int i = 0; i < TAB_ICON; i++) {
                ui_fill(x, y + i, TAB_ICON - i, 1, c);
            }
            break;
        case TAB_RECENT:
            ui_border(x, y, TAB_ICON, TAB_ICON, 2, c);
            ui_fill(x + TAB_ICON / 2 - 1, y + 5, 2, TAB_ICON / 2 - 4, c);   /* hand, up */
            ui_fill(x + TAB_ICON / 2, y + TAB_ICON / 2 - 1, TAB_ICON / 4, 2, c); /* hand, right */
            break;
        case TAB_MOST_PLAYED:
            ui_fill(x, y + TAB_ICON - 6, 4, 6, c);
            ui_fill(x + 6, y + TAB_ICON - 12, 4, 12, c);
            ui_fill(x + 12, y + TAB_ICON - 18, 4, 18, c);
            break;
        default:
            break;
    }
}

static void draw_tab_rail (app_t *app) {
    const theme_t *th = app->theme;

    ui_fill(TABRAIL_X, TABRAIL_Y, TABRAIL_W, TABRAIL_H, th->panel);

    /* All nine tabs are always present, empty or not, left-aligned, never centred or
     * distributed. A tab you have nothing for is still exactly where you left it.
     *
     * The three virtual tabs are icon-only until selected, which is section 4.3 and also the only
     * way the rail fits: nine labels at the body font's 12 px glyph metric measure 744 px against
     * a 608 px rail. Icons for the inactive ones brings the worst case -- Most Played active --
     * to 576. */
    int x = TABRAIL_X + TAB_PAD;
    for (int t = 0; t < TAB_COUNT; t++) {
        bool active = (t == (int)tab);
        bool virt = (t < TAB_N64);
        bool icon_only = virt && !active;

        const char *label = library_tab_label((tab_t)t);
        int w = icon_only ? TAB_ICON : (int)strlen(label) * TAB_GLYPH_W;

        if (active) {
            ui_fill(x - TAB_PAD / 2, TABRAIL_Y, w + TAB_PAD, TABRAIL_H, th->panel_alt);
            ui_fill(x - TAB_PAD / 2, TABRAIL_Y + TABRAIL_H - ACCENT_BAR, w + TAB_PAD, ACCENT_BAR,
                    th->tab_underline);
        }

        if (icon_only) {
            draw_tab_icon(x, TABRAIL_Y + (TABRAIL_H - TAB_ICON) / 2 - 2, (tab_t)t, th->text_dim);
        } else {
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
            ui_text(x, TABRAIL_Y + 32, w, ALIGN_LEFT,
                    active ? STL_DEFAULT : STL_GRAY, label);
        }
        x += w + TAB_PAD;
    }
}

static void draw_footer (app_t *app) {
    const theme_t *th = app->theme;
    char buf[96];

    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, th->panel);
    ui_fill(FOOTER_X, FOOTER_Y, FOOTER_W, HAIRLINE, th->panel_alt);

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);

    /* Tile titles live here at full width rather than on the tile, which is what lets the
     * product hold the no-marquee rule: 608 px is about 52 characters at 20 px. */
    if (view_count > 0) {
        const lib_record_t *rec = &app->lib->records[view[cursor]];
        ui_text(SAFE_X, FOOTER_Y + 26, SAFE_W, ALIGN_CENTER, STL_DEFAULT,
                rec->title ? rec->title : "?");
        snprintf(buf, sizeof(buf), "%d / %d", cursor + 1, view_count);
    } else {
        ui_text(SAFE_X, FOOTER_Y + 26, SAFE_W, ALIGN_CENTER, STL_GRAY, "NO TITLES IN THIS TAB");
        buf[0] = '\0';
    }

    /* L and R are dropped: the rail is on screen with the current tab underlined, so the tabs
     * advertise themselves and the shoulder buttons are the only thing they could mean. */
    int hx = SAFE_X;
    hx = ui_hint(hx, FOOTER_Y + 32, "A", BTN_A_COLOR, UI_BTN_DISC, "Details");
    hx = ui_hint(hx, FOOTER_Y + 32, "Z", BTN_Z_COLOR, UI_BTN_TALL, "Favourite");
    (void)ui_hint(hx, FOOTER_Y + 32, "S", BTN_START_COLOR, UI_BTN_DISC, "Settings");
    if (buf[0]) {
        ui_text(SAFE_X, FOOTER_Y + 48, SAFE_W, ALIGN_RIGHT, STL_ORANGE, buf);
    }
}

static void draw_position_bar (app_t *app) {
    const theme_t *th = app->theme;

    float max = scroll_max();
    if (max <= 0.0f) {
        return;                     /* hidden when nothing scrolls */
    }

    ui_fill(POSBAR_X, POSBAR_Y, POSBAR_W, POSBAR_H, th->panel);

    int track = POSBAR_H;
    int thumb = (int)((float)track * (float)GRID_H / (float)(GRID_H + max));
    if (thumb < POSBAR_THUMB_MIN) {
        thumb = POSBAR_THUMB_MIN;   /* so it never vanishes at 128 rows */
    }
    int travel = track - thumb;
    int pos = (int)((scroll_y / max) * (float)travel);

    ui_fill(POSBAR_X, POSBAR_Y + pos, POSBAR_W, thumb, th->text_dim);
}

/* ------------------------------------------------------------------ screen */

static void grid_enter (app_t *app) {
    /* Armed once per power-on, not once per visit: coming back from the detail sheet must not
     * replay the boot animation. */
    if (!boot_armed) {
        boot_armed = true;
        boot_plate_reset(&boot_anim);
    }

    rebuild_view(app);
    cursor = 0;
    scroll_y = scroll_target = 0.0f;
    retarget_scroll();
    scroll_y = scroll_target;
    tween_start(&grow, DUR_TILE_GROW);
}

static void grid_update (app_t *app, float dt) {
    const input_t *in = &app->input;

    /* The plate swallows input while it is up, so a button pressed during boot does not land on
     * a grid the user cannot see yet. It does NOT stop the grid updating -- scrolling, decoding
     * and the selection tween all run underneath, which is what makes the reveal a reveal. */
    if (boot_plate_step(&boot_anim, dt)) {
        return;
    }
    int prev = cursor;

    if (input_pressed(in, BTN_L)) {
        sound_play_effect(SFX_CURSOR);
        tab = (tab_t)((tab + TAB_COUNT - 1) % TAB_COUNT);
        rebuild_view(app);
        cursor = 0;
    }
    if (input_pressed(in, BTN_R)) {
        sound_play_effect(SFX_CURSOR);
        tab = (tab_t)((tab + 1) % TAB_COUNT);
        rebuild_view(app);
        cursor = 0;
    }

    if (view_count > 0) {
        if (in->left  && cursor > 0)                cursor--;
        if (in->right && cursor < view_count - 1)   cursor++;
        if (in->up    && cursor >= GRID_COLS)       cursor -= GRID_COLS;
        if (in->down) {
            /* Stepping down from the last partial row must land on the final title rather than
             * refusing to move, or the bottom-right corner of the library is unreachable. */
            if (cursor + GRID_COLS < view_count) {
                cursor += GRID_COLS;
            } else if (cursor / GRID_COLS < rows_total() - 1) {
                cursor = view_count - 1;
            }
        }
    }

    if (cursor != prev) {
        /* Section 6: cursor fires on every accepted step INCLUDING repeats, because a held
         * direction that stops moving at the end of a row must sound different from one that is
         * still travelling. Tab changes get it too -- they are the same gesture sideways. */
        sound_play_effect(SFX_CURSOR);
        tween_start(&grow, DUR_TILE_GROW);
        frames_since_move = 0;
    } else {
        frames_since_move++;
    }

    if (view_count > 0 && input_pressed(in, BTN_A)) {
        sound_play_effect(SFX_ENTER);
        grid_open(app, SCREEN_DETAIL);
    }
    if (view_count > 0 && input_pressed(in, BTN_Z)) {
        /* In memory only, and lost on reboot until playstate.dat exists -- writing to the card is
         * deferred until there is hardware to test a write against. The interaction is here
         * because the Favorites tab is otherwise permanently empty and unreviewable. */
        lib_record_t *r = &app->lib->records[view[cursor]];
        r->flags ^= LIBF_FAVORITE;
        sound_play_effect(SFX_SETTING);
        if (tab == TAB_FAVORITES) {
            rebuild_view(app);          /* un-favouriting from the Favorites tab removes it */
            if (cursor >= view_count) {
                cursor = view_count > 0 ? view_count - 1 : 0;
            }
        }
    }

    if (input_pressed(in, BTN_START)) {
        sound_play_effect(SFX_ENTER);
        /* Start is settings, not launch. Launching from the grid without seeing what a game
         * needs -- save type, Expansion Pak, whether cheats are ticked -- is how you boot with
         * the wrong thing on. The sheet is one button away and answers all three. */
        app_goto(app, SCREEN_SETTINGS);
    }

    retarget_scroll();
    scroll_y = smooth_towards(scroll_y, scroll_target, SMOOTH_RATE_SCROLL, dt);
    if (fabsf(scroll_target - scroll_y) < 0.5f) {
        scroll_y = scroll_target;
    }

    /* Track the selection's colour. Eased per channel in 5-bit space rather than crossfaded in
     * RGBA, because the destination is a single RGBA5551 value and interpolating the packed word
     * would walk through colours that are in neither endpoint. */
    if (view_count > 0) {
        uint16_t want = app->lib->records[view[cursor]].dominant;
        if (want != 0) {
            ambient_target = want;
        }
    }
    if (ambient_target != 0) {
        int cr = (ambient >> 11) & 0x1F, cg = (ambient >> 6) & 0x1F, cb = (ambient >> 1) & 0x1F;
        int tr = (ambient_target >> 11) & 0x1F, tg = (ambient_target >> 6) & 0x1F,
            tb = (ambient_target >> 1) & 0x1F;
        float k = 1.0f - expf(-AMBIENT_RATE * dt);
        cr += (int)roundf((tr - cr) * k);
        cg += (int)roundf((tg - cg) * k);
        cb += (int)roundf((tb - cb) * k);
        /* Snap when within one step, or the rounding leaves it one level short forever. */
        if (cr == tr && cg == tg && cb == tb) {
            ambient = ambient_target;
        } else {
            ambient = (uint16_t)((cr << 11) | (cg << 6) | (cb << 1) | 1);
        }
    }

    /* Only the visible window is advanced. Walking all 500 records every frame to age a timer
     * that only matters for the twelve on screen is the same shape of waste the idle cache flag
     * was added to remove. */
    for (int i = 0; i < view_count; i++) {
        lib_record_t *r = &app->lib->records[view[i]];
        if (r->art_age < DUR_TILE_ARRIVAL) {
            r->art_age += dt;
        }
    }

    tween_step(&grow, dt);
    pulse_phase += dt * SEL_PULSE_HZ * 2.0f * (float)M_PI;
    if (pulse_phase > 2.0f * (float)M_PI) {
        pulse_phase -= 2.0f * (float)M_PI;
    }
}

static void grid_render (app_t *app, surface_t *fb) {
#ifdef ALLOCWATCH_SELFTEST
    /* Deliberate per-frame allocation, so the zero reported by tools/inputs/idle.txt can be
     * shown to be a measurement rather than a broken counter. Never defined by any real build.
     *
     * The pointer goes through a volatile, because the obvious `free(malloc(64))` is deleted
     * outright by GCC's allocation DCE under -flto -- which made the first version of this
     * self-test report zero and very nearly certified a broken counter as a passing gate. */
    static volatile uint32_t selftest_acc;
    void *p = malloc(64);
    selftest_acc += (uint32_t)(uintptr_t)p;      /* the value escapes, so DCE cannot remove it */
    if (p == NULL) {
        debugf("selftest: malloc failed\n");
    }
    free(p);
#endif

    const theme_t *th = app->theme;

    thumbcache_begin_frame(app->thumbs);

    rdpq_attach(fb, NULL);
    rdpq_set_mode_fill(color_from_packed16(th->bg));
    rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H);

    /* Ambient wash: a soft field of the selected game's own colour behind the grid, so moving
     * across the library shifts the whole screen slightly. docs/design/README.md asks for a
     * 420x300 dithered quad; this is that, eased so the colour slides rather than cuts.
     *
     * The handoff also nominates this as the FIRST thing to cut if fill rate runs out (10.3),
     * so it is one blended quad and nothing more -- no gradient, no second layer. */
    if (ambient != 0) {
        ui_wash(AMBIENT_X, AMBIENT_Y, AMBIENT_W, AMBIENT_H, ambient, AMBIENT_ALPHA);
    }

    draw_tab_rail(app);

    /* Round once, here. Everything downstream is integer, so a fractional scroll can never
     * reach a blit and shimmer. */
    int sy = (int)roundf(scroll_y);

    /* Clip top and bottom only. A selected tile in column 0 or 3 overhangs the grid block
     * horizontally by 6 px and must not be cut. */
    rdpq_set_scissor(0, GRID_Y, SCREEN_W, GRID_Y + GRID_H);

    int first_row = sy / ROW_PITCH;
    int last_row = (sy + GRID_H) / ROW_PITCH;

    for (int pass = 0; pass < 2; pass++) {
        for (int row = first_row; row <= last_row; row++) {
            for (int col = 0; col < GRID_COLS; col++) {
                int idx = row * GRID_COLS + col;
                if (idx < 0 || idx >= view_count) {
                    continue;
                }
                bool selected = (idx == cursor);
                /* Unselected first, selected last, so its shadow and growth overlap
                 * neighbours instead of being overdrawn by them. */
                if (selected != (pass == 1)) {
                    continue;
                }

                const lib_record_t *rec = &app->lib->records[view[idx]];
                int x = COL_X(col);
                int y = ROW_Y(row) - sy;

                if (selected) {
                    int gx = x + SEL_DX, gy = y + SEL_DY;
                    float t = ease_bezier(tween_t01(&grow), EASE_TILE_GROW);
                    int w = (int)lerpf(TILE_W, SEL_W, t);
                    int h = (int)lerpf(TILE_H, SEL_H, t);
                    gx = x + (TILE_W - w) / 2;
                    gy = y + (TILE_H - h) / 2;

                    ui_wash(gx + SEL_SHADOW_DX, gy + SEL_SHADOW_DY, w, h,
                         th->sel_shadow, th->sel_shadow_a);
                    draw_tile(app, rec, view[idx], gx, gy, w, h, true);
                    draw_badges(app, rec, gx, gy, w);

                    /* Hue crossfade rather than a brightness blink, so the tile stays equally
                     * visible at every point in the cycle. */
                    float k = (1.0f - cosf(pulse_phase)) * 0.5f;
                    color_t a = color_from_packed16(th->sel_outline);
                    color_t b = color_from_packed16(th->text_accent);
                    color_t mix = RGBA32((int)lerpf(a.r, b.r, k),
                                         (int)lerpf(a.g, b.g, k),
                                         (int)lerpf(a.b, b.b, k), 255);
                    rdpq_set_mode_fill(mix);
                    rdpq_fill_rectangle(gx - SEL_OUTLINE, gy - SEL_OUTLINE,
                                        gx + w + SEL_OUTLINE, gy);
                    rdpq_fill_rectangle(gx - SEL_OUTLINE, gy + h,
                                        gx + w + SEL_OUTLINE, gy + h + SEL_OUTLINE);
                    rdpq_fill_rectangle(gx - SEL_OUTLINE, gy, gx, gy + h);
                    rdpq_fill_rectangle(gx + w, gy, gx + w + SEL_OUTLINE, gy + h);
                } else {
                    draw_tile(app, rec, view[idx], x, y, TILE_W, TILE_H, false);
                    draw_badges(app, rec, x, y, TILE_W);
                }
            }
        }
    }

    rdpq_set_scissor(0, 0, SCREEN_W, SCREEN_H);

    /* Drawn after the tiles so the 4 px of selection shadow that reaches x 622 sits behind it
     * rather than on top. See docs/DESIGN.md 5.1. */
    draw_position_bar(app);
    draw_footer(app);

    /* Last, over a finished grid. The plate is opaque black, so everything above is drawn and
     * immediately covered for the first 1.3 s -- deliberately. It costs one screen of fill on
     * ~78 frames of a boot and it is what makes the curtain reveal a grid that is already alive
     * rather than one that starts when the curtain lifts. */
    boot_plate_draw(&boot_anim, MENU_VERSION, app->lib != NULL ? app->lib->count : 0);

    rdpq_detach_show();
}

/* Decode budget per frame, in microseconds of emulated CPU.
 *
 * These are measured, not guessed. A card costs ~155,000 us to decode, so the budget divided
 * into that is the wall-clock wait per tile: at the 2,000 us this originally used, one tile
 * took 1.5 s and filling the twelve visible ones took nineteen seconds. That is the whole
 * reason the number is split in two.
 *
 * IDLE is spent when the cursor has not moved recently, which is when the user is looking at
 * the grid and waiting for it to fill. MOVING is spent while they are scrolling, when frame
 * rate matters more than art and the tiles are going past too fast to read anyway. */
#ifndef DECODE_BUDGET_IDLE_US
#define DECODE_BUDGET_IDLE_US   6000
#endif
#ifndef DECODE_BUDGET_MOVING_US
#define DECODE_BUDGET_MOVING_US 1200
#endif

/** Frames of stillness before the budget opens up. ~0.25 s at 60 Hz. */
#define DECODE_SETTLE_FRAMES    15

/**
 * @brief Decode art in the window where the CPU would otherwise wait on the RDP.
 */
static void grid_background (app_t *app, uint32_t budget_ticks) {
    (void)budget_ticks;
    /* Decode NOTHING while the selection is moving, rather than decoding on a small budget.
     *
     * A budget can only stop between rows, and one row of a real card costs 5,000-19,000 us --
     * a whole 60 Hz field for a single row of a single image. So "1,200 us while moving" spent
     * 9,000 us per frame in practice and 40 % of frames took two fields. Sweeping the budget
     * from 0 to 12,000 us moved the frame rate by less than one fps, because the smallest unit
     * of work the budget can stop on is already larger than the frame.
     *
     * Nothing here makes decoding cheaper. It moves it to where dropped frames do not read as
     * judder: art appears a quarter-second after you stop, which is when you can actually look
     * at it, and the scroll itself stays smooth. The permanent answer is to not decode twice --
     * see docs/AUDIT.md on the on-disk cache. */
    if (frames_since_move < DECODE_SETTLE_FRAMES) {
        return;
    }
    thumbcache_run(app->thumbs, app->lib, DECODE_BUDGET_IDLE_US);
}

const screen_t SCREEN_GRID_DEF = {
    .id         = SCREEN_GRID,
    .enter      = grid_enter,
    .update     = grid_update,
    .render     = grid_render,
    .background = grid_background,
};
