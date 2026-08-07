# Setting up a card

Only the first line is required. Everything else is optional, and leaving it out costs you exactly
that one feature and nothing else.

```
/sc64menu.n64                              the menu itself
your games                                 anywhere, in any arrangement you like
/emulators/…                               cores for the non-N64 systems
/cheats.db                                 the cheat database
/metadata/…                                a downloaded box-art pack
/mainmenu/                                 created by the menu; see below
<game folder>/saves/                       created when a game saves; see Players
```

**How you arrange your games is up to you.** The menu searches the whole card, five levels deep,
and works out what each file is by reading it. Folders, naming and nesting are all free — a folder
per system is a reasonable habit and nothing more than that. A `/roms` folder still works, because
it is just another folder.

Six folder names are skipped wherever they appear, because none of them holds games:
`mainmenu`, `menu`, `metadata`, `emulators`, `saves` and `System Volume Information`. So are
folders and files beginning with a dot, which is what keeps the litter your computer leaves on the
card out of your library.

`emulators` matters more than it looks: cores are named `neon64bu.rom`, `gb.v64`,
`lithium64.z64` and so on, and every one of those is a normal game extension. Without the skip
they would all appear in your library as games.

**Where the menu puts its own files.** `/mainmenu/` holds `config.ini`, the parental file, the
player roster and a `cache/` folder of decoded covers and play history. All of it is disposable
except the roster: delete the folder and the menu rebuilds it from your card, losing your settings,
your favourites, your play history and the names and faces your players chose. **Your saved games
are not in there** and are never touched by deleting it. The three places above are each *also*
looked for inside `/mainmenu/`, and in a `/menu/` folder if you have one from an older version, so
an existing card needs no rearranging.

## Players

Up to ten people can share a card. A card with one player looks exactly like a card from before
this existed, and that is deliberate rather than incidental — **player 1 writes where the menu has
always written**, so upgrading cannot strand a save:

```
<game folder>/saves/Game.sav               player 1
<game folder>/saves/p2/Game.sav            player 2
/mainmenu/cache/playstate.dat              player 1's favourites and history
/mainmenu/cache/p2/playstate.dat           player 2's
/mainmenu/profiles.ini                     the roster: names, faces, colours
```

Saves sit beside the game they belong to rather than in one place, because that is where the menu
already put them.

Cheat *definitions* are shared — one `cheats.db` for the card — but which of them are switched on
is per player. So is the play history. The parental code is deliberately **not**: it is one code
for the whole card, so switching player can never be the way around a locked game.

`profiles.ini` is the one file under `/mainmenu/` worth keeping. Deleting it does not lose a save,
but every player comes back nameless and faceless in slot order.

**Deleting a player deletes their saved games.** The menu counts them and says how many before it
asks, and player 1 cannot be deleted at all — those are the unsuffixed paths above, which on a card
that predates players is every save on it.

| system | file types |
|---|---|
| N64 | `.z64` `.n64` `.v64` `.rom` |
| NES | `.nes` |
| SNES | `.sfc` `.smc` |
| Game Boy / Color | `.gb` / `.gbc` |
| Master System / Game Gear | `.sms` `.gg` `.sg` |

Anything else is ignored, which is what lets cover images and save files sit in the same folders
as the games.

## Covers

A cover can be a file you dropped next to a game or one from a bulk pack, and they are looked at
in a fixed order. **A file you placed always beats the pack**, deliberately: a pack is something
you downloaded, a file you put there is a decision, and the decision should win.

| | where | example |
|---|---|---|
| 1 | an image named for the **game code**, anywhere on the card | `NLAE.png` |
| 2 | an image named for the **game file**, anywhere on the card | `Lantern Reef.jpg` |
| 3 | the art pack, for your exact region | `/metadata/N/L/A/E/boxart_front.png` |
| 4 | the art pack, any region | `/metadata/N/L/A/boxart_front.png` |
| 5 | the art pack, flat | `/metadata/NLAE.png` |

Worth knowing:

- Loose images can be **`.png`, `.jpg` or `.jpeg`**. An art pack is read as **`.png` only**.
- JPEGs must be **baseline, not progressive**. Progressive is the one kind that cannot be read at
  all, and the cover simply comes up blank. If a card full of art shows nothing, check this first:
  `magick in.jpg -interlace none out.jpg`, or re-save with "progressive" unticked.
- A **half-downloaded image** is not detected — it draws a part-blank cover. Download it again and
  the menu will notice the file changed and redo it.
- Case and extension are ignored, so `nlae.JPG` and `NLAE.png` are the same thing. If two files
  could both match, the one in the shallower folder wins.
- Options 1 and 3–5 need an N64 game code, which **NES, SNES, Game Boy, Master System and Game
  Gear titles do not have**. Option 2 is the one that works for them: name the image after the
  game file.
- Any size and either shape is fine — everything is scaled and cropped on the way in.
- A game with no cover gets a plain tile with its name on it. That is how it is meant to look, not
  a sign that something is missing.

The first time a card is read, each cover takes **about a quarter of a second**, and longer for a
very large image. This happens once: covers are saved into `/mainmenu/cache` and every later start
reads them straight back. Expect the grid to fill in over the first minute on a new card.

## Cheats

`/cheats.db` is the cheat database for N64 games. It is built from
[libretro's cheat collection](https://github.com/libretro/libretro-database) with
`tools/mkcheatdb.py --fetch`, and is not part of the download.

**Cheats are switched on as named groups, never line by line.** Many cheats are two codes that
only work together, and offering half of one silently changes the wrong thing in the game. You can
also enter your own from the menu, which is the only option for the emulated systems and for
anything newer than the collection.

## Parental controls

The code is six presses of the C buttons. Once one is set, it is asked for before a locked game
will start, before the parental panel opens, and **before the console clock can be set** — the
playing-hours window is measured against that clock and nothing else, so leaving it open would be
leaving the window open. With no code set none of that applies and nothing is enforced.

Wrong tries make you wait: five seconds after the first, ten after the second, and so on up to ten
minutes. Getting it right clears the count. Switching the console off does not — the number is on
the card, not in memory — so there is nothing to be gained by resetting.

### If you forget the code

Put the card in a computer and delete **`/mainmenu/parental.ini`**. That is the whole recovery.

It holds the code, the schedule and the count of wrong tries, and nothing else — so deleting it
costs you nothing you have set anywhere else in the menu. With no code, nothing is enforced: the
games you had locked become playable again, but they stay marked, so setting a code again brings
every one of those padlocks straight back.

The code itself is not in there in a form anyone can read, and there is no master code. Deleting
the file is the only way in, which is deliberate — a back door is a back door whoever finds it.

## Emulator cores

```
/emulators/{neon64bu.rom,lithium64.z64,gb.v64,gbc.v64,smsPlus64.z64}
```

SNES games run on [lithium64](https://github.com/UsaRandom/lithium64), a fork of sodium64 aimed at
this console, falling back to `sodium64.z64` if that is what you have.
