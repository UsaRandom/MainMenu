# MainMenu — layout geometry

**The authority is [`docs/design/README.md`](design/README.md)**, the Summer Cart UI spec board
handoff. Open `docs/design/Summer Cart Main Menu - Spec Board.dc.html` in a browser for the
pictures; the README carries the numbers.

This file exists for two things only: the numbers the firmware needs at a glance, and a record
of where the spec and the implementation disagree. Where this file and the handoff conflict and
nothing below says otherwise, **the handoff wins**.

---

## 1. Committed geometry

Unchanged from the pre-spec working set except the left margin. All device pixels at 640 × 480.

| | |
|---|---|
| Frame | 640 × 480, RGBA5551, fixed. No breakpoints, no reflow. |
| Safe rect | 608 × 448 at 16,16. Backgrounds bleed past; text and controls do not. |
| Tab rail | 608 × 48 at 16,16 |
| Grid viewport | 596 × 352 at 16,72. **Clips top and bottom only** — 8 px horizontal bleed. |
| Tile | 140 × 98, aspect 1.4286 : 1 |
| Column origins | 16, 168, 320, 472 (pitch 152 = 140 + 12) |
| Row origins | 72, 182, 292, 402 (pitch 110 = 98 + 12) |
| Selected tile | **152 × 106 at −6, −4 from its cell. Whole pixels, not a scale factor.** |
| Selection shadow | 152 × 106 at +4 x, +6 y from the selected rect. Solid, no blur. |
| Selection outline | 2 px, drawn outside the 152 × 106 rect |
| Position bar | 6 × 352 at 616,72, thumb min 24 px, hidden when nothing scrolls |
| Footer | 640 × 56 at 0,424; content in the top 40 px |
| Radius | **0 everywhere**, except baked button glyphs |
| Spacing scale | 4 · 8 · 12 · 16 · 32. Nothing between, nothing outside. |

12 tiles fully visible plus a 22 px peek of row 4.

**Whole-pixel rule.** Scroll targets are always a multiple of the 110 px row pitch. Ease in
float, round to integer *before* the display list. Sub-pixel texture scroll shimmers here.

### Changes the spec made to the earlier working geometry

- Left margin **22 → 16**, so the 596 px block ends at x 612 and leaves room for the position
  bar. Tile size, gap, pitch and tile count are unchanged, so **the memory and streaming budget
  in §3 is unchanged**.
- Selection is a **whole-pixel 152 × 106 rect**, replacing the earlier 1.102 scale factor
  (which gave 154 × 108 and a fractional origin). Strictly better on this hardware.
- The primary selection signal is **`tile_dim`, a wash over every *unselected* tile** — not the
  grow. Size, shadow and outline are reinforcement. This inverts what gets drawn: the common
  case is a blended quad per non-selected tile, not per selected tile. See §4.

## 2. Working set: 20 tiles, not 16

The handoff says 16 resident (12 visible + the peek row). We keep **20** (12 + one row above +
one row below), because during a scroll the incoming row must already be decoded or a tile pops
in after the motion has stopped. 20 is a superset of 16, so the spec is satisfied; the extra
cost is 4 × 14,624 = **58,496 bytes**, which is noise against 8 MB.

## 3. Thumbnail budget

Derived from §1; changing any number there changes these.

| | bytes |
|---|---|
| TLUT | 512 (256 × uint16 RGBA5551) |
| Pixels | 14,112 (98 rows × 144 stride; 140 used, padded to 8-byte alignment) |
| **Resident per tile** | **14,624** |
| **Working set, 20 tiles** | **292,480** |
| On-disk slot | 15,360 (30 × 512 B sectors, so every slot and sub-block is sector-aligned) |

**CI8**, chosen for scroll streaming rather than RAM. One row of scroll is 4 new tiles: 61 KB at
CI8 versus 115 KB at RGBA16 — at a realistic ~3 MB/s over FatFs, 20 ms versus 38 ms, or 1.2
frames versus 2.3 at 60 Hz. Both fit in RAM; only CI8 leaves headroom for C-stick fast scroll,
which is exactly when the streamer is under pressure.

TMEM load count is identical in both formats (14 rows per load), so it is not a discriminator.
Recorded so it is not re-argued.

## 4. Fill-rate estimate — measure this early

The `tile_dim` wash makes the grid a **two-pass fill** over most of the viewport, which the
pre-spec estimate did not account for. Rough numbers at 62.5 MHz, 1 cycle per blended pixel:

| pass | pixels | ≈ ms |
|---|---|---|
| 12 tile blits (140 × 98) | 164,640 | 2.6 |
| `tile_dim` over ~15 unselected tiles | 205,800 | 3.3 |
| ambient wash, 420 × 300 dithered quad | 126,000 | 2.0 |
| background + rail + footer | ~150,000 | 2.4 |
| **total** | | **≈ 10.3 of 16.7** |

That is feasible but not comfortable, and it is an estimate, not a measurement. The handoff
already nominates the ambient wash as the first thing to cut (§10.3) and caps the screen at two
blended layers. **Measure before optimising and before arguing.**

## 5. Unresolved — needs a decision

### 5.1 The selected tile in column 3 collides with the position bar

Column 3 sits at x 472–612. Selected, it becomes 466–618. The position bar is at 616–622, so
the tile overlaps it by 2 px, and the selection shadow (+4 x) spans 470–622 and covers the bar's
full width. Column 3 is a quarter of all selections, so this is not an edge case.

Proposed, pending the designer: move the position bar to **x 618–624** (right-aligned to the
safe rect, so the tile's exclusive right edge at 617 clears it), and **draw the bar last** so the
4 px shadow sliver behind it is hidden. Neither change touches a documented column origin.

Related and apparently intentional: a selected column-0 tile starts at x 10, which is outside
the 16 px safe inset. The handoff explicitly permits an 8 px horizontal overhang, so this is
accepted, but it is the one place the safe-inset rule is knowingly broken.

### ~~5.2 The detail sheet wants art at 1:1, which the grid cache cannot supply~~ — resolved

Resolved by measurement: a 280 × 196 decode plus downscale costs **86.7 ms**, not the 0.3–1.5 s
the plan assumed. That fits inside the 0.20 s sheet-open animation, so the sheet decodes its
own art during the rise from a **lazily populated LRU of large slots**. No second on-disk
cache, no 27.7 MB, no doubled first-run build. See AUDIT.md §1e.

### 5.3 CJK font page

Carried over from the handoff's own open questions. A Japanese title needs a second baked font
page. Confirm the ROM budget or fall back to the filename for unmatched CJK titles.

## 6. Fixture alignment

`tools/mkfixture.py` now generates placeholder art at **280 × 196**, the real asset spec, rather
than upstream's 158 × 112 box-art size. The game code is still stamped across each card in large
glyphs so a mis-mapped tile is visible in a contact sheet rather than merely plausible.

`docs/design/art/` holds 28 generated cards at the same spec, which are a better-looking library
to judge the grid against than the procedural ones. They are placeholder key art, not shippable
content.
