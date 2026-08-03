# MainMenu audit

Append-only. Findings are struck through when fixed, never deleted. Superseded measurements stay
and are marked superseded. Negative results and harness traps are recorded as prominently as
results — a trap that produced a convincing fake number is worth more than the number was.

Every claim here carries a measurement or is explicitly marked unverified.

---

## 0. What this project is

A replacement presentation layer for `Polprzewodnikowy/N64FlashcartMenu` @ `6407ab15` (v0.3.2),
targeting the **ModRetro M64** with a SummerCart64, developed against **ares**.

Kept from upstream: `src/boot/`, `src/flashcart/{flashcart,flashcart_utils,sc64}`,
`rom_info.c`, `cart_load.c`, `ini_parser.c`, `path.c`, `settings.c`, `src/utils/`.
Replaced: everything under `src/menu/views/` and `src/menu/ui_components/` except `background.c`,
plus `menu.c`, `actions.c`, `menu_state.h`.
De-scoped: ED64 and 64drive drivers.

Layout geometry is in [DESIGN.md](DESIGN.md).

---

## 1. Baseline — M0

Toolchain: libdragon submodule `5cb976a`
(`toolchain-continuous-prerelease-4513-g5cb976aab`), installed to `~/n64inst-preview`.

Unmodified upstream, `make sc64`, no `FLAGS`:

| | bytes |
|---|---|
| `.text` | 569,112 |
| `.data` | 124,660 |
| `.bss` | 57,456 |
| ELF total | 751,228 |
| `output/sc64menu.n64` | 1,671,168 |

Every later size claim is a delta against this row.

Boots in ares v148 and reaches the browser.

---

## 1b. Harness — M1

`tools/regress.sh` builds with `FIXTURE=1 DEV_HARNESS=1`, runs ares headless, and hashes every
frame the ROM dumped. The run ends when the input script's `exit` reaches `emux_ioctl_exit()`;
the timeout only catches a ROM that never gets there.

**Reproducibility gate: PASS.** Two independent `regress.sh` runs of `browse-roms` produce
byte-identical hashes across all five frames. Nothing downstream is measurable without this.

    browse-roms 00 84295a6e95c71ac7 160x120     credits (first-run screen)
    browse-roms 01 f36b53d5562a92e0 160x120     browser at /roms
    browse-roms 02 1c99e3cf17cb2379 160x120     roms/n64 listing, 40 stubs
    browse-roms 03 db618440ffc51a6f 160x120     scrolled six entries
    browse-roms 04 83f55a38cc649b84 160x120     ROM detail, art rejected (see 1c)

Frame 04 was `7e3bfbb0d5309e62` while the fixture generated 158 × 112 art. It changed once
the fixture moved to the 280 × 196 asset spec — see §1c.

Wall-clock cost of one scripted run under ares: **17 s**, build included.

Fixture ROM: 2,179,072 bytes, versus the 1,671,168 baseline — the 40 stubs, 40 box art PNGs
and 11 emulated-system stubs add 508 KB to the DFS.

Frame 04 confirms the whole art path end to end on unmodified production code: fixture tree →
DFS → `rom:/` prefix → `find_rom_in_database` → boxart directory probe → `png_decoder` →
`rdpq_tex_blit`. The art has its game code stamped across it in large glyphs, so the code on
the tile can be read against the title beside it; a mis-mapped index is visible rather than
plausible.

## 1n. The emulator launch path had never once run, and it was hiding a bad path separator

### lithium64 is now the SNES core

`cart_load.c` loaded `sodium64.z64`. It now loads `lithium64.z64` and falls back to
`sodium64.z64` when that is absent, so a card prepared for upstream still plays SNES titles.

lithium64 is our fork of sodium64 and is the one targeting this console: sodium64 sizes its ROM
cache to fill exactly 4 MB of RDRAM and programs the VI, AI, PI and SI with values a stock N64
accepts, and both are assumptions an FPGA reimplementation is entitled to break.

The two are interchangeable at this seam, and it is worth writing down *why* rather than
observing that it works, because nothing in the build would catch them diverging:

| | value | source |
|---|---|---|
| save type | `sram256k` | lithium64 `Makefile:12`, matching `FLASHCART_SAVE_TYPE_SRAM_256KBIT` |
| emulated ROM address | PI `0x10200000` | lithium64 `src/memory.S:225`, matching `emulated_rom_offset = 0x200000` |
| ROM format | headerless `.smc`/`.sfc` | lithium64 README, matching the copier-header strip below |

### The fixture had no emulators directory, so every emulated launch stopped at the first check

`menu/emulators/` contained one `.gitkeep`. Every SNES, NES, GB, GBC and SMS launch therefore
returned `CART_LOAD_ERR_EMU_NOT_FOUND` from the `file_exists` at the top of
`cart_load_emulator`, and **everything after that line had never executed** — the system-to-core
mapping, the path built for the core, the copier-header decision, the save path.

This is the same shape as the three defects in 1m: it looked finished from outside, because a
missing emulator core is a perfectly plausible thing for a fixture to report.

The fixture now writes 4 KB stubs for all five cores. They are not runnable, and do not need to
be: under ares the flashcart is the dummy vtable whose `load_rom`/`load_file` return `OK` without
doing anything, so what these exercise is every decision *around* the upload.

### The copier-header branch was untestable because every stub was the same size

`cart_load.c:221` strips a 512-byte copier header when `(size & 0x3FF) == 0x200`. Every fixture
SNES stub was a round 2048 bytes, so the test could only ever answer 0 — and a `file_get_size`
that failed and returned 0 would have produced *the same answer*, which is the reason this needed
a discriminating case rather than a passing one. Every second SNES stub now carries a 512-byte
header.

Measured, two runs of `tools/inputs/manual/launch-snes.txt`:

```
emu: type=1 core=rom:/menu/emulators/lithium64.z64
emu: rom=rom:/roms/snes/Chrono Drift.sfc dst=0x00200000 skip=0x0      (2048 bytes)
emu: rom=rom:/roms/snes/Star Relic.sfc   dst=0x00200000 skip=0x200    (2560 bytes)
```

The two differ, so the branch discriminates and the size was genuinely read. The frame counter
restarts at `n=240` immediately after those lines, which is `boot()` rebooting the console into
the cart — the launch completes rather than merely returning `OK`.

That script cannot be part of the hash gate, since the run never asks to exit. It lives in
`tools/inputs/manual/`; `regress.sh` globs `tools/inputs/*.txt` and does not recurse, so it is
excluded by where it sits rather than by a list someone has to maintain.

### And the thing that fell out of it: every library path had a doubled separator

The first run printed `rom://roms/snes/Star Relic.sfc`. `library_scan` joined its prefix and root
with a plain `snprintf("%s%s")`, and `app->storage` already ends in a slash (`"rom:/"`, `"sd:/"`)
while every caller passes an absolute root. The doubled separator was copied into `rec->path` for
**every title in the library**, and `rec->path` is what the launch path opens.

