# MainMenu

A box-art game launcher for the **ModRetro M64** with a [SummerCart64](https://github.com/Polprzewodnikowy/SummerCart64).

A fork of [N64FlashcartMenu](https://github.com/Polprzewodnikowy/N64FlashcartMenu) that keeps all
of its hardware plumbing and replaces the entire presentation layer. Upstream is a file browser —
a nineteen-row text list, no animation, no thumbnail cache. This presents **games, not files**: a
tabbed grid of box art that scrolls, remembers what you played, and requires you to prepare
nothing in advance.

> [!IMPORTANT]
> **This has never run on real hardware.** Every measurement in the audit was taken under
> [ares](https://ares-emu.net/). Read [Status](#status) before expecting it to work on a console.

---

## Status

Working under ares, against both a synthetic SD tree and real box art:

- Tabbed box-art grid — 140 × 98 tiles, four columns, twelve visible plus a peek row
- Resident library index built from upstream's 450-game database, with save-type and feature detection
- Thumbnail cache with on-cart generation and palette quantisation
- Detail sheet, cheats screen, settings, launch transition, boot plate, fault screen
- Launching N64 titles, and NES / SNES / GB / GBC / SMS / GG / Channel F through emulator cores

Not done, and stated plainly:

- **Nothing writes to the SD card.** The thumbnail cache, play history and cheat selections are
  in memory only. ares' filesystem is read-only, and a write path that has never executed is not
  a feature. These land with hardware.
- **No hardware validation of any kind.** Five open questions are listed in
  [AUDIT.md §4](docs/AUDIT.md), starting with whether libdragon's custom IPL3 boots on an M64 at
  all — which blocks everything else if it fails.
- **The development cart has no working USB**, so `sc64deployer upload` and the UNFLoader debug
  channel are both unavailable, and there is currently no telemetry path on hardware at all.
  See [HARDWARE.md](docs/HARDWARE.md).

## What is kept, and what is replaced

Upstream's roughly 15,000 lines of hardware-correct plumbing are worth considerably more than its
UI, so they are untouched.

| kept verbatim | replaced |
|---|---|
| `src/boot/` — bootloader, CIC, Datel cheat engine | `src/menu/views/` — all 23 views |
| `src/flashcart/{flashcart,flashcart_utils,sc64}` | `src/menu/ui_components/` except `background.c` |
| `rom_info.c` — the 450-game database | `menu.c`, `actions.c`, `menu_state.h` |
| `ini_parser.c`, `path.c`, `settings.c`, `src/utils/` | |

**De-scoped:** the ED64 and 64Drive drivers, and the MP3 player. This targets one cart on one
console. If you want broad flashcart support,
[upstream](https://github.com/Polprzewodnikowy/N64FlashcartMenu) is actively maintained and does
that job properly.

Two decisions are load-bearing enough to state up front:

**`render()` does no I/O and no allocation.** Streaming happens in a third phase, `background()`,
which runs while the RDP drains. Motion is specified in seconds rather than frames, so the scroll
feel does not change with the video mode.

**Cheats are toggled as named groups, never per line.** Upstream emits one address-value pair per
independently enabled line, while the engine consumes two consecutive entries for a `0x50` or
`0xD_` conditional — so disabling half of a pair silently patches an unrelated address. The group
model makes that unrepresentable. See [AUDIT.md §2](docs/AUDIT.md).

## Build

```sh
export N64_INST="$HOME/n64inst-preview"     # NOT ~/n64inst — see AUDIT.md §2.1
git submodule update --init --recursive
make sc64 -j8                                # -> output/sc64menu.n64
```

The pinned libdragon (`5cb976a`) must be installed to a **separate prefix**. Installing it over a
shared `~/n64inst` silently changes the memory map for anything else built against it. **Never set
`N64_GCCPREFIX`** — splitting the compiler prefix from the install prefix links a stale
`libdragon.a` and produces undefined references that look like the submodule being too old.

Corpora are fetched, never committed. The art repository is 1.77 GB and the cheat corpus is
someone else's work; neither belongs in this history.

```sh
tools/getart.py --count 40      # box art -> build/artcache
tools/mkcheatdb.py --fetch      # cheats  -> build/cheats.db
```

## SD card layout

```
/sc64menu.n64
/roms/n64/*.z64                            games
/roms/{nes,snes,gb,gbc,sms}/*              emulated systems
/menu/emulators/{neon64bu.rom,lithium64.z64,gb.v64,gbc.v64,smsPlus64.z64,Press-F.z64}
/menu/metadata/<G>/<A>/<M>/<E>/boxart_front.png
/menu/cheats.db                            optional; from tools/mkcheatdb.py
```

SNES titles run on [lithium64](https://github.com/UsaRandom/lithium64), a fork of sodium64
targeting this console, falling back to `sodium64.z64` when it is absent.

## Documentation

- [AUDIT.md](docs/AUDIT.md) — every measurement, every dead end, every harness trap. Append-only.
- [DESIGN.md](docs/DESIGN.md) — layout geometry, and where the spec and the implementation disagree.
- [HARDWARE.md](docs/HARDWARE.md) — the order to bring hardware up in, and why that order.

The numbered guides under [`docs/`](docs/) are inherited from upstream and describe upstream's
behaviour. Some — the file browser and MP3 player pages in particular — document features this
fork removed.

## House rules

Measure, don't assert: any claim about speed, size or memory carries a number and says how it was
obtained. Check that a test can fail before trusting a green result. Record negative results —
ruled-out hypotheses and harness traps go into AUDIT.md and stay there, struck through when fixed
rather than deleted.

---

# Licence

Released under the [GNU Affero General Public License](LICENSE.md), inherited from upstream.

Substantially the work of the N64FlashcartMenu authors and
[contributors](https://github.com/Polprzewodnikowy/N64FlashcartMenu/graphs/contributors); this
fork replaces the presentation layer and takes responsibility for its own bugs.

* [Mateusz Faderewski / Polprzewodnikowy](https://github.com/Polprzewodnikowy)
* [Robin Jones / NetworkFusion](https://github.com/networkfusion)

Not affiliated with or endorsed by ModRetro, Nintendo, or the upstream project. The GitHub Actions
workflows are upstream's and have not been adapted — the build artifact is still labelled for a
flashcart this fork no longer supports.

# Open source software and licences used

## Libraries
* [libdragon](https://github.com/DragonMinded/libdragon/tree/preview) - [UNLICENSE License](https://github.com/DragonMinded/libdragon/blob/preview/LICENSE.md)
* [libspng](https://github.com/randy408/libspng) - [BSD 2-Clause License](https://github.com/randy408/libspng/blob/master/LICENSE)
* [miniz](https://github.com/richgel999/miniz) - [MIT License](https://github.com/richgel999/miniz/blob/master/LICENSE)
* [minimp3](https://github.com/lieff/minimp3) - [CC0 1.0 Universal](https://github.com/lieff/minimp3/blob/master/LICENSE) — still vendored as a submodule, no longer compiled

## Sounds
See [License](https://pixabay.com/en/service/license-summary/) for the following sounds:
* [Cursor sound](https://pixabay.com/en/sound-effects/click-buttons-ui-menu-sounds-effects-button-7-203601/) by Skyscraper_seven (Free to use)
* [Actions (Enter, Back) sound](https://pixabay.com/en/sound-effects/menu-button-user-interface-pack-190041/) by Liecio (Free to use)
* [Error sound](https://pixabay.com/en/sound-effects/error-call-to-attention-129258/) by Universfield (Free to use)

See [License](https://creativecommons.org/licenses/by/4.0/) for the following sounds:
* [Background Music](https://www.playonloop.com/2017-music-loops/flying-dreams/) POL-flying-dreams-short

## Emulators
* [neon64v2](https://github.com/hcs64/neon64v2) by *hcs64* - [ISC License](https://github.com/hcs64/neon64v2/blob/master/LICENSE.txt)
* [sodium64](https://github.com/Hydr8gon/sodium64) by *Hydr8gon* - [GPL-3.0 License](https://github.com/Hydr8gon/sodium64/blob/master/LICENSE)
* [gb64](https://github.com/lambertjamesd/gb64) by *lambertjamesd* - [MIT License](https://github.com/lambertjamesd/gb64/blob/master/LICENSE)
* [smsPlus64](https://github.com/fhoedemakers/smsplus64) by *fhoedmakers* - [GPL-3.0 License](https://github.com/fhoedemakers/smsplus64/blob/main/LICENSE)
* [Press-F-Ultra](https://github.com/celerizer/Press-F-Ultra) by *celerizer* - [MIT License](https://github.com/celerizer/Press-F-Ultra/blob/master/LICENSE)

## Fonts
* [Firple](https://github.com/negset/Firple) by *negset* - (SIL Open Font License 1.1)
