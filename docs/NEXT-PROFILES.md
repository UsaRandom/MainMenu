# Next batch: icons, profiles, credits

Notes taken 6 Aug 2026, not yet worked. Scope is three things the user asked for in one sitting:
a credits/licence screen, a settings list that can outgrow one page, and the profile rework onto
a top-right player chip with the `Who's playing?` grid behind it — drawn from
`Summer Cart UI Spec Board.zip` (`design_handoff_profiles/`), and rendered with
[svg64](../../svg64) for the icon corpus.

Nothing below is decided by measurement yet. Every number quoted is either measured today or
labelled as the thing that has to be measured.

**Baseline to beat.** `output/sc64menu.n64` is 1,638,400 bytes today; `.text` 571,320,
`.data` 108,196, `.bss` 75,944. `src/screens/` is 5,106 lines. Record the delta at every gate.

---

## 0. Decisions to take before any code is written

Nine of them. Each is a real conflict between the spec, the existing code and the corpus — not a
preference. Recommendations given, because the work stalls on all nine otherwise.

### 0.1 L and R already page the tabs — collision

`screen_grid.c:513,519` binds L and R to previous/next tab. The request is that L and R reach the
player chip in the top right. Both cannot be literally true.

**Recommendation: the chip is the last stop on the tab rail.** R past the final tab lands the
focus on the chip; L off the chip returns to the last tab. One rail, one axis, and the chip is
reachable in at most `TAB_COUNT` presses from anywhere. No button is rebound, no muscle memory is
broken, and the "navigatable to by L/R" requirement is met exactly. A on the chip opens P1.

Rejected: rebinding tabs to C-left/C-right (C-right is already Fav, `screen_grid.c:556`), and a
dedicated modifier (nothing left on the pad that is not already a hint in the footer).

### 0.2 Deleting a slot — decided: stable slots, and the delete takes the saves with it

Spec §3: *"Deleting a slot clears the struct; it does not compact the array. Slot order is stable
forever."*

`profile.h` documents the opposite, and gives its reason: profile *n* owns `saves/pN/`, so
deleting player 2 promotes player 3's directory into player 2's slot. `screen_profiles.c`'s
delete confirmation says this on screen, in words, because it is surprising.

**Decided: adopt the spec's stable slots, and delete the profile's saves with it, warned.** That
resolves the promotion problem at the root rather than working around it — nothing is left behind
to be inherited by the next occupant of the slot. The confirmation stops saying "the saves stay
on the card" and starts saying the opposite, plainly.

Still required:

- **`profiles.ini` gains a version key.** Absent = the 0.2 compacted layout, read as-is and
  rewritten with the key, no renumbering. Profile 1 stays index 0 on unsuffixed paths, which is
  what makes an old card boot unchanged.
- Delete clears in place; the grid draws the hole as `+ Empty`.
- The delete removes **three** things, not one: `saves/pN/`, `cache/pN/playstate.dat`,
  `cache/pN/cheatstate.dat`.
- **Profile 1 is a special case and it is dangerous** — see GOTCHAS §5.1. Its saves are the
  unsuffixed `saves/`, which on a card that predates profiles is *everyone's* saves.

### 0.3 The spec's atlas is not how svg64 works

Spec §4 describes a **10 × 10 1-bit** sprite, paged 45 to a 320 × 200 I4 page, one page resident.
`PIcon.dc.html`'s 16 bitmaps are stand-ins the board renders so it can be opened in a browser —
they are not the shipping artwork. The shipping artwork is svg64's corpus, rasterised on the
console from SVG text.

So §4's *sizes* and *counts* survive and its *storage model* does not:

- ≈4,000 sprites → **3,894** packed (4,180 in the corpus, 286 excluded by `tools/ip-blocklist.txt`).
- 40 px and 60 px, whole-pixel only → svg64 fits `viewBox` to the destination surface at any size,
  so 40 and 60 are just two surface sizes. The ×3/×4 floor argument is about a 10 px bitmap and
  does not apply; **40 px stays the floor anyway**, because it is what the layout was drawn to.
- "one page resident, no full-set decode ever" → becomes svg64's direct-mapped render cache.

