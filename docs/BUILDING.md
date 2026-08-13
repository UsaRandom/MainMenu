# Building MainMenu

If you just want to play, you do not need this page. Get `sc64menu.n64` from a release and follow
[CARD.md](CARD.md).

Everything below is for compiling the menu yourself.

## Toolchain

```sh
export N64_INST="$HOME/n64inst-preview"     # NOT ~/n64inst — see AUDIT.md 2.1
git submodule update --init --recursive
make sc64 -j8                                # -> output/sc64menu.n64
```

The pinned libdragon (`5cb976a`) must be installed to a **separate prefix**. Installing it over a
shared `~/n64inst` silently changes the memory map for anything else built against it.

**Never set `N64_GCCPREFIX`.** Splitting the compiler prefix from the install prefix links a stale
`libdragon.a` and produces undefined references that look like the submodule being too old. That
one has cost an afternoon before; see AUDIT.md 2.1.

## The cheat corpus

Fetched, never committed — it is someone else's work and does not belong in this history.
`build/` is gitignored in full.

```sh
tools/mkcheatdb.py --fetch                  # -> build/cheats.db
tools/mkcheatkeys.py -i build/cht           # -> tools/data/n64_keys.tsv (this one IS committed)
```

`n64_keys.tsv` is committed because it is a table of pure facts: a filename and three header
fields. The corpus itself is not.

## Build knobs

| knob | what it does |
|---|---|
| `FIXTURE=1` | packs a synthetic SD tree into the DFS, so the menu has a library under ares |
| `DEV_HARNESS=1` | compiles `src/dev/` — debug opcodes, input scripts, frame timing |
| `INPUT_SCRIPT=` | the compiled input script to replay |
| `FBSCALE=n` | framebuffer dump scale; 4 by default, 1 for full 640 × 480 |
| `DEMO=1` | swaps the fixture for `mkdemo.py`'s tree of invented games |
| `SAMPLE=1` | swaps it for `mksample.py`'s full card of 115 games; `SAMPLE_MIX=true\|realistic\|hostile` |
| `PLAIN_ART=1` | builds the fixture from procedural cards instead of the real art corpus |
| `FIXTURE_DIR=` | use a fixture tree built elsewhere |

**Never pass `FLAGS=`** — it replaces the whole flag set rather than adding to it.

`CONFIG_STAMP` drops the object files whenever the configuration or the source list changes, so
switching `FIXTURE` or `DEV_HARNESS` cannot link objects from the other configuration. Without it
`make` is happy to do exactly that, because neither knob touches a timestamp.

## Fixtures, and which one to use

**`tools/mkfixture.py` is the one for measurements.** It harvests real game codes and titles out
of `rom_info.c`, so a scan exercises the real 450-game database and real save-type detection.
Every number in AUDIT.md was taken against it.

**`tools/mkdemo.py` is the one for pictures.** It invents every title, code and cheat and draws
original box art, because its output goes in the README. Every title in it misses the database on
purpose, so a scan measured against it measures the miss path. Never measure against it.

**`tools/mksample.py` is the one for layout.** 115 invented games with mkdemo's art, enough that
every tab is at least three full rows, and covers drawn at a *stated spread of aspects* rather
than all at the right one — because how well a tile shape can be read off its cover is a property
of art packs, not of code. `SAMPLE_MIX=true` is the control and the only one worth looking at for
"does this layout look good"; `realistic` and `hostile` are for finding out what a bad pack does
to it. Never measure a scan or a frame time against any of them: the mix is chosen to contain
failures, so anything measured against it measures the mix. See AUDIT.md 1ak.

**`build/artcache` has to be populated by hand** and nothing fetches it. Any tree of
`<GAMECODE>/boxart_front.png` works. Without it every fixture rebuild swaps real cards for
procedural gradients, which look fine and quietly change every decode number in AUDIT.md — while
`real-art.txt` and `jpeg-art.txt` go on passing against gradients. See AUDIT.md 1w.

**`tools/mksdmirror.py` mirrors a real card** into a tree small enough to pack into a ROM, by
truncating every ROM to its header. The menu only ever reads a 4 KB header at scan time, so this
changes nothing it can observe short of pressing A, and takes a 559 MB card down to about 2 MB.
It answers "what does the menu do with *my* card", which no fixture can.

## Running and regressing

```sh
tools/run.sh --script scroll-stress          # build FIXTURE=1 and launch ares
tools/regress.sh -o build/after tools/inputs/*.txt
diff build/before/hashes.txt build/after/hashes.txt
```

`regress.sh` builds the ROM itself; `-m 'VAR=VAL'` appends make variables. Input scripts are keyed
on **frame number, never elapsed time**, so the same run visits the same states however fast ares
happened to be going.

A run under a script uses a **fixed `dt` of 1/60**, so the whole run is a pure function of the
script. Before that, unrelated code changes moved the hashes — see AUDIT.md 1z.

**A hash suite is only evidence for the pixels it renders.** "Nothing moved" after a UI change is
a prompt to check the change is on screen somewhere, not a result. See AUDIT.md 2b for the time
that went wrong.

## Reproducing the README screenshots

```sh
tools/regress.sh -m 'DEMO=1 FBSCALE=1' -o build/demo-shots \
    tools/inputs/manual/demo-stills.txt
# -> build/demo-shots/demo-stills/frame*.png
```

None of them is a screen capture. The frames come out of `dbg_fbdump`, which hexdumps the
framebuffer through an ares debug opcode, so what lands on disk is what the RDP produced rather
than what a window manager made of it. `FBSCALE=1` is what makes them full 640 × 480; the default
of 4 exists because at full size a frame is 3.54 MB of hex.

## House style

In [`CLAUDE.md`](../CLAUDE.md), and it is short: measure rather than assert, check that a test can
fail before trusting a green one, and record negative results in AUDIT.md permanently.