DFS accepts it, which is exactly why it survived to now — the ares run above opened, sized and
launched the file correctly through it. FatFs over the SC64 has never been asked. Upstream's own
`path_init` handles this case; `library_scan` was the one place that hand-rolled the join.

Fixed, and re-measured: `rom:/roms/snes/Star Relic.sfc`, `skip=0x200` unchanged.

Worth naming the near-miss: this would have presented on hardware as "the menu lists games and
refuses to launch any of them", with the paths looking right in every log line because a doubled
slash does not read as wrong.

### A related trap in the Makefile, found while re-running the fixture

`make fixture` and the automatic regenerate in `dfsroot` both called `mkfixture.py` **without
`--art-from`**, so any automatic regenerate silently replaced 34 real box-art cards with
procedural gradients. Every decode measurement in 1g and 1h was taken against the real ones
(2.4 µs/px ordinary, 7.5 µs/px largest); the procedural fixture is 5.6 MB against 30.5 MB and
would have quietly reported a much faster decoder. Both fixtures look fine on screen — only the
numbers move. The Makefile now passes `--art-from build/artcache` whenever that directory exists.

## 1m. Type size, the real mark, and three things that were drawn but not wired

### The body font was half the size the spec asks for

docs/design/README.md section 3 is blunt: *"Upstream's single 15 px bold is roughly half the
angular size needed at 2.5 m from a 32-inch panel. If you have to drop a size to save ROM, drop
16, not 20."* Everything was rendering at upstream's 15 px.

Rebaked at 20 px: **486,203 -> 681,040 bytes**, +195 KB on a 1.77 MB ROM.

That forced a spec feature that had been skipped. Nine tab labels at the 20 px advance metric
measure **744 px against a 608 px rail** -- and at 15 px they measured 612, so the rail was
already 4 px over and nobody had noticed. Section 4.3: *"The three virtual tabs are icon-only when
inactive and gain their label when active."* Implemented -- corner triangle, clock, bar chart --
which brings the worst case (Most Played active) to **576 px**.

### The boot mark is now the real one

`tools/mklogo.py` converts `Polprzewodnikowy/SummerCart64` `assets/sc64_logo.svg` -- 256 x 180,
which is exactly the 1.422 aspect section 4.1 asks for, so it downscales to 192 x 135 with no
cropping.

The only rasteriser on this machine is Quick Look, which renders into a **square** and letterboxes,
and flattens onto **opaque white** with no alpha. So it renders twice: geometry comes from a plain
render, measuring content bounds against the corner colour rather than assuming where Quick Look
put the padding; pixels come from a second render with a black background rect injected into the
SVG. That second pass is what gets the antialiased edges right -- recolouring near-white
afterwards leaves a halo on every edge, which is the obvious approach and the wrong one.

### Three bugs, all of the same shape: drawn but not wired

**The mark vanished mid-animation.** The rise borrowed `EASE_SHEET_OPEN`, which overshoots to
1.08 by design. `alpha = lerpf(0.22, 1.0, 1.06)` is 1.06, and `(uint8_t)(alpha * 255)` wrapped to
**14** -- the mark went black for a third of its own entrance. Scale may overshoot; opacity may
not. Now on the tile-arrival curve with alpha clamped separately.

**Letter tracking was done by spelling the string.** `"M A I N   M E N U"` with a comment
asserting that *"rdpq has no letter-spacing parameter"*. It does: `rdpq_textparms_t.char_spacing`.
The hand-spelled version gave roughly 32 px of tracking where the spec asks for 10, and made the
string untranslatable. **The comment stated a limitation that was never checked.**

**No sound had ever played.** `sound_init_sfx()` was called at startup, five effects were baked
into the ROM, and no screen called `sound_play_effect()` even once. Wired across 15 sites per
section 6 -- cursor on every accepted step including repeats, enter on descent, exit on B, setting
on a toggle.

And `sound_use_sfx()` was never called at all, so the settings screen's "Sound effects" row
flipped a bool that reached nothing. It now applies at startup and on change.

## 1l. The boot plate, and 46,600 wasted record visits a frame

### Boot, section 4.1

Implemented as an overlay on the grid rather than as a screen of its own, and the reason is in the
spec's own wording: *"The grid is already composited underneath and already has a selection --
there is no second fade-in."* A separate `SCREEN_BOOT` would have to hand over at t=1.64 to
something arriving cold, and one frame of empty grid before the first tile lands **is** the second
fade-in. Drawn on top of a live grid, the library has been scanning and decoding underneath for
the whole 1.64 s and the curtain reveals something already running.

Timeline as specified: mark at 88 % scale / 22 % opacity at t=0, full at 0.55, holds, whole plate
translates -480 px over 0.34 s from t=1.30. Total 1.64 s. Input is swallowed while the plate is up
so a button pressed during boot cannot land on a grid nobody can see, but the grid keeps updating
underneath.

**Two deviations, both deliberate and both visible:**

The **SC64 mark is a placeholder** -- a bracket motif in a 192x135 plate. The handoff ships the
mark as art that is not in this repo, and inventing a logo would be worse than obviously standing
in for one. It reads as a placeholder at a glance, which is what a placeholder should do.

The **boot SFX does not fire**, because there is no boot sound in `assets/sounds/`. The spec calls
it "the only sound with a tail, and the only one anyone will remember"; substituting `enter.wav`
would be worse than silence.

**32 px type needed a second font page.** The body font is baked at 15 px and rdpq scales glyph
quads, so 15 stretched to 32 is visibly soft next to type that is not. The boot plate says four
fixed strings, so the second page carries a **41-character charset** instead of the body font's
7,931: **5,016 bytes** against the 486,203 the full charset costs at 15 px. Letter tracking is
spelled by hand (`"M A I N   M E N U"`) because rdpq has no letter-spacing parameter and the
alternative is a third font page differing only in advance width.

### An idle cache that was not idle

The same settled session that showed a clean 60 fps also showed `scanus=9409` with
`starts=0 stats=0 rows=0` -- nine milliseconds a frame spent looking for work and finding none.

Arithmetic on a 50,220-frame session: the spin loop calls `background()` **207.9 times per
displayed frame**, and each call walked all four passes over the whole library. **About 46,600
record visits per frame, all fruitless.**

It cost no frames -- `f1=60 f2=0` throughout -- which is exactly why it survived. There was slack
to waste, so wasting it was invisible.

`thumbcache_run()` now short-circuits on an `idle` flag, set when a full four-pass scan finds
nothing startable and cleared by anything that can create work. Measured after: **`scanus` 9,409
-> 0, spin iterations 23,464,219 -> 39,175.**

### The bug that found

Chasing the idle flag surfaced something worse in `claim_slot()`: eviction freed the surface and
decremented `resident`, but **never reset the evicted record's `art_state`**. The record kept
`ART_READY` with no art, so `thumbcache_get()` missed, added it to `wanted[]`, and
`thumbcache_run()` skipped it for not being `ART_PENDING`.