**The cost that replaces the page decode.** svg64 measures **4.8 ms per 40 × 40 icon** in ares
(`svg64/docs/PERF.md`), worst case 31.4 ms on one pathological file out of 4,204. A 45-cell page
is therefore ≈216 ms if filled in one frame, which it must not be. The demo already solves this:
**at most three new icons rasterised per frame**, so a fresh page fills over ~15 frames (¼ s) while
holding 60 VPS. Copy that policy exactly and put it in `background()`, which is where this project
already does streaming work.

### 0.4 Where the SVG text lives — decided: in the ROM

`icons.pack` measures **6,561,304 bytes** exactly (`svg64/build/fs/icons.pack`, 3,894 icons).
The ROM is 1,638,400 today, so the DFS build lands at **≈7.8 MB**.

**Decided: it goes in the DFS.** A cartridge is 64 MB and 8 is fine for what it buys. This is the
simpler system by some distance — no fourth file on the card to version, no absent-file path to
design, no `mksdmirror.py` step, and the fixture and the release build read through the same code.

Two consequences that follow from the decision rather than arguing against it:

- **ROM headroom is now a real budget, and type is what will spend it.** 7.8 MB is before any new
  font bake, and GOTCHAS §1 shows a full-charset 40 px face is ~2.7 MB on its own. Every font
  decision in this batch is a ROM decision. Record the size at every gate.
- **SC64 loads the whole ROM from SD at boot**, so boot time scales with it. Measure it —
  GOTCHAS §2.3. It is the one cost the user actually feels and it is not in any budget above.

The read cost still has to be measured (T3), just from the cartridge rather than from FatFs.
svg64's 4.8 ms figure excludes the read that precedes it.

### 0.5 A built-in fallback set

With the pack in the ROM it cannot go missing, so the fallback is no longer a card-state problem.
It is still worth having for one reason: `make ICON_LIMIT=200` and `make FIXTURE=1` build packs
that do not contain whatever index a profile was saved with.

**A profile whose `icon` index is out of range must draw something, not nothing.** Clamp to a
built-in placeholder — one glyph, embedded in `.rodata`, no I/O — and leave the stored index
untouched so a full build shows the real icon again. The failure is then visible and reversible
instead of silently rewriting the user's choice.

### 0.6 The 8 swatches are identity, not theme

Spec §4.1 fixes eight hex colours. `CLAUDE.md` says **no `#define` colour survives anywhere** —
every colour comes from `theme_t`.

These are the exception and should be stated as one: a profile's colour is how a person recognises
their own slot, and a theme change that repaints everybody's badge has changed who they are. Put
them in `profile.h` as a `static const` table with that sentence as the comment, and have
`theme_t` carry the *plate* and *text* colours around them, not the swatches themselves.

Also from §4: ink flips to `#101019` on the five light swatches and `#F7F7FF` on the three dark
ones. That is a per-swatch field in the same table, not a luma computation at draw time — the
spec has already decided each one and a computed threshold would disagree with it somewhere.

### 0.7 The position bar sits outside the safe area

Spec §6: *"Position bar 6 px wide at x=628."* `theme.h:57-60` puts the safe area at
`SAFE_X 16, SAFE_W 608` — so x ∈ [16, 624). A bar at 628..634 is **10 px past the right edge** and
will be cut off on a CRT.

Everything else in the spec lands inside the safe area exactly — P1's grid is 608 wide from x=16,
P3's field is 608 wide from x=16, the header sits at 16,16. The designer worked to the same box.
This one is an outlier.

**Recommendation: move the bar to x=616 (616..622), and shrink nothing.** The icon grid ends at
x=604, so there is 12 px of clearance. Flag it back to the designer rather than silently diverging.

### 0.8 The keyboard needs two charsets, not one

Spec P3 is QWERTY, letters and space, no digits — right for a name. The user also wants it used
for **cheat names**, and cheat names have digits and punctuation in them all over the corpus.

**Recommendation: one keyboard screen, a charset flag.** `KB_NAME` (A–Z, space; DELETE/SPACE/DONE)
and `KB_TEXT` (adds a digits row and a small punctuation set). Same layout engine, same cursor
rules, same footer. The name path gets exactly the spec's four rows; the cheat path gets five.

