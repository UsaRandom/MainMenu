/**
 * @file app.c
 * @brief Application state and the main loop.
 * @ingroup app
 */

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <libdragon.h>

#include "app.h"
#include "cheats/cheatstate.h"
#include "cheats/usercheats.h"
#include "library/boxart.h"
#include "library/cache.h"
#include "library/libindex.h"
#include "library/locks.h"
#include "library/playstate.h"
#include "library/thumbstore.h"
#include "cheats/cheatdb.h"
#include "menu/image_decoder.h"
#include "dev/allocwatch.h"
#include "dev/debug_emux.h"
#include "dev/hooktest.h"
#include "dev/inputscript.h"
#include "flashcart/flashcart.h"
#include "menu/fonts.h"
#include "menu/music.h"
#include "menu/launchlog.h"
#include "menu/memprofile.h"
#include "menu/profile.h"
#include "ui/icon.h"
#include "menu/parental.h"
#include "menu/paths.h"
#include "menu/sound.h"
#include "screens/boot_plate.h"
#include "screens/screens.h"
#include "ui/tween.h"

#define CONFIG_FILE     "config.ini"

/** Where the search for games starts: the whole card, not a folder called `roms`.
 *
 * Someone who empties a zip onto a card should get a working menu, and someone who has kept their
 * collection in `Games\N64` for fifteen years should not have to rename it. The cost is a walk of
 * whatever else is on the card; library.c's SCAN_SKIP is what keeps that bounded, and AUDIT.md
 * carries what it measured. */
#define SCAN_ROOT       "/"

/** The wall clock a scripted run is pinned to: 2026-08-04 14:30:00 UTC. Mid-afternoon on purpose
 *  -- it is inside the 8 am to 8 pm window the parental panel defaults to, so a script that turns
 *  the schedule on is testing the allowed case unless it deliberately moves the hours. */
#define SCRIPT_CLOCK_EPOCH  1785853800L

/** The one track a scripted run plays. -1 restores the shuffle; see where this is used. */
#ifndef SCRIPT_MUSIC_TRACK
#define SCRIPT_MUSIC_TRACK  0
#endif

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

/** Feed the mixer, and charge the time to snd_us.
 *
 * Called several times across a frame rather than once at the bottom. mixer_try_play() fills
 * every buffer that has drained and then returns, so calling it more often does not do more work
 * -- it does the same work sooner, which is the whole difference between smooth audio and
 * choppy audio when the frame either side of it is long. Rendering and background() together can
 * hold the loop for tens of milliseconds, and until this existed nothing touched the mixer for
 * that entire stretch. */
static void pump_audio (app_t *app) {
    uint32_t t = TICKS_READ();
    sound_poll();
    app->snd_us += TIMER_MICROS(TICKS_SINCE(t));
}

void app_fault (app_t *app, const char *message) {
    app->fault_message = message;
    app->next_screen = SCREEN_FAULT;
}

/** @brief How often the scan is allowed to stop and paint. ~30 Hz. */
#define SCAN_TICK_US    33000

/**
 * @brief Paint the boot plate from inside the library scan. See library_scan_progress_t.
 *
 * The second place in this codebase where drawing happens outside render(), and for the same
 * reason as the first (screen_launch.c's on_progress): the work is one blocking call and the main
 * loop is suspended above it, so either this draws or nothing does.
 *
 * Three things it must do, and the middle one is not obvious:
 *
 *  - Throttle. The scan calls back once per directory entry, which on this card is over a
 *    thousand times. Painting each one would cost more than the scan.
 *  - Feed the mixer. Nothing else is touching the AI for the whole scan, and an N64 whose AI runs
 *    dry does not fall silent, it repeats the last buffer it was given -- the same fragment-of-
 *    audio symptom documented at length in screen_launch.c. Three seconds of it, here.
 *  - Advance the plate by REAL elapsed time, so the hold that would otherwise follow the scan is
 *    spent during it instead.
 *
 * Skips the paint rather than blocking when no framebuffer is free, on screen_launch's reasoning:
 * a dropped frame is invisible, a stalled scan is not. The clock is advanced either way, or a card
 * that happens to be short of buffers would come out of the scan with the hold still to serve.
 */
