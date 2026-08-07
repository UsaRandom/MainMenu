# Icon artwork

3,894 SVGs from **[game-icons.net](https://game-icons.net)**, by 36 authors, under
**[CC BY 3.0](https://creativecommons.org/licenses/by/3.0/)** except where an author released
their work as CC0. This is somebody else's artwork, kept here under a licence that asks to be
told about it. [`LICENSE.txt`](LICENSE.txt) is the corpus's own licence statement, verbatim as
it ships; the table below is the mention it asks for.

These are the faces a player picks in the profile screen. They ride in the cartridge as SVG text
and `src/libs/svg64` turns one into pixels at the moment it is needed, which is why there can be
thousands of them and why they take whatever two colours a player chose.

## Layout

    assets/icons/<author>/<name>.svg

`tools/mkpack.py` walks this tree and keys every icon as `<author>/<name>` — the author is the
directory it sits in, so the layout *is* the provenance record, and moving a file between
directories misattributes it. Filenames and directory names are upstream's, unchanged.

Upstream publishes the corpus as `<fg>/<bg>/<aspect>/<author>/`; these are from the `ffffff`
on `000000` at `1x1` variant, flattened, because the colours in the file are discarded on the
console anyway.

`.gitattributes` marks this directory `-text`. Line endings here are content: `mkpack.py` copies
an `.svg` into the pack byte for byte, so normalising them would make the pack a property of the
machine that cloned the repository rather than of the commit.

## What is not here

286 of the corpus's 4,180 icons are left out. Every icon was reviewed individually and the ones
whose *subject* is recognisably someone else's property — Captain America's shield, a Carcassonne
meeple, a tetromino — were dropped; `tools/ip-blocklist.txt` lists all 286 with the reason for
each, and svg64's `docs/ICON-IP-REVIEW.md` describes how the review was done. That is a decision
about subjects and not about the licence, and it changes nothing that is owed here: leaving
artwork out does not reduce what is owed for the artwork kept.

`tools/iconcheck.py` asserts this directory and that blocklist cannot drift apart.

## Icons made by

The address beside a name is the one the author gave for themselves; CC BY 3.0 asks for the URI
an author specifies, not only their name, and 20 of the 36 supplied one. Counts are of what is in
this directory, not of what the authors have drawn.

| Directory | Author | Icons | Their site |
| --- | --- | ---: | --- |
| `delapouite/` | Delapouite | 1,865 | https://delapouite.com |
| `lorc/` | Lorc | 1,362 | http://lorcblog.blogspot.com |
| `skoll/` | Skoll | 135 | |
| `viscious-speed/` | Viscious Speed (CC0) | 121 | http://viscious-speed.deviantart.com |
| `caro-asercion/` | Caro Asercion | 118 | |
| `sbed/` | Sbed | 84 | http://opengameart.org/content/95-game-icons |
| `aussiesim/` | Aussiesim | 52 | |
| `darkzaitzev/` | DarkZaitzev | 30 | http://darkzaitzev.deviantart.com |
| `cathelineau/` | Cathelineau | 29 | |
| `faithtoken/` | Faithtoken | 10 | http://fungustoken.deviantart.com |
| `lord-berandas/` | Lord Berandas | 10 | http://berandas.deviantart.com |
| `quoting/` | Quoting | 10 | |
| `priorblue/` | PriorBlue | 7 | |
| `willdabeast/` | Willdabeast | 7 | http://wjbstories.blogspot.com |
| `carl-olsen/` | Carl Olsen | 6 | https://twitter.com/unstoppableCarl |
| `felbrigg/` | Felbrigg | 5 | http://blackdogofdoom.blogspot.co.uk |
| `lucasms/` | Lucas | 5 | |
| `seregacthtuf/` | *seregacthtuf* | 5 | |
| `guard13007/` | Guard13007 | 3 | https://guard13007.com |
| `john-redman/` | John Redman | 3 | http://www.uniquedicetowers.com |
| `pierre-leducq/` | Pierre Leducq | 3 | |
| `rihlsul/` | Rihlsul | 3 | |
| `andymeneely/` | Andy Meneely | 2 | http://www.se.rit.edu/~andy/ |
| `generalace135/` | GeneralAce135 | 2 | |
| `heavenly-dog/` | HeavenlyDog | 2 | http://www.gnomosygoblins.blogspot.com |
| `kier-heyl/` | Kier Heyl | 2 | |
| `various-artists/` | *various-artists* | 2 | |
| `zajkonur/` | Zajkonur | 2 | |
| `zeromancer/` | Zeromancer (CC0) | 2 | |
| `catsu/` | Catsu | 1 | |
| `irongamer/` | Irongamer | 1 | http://ecesisllc.wix.com/home |
| `john-colburn/` | John Colburn | 1 | http://ninmunanmu.com |
| `pepijn-poolman/` | Pepijn Poolman | 1 | |
| `sparker/` | Sparker | 1 | http://citizenparker.com |
| `spencerdub/` | SpencerDub | 1 | |
| `starseeker/` | Starseeker | 1 | |

`seregacthtuf` and `various-artists` are set in italics because they are directory names rather
than people. Both postdate the corpus's own `LICENSE.txt`, which names the other 34, so they are
credited as upstream labels them. Inventing a person's name for an attribution file would be
worse than an imprecise one.

## Where this is said again

Naming the authors in a file in a source tree is not the same as naming them to a player holding
the cartridge, and CC BY asks for the latter. [`docs/CREDITS.md`](../../docs/CREDITS.md) is the
one the console shows: `tools/mkcredits.py` bakes it into the ROM and the credits screen renders
it. It lists all 36 regardless of how many icons a given build packs. If you add or remove an
author here, that file is the one that has to change with it.