This retires **two** odometers, not one: `screen_profiles.c`'s and `screen_cheatedit.c`'s. Both
currently share the `ALPHABET` string and the same Left/Right/Up/Down winding, and
`screen_profiles.c`'s header argues at length that a keyboard is not worth building for a name
typed twice per household. That argument was correct when the keyboard served one screen; it is
not correct now that it serves three and the design exists. **Record that reversal in AUDIT.md
with the reason** — it is a decision being overturned, not a gap being filled.

### 0.9 What the licence screen is actually obliged to say

The repo is **AGPL-3.0** (`LICENSE.md:1`), and AGPL's source obligation is the one that binds
hardest: a distributed binary must carry an offer of source. That is a sentence and a URL, and it
has to be in the ROM, not only in the README.

`README.md:65-92` already enumerates the dependencies and is the source of truth to render from.
What is missing from the tree entirely:

- **No licence file accompanies any asset.** `assets/fonts/`, `assets/music/`, `assets/sounds/`
  hold artwork with no `LICENSE` beside it. **Firple is SIL OFL 1.1**, and OFL requires the licence
  text travel with the font. That is a gap to close in the repo, independent of the screen.
- **game-icons.net artwork is CC BY 3.0** — an attribution licence, and the ROM is the thing that
  redistributes. `svg64/docs/ICON-CREDITS.md` names all 36 authors and is the text to reproduce.
  Two directories (`seregacthtuf`, `various-artists`, 7 icons) postdate upstream's `license.txt`
  and are credited under their directory name; keep that, do not invent names.
- Attribution and the 286 IP exclusions are **unrelated obligations**. Dropping icons does not
  reduce the duty to credit the 3,894 shipped. Say so in the doc, not on screen.

---

## 1. svg64 integration — foundation, blocks §3

1. Vendor svg64 as `src/libs/svg64/` (3 `.c`, 1 public header, MIT, `+14,552` text / `525` bss).
   Sources unmodified; SPDX headers intact. Add to `SRCS` and to README's licence list.
2. Add `ICON_DIR ?= ../svgicons` and `ICON_EXCLUDE ?= tools/ip-blocklist.txt` to the Makefile,
   *(as built; both defaults have since changed — the corpus is vendored at `assets/icons` and
   `ICON_EXCLUDE` is empty against it. See §7.)*
   copying svg64's `Makefile:31,41,61-63` verbatim including the exclusions-before-`--limit`
   ordering. Copy `tools/mkpack.py`, `tools/ip-blocklist.txt`, `tools/icon-meta.jsonl`,
   `tools/metacheck.py`.
3. `make FIXTURE=1` packs `--limit 200` into the DFS. Release builds pack nothing: the full pack
   ships to `sd:/mainmenu/icons.pack` via `tools/mksdmirror.py`.
4. `src/ui/icon.c/.h` — the cache layer, and the only thing any screen calls:
   - `icon_init(const char *storage_prefix)` — open the pack, read the header, validate magic and
     format version, fall back to the built-in 16 on any failure. Same wholesale-invalidate
     discipline as every other cache here.
   - `icon_get(uint16_t index, int size)` → `const surface_t *` or NULL if not yet rasterised.
   - `icon_pump(uint32_t budget_ticks)` — called from `background()`, rasterises **at most three**
     per frame, in cursor-distance order so the cell under the cursor lands first.
   - Direct-mapped cache, two size classes (40 and 60), RGBA16, themed recolour.
     40 × 40 × 2 = 3,200 B per entry; a 64-entry 40 px cache is **204,800 B**. Against ~3.8 MB free
     that is affordable, but it is the largest single new allocation in this batch — measure the
     heap high-water, do not assume it.
   - Scratch: `SVG64_SCRATCH_BYTES(60,60)` allocated once at init, never per render.
5. `icon_name(uint16_t index)` from a names table for the 2a caption line, and `icon_category()`
   for the category list. Both baked at build time by `mkpack.py` from `icon-meta.jsonl`.

**Do not** add a second rasteriser, a sprite atlas, or a disk cache of rendered icons. The whole
point of svg64 is that neither is needed, and a rendered-icon cache on the card would be a fourth
versioned cache file to invalidate.

## 2. The keyboard — shared, blocks §3

