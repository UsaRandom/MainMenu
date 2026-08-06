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

## 2f. The whole card is searched, and one folder is ours

`/menu` meant two things — where the menu writes its own state, and where the user is expected to
put content. Split into two rules.

**Menu-written state goes to `/mainmenu/`, and is looked for nowhere else.** `config.ini`,
`cache/*` and the new `parental.ini`. Nothing migrates: every one of those is derivable from the
card, so a card that had a `/menu` folder rebuilds into `/mainmenu` on first boot and the old
folder is left alone. Deleting folders on someone else's card is not ours to do.

**Content is probed in three places** — `/mainmenu/<x>`, then `/<x>`, then `/menu/<x>` — for
`cheats.db`, `emulators/<core>` and the art pack. Three `stat`s at launch, no new scan state, and
an existing card keeps working with no action at all.

Measured rather than assumed, by moving `cheats.db` and rebuilding the fixture for each position.
The settings screen's "Cheat database" row is the readout:

| `cheats.db` at | `clock 00` |
|---|---|
| `/mainmenu/cheats.db` | `b63d632c0a55882f` |
| `/cheats.db` | `b63d632c0a55882f` |
| `/menu/cheats.db` | `b63d632c0a55882f` |
| `/roms/nowhere/cheats.db` | `0c643128841e21ad` |

The fourth row is the point: without a negative control, three identical hashes are equally
consistent with the probe finding nothing anywhere.

**The scan root moved from `/roms` to `/`.** Someone who empties a zip onto a card should get a
working menu, and someone who has kept their collection in a folder of their own naming should not
have to rename it.

NEXT.md asked whether a recursive walk of a whole card stays affordable. On this fixture it costs
nothing measurable:

| scan root | titles | total | per ROM |
|---|---|---|---|
| `/roms` | 48 | 998,852 µs | 20,809 µs |
| `/` | 48 | 992,760 µs | 20,682 µs |

0.6% apart, and in the wrong direction to be a cost — it is noise. **This does not answer the
question for a real card.** The fixture root holds three entries, two of which are excluded, so
what was measured is that the exclusions work, not that a 32 GB card with an unrelated folder tree
is affordable. `tools/mksdmirror.py` exists for that and needs a real card. Still open.

**The exclusion list is load-bearing, not an optimisation.** `library.c`'s `SCAN_SKIP` refuses
`mainmenu`, `menu`, `metadata`, `emulators`, `saves` and `System Volume Information` at every
depth; the leading-dot test already covered `.Spotlight-V100`, `.Trashes`, `.fseventsd` and
AppleDouble `._*` files, which carry the real file's extension and would otherwise index as a
second copy of every game.

Two of the entries were found only by trying the change against a real card rather than the
fixture. `emulators` is the first. `neon64bu.rom` is a NES core and `.rom` is an N64 extension;
`gb.v64`, `gbc.v64`, `lithium64.z64` and `smsPlus64.z64` are all ROM extensions too. Proved by
putting the cores at `/emulators` and building both ways: **48 games with the exclusion, 53
without** — every core listed in the N64 tab as something to play. Under the old `/roms` root they
were never in reach, so rooting the scan at `/` is what created this and nothing else would have
caught it.

**A bug found by reading, that ares cannot catch.** `libindex.c`'s signature walk — the thing that
decides on every boot whether the index is still good — did *not* apply the exclusion list, though
its own comment claimed it visited "the same entries `scan_dir()` visits". Rooted at `/` it would
have walked the art pack and `mainmenu/cache`, and `mainmenu/cache` is rewritten on every boot: the
writability probe alone is created and deleted inside it. So the signature would have differed from
the stored one **every single time**, the index would have been thrown away on every start, and the
full scan the index exists to avoid would have run for ever — a permanent 1-second boot regression
on exactly the cards that work properly. Under ares the DFS is read-only and nothing in there ever
moves, so the walk looks perfectly stable. `library_scan_skipped()` is now shared by both walks.

The second is the menu's own ROM. `sc64menu.n64` sits at the card root and ends in `.n64`, so
rooting the scan at `/` put this program in its own N64 tab, offering to boot itself. Proved the
same way — a `sc64menu.n64` planted in the fixture root gives **48 titles with the name skipped and
49 without**. `menu.bin`, `OS64.v64` and `OS64P.v64` are skipped beside it: this fork supports
neither the ED64 nor the 64drive, but a card that has been used with one still has the file.

**`make clean` used to destroy things nothing can put back.** It was `rm -rf build/`, and `build/`
is also where the fetched cheat corpus lives (1,345 files) and where `build/artcache` lives —
which has to be populated by hand and which *nothing fetches at all*. Losing artcache is the
AUDIT 1w trap firing silently: every fixture cover becomes a procedural gradient, every decode
number moves, and `real-art.txt` and `jpeg-art.txt` go on passing against gradients. `clean` now
keeps `cht`, `cheats.db` and `artcache`; `distclean` is the old behaviour, named so nobody reaches
it by reflex. This was found the direct way, by running `make clean` and losing them.

Still unexercised: the fixture carries no `library.idx`, so the *fresh* path — signature walk
matches, scan skipped — has never run under ares at all. Everything above about it is reading.

## 2e. Parental controls: six presses, its own file, and a wait that survives the power switch

**Six C presses, not four of eight.** The alphabet is the four C directions and nothing else, and a
code is six of them: 4⁶ = 4,096, exactly what four presses of eight buttons gave. Nothing is lost
and three things are gained — it is simpler to say out loud to the other parent, it frees A, B, Z
and the shoulders to go on meaning what they mean everywhere else, and the D-pad ambiguity goes
away (a D-pad arrow and a C arrow are the same picture).

**The code, the failure count and the schedule moved out of `config.ini` into
`/mainmenu/parental.ini`.** Forgetting the code is now recovered by deleting one file, and that
recovery costs nothing else the parent has set — which is only true if the file holds nothing else,
which is why the schedule moved with it. No master code to build, document or defend. No code set
means no enforcement, so deleting it also releases the locked games; their `LIBF_LOCKED` flags stay
in `playstate.dat` and come back the moment a code is set again.

**It deliberately does not go through `cache.c`.** Every file that layer writes is a cache, and a
version mismatch deletes it and rebuilds from the card. There is nothing on a card to rebuild a
code from, so routing this file through it would mean the next routine `MENU_CACHE_FORMAT_VER` bump
silently unlocked every locked game on every card in the field. It is written with `ini_save()`
instead — the same writer `config.ini` uses.

**A wrong guess costs 5 s more than the last, capped at 10 minutes.** Three properties, and each
one is load-bearing:

- **The count is written before the guess is compared.** `guess()` in screen_code.c calls
  `parental_note_attempt()` first, always. The other order makes pulling the power on a wrong
  answer free and the whole thing collapses to nothing.
- **A correct entry clears it.** This is what makes a counter viable with no clock — nothing has to
  expire, because the person who knows the code clears it every time they use it.
- **Ten minutes is a ceiling, not politeness.** Uncapped, a child who cannot get in leaves a few
  hundred failures behind and the parent waits forty minutes: the feature turned against its owner.
  Ten minutes still makes 4,096 combinations hopeless.

The countdown lives in `parental.c`, not in the pad, so leaving the screen and coming back does not
clear it and a console reset re-arms the full wait from the stored count rather than skipping it.
It is accumulated frame time — no clock anywhere in it. B still works during the wait: someone who
opened the screen by accident must not be held on it for ten minutes, and letting them go costs
nothing because the wait is not the pad's.

Verified end to end by `parental.txt`: one wrong sixth press, and frame 09 shows six dimmed dots,
"Wrong code", and "Try again in 5 seconds". The correct code afterwards launches.

**C-left locks or unlocks the game on the detail sheet**, asking for the code in both directions.
Locking needs it too — otherwise a child can pad every game on the card with locks the parent then
clears one at a time. This is the only request that carries an *action* through the pad rather than
only a destination: the sheet is rebuilt on the way back and its cursor may be on a different game
by then, so `screen_code_ask_toggle_lock()` records the `rom_id` and the pad applies it.

**Setting the clock is behind the code too, and only once there is one.** The schedule is enforced
against this clock and nothing else, so a child who can set the time can move bedtime and the
"Playing allowed 8 am to 8 pm" row is decoration. With no code set the clock opens straight away --
making everyone key in six presses to correct the date on a console whose owner never asked for
parental controls would be a cost with no matching benefit.

`clock-locked.txt` covers the gated half and `clock.txt` the ungated one, which together are what
make the *IFF* real: adding the gate moved **none of the 72 existing frames** and added three. Had
it fired unconditionally, `clock.txt` frame 01 would have become the pad instead of the date
fields. Its own frame 00 hashes `bf846a4fe5d4fcb4`, identical to `parental 03` — two scripts
arriving at the same screen by different routes.

**Deleted with no replacement anywhere:** "A lock on a menu, not security. See the manual." It is a
games menu; nothing here makes a claim that needs qualifying, and qualifying it invites the reader
to go and test the claim.

**A harness fault this uncovered, and it was reading noise as evidence.** ares hands the console the
host's wall clock, so the settings screen's Clock row — and every field the clock screen seeds from
it — came out different on every run. Two back-to-back runs of `clock.txt` disagreed on **all four**
of their frames, and `parental.txt`'s first frame moved with them. Those hashes had never been
evidence of anything. Fixed by pinning the clock to 2026-08-04 14:30 UTC when an input script is
driving, exactly as the fixed `dt` in 1z pins the frame time; it compiles out without
`DEV_HARNESS`. The suite now reproduces byte-for-byte across two full runs, 72 frames over 16
scripts, which it demonstrably did not before.

## 2d. The clock can be set, and libdragon's writability check is a lie by design

There was no way to set the time. `last_played`, the parental schedule and everything that wants
to say how long ago something happened all read a clock nobody could correct. `screen_clock.c` is
five fields — year, month, day, hour, minute — reached from Settings, whose row shows the current
time rather than the word "Clock" twice.

**Not `rtc_set()`.** The pinned libdragon deprecates `rtc_get`, `rtc_set` and `rtc_is_writable` in
favour of the ISO C ones: `rtc_init()` hooks the clock into newlib, so `time()` reads it and
`settimeofday()` writes it. `rtc_is_writable()` deserves naming — it is

```c
__attribute__((deprecated("just assume it's always writable")))
static inline bool rtc_is_writable( void ) { return true; }
```

an unconditional yes. NEXT.md called it "not a real check" from the header alone and that is
exactly right. So the screen **reads the clock back** after writing, with two seconds of tolerance
because the clock is running while it happens, and says so if the value did not take.

`rtc_get_source()` is the honest signal for whether a set survives a power cycle:
`RTC_SOURCE_NONE` is libdragon's software clock. The screen says which one is in play rather
than letting a parent set a bedtime that evaporates. Under ares the source is *not* NONE, so the
screen reads "Kept by the cartridge's clock" — which is what the API reports and is the best signal
available; whether it is true is a hardware question and stays open.

That line first read "the console's clock", which is wrong and points the reader at the wrong
hardware. **A stock N64 has no clock.** `RTC_SOURCE_JOYBUS` is libdragon's name for the PIF/
controller-port protocol, and on this machine the device answering it is the flashcart — so the
battery that keeps the time is on the cartridge, and that is what someone should go and check when
the date comes back wrong.

Day clamping is not politeness either: 31 January with the month stepped to February is 31
February, and `mktime()` resolves that silently to 2 or 3 March — the screen would accept one date
and the clock would hold another.

**No timezone and no DST, and that is the whole design.** Nothing sets `TZ`, so newlib runs with a
zero offset and no daylight rules: `localtime()` is `gmtime()`, and `mktime()` is its exact
inverse. The clock therefore holds the digits the user typed and nothing reinterprets them.
Measured rather than reasoned — `clock.txt` enters 2032 Nov 1 15:53 and the settings row afterwards
reads `01 Nov 2032  15:53`, so no offset is applied on either side of the write. The consequence a
user sees is that the hour has to be retyped twice a year in a country that shifts, which is what
every console of this era did. The consequence for the code is that the parental schedule can
compare `tm_hour` against a stored hour with no conversion anywhere, and that anything that ever
sets `TZ` breaks that silently in both directions at once.

**Cost: 15.5 KB of text and 2.1 KB of data**, 524,888 → 540,440 and 98,132 → 100,220. That is
`strftime`, `localtime` and `mktime` dragging in newlib's date machinery, and it is most of a
screen's worth of code for three format calls. Affordable on 8 MB and worth knowing before anyone
adds a fourth date format somewhere else expecting it to be free.

Verified against the whole suite: exactly one frame moved, `parental 00`, which is the settings
screen that gained the row. Nothing else in 67 frames changed.

## 2c. Shipped cheats become editable, and three scripts turn out to have been photographing the wrong thing

Z on the cheats list opens the editor on whatever is under the cursor, shipped or hand-entered.
`cheats.db` is read-only, so saving always writes a user cheat — and **a user cheat whose name
matches a group already in the set takes that group over** rather than appearing beside it. The
group keeps its name, its position and its `cheatstate` tick; only its codes change. Verified end
to end: `tools/inputs/cheat-edit.txt` opens AeroGauge's "Unlock Hidden Tracks & Cars", changes
`8009127C 0001` to `8009127C 0006`, saves, and comes back to a list still **43 long** with the same
name in the same place.

### The name limit was set by the screen and should have been set by the corpus

The first attempt sized the stored name at what the editor's cell strip can show — 23 characters,
`USERCHEAT_NAME_CAP` 24 — and refused anything longer, because a truncated name no longer matches
the group it came from and the edit would land beside the original instead of replacing it. The
refusal was correct and the limit was not. Measured against the fetched libretro corpus, over
**228,209 cheat names**:

| fits in | share |
|---|---|
| 15 chars | 57.7% |
| 19 | 73.2% |
| **23** | **80.5%** |
| 31 | 91.7% |
| 39 | 96.2% |
| **63** | **99.4%** |

Longest is 197; the median is 13. So the original limit refused one cheat in five, and the very
first one the script landed on — "Unlock Hidden Tracks & Cars", 27 characters — was refused.

