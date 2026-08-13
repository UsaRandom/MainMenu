# Hardware bring-up log

This is a lab notebook from the first M64 + EverGenesis64 cart, not a setup guide. To put the
menu on a card, see [CARD.md](CARD.md).

Everything in this repo has been measured against ares. **ares is not an N64 and the M64 is not
an N64 either** — it is an FPGA reimplementation. Two layers of divergence sit between every
number in AUDIT.md and the real thing, so the order below is deliberate: each step fails in a way
that tells you something, and doing them out of order produces failures that do not.

## Before you start: the deployer, and what it gives us

Get `sc64-deployer-macos-<ver>.tgz` from the SummerCart64 releases page. It is not an emulator and
has nothing to do with SNES or NES support — but three of its subcommands decide how much of an
afternoon each step below costs.

```sh
./sc64deployer list                             # is the device even enumerated
./sc64deployer firmware info sc64-firmware-<ver>.bin
./sc64deployer firmware update sc64-firmware-<ver>.bin
```

**Check the firmware version first.** `src/flashcart/sc64/sc64.c:28` refuses anything below
**2.17.0** and refuses any major version other than 2, and it reports that as
`FLASHCART_ERR_OUTDATED` — which on a screen that has not drawn yet looks identical to not
booting. Latest release at time of writing is **2.20.2** (18 Nov 2024), which satisfies it. The
deployer is separately coupled to firmware versions and errors out on a mismatch of its own, so
deployer and firmware want to come from the same release.

**`sc64deployer debug` was meant to be our telemetry channel on hardware — and on the cart we
actually have, it is not available at all.** See "USB is dead on this cart" below; the rest of
this paragraph describes what a working SC64 would give us, and is kept because it is what to
expect from any cart whose USB does come up. It speaks the UNFLoader protocol, and
`flashcart_init` already calls `debug_init_usblog()` — guarded by `#ifndef NDEBUG`, and nothing
in our Makefile ever defines `NDEBUG`, so it is live in a release build too. Every `debugf` line
this project emits therefore arrives on the PC unchanged: `LIBRARY scanned …`,
`FRAME n=… worst_us=…`, `HEAP n=… bytes=…`, and the `emu:` lines from `cart_load.c`.
~~The whole measurement culture transfers to hardware without writing anything new.~~

What does *not* transfer: framebuffer capture. `DBG_FBDUMP` rides EMUX `XHEXDUMP`, which is an
ares extension. **The hash gate is ares-only, permanently.** On hardware we get numbers and no
pictures.

**`--direct` is the lever for step 1.** `sc64deployer upload --direct` disables the SC64
bootloader during boot and reset, and the SC64 manual names testing a custom IPL3 as its intended
use — which is exactly the unknown below. If a ROM boots one way and not the other, that
difference localises the fault to the CIC/bootloader handshake rather than to our code.

## Order of operations

### 1. Stock libdragon hello-world, before this menu

**Do not skip to step 2.** This menu is a libdragon ROM, and libdragon ships a custom IPL3 whose
checksum matches no stock CIC seed. Booting therefore depends on the flashcart's CIC emulation
agreeing with the console's PIF, and *that has never been verified on an M64*. lithium64's own
audit flags it as the top unknown.

If a stock hello-world fails, nothing in this repo can work and the problem is upstream of
anything we wrote. That is a completely different afternoon from "our menu has a bug", and it
costs about an hour to tell them apart.

```sh
sc64deployer upload /path/to/libdragon/examples/.../hello.z64
sc64deployer upload --direct /path/to/libdragon/examples/.../hello.z64   # if the first fails
```

Run both before concluding anything. The pair distinguishes "the SC64's CIC emulation and the
M64's PIF disagree" from "the console will not run a custom IPL3 at all", and those have
different fixes.

### 2. The menu, over USB rather than from the card

```sh
make sc64 -j8                    # no FIXTURE, no DEV_HARNESS
sc64deployer upload output/sc64menu.n64
sc64deployer debug               # in a second terminal, before powering on
```

