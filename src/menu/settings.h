/**
 * @file settings.h
 * @brief Menu Settings
 * @ingroup menu 
 */

#ifndef SETTINGS_H__
#define SETTINGS_H__


/** @brief Settings Structure */
typedef struct {
    /** @brief Settings version */
    int schema_revision;

    /** @brief First run of the menu */
    bool first_run;

    /** @brief Use 60 Hz refresh rate on a PAL console */
    bool pal60_enabled;

    /** @brief Direct the VI to force progressive scan output at 240p. Meant for TVs and other devices which struggle to display interlaced video. */
    bool force_progressive_scan;

    /** @brief Show files/directories that are filtered in the browser */
    bool show_protected_entries;

    /** @brief Default directory to navigate to when menu loads */
    char *default_directory;

    /** @brief Show saves folder in file browser */
    bool show_saves_folder;

    /** @brief Show save files in file browser */
    bool show_save_files;

    /** @brief Show cheat files in file browser */
    bool show_cheat_files;

    /** @brief Show rom configuration files in file browser */
    bool show_rom_configuration_files;

    /** @brief Show rom file extensions in browser */    
    bool show_browser_file_extensions;

    /** @brief Show rom tags in browser */  
    bool show_browser_rom_tags;

    /* Two booleans became two levels. `soundfx_enabled` and `bgm_enabled` are still read on load
     * so that an existing config.ini keeps its owner's answer, but nothing writes them again --
     * see settings.c. bgm_enabled had never done anything at all: it was saved, loaded, and read
     * by no other line in the program. */

    /** @brief Background music volume, 0 (off) to MUSIC_VOLUME_MAX */
    int music_volume;

    /** @brief Which track to play, or MUSIC_TRACK_ALL for the whole set in order */
    int music_track;

    /** @brief Sound effect volume, 0 (off) to SOUND_SFX_VOLUME_MAX */
    int sfx_volume;

    /** @brief Enable rumble feedback within the menu */
    bool rumble_enabled;

    /** @brief Where tile shapes come from: "Automatic" reads each cover's own aspect, "NTSC" is
     *         the built-in table, anything else names a section of menu/boxart.ini. A name rather
     *         than an index, because the sections a card defines can change between boots and an
     *         index would then silently select a different one. */
    char *boxart_region;

    /* The parental code, its failure count and its schedule used to be four fields here. They
     * moved to /mainmenu/parental.ini so that forgetting the code is recovered by deleting one
     * file, without losing everything else a parent has set. See menu/parental.h. */

#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    /** @brief Enable the ability to bypass the menu and instantly load a ROM on power and reset button */
    bool rom_autoload_enabled;

    /** @brief A path to the autoloaded ROM */
    char *rom_autoload_path;

    /** @brief A filename of the autoloaded ROM */
    char *rom_autoload_filename;
    
    /** @brief Show progress bar when loading a ROM */
    bool loading_progress_bar_enabled;
#endif

} settings_t;


/** @brief Init settings path */
void settings_init (char *path);
/** @brief The settings to load */
void settings_load (settings_t *settings);
/** @brief The settings to save */
void settings_save (settings_t *settings);
/** @brief Reset settings to defaults */
void settings_reset_to_defaults();

#endif