6. `src/screens/screen_keyboard.c/.h`, armed like `screen_code.c` is (`screen_keyboard_ask()`
   carrying a destination and a buffer), because it already has three callers.
7. Layout exactly as spec §7: field 608 × 64 at 16,44; keys 52 × 48, gap 6; rows at y 132/190/248
   offset 32/62/120; row 4 DELETE 132 · SPACE 260 · DONE 132 offset 52, y 306. Caret 14 × 38
   `#F7B521` at **1 Hz square blink** — 0.5 s on, 0.5 s off, in seconds, never frames.
8. Rules: A types, B deletes from anywhere, START confirms from anywhere, 8 max for `KB_NAME`,
   9th character is a no-op plus reject SFX, empty name rejected on confirm with a field flash and
   the old value kept.
9. Retire both odometers. Delete the `ALPHABET` winding from `screen_profiles.c` and
   `screen_cheatedit.c`; both call the keyboard. Expect `src/screens/` to shrink here.

## 3. Profiles

10. `profile_t` grows `icon` (u16) and `colour` (u8). `profiles.ini` gains a version key and the
    two fields; absent version = 0.2 layout, migrated per §0.2. Stable slots, delete clears in
    place, `PROFILE_MAX 10` and `PROFILE_NAME_CAP 9` unchanged.
11. Uniqueness: **(icon, colour) unique across slots**, two profiles may share a sprite. Enforced
    at apply time with the reject SFX, not by filtering the grid — a greyed-out cell the user
    cannot reach is worse than a clear refusal.
12. **The chip.** Top right of the grid, drawn every frame regardless of profile count — this is
    now the entry point, so it cannot be conditional the way the Z hint was
    (`screen_grid.c:387`). Chip = 40 px icon on its swatch plate, name beside it. Focused state
    per spec §8: brightest thing on screen, plus lift and outline.
13. Retire the Z-in-footer hint and the Settings `ROW_PROFILES` row as the *entry points* — the
    settings row stays as a mirror (the file `screen_profiles.c`'s header is right that two
    screens drawing the same list is how one drifts), but the chip is primary.
14. **P1 — slot grid.** Cards 112 × 158, 5 × 2, gaps 12/14, origin 16,78 (= 608 wide, exactly
    `SAFE_W`). Header `Who's playing?` 32 px at 16,16, `N of 10 used` right-aligned. Plate
    64 × 64 with the sprite at 40 px centred, name 20 px centred 16 px below. Unselected at 0.66
    brightness; selected lifts 3 px with a 2 px `#FFFFFF` outline and a `0 5px 0 rgba(0,0,0,.56)`
    shadow. Empty slots: 1 px dashed `#52525A`, `+` and `Empty` in `#737384`. Footer
    `A Select · Z Edit · START New`. **No subtitle, no counts** — the spec is explicit.
15. **P2 — appearance edit.** Category list 132 wide, 24 px rows, gap 2, origin 16,96. Icon grid
    9 × 5 of 44 px cells, gap 6, origin 160,96 (ends x=604). Sprite 40 px centred. Cursor cell
    lifts 2 px on `#31314A` with a 2 px white outline; idle `#191929` with `#9C9CAD` ink. Position
    bar per §0.7. Name line at 160,356 with `PAGE n / m` right-aligned. **L and R page within the
    category**; the cursor never leaves the grid; changing category resets to page 1. Swatch row
    from frame 1a. Footer `A Apply · B Back`.
    **Implement 2a. Frame `1a · EDIT · APPEARANCE` is superseded — do not build it.**
    Search is out of scope; there is no Z affordance on this screen.
16. **P3 — name entry.** The §2 keyboard, `KB_NAME`.
17. Sound, per spec §9, from the six existing SFX: move on any cursor step; select on A over a
    slot/sprite/key; back on B or leaving a panel; confirm on START-new and DONE; reject on the
    9th character, empty-name confirm and duplicate (icon, colour); apply when the grid returns.

## 4. Settings scrollbar

18. Settings is 7 rows today (`ROW_PROFILES … ROW_PARENTAL`) at `ROW_H 34` from `LIST_Y 92`, so
    it occupies 92..330 against a footer at 424. **It is not overflowing yet** — it fits ten rows
    before it does. This batch adds Credits and Licence, taking it to 9.
