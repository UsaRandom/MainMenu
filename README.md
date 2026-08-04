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
- Launching N64 titles, and NES / SNES / GB / GBC / SMS / GG through emulator cores

`cart_load.c` can also boot a Fairchild Channel F ROM, but nothing can reach that path: the
scanner has no `SYS_` for it and no extension maps to one, so such a title never enters the
library. `Press-F.z64` is listed below for when that is wired up, not because it works today.

Not done, and stated plainly:

- **The write half has never executed.** Favourites, play history, the library index, the
  thumbnail atlas and cheat selections all persist through one layer that decides at boot whether
  storage is writable, and under ares it never is — the DFS is read-only. The code is there and
  host-tested against real files; it has not run against a real card.
- **No hardware validation of any kind.** Five open questions are listed in
  [AUDIT.md §4](docs/AUDIT.md), starting with whether libdragon's custom IPL3 boots on an M64 at
  all — which blocks everything else if it fails.
- **The development cart has no working USB**, so `sc64deployer upload` and the UNFLoader debug
  channel are both unavailable, and there is currently no telemetry path on hardware at all.
  See [HARDWARE.md](docs/HARDWARE.md).

**De-scoped:** the ED64 and 64Drive drivers, and the MP3 player. This targets one cart on one
console. If you want broad flashcart support,
[upstream](https://github.com/Polprzewodnikowy/N64FlashcartMenu) is actively maintained and does
that job properly.

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

The cheat corpus is fetched, never committed — it is someone else's work and does not belong in
this history.

```sh
tools/mkcheatdb.py --fetch      # -> build/cheats.db, for /menu/cheats.db on the card
```

## Setting up a card

Only the first line is required. Everything else is optional and the menu degrades cleanly
without it — no art, no cheats and no emulators each cost you exactly that one feature.

```
/sc64menu.n64                              the menu itself
/roms/**                                   your games, in any layout you like
/menu/emulators/…                          cores for non-N64 systems
/menu/cheats.db                            built by tools/mkcheatdb.py --fetch
/menu/metadata/…                           a downloaded box-art pack
/menu/cache/                               created by the menu; do not hand-edit
```

**ROM layout is yours.** The scan walks `/roms` recursively — four levels deep — and reads a 4 KB
header from each file, so subfolders, naming and nesting are free. Tabs come from what a file *is*,
not from what its folder is called. A directory per system is a reasonable habit and nothing more.

| system | extensions |
|---|---|
| N64 | `.z64` `.n64` `.v64` `.rom` |
| NES | `.nes` |
| SNES | `.sfc` `.smc` |
| Game Boy / Color | `.gb` / `.gbc` |
| Master System / Game Gear | `.sms` `.gg` `.sg` |

Anything else is ignored, which is what lets art and save files sit in the same folders.

### Box art, and which one wins

Art can come from a file you dropped next to a ROM or from a bulk pack, and the two are looked up
in a fixed order. **A loose file always outranks the pack**, deliberately: a pack is something you
downloaded, a file you placed is a decision, and a decision should win.

| | where | example |
|---|---|---|
| 1 | a loose image named for the **game code**, anywhere under `/roms` | `NGEE.png` |
| 2 | a loose image named for the **ROM file**, anywhere under `/roms` | `Super Mario 64.jpg` |
| 3 | the metadata pack, region-specific | `/menu/metadata/N/G/E/E/boxart_front.png` |
| 4 | the metadata pack, region-agnostic | `/menu/metadata/N/G/E/boxart_front.png` |
| 5 | the metadata pack, flat | `/menu/metadata/NGEE.png` |

Worth knowing about each half:

- Loose files may be **`.png`, `.jpg` or `.jpeg`**; the metadata pack is read as **`.png` only**.
- Matching ignores case and the extension, so `ngee.JPG` and `NGEE.png` are the same key. Where
  two loose files collide the shallower one wins.
- Rules 1 and 3–5 need an N64 game code, which **NES, SNES, GB, GBC, SMS and GG titles do not
  have**. Rule 2 is the only one that works for them: name the image after the ROM.
- Any size and either orientation is fine. Everything is scaled and cover-cropped to 140 × 98 on
  the way in. The corpus this was measured against runs from 112 px to 2118 px wide and a quarter
  of it is portrait.
- A title with no art draws a plain tile and its name. That is the intended appearance, not a
  placeholder for something missing.

The first decode of a card costs real time — **about a quarter of a second per tile**, and far
more for an oversized one. It happens once: decoded tiles are written to `/menu/cache` and every
later boot reads them back. On a read-only card, or before that cache exists, expect the grid to
fill in over the first minute.

### Cheats

`/menu/cheats.db` is built by `tools/mkcheatdb.py --fetch` from
[libretro's cheat corpus](https://github.com/libretro/libretro-database) (MIT). It is a release
artifact — never committed, never linked into the ROM.

**Cheats are toggled as named groups, never per line.** A `D0` conditional and the write it guards
are one indivisible thing; upstream let you enable half a pair, which silently patched an
unrelated address. See [AUDIT.md §2](docs/AUDIT.md).

### Emulator cores

```
/menu/emulators/{neon64bu.rom,lithium64.z64,gb.v64,gbc.v64,smsPlus64.z64,Press-F.z64}
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
