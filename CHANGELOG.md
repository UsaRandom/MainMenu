# Release Notes

- For the SummerCart64, use the `sc64menu.n64` file in the root of your SD card.
- For the 64Drive, use the `menu.bin` file in the root of your SD card.
- For the ares emulator, use the `N64FlashcartMenu.n64` file.

## Release Notes 2026-Vnext

- **New Features**
	- ~~Browser now allows hiding files and folders with hidden attributes set (thanks [Xeroxxx](https://github.com/Xeroxxx)).~~ Awaiting performance enhancement.

- **Bug Fixes**

- **Documentation**
	- Minor fixes.

- **Refactor**

- **Other**
	

### Breaking changes
- (as of 2026-03-15) libdragon SDK (and this menu) now requires MI repeat mode support, (supported by latest Ares and Gopher64, A3D also works though needs the latest FW). 

### Notes
- Progress has been made towards disk swapping, but it is still WiP.

### Current known Issues
- Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
- Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is fully implemented.
- Some users have reported crashes in Zelda OOT (anti-piracy checks). Menu V0.2.0 works as expected.
- A user has reported that the menu fails to load RTYI demo 2. Workaround by not setting a background image.
- PixelFX HDMI mods may need to be updated to latest FW to support display.


### Deprecation notices
- None.

## Release Notes 2026-08-23 - Tagged 1.3.0

One theme: a library of hacks of the same cartridge used to be five copies of one name.

- **Titles**
	- **Games that share a cartridge header show their file names instead.** The 20-byte ROM title is unique for a retail dump and identical for every hack of it, so the grid could not tell them apart. After the library is assembled, a given name that appears more than once is replaced on screen by the filename -- region tags and `[!]` kept, extension dropped. A leftover unique header still shows the header.
	- **C-up on a game's page names it.** The keyboard is seeded with the display title. The name is written as `display_name` under `[menu]` in a `.ini` next to the ROM, the same sidecar that already holds boot overrides, so it travels with the file. Empty plus Done reverts. Keyed by path, not game code, or two hacks of the same cart would share one name. A read-only card refuses the write and leaves the title alone. Renaming used to qsort the library under a RAM art pool keyed on array index, so every title that slid down the list went blank until reboot.
	- **The game's page shows the file path**, at the bottom of the sheet, with the storage prefix (`rom:`, `sd:`) stripped. It appears only once the sheet has finished opening and is gone the instant close starts, so it does not ride the animation.

- **Long strings**
	- **Titles that do not fit scroll**, on the grid footer and on the sheet heading. The old no-marquee rule assumed 608 px was enough for a 20-character header; filenames are not that short. Tiles still do not marquee.
	- **The keyboard field scrolls to keep the caret on screen.** It used to clip at about 24 characters and then hide the caret, so a long name was untypable past that point. The `n / 63` counter owns a strip on the right; the field starts scrolling before a glyph can land on it.
	- **Up off the top row of keys puts the cursor in the box.** Left and Right walk an insertion point through the name; typing from the keys inserts there, so a one-letter fix is not a retype. Down lands on the key under the caret.

- **Documentation**
	- CARD.md covers C-up, collision titles, and the sidecar key.

No index-format bump. An existing `library.idx` still loads; display titles are recomputed on every assembly. Old cards keep working.

## Release Notes 2026-08-15 - Tagged 1.2.0

Two themes: the console stopped stalling the music, and what the hardware taught went into the
audit instead of into folklore.

- **Sound**
	- **Music now starts as the boot screen lifts**, on every console. Playing it through the boot was implemented three ways -- including one whose own log showed 60 ms of feeding headroom -- and real hardware audibly paused all three; the full story and the measurements are AUDIT.md 1bi.
	- **Deleting a profile no longer stops the music or freezes the screen.** The save-erase walk feeds the mixer between card operations and draws a progress card with its own frames; the confirmation dialog appears instantly because it no longer counts save files before asking (the count cost the same card walk as the deletion itself).
	- Boot-time file walks now feed the audio queue on every callback rather than at paint rate.

- **Game Boy emulator**
	- **Saves survive on clone consoles.** As FLASHRAM, save reads returned nothing on the ModRetro M64 -- a valid save booted as absent, and in-game saves vanished on relaunch. Game Boy cores now launch with banked SRAM (96 KB), which is a plain PI DMA and works on the same hardware. Offered upstream as N64FlashcartMenu#390.
	- `tools/mkgb64sav.py` converts raw cart-RAM dumps into the emulator's version-3 save container, and round-trips them back out byte-identical.

- **Recent tab**
	- **A fresh play always sorts to the top.** Play stamps are now monotonic: on a console whose real-time clock does not answer (the M64's is a clone unknown), the old fallback timestamp sank new plays to the bottom of Recent, under everything stamped when a clock existed. A dead clock now yields a play counter and the order stays exact.

- **Appearance**
	- **Footer hints are drawn as the controller's own buttons** -- three-layer pixel sprites adapted from n64-game-template (MIT, credited), with the shoulders shaped like shoulders. Batched per colour layer and bottom-aligned to the layout cell, both lessons paid for on hardware (AUDIT.md 1bj).
	- The grid footer gained breathing room: title and hint rows separated, with clear floor under the buttons.

- **Diagnostics**
	- **A System Info page** shows what the boot measured -- index result, revalidation time, audio worst-gap against buffer, memory profile -- the numbers that turned the sound work from guessing into engineering.
	- Boot banners in `launch.log` carry the same measurements.

- **Documentation**
	- The audit gained entries 1bh-1bj: the profile-screen stalls and their fixes, the three failed boot-music approaches with measurements, and the button sprites' hardware lessons.
	- Commercial titles are no longer named in the docs; test subjects are referred to by role.

## Release Notes 2026-08-11 - Tagged 1.1.0

Two themes: the cheat engine now runs most of the database it ships with, and a large card boots
without a long blank screen.

- **Cheats**
	- **Cheats are now keyed per region.** The keys were wildcarded, so every regional file for a game merged into one row and the merge kept whichever sorted first -- which meant 11,162 USA cheats across 136 games were being replaced outright by a same-named foreign one. One puzzle title lost 7,183 of them. A racing title's "Infinite Money" wrote to where the in-game currency lives in the European binary. The database goes from 392 rows and 1.75 MB to 802 rows and 3.37 MB.
	- **Conditionals assemble.** A selection carrying a line the engine could not emit was refused whole rather than half-applied, so one conditional took everything ticked alongside it down silently -- which is what one adventure title's "Infinite Magic" was doing. 42,220 of 42,898 groups are now carried, up from 40,764; that title's V1.2 goes from 317 of 337 to 334.
	- **Repeaters fit.** The 0x50 repeater was unrolled at three instructions per iteration, up to 762 words for one cheat, against boot segments with 15 to 45 words of usable padding on half the shelf. It is a loop now: twelve words whatever the count.
	- **The exception preamble is identified by what it points at**, rather than by assuming __osException sits sixteen bytes on. That assumption was wrong three times in fifteen: two titles link it +212 away and were refused outright, and a third has a dispatcher stub at exactly +16 so its cheats were written into code that is never copied to 0x80000180. Thirteen of fifteen games on the shelf can hook, up from ten.
	- Boot-time writes and the osMemSize code are handled; GS-button variants remain refused, because there is no button to read.

- **Performance**
	- The menu no longer looks for a metadata pack that is not on the card. `menu/metadata/<G>/<A>/<M>/<E>/metadata.ini` was opened once per title, and on a card without an art pack every one of those missed; the answer is now looked up once at boot.
	- Loose box art is found by binary search rather than by walking the whole table, and the table no longer compares every entry against every other while it is built. On 278 covers that is about 116,000 string compares and 556 allocations on the boot path reduced to about 6,700 and none.

- **New Features**
	- The boot plate is held up while the card is scanned, with the title count climbing as titles are found, instead of the screen staying blank until the scan finished. The plate's animation runs during the scan rather than after it, so the curtain lifts as soon as the grid is ready.

- **Bug Fixes**
	- Audio no longer runs dry during a long scan. Nothing fed the mixer while the scan blocked, and an N64 whose audio interface is not given a new buffer repeats the last one it had.

- **Documentation**
	- `docs/AUDIT.md` 1aw records what the scan costs on a card and why the emulator could not measure it. The previous 11,499 us/rom figure is marked superseded as a predictor rather than replaced.
	- Corrected a claim in `library.c` that duplicate art resolution favours the shallowest copy. It follows scan order, which is not the same thing.

- **Other**
	- `tools/cheatshelf.py` audits the shelf one line per ROM; `tools/cheatlook.py` answers the narrower question about a single game.
	- New host test covering the loose-art table, 156 checks, with seven mutations proving it can fail.
	- Two of the existing mutation tests had not been mutating anything -- one targeted a rewritten line, the other was anchored with `$` against a tree checked out CRLF. `run.sh` now refuses to report that as a survival.

### Breaking changes
- **Use the `cheats.db` shipped with this release.** Cheats are keyed per ROM revision and now per region; an older database will apply another region's or revision's addresses.

### Notes
- **A flat folder of several hundred ROMs is slow, and no amount of this fixes that.** The cost is titles multiplied by entries in the directory, because the filesystem resolves every path by walking the directory linearly. Splitting a large library into subfolders is worth more than anything else in this release: one folder per initial letter took a 556-entry directory to about 21 on the card this was measured against.

### Current known Issues
- Two titles on the shelf still cannot be hooked. One's only preamble names an address 0x10000000 below its own handler and is unexplained, so nothing is touched; the other has no preamble anywhere in twelve megabytes.
- Three more per-title file probes (`<rom>.ini`, `<rom>.meta`, `<rom>.metadata.ini`) still miss on every title on a typical card. Removing them requires changing the order the scan walks directories in, which is what decides duplicate art resolution, so it has been left for a change of its own.
- On consoles where the menu cannot write to the card, `library.idx` and the thumbnail atlas never persist, so the full scan and every cover decode happen on every boot. That is the larger remaining cost and is not addressed here.

## Release Notes 2026-05-23 - Tagged 0.3.2

- **New Features**
	- Adds settings to hide cheat and save file types in the browser.
	- Adds ability to display embedded homebrew ROM metadata in ROM info.
	- Adds ability to display Commercial game metadata using ROM DB.
	- Menu settings now know what setting is currently applied.

- **Bug Fixes**
	- Neon64 1Mbit SRAM.
	- Potential buffer overflows.
	- Fixed an issue where large ROMs failed to load in certain circumstances.
	- Fixed a lockup when selecting a game in history when the ROM no longer exists.

- **Documentation**
	- Minor fixes.

- **Refactor**
	- PAL60 (using new libdragon support).
	- ROM view, Age ratings and other metadata now align and support homebrew metadata standard.
	- Menu credits.
	- Disk Drive, disk info view.
	- CPak manager, Added menu option to for notes restore.
	- Replace mini.c INI lib with custom implementation.
	- Browser highlight colour for better display on CRT.

- **Other**
	- Updated libDragon {preview} SDK.
	- Updated miniz lib.
	- Updated minimp3.
	- Add docfx devcontainer.
	- Remove rolling prerelease (all releases to main should be tagged).
	- Added AI instructions to repo.
	- Added an extra build option (run-debug-reboot) that aids debugging remotely without the need for uploading files to the SD card.
	

### Breaking changes
- (as of 2026-03-15) libdragon SDK (and this menu) now requires MI repeat mode support, (supported by latest Ares and Gopher64, A3D also works though needs the latest FW). 

### Notes
- (as of 2026-03-01) libdragon {preview} SDK now compiles ROMs that use EEPROM to conform with OG wait timings by default.
- Progress has been made towards disk swapping, but it is still WiP.

### Current known Issues
- Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
- Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is fully implemented.
- Some users have reported crashes in Zelda OOT (anti-piracy checks). Menu V0.2.0 works as expected.
- A user has reported that the menu fails to load RTYI demo 2. Workaround by not setting a background image.
- PixelFX HDMI mods may need to be updated to latest FW to support display.


### Deprecation notices
- None.


## Release Notes 2025-12-04 - Tagged 0.3.1

- **New Features**
	- Settings contexts now preset to the saved option.
	- Added latest Viewpoint64 final proto ROM to database.
	- Added Rumble PAK and Transfer PAK features to ROM info screen.

- **Bug Fixes**
	- Fixed MP3 Player crashes menu if the MP3 file's sample rate is less than 44100 hz and menu SFX are enabled.
	- Fixed game_code_path size that caused crash when loading homebrew boxart.
	- Fixed boot process which could lead to blank screens or crashes.
	- Fixed a potential issue that could happen when a RTC was not detected.


- **Documentation**
	- Moved ED64 documentation to [98_flashcart_wip.md](./docs/attic/upstream/98_flashcart_wip.md)
	- Other minor fixes.

- **Refactor**
	- Output 4MB files as MB, rather than kB.
	- Improved icons for direction.
	- Controller Pak now selects notes using up/down rather than left/right.

- **Other**
	- Updated libDragon SDK.
	- Updated docker container to Trixy

### Breaking changes
- None.


### Current known Issues
- Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
- Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is implemented.
- Some users have reported crashes in Zelda OOT (anti piracy checks). Menu V0.2.0 works as expected.
- A user has reported that the menu crashes with a CPU exception. Menu V0.2.0 works as expected.


### Deprecation notices
- None.


## Release Notes 2025-11-15 - Tagged 0.3.0

- **New Features**
	- Added ability to hide save folders (on by default).
	- Added ability to reset the menu setting to default from the menu UI.
	- Updated the UI font to Firple-Bold which supports more characters.
	- Shows info message within the loading progress bar.
	- Add the ability to display ESRB age ratings (see [documentation](./docs/attic/upstream/65_experimental.md)).
	- Add Beta Datel code GUI (see [documentation](./docs/attic/upstream/13_datel_cheats.md)).
	- Add ability to load boxart from ROMs that use the homebrew header (see [documentation](./docs/attic/upstream/19_gamepak_boxart.md)).
	- Add ability to extract files from ZIP archives (thanks [VicesOfTheMind](https://github.com/VicesOfTheMind)).
	- Add Alpha FEATURE_PATCHER_GUI_ENABLED (build flag to enable it).
	- Add Controller Pak manager (thanks [LuEnCam](https://github.com/LuEnCam))
	- Add Game art image switching (thanks [dpranker](https://github.com/dpranker))

- **Bug Fixes**
	- Fix ability to set the RTC via menu (Hotfixed in last release).
	- Fix Game ID (used by PixelFX HDMI mods) sent over Joybus is not working (Hotfixed in last release).
	- Fix GB / GBC emulator not saving in certain circumstances (Hotfixed in last release).
	- Fix issue with emulation of cold boot, as otherwise the FPU might start in an unexpected state.
	- Fix missing enum case for 1 Mbit SRAM saves (Hotfixed in last release).

- **Documentation**
	- Improved Emulator information for known working NES emulator version.
	- Updated experimental features to reflect feature change.
	- Added sounds documentation.
	- Updated autoload to reflect feature change.

- **Refactor**
	- Improve tab navigation by using any left/right control input and add cursor SFX.
	- Add ability for font style to be used in ui_components_main_text_draw and ui_components_actions_bar_text_draw.

- **Other**
	- Updated libDragon SDK.
	- Updated miniz library.
	- Updated Github templates.

### Breaking changes
* Deprecated "Autoload ROM" function was removed from menu (use `FEATURE_AUTOLOAD_ROM_ENABLED` as a build flag to re-enable it).
* Deprecated Boxart image handler was removed (see [documentation](./docs/attic/upstream/19_gamepak_boxart.md) for new boxart link).
* ROM's that used custom CIC, TV and/or Save type set from the menu will need to re-set them, now uses "custom_boot" header within the ini file.


### Current known Issues
* Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
* Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is implemented.
* MP3 Player crashes menu if the MP3 file's sample rate is less than 44100 hz and menu SFX are enabled.
- Some users have reported crashes in Zelda OOT (anti piracy checks). Menu V0.2.0 works as expected.
- A user has reported that the menu crashes with a CPU exception. Menu V0.2.0 works as expected.


### Deprecation notices
* Boxart directory has changed to metadata directory.


## Release Notes 2025-03-31 - Tagged 0.2.0

- **New Features**
	- Introduced tabs in main menu for ROM favorites and recently played ROM history.
	- Introduced first run check to ensure users are aware of latest changes.
	- Introduced ability to turn off GUI loading bar.
	- BETA_FEATURE: Introduces ROM descriptions from files.
	- BETA_FEATURE: Enabled setting for fast ROM reboots on the SC64.
	- Add macOS metadata to hidden files.
	- Added settings schema version for future change versioning.
	- Added setting for PAL60 compatibility mode (see breaking changes).
	- BETA_FEATURE: Added setting for line doublers that need progressive output, enable using "force_progressive_scan" setting in `config.ini`.


- **Bug Fixes**
	- Menu sound FX issues (hissing, popping and white noise).
	- RTC not showing or setting correct date parameters in certain circumstances.
	- ~~GB / GBC emulator not saving in certain circumstances.~~


- **Documentation**
	- Re-orginised and improved user documentation.
	- Added a lot of doxygen compatible code comments.
	- Added project license.


- **Refactor**
	- RTC subsystem (align with libDragon improvements).
	- Boxart images (Deprecates old boxart image folder layout).
	- Settings (PAL60 compatibility, schema version, fast reboot, first run, progress bar).

- **Other**
	- Updated libDragon SDK.
	- Updated miniz library.

### Breaking changes
* ~~GB /GBC emulator changed save type to SRAM (from FRAM) to improve compatibility with Summercart64 (which only uses H/W compatible FRAM), this may break your ability to load existing saves.~~
* For similar PAL60 functionality, you may need to also enable the new "pal60_compatibility_mode" setting in `config.ini`.


### Current known Issues
* The RTC UI requires improvement (awaiting UI developer).
* Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
* Fast Rebooting a 64DD disk once will result in a blank screen. Twice will return to menu. This is expected until disk swapping is implemented.
* MP3 Player crashes menu if the MP3 file's sample rate is less than 44100 hz.


### Deprecation notices
* Autoload ROM's will be deprecated in favor of Fast Reboot in a future menu version.
* Old boxart images using filenames for game ID is deprecated and the compatibility mode will be removed in a future release.


## Release Notes 2025-01-10

- **Bug Fixes**
	- Fixed menu display (PAL60) by reverted libdragon to a known working point and re-applying old hacks.

### Current known Issues
* The RTC UI requires improvement (awaiting UI developer).
* Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
[Pre-release menu]:
* BETA_SETTING: PAL60 when using HDMI mods has regressed (awaiting libdragon fix).
* ALPHA_FEATURE: ED64 X Series detection does not occur properly (however this is not a problem as not tag released asset).
* ALPHA_FEATURE: ED64 V Series only supports loading ROMs (however this is not a problem as not tag released asset).


## Release Notes 2024-12-30

- **New Features**
	- Introduced menu sound effects for enhanced user experience (the default is off).
	- Added N64 ROM autoload functionality, allowing users to set a specific ROM to load automatically.
	- Added menu boot hotkey (hold `start` to return to menu when autoload is enabled).
	- Added context menu and settings management options GUI for managing various settings in `config.ini`.
	- Added functionality for editing the real-time clock (RTC) within the RTC menu view.
	- Improved flashcart info view for showing supported flashcart features and version.
	- Enhanced UI components with new drawing functions and improved organization.
	- Added emulator support for `SMS`, `GG`, and `CHF` ROMs.
	- Enhanced joypad input handling for menu actions, improving responsiveness.
	- Optimized boxart image loading from filesystem.
	- Improved various text to make the functionality more clear.

- **Bug Fixes**
	- Improved error handling in multiple areas, particularly in save loading and ROM management.
	- Enhanced memory management to prevent potential leaks during error conditions.
	- Fixed text flickering in certain circumstances.

- **Documentation**
	- Updated README and various documentation files to reflect new features and usage instructions.
	- Added detailed setup instructions for SD cards and menu customization.
	- Enhanced clarity in documentation for RTC settings and menu customization.
	- Improved organization and clarity of SD card setup instructions for various flashcarts.

- **Refactor**
	- Standardized naming conventions across UI components for better organization.
	- Restructured sound management and input handling for improved responsiveness.
	- Streamlined the loading state management for ROMs and disks within the menu system.
	- Improved clarity and usability of the developer guide and other documentation files.

### Current known Issues
* BETA_SETTING: PAL60 when using HDMI mods has regressed (awaiting libdragon fix).
* The RTC UI requires improvement (awaiting UI developer).
* Menu sound FX may not work properly when a 64 Disk Drive is also attached (work around: turn sound FX off).
* ALPHA_FEATURE: ED64 X Series detection does not occur properly (however this is not a problem as not tag released asset).
* ALPHA_FEATURE: ED64 V Series only supports loading ROMs (however this is not a problem as not tag released asset).

### Breaking changes
* Disk drive expansion ROMs are now loaded with `Z|L` instead of `R` to align with ROM info context menu (and future functionality).
