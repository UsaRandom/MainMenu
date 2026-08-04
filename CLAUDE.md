# MainMenu

A game launcher for the **ModRetro M64** with a SummerCart64, developed against **ares**.
Forked from `Polprzewodnikowy/N64FlashcartMenu` @ `6407ab15`: we keep its boot and flashcart
plumbing and replace the entire presentation layer with a box-art grid.

Read [`docs/AUDIT.md`](docs/AUDIT.md) before changing anything. Layout numbers live in
[`docs/DESIGN.md`](docs/DESIGN.md).

## Corpora (never committed)

```sh
tools/mkcheatdb.py --fetch                 # libretro cheats -> build/cheats.db
tools/mkcheatkeys.py -i build/cht          # -> tools/data/n64_keys.tsv (this one IS committed)
```

`build/` is gitignored in full. The cheat corpus is someone else's work and does not belong in
this history. `n64_keys.tsv` is committed because it is a table of pure facts -- a filename and
three header fields.

**`tools/mkdemo.py` is not `tools/mkfixture.py`.** mkfixture harvests real game codes and titles
out of `rom_info.c` so a scan exercises the real database; mkdemo invents every title, code and
cheat and draws original box art, because its output goes in the README. `make DEMO=1` packs it
instead of the fixture — never measure against it, every title in it misses the database on
purpose. See AUDIT.md 1w.

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
  `+ 1` from `slot_offset()`, which 10 of the atlas suite's 31 checks catch. `libindex.c`,
  `playstate.c` and `cheatstate.c` are still verified only by compile-time size assertions. See
  AUDIT.md 1r for the hardware bring-up order and 1s for the pre-hardware bug scan.
- **Cheats are toggled as named groups, never per line.** A `D0` conditional and the write it
  guards are one indivisible thing; see AUDIT.md 2.2 for what per-line toggling did.
