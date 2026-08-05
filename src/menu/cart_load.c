/**
 * @file cart_load.c
 * @brief Cart loading functions
 * @ingroup menu
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>
#include "app.h"
#include "cart_load.h"
#include "path.h"
#include "paths.h"
#include "profile.h"
#include "utils/fs.h"
#include "utils/utils.h"

/** @brief The folder cores are looked for in, under each of the roots menu/paths.h probes.
 *
 * DDIPL_LOCATION used to sit beside this, pointing at a 64DD IPL that nothing in this fork has
 * ever loaded -- the 64DD path went with the file browser. */
#define EMU_SUBDIR              "emulators"

/**
 * @brief The SNES core, and the one it replaced.
 *
 * lithium64 is our own fork of sodium64, targeting this console specifically: sodium64 sizes its
 * ROM cache to fill exactly 4 MB of RDRAM and programs the VI, AI, PI and SI with values a stock
 * N64 happens to accept, both of which are assumptions an FPGA reimplementation is entitled to
 * break. On an M64 the fork is the one that has been tested; upstream is the fallback, so a card
 * carrying only sodium64.z64 still plays SNES titles rather than reporting a missing emulator.
 *
 * The two are interchangeable at this seam by construction and it is worth saying why, because
 * nothing here would catch it if they stopped being: both declare `sram256k`, matching the
 * FLASHCART_SAVE_TYPE_SRAM_256KBIT below, and lithium64 reads the emulated ROM from PI address
 * 0x10200000 (memory.S:225), which is the 0x200000 emulated_rom_offset below.
 */
#define SNES_CORE               "lithium64.z64"
#define SNES_CORE_FALLBACK      "sodium64.z64"

/**
 * @brief Create the saves subdirectory, and the profile's folder inside it.
 *
 * Profile 1 gets `<romdir>/saves/` and nothing else, which is exactly where every card written
 * before profiles existed already keeps its saves. Profiles 2..10 get `<romdir>/saves/pN/`.
 *
 * Nested rather than a sibling `pNsaves/` for two reasons worth stating here, because this is the
 * function that decides it. A sibling scheme has no unsuffixed case, so upgrading a card would
 * renumber profile 1's folder and every existing save on it would stop being found. And
 * `library.c`'s SCAN_SKIP excludes `saves` and everything beneath it, so nesting costs the
 * scanner nothing, while `p2saves` would be walked as though it held games.
 *
 * @param path Pointer to the path structure.
 * @return true if an error occurred, false otherwise.
 */
static bool create_saves_subdirectory (path_t *path) {
    path_t *save_folder_path = path_clone(path);
    path_pop(save_folder_path);
    path_push(save_folder_path, SAVE_DIRECTORY_NAME);

    bool error = directory_create(path_get(save_folder_path));

    const char *sub = profile_save_subdir();
    if (!error && sub != NULL) {
        path_push(save_folder_path, (char *)sub);
        error = directory_create(path_get(save_folder_path));
    }

    path_free(save_folder_path);
    return error;
}

/**
 * @brief Move @p path, which names a .sav beside its ROM, into the active profile's save folder.
 *
 * Paired with create_saves_subdirectory() and always called after it: one decides where the
 * folder is, the other has to agree, and the two used to be a single line each at two call sites.
 */
static void push_save_subdir (path_t *path) {
    path_push_subdir(path, SAVE_DIRECTORY_NAME);

    const char *sub = profile_save_subdir();
    if (sub != NULL) {
        path_push_subdir(path, (char *)sub);
    }
}

/**
 * @brief Convert the ROM save type to the flashcart save type.
 * 
 * @param save_type The ROM save type.
 * @return flashcart_save_type_t The flashcart save type.
 */
static flashcart_save_type_t convert_save_type (rom_save_type_t save_type) {
    switch (save_type) {
        case SAVE_TYPE_EEPROM_4KBIT: return FLASHCART_SAVE_TYPE_EEPROM_4KBIT;
        case SAVE_TYPE_EEPROM_16KBIT: return FLASHCART_SAVE_TYPE_EEPROM_16KBIT;
        case SAVE_TYPE_SRAM_256KBIT: return FLASHCART_SAVE_TYPE_SRAM_256KBIT;
        case SAVE_TYPE_SRAM_BANKED: return FLASHCART_SAVE_TYPE_SRAM_BANKED;
        case SAVE_TYPE_SRAM_1MBIT: return FLASHCART_SAVE_TYPE_SRAM_1MBIT;
        case SAVE_TYPE_FLASHRAM_1MBIT: return FLASHCART_SAVE_TYPE_FLASHRAM_1MBIT;
        case SAVE_TYPE_FLASHRAM_PKST2: return FLASHCART_SAVE_TYPE_FLASHRAM_PKST2;
        default: return FLASHCART_SAVE_TYPE_NONE;
    }
}

