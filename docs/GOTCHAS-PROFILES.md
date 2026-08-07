# Gotchas: icons, profiles, credits

Companion to [NEXT-PROFILES.md](NEXT-PROFILES.md). That file says what to build; this one says what
will go wrong while building it. Everything here is measured off the tree as it stands on
6 Aug 2026, or it is labelled as a thing to measure.

Ordered by how expensive it is to discover late.

---

## 1. Type is the hidden cost of this batch

### 1.1 There are two fonts, and one of them can spell 41 characters

`fonts.h:19-20` is the whole inventory:

| id | size | charset | baked size |
|---|---|---|---|
| `FNT_DEFAULT` | 20 px | `charset.txt`, 7,931 chars | **681,040 B** |
| `FNT_BOOT` | 32 px | `charset-boot.txt`, 41 chars | **5,016 B** |

`charset-boot.txt` is `ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-/:` — **no lowercase, no
apostrophe, no question mark.**

The spec asks for type at **20, 24, 32 and 40 px**. Two of those do not exist and one exists in a
form that cannot render the string the spec puts in it.

### 1.2 `Who's playing?` cannot be drawn with the font that is already 32 px

Spec §5 puts it at 32 px. In `FirpleBoot` that renders as `WHO ? PLAYING ?` with holes — nine of
its thirteen characters are missing from the charset.

Options, cheapest first:

- **`WHO'S PLAYING?` in caps, and add `'` and `?` to `charset-boot.txt`.** Two glyphs, a few
  hundred bytes, and it matches the boot plate's existing voice.
- A new 32 px face with a wider restricted charset (see 1.3).
- Full charset at 32 px — **do not**, see 1.4.

### 1.3 Every new size needs its own restricted charset, and they fail silently

`mkfont` bakes a glyph atlas. A character outside the charset does not error at build time and
does not error at run time — it draws as a missing glyph or as nothing, on hardware, in the one
string nobody re-read.

This is already a live hazard with `FNT_BOOT` and it is about to become four times as live. The
mitigation is a build-time check, and it is cheap:

> **T14 — charset coverage.** A script greps every string literal passed to a draw call with a
> restricted font and asserts every character appears in that font's charset file. Run it in
> `tools/hosttest/run.sh`. Prove it fails by adding a lowercase letter to a boot-plate string.

Without T14 this batch will ship a hole in a string. With it, the failure is a build error.

### 1.4 Full-charset bakes at large sizes are megabytes

A glyph atlas scales with the square of the type size. From the one measured point — 681,040 B at
20 px — the extrapolation is:

| size | full charset, extrapolated | note |
|---|---|---|
| 20 px | 681,040 B | measured |
| 24 px | ≈ 0.98 MB | |
| 32 px | ≈ 1.74 MB | the Makefile already calls this "megabytes for glyphs nothing renders" |
| 40 px | ≈ 2.72 MB | |

All four would be ≈ 6.1 MB **on top of** the 6.56 MB icon pack and the 1.6 MB ROM. That is the
whole 8 MB budget spent on type nobody reads.

**So: restricted charsets for 24, 32 and 40, and only `FNT_DEFAULT` keeps the full one.** Which
means the strings drawn at those sizes are constrained, which means 1.3's check is not optional.

The three new charsets, from what the spec actually draws:

- **24 px** — category names. See §4.2: these need display names anyway, so the charset is
  whatever those spell. Latin letters and space.
- **32 px** — the P1 header, and the keyboard's key glyphs (A–Z, `DELETE`, `SPACE`, `DONE`).
- **40 px** — the P3 name field. `KB_NAME` is A–Z and space, so 27 glyphs. If the same field is
  reused for `KB_TEXT` (cheat names) it needs digits and punctuation too, which roughly doubles
  it and is still small.

### 1.5 The button glyphs live in exactly one bake

`charset.txt` ends with `ⒶⒷⓈⓏⓁⓇ◀▶▼▲` — the controller glyphs `ui_hint` draws. They are in the
20 px full-charset face only. A footer at any other size renders them as holes. Keep footers at
`FNT_DEFAULT`, or add the ten glyphs to that size's charset explicitly.

### 1.6 The credits text is the one string that is not a literal

§5.22 bakes `docs/CREDITS.md` into the DFS and renders it. So the charset check in 1.3 cannot see
it — it is data, not a literal. All 36 game-icons authors are **pure ASCII** (checked), which is
the good case, but Markdown punctuation is not: an em dash, a smart quote or a non-breaking space
pasted into the doc renders as a hole.