**An evicted tile could never come back.** With 56 records against 20 slots, scrolling away and
back left permanent blanks -- and the blanks looked exactly like art that had not decoded yet,
which is why several sessions of looking at this screen never flagged it.

Eviction now returns the record to `ART_PENDING` and clears `idle`.

## 1j. The allocation gate, and the self-test that nearly certified a broken instrument

The plan's M5 asks for **zero allocations in the steady-state frame**. An allocation per frame is
invisible at 60 Hz until the heap fragments an hour later, and there is no MMU here to turn that
into a crash anyone can find.

`src/dev/allocwatch.c` counts heap traffic under `-Wl,--wrap=malloc` and friends -- the only
mechanism that sees allocations inside libdragon and libspng as well as ours. `tools/inputs/idle.txt`
waits 1,200 frames for art to settle, then sits still.

**Result: 0 mallocs, 0 reallocs, 0 frees per 60-frame window, sustained.**

### The part worth keeping

That number was worthless until the instrument was shown capable of reporting a different one,
and getting there took three attempts, two of which produced a **green result from a broken
test**:

1. `free(malloc(64))` in `grid_render()`, one per frame → reported **0**. GCC's allocation DCE
   under `-flto` deletes a malloc/free pair with no observable effect outright.
2. Routed through `static volatile void *sink` → still **0**. A volatile store whose value is
   never read does not make the allocation observable enough to survive.
3. Value accumulated into a volatile and branched on → **60 mallocs, 60 frees per 60 frames**,
   exactly as injected.

Attempts 1 and 2 both agreed with the real measurement. Had the self-test been skipped -- or
stopped at the first attempt, which *looked* like a correct negative control -- a counter that
never fired would have been recorded as a passing gate. **A self-test that reproduces the
expected answer for the wrong reason is worse than no self-test**, because it converts an
unverified claim into a verified-looking one.

The injection lives behind `ALLOCWATCH_SELFTEST`, which no real build defines, and the comment
next to it names the DCE so the next person does not rediscover it.

### Two other measurement facts from the same work

`N64_LDFLAGS` goes **straight to `ld`**, not through gcc, so `-Wl,--wrap=malloc` fails with
"unrecognized option '-Wl'". Bare `--wrap=malloc` is correct.

`tools/contactsheet.py` now exists: it tiles dumps into one strip with frame indices stamped on.
The hash gate answers "did anything change"; a strip answers "is the change the one I wanted",
and comb artefacts, a tile arriving a frame late, and sub-pixel shimmer are all invisible in any
single frame. Neither replaces the other, and the hash gate is the one that runs unattended.

## 1k. Ambient wash and favourites

**The ambient wash** (docs/design/README.md section 4) needed a per-game colour, and
`lib_record_t.dominant` had been declared and never computed. It is now a weighted mean taken in
the decode callback over every fourth pixel in both axes -- 858 samples, one pass, no allocation.

Weighted by `(max channel - min channel)`, not flat. A flat mean is the wrong answer for box art:
most cards are a large dark or white field around a small vivid logo, so averaging gives grey
every time -- **the same grey for every game**, which is worse than not having the feature.

Drawn as one blended 420x300 quad at 15 % alpha, eased per channel in 5-bit space over
`DUR_AMBIENT_REKEY`. Interpolating the packed RGBA5551 word instead would walk through colours
present in neither endpoint. The handoff nominates this as the first thing to cut if fill rate
runs out, so it is one layer and nothing more.

**Favourites** toggle with Z and the tab filters live -- un-favouriting from inside the Favorites
tab rebuilds the view under the cursor. In memory only, lost on reboot until `playstate.dat`
exists. The interaction is here because the tab is otherwise permanently empty and unreviewable.

## 1i. The menu is a launcher again, and cheats are real

Five screens now: grid, detail sheet, cheats, settings, launch. Everything below is read-only --
every write to the card (thumbnail cache, play history, which cheats you ticked) is deferred
until there is hardware to test a write against, since ares' DFS cannot be written at all.

### The launch path, and a framebuffer leak caught before it shipped

`cart_load.c` moved from `menu_t` to `app_t` -- 31 references, all of them reaching into either
`load.rom_info`, `settings`, `storage_prefix`, or `browser.directory` + `browser.entry->name`.
That last one was the only real coupling: it wanted "the entry the file list is sitting on",
which has no meaning in a grid. Both loaders now read `app->launch.rom_path`.

64DD went with it. `disk_info.c` is not compiled and the M64 has no disk drive.

**The bug worth recording** is one the structure invited. The main loop calls
`display_try_get()` and *then* `update()`, so the screen already holds a framebuffer by the time
update runs. `flashcart_load_rom()` blocks for the whole transfer and drives a progress callback,
and the obvious place to call it is update() -- where the held buffer is never attached and never
shown, so it leaks, and three progress frames later there are none left. The load runs from
`render()` instead, after the frame it was given has been shown.

### Cheats: 321 games, and four bugs found building it

`tools/mkcheatdb.py` converts libretro's MIT-licensed corpus; `tools/mkcheatkeys.py` bridges its
filename keys to N64 header keys using the 440 `MATCH_*` rows in `rom_info.c`, which is the only
title table we have. **976 of 1,345 corpus files key successfully**; the 369 that do not are
listed in `build/cheatkeys-report.txt` rather than dropped quietly.

Final database: **321 unique games, 36,972 cheats, 1.49 MB, 18,011 cheats dropped.**

Four things went wrong, all of which would have shipped silently:

**The region byte was invented.** `MATCH_ID` rows match *any* region, and the harvester appended
`"E"` to the 3-character code to make it 4. That keys every Japanese release to a USA code, so
`NHFJ` would never match a stored `NHFE`. The wildcard is now carried through as `'?'` and the
reader compares three characters when it sees one.

**The index had duplicate keys and the reader picked arbitrarily.** The corpus carries several
files per game -- regional variants, revisions, re-dumps -- and the key table maps them all to one
header key. Measured on the fixture: **126 index rows matched 48 games**, with AeroGauge present
five times at 3, 3, 133, 136 and 142 cheats. The reader's linear scan returns the first, so the
menu was showing 3 of the 142 available and nothing said so. Rows sharing a key are now merged
with names deduplicated: 936 rows became 321, and AeroGauge shows 43.

**The coverage report counted cheats from games it then discarded.** `kept_total` was accumulated
before the key lookup that drops unkeyable games, so the report claimed 199,675 cheats when a
third of them were in entries nothing could look up. Counting moved after the drop: 152,874, then
36,972 once duplicates merged. **A report that overstates by 5x is worse than no report**, because
it gets quoted.

**`code_first` and `cheat_count` are `uint16` on disk** and the corpus's largest entries run to
five figures of lines. A wrap there hands the engine addresses belonging to a different cheat.
Guarded in the converter with a `TRUNC` line in the report; nothing in the current corpus trips
it, which is exactly when a guard is cheap to add.

### The two engine bugs from section 2 are fixed

