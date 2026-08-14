#!/usr/bin/env python3
"""Convert a raw Game Boy battery save (.srm/.sav) into gb64's flashram save format.

The menu loads GB/GBC games through gb64, whose save file is NOT a raw SRAM image:
it is the emulator's own 128 KB flashram layout (gb64-next/src/save.c). A raw dump
handed to the menu fails twice over -- flashcart_load_save() rejects any file whose
size is not exactly SAVE_SIZE[FLASHRAM_1MBIT] (flashcart.c), and even padded, gb64
would find no 'GB64' header at offset 0 and treat the flash as empty.

Layout produced here, derived from gb64-next @ a5ce8da and checked against the
loaders rather than the savers:

  0x00  struct GameboySettings (64 bytes, big-endian, MIPS o32):
        header      u32  = 0x47423634 'GB64'         (gameboy.h GB_SETTINGS_HEADER)
        version     u32  = 3                          (GB_SETTINGS_CURRENT_VERSION)
        flags       u16  = 0
        bgpIndex    u16  = 0
        obp0Index   u16  = 0
        obp1Index   u16  = 0
        inputMapping     12 u8 + u32 reserved2        (gameboy.h:64; defaults from
                                                       gameboy.c gDefaultSettings)
        graphics    u32  = 2                          (bitfield word: MSB-first
                                                       unused:27 smooth:1 scale:4;
                                                       ScreenScale1_5 = 2)
        (4 bytes padding -- timer is u64-aligned)
        timer       u64  = 0                          (RTC base; settable in-game)
        storedType  u32  = 1  StoredInfoTypeSettingsRAM (gameboy.h:103 -- settings
                                                       plus cart RAM, no CPU state)
        compressedSize u32 = 0                        (uncompressed: loadRAM() then
                                                       reads RAM from the offset
                                                       below instead of inflating)
        wallAtSave  u64  = 0                          (v3: SC64 wall clock at
                                                       save; 0 = none, so the
                                                       first load applies no
                                                       off-time delta)
  0x80  cart RAM, verbatim from the input file. ALIGN_FLASH_OFFSET(64) == 128
        (save.c: offsets round up to the 0x80 flash block).
  ....  0xFF to 128 KB, the erased state the menu itself fills fresh saves with.

The RAM length gb64 reads back is decided at runtime by the GB ROM header's RAM
size code, not by anything stored here, so a short input (8 KB carts) simply
leaves erased flash behind it, which is what a real cart with less RAM looks like.

The same layout serves both of gb64's battery-backed modes -- only the container
size differs: SaveTypeFlash is 128 KB, SaveTypeSRAM3X (banked SRAM, what our
cores are stamped for after the flash path failed to read back anything on the
M64 + EverGenesis64) is 96 KB, matching the menu's SAVE_SIZE[SRAM_BANKED].

Usage: mkgb64sav.py [--sram3x] IN.srm OUT.sav   (also: --extract OUT.srm IN.sav SIZE)
"""

import struct
import sys

FLASH_SIZE = 128 * 1024
SRAM3X_SIZE = 96 * 1024
RAM_OFFSET = 0x80

# gameboy.c gDefaultSettings, as enum values from gameboy.h:25 --
# right=RD left=LD up=UD down=DD a=A b=B select=Z start=START
# save=DC load=UC openMenu=LC fastForward=RC
DEFAULT_INPUT = bytes([8, 9, 11, 10, 15, 14, 13, 12, 2, 3, 1, 0])


def settings_block(stored_type=1, compressed_size=0, timer=0):
    return struct.pack(
        ">IIHHHH12sI I 4x Q I I Q".replace(" ", ""),
        0x47423634,          # header 'GB64'
        3,                   # version
        0, 0, 0, 0,          # flags, bgpIndex, obp0Index, obp1Index
        DEFAULT_INPUT,
        0,                   # inputMapping.reserved2
        2,                   # graphics: ScreenScale1_5 in the low nibble
        timer,
        stored_type,
        compressed_size,
        0,                   # wallAtSave: no clock reading behind a converted save
    )


def convert(raw: bytes, total_size: int = FLASH_SIZE) -> bytes:
    if len(raw) > total_size - RAM_OFFSET:
        raise SystemExit(f"input is {len(raw)} bytes; too large for {total_size}")
    out = bytearray(b"\xFF" * total_size)
    settings = settings_block()
    assert len(settings) == 64, len(settings)
    out[0:len(settings)] = settings
    out[RAM_OFFSET:RAM_OFFSET + len(raw)] = raw
    return bytes(out)


def extract(sav: bytes, size: int) -> bytes:
    if sav[0:4] != b"GB64":
        raise SystemExit("no GB64 header; not a gb64 save")
    if struct.unpack(">I", sav[52:56])[0] != 0:
        raise SystemExit("compressed save; extraction not supported")
    return sav[RAM_OFFSET:RAM_OFFSET + size]


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--extract":
        data = extract(open(sys.argv[3], "rb").read(), int(sys.argv[4], 0))
        open(sys.argv[2], "wb").write(data)
        print(f"{sys.argv[2]}: {len(data)} bytes of cart RAM")
    elif len(sys.argv) in (3, 4):
        args = sys.argv[1:]
        size = FLASH_SIZE
        if args[0] == "--sram3x":
            size = SRAM3X_SIZE
            args = args[1:]
        raw = open(args[0], "rb").read()
        out = convert(raw, size)
        open(args[1], "wb").write(out)
        print(f"{args[1]}: {len(out)} bytes, RAM {len(raw)} bytes at 0x80")
    else:
        raise SystemExit(__doc__.splitlines()[-2].strip())