19. So build the scroll *mechanism* now and let it be inert until it is needed: a `first_visible`
    window, cursor-follows-window clamping, and the position bar `screen_grid.c` already draws
    (`draw_position_bar`) reused rather than rewritten. **Measure the actual row count against
    `FOOTER_Y` at build time with a static assert**, so the row that finally overflows fails the
    build rather than drawing over the footer.

## 5. Credits and licence

20. Close the repo-side gaps first, because a screen cannot render what the tree does not record:
    `assets/fonts/LICENSE-Firple.txt` (OFL 1.1 full text — required by the licence),
    `assets/music/LICENSE.txt`, `assets/sounds/LICENSE.txt` naming source and terms.
21. `docs/CREDITS.md` — the single source of truth, generated-adjacent to `README.md:65-92`:
    upstream N64FlashcartMenu (AGPL-3.0, Polprzewodnikowy), libdragon (Unlicense), libspng
    (BSD-2), miniz (MIT), midi64 (MIT), picojpeg, acutest, svg64 (MIT), the five emulator cores,
    Firple (OFL 1.1), Pixabay sounds, the 28 CC0 MIDIs, the ares-derived ROM database,
    libretro-database cheats (MIT), and **all 36 game-icons.net authors** from
    `svg64/docs/ICON-CREDITS.md` with CC0 authors marked.
22. `src/screens/screen_credits.c` — **one scrolling text column**, as asked: nobody is made to
    read it, anybody can. Up/Down scroll, L/R page, B back. The text is baked into the DFS as a
    plain UTF-8 file at build time from `docs/CREDITS.md`, so it cannot drift from the doc.
23. **Top of that text, before anything else:** the AGPL source offer — what this is, where the
    source is, one URL. It is the obligation with teeth and it belongs where it will be seen.
24. Two settings rows: `Credits` and `Licence`, or one row into a two-tab screen. One row is
    better — the distinction is not one a player has.
25. Reuse `src/ui/text.c`'s cached paragraphs. A credits screen that mallocs a paragraph per frame
    is the exact bug `file_list.c:95` had.

---

## 6. Tests

### 6.1 What must be measured, not asserted

| # | Measurement | Where | Gate |
|---|---|---|---|
| T1 | ms per icon at 40 px and 60 px on **this** ROM's build flags | ares, instrumented | within 2× of svg64's 4.8 ms; if worse, find out why before proceeding |
| T2 | Frames to fill a fresh 45-cell page at 3/frame | ares, scripted | ≤ 20 frames, zero missed fields during the fill |
| T3 | `icons.pack` **read** cost per icon over FatFs/SC64 | hardware only | the number ares cannot give; if it dominates T1, the 3/frame policy needs rethinking |
| T4 | Heap high-water with the 40 px and 60 px caches live | ares, `DEV_HARNESS` | measured, recorded; the 204,800 B estimate confirmed or corrected |
| T5 | ROM size and `.text`/`.data`/`.bss` delta | every gate | recorded against 1,638,400 / 571,320 / 108,196 / 75,944 |
| T6 | Steady-state allocations per frame on P1, P2, P3, credits | ares, wrapped malloc | **zero** |
| T7 | p95 wall over a 600-frame scripted P2 page-and-scroll | `tools/frametime.py` | ≤ 16.9 ms, zero missed fields |

### 6.2 Host tests — `tools/hosttest/run.sh`, extended

These are the ones that can run any time (seconds, no emulator), and they cover the half ares
cannot reach because the DFS is read-only.

- **T8 `profiles.ini` migration.** A 0.2-layout file with 3 profiles, one deleted from the middle,
  round-trips: all three names survive, slot indices are stable, and `saves/p3/` still belongs to
  profile 3. **Prove it can fail** by renumbering on write and watching it go red.
- **T9 (icon, colour) uniqueness.** Every rejection case, and the two-profiles-share-a-sprite case
  that must be *allowed*.
- **T10 Keyboard buffer.** 8-char cap, 9th is a no-op, delete from empty, empty-name rejection,
  both charsets. Pure state machine, no rendering — the reason to keep the keyboard's model
  separable from its draw.
