/**
 * @file app.c
 * @brief Application state and the main loop.
 * @ingroup app
 */

#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "cheats/cheatdb.h"
#include "menu/png_decoder.h"
#include "dev/allocwatch.h"
#include "dev/debug_emux.h"
#include "dev/inputscript.h"
#include "flashcart/flashcart.h"
#include "menu/fonts.h"
#include "menu/sound.h"
#include "screens/screens.h"
#include "ui/tween.h"

#define MENU_DIRECTORY  "/menu"
#define CONFIG_FILE     "config.ini"

/* Video. Three buffers, not upstream's two: with two, display_try_get() returns NULL whenever
 * the RDP has not drained, and the CPU spins instead of doing useful work -- which is exactly
 * the window the background() phase exists to use. The third buffer costs 614,400 bytes, which
 * only the M64's built-in Expansion Pak makes affordable; upstream runs two because it must fit
 * in 4 MB. See docs/AUDIT.md.
 *
 * Progressive, not INTERLACE_HALF. A scrolling grid of high-contrast art shimmers badly in
 * 480i. This is the starting point for the A/B described in docs/DESIGN.md, not a settled
 * answer -- measure it, do not argue it. */
#define FB_COUNT        3

static app_t app_state;

static const screen_t *SCREENS[SCREEN_COUNT];

void app_goto (app_t *app, screen_id_t screen) {
    app->next_screen = screen;
}

void app_fault (app_t *app, const char *message) {
    app->fault_message = message;
    app->next_screen = SCREEN_FAULT;
}

static void app_init (app_t *app, boot_params_t *boot_params) {
    memset(app, 0, sizeof(*app));
    app->boot = boot_params;
    app->running = true;
    app->screen = SCREEN_COUNT;          /* forces an enter() on the first transition */
    app->next_screen = SCREEN_GRID;

    flashcart_err_t ferr = flashcart_init(&app->storage);

    joypad_init();
    timer_init();
    rtc_init();
    rspq_init();
    rdpq_init();
    dfs_init(DFS_DEFAULT_LOCATION);

    input_init(&app->input);
    sound_init_default();
    sound_init_sfx();

    path_t *cfg = path_init(app->storage, MENU_DIRECTORY);
    path_push(cfg, CONFIG_FILE);
    settings_init(path_get(cfg));
    settings_load(&app->settings);
    path_free(cfg);

    /* The setting existed and the toggle drew, but nothing ever told the sound system about it --
     * turning sound effects off in settings changed a bool and nothing else. */
    sound_use_sfx(app->settings.soundfx_enabled);

    app->theme = &THEME_MIDNIGHT;

    resolution_t resolution = { .width = SCREEN_W, .height = SCREEN_H,
                                .interlaced = INTERLACE_OFF };
    display_init(resolution, DEPTH_16_BPP, FB_COUNT, GAMMA_NONE, FILTERS_RESAMPLE);

    fonts_init(NULL);

    if (ferr != FLASHCART_OK) {
        app_fault(app, "No supported flashcart detected.");
        return;
    }

    app->lib = library_init();
    if (app->lib == NULL) {
        app_fault(app, "Out of memory building the library.");
        return;
    }
    library_scan(app->lib, app->storage, "/roms");

    /* Opened once and held: the index is 24 bytes a game, and the alternative is reopening the
     * file every time a detail sheet appears. Absent is normal -- a card with no cheats.db is a
     * card whose owner did not install one, not a fault. */
    cheatdb_open(app->storage);

    app->thumbs = thumbcache_init(app->storage);
    if (app->thumbs == NULL) {
        app_fault(app, "Out of memory allocating the art cache.");
    }
}

static void app_deinit (app_t *app) {
    /* cheats is NOT freed here: boot_params->cheat_list may point into a buffer built from it,
     * and main() calls boot() the moment this returns. */
    cheatdb_close();
    thumbcache_free(app->thumbs);
    app->thumbs = NULL;
    library_free(app->lib);
    app->lib = NULL;

    display_close();
    sound_deinit();
    rdpq_close();
    rspq_close();
    rtc_close();
    timer_close();
    joypad_close();
    flashcart_deinit();
}

