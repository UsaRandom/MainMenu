# Setting up a card

You need:

- A [SummerCart64](https://github.com/Polprzewodnikowy/SummerCart64) (or a compatible cart)
- A microSD card
- The `sc64menu.n64` file from a [release](https://github.com/UsaRandom/MainMenu/releases)
- Your games

Everything except the first two files is optional. Leave something out and you only lose that one feature.

## 1. Put the card in a computer

Most cards that ship with the cart already work. If you have to format one, choose **FAT32** or **exFAT**. Do not pick NTFS or APFS.

Eject the card properly when you are done copying. A card that was yanked out can come up read-only on the console, and then every start feels like the first one.

## 2. Copy the menu

Copy `sc64menu.n64` to the **top** of the card — next to the folders, not inside one.

## 3. Copy your games

Put them anywhere you like. Folders are fine. One folder per system is a nice habit, not a rule.

| system | files it opens |
|---|---|
| N64 | `.z64` `.n64` `.v64` `.rom` |
| NES | `.nes` |
| SNES | `.sfc` `.smc` |
| Game Boy / Color | `.gb` `.gbc` |
| Master System / Game Gear | `.sms` `.gg` `.sg` |

Unzip them first. A `.zip` on the card is just a zip.

Do not put games inside folders named `emulators`, `metadata`, `saves`, `menu`, or `mainmenu`. Those are reserved, and the menu will walk past them.

If you have hundreds of games, split them into folders (by system, or A–Z). One giant folder is slow.

## 4. Put the card in the cart and turn the console on

The first start can take a minute while pictures are read. After that it should be quick.

The menu creates a `mainmenu` folder on its own. You can ignore it.

### On the console

- D-pad moves, **L** / **R** change tabs
- **A** opens a game's page, **A** again starts it
- **B** goes back
- **C-right** is favourite
- **Start** is settings

---

## Optional extras

### Cover pictures

Drop a `.png` or `.jpg` next to the game, with the same name:

```
Lantern Reef.z64
Lantern Reef.jpg
```

That is the whole rule for most people. NES, SNES, Game Boy and Master System games only work this way — they have no N64 game code to look up.

If every cover is blank, the pictures are probably **progressive JPEGs**. Re-save them as a normal JPEG (untick “progressive”).

A downloaded art pack can go in a folder called `metadata`. A picture you placed yourself always wins over the pack.

The first time a card is read, each cover takes a fraction of a second. They are stored in `mainmenu/cache` and later starts just read them back.

### Other systems

N64 games run on their own. Everything else needs an emulator file in a folder called `emulators` at the top of the card:

| system | file to put in `emulators/` |
|---|---|
| NES | `neon64bu.rom` |
| SNES | `lithium64.z64` (or `sodium64.z64`) |
| Game Boy | `gb.v64` |
| Game Boy Color | `gbc.v64` |
| Master System / Game Gear | `smsPlus64.z64` |
| Channel F | `Press-F.z64` |

Without the matching file, that tab simply does not appear. The same files also work inside `mainmenu/emulators` or an older `menu/emulators` folder.

### Cheats

The menu already contains a cheat database for N64 games from [libretro's collection](https://github.com/libretro/libretro-database). You can also drop `cheats.db` on the card (top, `mainmenu`, or `menu`) if you want a newer one. If you are building from source, `tools/mkcheatdb.py --fetch` makes it.

Cheats are switched on by name, not line by line. Two codes that only work together stay together. You can type your own from a game's cheat list with **R**.

**Start → Settings** has two rows for this:

- **Use cheat database** — Yes (the usual case) lists the shipped codes for that game. No hides them, so only codes you typed stay in the list. The database itself stays on the card either way.
- **Cheat engine** — Cartridge is the default. Classic is the older engine; try it on a title Cartridge cannot hook. The two never run together.

A game with no codes at all still lets you type one.

### More than one person

**L** off the first tab opens the player list (or **B** on the grid, when there is more than one person). Each person gets their own saves, favourites, history and colours. A card with one player looks the same as it always did.

Player 1 is the original person on the card and cannot be deleted — those are the saves that were already there. Deleting anyone else deletes their saved games. The menu counts them and asks first.

The parental code is one code for the whole card, so switching player is never the way around a lock.

### Parental locks

**Start → Parental.** The code is six presses of the C buttons. It is asked for before a locked game, before that panel, and before the clock can be changed (playable hours use that clock).

Wrong tries make you wait: five seconds, then ten, up to ten minutes. Getting it right clears the count. Switching the console off does not — the number is on the card.

**If you forget the code:** put the card in a computer and delete `mainmenu/parental.ini`. That file is only the code, the schedule and the wrong-try count. Locked games become playable; set a code again and the padlocks come back.

There is no master code. Deleting that file is the only way in.

---

## If something is wrong

The boot screen says the urgent ones. **Start → Settings → System info** is the full list --
the page to photograph when something is wrong.

| what you see | what to try |
|---|---|
| Empty grid | Games are zipped, or they sit inside `emulators` / `metadata` / `saves` / `menu` / `mainmenu`. Move them out. |
| No NES / SNES / Game Boy tab | The matching file is missing from `emulators/`. |
| No covers | First boot is still working, or the JPEGs are progressive, or there are no pictures on the card. |
| Slow every time you start | The card cannot be written. Check the lock switch. Eject it properly from the computer next time. |
| "files in one folder" | Split that folder (by system, or A–Z). One giant folder is slow. |
| Forgot the lock code | Delete `mainmenu/parental.ini`. |
| A game has no cheats | That title is not in the database, or **Use cheat database** is off. You can still type codes on its page. |

Deleting the whole `mainmenu` folder makes the menu forget settings, favourites, history and player faces. **Saved games are not in there** and are not touched.

---

## More detail

Skip this unless you are placing a bulk art pack, editing `boxart.ini`, or chasing a cheat that does nothing in-game.

### How covers are found

Looked at in this order. The first match wins.

| | where | example |
|---|---|---|
| 1 | an image named for the N64 **game code**, anywhere | `NLAE.png` |
| 2 | an image named for the **game file**, anywhere | `Lantern Reef.jpg` |
| 3 | the art pack, your exact region | `metadata/N/L/A/E/boxart_front.png` |
| 4 | the art pack, any region | `metadata/N/L/A/boxart_front.png` |
| 5 | the art pack, flat | `metadata/NLAE.png` |

- Loose images: `.png`, `.jpg` or `.jpeg`. A pack is `.png` only.
- Case does not matter. If two files could both match, the one in the shallower folder wins.
- Options 1 and 3–5 need an N64 game code. Other systems use option 2.
- Any size is fine. The tile takes the shape of the cover (portrait, square, or landscape). A game with no cover gets a plain tile with its name on it.
- A half-downloaded image draws a part-blank cover. Download it again.

### Box art shapes

**Start → Settings → Box art** is **Automatic** unless you change it. Automatic measures each cover. **NTSC** uses a fixed table per system instead. Further choices are sections you have written into `mainmenu/boxart.ini`:

```ini
[pal]
nes  = 135x190
snes = 127x181
gb   = 126x126
```

Only the ratio matters. A section only has to name the systems it changes. Changing the setting re-cuts every cover once; the old cuts stay cached.

### How saves are stored

```
Game folder/saves/Game.sav          player 1
Game folder/saves/p2/Game.sav       player 2
mainmenu/cache/playstate.dat        player 1's favourites and history
mainmenu/cache/p2/playstate.dat     player 2's
mainmenu/profiles.ini               names, faces, colours
```

`profiles.ini` is the one file under `mainmenu` worth keeping. Deleting it does not lose a save, but everyone comes back nameless.

### Folders the menu will not search

`mainmenu`, `menu`, `metadata`, `emulators`, `saves`, `System Volume Information`, and anything whose name starts with a dot. That last one is why the hidden files a Mac leaves on the card do not show up as games.

Cores are skipped for a second reason: they use the same extensions as games (`.rom`, `.v64`, `.z64`). Without the skip they would appear in the grid.

An older card that still uses `/menu` does not need rearranging. Content is still read from there.

### Cheat beacon

If cheats are ticked and nothing happens in the game, add this to `mainmenu/config.ini` and launch again:

```ini
[menu]
cheat_beacon = true
```

A band is painted across the middle of the game, every frame the engine runs.

- **Green** — hooked through the game's own exception handler
- **Red** — hooked through the Datel watch trap (unexpected on this console)
- **No bar** — the engine never ran

It draws over the game on purpose. Take the line out when you are done.

`mainmenu/launch.log` will have a `beacon self-test` line. **PAINTED** means the instrument works, so a missing bar is about the engine. **DID NOT PAINT** means the instrument itself failed — report that.
