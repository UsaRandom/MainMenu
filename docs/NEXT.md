# Next batch

Notes taken 4 Aug 2026, not yet worked. Nothing here is decided by measurement — it is a list of
intent plus the questions each item raises, so the work can start without re-deriving them.

Working tree when this was written: `src/ui/theme.c`, `src/ui/theme.h` and
`tools/inputs/manual/demo-themes.txt` already carry item 5 (done). `tools/mksdmirror.py` is
untracked and unrelated to this list. Everything else below is untouched.

---

## 1. README, for someone who wants to play a game

Currently written for whoever is going to build it. It should be written for whoever is going to
use it.

- Much less technical throughout.
- **No references to commercial products.** The box-art section uses `Super Mario 64.jpg` as its
  worked example; invent a name instead, the same way `mkdemo.py` does.
- **Delete `## Screenshots`** (line 176) — the reproduction recipe is developer material.
- **Delete `## House rules`** (line 210).
- Everything from `# Licence` (219) down stays; it has to.
- Open: does the build section stay at all, or move to a separate file? A player needs "put this
  file on a card", not `N64_INST`.

## 2. SD card layout — one folder we own, and everything else anywhere

Two halves.

**Menu-written state moves to `/mainmenu/`.** Today it is `/menu/config.ini` and `/menu/cache/*`.
The name change matters because `/menu` is about to stop being the place content lives, and a
folder the menu owns outright should say whose it is.

- `app.c:29` `MENU_DIRECTORY "/menu"`
- `library/cache.c:15` `CACHE_SUBDIR "menu/cache"`
- `cheats/cheatdb.c:15` `CHEATDB_LOCATION "/menu"`
- `menu/cart_load.c:16,19` `DDIPL_LOCATION`, `EMU_LOCATION`
- and the new parental file from §3, which is the one the user is ever told to delete by hand —
  so it wants a name that says what it is when they are looking at the card on a computer.

**Content is found anywhere on the card.** ROMs, emulator cores, box art and `cheats.db` should
all be discovered by searching from the root, so someone can empty a zip into the card and it
works. Today the scan is rooted at `/roms` and the cores and database are at fixed paths.

- The scan root becomes `/`, with an exclusion list: `/mainmenu`, `saves`, and the platform
  litter — `.Spotlight-V100`, `.Trashes`, `.fseventsd`, `System Volume Information`, and anything
  starting `._` (AppleDouble files share the real file's extension, so `._Banjo-Kazooie.z64`
  would otherwise index as a second copy — `mksdmirror.py` already learned this).
- Cores and `cheats.db` are cheaper as a short probe list than as scanner coupling: try
  `/mainmenu/<x>`, then `/<x>`, then the old `/menu/<x>` for anyone with an existing card. Three
  `stat`s at launch, no new scan state.
- **Open: does a recursive scan of a full 32 GB card stay affordable?** The measured cost is
  11,153 µs/ROM against real files, but that is per *ROM*, not per directory entry. A card with a
  large unrelated folder tree pays for the walk. Measure before committing to it.
- **Open: `libindex` directory signatures are keyed on the scanned root.** Changing the root
  invalidates every existing index, which is fine and automatic, but the signature scheme itself
  assumes a shallow, known set of directories. Check it still holds over an arbitrary tree.

## 3. Parental controls

**Six presses, C buttons only.** The alphabet drops from eight symbols to four (C-up, C-down,
C-left, C-right) and the length goes 4 → 6, so the space is 4⁶ = 4,096 — identical to today's
8⁴. Simpler to teach, simpler to draw, no worse.

- `PARENTAL_CODE_LEN` 4 → 6, `parental_btn_t` down to the four C directions.
- `screen_code.c` draws six dots and a four-glyph alphabet row.
- `tools/inputs/parental.txt` needs rekeying; `press cup`/`cdown`/`cleft`/`cright` already exist.

**Delete "A lock on a menu, not security. See the manual."** No replacement, and no version of it
anywhere else either — not in the README, not in a manual. It is an N64 games menu; nothing here
is making a claim that needs qualifying, and qualifying it invites the reader to test the claim.

**Consequences for wrong entries — a counter on the card, no clock involved.** Currently a wrong
code costs 0.9 s and nothing else, so 4,096 attempts is an afternoon of patience. Decided:

- A count of failed attempts lives **on the card**, and each failure adds **5 s** to the wait
  before the pad will accept input again, **bounded at 10 minutes**.
- The bound is not politeness. Uncapped, a child who cannot get in can still hammer wrong codes
  a few hundred times and leave the parent waiting forty minutes — the feature turned against
  its owner. Ten minutes still makes 4,096 combinations hopeless.
- **A correct entry resets the count to zero.** This is what makes a counter viable with no
  clock: nothing has to expire, because the person who knows the code clears it every time they
  use it, and the person who does not never clears it.
- **The write happens before the guess is judged, not after.** Increment, persist, *then*
  compare. The other order makes pulling the power on a wrong answer free, and the whole thing
  collapses to nothing.
- Resetting the console is not a bypass, because the number is not in RAM. The countdown itself
  runs on `TICKS_READ()` — the COP0 counter `app->dt` already uses — so no RTC anywhere.
- If `cache_writable()` is false the counter cannot persist and the backoff is session-only. The
  parental panel already warns when storage is read-only; that warning now covers this too.