USB first because it is the fast loop and it removes the SD card from the set of things that can
be wrong. If this boots to a grid — even an empty one — the IPL3 question is answered.

### 3. From the SD card, as users will run it

The SC64 loads `sc64menu.n64` **from the root of the card**. Layout the menu expects:

```
/sc64menu.n64
/roms/n64/*.z64                       games
/roms/{nes,snes,gb,gbc,sms}/*         emulated systems
/menu/metadata/<G>/<A>/<M>/<E>/boxart_front.png
/menu/cheats.db                       from tools/mkcheatdb.py; optional
/menu/emulators/{neon64bu.rom,lithium64.z64,gb.v64,gbc.v64,smsPlus64.z64}
/menu/config.ini
```

`build/fixture/` mirrors this exactly. Copying it onto a real card is the fastest way to get a
known-good tree, and it makes the hardware run directly comparable to every ares measurement.

## The card, as it stands (3 Aug 2026)

A 31.3 GB card arrived with the cart, already `MS-DOS FAT32` on an MBR partition and empty apart
from macOS's own `.Spotlight-V100` and `.fseventsd`. It was **not** reformatted — the SC64 takes
FAT32 or exFAT, and the factory format is one of them, so reformatting would have been a
destructive step taken for no reason.

What is on it, and why each thing rather than the alternative:

| path | what | provenance |
| --- | --- | --- |
| `/sc64menu.n64` | upstream N64FlashcartMenu **V0.3.2** | `eedcea28…` |
| `/menu/emulators/lithium64.z64` | our SNES core, 96 KB, release build | `1f116018…` |
| `/menu/emulators/neon64bu.rom` | NES, neon64v2 **v2.0-beta.4** | `f9fd3a29…` |
| `/menu/emulators/gb.v64`, `gbc.v64` | Game Boy / Color, gb64 | `d59a9031…`, identical files |
| `/menu/emulators/smsPlus64.z64` | SMS / GG, smsPlus64 **v0.7** | `8dc11807…` |
| `/menu/emulators/Press-F.z64` | Fairchild Channel F, Press-F-Ultra **r5** | `46b903d0…` |
| `/roms/snes/` | DKC (U) V1.2, Zelda ALTTP (USA), Super Mario World (U) | from `lithium64/reference/roms` |
| `/menu/{metadata,64ddipl}`, `/roms/n64`, `/roms/*/saves` | empty, structure only | — |

All six cores verified as big-endian N64 images (`80371240`) and checksum-matched against their
downloads. `sodium64.z64` is deliberately absent — `cart_load.c` only falls back to it when
`lithium64.z64` is missing, so shipping both would mean the fallback path never gets exercised
and never gets noticed when it breaks.

**`gb.v64` is generated, not downloaded, and that is worth knowing before regenerating it.** gb64
has no GitHub releases; it ships through a browser page
(`lambertjamesd.github.io/gb64/romwrapper/`) whose "Download Emulator" button runs a local
`convert()` over a `gb.n64` template. Rather than reimplement that, `scratchpad/mkgb64.js` lifts
`findIndex`, `copyInto` and `convert` out of the page by brace matching and runs them under node
against the real `crc.js`, so the bytes are the page's own code's output. The transform pads to
`0x101000` (truncating the 1,125,001-byte template), stamps `E`,`D` at `0x3C`/`0x3D` and the ED
save type at `0x3F`, zeroes the title field, and recomputes the CIC-6102 checksum — the run
reports *CRC 2 … (Bad, fixed)*. `gb.v64` and `gbc.v64` are byte-identical by design; the page
writes the same buffer to both names.

## USB is dead on this cart, and that costs us the whole telemetry plan

The cart is an **EverGenesis64**, a Chinese SC64 clone. It draws power over USB — the status LED
lights — but **presents no USB device at all**. Measured, across three cables (at least one
proven to carry data) and multiple ports including one a Teensy was enumerating through minutes
earlier: `ioreg -p IOUSB` shows no FTDI (`0x0403`), no CH340 (`0x1A86`), no Prolific, no CP210x,
and no unexplained vid/pid anywhere on the bus. `sc64deployer list` reports *No SC64 devices
found* throughout.