`cheats_install()` now bounds both pointers before each entry. The limit is derived from
`get_memory_size()` against where the engine will *end up*, not where it is staged -- the staging
buffer has 743 KB of slack while the final location has whatever lies between 0x807C5C00 and the
top of RDRAM. Worst case for one entry is 767 words, because a `0x50` repeater emits three
instructions per iteration with an 8-bit count. Checked before the entry rather than after each
write: `*engine_p++` appears in a dozen places and a check at each is a dozen chances to miss one.

The patcher region was unbounded too, and had no finding of its own. It runs from
`PATCHER_ADDRESS` to where the engine stages, so a long enough run of boot-writes walked into the
engine it was about to copy.

### Group toggles, so finding 2.2 cannot recur

The UI can only toggle a named group, and `cheatdb_emit()` writes every line of an enabled group
contiguously or none of it. There is no API that can separate a `D0` conditional from the write
it guards. The capacity check is per-group for the same reason -- stopping halfway through would
leave the dangling conditional to pair with whatever came next, which is the original bug.

### Smaller things fixed on the way

**Every `STL_GRAY` label was rendering white.** `text_at()` took a style parameter and never put
it in `rdpq_textparms_t.style_id`, so all seven styles registered in `fonts.c` were unreachable
and the dimmed secondary text the spec asks for was never dim.

**Detail rows overlapped instead of clipping.** "Accessories" against "Controller Rumble" drew
the two strings through each other, which reads as a font bug. Rows now wrap to a second line
when the pair does not fit.

### Regression state

**GATE PASS: 22 frames across 5 scripts, byte-identical across two runs.** Scripts:
`browse-roms`, `real-art`, `scroll-stress`, `detail-sheet`, `cheats`.

## 1g. The frame budget could never have worked, and three hypotheses died proving it

The starting claim, recorded in §1e and repeated to the user, was **"52 fps while decoding"** with
the decode budget named as the thing to tune. Both halves were wrong, and the way they were wrong
is the point: the number came from sampling `app->dt` **once every 60 frames** and reading 1.7 % of
the run as if it described all of it.

### What the sampled number actually said