> **T15 — credits charset.** The bake step itself asserts every character of the generated file is
> in `charset.txt`, and fails the build otherwise. It is a three-line addition to the generator and
> it is the only thing standing between a copy-paste and a hole in the licence text.

---

## 2. ROM size and boot time

### 2.1 The budget, with real numbers

| item | bytes | source |
|---|---|---|
| ROM today | 1,638,400 | `output/sc64menu.n64` |
| `icons.pack` | 6,561,304 | `svg64/build/fs/icons.pack`, 3,894 icons |
| **subtotal** | **8,199,704** | ≈ 7.82 MB |

That is before a single new font byte. The user's 7–8 MB comfort zone is *already spent* by the
pack alone. Three restricted font bakes are tens of KB each, so the plan fits — but only because
of §1.4's decision. Reverse that decision and it does not.

### 2.2 The DFS does not compress the pack

`svg64demo.dfs` is 6,562,560 B against a 6,561,304 B pack — 1,256 bytes of overhead and no
compression. SVG is text and deflates roughly 4:1, so ~1.6 MB is available for the taking.

**Do not take it yet.** Decompressing per icon adds to the 4.8 ms render, and the render is
already the thing under a 3-per-frame budget. It is the right lever to pull *if* §2.3 says boot
time hurts, and the wrong one to pull speculatively. Record it here so it is not rediscovered.

### 2.3 SC64 loads the whole ROM from the card at boot

This is the cost the user actually feels and it is in none of the budgets above. Going from 1.6 MB
to 7.8 MB is 5× the bytes over the same SD path.

> **T16 — boot time.** Stopwatch, on hardware, power-on to first frame, five runs, before and
> after. If the delta is under a second, close it. If it is not, §2.2 is the answer.

Not measurable in ares — there is no SD and no SC64 loader in the path.

### 2.4 Watch the fixture, not just the release build

`make FIXTURE=1` with a full pack makes every regression run load 8 MB. Cap it — `--limit 200` is
≈ 320 KB — and keep the full pack for release and hardware only. Paging, caching and the position
bar are all exercised fine by 200 icons across a few categories.

---

## 3. svg64 on this ROM

### 3.1 It has never run on real hardware

svg64's own README says so: *"It has not run on real hardware. ares models the R4300i closely, but
an emulator is not a console."* Its 4.8 ms figure is an ares number.

**This matters more here than it normally would**, because AUDIT 1ag is exactly this project
being burned by an ares green that did not predict hardware. Treat every svg64 timing as
unverified until §T1/T3 run on the M64, and put the icon path behind something that degrades
rather than hangs if it is slower than expected — a per-frame time check, not just a count of
three.

### 3.2 One icon in the corpus costs 31.4 ms

`svg64/docs/PERF.md`: `avg 4812 us  max 31442 us`. The worst file flattens to several hundred
segments. Three of those in a frame is 94 ms — six missed fields.

**Budget by time, not by count.** `icon_pump(budget_ticks)` should render *while there is budget*
and stop, with three as a cap rather than a target. A count-only policy makes the pathological
icon a visible stutter that nothing in the code predicts.

### 3.3 Scratch sizing, once

`SVG64_SCRATCH_BYTES(w,h)` is `(w+2)*h*4`:

- 40 × 40 → 6,720 B
- 60 × 60 → **14,880 B**

One allocation at `icon_init`, sized for 60, 8-byte aligned, reused for both. Never per render —
the library allocates nothing itself and that property is worth keeping true end to end.

### 3.4 A 60 px icon needs two TMEM loads; a 40 px one does not

TMEM is 4 KB. RGBA16 stride is `w * 2`:

- 40 px → 80 B/row → 51 rows fit → **one load**
- 60 px → 120 B/row → 34 rows fit → **two loads**

Not a problem, but `rdpq_tex_upload` will silently do the right thing and the cost difference will
show up in a frame-time measurement with no obvious cause. Write it down now.

### 3.5 Floating point in the arc path

svg64 is 16.16 fixed point except for elliptical-arc setup, which uses floats. Verify MainMenu's
build does not pass anything that would soft-float it, and that the arc path is actually reached
by the corpus (it is — the README calls out the compact unseparated flag form as a thing real icon
sets emit).

### 3.6 `recolor` is a two-colour luma remap, and the spec's ink rule is a table

svg64's `recolor` maps fills onto a two-colour palette by luma — built for the corpus convention
of a black backdrop path plus white artwork. The spec's rule (§4) is that ink flips to `#101019`
on the five light swatches and `#F7F7FF` on the three dark ones.

