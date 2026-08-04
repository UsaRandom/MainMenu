/**
 * @file screen_launch.c
 * @brief Loading a game onto the cart and handing off to the bootloader.
 * @ingroup screens
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "app.h"
#include "cheats/cheatdb.h"
#include "library/library.h"
#include "flashcart/flashcart.h"
#include "menu/cart_load.h"
#include "utils/fs.h"
#include "library/playstate.h"
#include "cheats/cheatstate.h"
#include <time.h>
#include "menu/fonts.h"
#include "menu/rom_info.h"
#include "screens/screens.h"
#include "ui/draw.h"
#include "ui/theme.h"
#include "ui/tween.h"

/* Loading is a single blocking call inside flashcart_load_rom(): it streams the whole ROM to the
 * cart and only returns when it is done, calling back with progress as it goes. There is no way
 * to drive that from update()/render() a frame at a time without rewriting the flashcart driver,
 * which is upstream code we keep verbatim. So this screen draws its own frames from inside the
 * progress callback -- the one place in this codebase where drawing happens outside render().
 *
 * ## Why there is usually no progress bar
 *
 * This screen was a bar and a percentage. It should almost never be seen. The SC64 reads its SD
 * at around **22 MB/s**, so a typical 8-16 MB ROM is streamed in 0.4-0.7 s and even a 64 MB
 * cartridge is under three seconds. A progress bar that appears and completes inside half a
 * second is not information, it is a flicker -- and it makes a launch feel like a transaction
 * rather than like starting a game.
 *
 * So the normal path is a **fade to black**, the same gesture as a console cutting to a game, and
 * the load happens behind it. The bar is kept for the case that actually warrants one: if a load
 * is still running after LATE_BAR_US it appears, because at that point the user is waiting and a
 * black screen with nothing on it stops being a transition and starts being a hang.
 *
 * That 22 MB/s figure is the cart owner's, not a measurement taken here -- ares has no SD at all.
 * It is used for the simulated load and nothing depends on it being exact; the threshold below is
 * far enough above any plausible fast-path load that being wrong by 2x changes nothing. */

/** The cut.
 *
 * 0.55 s, matching the boot plate's rise, so leaving the menu takes as long as arriving in it.
 * The first version was 0.22 s on the reasoning that a transition should be brief; at that length
 * it reads as a flinch rather than a decision, and since the load itself is only ~0.7 s behind it
 * there is nothing to be brief for. Slower is the whole point -- the fade IS the loading screen
 * now, so it should be the part with presence. */
#define DUR_LAUNCH_FADE   0.55f

/** How long a load may run behind the black before it earns a progress bar. */
#define LATE_BAR_US       1500000

static const char *progress_label;
static app_t     *progress_app;
static bool       started;
static float      fade_t;
static uint32_t   load_start_ticks;

/**
 * @brief Black over whatever is already in @p fb, at @p k of full.
 *
 * Composited rather than cleared, and that is the whole trick. With three buffers the one handed
 * back here still holds the detail sheet from a few frames ago, and by the time Start is pressed
 * that sheet is static -- so every buffer holds the same picture and laying absolute-alpha black
 * over it reads as the sheet dimming out. Clearing first would give a black screen with a fade
 * that has nothing to fade.
 */
static void draw_fade_into (surface_t *fb, float k) {
    rdpq_attach(fb, NULL);
    uint8_t a = (uint8_t)(clampf(k, 0.0f, 1.0f) * 255.0f);
    if (a >= 250) {
        ui_fill(0, 0, SCREEN_W, SCREEN_H, RGBA5551(0, 0, 0));
    } else {
        ui_wash(0, 0, SCREEN_W, SCREEN_H, RGBA5551(0, 0, 0), a);
    }
    rdpq_detach_show();
}

/** @brief The late bar, on black. Only reached when a load outlives LATE_BAR_US. */
static void draw_progress_into (surface_t *fb, float progress) {
    const theme_t *th = progress_app->theme;

    rdpq_attach_clear(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, RGBA5551(0, 0, 0));

    int bar_w = 384;
    int bar_h = 12;
    int bar_x = (SCREEN_W - bar_w) / 2;
    int bar_y = SCREEN_H / 2;

    ui_fill(bar_x, bar_y, bar_w, bar_h, th->panel);

    int filled = (int)(progress * (float)bar_w);
    ui_fill(bar_x, bar_y, filled, bar_h, th->text_accent);

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    ui_text(bar_x, bar_y - 40, bar_w, ALIGN_CENTER, STL_DEFAULT,
            progress_label ? progress_label : "Loading");

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(progress * 100.0f));
    ui_text(bar_x, bar_y + bar_h + 12, bar_w, ALIGN_CENTER, STL_GRAY, pct);

    rdpq_detach_show();
}

