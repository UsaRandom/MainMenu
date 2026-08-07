# MainMenu

A box-art game launcher for the N64, run from a [SummerCart64](https://github.com/Polprzewodnikowy/SummerCart64) flashcart.

![The grid](docs/images/demo-grid-scrolled.png)

*Every game, every cover and every cheat in that picture is invented —
[`tools/mkdemo.py`](tools/mkdemo.py) draws original art for titles that do not exist. Nothing is
written on the covers; the menu is what puts a name under one.*

## What it does

- **Presents games, not files.** Point it at an SD card and it finds the ROMs, reads their
  headers, matches them against a 450-game database and shows you a grid of covers. There is
  nothing to prepare and no list to hand-write.
- **Finds the covers itself**, PNG or JPEG, at whatever size and aspect the scan happens to be,
  and caches the decoded art on the card so the next boot is warm.
- **Fits the grid to the covers.** A tile is the shape of the art in it, and a tab of wide covers
  lays out four across (above) where a tab of tall ones lays out five. Nothing is cropped to fit.
- **Tabs per system** — N64, NES, SNES, GB, GBC, SMS — plus Recent, Most Played and Favourites.
  Empty tabs do not appear.
- **Remembers what you played** and for how long, and offers it back at the top.
- **Runs other systems** through emulator cores kept on the card.
- **Up to ten players share one card.** Each picks a name and a face out of 3,894 icons in two
  colours of their own, and keeps their own saves, favourites, play history, cheat selections and
  theme.
- **Cheats as named things.** A cheat database is prepared on a PC and read on the console; the
  detail sheet says how many are switched on, and there is a hex editor for the codes behind it.
- **Parental controls** — a code, playable hours, and per-game locks.
- **Themes, music and sound.** 28 CC0 MIDI tracks, synthesised on the console rather than
  streamed.
- **Credits screen** carrying every licence the cartridge owes, generated from the same file this
  repository publishes.

> Every measurement in this repository was taken under [ares](https://ares-emu.net/), an emulator.
> Nothing here has been timed on a real console. The open questions are in
> [AUDIT.md](docs/AUDIT.md); [HARDWARE.md](docs/HARDWARE.md) is the order to bring one up in.

**Setting up a card:** [CARD.md](docs/CARD.md). **Building it:** [BUILDING.md](docs/BUILDING.md).
Also [CREDITS.md](docs/CREDITS.md), [DESIGN.md](docs/DESIGN.md), [AUDIT.md](docs/AUDIT.md). The
numbered guides under [`docs/`](docs/) are inherited from upstream and describe upstream, some of
it removed here.

---

# Licence

Copyright © 2026 UsaRandom, and the N64FlashcartMenu contributors this is forked from. Released
under the [GNU Affero General Public License](LICENSE.md), version 3 or later, inherited from
upstream. There is no warranty; see sections 15 and 16.

Substantially the work of the N64FlashcartMenu authors and
[contributors](https://github.com/Polprzewodnikowy/N64FlashcartMenu/graphs/contributors); this
fork replaces the presentation layer and takes responsibility for its own bugs.

* [Mateusz Faderewski / Polprzewodnikowy](https://github.com/Polprzewodnikowy)
* [Robin Jones / NetworkFusion](https://github.com/networkfusion)

Not affiliated with or endorsed by any console or cartridge maker, or by the upstream project. The
GitHub Actions workflows are upstream's and have not been adapted — the build artifact is still
labelled for a flashcart this fork no longer supports.

## Libraries
* [libdragon](https://github.com/DragonMinded/libdragon/tree/preview) - [UNLICENSE License](https://github.com/DragonMinded/libdragon/blob/preview/LICENSE.md)
* [libspng](https://github.com/randy408/libspng) - [BSD 2-Clause License](https://github.com/randy408/libspng/blob/master/LICENSE)
* [miniz](https://github.com/richgel999/miniz) - [MIT License](https://github.com/richgel999/miniz/blob/master/LICENSE)
* [midi64](src/libs/midi64) - [MIT License](src/libs/midi64/LICENSE) — vendored, not a submodule
* [svg64](src/libs/svg64) - [MIT License](src/libs/svg64/LICENSE) — vendored, not a submodule
* [picojpeg](src/libs/picojpeg) - public domain, by Rich Geldreich

Two more are linked in from inside libdragon rather than chosen here, and are credited for that
reason:

* [FatFs](http://elm-chan.org/fsw/ff/) by *ChaN* - [its own licence](libdragon/src/fatfs/License.md), the filesystem that reads the card
* [libcart](https://github.com/devwizard64/libcart) by *devwizard64* - the flashcart driver. The copy inside libdragon states no licence terms.

## Icons
The 3,894 icons players choose from are from [game-icons.net](https://game-icons.net), under
[CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) (a few authors CC0). That is an
attribution licence and the cartridge is what redistributes, so all 36 authors are named in
[CREDITS.md](docs/CREDITS.md) and on the menu's own credits screen. They ride in the cartridge as
SVG text and are turned into pixels when one is needed, which is why there can be thousands of
them and why they follow whatever colours a player picks. The artwork is never committed here.

## Data
The database that recognises a cartridge and knows how it saves is derived from
[ares](https://ares-emu.net)' own, ISC licensed, copyright © 2004-2025 ares team, Near et al. The
cheat corpus, where a build ships one, is [libretro-database](https://github.com/libretro/libretro-database), MIT.

## Sounds
See [License](https://pixabay.com/en/service/license-summary/) for the following sounds:
* [Cursor sound](https://pixabay.com/en/sound-effects/click-buttons-ui-menu-sounds-effects-button-7-203601/) by Skyscraper_seven (Free to use)
* [Actions (Enter, Back) sound](https://pixabay.com/en/sound-effects/menu-button-user-interface-pack-190041/) by Liecio (Free to use)
* [Error sound](https://pixabay.com/en/sound-effects/error-call-to-attention-129258/) by Universfield (Free to use)

## Music
The 28 songs in [`assets/music/`](assets/music/) are CC0.

## Emulators
* [neon64v2](https://github.com/hcs64/neon64v2) by *hcs64* - [ISC License](https://github.com/hcs64/neon64v2/blob/master/LICENSE.txt)
* [sodium64](https://github.com/Hydr8gon/sodium64) by *Hydr8gon* - [GPL-3.0 License](https://github.com/Hydr8gon/sodium64/blob/master/LICENSE)
* [gb64](https://github.com/lambertjamesd/gb64) by *lambertjamesd* - [MIT License](https://github.com/lambertjamesd/gb64/blob/master/LICENSE)
* [smsPlus64](https://github.com/fhoedemakers/smsplus64) by *fhoedmakers* - [GPL-3.0 License](https://github.com/fhoedemakers/smsplus64/blob/main/LICENSE)
* [Press-F-Ultra](https://github.com/celerizer/Press-F-Ultra) by *celerizer* - [MIT License](https://github.com/celerizer/Press-F-Ultra/blob/master/LICENSE)

## Fonts
* [Firple](https://github.com/negset/Firple) by *negset* - (SIL Open Font License 1.1)
