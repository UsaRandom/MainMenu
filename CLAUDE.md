# MainMenu

A game launcher for the **ModRetro M64** with a SummerCart64, developed against **ares**.
Forked from `Polprzewodnikowy/N64FlashcartMenu` @ `6407ab15`: we keep its boot and flashcart
plumbing and replace the entire presentation layer with a box-art grid.

Read [`docs/AUDIT.md`](docs/AUDIT.md) before changing anything. Layout numbers live in
[`docs/DESIGN.md`](docs/DESIGN.md).

## Corpora

The cheat corpus is not committed and is refetched:

```sh
tools/mkcheatdb.py --fetch                 # libretro cheats -> build/cheats.db
tools/mkcheatkeys.py -i build/cht          # -> tools/data/n64_keys.tsv (this one IS committed)
```

**The icon corpus IS committed.** `ICON_DIR ?= assets/icons`, 3,894 CC BY 3.0 SVGs from
game-icons.net by 36 authors, 6.87 MB of source text. This reverses an earlier ruling that it must
never be vendored; that ruling is struck through in `docs/GOTCHAS-PROFILES.md` and
`docs/NEXT-PROFILES.md` rather than deleted. A clone builds the same ROM this tree does.

The 286 icons excluded by `tools/ip-blocklist.txt` were **never copied in** — they are absent from
this repository, not filtered out of it on the way past. So `make ICON_EXCLUDE=` no longer packs
4,180 and nothing can; `ICON_EXCLUDE` defaults to empty against the vendored tree and to the
blocklist against any other `ICON_DIR`. `tools/iconcheck.py`, in the host suite, is what keeps the
tree and the blocklist from drifting now that no build re-applies them.

Attribution is a separate obligation from the exclusions. It is discharged twice: in
`assets/icons/README.md` beside the artwork, and in `docs/CREDITS.md`, which `tools/mkcredits.py`
bakes into the cartridge and the credits screen renders. Both name all 36 authors regardless of
how many icons a build packs. `iconcheck.py` fails if an author directory appears in neither.

`build/` is gitignored in full. The cheat corpus is someone else's work and does not belong in
this history. `n64_keys.tsv` is committed because it is a table of pure facts -- a filename and
three header fields.

**There are three trees, and `tools/mkdemo.py` is not `tools/mkfixture.py`.** mkfixture harvests real game codes and titles
out of `rom_info.c` so a scan exercises the real database; mkdemo invents every title, code and
cheat and draws original box art, because its output goes in the README. `make DEMO=1` packs it
instead of the fixture — never measure against it, every title in it misses the database on
purpose. See AUDIT.md 1w.

**`tools/mksample.py` is the third.** 115 invented games with mkdemo's art, every tab at least
three full rows, and covers drawn at a *stated spread of aspects* rather than all at the right
one. `make SAMPLE=1`, with `SAMPLE_MIX=true|realistic|hostile`. For looking at layout and for
sensitivity analysis; never for measurement, because the mix is chosen to contain failures. See
AUDIT.md 1ak.

**`build/artcache` has to be populated by hand** and nothing fetches it. Any tree of
`<GAMECODE>/boxart_front.png` works. Without it every fixture rebuild swaps real cards for
procedural gradients, which look fine and change every decode number in AUDIT.md -- and
`real-art.txt` and `jpeg-art.txt` go on passing while testing gradients. See the Makefile comment
above `ARTCACHE_DIR`.

## Build

```sh
export N64_INST="$HOME/n64inst-preview"     # NOT ~/n64inst — see AUDIT.md §2.1
make sc64 -j8                               # -> output/sc64menu.n64
```

`~/n64inst-preview` is self-contained: a copy of `~/n64inst` with the pinned libdragon
(`5cb976a`) installed over it. **Never set `N64_GCCPREFIX`** — splitting the compiler prefix
from the install prefix links a stale `libdragon.a` and produces undefined references that look
like the submodule being too old. **Never install into `~/n64inst`** — lithium64 depends on
its exact libdragon, including the IPL3's 32 KB RDRAM reservation.

## House style

**Measure, don't assert.** Any claim about speed, size or memory carries a number and says how it
was obtained. "This should be faster" is not a result. If a script could measure it, do not
assert it instead.

**Check that a test can fail.** Before trusting a green result, establish that the setup is
capable of producing a red one. A harness that silently measures the wrong thing produces
convincing fake numbers — two of those are already recorded in AUDIT.md.

**Record negative results.** Ruled-out hypotheses, dead ends and harness traps go into AUDIT.md
and stay there. Items are struck through when fixed, never deleted. Superseded measurements are
marked superseded rather than replaced.

**Comments explain why, including what went wrong before.** A comment that restates the code is
noise. A comment naming the bug the code now prevents is worth ten of them.