/* Called from inside flashcart_load_rom(). Acquires its own framebuffer, because the one the main
 * loop handed to render() has already been shown by the time the load starts. Skips the update
 * rather than blocking when nothing is free -- a dropped frame here is invisible, a stalled load
 * is not.
 *
 * Stays black until the load has outlived LATE_BAR_US, so the common sub-second load is a clean
 * cut and only a genuinely slow one grows a bar. */
static void on_progress (float progress) {
    surface_t *fb = display_try_get();
    if (fb == NULL) {
        return;
    }
    if (TIMER_MICROS(TICKS_SINCE(load_start_ticks)) < LATE_BAR_US) {
        draw_fade_into(fb, 1.0f);
    } else {
        draw_progress_into(fb, progress);
    }
}

static void launch_enter (app_t *app) {
    progress_app = app;
    started = false;
    fade_t = 0.0f;
    progress_label = app->launch.rom_info.title[0] ? app->launch.rom_info.title : "Loading ROM";
    app->launch.err = CART_LOAD_OK;
}

static void launch_leave (app_t *app) {
    if (app->launch.rom_path != NULL) {
        path_free(app->launch.rom_path);
        app->launch.rom_path = NULL;
    }
    progress_app = NULL;
}

static void launch_update (app_t *app, float dt) {
    (void)app;
    fade_t += dt;
    /* Nothing. The load runs from render(), which is the only place that owns a framebuffer.
     * Doing it here stranded one: the main loop calls display_try_get() BEFORE update(), so a
     * blocking load in update() holds a buffer that is never attached or shown, and three
     * progress ticks later there are none left. */
}

/**
 * @brief Now, as a playstate timestamp, on a machine whose clock may not exist.
 *
 * `rtc_init()` returns false when no RTC source is found, and app.c does not check it because
 * there is nothing useful to do about it. The consequence lands here: with no clock,
 * gettimeofday() fails, `time(NULL)` returns (time_t)-1, and `(uint32_t)-1` is 0xFFFFFFFF.
 *
 * That number is wrong in a way that persists. It marks the record as played, which is right --
 * Recent is keyed on `last_played != 0` and Recent is the opening tab -- but it is also larger
 * than every real timestamp until the year 2106. Play three games on a clockless machine, then
 * fit a working RTC, and those three sit above everything genuinely recent for the rest of the
 * card's life. 1 says exactly the same thing about "was this played" and loses to every real
 * time, so the mistake ages out instead of compounding.
 *
 * The ModRetro M64 is a clone console and its joybus RTC behaviour is unverified; the SC64
 * carries one of its own. Which of them answers is a hardware question, and this is what makes
 * the answer not matter.
 */
static uint32_t play_timestamp (void) {
    time_t t = time(NULL);
    return (t == (time_t)-1 || t <= 0) ? 1u : (uint32_t)t;
}

/* Worst case the engine can hold. boot/cheats.c has 0x807F8000 - 0x807C5C00 = 205,824 bytes to
 * assemble into, and a conditional plus its write is seven instructions per two code lines, so
 * roughly 14,700 lines fit. 4,096 is well inside that and bounds the heap allocation at 32 KB;
 * cheatdb_emit() reports and stops rather than overrunning if a selection exceeds it. */
#define CHEAT_EMIT_MAX_LINES   4096
#define CHEAT_EMIT_MAX_WORDS   (CHEAT_EMIT_MAX_LINES * 2 + 2)

/**
 * @brief Flatten the enabled cheat groups for the bootloader, or NULL if there are none.
 *
 * Heap, not stack: upstream put a MAX_CHEAT_CODE_ARRAYLIST_SIZE array in an automatic in
 * load_rom.c, which was fine at 64 codes and is 32 KB here. boot() runs after this returns and
 * reads through the pointer, so the allocation is deliberately never freed.
 */