void app_run (boot_params_t *boot_params) {
    app_t *app = &app_state;

    screens_register(SCREENS);
    app_init(app, boot_params);

    dbg_emux_present();

    uint32_t prev_ticks = TICKS_READ();

    while (app->running) {
        /* Acquire the framebuffer FIRST and drive everything from it, so one iteration of this
         * loop is one displayed frame.
         *
         * The obvious structure -- update every iteration, render when a buffer happens to be
         * free -- looks equivalent and is not. display_try_get() returns NULL whenever the RDP
         * has not drained, so the loop spins, and anything ticked per iteration runs at CPU
         * speed rather than at 60 Hz. That silently made the input-script frame counter
         * meaningless: a scripted run blew through every event in a few hundred microseconds
         * and exited before drawing anything. Per-frame state belongs on a per-frame clock. */
        uint32_t spin_start = TICKS_READ();
        surface_t *fb = display_try_get();
        if (fb == NULL) {
            /* The RDP is busy and the CPU has nothing else to do, which is the best moment to
             * stream. This is the window the background() phase exists for. */
            app->starved++;
            const screen_t *busy = SCREENS[app->screen];
            if (busy != NULL && busy->background != NULL) {
                busy->background(app, 0);
                app->bg_calls++;
            }
            sound_poll();
            /* Timed, because leaving it out is how the first attribution pass came up 7 ms per
             * frame short and sent me looking for the missing time in the decoder. A spin
             * iteration is not free: it runs a whole background() pass, and one decoded row in
             * there can cost more than the frame it is trying to stay out of the way of. */
            app->spin_us += TIMER_MICROS(TICKS_SINCE(spin_start));
            continue;
        }
        app->spin_us += TIMER_MICROS(TICKS_SINCE(spin_start));

        uint32_t now_ticks = TICKS_READ();
        uint32_t raw_us = TIMER_MICROS(TICKS_DISTANCE(prev_ticks, now_ticks));
        /* Clamped so a stall -- an SD read, a shader compile under ares -- cannot teleport an
         * animation across the screen or make one frame of repeat count as half a second. */
        app->dt = clampf((float)raw_us / 1e6f, 1.0f / 120.0f, 1.0f / 15.0f);
        prev_ticks = now_ticks;

        /* Bin the UNCLAMPED interval. app->dt is clamped at 1/15 s so that animation stays sane
         * across a stall, and reporting that number would cap every measurement at 66.7 ms and
         * quietly erase the only frames worth looking at.
         *
         * Bin by field rather than by microseconds: on a 60 Hz display the question is never
         * "how many milliseconds" but "did this frame make its field", and 17 ms and 32 ms are
         * the same answer to that question while 32 ms and 34 ms are not. */
        /* Round UP, not to nearest. A frame taking 25 ms has missed a field -- the display held
         * the previous one for two -- but rounds to nearest as 1 and would be counted on time,
         * which is precisely the frame this instrumentation exists to catch. The jitter
         * allowance keeps a frame that lands a few hundred microseconds late out of the miss
         * column, since the loop is not phase-locked to the VI and always overshoots slightly. */
        uint32_t fields = (raw_us + FIELD_US - FIELD_JITTER_US) / FIELD_US;
        if (fields < 1) fields = 1;
        if (fields > FRAMESTAT_BINS) fields = FRAMESTAT_BINS;
        app->fieldbin[fields - 1]++;
        if (raw_us > app->worst_us) app->worst_us = raw_us;

        while (app->next_screen != app->screen) {
            const screen_t *old = (app->screen < SCREEN_COUNT) ? SCREENS[app->screen] : NULL;
            if (old != NULL && old->leave != NULL) {
                old->leave(app);
            }
            app->screen = app->next_screen;
            const screen_t *now = SCREENS[app->screen];
            if (now != NULL && now->enter != NULL) {
                now->enter(app);
            }
        }

        const screen_t *screen = SCREENS[app->screen];

        /* Attribute the frame to update / render / everything-else, because the field bins say
         * only that 40 % of frames miss and not what spent the time. The budget sweep already
         * cost a run each on a hypothesis these three counters would have refuted immediately:
         * cutting the decode budget to zero changed nothing, so the time is somewhere else. */
        uint32_t t0 = TICKS_READ();
        input_poll(&app->input, app->dt);
        if (screen->update != NULL) {
            screen->update(app, app->dt);
        }
        uint32_t t1 = TICKS_READ();

        if (screen->render != NULL) {
            screen->render(app, fb);
        } else {
            rdpq_attach_clear(fb, NULL);
            rdpq_detach_show();
        }
        uint32_t t2 = TICKS_READ();

        app->update_us += TIMER_MICROS(TICKS_DISTANCE(t0, t1));
        app->render_us += TIMER_MICROS(TICKS_DISTANCE(t1, t2));

        switch (inputscript_take_action()) {
            case ISCRIPT_ACT_FBDUMP:
                dbg_fbdump(fb, DBG_FBDUMP_SCALE);
                break;
            case ISCRIPT_ACT_EXIT:
                debugf("INPUTSCRIPT done, requesting exit\n");
                DBG_EXIT();
                break;
            default:
                break;
        }

        /* Also run the background phase on a frame that was NOT starved. Relying only on the
         * display_try_get()==NULL branch means that whenever the RDP keeps up -- which with
         * three buffers is most of the time -- background work never gets a single microsecond
         * and simply never happens. The starved branch is opportunistic extra time, not the
         * guarantee. */
        if (screen->background != NULL) {
            uint32_t t3 = TICKS_READ();
            screen->background(app, 0);
            app->bg_us += TIMER_MICROS(TICKS_DISTANCE(t3, TICKS_READ()));
            app->bg_calls++;
        }

        app->frame++;
        if ((app->frame % 60) == 0) {
            /* Every frame in the window, not one instantaneous sample of it. Sampling app->dt
             * once per 60 frames described 1.7 % of the run and was read as if it described all
             * of it: it put the median at a clean 16.7 ms while a quarter of the samples sat
             * above 25 ms, and offered no way to tell whether that tail was three bad frames or
             * three hundred. */
            debugf("FRAME n=%lu f1=%lu f2=%lu f3=%lu f4+=%lu worst_us=%lu "
                   "upd_us=%lu rnd_us=%lu bg_us=%lu spin_us=%lu rowus=%lu scanus=%lu starts=%lu stats=%lu rows=%lu worstrow_us=%lu inf_us=%lu scl_us=%lu spin=%lu bg=%lu\n",
                   (unsigned long)app->frame,
                   (unsigned long)app->fieldbin[0], (unsigned long)app->fieldbin[1],
                   (unsigned long)app->fieldbin[2], (unsigned long)app->fieldbin[3],
                   (unsigned long)app->worst_us,
                   (unsigned long)(app->update_us / 60), (unsigned long)(app->render_us / 60),
                   (unsigned long)(app->bg_us / 60),
                   (unsigned long)(app->spin_us / 60),
                   (unsigned long)(thumb_rows_us / 60), (unsigned long)(thumb_scan_us / 60),
                   (unsigned long)thumb_starts, (unsigned long)thumb_statcalls,
                   (unsigned long)png_rows_done, (unsigned long)png_worst_row_us,
                   (unsigned long)(png_inflate_us / 60), (unsigned long)(png_scale_us / 60),
                   (unsigned long)app->starved,
                   (unsigned long)app->bg_calls);
            /* Separate line so the frame line stays greppable as one shape. The claim under test
             * is that a frame which is only drawing allocates nothing; art decoding legitimately
             * allocates, so read this alongside rows=. */
            debugf("HEAP n=%lu mallocs=%lu reallocs=%lu frees=%lu bytes=%lu\n",
                   (unsigned long)app->frame,
                   (unsigned long)alloc_stats.mallocs, (unsigned long)alloc_stats.reallocs,
                   (unsigned long)alloc_stats.frees, (unsigned long)alloc_stats.bytes);
            allocwatch_reset();
            for (int i = 0; i < FRAMESTAT_BINS; i++) app->fieldbin[i] = 0;
            app->worst_us = app->update_us = app->render_us = app->bg_us = app->spin_us = 0;
            png_rows_done = png_worst_row_us = png_inflate_us = png_scale_us = 0;
            thumb_rows_us = thumb_scan_us = thumb_starts = thumb_statcalls = 0;
        }
        sound_poll();
    }

    app_deinit(app);

    while (exception_reset_time() > 0) {
        /* Hold if the reset button was pressed, so we do not boot into a half-reset machine. */
    }
}
