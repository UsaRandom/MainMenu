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
#include "menu/cart_load.h"
#include "menu/fonts.h"
#include "menu/rom_info.h"
#include "screens/screens.h"
#include "ui/draw.h"
#include "ui/theme.h"

/* Loading is a single blocking call inside flashcart_load_rom(): it streams the whole ROM to the
 * cart and only returns when it is done, calling back with progress as it goes. There is no way
 * to drive that from update()/render() a frame at a time without rewriting the flashcart driver,
 * which is upstream code we keep verbatim.
 *
 * So this screen draws its own frames from inside the progress callback. That is the one place
 * in this codebase where drawing happens outside render(), and it is deliberate: the alternative
 * is a black screen for however long a 64 MB ROM takes over real SD. */

static const char *progress_label;
static app_t     *progress_app;
static bool       started;

/** @brief Draw the bar into an already-acquired framebuffer, and show it. */
static void draw_progress_into (surface_t *fb, float progress) {
    const theme_t *th = progress_app->theme;

    rdpq_attach_clear(fb, NULL);
    ui_fill(0, 0, SCREEN_W, SCREEN_H, th->bg);

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

/* Called from inside flashcart_load_rom(). Acquires its own framebuffer, because the one the
 * main loop handed to render() has already been shown by the time the load starts. Skips the
 * update rather than blocking when nothing is free -- a dropped progress frame is invisible,
 * a stalled load is not. */
static void on_progress (float progress) {
    surface_t *fb = display_try_get();
    if (fb != NULL) {
        draw_progress_into(fb, progress);
    }
}

static void launch_enter (app_t *app) {
    progress_app = app;
    started = false;
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
    (void)app; (void)dt;
    /* Nothing. The load runs from render(), which is the only place that owns a framebuffer.
     * Doing it here stranded one: the main loop calls display_try_get() BEFORE update(), so a
     * blocking load in update() holds a buffer that is never attached or shown, and three
     * progress ticks later there are none left. */
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

/** @brief Load the ROM and hand off to the bootloader. Blocking; draws its own frames. */
static void do_load (app_t *app) {
    int emu = -1;
    if (app->launch.rom_id >= 0 && app->launch.rom_id < app->lib->count) {
        emu = emu_type_for(app->lib->records[app->launch.rom_id].system);
    }

    cart_load_err_t err = (emu >= 0)
                        ? cart_load_emulator(app, (cart_load_emu_type_t)emu, on_progress)
                        : cart_load_n64_rom_and_save(app, on_progress);
    if (err != CART_LOAD_OK) {
        app->launch.err = err;
        app_fault(app, cart_load_convert_error_message(err));
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
    /* Show an empty bar first, so the screen is never blank while the load spins up, and so the
     * framebuffer the main loop gave us is properly shown before anything blocking starts. */
    draw_progress_into(fb, 0.0f);

    if (!started) {
        started = true;
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