**Never run the regression suite unless asked.** `tools/regress.sh` and `tools/suite.sh` rebuild
the ROM once per input script and run each under ares; the full matrix is many minutes of machine
time and it interrupts whatever else is being looked at. Run individual scripts when a change
plausibly moved their frames, and say which ones were run and which were not. `tools/hosttest/run.sh`
is not covered by this — it is a few seconds and needs no emulator.

**Unlanded work stays a patch, not a commit.** If a change measures well but regresses something
else, keep the patch and the reasoning in `docs/`, not in history.

## Commits

Imperative, plain English, describing the **effect** rather than the mechanism. No conventional-
commits prefixes, no ticket IDs, no emoji. A rationale clause after a comma is idiomatic:

```
Give every thumbnail slot a palette of its own
Rank the scroll against a pessimistic SD, which changes what the budget can be
Stop the tile layout from depending on what sits above it
Record what today's measurement ruled out
```

Verb conventions: `Record …` for docs-only changes, `Add a way to …` for tooling, `Correct …`
for fixing a previous wrong *claim*.

Bodies are prose paragraphs, never bullet lists, and always carry numbers — the mechanism, the
measurement before and after, the cost, and any residual distrust.

## Layout

```
src/boot/         bootloader, CIC, Datel cheat engine     — keep verbatim
src/flashcart/    SC64 driver behind a vtable             — keep verbatim
src/menu/         rom_info (450-game DB), cart_load, ini_parser, path, settings
src/app.c         main loop, app_t, screen_t
src/ui/           draw, input, theme, tween, icon (svg64 cache)
src/library/      library (index + scan), thumbcache
src/cheats/       cheatdb -- read-only, group model; see cheatdb.h on why groups not lines
src/screens/      grid, detail, cheats, settings, launch, fault, profiles, appearance,
                  keyboard, credits
src/libs/svg64/   SVG rasteriser, vendored MIT; the icon corpus is NOT vendored
src/dev/          harness only; compiles to nothing without DEV_HARNESS
tools/            fixture, demo tree, input scripts, regression, video, cheat corpus fetcher
```

## Constraints that are not negotiable

- **640 × 480, RGBA5551.** One resolution. 32 levels per channel — near-identical greys collapse.
- **No pointer.** N64 controller only. Nothing may require aiming.
- **`render()` does no I/O and no allocation.** Streaming happens in `background()`, which runs
  while the RDP drains.
- **Motion is specified in seconds, never frames.** The frame rate is still an open question.
- **Every cache file carries a magic and a format version**, and a mismatch deletes and rebuilds
  rather than trying to migrate. `src/cheats/cheatdb.c` is the reference implementation.
- **Every write is soft.** Settings, `library.idx`, `playstate.dat`, `cheatstate.dat` and
  `thumbs.pak`/`.idx` all persist through `src/library/cache.c`, which decides once at boot whether
  storage is writable and short-circuits every writer if it is not. Under ares it never is — the
  DFS is read-only — so the menu must behave *identically* to the way it did before any of this
  existed, only slower. Nothing in this layer may ever become load-bearing.
- **The write half is unexecuted on real storage.** ares cannot reach it. `tools/hosttest/run.sh`
  compiles the real `cache.c` and `thumbstore.c` natively and round-trips them against actual
  files, and `--mutate` proves both suites can go red — breaking the CRC seed, and dropping the
  `+ 1` from `slot_offset()`, which 10 of the atlas suite's 31 checks catch. `libindex.c` joined
  them at 446 checks — round trip, the staleness matrix, and a repaired index compared record by
  record against a full scan of the same tree. `playstate.c` and `cheatstate.c` are still verified
  only by compile-time size assertions. See AUDIT.md 1r for the hardware bring-up order, 1s for
  the pre-hardware bug scan and 1ay for what the index test had to exist before.
- **Cheats are toggled as named groups, never per line.** A `D0` conditional and the write it
  guards are one indivisible thing; see AUDIT.md 2.2 for what per-line toggling did.
- **Three of the five fonts carry an 84-glyph charset.** A character outside it draws as nothing,
  silently, on a console -- a full-charset bake at 40 px would be about 2.7 MB against 681 KB at
  20 px. `tools/charsetcheck.py` turns that into a build failure and is run by the host suite; it
  sees string literals, not data. See GOTCHAS-PROFILES.md section 1.
- **Profile slots never move.** `profiles.ini` v2 gives every slot a `used` flag. The slot number
  names `saves/pN/`, so closing a gap on delete handed one player's saves to another -- which is
  what v1 did. Deleting a profile deletes its saves, and profile 1 is refused at both the UI and
  the function.
- **`icon_get()` is keyed on (index, ink, paper).** The same icon in two colours is two cached
  entries. A screen that requests one pair and draws another misses on every cell forever, with
  nothing in any log to say why.