**The code and the counter live in their own file on the card**, not in `config.ini`. Forgetting
the code is recovered by deleting one file, so recovery costs nothing else the user has set — and
there is no master code to build, document or defend.

- Goes under `/mainmenu/` with the rest of what the menu writes (see §2).
- **No code set means no enforcement**, so deleting the file also releases the locked games. The
  `LIBF_LOCKED` flags stay in `playstate.dat` and simply come back into effect if a code is set
  again, which is what a parent recovering their own card would want.
- This moves `parental_code` out of `settings_t`, where it went for the reason recorded in
  `settings.h:61` — `ini_save()` is proven and `cache.c` is not. That reason still stands, so the
  new file wants the same scrutiny: it is the one write whose failure is invisible.

**Show the status where the game is launched.** On the detail sheet, when a game is locked or the
hour is outside the window, say so before A is pressed — `Play unlocked in 2h 15m` rather than
only finding out at the prompt. Pressing A there goes straight to the code screen.

- Needs the same live clock as the schedule, and the same no-clock answer.
- For a game locked by flag rather than by hour there is no countdown; that row reads
  `Locked` and A still opens the code screen.
- The padlock badge on the tile stays as it is.

**Lock a game from the sheet with C-left.** Toggles `LIBF_LOCKED` on the game being looked at,
prompting for the code first — in both directions, locking as well as unlocking. This is the
short path; the list under Parental controls stays for working through a shelf.

- Sits naturally beside C-right, which already favourites from the same screen, so the two C
  buttons either side are the two things you can do *to* a game.
- No conflict with the C-only code alphabet: that is only read on the code screen, and this
  navigates away to it before any digit is entered.
- After a successful entry the toggle has to apply to the game the sheet was on, so the pending
  action has to survive the trip through `SCREEN_CODE` — `screen_code_ask()` currently carries
  only a destination, not an action to perform on success.
- Unlocking one game this way should not unlock everything, and should not leave the code
  "entered" for the session. Decide whether a correct entry has any memory at all, or whether
  every lock asks again — the second is more annoying and much easier to reason about.

## 4. Settings

**Add a way to set the clock — done.** `screen_clock.c`, reached from Settings, five fields and a
read-back check. Not `rtc_set()`: the pinned libdragon deprecates it in favour of
`settimeofday()`, and `rtc_is_writable()` really is an inline `return true` carrying a deprecation
note that says to assume it. See AUDIT.md 2d, including the 15.5 KB the date formatting cost.

**Remove three rows — done.**

Both toggles were removed by fixing them at the value they already defaulted to, so no existing
card changes behaviour and there is nothing to migrate.

- *Keep saves in a saves/ folder* — was `true` by default (`settings.c:18`). Saves now always go
  in a `saves/` folder **beside the ROM**. Beside it rather than one folder for the whole card:
  the save is named after the ROM file, so a single shared folder would hand two differently-filed
  copies of the same game one `.sav` between them.
- *Fast reboot back to the menu* — was `false` by default (`settings.c:31`), and **the label was
  backwards**: it set `BOOT_MODE_ROM`, so Reset re-ran the *game* and left no way back to the
  menu short of a power cycle. Nothing now calls `flashcart_set_next_boot_mode()` at all, so Reset
  always returns here. `CART_LOAD_ERR_BOOT_MODE_FAIL` and `CART_LOAD_ERR_FUNCTION_NOT_SUPPORTED`
  became unreachable with it and went too.
- *Storage* status row — dropped.

**Kept:** the Library count, the Cheat database count, and the Build line.

The suite reported byte-identical hashes across all fifteen scripts for this change, which was
not a pass — **no script screenshotted the settings screen**, and `parental.txt` reached Parental
controls with `press down x4` against a cursor that clamps, so it went on landing there after the
row moved from index 4 to index 2. Both are fixed: the script presses exactly twice and dumps the
settings screen as its first frame, and that frame does move (`acea4e13a62166a5` before,
`1e83b575be18bd6a` after).

## 5. Remove the `cartridge` theme — done

Left in the working tree. One consequence to carry: it was the **only light palette**, and it is
what exposed the font-style bug in AUDIT 1x. With it gone, nothing in the theme set can catch
that class of regression again, and `demo-themes.txt` now says so.

## 6. Shipped cheats need editing, not just new ones — done

Z on the cheats list opens the editor on whatever is under the cursor. Saving writes a user cheat,
and a user cheat whose name matches a group already in the set **takes it over** — same name, same
position, same remembered tick, new codes. See AUDIT.md 2c for the measurements and for the three
harness problems it uncovered.

Two things carried forward:

- The stored name cap is now 64 characters and the typed width is 23; a longer name is shown as a
  label with only its codes editable. Sized from the corpus, not the screen: 23 characters covers
  80.5% of 228,209 cheat names and 63 covers 99.4%.
- `MENU_CACHE_FORMAT_VER` went 1 → 2, so every cache file rebuilds. Free today because none has
  ever been written to a real card — but the same bump on a card in the field would silently drop
  favourites, locks and play history, and there is nothing that would tell the user why.

---

## Order suggested

1 and 5 are independent of everything. 4's removals are trivial; 4's clock is a prerequisite for
3's countdown and backoff. 2 is the largest and the only one that can regress the scan, so it
wants its own measurement pass. 6 is self-contained.

    5 (done) -> 4 removals (done) -> 1 (done) -> 6 (done) -> 4 clock (done) -> 3 -> 2