The fix separates two things that were wrongly one: how long a name can be **stored**
(`USERCHEAT_NAME_CAP`, now 64, record 108 → 140 bytes) and how many characters can be **typed**
(`NAME_CELLS`, still 23, which is a screen-width fact — 23 × 20 px against SAFE_W's 608). A name
too long for the strip is shown as a label and only its codes are editable. Nothing is refused for
length any more short of 63 characters.

Line counts needed no such change: **99.5%** of groups are 8 lines or fewer, which is what
`USERCHEAT_MAX_LINES` already allowed. The longest is 182.

### Three things found on the way, two of them silent

**The editor's error message had never been visible.** It was drawn at `LINES_Y + 8*ROW_H + 8` =
y 440, and `FOOTER_Y` is 424 — the footer is filled *after* the message, so every refusal to save
was painted over before the frame was shown. It now shares the header's right-hand slot with the
line counter. Nothing caught this because a refusal draws no other pixels.

**`realloc(NULL, 0)` may return NULL, and `grow_set()` could not tell that from being out of
memory.** Overriding a shipped group asks for zero name bytes, and on a game with no prior user
cheats `user_strtab` is NULL — so the first override on any such game would have reported a failed
edit. Caught before running, by reading the path the new call takes rather than the one it used to.
The strtab realloc is now skipped when the total is zero.

**`cheats.txt` had never once drawn the cheats screen.** It opened `wait 90`, and the boot plate is
still up at frame 90, so `press right` and `press a` were swallowed and all five of its frames were
the same picture of the grid. It is the script named for the feature, it asserts in its own header
that "the sheet's N of M enabled and the list agree", and it had been passing its hash check while
photographing a tab bar. Now `wait 140`, and it reaches AeroGauge's 43 cheats and its sheet reading
`2 of 43 enabled`. `press r  # page down` was stale in the same file — R became Add when the editor
needed a button.

Same family as 2b: a script that navigates by a count it cannot verify, or acts before the screen
it names exists, produces frames that look like a result and are not one.

### The whole suite had been running against no cheat database at all

`build/cheats.db` is a release artifact, gitignored, and was simply absent on this machine —
so `cheats.txt`, `cheat-leak.txt` and `cheat-entry.txt` were all exercising the empty-list path and
`cheatdb_load()` was never returning a group. Fetched: **1,345 `.cht` files → 325 games, 37,638
cheats retained, 18,211 dropped** (1,527 KB). The Makefile stages it when present, so the suite now
covers the populated path. Worth stating plainly: the cheats screens have been measured for the
first time here, and any earlier claim about them rests on an empty database.

### One number for all the caches has a host-side half nobody had wired up

`MENU_CACHE_FORMAT_VER` 1 → 2 for the record growth, which is the documented behaviour — one number
for every cache file, everything rebuilds together, and it costs nothing today because no cache has
ever been written to a real card. But `tools/mkplaystate.py` carried its own `FORMAT_VER = 1` with a
comment saying it must match, and it stopped matching. The symptom was not an error: the menu
rejected the fixture's `playstate.dat`, Recent and Favourites went empty and were therefore hidden,
the grid opened on N64 instead of Recent, and **every script that steps right through the tabs
quietly visited different games**. Two runs of `cheat-edit.txt` an hour apart opened different
titles for this reason. `mkplaystate.py` now reads the value out of `cache.h` instead of holding a
copy of it.

## 2b. Two settings removed, and a blind spot that made the suite agree with itself

Three rows left the settings screen: *Keep saves in a saves/ folder*, *Fast reboot back to the
menu* and the *Storage* status line. Both toggles were removed by fixing the behaviour at the
value it already defaulted to — `use_saves_folder` was `true` (`settings.c:18`) and
`rom_fast_reboot_enabled` was `false` (`settings.c:31`) — so no card in existence changes
behaviour and there is nothing to migrate.

The fast-reboot row was also **mislabelled in the direction that matters**. It set
`FLASHCART_REBOOT_MODE_ROM` → `BOOT_MODE_ROM` (`cart_load.c:134`, `sc64.c:728`), which makes the
console's Reset button re-run the *game*. A row called "fast reboot back to the menu" was the one
setting that took the menu away and left no route back short of a power cycle. Nothing now calls
`flashcart_set_next_boot_mode()`, so Reset always returns here;
`CART_LOAD_ERR_BOOT_MODE_FAIL` and `CART_LOAD_ERR_FUNCTION_NOT_SUPPORTED` became unreachable and
went with it.

### The suite reported byte-identical hashes, and it was wrong to

All fifteen scripts, every frame, unchanged — against a screen that had just lost two of five
rows and a status line. The house rule is that a green result is worth nothing until the setup has
been shown capable of going red, and this is what that rule is for. Two compounding causes:

**No script ever screenshotted the settings screen.** Five scripts press Start, none dumped a
frame there. The screen was simply outside the suite, so of course nothing moved.

**`parental.txt` navigated by over-pressing.** It reached Parental controls with `press down x4`
against a cursor that clamps at `ROW_COUNT - 1`. The row moved from index 4 to index 2 and the
script went on landing on it exactly, so even the frames downstream of the settings screen were
unaffected. Clamped navigation makes a script insensitive to the layout it is navigating — it
cannot tell "the row is where I think" from "the row is somewhere at or past where I think".

Both fixed: the script presses exactly twice, and dumps the settings screen as its first frame.
The new frame does move — `acea4e13a62166a5` with the old rows, `1e83b575be18bd6a` without them —
verified by stashing only `src/` and running the same script against both, so the hash difference
is attributable to the code and not to the script edit.

Worth carrying: **a hash suite is only evidence for the pixels it renders**, and "nothing moved"
after a UI change is a prompt to ask whether the change is on screen anywhere, not a result. Same
family as 1j and 1u — an instrument that cannot go red reporting green.

## 2a. The write path answered by reading the firmware instead of guessing

`Polprzewodnikowy/SummerCart64` @ `a1e7996` (`v2.20.2-25-ga1e7996`), cloned to
`../SummerCart64`. The standing question — *can the menu write to the SD card* — is answerable
from source, and the answer is **yes, and it needs nothing the read path does not already have.**

### The chain, top to bottom

| layer | what it does | verdict |
|---|---|---|
| `flashcart.c:157` | `debug_init_sdfs("sd:/", -1)` | mounts FatFs on the cart's SD |
| `libdragon/src/fatfs/ffconf.h:14` | `FF_FS_READONLY 0`, `FF_FS_MINIMIZE 0` | writes and `f_unlink` compiled in |
| `libdragon/src/fat.c:493` | `newlib_fs->mkdir = __fat_mkdir` → `f_mkdir` | `directory_create()` can make `menu/cache` |
| `libdragon/src/debug.c:215` | `fat_disk_write_sd` → `cart_card_wr_dram` | FatFs's `disk_write` is real, not a stub |
| `libcart/cart.c:1759` | `sc_card_wr_dram`: DMA to `SC_BUFFER_REG`, then `SD_SECTOR_SET` + `SD_WRITE`, 16 sectors at a time | handles unaligned buffers via `__cart_buf` |
| `libcart/cart.c:100` | `__cart_dma_wr` does `data_cache_hit_writeback` first | **no manual cache flush needed in our code** |
| `sw/controller/src/cfg.c:725` | `CMD_ID_SD_WRITE` — count `< 0x800000`, source in `SDRAM\|FLASH\|BRAM`, `sd_get_lock(SD_LOCK_N64)` | |
| `sw/controller/src/sd.c:533` | `sd_write_sectors` — CMD25 multi-block, CMD12 stop | |

**The decisive detail is that `CMD_ID_SD_READ` and `CMD_ID_SD_WRITE` are gated on the same
`sd_get_lock(SD_LOCK_N64)`, translate their address the same way, and share the same count limit.**
The lock is taken once by `SD_OP_INIT`, which `libcart`'s `sc_card_init()` issues and which
`fat_disk_initialize_sd` calls at mount. So there is no separate permission, no config flag and no
extra initialisation for writing. **If the menu can enumerate the card, it can write to it.** The
only way to hold the lock away from the N64 is `SD_LOCK_USB`, i.e. the deployer talking to the
cart at the same time.

### "Every write is soft" — verified, not asserted

CLAUDE.md claims every write fails softly. That was a claim about our own code, and it depended on
an assumption about the layer underneath. `CART_ABORT()` is
`{__cart_acs_rel(); return -1;}` (`libcart/cart.c:29`) — it releases the bus and **returns an
error**. It does not hang, assert or reset. So a card pulled mid-write propagates
`-1` → `RES_ERROR` → `FR_DISK_ERR` → a short `fwrite` → `cache_store()` deletes the partial file
and returns false → the menu carries on. The discipline holds all the way down to the cart.

### Two things worth knowing before hardware

- **The menu refuses firmware older than 2.17.0.** `sc64.c:28-30` requires major == 2 and
  minor >= 17, and returns `FLASHCART_ERR_OUTDATED` otherwise — which lands on the fault screen,
  not the grid. Upstream's HEAD is 2.20.2, so any recently-shipped cart is fine, but **reading the
  firmware version is the cheapest possible first hardware check** and it explains one whole class
  of "it does not boot".
- **`sc64_init` waits on `writeback_pending` with no timeout** (`sc64.c:279-285`). It does
  terminate: `writeback_process()` clears `pending` unconditionally after an SD writeback attempt
  and disables writeback outright if the card is gone (`writeback.c:163-184`), and the delay is
  `WRITEBACK_DELAY_MS` = 1000. So worst case is about a second. Only the USB writeback mode can
  spin, and the menu never enables it.

### A shared buffer that is safe for a reason, not by design

`SC_BUFFER_REG` in libcart is `0x1FFE0000`. `SC64_BUFFERS_BASE` in our own driver is
`0x1FFE0000`. **They are the same 8 KB of cart BRAM**, so every FatFs transfer reuses the exact
memory `sc64_set_save_writeback()` stages the save's sector table in.

That is safe, twice over, and neither reason is obvious from our side:

1. `sc64_set_save_writeback()` builds the table in a **stack** array first — `fatfs_get_file_sectors`
   does its own SD reads through that BRAM while walking the cluster chain — and only DMAs it into
   the buffer immediately before enabling writeback.
2. `writeback_load_sector_table()` (`writeback.c:127`) does `fpga_mem_read` into the MCU's own
   `p.sectors` **at once**. Once `WRITEBACK_SD_INFO` returns, the buffer is free.

This matters to us specifically because of an ordering we introduced: `app_deinit()` writes
`library.idx`, `playstate.dat`, `cheatstate.dat` and `usercheats.dat` **after** `do_load()` has
already armed writeback and staged the ROM. Every one of those writes runs through that same
buffer. Had the MCU read the table lazily, each launch of a game with a save would have written
save data to sector numbers taken from whatever our last cache write left behind. It does not, so
this is a note and not a bug — but it is a dependency on upstream firmware behaviour that nothing
in our tree records, and it would break silently if it ever changed.

The sizes agree, checked rather than assumed: `SAVE_WRITEBACK_MAX_SECTORS` 256 (`flashcart_utils.h:16`)
× 4 bytes = 1024 = `WRITEBACK_SECTOR_TABLE_SIZE` (`writeback.h:9`) = `SAVE_MAX_SECTOR_COUNT` 256
(`writeback.c:10`).

### One asymmetry in libdragon, latent for us

`fat_disk_read_sd` dispatches on where the buffer lives — RDRAM below `0x00800000` goes to
`cart_card_rd_dram`, a PI-accessible address to `cart_card_rd_cart`. `fat_disk_write_sd` has no
such dispatch and always calls `cart_card_wr_dram`. Writing *from* cart SDRAM through FatFs
therefore does not work. Nothing here does that — FatFs writes from its own RDRAM buffers — so it
is recorded rather than worked around. Note also that the RDRAM test is `< 0x00800000`, exactly
8 MB, which is the whole of an expanded machine and therefore fine on the M64.

**What this does NOT establish.** That the card can be written says nothing about throughput. The
streaming budget in the thumbnail design is still the one number ares cannot give us, and
`tools/hosttest/run.sh` still only proves the formats round-trip on a PC. Reading the firmware
moved the question from *"is it possible"* to *"how fast, and does our code get it right"* — the
second half of 1r is unchanged.

---

## 1z. Parental controls, hand-entered cheats, a crash on any title containing `^`, and a hash gate that could not tell code size from drawing

Four things, found in that order, each while doing the one before it.

### The feature

A four-button code, a per-game lock, and an optional window of the day. `src/menu/parental.c`
holds the policy; `screen_code.c`, `screen_parental.c` and `screen_locks.c` are the UI.

**Locked, never hidden**, which is what made it cheap. Hiding is a filter and a filter touches
every tab view, the position counter, Recent, Favourites and the opening-tab logic. Locking is one
predicate at one call site — `screen_detail.c`'s A/Start handler is the only place in the menu
where a game starts. The tile keeps its art and gains a padlock, so pressing A is an informed
choice rather than a surprise.

Four decisions worth not relitigating:

- **The code lives in settings, the locks live in playstate.** `settings.c` writes through
  `ini_save()`, upstream's own writer; the locks ride `src/library/cache.c`, which has never run
  against real storage (1r). So on a card the menu cannot write, the code survives a reboot and the
  locks do not. That is worse than the same exposure on favourites — a lost favourite is an
  annoyance, a lost lock is the feature silently not working — so the panel says
  *"Locked games are not saved to this card"* when `cache_writable()` is false rather than
  accepting a lock it knows it will drop.
- **The panel is itself behind the code.** The schedule is enforced against a clock this menu can
  set, and the lock list is edited here. A parental panel reachable without the code is one a child
  switches off, and that is the entire feature in one sentence.
- **The window wraps midnight, and the wrapping case is the normal one.** 20:00 to 07:00 is what a
  parent actually sets, and `h >= from && h < to` is false for every hour of the day when
  `from > to`. `from == to` means no restriction rather than a locked-out console.
- **No clock fails open, and says so.** Same discipline as `play_timestamp()`: `rtc_init()` returns
  false when there is no RTC source, and a schedule that failed closed on a dead clock would lock a
  family out of a menu they never asked to be locked out of.

The code is stored as a hash. 4,096 combinations fall to a four-line script, so this is not a
defence — the realistic attacker is a child who opens `config.ini` in Notepad, and
`code = 3F2A91C4` tells them nothing where `code = AZLR` tells them everything. **Nothing here
should be described to a user as security**, and the panel says as much on screen.

The alphabet is A, Z, L, R and the four C directions. B is excluded so it can always mean "delete
that press" without a mode; Start is excluded because it is the button that opens Settings and a
code beginning with it would be entered by accident; the D-pad is excluded because a D-pad arrow
and a C arrow are the same picture. `input_t` gained `BTN_CUP`/`BTN_CDOWN`/`BTN_CLEFT` and
`mkinput.py` gained `press cup` and friends — **not for the console, for the harness**. Without
them `tools/inputs/parental.txt` could not key a code and the feature would have shipped with no
scripted test. `input_event_t.buttons` went from `uint8_t` to `uint16_t` at the same time; the new
bits are 7, 8 and 9, and the old width would have truncated two of them into nothing with no
warning from either the generator or the compiler.

### Cheats typed in by hand

`src/cheats/usercheats.c` and `screen_cheatedit.c`, reached with R from the cheats list. The
shipped corpus covers a few hundred N64 titles and nothing else — no NES, no SNES, no homebrew, and
nothing published after it was built — so for most of a card this was the only missing half of the
cheats feature.

**They are groups, with the same indivisibility rule as everything else.** A user cheat is a named
group of up to eight lines and the list can only toggle the group. This is the one place a person
could otherwise have built half a `D0`/`80` pair by hand, which is the failure 2.2 records; the
editor adds and removes whole lines within one group and never offers a line as a thing to enable.

**One edit mode, not two.** The obvious design is a hex keypad plus a separate keyboard for the
name — two grids, two modes, a lot of travel per character. Instead the name's characters and each
line's twelve nibbles are **one strip of cells**: left/right moves a cell and wraps across rows,
up/down changes the value under it, L/R jump a whole row. It is the arcade high-score idiom, which
is the one text-entry pattern a console player already knows, and there is no mode to be in the
wrong one of.

Two things the editor refuses rather than accepts quietly. A blank line is not a cheat —
`00000000 0000` is a write to address zero and `boot/cheats.c` would assemble it — and a name that
is empty or all spaces would draw as a blank row and hash as a different cheat every time a
trailing space came and went, so both are rejected with the reason on screen.

Records are fixed at 108 bytes with the name and lines inline, and `line_count` is clamped on the
way in as well as on the way out: the file sits on a card the user can edit, and a stored count of
60,000 would walk a loop off the end of a fixed array. `cheatstate` needed no changes at all —
it keys on the group name, so a hand-entered cheat's tick is remembered by the same mechanism.

The one real trap was in the append. `cheat_group_t::name` is a pointer, and growing the set
reallocs the string table under every name already in it. Repointing them needs to know where the
user groups begin, and telling them apart by comparing pointers into two unrelated allocations is
not something C promises to answer — hence `cheatset_t::user_first`.

`tools/inputs/cheat-entry.txt` types a two-line cheat in, saves it, and toggles it. Two lines
rather than one deliberately: the group model exists for the multi-line case.

### `^` and `$` in a drawn string take the menu down

`ui_button(..., "^", ...)` on the code pad asserted immediately:

```
ASSERTION FAILED: !error
file "src/rdpq/rdpq_paragraph.c", line 537, function: __rdpq_paragraph_build
invalid style id:    at position 0 (font id must be two hex digits)
```

`$XX` selects a font in rdpq_text and `^XX` selects a style, and a malformed pair is not ignored —
`__rdpq_paragraph_build` calls `assertf`, which on this target is a hard drop into the inspector.
**Every string this menu draws goes through that parser, and they include ROM filenames, ROM header
titles and cheat names from a corpus nobody here wrote.** A game called `Foo^Bar.z64` would have
taken the grid down the moment its tile scrolled into view, and a cheat named `Infinite $$$` would
have taken down the cheats screen.

Nothing in this tree uses the markup deliberately — checked before choosing the fix — so `ui_text()`
now doubles both characters unconditionally, which is rdpq's own escape. One place, no source to
sanitise, and nothing to preserve. The caret also needed four pixels of baseline in `ui_button`:
it is the only glyph on a button whose ink sits entirely at the top of the cap box, and the
baseline that centres a capital pushed it out through the top of the disc.

This is a **latent crash that predates the feature** and was found by accident. There is no test
for it yet; the fixture harvests real game codes and none of them contain either character.

### The hash gate moved on changes that drew nothing

Ten of the suite's thirteen scripts changed hashes after the sheet cleanup — including `idle`,
`grid-edges` and `tabs`, which exercise none of the code that changed. Each moved by **exactly 104
pixels of exactly one colour pair**, one step apart on the RGBA5551 ladder, in the rectangle around
the selected tile.

That is the selection outline, which pulses on `phase += 6.0f * dt`. `dt` comes from `TICKS_READ()`,
so the phase at a given frame is a function of how many CPU cycles the frames before it took — which
is a function of the binary. Measured rather than assumed: inserting a `volatile int[64]` that
nothing reads into `app_init()` moved `grid-edges` frame 00 and moved frame 01 **back** to the value
it had two builds earlier.

The M1 reproducibility gate passed throughout, because the same binary always produces the same
cycle counts under ares. What was broken is the comparison the suite exists for —
`diff before/hashes.txt after/hashes.txt` could not tell *the drawing changed* from *the binary got
bigger*, and a red result you learn to ignore is worse than no result.

**Fixed by giving a scripted run a fixed `dt` of 1/60 s** (`app.c`, guarded on
`inputscript_active()`, which compiles to `false` without `DEV_HARNESS`). The whole run is now a
pure function of the input script, which is the same principle that already keys input events on
frame number rather than elapsed time. It does not weaken "motion is specified in seconds, never
frames": the animation code is untouched and still integrates `dt`; only the clock the harness runs
it against is fixed. Frame-time measurement is unaffected — the field bins use the *unclamped*
interval and `frametime.c` reads `TICKS` directly.

Verified the way round that matters: with the fix in, the same no-op probe produces
**byte-identical** hashes. Two full suite runs after it are byte-identical to each other, 15 scripts
and 62 frames, and `idle.txt` still reports `mallocs=0 reallocs=0` with a flat heap at frame 1200.

**All hashes move once as a result of this change and should.** Every frame containing a tween was
previously recording a number that depended on the compiler.

The gate then earned itself back on the next change. Making the detail sheet's Z hint unconditional
moved exactly eleven frames — the three detail-sheet stills, both `cheat-leak` sheet frames,
`parental`'s sheet frame, and all five `launch` frames, which fade *from* the sheet image because
`draw_fade_into()` composites onto whatever the buffer already held. Nothing on the grid, the tabs,
the settings screen, `idle` or either art script moved. Before the fix that same change would have
moved ten scripts and told nobody anything.

---

## 1y. The detail sheet stops describing the cartridge, and a shipped metadata table is ruled out

The sheet led with three rows a player cannot act on: game code, save type and TV standard. Save
type in particular is only interesting when it is *wrong*, and the sheet already has a row for
that case. They are replaced by a play count and by a cheats row that now draws unconditionally.

**"Cheats" was conditional, and its absence meant two different things.** No row appeared when
`group_count == 0`, which is true both when this game has no codes and when the card has no
`cheats.db` at all. Those want opposite responses from the user and looked identical. It now
reads `None available`, so the second case shows up as *every* game saying the same thing.

**A warning was being clipped into looking like a rendering fault.** `Not in the ROM database`
is 23 characters against an `INFO_W` of 256 px at ~12.2 px/character, and `ui_label` hard-clips
rather than ellipsising, so it drew as `Not in the ROM databa`. It had been wrong since the row
was added and no screenshot had ever contained it: the fixture harvests real game codes out of
`rom_info.c`, so nothing in it can miss the database. The demo tree, where every title misses on
purpose, put it on screen the first time it was used. Shortened to `Not in the database`, which
fits with margin. **`ui_label` still clips silently** — that is a trap, not a fixed bug, and the
next string to outgrow its column will fail the same way.

### The extras table: built, measured, and deliberately not shipped

A shipped database of release dates and ESRB ratings was built and then removed. Recording it so
it is not proposed again, and because the numbers were real:

`tools/mkextras.py` queried **Wikidata** (`P400` = Nintendo 64, `P577` publication date, `P852`
ESRB rating) and joined it to the 432 `MATCH_*` rows in `rom_info.c` on normalised titles, the
same bridge `mkcheatkeys.py` uses. Result: **327 of 432 matched, 323 with a date, 189 with a
rating, 6,572 bytes packed.** Two harness facts worth keeping:

- **`FILTER(lang(?title) = "en")` silently dropped 22 of 474 items**, including Yoshi's Story,
  Mega Man 64, Ridge Racer 64 and StarCraft 64. Wikidata migrated titles spelled the same in
  every language to the `mul` language code. The join reported 288 hits and looked healthy; the
  losses were indistinguishable from the Japan-only titles that genuinely do not match. Accepting
  `mul` and `en-gb` took it to 324. This is the same failure shape as the harness traps in 1u and
  1c — a green result from a setup that was measuring less than it claimed.
- **Ocarina of Time came back rated E10+**, a category ESRB did not introduce until March 2005.
  Wikidata was reporting a re-release's rating for a 1998 cartridge. Detectable only because the
  anachronism is on its face; a game quietly re-rated T to M would have been invisible.

Not shipped, for a reason that is about this repo rather than about the data. The rule here has
been **ship code, never ship corpora**: `build/artcache` is fetched and gitignored, `cheats.db` is
fetched and gitignored, and `n64_keys.tsv` is committed only because it is a bridge that makes a
user-fetched corpus usable. Baking 327 commercial titles into the ROM would have been the first
content corpus that arrives whether the user asked for it or not, and the exception was not worth
carving. The parental-control design that would have consumed the ratings does not need them
either: locking chosen titles behind a code is a per-record flag over the user's own library, with
no external table involved.

`rom_info.c` stays as it is. It is a commercial-title database too, but it is inherited and it is
load-bearing — without the right save type the user's own cartridge dump does not save.

---

## 1x. The themes only ever changed half the screen

Reported as "the cartridge theme is too hard to read the white text". It is not a palette
preference. `fonts.c` registered the rdpq text styles **once, at font load, in fixed colours** --
`STL_DEFAULT` pure white and `STL_GRAY` `0xA0A0A0` -- and nothing ever rebound them. A theme
therefore changed every surface and no text.

That is invisible on the three dark palettes and fatal on the one light one. Under `cartridge`,
`panel_alt` is `0xF7F7EF` and `text` is `0x191919`; the theme was asking for near-black text and
getting white, on every screen, from the day it was added. The settings screen is the worst of
them because it is the most text on the largest panel.

`theme_apply()` now rebinds `STL_DEFAULT` → `text`, `STL_GRAY` → `text_dim`, and both accent
tokens → `text_accent`, and every path that assigns `app->theme` goes through it. Two details
that are not obvious:

- **`STL_YELLOW` and `STL_ORANGE` collapse to one colour.** They were used for the position
  counter and for the "not in the ROM database" note, which are the same role. Keeping two names
  costs nothing; keeping two hardcoded colours meant one of them was wrong under some theme.
- **Button glyphs needed a style of their own** (`STL_ONBTN`, always white). They are drawn on
  controller-colour discs, which `theme.h` deliberately excludes from the theme because they
  describe physical hardware. Binding them to `text` with everything else turned the letters
  black on the blue A button under `cartridge` — caught by looking at `ui_button` before
  changing it, not by the screenshots.

**Every framebuffer hash in the suite moves as a result, and should.** Midnight's `text` is
`0xF7F7FF` and its `text_dim` is `0x9C9CAD`; the hardcoded styles were `0xFFFFFF` and
`0xA0A0A0`. The values the design specified are now the values that get drawn.

`screen_settings.c` also kept **a second copy of the theme list**, so a theme added to `theme.c`
was unreachable from the only UI that can select one. It now calls `theme_count()`/`theme_at()`,
which `theme.h` had exported all along.

Two palettes added, `purple` and `red`, both on the same `round(i * 255/31)` ladder as the
originals so the hex and the packed word agree exactly. Neither accent is a tint of its own
theme -- purple takes pink, red takes gold -- because an accent inside the surface hue is the
failure rule 2 at the top of `theme.c` exists to prevent.

Verified by looking: `tools/inputs/manual/demo-themes.txt` walks all five and dumps the settings
screen and the grid under each at 640 × 480, plus the detail sheet under `cartridge`.

**Still open: the theme does not persist.** `settings_t` has no theme field, so `app.c` assigns
`THEME_MIDNIGHT` at every boot and a choice made in settings survives until power-off. Adding
two more palettes makes that more annoying, not less. Not fixed here.

---

## 1w. Screenshots that can be published, and a still that could not be timed

The README had no pictures, and could not have them: the fixture is built from real game titles
harvested out of `rom_info.c`, and with `build/artcache` populated its tiles are real cover
scans. Neither is publishable. `tools/mkdemo.py` is the answer — 24 invented N64 titles and nine
across the other systems, with box art drawn procedurally from the title string, per-ROM save
types, a play history and a small cheat database of invented codes. `make DEMO=1` packs it in
place of the fixture.

**It is not a fixture substitute, and the Makefile comment says so.** Every title in it misses the
450-game database by construction — `game_code_for()` checks each invented code against the
harvested set and refuses a collision, because a demo ROM that collided would inherit that game's
save type and feature mask and the detail sheet would then show accessories for a game that does
not exist. A scan measured against this tree measures the miss path.

### Cards are emitted at 140 × 98, not 280 × 196

The first version wrote 280 × 196, on the reasoning in DESIGN.md that this is twice the tile so
the detail sheet can show it 1:1. That reasoning does not survive contact with the code:
`screen_detail.c:212` blits **the cached tile** at `scale_x = 2`, so the 280 × 196 panel is a 2×
magnification of a 140 × 98 image either way, and nothing ever reads a larger source. Emitting at
280 × 196 bought a decode of four times the pixels followed by a runtime box-filter down to the
size it should have been in the first place.

Emitting at the target instead means the card goes through one resampler (Lanczos, from the
840 × 588 supersampled canvas) rather than two. The tree also drops from 1,127 KB to 567 KB.

### Two harness gaps this needed

`DBG_FB_MAX_PIXELS` was a flat `320 * 240`, so a full-resolution dump — the only kind worth
putting in a README — was **refused by its own bounds check** and produced no frames at all. It
is now derived from `DBG_FBDUMP_SCALE`, floored at the old value so that scale 2 and 4 keep the
exact heap they had and no allocation measurement in this file moves.

`record on` / `record off` are new script directives that dump every frame. They are latched
state rather than per-frame `fbdump` actions, and that distinction is load-bearing: an event in
`inputscript.c` carries a direction and a button, so a per-frame dump action would mean a
recorded frame could never also be a frame the script was pressing something on, and the video
would drop two frames at every press.

`tools/regress.sh` rebuilds the ROM itself, which silently threw away a hand-built one: the first
capture attempt produced nine **160 × 120 fixture** screenshots from a run set up for 640 × 480
demo ones, and looked entirely successful doing it. It now takes `-m 'VAR=VAL'`.

### The parser had to be replaced, and was checked against the one it replaced

`fbdump2png.py` holds every frame in memory and converts pixels in a Python loop. That is right
for the nine frames a regression script dumps and impossible for 385: 118 million iterations, and
230 MB of framebuffers resident. `tools/mkvideo.py` streams — one frame at a time, numpy for the
RGBA5551 → RGB24 expansion, straight down a pipe to ffmpeg. **1.36 GB of log parses in 9.1 s.**

Verified rather than assumed: over the nine-frame stills log the two parsers produce
**byte-identical** output. And the check was shown capable of failing — replacing the 5 → 8 bit
expansion `(c << 3) | (c >> 2)` with a plain `c << 3` makes it report a mismatch.

### ~~A still of the launch fade~~ — dropped, it cannot be timed

The launch is a fade to black over `DUR_LAUNCH_FADE` = 0.55 s. Dumping at a fixed frame offset
after Play does not land in the middle of it, because `app.c` clamps `dt` to the range
[1/120, 1/15] s: a stretch of slow frames covers the whole fade in **nine** of them and a stretch
of fast ones takes **thirty-three**. Two attempts at the same offset gave pure black (mean pixel
value 0.0) and then the grid it had already handed back to. Recorded because the same trap
applies to any timed animation sampled by frame index — the boot plate's six-frame timeline in
`boot.txt` is exposed to it and has simply been lucky. The video shows the fade in motion.

---

## 1v. The two untested JPEG paths, and a third thing found while testing them

JPEG support was measured against exactly one corpus — the SD card that prompted it, sixteen
files, all baseline colour. Grayscale and progressive were listed as untested in [1p](#1p) and
stayed that way. Both are ordinary "Save As" options, so both will arrive on somebody's card.
Measured now, against the vendored picojpeg on the host:

| | init | components | drains |
|---|---|---|---|
| baseline 4:2:0, 16×16 MCU | OK | 3 | clean, 234 MCUs |
| baseline 4:4:4, 8×8 MCU | OK | 3 | clean, 875 MCUs |
| **grayscale baseline** | **OK** | **1** | clean, 875 MCUs |
| grayscale, DC-only path | OK | 1 | — |
| **progressive, colour** | **UNSUPPORTED_MODE** | — | — |
| **progressive, grayscale** | **UNSUPPORTED_MODE** | — | — |

**Grayscale works and always did.** `m_comps == 1` is the flag `jpeg_fill_band()` reads to
replicate the single plane into R, G and B, and it is reported correctly at both reduce settings.
Nothing to fix; it simply had never been run.

**Progressive cannot be decoded, by design.** picojpeg says so in its own header —
`PJPG_UNSUPPORTED_MODE`, *"picojpeg doesn't support progressive JPEG's"* — and it fails at
`pjpeg_decode_init`, before a single buffer is allocated. The handling was already graceful:
`jpeg_open()` returns, `image_decoder_deinit(true)` frees everything, the record settles at
`ART_NONE` and the tile draws unillustrated. No crash, no leak, no retry.

What was wrong was the diagnosis. It came back as `IMG_ERR_BAD_FILE`, indistinguishable from a
corrupt download, so a user whose art pack happens to be progressive would see a card full of
blank tiles and be sent looking for the wrong problem. There is now `IMG_ERR_UNSUPPORTED` and a
log line naming the actual cause. Three lines, and it is the difference between "convert your
files" and "your files are broken".

### Found while testing that: truncation is completely invisible

A baseline file cut in half **inits fine, yields the same 234 MCUs as the whole file, and ends
with `NO_MORE_BLOCKS`**. picojpeg's read callback reports zero bytes at EOF and the decoder
carries on over the resulting nothing. So a half-downloaded image decodes "successfully" and
draws a part-blank tile, with nothing anywhere in the log to say so.

Deliberately not fixed. Detecting it means scanning back for an `EOI` marker, which some encoders
follow with padding, so it buys a cosmetic improvement on invalid art at the cost of a new way to
reject valid art — a bad trade to make immediately before hardware. It also self-heals: the atlas
is keyed on path and file size, so replacing the file misses the cache and decodes again. The
behaviour is asserted *in the affirmative* in the suite, so that whoever does add truncation
detection is told exactly what they changed.

### The suite

`tools/hosttest/test_jpeg.c`, 23 checks, compiling the real vendored picojpeg against JPEGs
generated by `mkjpegs.py` — generated rather than committed, and generated rather than downloaded
so the suite needs no network. `--mutate` makes picojpeg stop refusing progressive and **4 of the
23 go red**, which is what proves the four refusal checks are pinned to `UNSUPPORTED_MODE`
specifically rather than to "some error happened". Pillow is the one third-party dependency in
the whole host suite; when it is absent the section skips *loudly*, because a skipped section
that reads as a pass is the exact failure this file exists to catch.

---

## 1u. The thumbnail pool never stopped working, and the gate that should have said so could not

Closes the item left open at the end of [1s](#1s). Two defects, found one behind the other, and a
third finding about the test that was supposed to catch them.

### Reproducing it, which took a fixture change

`tools/inputs/idle.txt` waits 1,200 frames and asserts that a frame which is only drawing
allocates nothing. Run against the default fixture it reports `mallocs=0` — and it cannot report
anything else, because at frame 1,200 the run has not finished decoding:

```
FRAME n=1200 ... bg_us=14940 starts=0 rows=60 worstrow_us=26451
```

`starts=0` with `rows=60` is a decoder grinding through a single image, not a settled library.
The real corpus contains the 2118 × 1457 card [1f](#1f) records at 38 seconds on its own, so 40 of
them cannot possibly settle inside 20 seconds of frames. The gate was green because the run never
reached the state the gate is about. That is the third instance of this pattern in the file, after
the two in [1t](#1t), and the first one where the cause was the *corpus* rather than the script.

Rebuilding the same fixture with procedural art instead — 51 records, 40 of them illustrated,
against a 20-slot pool — settles in about five seconds and makes the gate live. It went red
immediately:

| per 60 frames, 1,200 frames after the last input | mallocs | frees | bytes | starts/s | scan µs/frame | f1 of 60 |
|---|---|---|---|---|---|---|
| as found | 66 | 72 | 449,028 | 6 | 1,884 | 38 |
| prefetch stops evicting | 0 | 0 | 0 | 0 | 13,418 | 53 |
| artless tiles stop being wanted | **0** | **0** | **0** | **0** | **0** | **60** |

### Defect one: prefetch evicted, so the pool could never be full

`thumbcache_run()`'s third and fourth passes prefetch over the whole library. `claim_slot()`
always evicted the least-recently-wanted slot, and an eviction hands the evicted record back to
the queue as `ART_PENDING` — so with more titles than slots there was always another candidate.
The cache decoded and evicted forever and never once reached the idle state added in [1l](#1l).
449 KB of 27 KB surfaces churned per second against a heap with no MMU behind it, on a machine
sitting at a menu with nobody touching it.

The fix is one bool. Visible tiles still evict, because the alternative is a blank tile under the
cursor; prefetch only ever fills what is already free, so a full pool ends the walk instead of
restarting it. `no_slot()` distinguishes the two cases — a visible pass that cannot claim must
stay armed for next frame, when the clock has moved on, while a prefetch pass that cannot claim
knows no later pass or frame can do better until an eviction or a new want frees something, and
both of those clear the flag themselves.

This also settles the question [1r](#1r) left open about 500 titles: the steady state of a real
library was the thrashing state, since 500 will never fit in 20 slots either.

### Defect two: a tile with no art was wanted forever

With the pool fixed the allocations stopped and the scan got **seven times worse** — 1,884 µs per
frame to 13,418, with 421 filesystem probes a second. `tc->idle` is cleared by any visible tile
that is not resident, and `thumbcache_get()` was adding artless records to the want list every
frame. A want that can never be met kept the four-pass walk armed permanently. The fixture's
eleven emulated-system stubs have no art at all, so a single screenful contained several of them.

The state is already known — the scan sets `ART_NONE` and caches it — so the check is three lines
in `thumbcache_get()`, which had been ignoring its `lib` argument.

With both fixed the settled grid reports `bg_us=0 scanus=0 stats=0 starts=0` and **60 of 60 frames
single-field**, up from 38. The gate now measures what it says it measures, and the menu does no
work at all when nothing is happening.

### Found while verifying the above: the regression suite is no longer reproducible

The M1 gate in [1b](#1b) is that two identical `regress.sh` runs produce byte-identical
`hashes.txt`, on the grounds that nothing downstream is measurable without it. Run twice against
the same ROM today, it does not: **10 of 47 frames differ**, across `boot` and `browse-roms`.

The cause is that art decoding is budgeted in microseconds. `image_decoder_poll_budget(budget_us)`
finishes however many PNG rows fit in the budget, and how many that is depends on how fast the
host is running ares that second. A frame dumped while art is still arriving therefore catches a
different number of completed tiles from run to run. It is not a bug in the menu — the budget has
to be wall-clock on real hardware — but it does mean **a hash diff on any frame dumped mid-decode
is not evidence of anything**, and the 32-line before/after diff for the fix above is partly this
noise.

So the fix was verified on invariants that are deterministic instead. Across all thirteen scripts:
the `ART` resolution log is byte-identical before and after on twelve of them, and final
`resident=` is 8 on all twelve. The thirteenth is `boot`, which exits mid-decode and resolved one
extra path (`NDKJ`) in the "before" run — the same cut-off sensitivity, and its `ART` log is
identical between two runs of the *same* build.

Not fixed here, and it should be before the next measurement leans on hashes. The shape of the
answer is a harness-only row budget instead of a time budget for scripts whose dumps are aimed at
unsettled states, which would make the dump depend on the frame number rather than the host —
the same reasoning that made input scripts frame-counted rather than time-based in the first
place ([1b](#1b)).

### Not done, and deliberately

`THUMB_SLOTS` is still 20. [1r](#1r) argues it is six times too small and that cluster-aligned CI8
would hold 128 slots in the measured 3.54 MB of free heap, which is a different piece of work with
a different measurement: it changes the tile format, not the eviction policy. What this section
fixes is that the pool never stopped working, which was true at any slot count.

---

## 1t. Fav moves to C-right, and the detail sheet gets one at all

The grid's Fav hint was already labelled, on Z. The **detail sheet had no favourite control of any
kind** — its footer read `S Play` and `Back B`, and `detail_update()` had no handler. So the screen
where you decide whether you want a game was the one screen that could not act on it; you had to
back out to the grid and find the tile again.

Fav is now **C-right on both screens**, drawn as a yellow disc carrying the `>` printed on the pad
itself, labelled `Fav`. Naming the key by its shape rather than by a letter avoids the problem that
nobody calls it "C-right" out loud, and the short label is what lets the same hint fit the sheet's
footer beside Play and Cheats.

**The C-pad is no longer a four-way fast-scroll pad**, and that is the cost. Only up and down
remain. Left and right step one tile in a four-column grid, which the d-pad already does at a
perfectly good rate, so there was nothing there to hurry — down steps a whole row, and crossing a
500-title library is the entire reason the fast pad exists. Dropping left and right is also what
makes C-right safe to use as a button: if it stayed a direction, every press of Fav would step the
cursor one tile right and favourite the game you just left. The diagonals go for the same reason.

Verified end to end in ares rather than by eye on the footer alone: two games favourited with
C-right show corner marks on exactly tiles 0 and 2; the Favourites tab then holds exactly `NAGE`
and `ND4J`; pressing Fav again from inside that tab leaves `ND4J` alone with the view rebuilt under
the cursor.

`tools/inputs/favourites.txt` also turned out to be **navigating to the wrong tab**. It carried
`press l x3`, written when N64 led the rail; since Recent and Favourites took the first two slots
that walks N64 → Favourites → Recent → SMS, and its "Favorites tab holding exactly those two"
screenshot had been of the SMS tab for some time. One `press l` now. A test whose assertion is a
screenshot will keep passing after it stops testing anything, which is the whole hazard of the
form — the frame was there, it just was not the frame the comment claimed.

Note for scripts: Fav is `press cright`, a button. `cpress right` is a C-pad *direction* and now
does nothing.

### The no-art tile drops its system watermark

A tile with no box art drew the system code — `N64`, `SNES`, `SMS` — in large type across its
middle. Gone; the plate is now the `bg_alt` fill, the 2 px border and the title, nothing else.

It was repeating known information in the biggest type on the tile: the tab rail directly above
already says which system you are looking at, because the rail is how you got there. And at tile
scale a single word centred on a card does not read as "this is a Game Boy game", it reads as the
name of the game. An empty plate carrying only the title is honest about there being no art.

### Harness trap: the fixture's playstate.dat was hand-made, and `make clean` deleted it

`build/fixture/menu/cache/playstate.dat` is the only way to reach the cache **read** path under
ares — the DFS is read-only so the menu can never write one, but it loads one that is already
there quite happily. It had been generated by hand with `tools/mkplaystate.py` and never wired
into the build, so a `make clean` removed it and nothing said so.

What that costs is specific: with no playstate, Recent and Favourites are both empty, so
`pick_opening_tab()` falls through to N64 and `tabs.txt` — whose entire subject is "open on the
first non-empty tab", and whose comment says the Recent branch "was previously untestable" —
**silently went back to testing the fallback it was written to stop testing**. It still passed. It
dumped three frames, hashed them, and reported success, because a screenshot test cannot tell you
that the screenshot is of the wrong thing.

Now generated by the `dfsroot` rule, so it cannot go missing again. The names passed to it must
exist in the tree mkfixture.py generates: the first attempt used `Super Mario World.smc` and
`Donkey Kong Country.smc`, which are not fixture titles, and logged `3 records, 1 matched the
library` — a line that looks like partial success and means the fixture and the test disagree
about what is on the card. `3 records, 3 matched` and `GRID opening on RECENT` is the pass.

Second entry in this audit for a test that kept passing after it stopped testing anything, in the
same session as `favourites.txt` navigating to the wrong tab. Both were screenshot assertions.

---

## 1s. A bug scan before hardware, and what it found

A read of everything that only runs on a writable card, ahead of the SC64 arriving. Seven real
defects, one of them user-visible on every launch under ares today. Each is recorded with how it
was established, because two of the seven were found by reading and five by making something fail.

### The cheat set outlived the game it belonged to

**`app->cheats` was loaded per *sheet*, not per *game*.** `detail_enter()` guarded the load with
`app->cheats.group_count == 0`, and the set is freed in exactly one place — the B handler in
`detail_update()`. Leaving the sheet with Start does not free it, deliberately, because
`screen_launch.c` has to read it. So after any launch that returns rather than boots, the next
game's sheet found `group_count` non-zero and **skipped the load entirely**.

Proven, not argued. [`tools/inputs/cheat-leak.txt`](../tools/inputs/cheat-leak.txt) opens
GoldenEye's sheet, launches it, comes back, and opens 1080 Snowboarding's:

| | game code | Cheats row |
|---|---|---|
| frame 0 | `NGEE` | a three-digit count |
| frame 2 before the fix | `NTEA` | **byte-identical to frame 0** |
| frame 2 after the fix | `NTEA` | `0 of 22 enabled` |

(The dumps are quarter-scale, so GoldenEye's exact count is not legible off them and is not
claimed here. Three digits against 1080's two is enough, and the byte-identity below settles it
without needing to read either.)

The two sheets' cheat rows compared equal byte for byte across two different games while the
code row differed — a coincidence that number cannot be. After the fix, frame 2 differs from its
own previous self on **exactly rows 55–59**, the count line, and GoldenEye's sheet is unchanged
on every row. A five-row diff on the one line that was wrong.

The second half is worse than the display. `cheatstate_capture()` is keyed on group *name* hash,
so backing out of that sheet wrote GoldenEye's cheat names to the card **under 1080's key** — a
selection the user never made, persisted against a game it does not belong to, restored on every
future visit. Under ares this fires on every launch, because a dummy launch always returns to the
grid. On hardware it needs a launch that returns, which today only the fault path does.

Fixed by tracking *which* record the set belongs to (`cheats_for_rom`) instead of *whether* one is
loaded.

### `time(NULL)` on a machine with no clock poisons Recent forever

`rtc_init()` returns false when no RTC source is found and `app.c` ignores it, which is fine.
The consequence was not: `gettimeofday()` then fails, `time(NULL)` returns `(time_t)-1`, and
`(uint32_t)-1` is **0xFFFFFFFF**. That is larger than every real Unix timestamp until 2106, and
Recent sorts descending on it. Play three games before fitting a working clock and those three
outrank everything genuinely recent for the life of the card. Now `1` — still "played", so the
record reaches Recent, and beaten by every real time, so the mistake ages out. The M64 is a clone
console and its joybus RTC is unverified (§4); this is what makes the answer not matter.

### The atlas had never executed anywhere, so it now runs on the host

`thumbstore.c` is the file with the most arithmetic and the least coverage: under ares
`cache_writable()` is false, so `thumbstore_open()` never creates a pak and both `put` and `fetch`
return on their first line. **Every slot offset, both stride branches, the padding that keeps the
file a whole number of slots, and the header rewrite that keeps `slot_count` honest had never run
on any machine.** [`tools/hosttest/test_thumbstore.c`](../tools/hosttest/test_thumbstore.c)
compiles the unmodified file natively against a `surface_t` shim and exercises create → append →
close → **reopen** → fetch, the size-changed miss, padded strides both ways, three-slot offset
arithmetic, a corrupt header, and the read-only path. 31 checks.

Green means nothing on its own, so `run.sh --mutate` drops the `+ 1` from `slot_offset()` — the
class of mistake that still writes, still indexes, and still reports a hit. **10 of the 31 checks
go red**, including all three cold-reopen fetches and both stride cases. The shim pins `stride` as
`uint16_t` to match libdragon's real `surface.h`; declaring it `uint32_t` compiles code the target
does not, since the signedness of `surf->stride == row_bytes` in `slot_io()` turns on it.

### Four smaller ones

- **`str_push()` reported allocation failure as `STR_NONE`**, which is also the legitimate value
  for "this record has no such string". A failed realloc mid-save therefore wrote a valid,
  CRC-correct `library.idx` in which some records had **lost their path** — and `grid_open()`
  correctly refuses to launch a record with no path, so those games would simply stop responding
  to A, with an index passing every integrity check saying they should work. Now fatal to the
  save: no cache is a slow boot, a cache missing paths is a broken library.
- **`&recs[i].path_off` is the address of a packed member.** Caught by
  `-Werror=address-of-packed-member` on the first build of the fix above — on MIPS a 32-bit store
  through an unaligned pointer traps. The fields happen to be aligned (40-byte stride, 8-byte base)
  and that is not something to write an unaligned store against. Routed through locals.
- **`setvbuf()` ran after the pak's header had already been read.** C99 7.19.5.6 defines it only
  before any other operation on the stream. Survivable here because every access seeks first, which
  is not a reason to carry undefined behaviour on the art cache's read path onto new hardware.
- **`cheatstate_capture()` dropped a game's rows before securing room for the replacements**, so a
  failed realloc lost the old selections and never wrote the new ones — an out-of-memory condition
  turned into silent data loss on the next save. Reserve first, forget second.

Also: `cart_load_n64_rom_and_save()` leaked its `path_t` on the fast-reboot-unsupported return,
the only return in the function that did not free it; every fault now names the actual flashcart
error instead of printing "No supported flashcart detected" for `FLASHCART_ERR_SD_CARD`, which
would have sent someone hunting a cart-detection problem that was not there; and a tab clipped at
`MAX_VIEW` now says so, since `library_tab_view()` returning `cap` is otherwise indistinguishable
from a tab holding exactly that many.

### Ruled out by reading, recorded so they are not re-read

- `library_join()` and `cache_init()` both strip the prefix's trailing slash; `"sd:/"` + `"/roms"`
  is `sd:/roms` and the cache directory is `sd:/menu/cache`. No doubled separator.
- `directory_create()` walks and creates each component, and libdragon's FatFs does export
  `mkdir` (`fat.c:369`, wired per volume at `fat.c:493`). The cache directory will exist.
- `dir_findfirst()` populates `d_size` from FatFs (`fat.c:348`), so the per-directory size sums the
  index revalidates against are real on hardware. Under ares they are not populated and the entry
  counts carry the signature alone — which is why revalidation still works there.
- `flashcart_init()` sets `*storage_prefix` on every path including its failures, so `app->storage`
  is never NULL and `cache_init()` cannot be handed one.
- Every `view[cursor]` in the grid is guarded by `view_count > 0`. The empty-tab case was checked
  specifically; Start is the only unguarded handler and it opens settings.
- `directory_create()` returns **true on error**, and `cart_load.c` uses that inverted convention
  correctly.

### Still open

- **`settings_save()` cannot create `sd:/menu/`, and runs before `cache_init()` does.** On a card
  with no `menu/` directory the first-boot settings write fails and the second boot succeeds.
  Left alone rather than reordered: `cache_init()` sits below the flashcart fault return on
  purpose, and a prepared card has `menu/` already because that is where `cheats.db` goes.
- **`app_deinit()` walks the whole ROM tree again to re-fingerprint the index, on the way to
  booting a game.** Correct — art paths are only known by then — but it is a full directory
  enumeration plus an ~84 KB write added to every launch, and it happens *after* the ROM is
  staged in the cart's SDRAM. Both the cost and whether SD writes behave normally in that state
  are hardware measurements. Nothing else in the project exercises that ordering.

---

## 1r. The card can be written to now, and none of it has ever run

Five things are persisted: settings, the library index, play history, cheat selections and decoded
art. Before this, **the menu had never written a byte to storage** — not "written but untestable",
not built. `library.idx`, `playstate.dat` and `cheatstate.dat` each appeared exactly once in the
tree, and all three were comments saying they did not exist.

### The harness trap that cost an hour: `Defocus: Pause`

Recorded first and prominently, because it wasted more time than anything else in this section and
it will recur on any machine whose ares has not been told otherwise.

Every regression script began timing out at once — ten for ten, `the ROM never asked to exit`,
with **zero-byte logs**. The obvious reading is a hang introduced by the change under test, and
that is the reading I took: I spent an hour bisecting my own code, including reverting the entire
working tree to a commit that had passed the same suite an hour earlier. **The commit that had
passed also timed out.** That was the first evidence the cause was not in the repository at all.

Two settings conspired:

1. `Defocus: Pause` in ares' `settings.bml`. ares stops emulating the instant its window loses
   focus. A `regress.sh` run launches ares in the background and never gives it focus, so the ROM
   never advances a frame, never reaches its exit opcode, and dies on the timeout.
2. **ares block-buffers stdout when it is not a tty, and a process killed on timeout never
   flushes.** So the log is not merely incomplete, it is empty — no ares banner, no boot output,
   nothing. Which looks precisely like a ROM that crashed before reaching any instrumentation.

Neither is wrong on its own. Together they turn "your emulator is paused" into "your ROM is
broken", and the empty log removes the evidence that would distinguish them. The tell, missed at
the time, was ares sitting at 8 % CPU while allegedly running flat out.

`regress.sh` and `run.sh` now assert `Defocus: Allow` alongside the existing `HomebrewMode` and
`ExpansionPak` checks, and say what the failure would have looked like. Same reasoning as those
two: a measurement harness that can silently measure nothing is worse than one that refuses.

### The honest status of all of it

**Every write path in this section is unexecuted on real storage.** Under ares the storage prefix
is the ROM's own DFS, which is read-only, so `cache_writable()` is false and every writer returns
before touching a byte. What the regression suite proves is that the menu degrades to exactly its
previous behaviour, which matters and is not the same thing. Read the rest of this section with
that in front of it.

### One shared discipline, in `library/cache.c`

Magic, format version, payload length and CRC32 on every file; a failure of any of them deletes
and rebuilds rather than migrating. One `MENU_CACHE_FORMAT_VER` for all five rather than one each,
because the caches reference each other — `thumbs.idx` holds slot numbers that only mean anything
against the pak — and a scheme that lets them disagree eventually lets a stale one be believed.

Writability is decided **once**, by writing a real byte to a real file and checking it landed. An
`fopen` that succeeds and an `fwrite` that writes nothing is what a full card looks like, and a
probe that only opened would report it writable and then fail on every cache afterwards.

### What the compile-time assertions caught, immediately

Every on-disk struct has its size pinned by `_Static_assert` in the target build, because the one
failure this family of files cannot survive is silent padding — a file that passes its own magic,
version and CRC and is read back with every field after the padding shifted.

The first build failed. `idx_record_t` is **36 bytes, not the 40** the comment above it claimed.
36 is also the wrong number: the record array starts at offset 24, so a 36-byte stride puts every
other record on a 4-byte boundary and makes every `check_code` an unaligned 64-bit load. Padded to
40 so both the start and the stride are multiples of 8. That is an assertion earning its place on
the first run.

### Testing the half that ares cannot reach

`tools/hosttest/run.sh` compiles **the real `cache.c`** — not a copy, not a reimplementation —
natively against two small shims, and makes it write and read actual files. 29 checks: round trip,
and every rejection path, each verified to both fail *and* delete, since a reader that rejects
without deleting looks identical until you notice it never recovers.

`run.sh --mutate` is the house rule made executable. It breaks the CRC seed in a copy and requires
the suite to go red. **Worth recording what survives that mutation: every round-trip check still
passes**, because writer and reader share the broken function. A suite of round trips can never
detect a self-consistent error. That is why CRC32 is also pinned against the published IEEE check
value for `"123456789"` (0xCBF43926), which is the one assertion a self-consistent mistake cannot
satisfy — and it is the only check the mutant fails.

What this does not cover, and cannot: FatFs, the SC64, alignment, and power loss. Those need the
card.

### A bug found by reading libdragon rather than by testing

`thumbstore_fetch()` originally ended with `data_cache_hit_writeback()` on the surface it had just
filled, reasoning that the RDP reads RDRAM directly and would otherwise sample a stale line.
Wrong: `surface_alloc()` uses `malloc_uncached_aligned()` (libdragon `surface.c:47`), so the
buffer is already uncached and there is no line to flush. The call would have produced ares'
"CACHE access to non-cacheable address" warning on every tile and cost a pointless pass over
27 KB. Removed.

The same allocation is quietly load-bearing in the other direction: 64-byte alignment clears the
`(long)addr & 7` test in libcart's `sc_card_rd_dram()`, so a tile read takes the bulk PI DMA path
rather than the per-512-byte bounce-buffer fallback — the cliff recorded in 1q. That is luck, not
design, so it is written down here before someone changes the allocator.

### Decisions worth the ink

**RGBA16 on disk, not CI8.** DESIGN.md picks CI8 on a scroll-bandwidth argument that is real and
worth ~3.6 ms per tile. It also costs a median-cut quantizer, a 32 KB inverse LUT and a per-tile
TLUT, and those bugs would present as "the art is subtly wrong". The card has **29 GB free**, so
16.4 MB for 500 titles at RGBA16 is free, and the win over a 259,633 µs decode is ~29× either way.
CI8 is an optimisation of this, not a prerequisite.

**Slots are 32,768 bytes** — two of the card's 16,384-byte clusters — so a tile never straddles a
cluster and FatFs' clip-at-cluster-boundary never splits a read. 16 % waste, bought deliberately.

**The atlas is checked before the cost gate**, so the 2118 × 1457 card that monopolises the decoder
for 38 seconds becomes an ordinary 27 KB read the moment it has been decoded once.

**Play history is a separate file from the index**, and that split is the design rather than
tidiness. `library.idx` is derived data whose recovery strategy is "delete it"; favourites are the
only thing here that cannot be recovered from the card. Records are keyed on `check_code`, not
path, so a favourite survives the user reorganising their card.

**Cheat selections are keyed on a hash of the group NAME**, not its index, because `cheats.db` is
regenerated from a corpus that gains and loses entries. Index keys would silently re-point every
selection the user ever made — which is 2.2 all over again.

**Nothing writes on the interaction.** Favourites, cheat toggles and settings mark dirty; the
writes happen when the screen closes or the menu exits. A favourite is one button press and a user
may make a dozen in a row.

### What to check first when the card is talking

In this order, because each one is cheap and rules out the next:

1. `[SETTINGS] Failed to save settings to sd:/menu/config.ini` **stops appearing**. That single
   line is the whole write path end to end — prefix, path join, stdio, FatFs, `cart_card_wr_dram`
   — and it is already in the boot sequence.
2. `CACHE sd:/menu/cache: writable`.
3. Second boot logs `LIBINDEX loaded N titles` instead of `LIBRARY scanned`, and reports the
   revalidation cost. Against the measured 11,499 µs/rom this is the number the whole file is for.
4. `THUMBSTORE hit slot N` on the second boot, and the boot plate collapsing to its 1.30 s floor
   because the first row is free.
5. Favourite something, launch it, power cycle: it should still be favourited and in Recent.

## 1q. The ends of the list were being clipped, and the boot plate was decoding nothing

Both reported by eye against the real SD card library, both confirmed against a dumped frame.

### The first and last rows lost their selection to the scissor

A selected tile is drawn larger than its cell — centred, so `(SEL_H - TILE_H) / 2 = 4 px` at each
end — and then adds a 2 px outline above and a 6 px shadow offset below. The grid is scissored to
`[GRID_Y, GRID_Y + GRID_H)`, which it has to be, or a scrolling tile would draw over the tab rail
and the footer. At either end of the list there is no scroll left to make room, so the overhang
went under the clip: **6 px off the top of the top row, 10 px off the bottom of the last one.**

The horizontal case had already been thought about — the scissor is deliberately full-width so a
column 0 or column 3 selection is not cut — and the vertical case was simply missed alongside it.

Fixed by padding the scrollable *content*, not by widening the scissor. Widening it cannot work
downward: the footer is drawn after the grid and would cover the overhang anyway. `ROW_Y()` now
carries `GRID_PAD_TOP` and `scroll_max()` adds both pads, so at scroll 0 the top row sits at
y 78 with its outline landing exactly on `GRID_Y`, and at maximum scroll the last row's shadow
ends exactly on `GRID_Y + GRID_H`. Costs 6 px of the peek row, 22 px to 16 px.

Verified with `tools/inputs/grid-edges.txt`, which sits on row 0 at scroll 0 and then on the last
row at maximum scroll and dumps both. Before: the outline is a bracket, open at the clipped edge.
After: a closed rectangle at both ends.

### The boot plate spent its entire hold decoding nothing

`boot_plate.c`'s own file comment claims the library "has been scanning and decoding underneath
for the whole 1.64 s". It had been doing neither. `grid_update()` returns early while the plate is
stepping — correctly, so buttons pressed during boot do not land on a grid nobody can see — and
that early return sits **above** the `frames_since_move++`. The counter therefore stayed at 0 for
all ~78 frames of the plate, `grid_background()`'s `frames_since_move < DECODE_SETTLE_FRAMES` gate
never opened, and the first decode began **a quarter-second after the curtain had already lifted**.
The plate's entire reason for being an overlay rather than a screen was not happening.

Found by reading, not by measurement. It is invisible unless you notice the tiles arriving
slightly too late, which is exactly what "the startup menu doesn't linger long enough" turned out
to mean.

### And a second priority inversion, arriving by a different route

With decoding switched on during the plate, the tile under the cursor at boot — 1080
Snowboarding, first in the N64 tab — was still the *last* one to get art. Its path was not even
resolved until log line 10,246, long after the rest of the grid had filled in.

`thumbcache_run()` serves `wanted[]` in order, and `wanted[]` is built by `thumbcache_get()` calls
made from `draw_tile()`. The render loop draws unselected tiles first and the selected one last,
so the selection's shadow and growth land on top of its neighbours rather than under them — which
means the one tile the user is actually looking at was **sixteenth of sixteen** in the decoder's
queue. This is [1f.1](#1f) again in different clothing: that one was the decoder walking the
library from index 0, this one is the want list being built in painter's order.

Fixed by requesting the selected record once, explicitly, before the draw loop. `thumbcache_get()`
already dedupes, so it only reorders.

### Measured, on the SD card's own 24-title N64 library

| | measured |
|---|---|
| Decode per card, this corpus | **259,633 µs** |
| Library scan | 27 titles, 308,834 µs (11,438 µs/rom) |
| Cards decoded during the plate — before | **0** |
| Cards decoded during the plate — after | **4**, the whole first row |
| Plate hold — two-row target | 3,028 ms, *released by the ceiling*, 7 of 8 done |
| Plate hold — one-row target, inversion still present | 3,028 ms, *released by the ceiling* |
| Plate hold — one-row target, inversion fixed | **1,998 ms, released by the grid** |
| Total boot | 1.64 s → **2.34 s** |

**259,633 µs supersedes the 155,000 µs in section 1f.** That figure was the mixed fixture corpus;
these are the user's own scans and they cost 1.7× more.

The hold converts **72 %** of its working time into decode — 1.04 s of decode inside the 1.45 s
between the rise ending and the release — the rest going to the plate's own frame and the tail of
the library scan. That ratio is what rules out a two-row target: 2.08 s of decode needs a 2.89 s
hold, which overruns the 3.0 s ceiling and delivers the worst of both, the longest possible plate
*and* an incomplete first screen. Measured, not argued: the two-row version is the 3,028 ms row in
the table.

So the hold is now elastic rather than a constant. Floor 1.30 s, which is the spec's number and
what an artless or already-warm library still gets. Ceiling 3.00 s, which exists because decode
time is a property of the user's art and not of ours. Between them it waits for the first row,
counting a tile with no art as settled — otherwise an unillustrated library would sit at the
ceiling on every single boot. The rise and the curtain are excluded from the decode window, so the
two animated stretches keep the whole field and only the static hold is spent working.

### Found while measuring the above: the zero-allocation gate no longer holds, and never could have

`tools/inputs/idle.txt` exists to prove one claim — a frame that is only drawing allocates
nothing — and it has been reporting `mallocs=0` since [1j](#1j). Against the SD card's library it
reports **28 mallocs and 28 frees per 60 frames, indefinitely, 1,200 frames after the last input**.

**This is not a regression from the work above.** Checked the only way worth checking: the
pre-change source was stashed, rebuilt against the same fixture, and re-run. It reports 28 as
well. What changed is the corpus, not the code.

The cause is that the card has **27 titles and the pool has 20 slots**. `thumbcache_run()`'s third
and fourth passes prefetch in library order over `lib->count`, an evicted record goes back to
`ART_PENDING`, and with more titles than slots there is therefore always a candidate: the cache
decodes and evicts forever and never reaches the `idle` state that flag was added for in
[1l](#1l). Every fixture before this one fit inside the pool, so the gate was measuring a library
small enough that the question never arose — a green result from a test that could not have gone
red, which is the specific failure mode this file was started to catch.

Two consequences, both unaddressed here because this is a different piece of work from the two
above: it is permanent background CPU on a machine that is otherwise idle, and it is unbounded
churn of 27 KB surfaces against a heap with no MMU behind it. It also means the **steady state on
any real library is the thrashing state**, since 500 titles will never fit in 20 slots either.

Not yet measured: whether the churn actually fragments, and what the fix costs. The obvious
candidate is to stop the prefetch passes once the pool is full of art that is wanted or recently
wanted, rather than once the library is exhausted, but that changes the eviction policy and
deserves its own measurement rather than a same-day patch.

### How fast a pre-baked tile could actually load, and why the answer is "don't load one"

Asked directly: with an optimal on-SD format, what is the floor? Everything below is either
arithmetic or read out of the transport code. The one number that is neither is SD bandwidth,
which ares cannot produce because it has no SD at all.

**Measured, first, because it decides the shape of the answer.** With the card's 27-title library
scanned and the 20-slot pool full: heap `total=7,680,000  used=4,137,352` — **3,542,648 bytes
free**. That is a measurement, replacing the plan's estimate of "3.8 MB".

| tile format, 140 × 98 | bytes | titles that fit in 3.54 MB |
|---|---|---|
| RGBA16 | 27,440 | 129 |
| CI8 + TLUT, padded to DESIGN's slot | 15,360 | 230 |
| CI8 + TLUT, padded to one cluster | 16,384 | 216 |
| CI4 + TLUT | 8,192 | 432 |

**So for this card the fastest load is one that happens once.** 24 titles of art is 393 KB at
cluster-aligned CI8, or 658 KB at RGBA16 — 11 % and 19 % of free heap. Read the atlas in a single
sequential pass during the boot plate, hold it resident for the session, and the per-tile cost is
not reduced, it is *deleted*: no seek, no read, no eviction, no streaming budget, and the
thumbcache thrash recorded two sections above stops existing because nothing is ever evicted.
The crossover where streaming becomes necessary again is ~129 titles at RGBA16 and ~216 at CI8.

**A consequence that inverts DESIGN.md §5.2 for libraries this size.** CI8 was chosen over RGBA16
purely on scroll-streaming bandwidth. A resident atlas has no scroll streaming, so below ~129
titles RGBA16 is the better format on every remaining axis: no quantizer, no median cut, no
inverse LUT, no TLUT load per tile, and it blits in copy mode. CI8 earns its quantizer above that
line, not below it.

> **Scope correction.** The paragraph above is right and it is also nearly irrelevant, because the
> target is a 500+ title library. See "At 500 titles" below: the resident atlas dies, streaming
> comes back, and **DESIGN.md's original CI8 choice stands**. The RGBA16 argument applies only to
> a library under ~129 titles, which is a card like the current test one and not the design point.

### At 500 titles, the art stops being the long pole

| | 500 titles |
|---|---|
| Atlas on card, CI8 @ 16,384 | 8.2 MB |
| Atlas on card, RGBA16 | 13.7 MB |
| Fraction resident in 3.54 MB, CI8 | **216 / 500 — 43 %** |
| Fraction resident, RGBA16 | 129 / 500 — 26 % |
| Fraction resident, CI4 | 432 / 500 — 86 %, and rejected on quality |
| First-run atlas generation @ 259,633 µs | **130 s** |
| **Library scan @ 11,499 µs/rom** | **5.75 s** |

Two things fall out, and neither is about texture loading.

**The scan is the bigger cost.** 5.75 s of scanning against, at worst, tens of milliseconds per
scroll row of art. And 11,499 µs/rom was measured through the DFS, which is faster than FatFs
over the cart, so the real figure is worse. `library.idx` with per-directory signatures — the
plan's sub-200 ms revalidation — stops being an optimisation and becomes the thing that makes a
500-title library bootable at all. It is also strictly easier than the art cache: 500 records is
84 KB, one `fread`, no format questions.

**The pool is six times too small, and the memory to fix it is already sitting there.**
`THUMB_SLOTS` is 20, which at the current RGBA16 slot is 549 KB against 3.54 MB free. At
cluster-aligned CI8 the same free heap holds **128 slots for 2.1 MB, leaving 1.4 MB spare** — 32
rows of coverage out of a 125-row library. Scrolling anywhere inside a 32-row neighbourhood would
never touch the card. This also subsumes the thrash finding above: at 20 slots and 500 titles the
prefetch passes walk 500 records forever and the cache never once reaches its idle state.

**And the per-row read, which is what streaming actually costs.** One scroll row is four tiles:
65,536 B at cluster-aligned CI8, or 21.8 ms at 3 MB/s and 44 ms at 1.5 — against a 16.7 ms field.
That only holds if the four are **contiguous in the atlas**, which means ordering the atlas in
display order so a row is one seek and one read rather than four seeks, four FAT walks and four
reads. Ordering it for the default tab makes the common case contiguous and charges the other
tabs a seek per tile.

**The unverified option that would beat all of this.** The SC64 has SDRAM behind the PI bus, and
`sc_card_rd_cart()` reads SD straight into it in a *single* command — no 8 KB chunking, no PI DMA,
no FatFs in the loop. Stage the whole 8.2 MB atlas there once per power-on (~2.7 s at 3 MB/s, in
the background, not blocking the plate), and every subsequent tile fetch is one `dma_read` from
cart to RDRAM: no SD command, no seek, no cluster arithmetic. That turns a 500-title library into
the same problem as a 24-title one. Entirely unverified — it needs hardware, and it needs the
card to be writable first.

**If a tile must be loaded individually, three things in the transport path set the floor.** All
three are in `libdragon/src/libcart/cart.c` and `src/fatfs/`, and all three are checkable:

1. `sc_card_rd_dram()` transfers in **16-sector (8 KB) chunks**, each one a blocking SD command
   polled to completion and *then* a PI DMA — serialised, not overlapped. A 16 KB CI8 slot is two
   round trips; a 27,440 B RGBA16 tile is four.
2. **The destination must be 8-byte aligned.** If `(long)addr & 7`, the same function drops to a
   per-512-byte path: sixteen separate PI DMAs plus a CPU copy through a bounce buffer, per chunk.
   This is the largest cliff in the path and it is one `memalign` away.
3. **The card's cluster is 16,384 bytes** (`diskutil`: Allocation Block Size 16384), and FatFs
   clips every `disk_read` at a cluster boundary (`ff.c:3978`). DESIGN.md's 15,360 B slot
   therefore straddles clusters and costs two `disk_read` calls with a FAT walk between them.
   **Sizing the slot to exactly 16,384 makes every slot exactly one cluster** — one seek, one
   contiguous run. It also costs 16 % more card space than the 15,360 figure, which is nothing.

Also worth knowing: `FF_USE_FASTSEEK` is **0** in libdragon's `ffconf.h`, so `f_lseek` walks the
cluster chain rather than using a link map — another argument for sequential-once over
seek-per-tile.

**The arithmetic, parameterised on the unknown.** At B MB/s the per-tile read is 16.4/B ms for
cluster-aligned CI8. At the plan's assumed 3 MB/s that is **5.5 ms**, against a measured
**259.6 ms** to decode the same tile from JPEG — **47× faster**, and inside a single 16.7 ms
field. At a pessimistic 1.5 MB/s it is 11 ms and 24×. The whole 393 KB atlas is 131 ms at 3 MB/s,
which disappears entirely inside the boot plate's 1.30 s floor.

**Not yet measured, and not measurable here:** the bandwidth itself, and whether
`sc_card_rd_cart()` — which reads SD straight into cart SDRAM in *one* command with no chunking
and no PI DMA at all, and could then be pulled to RDRAM in a single large DMA — beats 48 chunked
round trips for a bulk atlas load. That is a hardware experiment, and it is the first one to run
once the card can be written.

### Deferred: caching the decode, which is what would actually fix this

The ask is to persist the processing so a boot does not repeat it. It is the right answer and it
is [already argued for in 1f](#1f) — the case is not streaming bandwidth, it is never paying
259,633 µs twice. At 24 titles that is **6.2 s of decode burned on every single boot**, and the
2.34 s plate above is a way of hiding a fraction of it, not a way of removing it.

Still blocked, and blocked on the same thing as everything else in this row: under ares the
storage prefix is `rom:/` and the DFS is **read-only**, so no cache file can be written on the
machine this is developed against. A write path that has never executed is not a feature. This
lands with hardware, and it needs the USB side of the cart working first — see
[HARDWARE.md](HARDWARE.md).

When it does land, the shape is already fixed by `src/cheats/cheatdb.c`, which is the reference
implementation for every cache file here: magic, format version, and a mismatch that deletes and
rebuilds rather than attempting to migrate. `DESIGN.md` specifies the atlas — one `thumbs.pak`
held open for the session, fixed-size sector-aligned slots, `fseek` + `fread` rather than a
directory traversal per tile — and CI8 becomes worth its quantizer at that point, because that is
when a tile is read back over FatFs rather than decoded.

The measurable consequence to expect, stated now so it can be checked rather than assumed: the
boot plate should collapse back to its 1.30 s floor and release *by the grid*, because a warm
cache makes the first row free.

## 1p. JPEG, and a decoder that was quietly reading the wrong pixel

The card carries 22 titles whose art is `<ROM name>.jpeg`, so two things were missing: the
scanner only indexed `.png`, and nothing could decode a JPEG at all.

**picojpeg, not libjpeg, and the corpus is why.** The case for libjpeg was its reduced-size IDCT,
which matters when the source is many times the 140 px tile. Measured, the real art is
**256 x 187, baseline, 4:2:0, 12-20 KB, all sixteen sampled files** -- an eighth of that is 32 px,
far under the tile, so scaled decode would never engage. 0.8 Mpixel for the whole library against
the PNG fixture's 21.3. The argument for the larger dependency evaporated on contact with the
data. picojpeg is 2,454 lines and public domain, by the author of the miniz already vendored.

**Where it plugs in.** `scaler_add_row` was reading `d->row_buffer` directly; it now takes a row
pointer, and that one change is the whole integration. picojpeg emits MCUs rather than rows, so a
band of one MCU row is assembled and fed to the scaler a row at a time -- the budget's unit of
work stays the same size for both formats, and the crop, the box filter and the RGBA5551 pack are
shared rather than duplicated. Format comes from the magic bytes, not the extension: the card has
fifteen `.jpeg` and one `.jpg`, and a PNG misnamed `.jpg` still draws.

`png_decoder` became `image_decoder`. A file called png_decoder.c that decodes JPEG is the kind of
comment-shaped lie this file exists to catch.

### picojpeg had undefined behaviour in its chroma path

Four lines accumulate chroma as `*pDstG++ = subAndClamp(pDstG[0], cbG)`. There is no sequence
point between reading `pDstG[0]` and incrementing `pDstG`, so **whether the correction landed on
this pixel or the next one was the compiler's choice** -- a silent one-pixel chroma shift on every
block, appearing or not with optimisation settings. GCC flagged it and `-Werror` turned it into a
build failure, which is the only reason it was seen at all.

Split into two statements. `-Wsequence-point` is deliberately **not** silenced for this object, so
re-pulling the file verbatim fails the build rather than quietly decoding wrong colours. Only the
cosmetic warnings are suppressed: progressive-scan header fields it parses but cannot use, and an
unused helper.

### Measured

Steady-state window, one 256 x 187 card decoding: 60 rows in 15,244 us, 254 us/row at 256 px, so
**about 1.0 us/pixel against PNG's 2.4** on ordinary cards, 91 % of it in the entropy and IDCT
stage. Roughly two and a half times faster per pixel, on files an order of magnitude smaller.
That does not retire the on-disk cache argument in 1h -- it moves it.

Correctness was checked by looking, not by absence of errors: tile 0 cropped out of the frame
dump and enlarged shows the 1080 logo, the red board, the rider and the Nintendo 64 banner,
matching the source. Right colours also rule out the chroma shift above.

**The reduce path was written before it could be tested, and that was caught.** No file in the
corpus can trigger it, so the branch would have shipped unexercised. A 2200 x 1600 card was
synthesised for the fixture; both branches are now observed live:

```
JPEG 2200x1600 DC-only/8 -> 140x98 (band 2)
JPEG  256x187  full      -> 140x98 (band 16)
```

**Still untested:** progressive JPEG, which picojpeg cannot decode at all -- it will report a bad
file and the tile falls back to its placeholder. None of the corpus is progressive. Grayscale is
handled in code but no sample exercises it.

---

## 1o. Art can live anywhere, and emulated systems could never have had any

Box art was found exactly one way: `menu/metadata/<G>/<A>/<M>/<E>/boxart_front.png`, built by
splitting the N64 game code into four single-character directories. That is upstream's layout and
it is kept, but it had two consequences worth naming.

**Emulated-system titles could not have art at all, by construction.** `index_n64` runs only for
`SYS_N64` (`library.c:220`), so `game_code` stays empty for every NES, SNES, GB, GBC and SMS ROM,
and `art_path` returned false at its first line. No file placed anywhere on the card could have
changed that. The SD card prepared for the M64 carries three SNES ROMs and no N64 ROMs, so it was
a card on which nothing could ever display art.

**Upstream's own documented fallbacks were not implemented.** `docs/19_gamepak_boxart.md`
describes a 3-character region-agnostic path and a homebrew-by-title path; neither existed here.

Art is now found five ways, ordered so the free lookups come first:

| # | rule | cost |
|---|---|---|
| 1 | a loose PNG named for the game code, anywhere under the scanned root | memory |
| 2 | a loose PNG named for the ROM itself | memory |
| 3 | `menu/metadata/N/G/E/E/boxart_front.png` — upstream's layout | one stat |
| 4 | `menu/metadata/N/G/E/boxart_front.png` — upstream's region fallback | one stat |
| 5 | `menu/metadata/NGEE.png` — flat | one stat |

Rules 1 and 2 cost nothing because the scan already visits every file: noticing PNGs is one
extension compare per directory entry in a loop that was walking the tree anyway. A search per
title would have been hundreds of stats on a cold FatFs, which is what made "anywhere" affordable
at all. Rule 2 is the only one that does anything for emulated systems.

A loose file outranks the metadata tree deliberately. The tree is a bulk pack somebody
downloaded; a PNG next to a ROM is a decision.

**Two things guard the cost, both of them fixing mistakes this file already records.** The
resolved path is cached in `rec->art_file`, so the five-way walk happens once per record rather
than once per pass — 1g records what re-probing per pass cost last time (180 filesystem probes
and 6,437 us a frame). And `menu/metadata` is stat'd once and remembered, because without it a
card with no art pack pays three probes per title to learn three times over that a directory it
does not have is still absent: 1,500 stats on a 500-title library, all answerable by one.

Verified under ares on a fixture built with one case per rule, plus a control whose art was
deleted outright:

```
ART: metadata dir present
ART N3HE: loose rom:/roms/n64/N3HE.png                                  rule 1
ART ----: loose rom:/roms/snes/Star Relic.png                           rule 2, no game code
ART NAGE: metadata rule 0, rom:/menu/metadata/N/A/G/E/boxart_front.png  rule 3
ART N3HJ: metadata rule 1, rom:/menu/metadata/N/3/H/boxart_front.png    rule 4
ART NABE: metadata rule 2, rom:/menu/metadata/NABE.png                  rule 5
ART NADE: none                                                          control
```

The control is the point of the table: without a case that must fail, five passing lines only
prove the resolver returns something. The guard was tested the same way — with `menu/metadata`
moved aside the run reports `metadata dir absent`, rules 1 and 2 still resolve, and rules 3–5
issue no probes at all.

**Still not implemented:** upstream's homebrew-by-title path
(`menu/metadata/homebrew/{title}/boxart_front.png`) for `xEDx` headers. `LIBF_HOMEBREW` is set at
`library.c:175` but nothing consults it when resolving art, so a homebrew ROM is still looked up
by its ID.

---

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

**Favourites** toggle with Z ~~and~~ (superseded: C-right since 1t) and the tab filters live --
un-favouriting from inside the Favorites tab rebuilds the view under the cursor. In memory only,
lost on reboot until `playstate.dat` exists. The interaction is here because the tab is otherwise
permanently empty and unreviewable.

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

> **`getart.py` has since been removed** — this repository no longer ships a fetcher for someone
> else's scans. `build/artcache/` is still what `--art-from` reads and still has to exist for any
> measurement on this page to be reproducible; populate it by hand with a tree of
> `<GAMECODE>/boxart_front.png`. Everything measured below stands, but re-deriving it now takes
> a step that used to be one command.

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

### Music is affordable in bytes and expensive in frames

Background music is 28 Standard MIDI Files synthesised at runtime by `src/libs/midi64`
(vendored at `1c79dc8`, MIT). The size case is settled and lopsided:

| | bytes |
|---|---|
| midi64 `.text`, six objects, before LTO | 17,938 |
| all 28 songs, as `.mid` | 295,760 |
| **engine plus the whole soundtrack** | **313,698** |
| two of the same songs rendered to Opus wav64 | 591,854 |
| `bgm.wav64`, shipped since the fork, opened by no line of code | 536,219 |

The ROM went **1,835,008 → 1,622,016 bytes**, 212,992 smaller, while going from no music at all
to twenty-eight tracks. `settings.bgm_enabled` was saved and loaded and read by nothing; it is
gone, along with `src/libs/minimp3`, which nothing had compiled since the fork.

There is a second, larger reason to prefer synthesis that has nothing to do with size: **wav64
streams off storage for the whole duration of playback.** That would have put a permanent second
reader on the FatFs-over-SC64 pipe the thumbnail streamer already competes for during a scroll —
the one budget §5 says ares cannot validate. midi64 reads the file once into RAM and then never
touches storage: verified by grep, there is no `fread`, `fseek`, `fopen` or `dfs_` anywhere in
`seq.c`, `synth.c` or `mixer_glue.c`.

**The frame cost is the problem.** `tools/inputs/idle.txt`, `PLAIN_ART=1`, settled, the last
60-frame window of a 1,200-frame run — the same binary and the same script throughout, with
`music_volume=0` in the fixture's `config.ini` as the only difference for the first row:

| | frames making their field, of 60 | worst frame | `snd_us` |
|---|---|---|---|
| music off | **60** | 16,759 µs | — |
| 22050 Hz | 38 | 38,671 µs | 1,042 |
| **16000 Hz (shipped)** | 45 | 31,559 µs | 1,046 |
| 11025 Hz | 50 | 25,453 µs | 485 |
| 22050, `M64_CTRL_BLOCK=64` | 40 | 36,128 µs | 1,259 |
| 22050, `MIDI64_MAX_VOICES=16` | 40 | 32,836 µs | 977 |
| 22050, 8 audio buffers + mid-frame pumping | 39 | 36,094 µs | 1,442 |
| 22050, `NUM_CHANNELS=4` (was 16) | 39 | 35,516 µs | 1,428 |
| 22050, `NUM_CHANNELS=16` | 38 | 35,725 µs | 1,227 |

Music off is a clean sweep — 60 of 60, nothing missed, worst frame inside a field. Switching it
on costs a third of all frames their field on a screen that is doing nothing else. **The gate in
§1 goes from green to red on this change alone.**

**Six levers, one of them works.** Sample rate, midi64's control block, its voice cap, audio
buffer depth, mid-frame mixer pumping and the mixer channel count — five land within noise of
each other and only the rate moves anything. Default dropped to 16000, which recovers about a
quarter of the misses; midi64's `docs/PERF.md` puts that above what its patch set's low-pass
corners need, so it is the last step down that costs nothing audible. No rate reaches zero:
11025 Hz still misses one frame in six.

The buffer and pumping changes were made against reported *choppiness* rather than against this
table, and they are kept on their own merits even though they move no bin: `snd_us` says
synthesis costs ~1 ms of a 16.7 ms frame, so the CPU has headroom and audible dropouts are
starvation, not shortfall. Before them, nothing fed the mixer between `render()` and the end of
`background()` — the longest stretch in the frame.

**Negative result worth keeping: the timer added for this does not explain it.** `snd_us` brackets
`sound_poll()`, where `mixer_try_play()` synthesises, and reports about 1 ms per frame — 6% of
wall clock. But average frame time goes 16.7 → 23.3 ms, and `upd_us`, `rnd_us` and `bg_us` barely
move. Roughly 4.5 ms per frame lands outside every counter in the loop. The likeliest homes for it
are the AI interrupt handler and RSP time shared between the mixer ucode and rdpq, neither of
which any counter here can see. **Do not quote `snd_us` as the cost of music; it is a floor.**

Two caveats before anyone plans against the table. It is an ares number, and midi64's own
`docs/PERF.md` argues this workload is dominated on hardware by misses against a 92 KB table that
does not fit the VR4300's 8 KB data cache — an effect ares does not model, so hardware could land
either side. And the relative comparison is the trustworthy part: same emulator, same script,
one variable.

Frame hashes are **identical** with music on and off, at every sample rate tested, so none of this
moves a pixel and the existing suite is unaffected.

### A track selector must not start a track on every step

First playable build: switching tracks "didn't always change music, sometimes seemed like it was
playing two songs at once".

Not a midi64 fault. A held direction repeats at up to **20 steps a second** (`ui/input.h`), and
the track row called `music_set_track()` on every one of them — each a full teardown, file read,
parse and player rebuild. Meanwhile the audio DMA holds four buffers of the previous song and
there is no way to recall them. Twenty swaps a second against that pipeline plays fragments of
several songs over each other and lands on audio unrelated to the row you stopped on.

Fixed by separating choosing from playing: the row moves immediately, the track starts 0.30 s
after the row stops (`TRACK_SETTLE_S`), and `leave()` flushes a pending choice so backing out
inside the window still plays what was picked. Scrolling the list now costs one load at the end
instead of one per step.

### The shuffle seed cannot be tested here, and the obvious seed did not work

Track selection is random, seeded per boot. The first attempt mixed `time(NULL)` with
`TICKS_READ()`, reasoning that the boot path's cycle count varies even when the clock does not —
which matters because a cart with no RTC returns the same epoch on every switch-on, and that is
the case on this hardware and under ares.

**Measured: four scripted boots picked the same song every time.** A scripted run pins the clock
to `SCRIPT_CLOCK_EPOCH` and ares' cycle counts are reproducible, so both inputs were constant.
The fallback did nothing. Replaced with `getentropy32()`, which libdragon collects during IPL3
and documents as differing on every boot on hardware; the clock is still mixed in, so a console
with a working RTC contributes it.

Two limits worth stating rather than discovering later:

1. **`entropy.h` says the numbers are consistent on an emulator.** Two ares boots after the change
   both picked *Unknown Island* — a different song from the pre-change runs, which shows the new
   path is live, but shuffle cannot be observed working short of a console.
2. **The harness reaches this at all only through `SCRIPT_MUSIC_TRACK`.** A scripted run pins one
   looping track, because shuffle advances on mixer progress rather than frame count and each
   advance allocates, which would make `idle.txt`'s no-allocation gate a coin toss.
   `make TUNE='-DSCRIPT_MUSIC_TRACK=-1'` restores it for a run whose frames are not compared.
   Without that knob the question has no way of being asked.

---

## 1ac. Profiles — on a branch, not on main

Ten players, each with their own favourites, play history, cheat selections, theme and saves.
Built on the `profiles` branch because the feature has not been decided on.

### The split was already most of the way there

`playstate.dat` has been a separate file from `library.idx` since M3, for a reason recorded in
playstate.h: the index is derived from the card and safe to delete, play history is not. That is
the same line a profile has to be cut along, so the model layer came to **+2,240 bytes of text**
(556,824 -> 559,064) before any UI. The whole feature, screen included, is **+10,624 bytes**
(-> 567,448). Nothing else moved much: `data` +704 (-> 106,892), `bss` +920 (-> 71,264).

What is per profile is `playstate.dat` and `cheatstate.dat`, plus the theme and the saves folder.
What stays shared is everything derived from the card — `library.idx`, `thumbs.pak`/`.idx`,
`cheats.db` — because ten copies of a 292 KB atlas would be the largest thing on the card and
every byte of it identical.

### Profile 1 writes where every existing card already writes

Profile 1 is index 0 and takes the unsuffixed paths: `cache/playstate.dat`, `<romdir>/saves/`.
Profiles 2..10 nest — `cache/p2/…`, `<romdir>/saves/p2/`.

The proposal on the table was sibling folders, `p0saves/` and `p1saves/`. Rejected for two
measured reasons rather than taste. There is no unsuffixed case in that scheme, so **every save on
every existing card stops being found on the first boot after the upgrade** — silently, because
the menu still runs and the folder it now looks in is simply empty. And `library.c`'s SCAN_SKIP
excludes `saves` by exact name, so a sibling `p2saves/` would be walked as though it held games,
while nesting costs the scanner nothing at all.

### The padlock had to come out of playstate first

`LIBF_LOCKED` rode in the same flags word as `LIBF_FAVORITE` inside `playstate.dat`. Making that
file per profile would have made the parental padlock per profile too — **two presses on the boot
screen and every locked game on the card is open**, with no code entered and nothing to notice.

Split into `locks.dat`, shared, keyed the same way. Cards written before it exists keep their
locks in `playstate.dat`; that read path is untouched and `locks_load()` notices the case (no
`locks.dat`, but the library already has locked records) and marks itself dirty, so the first save
moves them across. A version bump would have been the other option and would have cost every user
their favourites, since playstate is the one cache that cannot be rebuilt from the card.

### The tab rail could not carry the player name

The name started in the tab rail, right-aligned, on the reasoning that a shared console's real
failure is playing an hour as the wrong person. The rail is 608 px and the comment above
`draw_tab_rail()` measured its worst case at 468, so 140 px looked like enough for a Z glyph and
a name.

It was not, because the worst case moves. Recent and Favourites were icon-only *until selected*
and then spelled themselves out, so selecting Favourites turned one 20 px glyph into nine
letters and shoved every tab right into the name. An icon that changes size when you look at it
is a layout that moves under the cursor.

Both fixed at once: the two virtual tabs are now always icons, and the name moved to the footer
as a fourth hint on a `UI_BTN_TALL` Z -- the same shape the detail sheet's Cheats hint uses,
because Z is a trigger and drawing it as a coloured disc says "face button" about something that
is not one. `PROFILE_NAME_CAP` came down from 13 to 9 to keep that footer row inside the safe
area at four hints, and the two status counts under the settings list -- a library total and a
cheat database total -- came off, because the grid answers the first in a more useful form and
nobody asks the second.

The Favourites icon became a five-pointed star with rounded points and intersections, replacing
the corner triangle. It is a baked table of 26 horizontal runs, not geometry: rounding a star is
a morphological closing followed by an opening, which is a supersampled distance operation and
not something to run per frame for a glyph this size.

Two things had to be got right and only one of them was obvious. **Fit the bounding box, not the
radius** -- a star is 1.902 outer-radii wide and 1.809 tall, so centring it on the radius leaves
two dead rows under the legs, which at 20 px is most of a leg. And **20 px is not enough**: the
first version was drawn in TAB_ICON's existing box, and once the rounding was applied the legs
merged into the waist and the shape read as a pentagon with a spike on top. `TAB_ICON` went to 24,
which the rail can afford now that the two virtual tabs never spell themselves out -- worst case
about 444 px of 608, against 468 before. The final parameters are inner radius 0.36 and a 0.4 px
rounding radius, chosen by rendering four candidates as ASCII and looking at them.

### Hardware, at last, and four faults ares structurally could not show

The first run on a real SC64 and a real card. All four reported problems were diagnosed from the
card's contents plus the source; three had a single root cause each, and the fourth turned out to
be a different bug from the one that looked obvious.

**Art vanished after a restart, and it was not the thumbnail cache.** `lib->art[]` -- the table
mapping a game to its loose image file -- is filled by exactly one call site, `art_push()` inside
`scan_dir`. `libindex_load()` never touches it. So every boot that hits a warm index skips the
scan, leaves that table empty, and `library_find_art()` returns NULL for every title. The only
fallback is `rec->art_file` in the index, which was resolved lazily per tile and written back only
by `app_deinit` -- which runs when a game is launched and not when the console is switched off.

The card proved it: `library.idx` held 56 strings, all ROM paths and titles, and **zero** image
paths. Everything else followed. Nothing resolved art, so nothing decoded, so nothing was stored:
`thumbs.pak` was 32,768 bytes -- one reserved slot -- with `slot_count = 0` and every byte after
its 32-byte header zero. And `thumbstore_flush()` with no rows *deletes* the index, which is why
there was no `thumbs.idx` at all.

Fixed by resolving loose art at scan time, before the first `libindex_save`. It is pure memory --
the scan already saw every file -- so it costs a hash lookup per record. `LIBINDEX_MAGIC` was
bumped 'M64L' -> 'M64M' so existing indexes rebuild once. Deliberately not `MENU_CACHE_FORMAT_VER`:
that is one number for every cache by design, and raising it would take `playstate.dat` with it,
which is the one file here that cannot be rebuilt.

**Cheats did nothing in-game, and the obvious culprit was not it.** `cheats_patch_ipl3()` returns
`true` on error -- its success path returns `false` and the caller bails on truthy -- but the check
that verifies the IPL3 layout does `return false` when it *fails*. So an unrecognised IPL3 was
reported as patched, the hook was never written, `cheats_install()` returned true anyway, and
`boot.c` set `skip_rdram_reset` on the strength of it. Engine assembled, never hooked, cheats
silently inert. That is upstream's code: `git diff 6407ab15 -- src/boot/cheats.c` is +34 lines,
additive, all bound checks from 2.3.

It is real and it is fixed, but **it is not what bit this card**, and the way that was established
is worth recording. `tools/hosttest/test_cheatinstall.c` compiles the real `cic.c` and runs it over
a ROM's first 4 KB. Its first run reported that both tested games would fail -- and printed its own
control as `jr $t1 = 0x20000240`, which is not a MIPS instruction. `vr4300_asm.h` builds
instructions through a **bitfield union**, and C does not specify bitfield packing: correct for
mips64-elf, garbage on x86. Hand-encoding the constant (`9 << 21 | 8` = `0x01200008`) reversed the
answer completely. Across 23 retail ROMs, 22 hook correctly; only Star Fox 64 (CIC 6101,
`word[466] = 1509fffe`) takes the broken path. **Both games actually tested hook fine, so the cause
of the reported failure is still unknown.** Recorded as open rather than closed.

**Music stopped for about a second when a detail sheet opened.** `detail_enter()` called
`cheatdb_load()` synchronously -- an `fseek` and `fread` of that game's blob. Nothing under ares,
where the DFS is inside the ROM; about a second on FatFs over a real cart, and nothing feeds the
mixer during a screen transition. Moved into `background()`, which runs every frame, with a forced
load on the Z path so the cheats screen can never open against a set that has not arrived.

**The launch progress bar appeared for some games and not others.** It was gated to appear only
after a load outlived 1.5 s. On this hardware the big cartridges sit either side of that, so it
looked like a glitch rather than a considered escalation. Removed; the launch is now always a fade
to black.

### The diagnostic channel is a file on the card, not USB

This cart's USB port does not enumerate, so `debugf` reaches nothing, and a replacement is weeks
away. That matters less than it sounds: the cheat engine installs inside `boot()`, after the
display is closed and the filesystem unmounted, so USB could never have observed the interesting
part anyway.

Two things replace it. Questions that depend only on a ROM's bytes are answered on the development
machine against the real production code -- `test_cheatinstall.c` is the pattern, and it compiles
`cic.c` rather than reimplementing a checksum that must never disagree with the console. Questions
that are genuinely runtime go to `/mainmenu/launch.log`, written immediately before the point of
no return: which ROM, which CIC, whether the engine can hook it, how many cheat words were
emitted. The write path is known to work now -- the card came back with config, index, playstate
and cheatstate all correctly written and CRC-valid.

The same pre-flight feeds the detail sheet, so a game the engine cannot hook says "Not supported
for this game" where the cheat count would be, rather than letting somebody tick twenty cheats
that were never going to run.

### Two card facts worth keeping

The RTC has never been set: every timestamp on the card is 5 December 2024, so `last_played` is
fiction and the Recent tab sorts on it. And `cheats.db` matched both games through the `?` region
wildcard -- 87 groups for Episode I Racer, 37 for Spider-Man -- with all codes type `80`/`81`,
masking to valid addresses. The database, the wildcard, the group model and the emitter are all
working; **324 of its 325 entries carry `check_code = 0`**, so every lookup reaches the game-code
fallback rather than the primary key. That is the converter's doing and is not itself a fault, but
it means the check-code path has never been exercised by real data.

### The host test caught two bugs ares could not reach

Neither is visible in a frame, so neither was reachable from the regression suite at all.

1. **`profile_save()` never created `/mainmenu/`.** It worked only because `cache_init()` happens
   to create `/mainmenu/cache`, and therefore its parent, before any screen can reach the roster
   editor. An ordering dependency, not a guarantee. Caught by calling `profile_save()` without
   `cache_init()`: the roster silently failed to persist and every name came back as "Player N".
2. **`locks_load()` left `dirty` set from a previous load** when it took the no-file path. Harmless
   in the product, where it loads once at boot, and wrong.

`tools/hosttest/run.sh --mutate` gains a fourth mutation: give profile 1 a `p1/` prefix. That is
the one mistake here that is silent, unrecoverable and shaped exactly like working software — the
menu still boots, still saves, still loads, into a folder nothing has ever written. 47 checks, and
the mutation takes exactly one of them red.

### ~~The host test suite had not linked since the /menu rename~~ — fixed

Found while adding to it, and worth recording because of how it failed. `cache_init()` started
calling `menu_path()` when `/menu` became `/mainmenu`, and `run.sh` was never told to link
`paths.c` — so **every suite in it failed at the link step**, and had been doing so silently for
however long. `tools/hosttest/run.sh` was the answer to "the write half is unexecuted on real
storage", and it had been answering nothing.

Underneath that, `test_cache.c` hardcoded `"%s/menu/cache/%s"`. Once it linked again, six checks
went red: `poke()` and `chop()` were opening a path that no longer exists, so the three corruption
cases never corrupted anything. Their matching assertions still **passed** — `!exists("b.dat")` is
true of a file that was never written. Six green checks for a rejection path that was never taken,
which is the exact failure mode CLAUDE.md's "check that a test can fail" rule exists to catch, and
which no amount of reading the output would have revealed.

### What this run does not prove

- **The roster does not survive a power cycle under ares**, because nothing does: the storage
  prefix is the ROM's read-only DFS. `tools/inputs/profiles.txt` exercises every screen and every
  transition, and `profiles.ini` is never written. Same gap as every other written file here.
- **Switching profile changes which `playstate.dat` is read and which `saves/` a launch writes
  into, and neither is observable from a frame.** The fixture has no saved games and no
  favourites to lose. This is what the host test covers instead, and it is not the same thing.
- **The full regression suite has still not been run against the music, settings, clock or
  profile work.** Only `profiles`, `clock`, `clock-locked`, `parental` and `idle`. The three
  Settings-row counts moved because Settings gained a row, and were updated; the other nine
  scripts are unverified.

The settled-frame no-allocation gate does still hold: `mallocs=0 reallocs=0 frees=0` at n=1,200
on `idle.txt` with the roster loaded.

---

## 1ad. The second hardware run — five reports, and what the card's own bytes said

Five things came back from the console. Four had causes findable by reading; the fifth is still
open and now has an instrument pointed at it.

### The grid forgot where you were

`grid_enter()` set `cursor = 0` unconditionally, and it runs on every entry, not only the first.
So backing out of a game sheet dropped you at the top of the tab. On the test card's N64 tab that
is thirty-seven titles; open the fifteenth, read it, press B, and the fifteenth is nine presses
away again. Fixed by not resetting — the static already survives, and `rebuild_view()` was
already clamping it for the two cases where the view can shrink underneath a held position
(un-favouriting from the sheet while Favorites is up, switching to a player with a shorter
Recent).

Pinned by `tools/inputs/detail-return.txt`, which is read rather than diffed: frames 1 and 3
legitimately differ because the sheet restarts the arrival tween, so the assertion is that both
show **15 / 37, CHOPPER ATTACK, at the same scroll offset**. Verified at 640x480.

### Art refused to appear until scrolling stopped, on a card whose art was already decoded

`grid_background()` returned early while `frames_since_move < 15`. That gate is correct and its
reasoning (1q) is unchanged: one row of a real PNG can cost more than the frame it is trying to
stay out of the way of, so decoding while the cursor moves buys judder and nothing else.

It was applied to the **atlas** as well, and it never should have been. A tile already in
`thumbs.pak` is one seek and a 27,440-byte read — tens of times cheaper than a decode and nowhere
near a field. So a warm card behaved exactly like a cold one, which is the whole benefit of the
atlas thrown away by a branch written about something else.

`thumbcache_run_cached()` now runs during a scroll, on its own 4,000 µs budget, looping rather
than doing one tile per call — a row is four tiles and one fetch per frame leaves the grid
permanently a row behind the eye. It never sets `tc->idle`, because declining to decode is not
the same as establishing there is nothing to do.

The working set was also too small, and the old comment miscounted it: the grid window is 352 px
on a 110 px row pitch, so a straddling scroll shows **four** rows and sixteen tiles, not twelve.
Twenty slots left four spare. Now 36 — sixteen visible, `THUMB_PREFETCH_ROWS`(2) either side, and
four spare — and the grid asks for the off-screen rows itself in `grid_render()`. It has to be
the grid: `thumbcache_run`'s own prefetch passes walk the library from index 0, so on a large card
they fill the pool with the front of the alphabet while the tiles either side of the cursor get
nothing. Proximity is a fact only the screen knows.

Cost is lazy — a slot allocates its surface when something lands in it — so a small library still
holds a small cache. Full, 36 slots is 987,840 bytes.

### The cheat lookup read three quarters of a megabyte, every time a sheet opened

The reported symptom was "cheat db search seems to lag music when opening details". Moving the
load off the transition into `background()` (f0192dd4) did not fix it, and could not have: the
main loop feeds the mixer either side of `background()`, not during it, so a long blocking call
there starves the DAC exactly as one in `enter()` did.

The blocking call was `cheatdb_load()` reading **`db_head.strtab_size` = 769,488 bytes** — the
whole database's string table, allocated and freed per lookup — to resolve a few hundred bytes of
group names. Format 1 interned names across all 341 games, which saved 41 KB on the card and cost
three quarters of a megabyte of I/O per detail sheet. Against ~133 ms of audio held ahead of the
DAC (8 buffers at 22,050 Hz), that is not close.

Format **2** puts each game's names in its own blob, after its group rows and codes, with a
`blob_size` in the index row (now 24 bytes, was 20). A load is one seek and one read:

| | bytes read per lookup |
|---|---|
| format 1 | 769,488 + group rows + codes |
| format 2, mean over 341 games | **5,106** |
| format 2, worst (GoldenEye, 4,315 cheats) | 240,736 |

### The database was missing seven of the twenty-four games on the card

Measured against the card's actual ROM headers, not estimated. Before: **17 of 24**. Two
independent causes, both in the key table generator:

* **Version was written as 0 where it meant "any".** `harvest_database()` gave every row
  `int(ver) if ver else 0`, but only `MATCH_ID_REGION_VERSION` names a version — the other two
  macros match every revision. The reader takes an exact match or the `0xFF` sentinel, so a stored
  0 matched revision 0 and nothing else. Cost five titles whose game code was sitting in the
  database holding hundreds of cheats: Banjo-Kazooie (v1, 269), Star Fox 64 (v1, 279), Rogue
  Squadron (v1, 219), Pokemon Stadium (v2, 626), Shadows of the Empire (v2, 388).
* **Region collisions were refused outright.** Two game codes for one title — Ocarina of Time's
  CZL and NZL, Mario Party's CLB and NLB — were dropped, on the correct reasoning that guessing
  between them applies one region's addresses to the other region's binary. It is not a guess,
  though: the corpus filename says which region it is, in the same notation the `rom_info.c`
  comments use. The join gained a second component instead of giving up, collapsed to three
  families (NTSC / PAL / Japan), and still refuses when the filename does not say. Cost the two
  biggest games on the card — Ocarina of Time alone has 553 cheats.

After: **24 of 24**, 341 games in the database (was 325), 1,039 of 1,345 corpus files keyed
(was 996).

Both were invisible from the console. A game with no cheats and a game whose cheats were keyed to
a revision nobody owns present identically: "None available".

### The music blipped just before the game started

Reported as "screen turns black, waits a bit, sound plays for a fraction of a second, game
launches". Nothing in the menu plays anything at that point, which is the clue: **an N64 whose AI
is not handed a new buffer does not fall silent, it keeps playing the one it has.** `do_load()`
blocks for the whole ROM stream with nothing feeding the mixer, so the DAC runs dry part way
through and repeats a fragment of the last thing mixed.

Two changes, and the first alone would probably do it:

* `on_progress()` — called throughout the blocking load — now pumps `sound_poll()`. A DAC being
  fed silence cannot repeat anything.
* The music fades with the picture over the same 0.55 s (`music_fade()`, which scales the player
  gain without touching the stored setting or releasing the player, as `music_set_volume(0)`
  would), and the player is released at black rather than in `app_deinit()` afterwards. So the
  last samples mixed before the load are already silent, and the fade is now a fade rather than a
  cut-off mid-note.

Unverified: the exact mechanism is inferred from the symptom and from how the AI behaves on
underrun, not measured — there is no way to observe the audio hardware between `boot()` and the
game. What is certain is that nothing fed the mixer for the duration of a load, and now something
does.

### The cheat engine: everything observable says yes, and cheats still do nothing

The first launch log is the whole problem in five lines:

```
rom      sd://roms/n64/Zelda - Ocarina of Time.z64
groups   1 loaded, 1 ticked
emitted  4 cheat words (2 lines)
engine   will hook
detail   CIC 6105/7105 word[499]=01200008 ok
```

`01200008` is `jr $t1`, the instruction the engine replaces, at the offset x105 expects. One group
ticked — that was the user's hand-entered cheat, because Ocarina of Time was one of the seven the
database was missing. Two lines emitted, both plain 16-bit writes. Nothing here is wrong.

Read and ruled out, none of it by measurement — all of it by reading against the VR4300 manual and
the alt64 original:

* `A_BASE`/`A_OFFSET` carry the sign correction, so a negative 16-bit offset does not land 64 KB
  away.
* `I_ORI(REG_K1, REG_ZERO, EXCEPTION_HANDLER_ADDRESS | WATCHLO_W)` truncates to 16 bits, giving
  `0x0181` — which is the correct WatchLo, because the field is the **physical** address and
  0x80000180 is physical 0x180.
* The engine's `I_BNE(REG_K0, REG_K1, 15)` lands on the first cheat instruction, and
  `final_engine_address + 20` is the `I_NOP()` placeholder, both counted out instruction by
  instruction.
* Both patcher loops' branch offsets (`-5` and `-4`) resolve to their own heads.
* `I_J(PATCHER_ADDRESS)` executed from DMEM lands at `0xA0700000`, the uncached mirror of the
  region `cheats_update_cache()` just wrote back. That is right, not a bug.
* `reboot.S` skips `reset_rdram` on `$a0 != 0`, so the staged engine survives.
* `cheat_list` is heap and never freed, and nothing allocates after it.

What is left is on the far side of `boot()`, and the leading candidate cannot be checked from a
desk: the engine hooks by arming a **watch exception** and waiting for the game to write its own
exception handler to 0x80000180. WatchLo/WatchHi are among the most obscure registers the VR4300
has. **The ModRetro M64 is a clone console, and an FPGA CPU core that omitted the watch exception
would break exactly this and nothing else anyone would notice.**

#### Two instruments, since there is no debug link

**The launch log now says everything the menu can know** — header key and check code, CIC and the
word at the patch offset, `get_memory_size()` and `is_memory_expanded()`, database coverage, which
groups are ticked by name, and **every emitted line decoded the way `cheats_get_next()` decodes
it**: the raw word, the type byte, and the address after the `& 0xA07FFFFF` mask. A code that does
nothing is either not reaching the engine, reaching it at the wrong address, or reaching it
correctly and being a wrong code, and those have three different fixes.

**And the menu is now able to be its own test subject** (`src/menu/enginetest.c`). A `volatile
uint16_t` in `.data`, its address printed on the Settings screen as a ready-made Datel line
(`810A5D50 BEEF` in the current build). Copy `sc64menu.n64` to `roms/n64/enginetest.n64` — a copy
under another name, because the scan skips the menu at the card root by name — open it, type the
code, tick it, launch. The engine writes on every exception, of which a menu drawing frames has
hundreds a second, so the copy that boots reports in its own Settings whether the write landed.

That is a definite answer either way, on the user's hardware, needing nothing but the card. If it
says the engine ran, a cheat that does nothing is a wrong code or a wrong game. If it says it did
not, nothing downstream of the IPL3 patch is running and no cheat will ever work on this console.

### Byte order, stated rather than assumed

`cheatdb.c` read its structs straight off disk and the fields came out right because mips64-elf is
big-endian and nothing else had ever run the file. Which is exactly why the reader could not be
tested: `tools/hosttest` compiles the real production C natively, and on a little-endian host a
raw struct read turns `'M64C'` into `'C64M'` and format 2 into 512. The suite's first run said so.

So the format is now explicitly big-endian, behind a `CHEATDB_SWAP` constant. **On target this
cost nothing measurable: `.text` was 569,976 bytes before and after** — the swap loops are
constant-folded away entirely.

### The new suite, and proof it can fail

`test_cheatdb.c` reads the real `build/cheats.db` through the production reader and compares
against `cheatdb_expect.py`, a parser written from the spec rather than from the C. Three
independent implementations — mkcheatdb.py writes, cheatdb_expect.py reads, cheatdb.c reads —
because a writer and reader sharing a header can be wrong together all day and round-trip
perfectly. Compare 1j and 1u, both of which are harnesses that measured the wrong thing and looked
green doing it.

**3,756 checks over all 341 games, 0 failures.** Every game's group count, code count, first
group name, first code line and **last** code line — the last one because a blob read one row
short still gets code 0 right.

The mutation: `codes_off` set to 0, so a game's codes are read from its own group table. **681 of
3,756 checks go red.** That mistake is invisible from the console — the detail sheet shows the
right names and the right count, and the engine is handed addresses taken from group headers.

Whole suite, `run.sh --mutate`: 29 + 31 + 47 + 3,756 + 23 checks, 0 failures, all five mutations
detected.

### Sizes

| | bytes |
|---|---|
| `.text` | 570,520 (+544 over 1ac) |
| `.data` | 107,916 |
| `.bss` | 75,912 |
| `output/sc64menu.n64` | 1,638,400 |
| `menu/cheats.db` | 1,749,404 (was 1,563,980) |

---

## 1ae. The cached-tile path made the whole console slow, and ares cannot see it

The card came back from 1ad reported as laggy at everything — music, input, scrolling — against a
run of the previous build that had felt fine. Two of the three causes were mine.

### Removing an early-out put a full library walk in the spin loop

`run_once()` opens with "is a decode in flight? then poll it and return". 1ad made cached mode
**fall through** that, on the reasoning that an atlas fetch goes nowhere near the decoder, so a
scroll beginning mid-decode could still fill from `thumbs.pak`.

The reasoning is true and the consequence was not thought through. That early-out is what kept
the four-pass walk from running during a **cold fill**, and a cold fill has a decode in flight
almost continuously. So every `thumbcache_run_cached()` call did a full walk instead of returning
in three instructions — and `background()` is called once per displayed frame **plus once per
spin iteration**, which `scroll-stress` measures at `spin=117880` over 600 frames: **196 walks per
displayed frame** where there had been none.

Nothing was lost by putting it back. A decode is in flight only while the cache is cold, and a
cold cache has nothing in the atlas to fetch.

### Every miss allocated a whole tile and threw it away

This one predates 1ad and had simply never been reachable often enough to matter. The atlas block
claimed a slot, `malloc`'d a `surface_t`, `surface_alloc`'d **27,440 bytes**, attempted the fetch,
and unwound all of it on a miss — per candidate, per pass, per call. On a cold card every
candidate misses.

Worse, `claim_slot()` may **evict** to produce a slot. So a full pool would destroy a resident
tile in order to fail to fill the space with a tile that was never in the atlas.

`thumbstore_has()` asks the resident index instead: a hash and a scan of a few hundred rows, no
allocation. Checked before anything is claimed.

Combined with the walk regression, the arithmetic on the reported run is roughly **196 walks per
frame × ~40 candidates = 8,000 whole-tile allocate-and-frees per displayed frame**, each with a
filesystem probe beside it. That is the lag. It is arithmetic from a measured call count, not a
measured result — see below.

### The redundant pass

1ad also called `thumbcache_run_cached()` before `thumbcache_run()` on settled frames. That is
pure duplication: `thumbcache_run`'s own walk already checks the atlas before the cost gate, which
is a deliberate ordering recorded in the source. So the entire four-pass walk ran twice per
`background()` call to reach the same answer. Removed.

### ares reports no difference, and that is the finding

`scroll-stress`, same script, same fixture, regressed build against fixed build:

| | before | after |
|---|---|---|
| `scanus` (walk, µs/frame) | 1,276 | 1,287 |
| `bg_us` (µs/frame) | 7,731 | 7,857 |
| `stats` (filesystem probes) | 4 | 4 |

Identical inside noise. **ares cannot execute the path at all**: the storage prefix there is the
ROM's read-only DFS, so `cache_writable()` is false, no pak is created, `thumbstore_available()`
returns false, and the whole atlas block — the allocate-and-free, the eviction, the probe — is
skipped by its own guard. The one machine that could have caught this before hardware is
structurally unable to.

This is section 5's known limit arriving with teeth. It previously read as "the streaming budget
is the one number ares cannot validate"; it is broader than that. **Any bug that lives inside
`if (thumbstore_available())` is invisible to every regression script in this repo.** The host
suite is the only instrument that reaches that code, and it tests `thumbstore.c`, not its caller.

So the fix here is a code-reading argument backed by a call count, and it is recorded as such. The
part that *is* verified is the new function: `thumbstore_has()` must agree with
`thumbstore_fetch()` on every key, or it either puts the allocation back or hides art that is
sitting on the card. Six checks in the atlas suite, which is now 37 and still goes red on the
slot-offset mutation.

### What was kept

`THUMB_SLOTS` stays at 36 and the ±2-row prefetch stays. Neither is implicated: the slots are
allocated lazily, and the prefetch only adds entries to a list that was already being walked. On
a cold card the larger pool does mean more of the library gets decoded before the cache goes
quiet, which is more work up front and a fully populated `thumbs.pak` at the end of it.

Cached mode is also now narrowed to what it is for: it never leaves the wanted list (the two
whole-library passes exist to decode ahead, and it does not decode), and it skips any record whose
`art_file` is still NULL, because `art_resolve()`'s five-rule search with up to three probes is
the expensive half of a question whose answer, for a record that has never been decoded, is always
no.

---

## 1af. The self-test could never have worked, and the replacement found something

The cheat-based engine self-test shipped in 1ad was wrong, and the console said so within minutes:
open the menu's own copy in the menu, and the detail sheet reads **"Not supported for this game"**.

That message is correct and it is the pre-flight doing exactly its job. **The one ROM whose insides
we control is the one ROM the engine cannot patch.** This is a libdragon ROM and libdragon ships
its own IPL3 rather than a retail libultra one; `cheats_ipl3_layout_ok()` looks for `jr $t1` at the
CIC's patch offset and refuses anything else. Measured with the production `cic_detect()` through
tools/hosttest/test_cheatinstall.c:

```
sc64menu.n64          CIC 6102/7101 word[475]=27bd0050  != jr $t1 -> engine would NOT hook
Super Mario 64.z64    CIC 6102/7101 word[475]=01200008  == jr $t1 -> engine WOULD hook
Ocarina of Time.z64   CIC 6105/7105 word[499]=01200008  == jr $t1 -> engine WOULD hook
```

`27bd0050` is `addiu $sp, $sp, 0x50`. libdragon's IPL3 is brute-force signed to pass as CIC 6102
but is not the retail boot code, and the layout check catches that -- which is the whole reason
that check exists. The design assumed a self-hosted test was possible without checking whether the
host qualified. It did not.

### Ask the CPU instead

No cheat, no second ROM, no launch. `src/menu/enginetest.c` arms a watch on eight bytes of its own
`.data`, stores to them, and reports whether the exception fired -- the same mechanism the engine
depends on, exercised while the menu is still alive to say what happened. Four outcomes, and they
mean four different things: the register does not hold, it holds but a cached store does not trap,
only an *uncached* store traps, or it works.

The uncached case is separated because it is a real possibility rather than a hypothetical: a watch
compares physical addresses in the pipeline, so on real silicon a cache hit traps like a miss, but
an implementation that only checks the memory bus would trap one and not the other. Reporting
"cannot run cheats" when the truth is "traps, but not the way the engine needs" would send someone
hunting the wrong fault.

### The positive control, and why the verdict would be worthless without it

"The store did not trap" has two explanations that look identical from inside the test: the CPU has
no watch exception, or the handler is never reached and no exception of any kind would be seen.

So the test raises a `break` first. Every VR4300 and every emulator of one implements it, and
stepping over it -- `ex->regs->epc += 4`, which libdragon documents as the supported way to correct
a synchronous fault -- proves the handler is wired up. If the control does not come back the
verdict is **"Inconclusive, control failed"** rather than the alarming and possibly wrong thing.

**Verified under ares before shipping**, which is the only place it could be verified:

```
WATCHTEST Not supported by this console
          (target 0x800a6a28, wrote 000a6a29, read 000a6a29, break=1, fired=0)
```

`break=1`: the handler ran and EPC+4 resumed cleanly, so the plumbing works and the menu boots
through it. The armed value read back **exactly**, so the register holds and the physical-address
arithmetic is right. And `fired=0` for both the cached and the uncached store.

### ares does not implement the watch exception

That is the finding, and it is bigger than the test. **The Datel cheat engine has never once run
under ares, and could not have.** Every cheat measurement in this file up to 1ad -- the group
model, the emitter, the IPL3 pre-flight, `test_cheatinstall.c` -- verifies what is handed *to* the
engine. Nothing has ever verified the engine itself, on any machine, because the one machine
available lacks the CPU feature it hooks with. That belongs beside section 5's other harness
limits and it is the more serious of the two found today.

It also means the verdict this test returns on the M64 is genuinely new information, whichever way
it goes.

### The interlock

A watch exception that fires and cannot be disarmed is an infinite exception loop on a boot path --
a console that will not start. The handler clears WatchLo before it does anything else, so it
should not happen; "should not" is not something to gamble a stranger's evening on, so the test
drops `/mainmenu/watchtest.busy` before it runs and removes it after. A marker still present at the
next boot means the last attempt did not return, and the test is never attempted again.

### Sizes

`.text` 571,320 (+800 over 1ae), `.data` 108,196, `.bss` 75,944.

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
6. **What firmware is on the cart?** Not an M64 question, and the cheapest one to answer. The menu
   refuses anything below 2.17.0 and lands on the fault screen — see 2a.
7. **Does the M64's CPU implement the watch exception?** The cheat engine hooks by arming
   WatchLo on physical 0x180 and trapping the game's write of its own exception handler. If the
   core does not implement it, no cheat can ever run and everything the menu can observe still
   says "will hook" — which is exactly what the first two hardware runs reported. **Answerable
   now, without a debug link**: the Settings screen prints a Datel line aimed at a probe in the
   menu's own `.data`, and a copy of the menu booted with that cheat ticked reports whether the
   engine wrote it. See 1ad and `src/menu/enginetest.h`.

~~**Can the menu write to the SD card at all?**~~ — answered by reading the SC64 firmware and
libcart rather than by testing: yes, and the write path needs nothing the read path does not
already have. See 2a. What remains open is throughput, not capability.

---

## 5. Known harness limits

- **ares has no SD card.** The fixture is read through DFS/PI, which is faster and
  lower-latency than FatFs over SC64. **The thumbnail streaming budget is therefore the one number
  ares cannot validate.** Plan: an artificial `--sd-delay-us` knob under `DEV_HARNESS` to test
  against a pessimistic 1.5 MB/s, and a real measurement on hardware.
- **ares does not implement the VR4300 watch exception.** Verified in 1af with a positive control:
  WatchLo reads back what is written, a `break` in the same window traps and returns through the
  handler, and neither a cached nor an uncached store to the watched address fires. **The Datel
  cheat engine hooks with that exception, so the engine has never once run on any machine
  available to this project.** Everything measured about cheats here is about what is handed *to*
  the engine. Nothing verifies the engine.
- **Worse than that: ares cannot execute the atlas read path at all.** `cache_writable()` is false
  under the read-only DFS, so no pak is created, `thumbstore_available()` returns false, and every
  line inside `if (thumbstore_available())` in thumbcache.c is skipped by its own guard. A
  regression that lives in there produces byte-identical hashes and identical frame times on every
  script in the suite — 1ae is one that did, and it made the console visibly slow at everything.
  The host suite reaches `thumbstore.c` but not its caller, so **thumbcache.c's cached branch is
  currently verified by nothing.** Nothing here is a plan yet; it is the largest hole in the
  instrumentation and it should be named as one.
- **ares is not cycle-accurate for RDP/RDRAM contention**, so `rdp_us` from the frame-time
  instrumentation is indicative, not predictive.