Those agree, but only if the mapping is driven by the **swatch table's declared ink field**
(NEXT §0.6), not by computing luma at draw time. A computed threshold will disagree with the
designer's choice on at least one swatch, and the swatch it disagrees on will be the one nobody
tests.

**Superseded in part.** The ink is no longer derived from the plate at all: plate and artwork are
independent choices from one nine-colour palette, and the flip above survives only as
`profile_default_ink()` — what a new slot gets and what a card with no `ink` key reads as. The
warning still holds for that default, which is a table (`DARK_PLATES`) and not a computation. The
palette is nine rather than the spec's ten because two of its entries were the same white to
within three of thirty-two levels; see AUDIT 1ah, third pass.

### 3.7 svg64's stats are per-thread

`svg64_stats_t` is documented as per-thread. Irrelevant on this target, noted so nobody designs
around it.

---

## 4. The category browser (P2)

### 4.1 There are 30 categories and about 9 fit

The corpus splits into **30 categories** (`tools/icon-meta.jsonl`, key `cat`). The spec's category
list is 24 px rows with a 2 px gap from y=96, and the icon grid it sits beside ends at y=340 — so
the list is 244 px tall and holds **9 rows**.

The spec says *"Categories are a flat list, no nesting"* and does not say what happens to the
other 21. It needs either scrolling within the list, or a curated shorter list. **Scrolling is the
honest answer** and it reuses the §4 settings window mechanism.

### 4.2 The category names do not fit the box, and are not for humans

The slugs are `creature-animal`, `abstract-geometric`, `building-structure`,
`knowledge-writing`, `treasure-currency`. Eighteen characters at 24 px is roughly 216 px against
a **132 px** column.

Needs a display-name table: `creature-animal` → `Animals`, `abstract-geometric` → `Abstract`,
`weapon-melee` → `Melee`, and so on for all 30. Baked at build time beside the counts.

### 4.3 The baked counts must come from the pack that shipped

Spec §6: *"Counts are per category and are baked at build time."* True — but `ICON_LIMIT` and
`ICON_EXCLUDE` both change them. A count baked from the full corpus and shipped alongside a
`--limit 200` pack is a lie the position bar will draw.

Generate the counts **from the pack**, in the same `mkpack.py` run that writes it, not from
`icon-meta.jsonl` independently.

### 4.4 Page counts, for the record

45 cells per page. Largest category is `creature-animal` at 276 icons → **7 pages**. Smallest is
`misc` at 42 → 1 page. So `PAGE n / m` never needs more than one digit either side.

### 4.5 The position bar is 10 px outside the safe area

Repeated from NEXT §0.7 because it is the kind of thing that gets implemented as specified and
then found on a CRT: spec says x=628; `theme.h:57-60` puts the safe area at x ∈ [16, 624).

---

## 5. Profiles and saves

### 5.1 Deleting profile 1 deletes `saves/` — everyone's

This is the sharpest thing in the batch.

`profile.h` is explicit: profile 1 is index 0 and *"its files carry no suffix at all"* —
`cache/playstate.dat` and `<romdir>/saves/Game.sav`. That asymmetry is deliberate and load-bearing:
it is what makes a card written before profiles existed keep working.

Now that delete removes saves (NEXT §0.2), **deleting profile 1 means deleting `<romdir>/saves/`
outright** — which on a single-profile card is every save on the card, and on a multi-profile card
is still the shared-looking directory that predates the feature.

Options:

1. **Refuse to delete slot 1.** It can be renamed and re-iconed, never removed. Simplest, and
   defensible: there is always a player one. `profile.h` already says there is no "no profile"
   state, so this is consistent with the model rather than a special case bolted on.
2. Allow it, with a differently-worded and much louder warning naming the directory.

**Recommendation: option 1.** A destructive action whose blast radius is "the whole card's saves"
should not be reachable by two button presses on a screen designed for picking a face.

### 5.2 Delete removes three things, not one

`saves/pN/`, `cache/pN/playstate.dat`, `cache/pN/cheatstate.dat`. Missing the cache files leaves
favourites and enabled cheats that reattach to whoever next occupies the slot — which is precisely
the inheritance problem stable slots were adopted to avoid, reintroduced by omission.

### 5.3 There is no undo, and the confirm is one press

The card is the only copy. Recommend the confirmation default sitting on **Cancel**, and the
destructive option requiring a deliberate move onto it — not a Yes/No where Yes is pre-selected.

### 5.4 `profile_t` changing size will trip the static asserts