static uint32_t *build_cheat_list (app_t *app) {
    if (app->cheats.group_count == 0 || !is_memory_expanded()) {
        /* The Expansion Pak check stays even though the M64 has one built in. If it ever reports
         * false the engine's address range does not exist and installing would write into
         * nothing; better to skip cheats loudly than to boot a corrupted game. */
        if (app->cheats.group_count > 0) {
            debugf("cheats: no Expansion Pak, skipping %d groups\n", app->cheats.group_count);
        }
        return NULL;
    }

    uint32_t *list = malloc(CHEAT_EMIT_MAX_WORDS * sizeof(uint32_t));
    if (list == NULL) {
        return NULL;
    }

    size_t words = cheatdb_emit(&app->cheats, list, CHEAT_EMIT_MAX_WORDS);
    if (words == 0) {
        free(list);
        return NULL;              /* nothing ticked; not an error */
    }

    debugf("cheats: %u words, %u lines\n", (unsigned)words, (unsigned)((words - 2) / 2));
    return list;
}

/**
 * @brief Which emulator core plays this system, or -1 for a native N64 ROM.
 *
 * The cores are separate ROMs the user drops in /menu/emulators; cart_load_emulator() loads the
 * core and then the game into it at a fixed offset. A missing core is reported as such rather
 * than as a failure to load the game, because the two have completely different fixes.
 */
static int emu_type_for (uint8_t system) {
    switch (system) {
        case SYS_NES:  return CART_LOAD_EMU_TYPE_NES;
        case SYS_SNES: return CART_LOAD_EMU_TYPE_SNES;
        case SYS_GB:   return CART_LOAD_EMU_TYPE_GAMEBOY;
        case SYS_GBC:  return CART_LOAD_EMU_TYPE_GAMEBOY_COLOR;
        case SYS_SMS:  return CART_LOAD_EMU_TYPE_SEGA_GENERIC_8BIT;
        default:       return -1;
    }
}

/* Simulated-load pacing, used only when there is no real flashcart.
 *
 * 22 MB/s is the cart owner's figure for SC64 SD reads. It is not measured here and cannot be --
 * ares has no SD -- but it is the number the screen above is designed around, so the simulation
 * uses the same one rather than a more pessimistic one that would make the fade look wrong. */
#define SIM_RATE_KB_PER_S   22000
/* The fixture's ROMs are truncated to a 4 KB header, so an honestly-simulated load of one is over
 * in a hundred microseconds and shows nothing. Anything under this is treated as a stand-in for a
 * real cartridge and given a nominal size instead, which is stated in the log rather than hidden. */
#define SIM_STUB_MAX        (1024 * 1024)
#define SIM_NOMINAL_BYTES   (16 * 1024 * 1024)
#define SIM_MAX_US          6000000

/**
 * @brief Pretend to load, at a plausible rate, when there is no cart to load onto.
 *
 * Under an emulator `flashcart` is the dummy driver, and `dummy_load_rom()` returns FLASHCART_OK
 * immediately without copying a byte and **without ever calling the progress callback**. So the
 * real path here draws one 0 % frame, believes the load succeeded, and hands control to boot() --
 * which jumps into a cart that has nothing on it. The loading screen was therefore not merely
 * untested under ares, it was unreachable: there was no state in which it drew anything but zero.
 *
 * This is not a test hook bolted onto production code. Booting is impossible without a cart, so
 * on the dummy driver the honest behaviour is to show what a load looks like and return to the
 * grid rather than to jump into nothing. It cannot engage on hardware: flashcart_is_dummy() is
 * false the moment a real cart is detected.
 *
 * Everything up to the handoff still runs for real -- the cheat groups are flattened by the
 * actual emitter, play history is stamped, selections are captured -- so the only thing being
 * faked is the byte copy that has no destination.
 */
static void simulate_load (app_t *app) {
    int64_t bytes = (app->launch.rom_path != NULL)
                  ? file_get_size(path_get(app->launch.rom_path)) : 0;
    if (bytes < 0) {
        bytes = 0;
    }

    bool nominal = (bytes < SIM_STUB_MAX);
    if (nominal) {
        bytes = SIM_NOMINAL_BYTES;
    }

    uint32_t us = (uint32_t)((bytes / 1024) * 1000000ull / SIM_RATE_KB_PER_S);
    if (us > SIM_MAX_US) {
        us = SIM_MAX_US;
    }

    debugf("LAUNCH simulated: %lld bytes%s at %d KB/s = %lu ms (no flashcart present)\n",
           (long long)bytes, nominal ? " (nominal; the fixture ROM is a 4 KB stub)" : "",
           SIM_RATE_KB_PER_S, (unsigned long)(us / 1000));

    uint32_t t0 = TICKS_READ();
    for (;;) {
        uint32_t elapsed = TIMER_MICROS(TICKS_SINCE(t0));
        if (elapsed >= us) {
            break;
        }
        on_progress((float)elapsed / (float)us);
    }
    on_progress(1.0f);
}