**The detection method was shown capable of failing before it was trusted.** Over the same
session the device count moved 11 → 10 → 9 as the Teensy and the card reader were unplugged and
replugged, and the card's mount appeared and disappeared with it. This is not a harness that
would sit silent regardless. A cart that enumerated would have shown up in it.

Note for anyone re-testing: a lit LED proves *power*, not data — a charge-only USB-C cable powers
the board identically. And an unprogrammed FT232H would still enumerate as `0403:6014`, so total
silence points at the chip being absent or its data lines unrouted, not at misconfiguration.

What this removes, all of it verified rather than assumed:

- **Firmware can never be updated on this cart.** Update is USB-only. The menu can only *read*
  the version (`sc64_get_firmware_version`, `sc64.c:250`); there is no SD-card or N64-side update
  path, and `primer.py`'s UART recovery route needs USB as well.
- **No `sc64deployer upload`.** Step 2 above — the menu over USB — is impossible. Everything
  reaches the cart through the SD card, which makes the edit/test loop a card swap.
- **No `sc64deployer debug`.** ~~The whole measurement culture transfers to hardware without
  writing anything new.~~ That claim above is now false and is struck rather than deleted. Every
  `debugf` line this project emits — `LIBRARY scanned …`, `FRAME n=… worst_us=…`, `HEAP …`, the
  `emu:` lines — has nowhere to go. Telemetry on hardware needs a new channel: an on-screen
  overlay, or a log file on the card once writes work. **Neither exists yet.**

The firmware version therefore becomes something to *read and live with* rather than fix. Boot
the card and open the Flashcart Information screen; if it reports below 2.17.0 the menu will
refuse the cart as `FLASHCART_ERR_OUTDATED` and there is no remedy short of different hardware.
Try Windows before concluding — FTDI's own drivers and FT_PROG are the SC64 build guide's
troubleshooting path, and a chip that is present but oddly programmed would show up there.

**Do not flash official firmware to a clone reflexively** if USB ever does come up. Run
`firmware backup` first. Clone boards may use different FPGA or MCU parts, and the SC64 docs list
30× [Long ON – Long OFF] as *"device is most likely bricked."*

**The root menu is upstream's, not ours, deliberately.** Step 2 above uploads our build over USB,
where the fast loop is; the card carries a known-good menu so that a failure to boot from it
cannot be our menu's fault. Ours replaces it once step 2 passes.

**The core on the card is a release build.** `build/.debug-off`, 98,304 bytes. lithium64's own
`debug.h` says never to ship a ROM built with `DEBUG`, and the tree's last build before this was
`DEBUG=1 DBG_SKIP=5000` — so the card would have got a frame-dumping build had it been copied
without rebuilding first.

**The core and the menu agree about where the SNES ROM goes, which was worth checking rather than
assuming.** `cart_load.c:162` writes the emulated ROM to cart offset `0x200000`;
`src/memory.S:224-225` probes exactly two offsets, `0x10104000` (where lithium64's own
`rom-converter.py` puts it) and `0x10200000`. The second is the menu's. Had it probed only the
converter's offset, every SNES title would have failed to boot from the menu while working
perfectly under `tools/run.sh`, which splices at the first.

Copier headers are handled: `cart_load.c:225` strips 512 bytes when `size & 0x3FF == 0x200`. Of
the three ROMs on the card only Super Mario World (524,800 B) has one.

**Firmware is not yet flashed.** `sc64deployer list` reports *No SC64 devices found* — the cart
was not connected over USB when the card was prepared, and firmware update is a USB-only
operation with no SD-card path. `sc64-firmware-v2.20.2.bin` is downloaded and verified
(`firmware info`: MCU `0x5490`, FPGA `0x196D0`, bootloader `0x10000`, primer `0xD5C`, FPGA at
102.891 MHz). **Check the version before concluding anything about a boot failure** — `sc64.c:28`
rejects anything below 2.17.0 as `FLASHCART_ERR_OUTDATED`, which looks exactly like not booting:

