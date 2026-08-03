# MainMenu

A game launcher for the **ModRetro M64** with a SummerCart64, developed against **ares**.
Forked from `Polprzewodnikowy/N64FlashcartMenu` @ `6407ab15`: we keep its boot and flashcart
plumbing and replace the entire presentation layer with a box-art grid.

Read [`docs/AUDIT.md`](docs/AUDIT.md) before changing anything. Layout numbers live in
[`docs/DESIGN.md`](docs/DESIGN.md).

## Corpora (fetched, never committed)

```sh
tools/getart.py --count 40                 # box art -> build/artcache
tools/mkcheatdb.py --fetch                 # libretro cheats -> build/cheats.db
tools/mkcheatkeys.py -i build/cht          # -> tools/data/n64_keys.tsv (this one IS committed)
```

`build/` is gitignored in full. The art repo is 1.77 GB and the cheat corpus is someone else's
work; neither belongs in this history. `n64_keys.tsv` is committed because it is a table of pure
facts -- a filename and three header fields -- and regenerating it needs both corpora.

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
src/ui/           draw, input, theme, tween
src/library/      library (index + scan), thumbcache
src/cheats/       cheatdb -- read-only, group model; see cheatdb.h on why groups not lines
src/screens/      grid, detail, cheats, settings, launch, fault
src/dev/          harness only; compiles to nothing without DEV_HARNESS
tools/            fixture, input scripts, regression, art + cheat corpus fetchers
```

## Constraints that are not negotiable

- **640 × 480, RGBA5551.** One resolution. 32 levels per channel — near-identical greys collapse.
- **No pointer.** N64 controller only. Nothing may require aiming.
- **`render()` does no I/O and no allocation.** Streaming happens in `background()`, which runs
  while the RDP drains.
- **Motion is specified in seconds, never frames.** The frame rate is still an open question.
- **Every cache file carries a magic and a format version**, and a mismatch deletes and rebuilds
  rather than trying to migrate. `src/cheats/cheatdb.c` is the reference implementation.
- **Nothing writes to the card yet.** Thumbnail cache, play history and cheat selections are all
  in-memory only, because ares' DFS is read-only and a write path that has never run is not a
  feature. These land with hardware.
- **Cheats are toggled as named groups, never per line.** A `D0` conditional and the write it
  guards are one indivisible thing; see AUDIT.md 2.2 for what per-line toggling did.