Re-analysing a 15,120-frame interactive session (the user's own, 4.2 minutes, 252 samples):

| | µs |
|---|---|
| p50 | 16,707 — a clean 59.9 fps |
| p95 | 26,644 |
| max | 29,755 |
| samples over 17.5 ms | 63 of 252 (25 %) |

So the median frame was always making its field and a quarter of samples were missing badly. A
single mean over that distribution is the one statistic that describes neither mode. "52 fps"
was the average of a bimodal signal, and averaging a bimodal signal is how you get a number that
is never observed.

Replaced with per-field bins over **every** frame, not one in sixty (`src/app.c`). Frames are
binned by how many 60 Hz fields they occupied, because on a fixed-refresh display "did it make
the field" is the only question and 17 ms and 32 ms are the same answer to it.

Two details worth keeping. The bin uses the **unclamped** interval: `app->dt` is clamped to 1/15 s
so a stall cannot teleport an animation, and binning the clamped value would cap every
measurement at 66.7 ms and erase exactly the frames worth looking at. And it rounds **up**, not to
nearest — a 25 ms frame has missed a field, and rounding to nearest files it as on time, which is
precisely the frame the instrumentation exists to catch.

### Under sustained scroll: 40 % of frames take two fields

`tools/inputs/scroll-stress.txt` keeps the selection moving so the working set is never allowed
to catch up. Steady state, every window: **f1 ≈ 36, f2 ≈ 24, effective 43 fps.** Not a tail — a
mode.

### Three dead hypotheses

**Dead 1 — the decode budget.** Swept `DECODE_BUDGET_IDLE_US` across five builds:

| budget µs | 1 field | 2 fields | eff. fps |
|---|---|---|---|
| 0 | 61 % | 39 % | 43.0 |
| 1,500 | 59 % | 41 % | 42.5 |
| 3,000 | 58 % | 42 % | 42.3 |
| 6,000 | 60 % | 40 % | 42.7 |
| 12,000 | 35 % | 64 % | 36.4 |

**Zero budget changed nothing.** The tunable named as the cause was not the cause.

That sweep was also a badly built experiment and is recorded as one: the stress script keeps the
cursor moving, so `frames_since_move` almost never reaches `DECODE_SETTLE_FRAMES` and the IDLE
branch being swept barely ran. It only produced a real result at 12,000 by accident. **Check that
the knob you are turning is connected before believing the four readings that did not move.**

**Dead 2 — where the time goes.** Attributing the frame to update / render / background, which
should have preceded the sweep and would have refuted it in one run:

```
upd_us=67   rnd_us=4,700   bg_us=9,000   spin_us=3,500
```

`background()` at **9,000 µs per frame against a 1,200 µs budget** — over by 7.5×.

**Dead 3 — unbuffered file I/O.** Upstream opens the PNG with `setbuf(f, NULL)`, so every read
spng issues becomes a filesystem trip. Plausible, cheap to test, and **wrong**: a 16 KB `setvbuf`
moved the worst row from 17,578 µs to 19,275 µs, i.e. nothing outside noise. The buffer is kept
because it is free on this machine and correct on principle, but it bought no measured time and
must not be cited as an optimisation.

### The actual cause: the unit of work is larger than the frame

Counting rows rather than guessing (`png_rows_done`, `png_worst_row_us`):

**A single decoded PNG row costs 5,000–20,000 µs.** The worst measured row is 20,634 µs — one
row of one image taking longer than a whole 60 Hz field.

`png_decoder_poll_budget` can only stop **between** rows. When one row costs more than the entire
frame, the budget is decorative: any value from 0 to 6,000 produces the same behaviour, which is
exactly what the sweep showed and neither the sweep nor the budget's own comment could explain.

Not an I/O problem and not a budget problem — a 1000-to-2118 px row of inflate plus unfilter plus
box-filter on a VR4300 with an 8 KB data cache against zlib's 32 KB window. There is no tuning
constant for that.

### What was changed, and what it is worth

`grid_background()` now decodes **nothing at all** while the selection is moving, instead of
decoding on a small budget. Same stress script:

| window | | |
|---|---|---|
| moving (rows=0) | f1=60 f2=0 | worst 17,612 µs |
| moving (rows=3) | f1=59 f2=1 | worst 20,819 µs |
| settled, decoding | f1=36 f2=24 | worst 26,854 µs |

**Scrolling is now a clean 60 fps.** The 43 fps still exists and is unchanged; it has been moved
to the moment after you stop, where a dropped field reads as art appearing rather than as judder.

This makes nothing faster and is not claimed to. It relocates a cost that cannot currently be
removed. The permanent answer is the on-disk cache — not for streaming bandwidth, but so a
155 ms decode is paid once ever instead of once per boot — and that cannot be built against ares,
where the DFS is read-only. Parked until hardware.

Reproducibility gate re-run after all of it: **8 frames across 2 scripts, byte-identical.**

## 1h. The decoder is the product's real constraint, and it is not schedulable

Chasing the frame rate in §1g led to the decoder, and the decoder turns out to be the binding
constraint on the whole design. Four bugs, then a number that ends the argument.

### The scaled decoder allocated the full-size surface anyway

`png_decoder_start_scaled()` called `png_decoder_start()`, let it `surface_alloc` a surface **the
size of the file**, then freed it and allocated the 140 x 98 one. For the corpus's largest card,
2118 x 1457, that is **6.17 MB** requested on an 8 MB machine already holding 1.84 MB of
framebuffers. It failed with `PNG_ERR_OUT_OF_MEM` before decoding a row.

The header comment above it promised "27 KB rather than 1.5 MB". That described the intent of the
code and not its behaviour, and it had been written confidently enough that it was never checked.
**A comment asserting a measurable property is a claim, and claims get verified.**

Fixed by taking the destination size in the opener, so the file-sized surface never exists.

### Fixing that exposed a worse one: one card blocks every other

With the big card decodable, a 900-frame run produced **`starts=1`, then zero for the remaining
840 frames, and a completely empty grid**. Inflate is sequential — there is no way to decode less
of a PNG than all of it — so 1,457 rows at ~26,000 µs is **38 seconds for one tile**, and the
decoder is single-instance, so nothing else decodes for the duration.

Invisible before the OOM fix, because the card used to fail instantly and cost nothing. Making it
work is what revealed that it monopolises everything.

Now four passes: visible-cheap, visible-costly, any-cheap, any-costly, with `THUMB_CHEAP_BYTES`
at 400 KB.

Two things went wrong getting there, both worth keeping:

- **First ordering put both cheap passes ahead of both costly ones**, so the grid filled four
  visible tiles and then went off to prefetch art the user could not see. Visibility outranks
  cost; size only breaks ties within a visibility class.
- **First implementation re-ran `file_get_size` on every candidate on every pass** — 180
  filesystem probes and 6,437 µs per frame, producing 100,342 µs frames. Worse than the problem
  it was added to fix. The verdict is now cached as `ART_COSTLY`, so each card is probed once:
  stats 180 → 22, worst frame 100,342 → 43,384 µs.

### Where row time actually goes

| | µs per frame |
|---|---|
| `spng_decode_row` | 13,583 |
| box-filter scaler | 2,396 |

**85 % is inflate.** The scaler's inner loop recomputed `(sx * dst_w) / crop_w` per *source*
pixel — a ~37-cycle non-pipelined VR4300 divide, 2,118 of them on the widest card. Replacing it
with an incremental index took the scaler from 2,396 to 1,244 µs per frame, **48 % off the 15 %
that was mine to fix.**

That rewrite is bit-exact, and proving it needed a host program rather than the hash gate: the
faster scaler decodes more tiles before the dump frame, so the frames legitimately differ and the
gate cannot isolate the change. Exhaustive enumeration over every `crop_w` from 1 to 4096 at
`dst_w = 140` — 8,390,656 pairs — gives an identical `dx` and an identical skip decision at every
one. **When the gate cannot separate two variables, do not read its verdict as being about the
one you care about.**

### The number that settles the design

Measured decode rate: **2.4 µs/pixel** on ordinary cards, **7.5 µs/pixel** on the largest.

The 48-card fixture is 21.3 M pixels. **50 to 160 seconds to decode the library once** — and
today that is paid on every single boot.

No scheduling policy fixes this; the four-pass ordering only chooses who waits. The remaining
85 % is inside spng and would need a different inflate, not a different caller. This is now the
strongest argument in the repo for two things already in the plan:

- **the on-disk thumbnail cache** — not for streaming bandwidth, which was always the weaker
  case, but so this cost is paid once ever rather than once per boot;
- **`tools/mkthumbs.py`**, the host escape hatch, which turns 50-160 seconds of VR4300 inflate
  into a few seconds of desktop CPU.

Both need hardware to develop, since ares' DFS is read-only. Parked, but no longer optional:
without the cache the menu spends its first minute unable to show what it is for.

## 1f. The real art corpus is not the asset spec

`n64-tools/n64-flashcart-menu-metadata` is what people will actually put on their cards.
Unlicense, 1,672 PNGs, **1.77 GB**, mean 1 MB per file, 711 games with a `boxart_front.png`.
Nothing from it is committed; `tools/getart.py` fetches a bounded, gitignored subset into
`build/artcache/`.

Measured over a 120-card sample stratified across the size distribution:

| dimensions | count | aspect |
|---|---|---|
| 158 × 112 | 34 | 1.411 (legacy stock-menu thumbnails) |
| **112 × 158** | **31** | **0.709 — portrait Japanese box scans** |
| 680 × 498 | 27 | 1.365 |
| 1020 × 747 | 9 | 1.365 |
| ~1000 × 690 | 5 | ~1.44 |
| 129 × 112 | 6 | 1.152 |

**74 of 119 are more than 0.05 off the 1.4286 the spec asks authors for, and a quarter are
portrait.** The spec's asset rules are a request to authors; the corpus is the reality.

Two consequences, both of which invalidated code that had already been written and tested:

1. **The thumbcache rejected anything that was not exactly 280 × 196.** Against this corpus it
   would have rejected all of it. The rejection was written deliberately — "refusing makes it a
   content problem the Settings screen can report" — and it was the wrong call because it was
   made without looking at what the content is.
2. **A decode that allocates a surface the size of the file needs 1.5 MB of intermediate for a
   1020 × 747 scan** and throws almost all of it away.

`png_decoder_start_scaled()` now box-filters each row straight into the 140 × 98 destination,
holding only that plus one accumulator row — 27 KB instead of 1.5 MB — and applies the spec's
cover-and-crop: scale to cover, never letterbox or squash, horizontal crop centred, vertical
crop anchored 40 % from the top so a portrait scan keeps its logo. Verified on screen against a
real 1000 × 696 landscape scan and a real 112 × 158 portrait card in the same frame.

Sampling note worth keeping: the first fetch took the *smallest* 40 files and returned nothing
but legacy 158 × 112 thumbnails. That corpus would have reported the menu handling real art
perfectly while never once exercising a 1000 px scan or a portrait card. `getart.py` now
stratifies across the size distribution.

### Decode cost went up, and the case for the disk cache changed

| | measured |
|---|---|
| Decode, spec-sized 280 × 196 only | 86,700 µs |
| Decode, mixed real corpus | **~155,000 µs** |

The plan justified CI8 and an on-disk atlas on **streaming bandwidth**. That argument was
always weak for a RAM-only cache and it is not the important one. The real argument is
**never paying a 155 ms decode twice**: a cold screen of twelve tiles is several seconds of
work on any hardware, and it recurs on every boot until the results are persisted. Persistence
is the point; CI8 is how it is made cheap to read back.

This cannot be developed against ares, where the DFS is read-only and no cache file can be
written at all. It needs hardware.

### The decode budget was guessed, and the guess was 5x wrong

> **Superseded by section 1g.** The budget is not tunable at all: one decoded PNG row costs
> 5,000-20,000 us, so a budget that can only stop between rows cannot bound anything. Sweeping it
> from 0 to 12,000 us moves the frame rate by less than one fps. The numbers below stand as
> measured; the conclusion drawn from them does not.

`grid_background()` originally spent 2,000 µs per frame. Against a 155 ms decode that is
**1.5 s of wall clock per tile** and nineteen seconds to fill a screen. Now adaptive: 6,000 µs
when the cursor has been still for 15 frames, 1,200 µs while scrolling, on the grounds that
frame rate matters more than art while tiles are going past too fast to read.

Still not fast enough to be comfortable — a cold screen takes several seconds — which is
another way of saying the same thing as the paragraph above.

### Two more harness bugs, both caught by hashes rather than by looking

1. **Priority inversion in the decoder.** It walked the library from index 0, so scrolling to
   the end of a library meant waiting for every earlier decode. It now serves the records the
   screen asked for this frame first and only prefetches in library order when nothing visible
   is outstanding.
2. **`inputscript_generated.h` survived a script change.** The config stamp deleted the object
   trees but not the generated header, and make judged it up to date because its timestamp was
   newer than the newly-named script. Two *different* input scripts therefore produced
   byte-identical hashes — which is what gave it away, since that is not a thing two different
   programs do. Added to the stamp's cleanup list.

---

## 1e. M3 — art on the tiles, and the estimate that was wrong by an order of magnitude

The grid now decodes the 280 × 196 title cards into a 20-slot resident pool and blits them.
Measured under ares, with the fixture library:

| | measured |
|---|---|
| PNG decode + downscale, per card | **86,700 µs** |
| Library scan, per ROM | **20,700 µs** |
| Resident pool | 20 slots, holding at 20 as designed |
| Frame interval while decoding | ~~18,000–19,600 µs (~52 fps)~~ — sampled 1 frame in 60; see §1g |

**The first-run cost estimate in the plan was wrong by roughly 10×, in our favour.** It assumed
0.3–1.5 s per image and concluded 500 titles would take 3–12 minutes. At 86.7 ms the real
figure is **~43 s of art plus ~10 s of scanning: under a minute.** That was an estimate stated
as an estimate and it has now been replaced by a measurement, which is the point.

Three consequences:

1. The elaborate mitigations the plan proposed for a 12-minute first run — decode in
   current-tab display order first, promote it from nice-to-have to required above 600 ms —
   are **not needed**. Plain library order is fine.
2. **DESIGN.md §5.2 resolves in favour of the lazy LRU.** At 86.7 ms, populating a large-art
   slot when the detail sheet opens is a sub-100 ms cost against a 200 ms open animation, so it
   can be decoded during the rise. A second 27.7 MB on-disk cache buys nothing.
3. The CI8 quantizer is not on the critical path for correctness, only for on-disk streaming.

Caveat on transferability: the decode is CPU-bound and the figure is in *emulated* VR4300
cycles, so it should carry to hardware better than any I/O number here. The scan figure should
not — it reads through the DFS, which is faster than FatFs over SC64.

~~**52 fps while decoding is not 60.** The budget is 2,000 µs per frame in `grid_background()`,
chosen by opinion, not measurement. It is the first thing to tune once the frame-time
instrumentation lands.~~

**Superseded by §1g, and wrong twice over.** 52 fps was a mean over a bimodal distribution
sampled one frame in sixty; the real behaviour is 60 fps and 43 fps in alternation, and 52 is a
rate no frame ever ran at. The budget was also not tunable — one decoded row costs up to
20,634 µs, so it can only stop between units of work larger than the frame itself.

### A time-budgeted job is still reproducible

Worth recording because the opposite is the intuitive expectation: adding time-budgeted
background decoding did **not** break the hash gate. `TICKS_READ()` is the emulated COP0
counter, which advances with emulated cycles rather than host wall-clock, so "spend 2 ms
decoding" resolves to the same instruction count on every run regardless of host load. Two runs
remain byte-identical across all six frames.

The corollary matters: any harness budget must be expressed in emulated ticks. A budget taken
from a host clock would silently make every run different.

---

## 1d. M2 — the presentation layer is replaced

The `view_t { init, show }` table, all 23 views, `ui_components/`, `menu_state.h`, `actions.c`
and the ED64 / 64drive drivers are gone. `screen_t { enter, leave, update(dt), render,
background }` and a themed grid replace them.

| | M0 baseline | M2 | delta |
|---|---|---|---|
| `.text` | 569,112 | 392,952 | **−176,160** |
| `.data` | 124,660 | 88,556 | −36,104 |
| `.bss` | 57,456 | 51,944 | −5,512 |
| ELF total | 751,228 | 533,452 | **−217,776 (−29%)** |
| first-party source | ~21,000 lines | 11,829 | −44% |

Reproducibility gate: **PASS**, two runs byte-identical across six frames.

Library scan of the 40-stub fixture: **48 titles in 984,685 µs = 20,514 µs/ROM.** Extrapolated
(and stated as an extrapolation) that is ~10 s for a 500-title card. Each ROM costs one 4 KB
header read plus the ~450-entry database walk. This is measured through ares' DFS, which is
*faster* than FatFs over SC64, so treat 20.5 ms as a floor, not a prediction. Worth attacking
before it becomes a 500-title boot cost — the directory-signature cache in the plan exists
precisely so this runs once rather than every boot.

Deferred rather than ported, and recorded so nobody assumes they were forgotten:
`cart_load.c` and `usb_comm.c` still take a `menu_t`; `bookkeeping.c` is superseded by library
playstate; `disk_info.c`, `hdmi.c` and `cpakfs_utils.c` return with the screens that use them.
**There is no launch path in M2** — the grid browses, it does not boot.

`background.c` was deleted rather than moved. The spec has no background image: `bg` is a flat
full-frame colour plus an optional ambient wash. Its value was the cache-file pattern, which is
recorded in §1c and DESIGN.md rather than kept as dead code.

MP3 support was stripped from `sound.c` along with `mp3_player.c` and the minimp3 submodule
dependency — there is no music player in the spec.

### Two bugs found while bringing the loop up

1. **Ticking per loop iteration instead of per displayed frame.** The first cut of `app_run`
   updated every iteration and rendered whenever `display_try_get()` happened to return a
   buffer. Those look equivalent and are not: `display_try_get()` returns NULL while the RDP
   drains, so the loop spins and anything on a per-iteration clock runs at CPU speed. It made
   the input script's frame counter meaningless — a scripted run consumed every event in a few
   hundred microseconds and asked ares to exit before drawing anything, which presented as
   "the harness stopped producing frames". The loop now acquires the framebuffer first and one
   iteration is one frame; `background()` runs in the NULL case, which is exactly the window it
   was designed for.
2. **`make FLAGS=-DX` silently disables the harness.** A command-line variable *replaces* the
   makefile's value, so passing `FLAGS` also drops the `FLAGS += -DDEV_HARNESS` that
   `DEV_HARNESS=1` adds. The run produced no dumps and looked like a ROM crashing before the
   instrumentation. Knobs now get their own variables — `FBSCALE=2` for a 320 × 240 capture.

---

## 1c. The UI spec board is now the authority

`docs/design/` holds the Summer Cart UI spec board handoff. It supersedes the earlier working
geometry. [DESIGN.md](DESIGN.md) records the deltas; the substantive ones are the left margin
moving 22 → 16 to make room for the position bar, selection becoming a whole-pixel 152 × 106
rect rather than a scale factor, and `tile_dim` — a wash over every *unselected* tile — becoming
the primary selection signal. Tile size, gap, pitch and count are unchanged, so the memory and
streaming budget stands.

### Upstream's decoder rejects the spec's art size

The asset spec is **280 × 196**, twice the 140 × 98 tile, so the detail sheet can show it 1:1.
`boxart.c:118` passes `BOXART_WIDTH_MAX` / `BOXART_HEIGHT_MAX` = **158** to
`spng_set_image_limits`, so upstream refuses anything larger and the panel draws empty.

Moving `mkfixture.py` to the real spec therefore changed exactly one frame hash — 04, the detail
view — while **frames 00 through 03 stayed byte-identical**. That isolation is the harness
earning its keep: a one-line change to the asset size produced exactly one changed frame, and
the diff said which.

Not a defect to fix in place. The boxart path is replaced wholesale in M2/M4, and the new
thumbnail cache consumes 280 × 196 sources by design. Until then the fixture's detail view is
expected to show no art.

### Two geometry problems the spec has not resolved

1. **A selected column-3 tile collides with the position bar.** Column 3 is x 472–612; selected
   it becomes 466–618, and the shadow (+4 x) spans 470–622. The bar is at 616–622. A quarter of
   all selections land here. Proposal in DESIGN.md §5.1: move the bar to 618–624 and draw it
   last. Needs the designer's assent.
2. **The detail sheet wants 1:1 art the grid cache cannot supply.** The sheet-open animation is
   0.20 s; a PNG decode on a VR4300 is 0.3–1.5 s. Either a second CI8 slot per game at 280 × 196
   (55,392 B, 27.7 MB of SD for 500 games, roughly double the first-run build) or a lazily
   populated LRU of large slots. **Decide before M4** — it changes the cache format version.

---

### Two sources of nondeterminism the gate caught

Both were found by the reproducibility check failing on its first run, and both would
otherwise have made every future diff report phantom changes.

1. **The browser footer prints `ctime(menu->current_time)`**
   ([`browser.c:634`](../src/menu/views/browser.c#L634)), so every frame showing the clock
   hashed differently second to second. Frozen to a fixed `DEV_FROZEN_TIME` under
   `DEV_HARNESS`; the RTC is not what is under test.
2. **`credits.o` is force-rebuilt every make** with `-DBUILD_TIMESTAMP=$(shell date)`
   (`Makefile:132`), so the credits screen changed on every build. Pinned to a fixed string
   under `DEV_HARNESS`.

Release builds keep the real clock and the real timestamp. Frame 04 was stable from the very
first run precisely because the detail view shows neither.

### Harness trap: `emux_detect(0)` reports absent on a working host

The detection helper reported `EMUX ABSENT` while `emux_hexdump` and `emux_ioctl_exit`
demonstrably worked in the same run — a detector failing closed on a functioning system, which
is worse than no detector.

The cause is a misread of the subcode, not an ares defect. ares' `XDETECT`
(`ares/n64/cpu/emux.cpp:13`) returns `detect.bit(0x00,0x1F)` for subcode 0 and
`detect.bit(0x20,0x3F)` for subcode 1. Every opcode it implements — XLOG 0x25, XHEXDUMP 0x27,
XIOCTL 0x2C — lives in the upper range, so **subcode 0 is empty on every host that exists**.
libdragon's `EMUX_FEAT1_*` constants are named for that second bitmask: `EMUX_FEAT1_HEXDUMP`
is `1 << 0x7`, which is `0x27 - 0x20`. Calling `emux_detect(1)` matches exactly.

### libdragon already ships EMUX, including a profiler

An earlier revision of `src/dev/debug_emux.h` hand-rolled the EMUX instruction encoding and
collided at compile time with `<emux.h>`, which libdragon installs. Use libdragon's:
`emux_hexdump()`, `emux_ioctl_exit()`, `emux_detect()`, `emux_log()`.

Worth knowing for the frame-time work: it also exposes `emux_prof_start/stop/read` with
`EMUX_PROF_CYCLES`, `EMUX_PROF_ICACHE_MISSES`, `EMUX_PROF_DCACHE_MISSES`,
`EMUX_PROF_RSP_CYCLES`, `EMUX_PROF_RSP_STALLS`, and RDRAM byte counters broken out per
initiator (CPU, RSP DMA, PI DMA, RDP draw, VI). That is a better instrument than COP0 cycle
counting alone for the resolution and interlace decision — **but it measures ares, not an M64
or a real N64**, so it ranks builds against each other rather than predicting hardware.

---

## 2. Findings

### 2.1 The two-prefix toolchain split silently links the wrong libdragon

`~/n64inst` holds a libdragon predating this codebase. Verified absent from its headers:
`display_set_fps_limit`, `INTERLACE_HALF`, `vi_set_timing_preset`, `cpakfs_*`. So
`menu.c:93-113` alone does not compile against it.

The obvious fix — install the pinned libdragon to a second prefix and set
`N64_INST=~/n64inst-preview N64_GCCPREFIX=~/n64inst` — **compiles but does not link**:

```
undefined reference to `cpakfs_unmount'   (and rtc_get_source, settimeofday, debugf, ...)
```

All of those symbols *are* defined in `~/n64inst-preview/mips64-elf/lib/libdragon.a`. The cause is
that `mips64-elf-gcc -print-search-dirs` lists
`~/n64inst/lib/gcc/mips64-elf/14.4.0/../../../../mips64-elf/lib` — the *old* prefix's sysroot —
and n64.mk passes its own path as `-Wl,-L…`, which the driver places **after** its built-in `-L`
entries. The stale 3.6 MB `libdragon.a` wins.

The trap here is that the failure looks like "the pinned libdragon is too old", which is the
opposite of true, and would send you upgrading the submodule.

**Resolution:** `~/n64inst-preview` is a full copy of `~/n64inst` (237 MB) with the pinned
libdragon installed over it. Self-contained, so only `N64_INST` is set and `N64_GCCPREFIX` is
never used. `~/n64inst` is untouched, so lithium64's memory map — whose 32 KB IPL3 reservation
its own audit calls load-bearing — cannot be disturbed by work in this repo.

### ~~2.2 The cheat pipeline can pair a conditional with an unrelated code~~ — fixed, see 1i

`cheats_get_next` ([`src/boot/cheats.c:147`](../src/boot/cheats.c#L147)) consumes **two
consecutive entries** whenever the type is `0x50` (repeater) or `0xD_` (conditional):

```c
for (int i = 0; i < 2; i++) {
    ...
    if (!IS_DOUBLE_ENTRY(c->type)) break;
    c = &cheat->sub;
}
```

But `generate_enabled_cheats_array`
([`src/menu/datel_codes.c:29`](../src/menu/datel_codes.c#L29)) emits one `{address, value}` pair
per independently-`enabled` line. Disable one half of a `D0…`/`80…` pair in the editor and the
engine pairs the surviving conditional with whatever code happens to follow, then patches memory
at an address neither code names. Silent; no diagnostic.

**Consequence for the design:** cheats must be toggled as **named groups**, never per line. A
group is emitted whole or not at all. Line-level toggling is not offered, including in the
advanced hex view.

### ~~2.3 `cheats_install` has no bound on the engine buffer~~ — fixed, see 1i

Nothing checks `engine_p` against the end of the engine region. A `0x50` repeater carries an
8-bit count ([`cheats.c:316`](../src/boot/cheats.c#L316)) and emits `3 × count` instructions, so
a handful of repeaters with large counts write past the end into whatever follows.

Budget: `DEFAULT_ENGINE_ADDRESS` 0x807C5C00 to the 32 KB libdragon reserves at the top of RDRAM
(0x807F8000) = 0x32400 = **205,824 B = 51,456 instructions**. A plain write is 3 instructions, a
conditional-plus-write 7; a single `0x50` with count 255 is 765.

**Fix:** add `CHEATS_ENGINE_MAX_WORDS` and return `false` on overflow. Required regardless of what
`MAX_CHEAT_CODES` is raised to.

### 2.4 The file list allocates a text layout every frame

[`file_list.c:95`](../src/menu/ui_components/file_list.c#L95) mallocs
`sizeof(rdpq_paragraph_t) + sizeof(rdpq_paragraph_char_t) * total_length` **per frame**, then
builds two full paragraphs and frees them. Same pattern in `context_menu.c:120` and
`common.c:230`. At the 30 fps cap that is ~60 layout allocations per second, forever.

Not yet measured in µs — do that before claiming a win from removing it.

### 2.5 License is AGPL-3.0

`LICENSE.md:1`. Rules out aggregating a GPLv2-only cheat corpus (notably mupen64plus'
`mupencheat.txt`) into the build. See the cheat-database plan for the MIT alternative.

---

## 3. Verified environment facts

- **ares is already configured.** `~/Library/Application Support/ares/settings.bml` has
  `HomebrewMode: true` (line 57) and `ExpansionPak: true` (line 79). `run.sh` should still assert
  both — a silently-4 MB run produces a fault screen and wastes an afternoon.
- **The `rom:/` fixture seam works.** Under ares `cart_type == CART_NULL`, `flashcart_init` falls
  to `default:` ([`flashcart.c:179`](../src/flashcart/flashcart.c#L179)) and sets
  `storage_prefix = "rom:/"`. Confirmed empirically in the M0 smoke boot, which logged
  `ini_load(rom:/menu/config.ini): file not found` — so a DFS tree mirroring the SD layout is read
  by unmodified production code. Writes fail (DFS is read-only), which the menu already tolerates.
- **`debugf()` reaches ares' stdout for free.** `debug_init_isviewer()` is called on the emulator
  path and ISViewer output appears in the ares log. The EMUX macros are still needed for
  framebuffer dumps and self-termination, but plain logging needs no harness at all.
- **The M64 has a built-in Expansion Pak and Controller Pak**, so `is_memory_expanded()` is always
  true and the 4 MB budget upstream is contorted around does not apply. Vendor claim, not measured
  by us — keep the runtime check and fault loudly if it ever fails.
- **Our telemetry reaches hardware unchanged, and the hash gate does not.** `flashcart_init` calls
  `debug_init_usblog()` under `#ifndef NDEBUG`, and **nothing in our Makefile ever defines
  `NDEBUG`** — checked, not assumed — so it is live in a release build. `sc64deployer debug`
  speaks the UNFLoader protocol, so every `debugf` line (`LIBRARY scanned`, `FRAME`, `HEAP`,
  `emu:`) arrives on the PC with nothing new written. `DBG_FBDUMP` does not: it rides EMUX
  `XHEXDUMP`, an ares extension, so **frame hashing is ares-only, permanently**.
- **SC64 firmware 2.20.2 (18 Nov 2024) is the latest release** and satisfies our driver, which
  demands major == 2 and minor ≥ 17 ([`sc64.c:28`](../src/flashcart/sc64/sc64.c#L28)) and reports
  a miss as `FLASHCART_ERR_OUTDATED` — indistinguishable, on a screen that has not drawn yet, from
  not booting at all. Verified against the GitHub releases API, not from memory.
- **The SC64 firmware repo is not useful for emulation.** It is FPGA gateware, controller firmware
  and the deployer; it contains no emulator and nothing that helps NES/SNES/GB support, which come
  entirely from the cores in `/menu/emulators`. What it does contain is
  `sc64deployer upload --direct`, which disables the SC64 bootloader during boot and reset, and
  whose documented use is **testing a custom IPL3** — §4 question 1 exactly. Running an upload
  both ways separates "the SC64's CIC emulation and the M64's PIF disagree" from "the console will
  not run a custom IPL3 at all".

---

## 4. Open questions

Unverified. No M64 hardware or documentation has been consulted for this repo.

1. **Does libdragon's custom IPL3 boot on the M64 at all?** A custom IPL3's checksum matches no
   stock CIC seed, so booting depends on the flashcart's CIC emulation agreeing with the console's
   PIF. lithium64's audit flags this as unverified and it applies unchanged here, because this
   menu *is* a libdragon ROM. **Recommended first hardware test: a stock libdragon hello-world on
   the M64 via SC64.** If that fails, everything in this repo is blocked and the fault is upstream
   of any code we write. Costs about an hour.
2. **Does the M64's VI accept 640×480 progressive?** On an FPGA with an HDMI scaler it is
   plausible; on a real N64 it is not. Establish in ares first, then on hardware.
3. **Does the M64 crop overscan?** DESIGN.md assumes a 16 px safe inset. Test-pattern it.
4. **Does the M64's RDP match the real RDP for the scaled texture-rectangle path?** The grid leans
   on it for every tile.
5. **Does the built-in Controller Pak present as a standard joybus mempak?**

---

## 5. Known harness limits

- **ares has no SD card.** The fixture is read through DFS/PI, which is faster and
  lower-latency than FatFs over SC64. **The thumbnail streaming budget is therefore the one number
  ares cannot validate.** Plan: an artificial `--sd-delay-us` knob under `DEV_HARNESS` to test
  against a pessimistic 1.5 MB/s, and a real measurement on hardware.
- **ares is not cycle-accurate for RDP/RDRAM contention**, so `rdp_us` from the frame-time
  instrumentation is indicative, not predictive.