/**
 * @brief Convert the cart load error code to a human-readable message.
 * 
 * @param err The cart load error code.
 * @return char* The error message.
 */
char *cart_load_convert_error_message (cart_load_err_t err) {
    switch (err) {
        case CART_LOAD_OK: return "Cart load OK";
        case CART_LOAD_ERR_ROM_LOAD_FAIL: return "Error occured during ROM loading";
        case CART_LOAD_ERR_SAVE_LOAD_FAIL: return "Error occured during save loading";
        case CART_LOAD_ERR_64DD_PRESENT: return "64DD accessory is connected to the N64";
        case CART_LOAD_ERR_64DD_IPL_NOT_FOUND: return "Required 64DD IPL file was not found";
        case CART_LOAD_ERR_64DD_IPL_LOAD_FAIL: return "Error occurred during 64DD IPL loading";
        case CART_LOAD_ERR_64DD_DISK_LOAD_FAIL: return "Error occurred during 64DD disk loading";
        case CART_LOAD_ERR_EMU_NOT_FOUND: return "Required emulator file was not found";
        case CART_LOAD_ERR_EMU_LOAD_FAIL: return "Error occurred during emulator ROM loading";
        case CART_LOAD_ERR_EMU_ROM_LOAD_FAIL: return "Error occurred during emulated ROM loading";
        case CART_LOAD_ERR_CREATE_SAVES_SUBDIR_FAIL: return "Couldn't create saves subdirectory";
        case CART_LOAD_ERR_EXP_PAK_NOT_FOUND: return "Mandatory Expansion Pak accessory was not found";
        default: return "Unknown error [CART_LOAD]";
    }
}

/**
 * @brief Load an N64 ROM and its save file.
 *
 * Takes app_t and reads what it needs out of app->launch, rather than reaching into a browser
 * cursor. That is the whole coupling this file had to the old UI: it wanted "the entry the file
 * list is sitting on", which does not exist in a grid of games.
 *
 * Nothing here calls flashcart_set_next_boot_mode(). The cart stays pointed at the menu, so the
 * console's Reset button always comes back here. The setting that changed this claimed to be
 * "fast reboot back to the menu" and did the exact opposite -- it set BOOT_MODE_ROM, which makes
 * Reset re-run the game and leaves no way back to the menu short of a power cycle.
 */
cart_load_err_t cart_load_n64_rom_and_save (app_t *app, flashcart_progress_callback_t progress) {
    path_t *path = path_clone(app->launch.rom_path);

    bool byte_swap = (app->launch.rom_info.endianness == ENDIANNESS_BYTE_SWAP);
    flashcart_save_type_t save_type = convert_save_type(rom_info_get_save_type(&app->launch.rom_info));

    app->flashcart_err = flashcart_load_rom(path_get(path), byte_swap, progress);
    if (app->flashcart_err != FLASHCART_OK) {
        path_free(path);
        return CART_LOAD_ERR_ROM_LOAD_FAIL;
    }

    /* Saves go in a saves/ folder beside the ROM, always -- the setting that used to switch this
     * off is gone and this was its default, so no existing card changes behaviour. Beside the ROM
     * rather than one folder for the whole card, because the save is named after the ROM file and
     * a single shared folder would hand two differently-filed copies of the same game one .sav
     * between them. */
    path_ext_replace(path, "sav");
    if ((save_type != FLASHCART_SAVE_TYPE_NONE) && create_saves_subdirectory(path)) {
        path_free(path);
        return CART_LOAD_ERR_CREATE_SAVES_SUBDIR_FAIL;
    }
    push_save_subdir(path);

    app->flashcart_err = flashcart_load_save(path_get(path), save_type);
    if (app->flashcart_err != FLASHCART_OK) {
        path_free(path);
        return CART_LOAD_ERR_SAVE_LOAD_FAIL;
    }

    path_free(path);

    return CART_LOAD_OK;
}

/**
 * @brief Load an emulator and its ROM.
 * 
 * @param app Pointer to the application state.
 * @param emu_type The type of emulator to load.
 * @param progress Progress callback function.
 * @return cart_load_err_t Error code.
 */