- **T11 `mkpack.py` determinism.** Same corpus, same exclusions → byte-identical `icons.pack`.
  And exclusions applied **before** `--limit`, verified by packing with `--limit 50` and asserting
  no blocklisted name appears.
- **T12 `icon-meta.jsonl` covers the pack.** `metacheck.py` reports 0 untagged for the default
  build. Not a hard failure for `ICON_EXCLUDE=` builds, by design.
- **T13 Credits text is complete.** A script asserts every author in
  `svg64/docs/ICON-CREDITS.md` and every entry in `README.md:65-92` appears in the baked DFS
  credits file. This is the test that stops the licence screen rotting — it is the only one here
  with a legal consequence, so it is the one that must be shown to fail before it is trusted.

### 6.3 ares, scripted — `tools/regress.sh`, only when asked

New input scripts under `tools/inputs/`: `profile-chip.txt` (R off the tab rail onto the chip, A,
P1 appears), `profile-edit.txt` (Z, category move, L/R page, A apply, back at the grid),
`profile-name.txt` (every key row, cap, delete, DONE), `settings-scroll.txt`,
`credits-scroll.txt` (to the end of the text and back).

Hash stability is the gate, same contract as everything else: two identical runs, byte-identical
`hashes.txt`. Remember the ares log is `<script>.ares.log`.

Contact sheets for P1 selection lift, P2 cursor inversion and the caret blink — comb and shimmer
are visible across a strip and invisible in one frame.

### 6.4 Hardware

Order matters, cheapest disqualifier first:

1. Boot with **no** `icons.pack` on the card. The fallback 16 must work and nothing may hang.
2. T3, the read cost. Then T1 on real silicon.
3. P2 paging under a held L, which is the worst case for read + rasterise together.
4. The migration, T8, against a card that genuinely predates this batch — copy one first.

---

## 7. What not to do

- **Do not implement frame `1a · EDIT · APPEARANCE`.** The spec keeps it for history and says so.
- **Do not build search.** Out of scope for this release; no Z affordance on P2.
- **Do not add avatars, photos, free colour picking, per-profile themes.** Spec §1 excludes them.
- ~~**Do not vendor the icon corpus into the repo.** `ICON_DIR` points outside it, deliberately, so
  `git clone` does not ship CC BY artwork.~~ **Superseded** — `git clone` does ship it now, from
  `assets/icons`, with the licence and the 36 authors beside it. See the struck entry in
  GOTCHAS-PROFILES.md §7 for what the original ruling got wrong.
- **Do not let the swatch exception (§0.6) spread.** Eight colours in `profile.h` and nothing else
  leaves `theme_t`.
- **Do not pre-render icons to a cache file on the card.** Fourth versioned cache, no benefit.
- **Do not run the regression suite unless asked.** `tools/hosttest/run.sh` is exempt.
- **Do not treat profiles as a lock.** `profile.h` is emphatic and the parental code stays outside
  and global. Adding a face to a profile must not make it look like a login.

---

## 8. Order, and the gates between

| Stage | Contents | Gate to pass before the next |
|---|---|---|
| A | §0 decisions recorded in AUDIT.md, §5.20 asset licence files | the nine questions have written answers |
| B | §1 svg64 + `icon.c` + fixture pack | T1, T2, T4, T5 |
| C | §2 keyboard, both odometers retired | T10, `src/screens/` line count **down** |
| D | §4 settings window + §5 credits screen | T13, T5, T6 |
| E | §3 profiles: data, migration, chip | T8, T9 |
| F | §3 P1, P2, P3 | T6, T7, contact sheets |
| G | hardware, §6.4 | T3 — the only number ares cannot give |

C and D are independent of B and can be built in parallel with it. E depends on B and C both.

The riskiest item is **T3**, and it is last, which is wrong in the abstract and right here: the
whole picker is throwaway if the read cost dominates, but nothing can measure it until the picker
exists. Mitigate by adding a `--sd-delay-us` knob to `icon_get`'s fetch under `DEV_HARNESS`, the
same way the thumbnail streamer was tested against a pessimistic 1.5 MB/s, and by keeping the
3-per-frame policy a constant rather than a scattered assumption.