```sh
tools/sc64/sc64deployer list                                        # confirm it enumerates
tools/sc64/sc64deployer firmware backup sc64-firmware-backup.bin    # before updating, not after
tools/sc64/sc64deployer firmware update tools/sc64/sc64-firmware-v2.20.2.bin
```

v2.20.2 (18 Nov 2024) is still the newest SummerCart64 release as of today, so there is nothing
newer to weigh against it.

## What to measure first, and why these

Three numbers are load-bearing and **ares cannot produce any of them**.

### SD read throughput — the one number the whole cache design rests on

Every streaming figure in AUDIT.md came from DFS-over-PI, which is faster and lower-latency than
FatFs over SC64. §3 of the plan assumed ~3 MB/s and the thumbnail format was chosen against that
assumption.

Measure with a real card before writing the cache, not after.

### PNG decode, against the ares figure

AUDIT.md §1h measures 2.4–7.5 µs/pixel and 3.0 s per real card. That is CPU-bound work counted in
*emulated* VR4300 cycles, so it should carry to hardware better than any I/O number here — but
"should" is the word that gets checked. An FPGA VR4300's cache behaviour is its own thing.

If it carries, a cold 48-game library costs about 160 seconds on first boot, and the on-disk
cache stops being an optimisation and becomes the difference between shippable and not.

### Whether 640×480 progressive is accepted

`display_init(640, 480, INTERLACE_OFF)` is what we program. On an FPGA console with HDMI that is
plausible; on a real N64 it is not. If the M64 refuses it, the fallback is `INTERLACE_HALF` and
the interlace-versus-progressive A/B in DESIGN.md gets settled by necessity rather than by taste.

## Things that will only fail on hardware

- **Writes.** Nothing in this build has *executed* a write, because ares' DFS is read-only. The
  thumbnail cache, play history, parental locks and cheat selections all land here, and the first
  real attempt should be expected to find something.

  What is no longer in doubt is whether it is *possible*. The chain was read end to end against
  `SummerCart64` @ `a1e7996` and libdragon's own FatFs glue, and `CMD_ID_SD_READ` and
  `CMD_ID_SD_WRITE` sit behind the same `sd_get_lock(SD_LOCK_N64)`, the same address translation
  and the same count limit — so a menu that can list your games has everything it needs to write.
  `CART_ABORT()` returns `-1` rather than hanging, so a failure propagates to `cache_store()` and
  is dropped softly. Full trace in [AUDIT.md 2a](AUDIT.md).

  One consequence worth carrying into the first session: `app_deinit()` writes every cache file
  **after** `do_load()` has armed save writeback, and both use the same 8 KB of cart BRAM at
  `0x1FFE0000`. That is safe only because the firmware copies the sector table out immediately.
  **If saves come back corrupted after a launch, that is the first place to look.**
- **The Controller Pak.** The M64 has one built in. Whether it presents as a standard joybus
  mempak is unverified.
- **`is_memory_expanded()`.** Assumed true, and the cheat engine's address range depends on it.
  The check is still in the code and fails loudly rather than being compiled out.
- **The cheat engine end to end.** The only honest test is booting a game with a known
  `80xxxxxx yyyy` write cheat enabled and confirming the value landed. Everything up to that
  point — parsing, grouping, emitting — is verified; the engine itself patching a running game
  is not, and cannot be under an emulator that never boots the patched ROM.

## Getting the deployer

`sc64deployer` comes from the SummerCart64 releases page and goes in `tools/sc64/`, which is
gitignored — it is someone else's binary and does not belong in this history.

```sh
sc64deployer upload rom.n64                      # autodetects save type
sc64deployer upload rom.n64 --save-type eeprom4k --save game.sav
```
