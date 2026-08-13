/**
 * @file app.h
 * @brief Application state and the screen interface.
 * @ingroup app
 *
 * Replaces upstream's menu.c and its `view_t { init, show(menu_t*, surface_t*) }` table.
 *
 * The important change is that `show` becomes three callbacks. Upstream fuses input handling
 * and rendering into one function, which means you cannot step a screen's logic without also
 * drawing it, cannot keep a model updated while something else draws, and cannot run any work
 * during the window where the CPU is otherwise waiting on the RDP. The thumbnail streamer needs
 * exactly that window, so the split is not tidiness -- it is the thing that makes streaming
 * possible at all.
 */

#ifndef APP_H__
#define APP_H__

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <libdragon.h>

#include "boot/boot.h"
#include "library/library.h"
#include "library/thumbcache.h"
#include "cheats/cheatdb.h"
#include "menu/cart_load.h"
#include "menu/path.h"
#include "menu/rom_info.h"
#include "menu/settings.h"
#include "ui/input.h"
#include "ui/theme.h"

/** One 60 Hz field in microseconds. NTSC is 59.94, so 16683; the 17 us against 16667 is below
 *  the resolution of anything measured here but keeps the bin edges from drifting over a run. */
#define FIELD_US        16683
/** How late a frame may land and still count as on time. The main loop is not phase-locked to
 *  the VI, so it overshoots the field boundary by a few hundred microseconds as a matter of
 *  course; without this every frame in the run would report as a miss. */
#define FIELD_JITTER_US 1000
/** Bins for 1, 2, 3 and 4-or-more fields. Past four the frame is bad and the exact count adds
 *  nothing; worst_us carries the magnitude. */
#define FRAMESTAT_BINS  4

typedef enum {
    SCREEN_GRID = 0,
    SCREEN_DETAIL,
    SCREEN_CHEATS,
    SCREEN_CHEATEDIT,  /**< typing a cheat in by hand; see screen_cheatedit.c */
    SCREEN_SETTINGS,
    SCREEN_PARENTAL,   /**< the parent's panel; itself behind the code once one is set */
    SCREEN_LOCKS,      /**< the library, with a padlock per title */
    SCREEN_PROFILES,   /**< who is playing; the boot picker and the roster editor */
    SCREEN_CLOCK,      /**< setting the date and time; see screen_clock.c */
    SCREEN_CODE,       /**< the button-code pad; see screen_code_ask() */
    SCREEN_CREDITS,    /**< what this program owes to other people; see screen_credits.c */
    SCREEN_SYSINFO,    /**< build, card, memory; see screen_sysinfo.c */
    SCREEN_KEYBOARD,   /**< typing a name; see screen_keyboard_ask() */
    SCREEN_APPEARANCE, /**< picking a profile's face and colour; see screen_appearance.c */
    SCREEN_LAUNCH,
    SCREEN_FAULT,
    SCREEN_COUNT,
} screen_id_t;

typedef struct app_s app_t;

/** @brief One screen. Any callback may be NULL. */
typedef struct {
    screen_id_t id;
    void (*enter)      (app_t *app);
    void (*leave)      (app_t *app);
    void (*update)     (app_t *app, float dt);
    void (*render)     (app_t *app, surface_t *fb);
    /** Runs after the frame is submitted, while the RDP drains. No drawing here. */
    void (*background) (app_t *app, uint32_t budget_ticks);
} screen_t;

struct app_s {
    boot_params_t *boot;
    const char    *storage;      /**< "sd:/" on hardware, "rom:/" under an emulator */
    flashcart_err_t flashcart_err;  /**< last flashcart result, set by cart_load */
    settings_t     settings;
    const theme_t *theme;

    input_t   input;
    float     dt;
    uint32_t  frame;
    uint32_t  starved;    /**< loop spins where the RDP had not drained -- NOT a frame count */
    uint32_t  bg_calls;   /**< background() invocations, for diagnosing starvation */
    /** Displayed frames in the last reporting window, binned by how many 60 Hz fields each took.
     *  Reset every window, so these are rates and not run totals. */
    uint32_t  fieldbin[FRAMESTAT_BINS];
    uint32_t  worst_us;   /**< longest unclamped inter-frame interval in the window */
    /** Summed CPU microseconds per window, divided by 60 when reported. render_us is display-list
     *  construction only: rdpq_detach_show() returns before the RDP has drawn anything, so the
     *  drain lands in the gap between these and the frame interval, not in render_us. */
    uint32_t  update_us, render_us, bg_us, spin_us, snd_us;
    /** Icon rasterising, bracketed separately from bg_us. See the call site in app.c: it was
     *  originally OUTSIDE every bracket, so the most expensive new work in the program was
     *  measured by nothing and `bg_us=42` on the icon picker read as "this is free". */
    uint32_t  icon_us;
    uint32_t  icons_done;   /**< icons rasterised in the window */
    time_t    now;

    library_t *lib;
    thumbcache_t *thumbs;

    /** What SCREEN_LAUNCH is about to boot. Filled by whoever navigates there; owned by the
     *  launch screen, which frees rom_path in leave(). Kept out of the library record because a
     *  rom_info_t is 200-odd bytes and only one game is ever being launched. */
    struct {
        path_t         *rom_path;
        rom_info_t      rom_info;
        int             rom_id;      /**< library index, or -1 */
        cart_load_err_t err;
    } launch;

    /** Cheats for the game in app->launch, loaded when the detail sheet opens and freed when it
     *  closes. Enabled flags live here and nowhere else -- they are deliberately not persisted
     *  yet, because writing cheatstate.dat needs hardware to test against. */
    cheatset_t cheats;

    screen_id_t screen;
    screen_id_t next_screen;

    const char *fault_message;
    bool running;
};

/** @brief Run the menu. Returns once a ROM has been staged into @p boot_params. */
void app_run (boot_params_t *boot_params);

/** @brief Request a screen change; takes effect before the next frame. */
void app_goto (app_t *app, screen_id_t screen);

/** @brief Switch to the fault screen with a message. Unrecoverable. */
void app_fault (app_t *app, const char *message);

#endif /* APP_H__ */