Adding `icon` (u16) and `colour` (u8) changes the struct. `CLAUDE.md` records that `libindex.c`,
`playstate.c` and `cheatstate.c` are verified *only* by compile-time size assertions — so those
will fire, correctly, and each one needs its number updated with the new layout understood rather
than the number bumped until it compiles.

### 5.5 `profiles.ini` needs a version key even though delete no longer promotes

Old cards are compacted; new ones are not. Without the key there is no way to tell a card with
three consecutive profiles from a card that had five and deleted two. The read path is the same
either way, but the *rewrite* differs, and a rewrite that renumbers is the bug this whole section
exists to prevent.

### 5.6 The chip breaks the "invisible with one profile" property, deliberately

`screen_grid.c:387` draws the Z hint only when `profile_count() > 1`, and `screen_profiles.c`'s
header argues at length that the whole feature stays invisible until asked for. The chip is the
entry point now, so it must draw always — otherwise a single-profile card has no route to
`Who's playing?` at all.

That is a real reversal of a documented design decision. Record it in AUDIT.md as such.

### 5.7 (icon, colour) uniqueness is rare enough to go untested

3,894 icons × 8 colours = 31,152 combinations against 10 slots. Two users will essentially never
collide by accident, which means the rejection path will essentially never run, which means it
will be broken. **Force it in a host test** (T9) rather than hoping to see it.

The error also has to say *which* slot owns the combination, or the user is refused with no way to
act on the refusal.

### 5.8 The scanner already skips `saves/`

`library.c`'s `SCAN_SKIP` excludes `saves/` and everything under it, which is why `profile.h`
nested `saves/pN/` inside rather than beside. Deleting a profile's saves therefore cannot
invalidate `library.idx` — confirm that stays true rather than triggering a rescan on delete.

---

## 6. Testing traps

### 6.1 ares cannot execute the half of this that matters most

The DFS is read-only under ares. So **every write path in this batch is unexecutable there**:
creating a profile, renaming one, applying an appearance, deleting one and its saves, and the
`profiles.ini` version migration.

`tools/hosttest/run.sh` is the only thing that can reach them, and it is exempt from the
never-run-the-suite rule. Every new state machine gets a host test — and per house rule, each is
**shown to fail before it is trusted**.

### 6.2 The lesson from AUDIT 1ag applies directly

The preamble hook was green end-to-end under ares, on executed emitted code, with a working
mutation control, and it did nothing on the M64. The relevant part is not "ares is unreliable" —
it is that **a green on a path the emulator only partly executes predicts nothing**. Icons and
profile writes are both such paths. Plan the hardware check as the gate, not as confirmation.

### 6.3 Blinking things destroy hash stability

The caret blinks at 1 Hz and the selection outline pulses. Regression screenshots must be taken at
**deterministic frame numbers**, and both animations must be driven by accumulated `dt` from a
known start rather than anything wall-clock. `tools/mkinput.py` scripts are frame-counted already,
which is the property that makes this work — do not introduce a time-based blink.

### 6.4 The ares log filename

`<script>.ares.log`, not `<script>.log`. Cost an afternoon once already.

### 6.5 Stale dependency files after files are deleted

Retiring the odometers deletes code that `build/*.d` still references, and make will stop with
`No rule to make target`. `rm -f build/app.o build/app.d` and rebuild — this has already happened
once this month, during the preamble-hook revert.

---

## 7. Small things that will still cost an hour each

- **`FNT_DEFAULT` is 20 px in the Makefile and documented as 15 px** in `fonts.h:19`. One of them
  is wrong. Find out which before laying out anything against a nominal size.
- **`PROFILE_NAME_CAP` is 9** — eight characters plus the terminator. The spec says eight. They
  agree; the off-by-one is the kind that gets "fixed" into a bug.
- **Spec names are uppercase-only**; the existing `ALPHABET` includes digits and punctuation.
  Existing profile names may contain characters `KB_NAME` cannot type. The keyboard must be able
  to *display and delete* a name it could not have produced.
- **Frame `1a · EDIT · APPEARANCE` is on the board and is superseded.** It is the first thing
  anyone opening the board will see and try to build. NEXT §7 says so; this says so twice.
- **`support.js` is required to open the `.dc.html` boards locally.** Opening a frame file alone
  renders nothing and looks like a corrupt spec.
- **Motion in seconds, never frames.** The 1 Hz blink, the 3 px lift, the 0.66 brightness ramp.
  The repo rule exists because the frame rate is still an open question.
- **`ICON_DIR` points outside the repo on purpose.** Do not vendor `svgicons/`; `git` keeps blobs
  forever and that is 17 MB of CC BY artwork that cannot be removed later.