/** @brief Load the ROM and hand off to the bootloader. Blocking; draws its own frames. */
static void do_load (app_t *app) {
    int emu = -1;
    if (app->launch.rom_id >= 0 && app->launch.rom_id < app->lib->count) {
        emu = emu_type_for(app->lib->records[app->launch.rom_id].system);
    }

    if (flashcart_is_dummy()) {
        simulate_load(app);
    }

    cart_load_err_t err = flashcart_is_dummy() ? CART_LOAD_OK
                        : (emu >= 0)
                        ? cart_load_emulator(app, (cart_load_emu_type_t)emu, on_progress)
                        : cart_load_n64_rom_and_save(app, on_progress);
    if (err != CART_LOAD_OK) {
        app->launch.err = err;
        app_fault(app, cart_load_convert_error_message(err));
        return;
    }

    /* Count the play now, while we know the load succeeded. Recording it any earlier would
     * credit a play to a game that failed to load, and there is no "later" -- the next thing
     * that happens is boot(), and this program does not come back.
     *
     * app_deinit() is what actually writes it; this only marks the record. */
    if (app->launch.rom_id >= 0 && app->launch.rom_id < app->lib->count) {
        playstate_played(&app->lib->records[app->launch.rom_id], play_timestamp());

        /* Cheat selections are captured when the detail sheet is backed out of, which misses the
         * most likely path of all: tick some cheats, press Start, play. Captured here as well so
         * the ticks survive the one journey the user actually makes. cheatstate_capture()
         * replaces rather than merges, so doing it twice is harmless. */
        cheatstate_capture(&app->cheats, playstate_key(&app->lib->records[app->launch.rom_id]));
    }

    if (flashcart_is_dummy()) {
        /* Everything above ran. What comes next cannot: boot() would jump into a cart holding
         * nothing but this menu. Report what WOULD have been handed over, free the cheat list
         * that nobody is going to read, and go back to the grid. */
        uint32_t *would_be = build_cheat_list(app);
        debugf("LAUNCH would boot %s with %s\n",
               app->launch.rom_info.title[0] ? app->launch.rom_info.title : "(untitled)",
               would_be ? "cheats" : "no cheats");
        free(would_be);
        app_goto(app, SCREEN_GRID);
        return;
    }

    /* Hand the bootloader everything it needs, then stop the main loop. main() calls boot()
     * immediately after app_run() returns, with interrupts already disabled. */
    boot_params_t *bp = app->boot;
    bp->device_type = BOOT_DEVICE_TYPE_ROM;

    /* An emulated game boots the CORE, so the CIC and TV type come from the core's own header,
     * which flashcart_load_rom has already put on the cart. Detecting from the emulated game's
     * rom_info would describe the NES file, not the N64 ROM the console is about to run. */
    if (emu >= 0) {
        bp->detect_cic_seed = true;
        bp->tv_type = BOOT_TV_TYPE_PASSTHROUGH;
        bp->cheat_list = NULL;              /* the engine patches N64 code; there is none here */
        app->running = false;
        return;
    }

    bp->detect_cic_seed = rom_info_get_cic_seed(&app->launch.rom_info, &bp->cic_seed);

    switch (rom_info_get_tv_type(&app->launch.rom_info)) {
        case ROM_TV_TYPE_PAL:  bp->tv_type = BOOT_TV_TYPE_PAL;  break;
        case ROM_TV_TYPE_NTSC: bp->tv_type = BOOT_TV_TYPE_NTSC; break;
        case ROM_TV_TYPE_MPAL: bp->tv_type = BOOT_TV_TYPE_MPAL; break;
        default:               bp->tv_type = BOOT_TV_TYPE_PASSTHROUGH; break;
    }

    bp->cheat_list = build_cheat_list(app);

    app->running = false;
}

static void launch_render (app_t *app, surface_t *fb) {
    float k = fade_t / DUR_LAUNCH_FADE;
    draw_fade_into(fb, k);

    /* The load does not start until the screen is actually black. It blocks for its whole
     * duration, so starting it a frame early would freeze the fade partway through and the cut
     * would read as a stutter rather than as a transition. */
    if (!started && k >= 1.0f) {
        started = true;
        load_start_ticks = TICKS_READ();
        do_load(app);
    }
}

const screen_t SCREEN_LAUNCH_DEF = {
    .id     = SCREEN_LAUNCH,
    .enter  = launch_enter,
    .leave  = launch_leave,
    .update = launch_update,
    .render = launch_render,
};
