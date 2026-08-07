# MainMenu

A box-art game launcher for the N64, run from a [SummerCart64](https://github.com/Polprzewodnikowy/SummerCart64) flashcart.

Put your games on an SD card in whatever arrangement suits you, and the menu presents them as
**games rather than files**: a grid of covers you scroll through, that remembers what you played
and how long for. There is nothing to prepare in advance and no list to hand-write.

> [!IMPORTANT]
> **This has never run on a real console, and nothing has ever been written to a real SD card.**
> Everything here was developed and measured under [ares](https://ares-emu.net/), an emulator.
> The open questions are listed in [AUDIT.md](docs/AUDIT.md).

| | |
|---|---|
| ![Scrolled](docs/images/demo-grid-scrolled.png) | ![Detail sheet](docs/images/demo-detail.png) |
| The grid. The bar on the right is your position; the yellow corner marks a favourite. | The detail sheet: what you have played, and how many cheats are switched on. |
| ![Cheats](docs/images/demo-cheats.png) | ![Settings](docs/images/demo-settings.png) |
| Cheats, as named groups rather than raw codes. | Settings, and what the menu found on your card. |

**Every game, every cover and every cheat above is invented.**
[`tools/mkdemo.py`](tools/mkdemo.py) draws original box art for titles that do not exist, so
nothing on this page belongs to anyone else.

---

## Setting up a card

See [CARD.md](docs/CARD.md) — the folder layout, where covers come from, the cheat database and
the emulator cores.

## Building it yourself

See [BUILDING.md](docs/BUILDING.md).

## Documentation

- [CARD.md](docs/CARD.md) — what goes on the SD card, and where covers come from.
- [AUDIT.md](docs/AUDIT.md) — every measurement, every dead end, every wrong turn. Append-only.
- [DESIGN.md](docs/DESIGN.md) — layout geometry, and where the design and the code disagree.
- [HARDWARE.md](docs/HARDWARE.md) — the order to bring hardware up in, and why that order.
- [BUILDING.md](docs/BUILDING.md) — toolchain, build knobs, fixtures and the regression suite.

The numbered guides under [`docs/`](docs/) are inherited from upstream and describe upstream's
behaviour. Some of them — the file browser and music player pages in particular — document
features this version removed.

---

# Licence

Released under the [GNU Affero General Public License](LICENSE.md), inherited from upstream.

Substantially the work of the N64FlashcartMenu authors and
[contributors](https://github.com/Polprzewodnikowy/N64FlashcartMenu/graphs/contributors); this
fork replaces the presentation layer and takes responsibility for its own bugs.

* [Mateusz Faderewski / Polprzewodnikowy](https://github.com/Polprzewodnikowy)
* [Robin Jones / NetworkFusion](https://github.com/networkfusion)

Not affiliated with or endorsed by any console or cartridge maker, or by the upstream project. The GitHub Actions
workflows are upstream's and have not been adapted — the build artifact is still labelled for a
flashcart this fork no longer supports.

# Open source software and licences used

## Libraries
* [libdragon](https://github.com/DragonMinded/libdragon/tree/preview) - [UNLICENSE License](https://github.com/DragonMinded/libdragon/blob/preview/LICENSE.md)
* [libspng](https://github.com/randy408/libspng) - [BSD 2-Clause License](https://github.com/randy408/libspng/blob/master/LICENSE)
* [miniz](https://github.com/richgel999/miniz) - [MIT License](https://github.com/richgel999/miniz/blob/master/LICENSE)
* [midi64](src/libs/midi64) - [MIT License](src/libs/midi64/LICENSE) — vendored, not a submodule
* [svg64](src/libs/svg64) - [MIT License](src/libs/svg64/LICENSE) — vendored, not a submodule
* [picojpeg](src/libs/picojpeg) - public domain, by Rich Geldreich

## Icons
The icons players choose from are from [game-icons.net](https://game-icons.net), under
[CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) (a few authors CC0). That is an
attribution licence and the cartridge is what redistributes, so all 36 authors are named in
[docs/CREDITS.md](docs/CREDITS.md) and on the menu's own credits screen.

## Sounds
See [License](https://pixabay.com/en/service/license-summary/) for the following sounds:
* [Cursor sound](https://pixabay.com/en/sound-effects/click-buttons-ui-menu-sounds-effects-button-7-203601/) by Skyscraper_seven (Free to use)
* [Actions (Enter, Back) sound](https://pixabay.com/en/sound-effects/menu-button-user-interface-pack-190041/) by Liecio (Free to use)
* [Error sound](https://pixabay.com/en/sound-effects/error-call-to-attention-129258/) by Universfield (Free to use)

## Music
The 28 songs in [`assets/music/`](assets/music/) are CC0. They are MIDI, not recordings: the
menu holds the notes and synthesises them as it plays, which is why a soundtrack that long costs
296 KB.

## Emulators
* [neon64v2](https://github.com/hcs64/neon64v2) by *hcs64* - [ISC License](https://github.com/hcs64/neon64v2/blob/master/LICENSE.txt)
* [sodium64](https://github.com/Hydr8gon/sodium64) by *Hydr8gon* - [GPL-3.0 License](https://github.com/Hydr8gon/sodium64/blob/master/LICENSE)
* [gb64](https://github.com/lambertjamesd/gb64) by *lambertjamesd* - [MIT License](https://github.com/lambertjamesd/gb64/blob/master/LICENSE)
* [smsPlus64](https://github.com/fhoedemakers/smsplus64) by *fhoedmakers* - [GPL-3.0 License](https://github.com/fhoedemakers/smsplus64/blob/main/LICENSE)
* [Press-F-Ultra](https://github.com/celerizer/Press-F-Ultra) by *celerizer* - [MIT License](https://github.com/celerizer/Press-F-Ultra/blob/master/LICENSE)

## Fonts
* [Firple](https://github.com/negset/Firple) by *negset* - (SIL Open Font License 1.1)