static void scan_progress (int found) {
    static uint32_t last_ticks;

    uint32_t now = TICKS_READ();
    uint32_t dt_us = (last_ticks != 0) ? TIMER_MICROS(TICKS_SINCE(last_ticks)) : SCAN_TICK_US;
    if (last_ticks != 0 && dt_us < SCAN_TICK_US) {
        return;
    }
    last_ticks = now;

    sound_poll();
    boot_plate_hold((float)dt_us / 1000000.0f);

    surface_t *fb = display_try_get();
    if (fb == NULL) {
        return;
    }
    rdpq_attach(fb, NULL);
    boot_plate_draw(MENU_VERSION, found);
    rdpq_detach_show();
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

    /* A scripted run has to be a pure function of the script, and the clock is not. ares hands the
     * console the host's time, so the settings screen's Clock row -- and every field the clock
     * screen seeds from it -- came out different on every run: two back-to-back runs of clock.txt
     * disagreed on all four of their frames, and parental.txt's first frame moved with them. The
     * hashes for those frames were noise being read as evidence. Same reasoning as the fixed dt in
     * AUDIT.md 1z, and it compiles out entirely without DEV_HARNESS. */
    if (inputscript_active()) {
        struct timeval tv = { .tv_sec = SCRIPT_CLOCK_EPOCH, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }
    rspq_init();
    rdpq_init();
    dfs_init(DFS_DEFAULT_LOCATION);

    input_init(&app->input);
    sound_init_default();
    sound_init_sfx();

    char cfg[300];
    menu_path(cfg, sizeof(cfg), app->storage, CONFIG_FILE);
    settings_init(cfg);
    settings_load(&app->settings);

    /* The code and the failure count are not settings and are not a cache: they are the one file
     * on the card a user is ever told to delete by hand. See parental.h. */
    parental_load(app->storage);

    /* Where the launch path records what it did, since USB is not a channel here. */
    launchlog_init(app->storage);

    /* The icon corpus, for the faces profiles wear. Absent is normal and not a fault: a build
     * made without ICON_DIR pointing at the artwork has no pack, and everything except the
     * picker works exactly as it did.
     *
     * Before profile_load(), and it has to be. A profile with no icon of its own takes the
     * default for its slot out of the pack's metadata, and every card written before this feature
     * existed is in exactly that state -- so with the order the other way round every profile on
     * every existing card would come up blank, permanently, because profile_load() only runs once.
     * Needs nothing but dfs_init(), which is well above. */
    mem_report("start");
    icon_init();
    mem_report("icons");

    /* Before cache_init(), which is fine -- profile_load() only reads an ini and clamps an index.
     * It has to happen before playstate_load() below, because which file that reads is a function
     * of which profile is active. */
    profile_load(app->storage);
    mem_report("profiles");

    /* The setting existed and the toggle drew, but nothing ever told the sound system about it --
     * turning sound effects off in settings changed a bool and nothing else. */
    sound_set_sfx_volume(app->settings.sfx_volume);

    /* Music starts here, before the library scan, and deliberately: building the oscillator
     * tables is the one unchunkable cost in the whole boot, so it goes next to the other fixed
     * boot costs rather than landing on a keypress later.
     *
     * A scripted run plays too, rather than being silenced, because the cost of music is a frame
     * cost and silencing it would make the one harness that can measure that cost the one place
     * it never appears. What a script does NOT get is "All tracks": advancing to the next song
     * happens when the mixer has pulled a song's worth of samples, which is a function of how
     * fast ares happens to run rather than of the frame counter -- and each advance allocates, so
     * tools/inputs/idle.txt's "a settled frame allocates nothing" gate would go red or green
     * depending on the host's mood. One track, looping, allocates nothing after boot.
     *
     * Overridable so the shuffle itself can be exercised, which is otherwise unreachable from any
     * harness -- `make TUNE='-DSCRIPT_MUSIC_TRACK=-1'` restores it for a run whose frames are not
     * being compared. Without that knob "does the seed actually vary" has no way of being asked. */
    music_start(inputscript_active() ? SCRIPT_MUSIC_TRACK : app->settings.music_track,
                app->settings.music_volume);
    mem_report("music");

    /* The theme belongs to the person, not the console, so it comes off the active profile. Until
     * profiles existed this was a bare assignment and the setting was never persisted at all --
     * changing the theme in Settings lasted exactly as long as the power did. */
    app->theme = theme_by_name(profile_theme(profile_active()));

    resolution_t resolution = { .width = SCREEN_W, .height = SCREEN_H,
                                .interlaced = INTERLACE_OFF };
    /* Not FB_COUNT any more: on 4 MB the third buffer is the allocation that failed, and it failed
     * inside display_init's assert -- so the program did not start at all on a console without an
     * Expansion Pak. With a pak this is still 3 and nothing about the build changes. */
    display_init(resolution, DEPTH_16_BPP, mem_fb_count(), GAMMA_NONE, FILTERS_RESAMPLE);
    mem_report("display");

    mem_report("pre-fonts");
    fonts_init(NULL);
    mem_report("post-fonts");
    /* After fonts_init, not before: it registers the styles this rebinds. */
    theme_apply(app->theme);

    if (ferr != FLASHCART_OK) {
        /* Say which failure it was. "No supported flashcart detected" was printed for every one
         * of them, including FLASHCART_ERR_SD_CARD -- which means an unreadable or unformatted
         * card would have sent someone hunting a cart-detection problem that was not there. The
         * cases are distinguishable and this is the screen that has to distinguish them. */
        app_fault(app, flashcart_convert_error_message(ferr));
        return;
    }

    /* Box shapes before the library, because everything that touches art asks for a shape: the
     * grid for its row height, the thumbnail cache for the size to decode into, the atlas for
     * whether a cached tile still fits. Reading one small ini before any of them exist is
     * cheaper than making all three cope with not knowing yet. */
    boxart_init(app->storage, app->settings.boxart_region);

    app->lib = library_init();
    if (app->lib == NULL) {
        app_fault(app, "Out of memory building the library.");
        return;
    }
    /* Cache first, scan only if it will not do. At a measured 11,499 us per ROM a 500-title
     * scan is 5.75 s, which is the single largest fixed cost in the product; libindex_load()
     * answers the same question with a directory enumeration and no file opens at all. */
    cache_init(app->storage);

    
    /* Harness builds only: execute the emitted cheat patcher against a synthetic game image and
     * prove the preamble hook end-to-end, which no launch under ares can do -- ares has no cart
     * to launch into. Costs one boot-time megabyte scan; compiles to nothing in release. */
    hooktest_run();

    /* Before the scan, not after it, and this is the whole point of arming it here rather than
     * leaving it to the first screen's enter(). The scan is one blocking call -- 11,499 us per
     * ROM, so 3.2 s on a 278-title card -- and it ran with nothing on screen at all, after which
     * the plate started its own 2.5 s hold. Sequential, so the plate covered none of the cost it
     * exists to cover, and a card that grew from 19 titles to 278 turned a fast boot into a long
     * one with a blank screen for most of it.
     *
     * Armed unconditionally rather than only on the slow path: on a warm index there is no scan
     * to cover, boot_plate_hold() is never called, and the plate behaves exactly as it did. The
     * grid and the picker still call boot_plate_arm() in their enter() and both are now no-ops,
     * which is the documented contract -- first caller wins. */
    boot_plate_arm();

    /* Where the boot time actually goes, written to the card because nothing else on this console
     * can say. The index either revalidates in a directory walk or it does not and the whole card
     * is rescanned, and those two differ by seconds -- but from the outside they look identical:
     * a plate, then a grid. Both are timed and both are reported, so "the list is slow" becomes a
     * number with a cause attached instead of a hypothesis. */
    uint32_t t_idx = TICKS_READ();
    libindex_result_t idx = { 0 };
    bool idx_hit = libindex_load(app->lib, app->storage, SCAN_ROOT, scan_progress, &idx);
    uint32_t idx_us = TIMER_MICROS(TICKS_SINCE(t_idx));
    uint32_t scan_us = 0;

    if (!idx_hit) {
        uint32_t t_scan = TICKS_READ();
        library_scan(app->lib, app->storage, SCAN_ROOT, scan_progress);
        /* Between the scan and the save, and it has to be between them. The scan is the only
         * thing that ever fills lib->art, and nothing rebuilds that table when the index loads
         * instead -- so unless the answers are written into the index now, every boot from a warm
         * index finds no art for anything. Hardware showed exactly that: art on the first run,
         * none after a restart, and a library.idx holding 56 strings without a single image path
         * among them. Memory only; the scan already saw every file. */
        library_resolve_loose_art(app->lib);
        libindex_save(app->lib, app->storage, SCAN_ROOT);
        scan_us = TIMER_MICROS(TICKS_SINCE(t_scan));
    }

    mem_report("library");

    /* Best-effort and never load-bearing, same as every other line this file writes. Opened here
     * and closed immediately: the launch path opens the log again later with its own banner, and
     * two open handles on one file is not something to find out about on a card. */
    launchlog_begin_boot();
    if (launchlog_open()) {
        const char *verdict = "index MISS -> full scan";
        if (idx_hit) {
            verdict = idx.incremental ? "index REPAIRED" : "index HIT";
        }
        launchlog_line("%s %s  storage [%s]  cache %s", MENU_VERSION, verdict,
                       app->storage, cache_status());
        launchlog_line("titles %d", app->lib->count);
        /* Which memory profile ran, on the card, because "the art cache is smaller than I expected"
         * and "this console has no Expansion Pak" are the same fact and only one of them is
         * visible from the sofa. */
        launchlog_line("memory %u bytes, %s profile", mem_total_bytes(),
                       mem_small() ? "SMALL (no Expansion Pak)" : "full");
        if (idx.incremental) {
            /* The two halves separately, because they answer different questions: how much of the
             * card the repair had to read, and whether the directory signatures are actually
             * localising the change or firing across the whole tree. */
            launchlog_line("repair %lu us: %d of %d dirs rescanned, %d titles kept, %d rescanned",
                           (unsigned long)idx_us, idx.dirs_rescanned, idx.dirs_total,
                           idx.records_kept, idx.records_scanned);
        } else if (idx_hit) {
            launchlog_line("revalidate %lu us", (unsigned long)idx_us);
        } else {
            launchlog_line("revalidate %lu us (rejected), scan %lu us = %lu us/rom",
                           (unsigned long)idx_us, (unsigned long)scan_us,
                           (unsigned long)(app->lib->count ? scan_us / (uint32_t)app->lib->count : 0));
        }
        launchlog_end();
    }

    /* After the library exists, never before: playstate is applied onto records and keys on the
     * check codes the index or the scan just produced. */
    playstate_load(app->lib);
    /* After playstate, never before: locks_load() decides whether a card is carrying its padlocks
     * in the old place by looking at what playstate just applied. See locks.h. */
    locks_load(app->lib);
    cheatstate_load();
    usercheats_load();

    /* Opened once and held: the index is 24 bytes a game, and the alternative is reopening the
     * file every time a detail sheet appears. Absent is normal -- a card with no cheats.db is a
     * card whose owner did not install one, not a fault. */
    cheatdb_open(app->storage);

    app->thumbs = thumbcache_init(app->storage);
    if (app->thumbs == NULL) {
        app_fault(app, "Out of memory allocating the art cache.");
    }
    thumbstore_open();

    /* The last thing app_init does, and it has to be: asking who is playing is only worth a
     * screen once the library behind it exists, and app_fault() above may already have taken the
     * next screen for itself. A single-profile card never gets here at all -- it boots to the
     * grid, same first frame as before this feature existed. */
    if (app->next_screen == SCREEN_GRID && screen_profiles_needed()) {
        screen_profiles_ask();
        app_goto(app, SCREEN_PROFILES);
    }
}

static void app_deinit (app_t *app) {
    /* cheats is NOT freed here: boot_params->cheat_list may point into a buffer built from it,
     * and main() calls boot() the moment this returns. */
    /* Last chance to persist. This runs on the way to booting a ROM, so anything not written
     * here is lost -- the menu does not come back, the game does. Ordered before the frees
     * because playstate_save() reads the library it is about to lose. */
    if (app->lib != NULL && app->lib->dirty) {
        /* Rewrite the index now that art resolution has run. The copy written at boot was made
         * before a single tile had been asked for, so it carried no art paths at all -- see
         * library_t::dirty. Without this the five-rule art search is repeated in full on every
         * boot for the entire library, which is the one cost the index was supposed to remove. */
        libindex_save(app->lib, app->storage, SCAN_ROOT);
    }
    /* app->lib is NULL whenever app_init() faulted before building it -- no flashcart, or out of
     * memory. Nothing can have marked playstate dirty in that case, so this guard has never
     * fired; it is here because "has never fired" is a property of the current fault paths and
     * not of this function, and the failure would be a null dereference on the way out. */
    if (app->lib != NULL && playstate_dirty()) {
        playstate_save(app->lib);
    }
    if (app->lib != NULL && locks_dirty()) {
        locks_save(app->lib);
    }
    if (cheatstate_dirty()) {
        cheatstate_save();
    }
    if (usercheats_dirty()) {
        usercheats_save();
    }
    thumbstore_close();
    cheatstate_free();
    usercheats_free();

    cheatdb_close();
    thumbcache_free(app->thumbs);
    app->thumbs = NULL;
    library_free(app->lib);
    app->lib = NULL;

    display_close();
    /* Before sound_deinit(), which closes the mixer the player is attached to. */
    music_shutdown();
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
            music_poll();
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

        /* A scripted run gets a FIXED dt, so the whole run is a pure function of the script.
         *
         * Without this, every animated value is a function of how many CPU cycles the frame took,
         * and that is a function of the binary. The selection outline pulses on `phase += 6*dt`,
         * so the outline colour at a given frame moves by one step on the RGBA5551 ladder for any
         * change to the code -- and the hash of every screenshot containing a selected tile moves
         * with it. That was measured, not assumed: inserting a `volatile int[64]` that nothing
         * reads into app_init() changed grid-edges frame 00 and changed frame 01 BACK to the value
         * it had two builds earlier. Ten of the suite's thirteen scripts moved for a change that
         * touched none of the code they exercise, each by exactly 104 pixels of one colour.
         *
         * The M1 reproducibility gate still passed throughout, because the same binary always
         * produces the same cycle counts. What was broken is the comparison the suite exists for:
         * `diff before/hashes.txt after/hashes.txt` could not tell "the drawing changed" from
         * "the binary got bigger", which makes a red result something you learn to ignore.
         *
         * This does not weaken the "motion is specified in seconds, never frames" rule -- the
         * animation code is unchanged and still integrates dt. It fixes the clock the harness
         * runs it against, exactly as the input scripts are keyed on frame number rather than on
         * elapsed time, and for the same reason. Frame-time measurement is unaffected: the bins
         * below use the UNCLAMPED interval and frametime.c reads TICKS directly. */
        if (inputscript_active()) {
            app->dt = 1.0f / 60.0f;
        }

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

        /* Between render and background, which is the longest unfed stretch in the frame. */
        pump_audio(app);

        /* Continuous capture for the demo video. Checked before the one-shot actions so a
         * `fbdump` inside a recorded stretch cannot dump the same frame twice, which would show
         * up in the finished video as a single stuttered frame and be very hard to explain. */
        if (inputscript_recording()) {
            dbg_fbdump(fb, DBG_FBDUMP_SCALE);
        }

        switch (inputscript_take_action()) {
            case ISCRIPT_ACT_FBDUMP:
                if (!inputscript_recording()) {
                    dbg_fbdump(fb, DBG_FBDUMP_SCALE);
                }
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

        /* Icon rasterising, here rather than in each screen's background(), because three screens
         * want it and the one that forgot would be the one showing blank plates. It costs nothing
         * on a screen that has requested nothing: icon_pump() returns immediately with an empty
         * queue.
         *
         * ICON_BUDGET_US is about one 40 px icon per frame at svg64's measured 4.8 ms. The cap is
         * on TIME and not on a count of three, deliberately: the worst file in the corpus takes
         * 31 ms, and a count-only budget would call three of those a normal frame and drop six
         * fields saying so. See src/ui/icon.h.
         *
         * Bracketed on its own. It sat outside every timer at first, which meant the one genuinely
         * new cost in this program appeared in no column of the FRAME line -- and the picker
         * reported bg_us=42 while taking 90 ms frames, which reads as "icons are free" and is
         * exactly the convincing fake number AUDIT.md keeps warning about. */
        uint32_t t4 = TICKS_READ();
        app->icons_done += icon_pump(TICKS_FROM_US(ICON_BUDGET_US));
        app->icon_us += TIMER_MICROS(TICKS_DISTANCE(t4, TICKS_READ()));

        app->frame++;
        if ((app->frame % 60) == 0) {
            /* Every frame in the window, not one instantaneous sample of it. Sampling app->dt
             * once per 60 frames described 1.7 % of the run and was read as if it described all
             * of it: it put the median at a clean 16.7 ms while a quarter of the samples sat
             * above 25 ms, and offered no way to tell whether that tail was three bad frames or
             * three hundred. */
            debugf("FRAME n=%lu f1=%lu f2=%lu f3=%lu f4+=%lu worst_us=%lu "
                   "upd_us=%lu rnd_us=%lu bg_us=%lu icon_us=%lu icons=%lu snd_us=%lu spin_us=%lu rowus=%lu scanus=%lu starts=%lu stats=%lu rows=%lu worstrow_us=%lu inf_us=%lu scl_us=%lu spin=%lu bg=%lu\n",
                   (unsigned long)app->frame,
                   (unsigned long)app->fieldbin[0], (unsigned long)app->fieldbin[1],
                   (unsigned long)app->fieldbin[2], (unsigned long)app->fieldbin[3],
                   (unsigned long)app->worst_us,
                   (unsigned long)(app->update_us / 60), (unsigned long)(app->render_us / 60),
                   (unsigned long)(app->bg_us / 60),
                   (unsigned long)(app->icon_us / 60), (unsigned long)app->icons_done,
                   (unsigned long)(app->snd_us / 60),
                   (unsigned long)(app->spin_us / 60),
                   (unsigned long)(thumb_rows_us / 60), (unsigned long)(thumb_scan_us / 60),
                   (unsigned long)thumb_starts, (unsigned long)thumb_statcalls,
                   (unsigned long)img_rows_done, (unsigned long)img_worst_row_us,
                   (unsigned long)(img_entropy_us / 60), (unsigned long)(img_scale_us / 60),
                   (unsigned long)app->starved,
                   (unsigned long)app->bg_calls);
            /* Separate line so the frame line stays greppable as one shape. The claim under test
             * is that a frame which is only drawing allocates nothing; art decoding legitimately
             * allocates, so read this alongside rows=. */
            heap_stats_t hs;
            sys_get_heap_stats(&hs);
            debugf("HEAP n=%lu mallocs=%lu reallocs=%lu frees=%lu bytes=%lu "
                   "total=%d used=%d free=%d\n",
                   (unsigned long)app->frame,
                   (unsigned long)alloc_stats.mallocs, (unsigned long)alloc_stats.reallocs,
                   (unsigned long)alloc_stats.frees, (unsigned long)alloc_stats.bytes,
                   hs.total, hs.used, hs.total - hs.used);
            allocwatch_reset();
            for (int i = 0; i < FRAMESTAT_BINS; i++) app->fieldbin[i] = 0;
            app->worst_us = app->update_us = app->render_us = app->bg_us = app->spin_us = 0;
            app->icon_us = app->icons_done = 0;
            app->snd_us = 0;
            img_rows_done = img_worst_row_us = img_entropy_us = img_scale_us = 0;
            thumb_rows_us = thumb_scan_us = thumb_starts = thumb_statcalls = 0;
        }
        /* snd_us is timed because the first music measurement had to be taken by subtraction:
         * update and render barely moved when music was switched on, and the whole cost -- 17 of
         * 60 frames losing a field on an otherwise idle screen -- sat in the unattributed
         * remainder. A cost that large must not be something you infer. It reports about 1 ms a
         * frame, which is a floor and not the answer; see AUDIT.md. */
        pump_audio(app);
        /* After the pump, so a track that just ended is replaced on the same frame the mixer
         * noticed rather than one later. */
        music_poll();
    }

    app_deinit(app);

    while (exception_reset_time() > 0) {
        /* Hold if the reset button was pressed, so we do not boot into a half-reset machine. */
    }
}
