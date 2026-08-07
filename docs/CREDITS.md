# Credits and licences

This file is the single source of truth for everything this program owes to someone else. It is
baked into the cartridge by `tools/mkcredits.py` and shown by the Credits screen, so **it is not
documentation about the product — it is part of the product**. Editing it changes what the console
displays. Adding a dependency without adding it here is a bug.

Format rules, because the baker is simple on purpose:

- `# ` starts a section, `## ` a subsection. Everything else is a paragraph or a `- ` bullet.
- No tables, no nested lists, no inline links — put the URL on its own line.
- **ASCII only.** The 40 px and 32 px faces carry restricted charsets and the baker refuses any
  character outside `assets/fonts/charset.txt`. A smart quote pasted in here fails the build.

---

# Source code

Copyright (C) 2026 UsaRandom. Portions copyright the N64FlashcartMenu contributors, whose project
this is a fork of.

This program is free software: you may redistribute it and change it under the terms of the GNU
Affero General Public License as published by the Free Software Foundation, either version 3 of
that licence or, at your option, any later version.

There is NO WARRANTY for this program, to the extent permitted by law. See sections 15 and 16 of
the licence for the full disclaimer.

A copy of the licence travels with the source as LICENSE.md. The complete corresponding source,
including everything needed to build this exact cartridge, is at:

  https://github.com/UsaRandom/MainMenu

The AGPL says that anyone handed the program can also get its source. That is what the address
above is for. If you were given this cartridge and that address no longer works, whoever gave it
to you is the one the licence obliges to supply the source.

## Built from

This is a fork of N64FlashcartMenu by Polprzewodnikowy, which is where all of the hardware
knowledge in it comes from: the cartridge drivers, the CIC handling, the save detection and the
cheat engine.

  https://github.com/Polprzewodnikowy/N64FlashcartMenu

Also AGPL-3.0.

---

# Icon artwork

The icons a player picks from are from game-icons.net and are licensed CC BY 3.0, except where
an author released their work as CC0, marked below.

  https://game-icons.net
  https://creativecommons.org/licenses/by/3.0/

CC BY is an attribution licence. This cartridge redistributes the artwork, so the authors have to
be named, and naming them is what this section is for. That obligation stands whole regardless of
how many icons a given build actually packs.

Icons made by, with the site each author gave for themselves where they gave one:

- Delapouite - https://delapouite.com
- Lorc - http://lorcblog.blogspot.com
- Skoll
- Caro Asercion
- Viscious Speed (CC0) - http://viscious-speed.deviantart.com
- Sbed - http://opengameart.org/content/95-game-icons
- Aussiesim
- DarkZaitzev - http://darkzaitzev.deviantart.com
- Cathelineau
- Quoting
- Lord Berandas - http://berandas.deviantart.com
- Faithtoken - http://fungustoken.deviantart.com
- Willdabeast - http://wjbstories.blogspot.com
- PriorBlue
- Carl Olsen - https://twitter.com/unstoppableCarl
- Felbrigg - http://blackdogofdoom.blogspot.co.uk
- Lucas
- seregacthtuf
- John Redman - http://www.uniquedicetowers.com
- Kier Heyl
- Guard13007 - https://guard13007.com
- Pierre Leducq
- Rihlsul
- Zeromancer (CC0)
- Zajkonur
- HeavenlyDog - http://www.gnomosygoblins.blogspot.com
- GeneralAce135
- Andy Meneely - http://www.se.rit.edu/~andy/
- Various artists
- Starseeker
- SpencerDub
- Sparker - http://citizenparker.com
- Pepijn Poolman
- John Colburn - http://ninmunanmu.com
- Irongamer - http://ecesisllc.wix.com/home
- Catsu

The addresses are there because CC BY 3.0 asks for the URI an author specifies, not only their
name, and the corpus supplies one for twenty of the thirty-six. The other sixteen gave none.

Two of those names are directory names rather than people. seregacthtuf and Various artists
postdate the corpus's own license.txt and are not listed in it, so they are credited as they are
labelled upstream. Inventing a person's name for an attribution file would be worse than an
imprecise one.

A few of the corpus's icons are not packed into this build. That is a decision about the subjects
some of them depict and it changes nothing here: leaving artwork out does not reduce what is owed
for the artwork kept, which is why the list above is the whole corpus rather than the part that
shipped.

## Rendered by

svg64, which turns the SVG text into pixels on the console at the moment an icon is needed. MIT.

  https://github.com/UsaRandom/svg64

---

# Fonts

Firple, by negset. SIL Open Font License 1.1.

  https://github.com/negset/Firple

Firple is itself built from two other faces, and their authors are named in the licence too:

- Fira Code, (c) 2014 The Fira Code Project Authors
- IBM Plex, (c) 2017 IBM Corp.

The full licence text is in assets/fonts/LICENSE-Firple.txt, which the OFL requires travel with
the font.

---

# Sound

Sound effects from Pixabay, under the Pixabay Content Licence.

  https://pixabay.com/service/license-summary/

- Skyscraper_seven, for the cursor click
- Liecio, for the enter, back and settings sounds
- Universfield, for the error sound

The licence asks for no attribution. They are named anyway.

# Music

Twenty-eight songs, released CC0 into the public domain. Nothing is owed for them.

They are MIDI, not recordings. The console holds the notes and plays them itself, with midi64.

---

# Libraries

- libdragon, the SDK this is built on. Unlicense.
  https://github.com/DragonMinded/libdragon
- libspng, for PNG decoding. BSD 2-Clause.
  https://github.com/randy408/libspng
- miniz, for deflate. MIT.
  https://github.com/richgel999/miniz
- picojpeg, for JPEG decoding. Public domain, by Rich Geldreich.
- midi64, for music synthesis. MIT.
- acutest, for the host test suite. MIT. Not shipped in the cartridge.
- svg64, for icon rasterisation. MIT.

Two more arrive inside libdragon and are linked into this cartridge, so they are named here even
though this project did not choose them:

- FatFs, by ChaN, which is the filesystem that reads the SD card.
  http://elm-chan.org/fsw/ff/
- libcart, by devwizard64, which is the driver that detects the flashcart and talks to its card
  slot. The copy inside libdragon states no licence terms.
  https://github.com/devwizard64/libcart

---

# Emulators

Other systems are run by other people's emulators, loaded from the card. None of them are part of
this program and each carries its own licence.

- neon64v2, by hcs64. ISC.
  https://github.com/hcs64/neon64v2
- sodium64, by Hydr8gon. GPL-3.0.
  https://github.com/Hydr8gon/sodium64
- gb64, by lambertjamesd. MIT.
  https://github.com/lambertjamesd/gb64
- smsPlus64, by fhoedemakers. GPL-3.0.
  https://github.com/fhoedemakers/smsplus64
- Press-F-Ultra, by celerizer. MIT.
  https://github.com/celerizer/Press-F-Ultra

---

# Data

The game database that recognises a cartridge and knows how it saves is derived from the ares
emulator's database. ares is ISC licensed, copyright (c) 2004-2025 ares team, Near et al.

  https://ares-emu.net

The cheat codes, where a build ships them, come from libretro-database. MIT.

  https://github.com/libretro/libretro-database

---

# Not affiliated

This project is not affiliated with, endorsed by, or connected to any console maker, cartridge
maker, or game publisher. Every product name that appears in it belongs to whoever owns it.

It IS derived from N64FlashcartMenu, which is stated above and is the point of a fork. Nothing
here is endorsed by that project either, and any fault in this cartridge is this project's own.