cart_load_err_t cart_load_emulator (app_t *app, cart_load_emu_type_t emu_type, flashcart_progress_callback_t progress) {
    const char *core = NULL;
    flashcart_save_type_t save_type = FLASHCART_SAVE_TYPE_NONE;
    uint32_t emulated_rom_offset = 0x200000;
    uint32_t emulated_file_offset = 0;

    switch (emu_type) {
        case CART_LOAD_EMU_TYPE_NES:
            core = "neon64bu.rom";
             // Tested against Neon 64 v1.2, v0.3 and v2
            save_type = FLASHCART_SAVE_TYPE_SRAM_1MBIT;
            break;
        case CART_LOAD_EMU_TYPE_SNES:
            core = SNES_CORE;
            save_type = FLASHCART_SAVE_TYPE_SRAM_256KBIT;
            break;
        case CART_LOAD_EMU_TYPE_GAMEBOY:
            core = "gb.v64";
            // TODO: Saves might be less problematic by using the FAKE type.
            save_type = FLASHCART_SAVE_TYPE_FLASHRAM_1MBIT; //FLASHCART_SAVE_TYPE_FLASHRAM_FAKE;
            break;
        case CART_LOAD_EMU_TYPE_GAMEBOY_COLOR:
            core = "gbc.v64";
            // TODO: Saves might be less problematic by using the FAKE type.
            save_type = FLASHCART_SAVE_TYPE_FLASHRAM_1MBIT; //FLASHCART_SAVE_TYPE_FLASHRAM_FAKE;
            break;
        case CART_LOAD_EMU_TYPE_SEGA_GENERIC_8BIT:
            core = "smsPlus64.z64";
            save_type = FLASHCART_SAVE_TYPE_NONE;
            break;
        case CART_LOAD_EMU_TYPE_FAIRCHILD_CHANNELF:
            core = "Press-F.z64";
            save_type = FLASHCART_SAVE_TYPE_NONE;
            break;
    }

    /* Probed across the three roots rather than fixed at one, so a card that keeps its cores in
     * /emulators works without being reorganised first. See menu/paths.h. */
    char leaf[80];
    char core_path[300];
    snprintf(leaf, sizeof(leaf), EMU_SUBDIR "/%s", core);
    bool found = menu_find_file(core_path, sizeof(core_path), app->storage, leaf);

    if (!found && emu_type == CART_LOAD_EMU_TYPE_SNES) {
        snprintf(leaf, sizeof(leaf), EMU_SUBDIR "/%s", SNES_CORE_FALLBACK);
        found = menu_find_file(core_path, sizeof(core_path), app->storage, leaf);
    }

    if (!found) {
        return CART_LOAD_ERR_EMU_NOT_FOUND;
    }

    /* Which core was actually chosen, said out loud. Every decision here -- the system-to-core
     * mapping, whether the sodium64 fallback was taken, which of the three roots it came from, and
     * below whether a copier header was stripped -- becomes invisible the moment the console
     * reboots into the core, and on hardware there is no framebuffer left to inspect. */
    debugf("emu: type=%d core=%s\n", (int)emu_type, core_path);

    app->flashcart_err = flashcart_load_rom(core_path, false, progress);
    if (app->flashcart_err != FLASHCART_OK) {
        return CART_LOAD_ERR_EMU_LOAD_FAIL;
    }

    /* The emulated ROM itself. app->launch.rom_path already holds it, where upstream rebuilt it
     * from the browser's directory plus the highlighted entry's name. */
    path_t *path = path_clone(app->launch.rom_path);

    switch (emu_type) {
        case CART_LOAD_EMU_TYPE_SNES:
            // NOTE: The emulator expects the header to be removed from the ROM being uploaded.
            emulated_file_offset = ((file_get_size(path_get(path)) & 0x3FF) == 0x200) ? 0x200 : 0;
            break;
        default:
            break;
    }

    debugf("emu: rom=%s dst=0x%08lX skip=0x%lX\n", path_get(path),
           (unsigned long)emulated_rom_offset, (unsigned long)emulated_file_offset);

    app->flashcart_err = flashcart_load_file(path_get(path), emulated_rom_offset, emulated_file_offset);
    if (app->flashcart_err != FLASHCART_OK) {
        path_free(path);
        return CART_LOAD_ERR_EMU_ROM_LOAD_FAIL;
    }

    path_ext_replace(path, "sav");
    if ((save_type != FLASHCART_SAVE_TYPE_NONE) && create_saves_subdirectory(path)) {
        path_free(path);
        return CART_LOAD_ERR_CREATE_SAVES_SUBDIR_FAIL;
    }
    push_save_subdir(path);

    app->flashcart_err = flashcart_load_save(path_get(path), save_type);
    if (app->flashcart_err != FLASHCART_OK) {
        path_free(path);
        return CART_LOAD_ERR_SAVE_LOAD_FAIL;
    }

    path_free(path);

    return CART_LOAD_OK;
}
