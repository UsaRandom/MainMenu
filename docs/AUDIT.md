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

## 2aa. The engine was right and the addresses were not: one cheat entry for six Ocarinas

The first menu-driven launch came back "it booted, cheats did nothing", and every check the
console can make had passed: engine written, read back, checksum agreeing, `cheats FIT 4 line(s)
4 carried`, `engine rom+00103c ram 8000043c 16 words in 2 run(s)`, hook `3c1a8000 275a043c`
resolving to exactly that address. That is the shape of dead end this investigation keeps
producing, so the marker went back in -- three instructions storing to VI_X_SCALE at the head of
the engine, ahead of any cheat, depending on no game and no address being right.

**Both launches stretched the picture and neither changed a heart.** One cheat and four; single
segment and chained. So the engine executes, the chain executes, the stores execute -- and they
were writing to addresses that mean nothing in this binary. That also retires the last unproven
piece of 2z: control really does jump 0x8000043c → 0x8000357c and come back through the tail.

The database entry the menu loaded was `CZL?`, version 255, **553 groups** -- every Ocarina
revision and region merged into one:

| | database | the V1.2 file that gave 20 hearts |
|---|---|---|
| Max Heart Containers | `8111A5FE 0140` | `8111ACAE 0140` |
| Infinite Rupees | `8111B99C 0001` | `8111C04C 0001` |

0x8011A5FE is **V1.0**'s save context. The cartridge is `CZLE v2`. Worth recording separately:
2x's inline stub was aimed at 0x8011B99C, taken from this same entry -- the wrong revision's
Infinite Rupees. Even had it executed, nothing would have happened.

The cause is a granularity mismatch two tools apart. `rom_info.c`'s Ocarina row is a `MATCH_ID`,
which matches any region and any revision **on purpose** -- that is right for choosing a save type
and wrong for choosing cheat addresses. mkcheatkeys.py carried that straight through, so all three
USA revisions keyed to `CZL? / ANY`; mkcheatdb.py merges on `(check_code, game_code, version)` and
deduplicates by name, first wins; V1.0 sorted first. The filename said `(U) (V1.2)` the whole time
and the script parsed the region out of it while discarding the revision.

`revision_of()` now reads `(V1.x)` and `(Rev N)` from the corpus filename and lets it override the
database row's version. Lettered revisions are deliberately not decoded -- "Rev A" → 1 is a
convention, not a fact, and a wrong one here is silently wrong cheats. An unmarked filename stays
ANY rather than being guessed at 0, which is what keeps the wildcard row alive as a fallback.

**73 of 1039 keyed corpus files gained a concrete revision**, and Ocarina splits into six entries:

    CZL? v0    34 groups   Max Heart Containers 8111A5FE
    CZL? v1   245 groups                        8111A7BE
    CZL? v2   337 groups                        8111ACAE   <- the cartridge
    CZL? v255 325 groups   (Master Quest, GameCube editions, unmarked dumps)
    NZL? v0/v1/v255                             the PAL builds, split the same way

Simulating `find_row()` over the fifteen-ROM shelf: every game resolves the same as before or
better, and Ocarina moves from the merged wildcard to `CZL? v2`. All seven ticked names exist in
it with the V1.2 addresses, so `cheatstate.dat` -- keyed on the name hash, not the index -- carries
the user's selection across the rebuild.

**And the host suite caught a second bug, pre-existing, that the split made visible.** Five checks
went red on `NSMJ`: Super Mario 64 carries both a wildcard `NSM?` row and a Japan-specific `NSMJ`
one, both match a Japanese cartridge, and `find_row()` returned whichever sat earlier in the
index -- the merged all-regions row. That was true before this change and simply unobservable.
`find_row()` now ranks by specificity rather than taking the first hit: exact four-character code
beats three-character wildcard, exact version beats the ANY sentinel. 4,317 checks, 0 failures.

Method note, because it is the same one AUDIT keeps writing down: the ares loop validated the
*algorithm*, and the console runs an independent C implementation of it fed by a database ares
never touches. Two of the three things that broke after "it works in ares" were in the half ares
cannot see. The marker is the instrument that made both findable in one launch each, and it costs
three instructions.

## 2z. The menu does it now, and the gap rule turned on one instruction

Both pre-patched Ocarina images booted on the M64 and did what they were built to do -- the marker
stretched the title screen 2×, the cheat build gave 20 hearts, 9 bombs and 9 arrows off the user's
own save. So the ROM edit is confirmed on hardware and the remaining work was to move it from a
file a PC writes to three PI writes the menu makes at launch, which is the same three edits into
cartridge SDRAM that 2m already proved this console accepts and reads back.

**Capacity forced one design decision.** A single padding run holds about six cheats -- Ocarina's
is 108 bytes, 27 words, less four for guards and four for the tail -- and Ocarina's "Infinite Big
Key, Small Keys, Compass & Map" is nineteen lines on its own. So the engine is allowed to be
discontiguous, each segment ending with `j` into the next and its delay slot, two words a hop.

**And chaining is where the naive gap rule died.** A 70-word engine spread over ten runs
black-screened; 19 and 28-word engines over two runs boot and run. The ten-run version reached
into runs at 0x800067e0 and 0x80006804, and dumping their neighbourhoods says why:

    good, ram 80005004 (DK64)      27bd0028  03e00008  <- jr $ra, run swallowed the nop delay slot
    good, ram 80003574 (Ocarina)   03e00008  a02b902f  <- jr $ra, then the delay slot, then padding
    bad,  ram 800067e0 (Ocarina)   80006d30  00000008  <- a {pointer, length} record
    bad,  ram 80006804 (Ocarina)   00000000  00000001  <- more of the same table

"A run of zeros bounded by non-zeros" cannot tell padding from a zero-valued field. What can is
what precedes it: real padding follows a function's last instruction, so a `jr`/`j` sits one or two
words back. Over the fifteen-ROM shelf that rule keeps every run that has ever booted -- on
hardware or in ares -- and drops 37 of Ocarina's 39 candidates, including all four the failing
chain used.

**One instruction decides it.** 0x00000008 is a well-formed `jr $zero`; no compiler emits one and
it is an extremely common data value, so accepting it makes `{pointer, 0x00000008}` look exactly
like the end of a function. The first version of the rule accepted it and kept the bad runs. That
is now `rompatch_run_is_padding()`, split into rompatch_find.c so the host suite can reach it, and
pinned by nine checks built from the words measured either side of the real runs above. Deleting
the `jr $zero` exclusion turns exactly one of them red, which is the whole point of writing it
down as a table of real addresses rather than as a plausible predicate.

Three complications disappeared with the boot-time engine:

- **No DMEM patch, so no `nop486`.** That coupling -- the header computed for a bootcode edit made
  somewhere else -- broke in both directions across three builds (2t, 2v). The header this writes
  describes the cartridge exactly. `cheats_disable_x105_check()`, `boot_params_t::rom_patched` and
  the `crc_nop486` pairing are all gone.
- **No fallback to the Datel path, and that is a fix rather than a simplification.** Handing
  `cheat_list` to `boot()` when rompatch wrote nothing would have `cheats_install()` nop DMEM word
  486 against a header that still describes the pristine image -- byte 0x798 is inside the window
  6105 mixes in -- so "your selection contains a conditional" would have become a black screen on
  every 6105 game. `bp->cheat_list` is now always NULL: cheats apply through the cartridge or not
  at all, and the log says which.
- **Nothing has to survive the handoff**, so `skip_rdram_reset` is irrelevant to cheats.

The limits are real and reported rather than worked around. Unconditional 8/16-bit writes only
(0x80/0x81/0xA0/0xA1, GS-button bit clear) because the engine emits no branches; a selection
containing anything else is refused **whole**, never filtered, because a `D0` and the write it
guards are one indivisible thing (2.2). At most four segments and 128 words. Per-game capacity
from the shelf sweep: Ocarina 8 cheats, Banjo-Kazooie 5, Tony Hawk 9, GoldenEye 3, Shadows of the
Empire 2, 1080 7, Donkey Kong 64 42, Mario Party 269.

Still open: the gap is *chosen* by a heuristic and only *proved* by booting the game. Ocarina and
Donkey Kong 64 are proved. The rest are argued from a rule that has never yet kept a bad run --
which is not the same claim.

## 2y. The engine runs. It arrives in the ROM file, and ares can see it in one second

The inline stub of 2x went black on hardware, and its safety argument was wrong in a way worth
naming: it did **not** differ from the canary "only in the values". The canary reaches
`__osException` with `jr $k0` and `$k0` holding `__osException`'s own address; the stub reaches it
with `j`, having spent `$k0` as the store's base register. Any dependence on that register on
entry breaks. The prior that made it worth one shot was based on a difference that was not the
only difference.

That is eight rounds, and the loop itself was the problem: every one of them cost a build, a card
handover and a photograph, and four of the eight were void on self-inflicted probe bugs. So the
question changed from *where does the engine live* to *why does it arrive at run time at all*.
It does not have to. IPL3 copies ROM `[0x1000, 0x101000)` into RDRAM before the game's first
instruction; an engine inside that megabyte is installed by the console, from the cartridge,
through the same DMA that loads the game. No survival, no cart write, no IPL3 patch, no timing --
and the artefact is a **file**, which a PC builds and an emulator boots.

`tools/rompatch.py` makes three edits, all inside the checksum window: the engine into a run of
zero padding, the preamble's first two words repointed at it, and CRC1/CRC2 recomputed. The
engine's tail replays the preamble's own two words and `jr $k0`, so `__osException` is entered
with exactly the register state it had before -- the thing 2x got wrong. `tools/aresshot.sh` boots
a ROM and screenshots it, which turns the round trip from a day into about thirty seconds.

**The gap rule is reversed, and that is what 2w actually died of.** `rompatch.c` took the *last*
long-enough zero run, on the reasoning that padding collects at the tail. It does not: on Ocarina
that picks ROM 0x04125c, 9,204 bytes of zeros inside compressed asset data at RDRAM 0x8004065c --
which both corrupts an asset and lands in RAM the game reuses within a second. The rule here takes
the *lowest* run that fits, caps it at 1,024 bytes so a data void cannot qualify, and leaves two
zero words as guard either side. On Ocarina that is ROM 0x004174: 108 bytes between a function
ending `03e00008 jr $ra` and one beginning `40846000 mtc0`, at RDRAM 0x80003574, inside resident
boot code and below the 0x4a20 bytes at 0x80006d60 that the entry stub clears.

Three ares runs, all on Ocarina (U) 1.2, CIC 6105, entry 0x80000400, preamble at ROM 0x0031f0 /
RDRAM 0x800025f0 → 0x80002600:

| image | engine | result |
|---|---|---|
| unpatched | — | intro cutscene, **61 VPS** |
| chain only, 4 words at 0x8000043c | tail alone: a copy of the preamble | title screen, **60 VPS**, indistinguishable |
| marker, 7 words at 0x8000043c | `sw` of 0x0100 to VI_X_SCALE, then the tail | title screen, **59 VPS**, picture stretched exactly 2× |

The middle row is the control: the vector now routes every exception through our code and the game
does not notice. The bottom row is the result this investigation has been trying to get for eight
rounds -- **the handler executes**, sixty-plus times a second, and the game keeps running at full
speed while it does. A store to VI_X_SCALE was chosen over a game cheat deliberately: it needs no
save, no input and no knowledge of the title, and it cannot be confused with a crash, because a
crash is black and this is the same title screen at the wrong scale.

Two complications simply disappear. There is no DMEM patch, so 6105's runtime code check has
nothing to defeat and the `nop486` coupling that broke in both directions across three builds
(2t, 2v) is gone -- the header genuinely describes the image. And `skip_rdram_reset` no longer
matters, because nothing has to survive the handoff.

Shelf sweep, 15 ROMs: **10 patch under the strict rule**, gap sizes 44 to 112 bytes. Of the five
refused, three carry a preamble the strict rule rejects, and two of those -- 1080 Snowboarding and
Harvest Moon 64 -- put `__osException` at **+212 rather than +16, the same number in two unrelated
games**, which is a different libultra build rather than a coincidence. `--accept-odd` takes any
forward target inside 4 KB; it still refuses GoldenEye's 0x700101A0, which is not a KSEG0 address
and therefore not an address. Shadows of the Empire has nothing preamble-shaped in the megabyte at
all, and Pokemon Stadium is refused by the pre-existing gate, its header having disagreed with its
own contents before anyone touched it. **12 of 15 patchable, and the three failures are loud.**

The heuristic is not trusted, it is booted. Five marker builds run in ares, and the stretch is
visible in every one:

| game | cic | engine | result |
|---|---|---|---|
| Ocarina of Time | 6105 | 0x8000043c | title screen, 59 VPS, 2× stretch |
| Donkey Kong 64 | 6105 | 0x8000500c | DK Rap intro, 59 VPS, text clipped off the right edge |
| Banjo-Kazooie | 6102 | 0x80002ff8 | attract, 61 VPS, stretch confirmed against an unpatched baseline |
| 1080 Snowboarding | 6103 | 0x80003170 | title, 60 VPS, `SNOWBOARDING` clipped -- `--accept-odd` |
| Harvest Moon 64 | 6102 | 0x800ff454 | attract, 60 VPS, 2× stretch -- `--accept-odd` |

Five games, four CIC variants, two preamble layouts, gaps from 44 to 108 bytes and RDRAM
addresses from 0x80000434 to 0x800ff454. Banjo is the one that needed a control: at 2× a
waterfall is not obviously wrong until an unpatched shot of the same frame is beside it.

Open, and the reason the hardware round is a file and not a menu change: ares is not an M64. Two
pre-patched Ocarina images are on the card -- a marker build and one carrying max hearts, infinite
energy, bombs and arrows (16 words at RDRAM 0x8000357c) -- plus the DK64 marker, because launching
a file tests the ROM edit with no menu code in the path at all. What that cannot test is the
menu-side version of the same three writes, which is where this has to end up: a 32 MB file copy
per launch is not a shipping design, and cartridge SDRAM already accepts these writes and reads
them back (2m).

## 2x. Stop placing an engine: the cheat fits in the four words the game already copies

Two measurements ended the placement hunt. First, running find_zero_run()'s logic over all 15
shelf ROMs: **14 of them have no zero run of 64+ words anywhere within ±32 KB of their preamble**
(only Donkey Kong 64 does). There is no in-image home for an engine, in general or for Ocarina.
Second, re-reading the pre-August behaviour against the code: launches booted with cheats inert,
which means the patcher's own preamble scan -- which WOULD have hooked correctly -- never ran, so
the IPL3 DMEM patch does not take on this console. Combine that with menu-context RDRAM writes
not surviving the handoff, and every engine-based design has both of its placement mechanisms
broken at once. That single sentence explains all seven rounds without needing the placements to
have been individually wrong.

So the design changes shape: **no engine, anywhere.** libultra copies four words from
`__osExceptionPreamble` onto the exception vectors. A GameShark write cheat needs three
instructions, and the jump home needs one -- which fits exactly, because MIPS executes the jump's
delay slot, so the store goes there:

    lui  $k0, 0x8012          # A_BASE(0x8011b99c), the +1 sign-extension carry
    ori  $k1, $zero, 0x0001   # the value
    j    0x80002600           # the game's own __osException
    sh   $k1, 0xb99c($k0)     # delay slot -- runs before the jump lands

Every exception the game takes, sixty-plus times a second, writes the cheat and hands control to
the real handler. No RDRAM survival, no IPL3 patch, no patcher, no cart execution, no relocation:
the only address involved is the preamble, which is already proven writable and read-back-verified
on this console. $k0/$k1 only, the same registers the preamble itself clobbers, and libultra's
__osException overwrites $k0 in its own first instruction.

The safety argument is the canary's: the canary rewrites the same four words at the same offset
with the same recomputed checksum and boots every time. The stub differs only in the VALUES, which
are four valid instructions ending at the address the canary jumps to. If the canary boots, this
boots -- and if it boots, the cheat has already happened. That is a much stronger prior than any
placement round had.

Verified before shipping, since the encodings could not be checked by the host suite (the
assembler macros are compound literals, unusable in static initialisers and endianness-dependent):
the four macros were compiled by the real MIPS toolchain and disassembled, giving
`3c1a8012 341b0001 08000980 a75bb99c` = exactly the listing above, with the store's effective
address computing to 0x8011b99c. Both the encoding and the sign-extension carry are therefore
measured, not reasoned about. The words are also written to launch.log every launch, so a black
screen still returns them for hand-checking.

The hard limit, stated plainly: **one unconditional 8- or 16-bit write per game.** Conditionals,
repeaters and multi-line groups do not fit in four words, and a selection containing any of them
is refused whole rather than half-applied (AUDIT 2.2's lesson). Infinite Rupees is one line, which
is why it is the test. If this works, the shipping question becomes which cheats qualify, and the
answer for most "infinite X" codes is that they do.

## 2w. The in-image gap was inside compressed game data, and the cheat patch path ships disabled

07990B -- the 6105 check disabled independently of the cheat list -- still black with a cheat
ticked. The 6105 fix is a real fix for a real bug and remains UNPROVEN, because the vehicle
carrying it was independently broken, which a measurement off the ROM file settles without
hardware.

Running find_zero_run()'s exact logic over Ocarina on the host: the last qualifying run starts at
ROM 0x04125c, so the trampoline went to ROM 0x04129c, RAM 0x8004069c. The bytes bracketing that
run are `ff71b102 1308fe81 f303f2fa ffff0000` before and `d320d532 d75ef7a0 0031a100` after --
high-entropy compressed content, not padding. So the write corrupted Ocarina's own data, and the
RAM address sits far past the resident boot segment in memory the game reuses within its first
frames. "Padding collects at the tail" was asserted, not measured, and is false for this ROM.
Both failure modes are consequences of that one wrong assumption; the probe tested neither
placement nor the 6105 fix. (The first qualifying run, 0x026bf8 -> RAM 0x80025ff8, is a candidate
a future attempt should prefer, though it too is probably beyond OoT's resident boot segment.)

**Decision: ROMPATCH_ENABLED 0, and the menu ships that way.** Seven hardware rounds produced no
launch where a ticked cheat both booted the game and ran the engine, and the failure mode is the
worst available -- ticking a cheat black-screens the console. A menu whose cheats do nothing is
strictly better than a menu that bricks a launch. With the patch off, the engine falls back to the
runtime preamble scan, which is upstream's behaviour and every pre-August launch's behaviour:
games always boot, and on this console cheats are inert because the scan does not find its hook.
L and R stop reserving themselves for the diagnostic page.

Kept, not deleted: rompatch, romcrc, the diagnostic page, the beacon and the whole apparatus, all
of it behind one #define, with 4,048 host checks and a photographed all-green hardware run
standing behind the parts that ARE proven -- the cartridge is writable and verifiable in menu
context, the preamble finder agrees with the Python across 24 ROMs, and the checksum arithmetic
reproduces 23 of 24 real headers. What was never established is any engine placement that
executes. Two candidates remain honestly untested: 807C5C00 with the nop written (E371D1 showed
no beacon bar there, unexplained) and a gap inside the resident boot segment.

Method note for whoever resumes this. Of the seven rounds, at least four were void for reasons
introduced by the probe rather than found in the hardware: a control that could not fail (2s), a
header computed for a nop the launch would not write (2t), the nop removed along with the cheat
list (2v), and a gap chosen inside compressed data (2w). The house rule "check that a test can
fail" was applied to the harness and not to hardware probes, and that is where it was needed
most. A hardware probe should state, before it runs, what each outcome will mean AND what it
assumes about the world -- the second half is what kept being skipped.

## 2v. It was never the placement: 6105's runtime code check, armed against every probe

30C415 -- the in-image trampoline, placed by the game's own IPL3 load, in the game's own RDRAM,
inside the mapped region, running cached -- black. That result has no survival, mapping or PI
explanation left, which is what makes it the useful one: if a trampoline in the game's own loaded
image still black-screens, the target address was never the variable, and five rounds of
placement work were measuring something that was not moving.

The actual mechanism was in the tree the whole time, at cheats.c:374, in a comment: CIC 6105's
IPL3 performs a **runtime game-code verification** beyond CRC1/CRC2, and every cheat engine since
Datel disables it by writing a nop over DMEM word 486. That write lives inside
cheats_patch_ipl3(), reached only from cheats_install(), which returns at its first line when
there is no cheat list. Ocarina is 6105. So:

| run | boot segment edited | nop written | result |
|---|---|---|---|
| canary (all builds) | yes | YES -- cheat_list non-NULL, full install path | boots |
| 0A51D5 rom-hook | yes | no -- 2n's early return skipped patch_ipl3 | black |
| D3E6D8, 5FEE6B | yes | no -- same | black |
| E371D1 patcher | yes | yes | black (its own RDRAM placement fault) |
| 8ACA4F cart tramp | yes | no -- NULL cheat list | black |
| 30C415 in-image | yes | no -- NULL cheat list | black |
| pre-Aug-4 launches | no | yes | boots, cheats silently inert |

Every black screen in rom-hook mode is explained, and so is the canary: it booted not because
its bytes were harmless but because its launch carried a cheat list and therefore wrote the nop.
The control that could not fail (2s) also could not isolate -- it differed from the probes in a
second variable nobody was tracking. The isolation probes each removed the cheat list to remove
the RDRAM engine, and removed the nop along with it, arming a check against a modified image.

The fix separates the two: cheats_disable_x105_check() is lifted out of the install path, and
boot() calls it whenever boot_params.rom_patched is set, independently of the cheat list. nop486
returns to true and its meaning is corrected -- it tracks "will the nop be written", which is no
longer the same question as "will cheats_install() run". The page's MISMATCH line now compares
against launch_patch.written rather than the mode.

What this does NOT establish: whether any of the four placements works. All four tests were void.
The next run re-tests in-image with the check disabled, and if it boots, the placements that were
convicted in 2n-2u deserve re-examination before any of them is treated as ruled out -- 807C5C00
with a live engine may have been fine all along.

## 2u. Cart execution fails too, and the pattern names the one placement left

8ACA4F re-ran the cart-SDRAM trampoline with the header and the bootcode finally agreeing
(nop486 false, no IPL3 patch, no nop) and came back black. That is the clean read §2t said was
still owed, and it is a no: with the four words verified present at 32 MB and the checksum
consistent, a preamble aimed at 0xB2000000 does not produce a running game. Most likely the SC64
stops mapping addresses past the loaded ROM once the game is running -- the read-back happened
in menu context with write-enable set, which is a different cart state -- so the jump lands in
unmapped space. Not proven; not worth a run to prove, because the conclusion either way is that
the engine cannot live past the ROM.

Four placements, one pattern: **every redirect to an address the game does not already own goes
black, and the canary's redirect to the game's own handler boots.** RDRAM at 807C5C00 twice
(menu copy, patcher), cart SDRAM once, against the game's own __osException. Since the
trampoline hands __osException identical register state to the untouched preamble -- $k0 =
0x80002600, same as it would have been -- a black screen means the trampoline did not execute,
never that it executed wrongly. So all four results are about WHERE the code sits, and three
distinct wheres have failed for three plausibly distinct reasons: RDRAM not preserved across the
handoff, RDRAM not preserved across the handoff again, and cart space not mapped or not
fetchable at runtime.

30C415 tries the last placement that dodges all three at once: inside the game's own boot
segment. ROM [0x1000,0x101000) is what IPL3 copies to RDRAM at the entry address, so four words
written into a run of zero padding there arrive in RDRAM by the same DMA that loads the game --
placed by the game's own loader, inside the mapped region, executing cached with no PI fetch and
nothing asked of reboot.S. rompatch_install_inimage() scans for the run (last long-enough one
wins, since padding collects at the tail; 16 guard words of zero required either side), writes
the trampoline, points the preamble at its RAM address, recomputes the header over both, and
reads back. The page reports the offset, the RAM address and the gap size, so a photograph shows
where it went. Residual risk, stated before the run: a zero run in the image may be BSS the game
later writes over, which would show up as a game that boots and then dies rather than one that
never starts. Boots = placement solved and the real engine follows into that gap; black = every
placement available to this design has failed and the honest move is the shipping decision in
§2v rather than a fifth address.

## 2t. The trampoline was in the cartridge all along; the header was computed for a nop nobody wrote

The R run on 0F86F4 photographed `crc ok 1  found 1  written 1  read back 1  agrees 1`, so
hypothesis (a) of §2s is dead: the four trampoline words ARE in cart SDRAM at 32 MB, read back
word-for-word, and the patched image agrees with its recomputed header. But the page also shows
why the boot died, and it is not cart execution.

ROM-trampoline mode hands boot() a NULL cheat list so cheats_install() returns at its first line
-- no IPL3 patch, so no I_NOP into DMEM word 486 -- while the header was computed with
nop486=true, i.e. for a bootcode window containing that nop. Ocarina is CIC 6105, the only CIC
that mixes 256 bytes of its own bootcode (offset 0x750) into the checksum, and byte 0x798 is
inside it. Console computes one number, header carries another, IPL3 refuses the image. Black,
for a checksum reason, with the execution question never reached. The invariant broken here is
the one written into this file at 2n and into the code at 2q -- "the two switches move together"
-- now broken in both directions within four builds.

`agrees 1` cannot catch it, and that is the more useful finding: reverification checks our
arithmetic against our own nop assumption, so a header computed for a nop the launch will never
write reads as agreeing right up to the moment the console disagrees. The instrument was
self-consistent and wrong, the same shape as the canary in §2s. The fix is nop486=false for
trampoline mode plus a page line printing header-assumption against will-actually-write with an
explicit MISMATCH flag for 6105, so the next occurrence is visible in a photograph rather than
inferred four builds later.

The cart-execution question is therefore still OPEN, not answered -- 0F86F4 tested nothing about
it. The re-run is the same probe with the checksum consistent.

## 2s. The ROM trampoline is black too, and the canary turns out not to be a control

0F86F4 -- preamble aimed at a four-word trampoline in cart SDRAM at 0xB2000000 -- black. Three
placements now fail (menu-context RDRAM, patcher-context RDRAM, cart SDRAM) while the canary
boots, and the pattern across all four is sharper than any one of them: the ONLY variable is the
address the preamble computes into $k0. 0x80002600 (the game's own __osException) boots;
0x807C5C00 and 0xB2000000 both black. Same two-instruction shape, same CRC path, same everything
else.

**The canary cannot fail, and that invalidates how it has been read.** It rewrites the preamble
to compute the identical target, so a canary launch boots if the patch survives into the game AND
boots if the patch is reverted, ignored, or re-streamed away -- one outcome for both worlds. It
proves the menu-side write and the checksum arithmetic (both real, both read back), and it proves
IPL3 accepts a modified boot segment with a recomputed header. It proves nothing about whether
the patched preamble is what the running game executes. Exactly the test-that-cannot-go-red this
project's own house rule warns about, live for eight hardware rounds.

Two live hypotheses, and one instrument already on the card can split them. (a) The trampoline
write at 32 MB never stuck -- install() writes the preamble hook BEFORE the read-back and does
not roll it back on failure, so a failed verify leaves a cart whose preamble points at absent
code, which is black. (b) It stuck, and cart-space instruction fetch during exception handling
is the fault -- a genuine hazard, since an exception taken while the game has a PI DMA in flight
would fetch the handler over the same bus. R on the detail sheet runs FULL (the trampoline) with
the diagnostic page and does not boot, so `read back` on that page reads out (a) directly: 0
means the write did not stick, 1 means it did and the fault is execution. No rebuild required.

If (a): the fix is placement inside the loaded image -- write the engine into a run of zero words
in the ROM's boot segment, where IPL3's own DMA puts it in RDRAM as part of the image, which
sidesteps both survival (IPL3 places it) and cart execution (it runs cached from RDRAM).

## 2r. Two RDRAM placements dead, so the engine moves into the cartridge

E371D1 -- patcher placement, game context, the mechanism upstream ships on real SC64 hardware --
came back pure black, no bar. That is the second placement mechanism to fail (menu-context copy
was 5FEE6B). Both write the engine to 807C5C00 in RDRAM; neither executes at the game's first
exception. On a real N64 807C5C00 is where the GameShark has lived for twenty years, so this is
not the address -- it is the M64 not preserving RDRAM across the boot handoff, for menu-context
AND game-context writes alike. The RDRAM-engine approach is dead on this console. (A caveat kept
honest: the patcher only runs if the IPL3-patch link fires, itself never independently confirmed
on the M64, so "patcher placement failed" may fold in "patcher never ran" -- but the conclusion
is the same, RDRAM is not the place to put the engine here.)

What DOES survive the handoff is the one thing the hook already writes and reads back: cart
SDRAM. rompatch writes it with CFG_ID_ROM_WRITE_ENABLE set; the handoff only clears the
write-enable, leaving the contents readable over the PI for the whole life of the game. So the
engine moves there. 2r ships the minimal proof: rompatch_install_rom_trampoline() writes the
preamble's own four words (word0, word1, jr $k0, nop) into cart SDRAM 32 MB in -- past every
reference game, inside the SC64's 64 MB, outside the checksum window -- and aims the preamble at
0xB2000000 (KSEG1). The CPU executes those four words from cart over the PI and jumps to the real
__osException, behaviour identical to no hook. Boots = cart-SDRAM execution and preamble routing
both work, and the real engine follows into ROM the same way; black = routing is the fault, not
survival, which would be a genuinely new finding. The four-word copy is read back before boot, so
a cart too small or a write that did not stick is reported rather than silently black. FULL mode
is this probe; cheats do nothing this build (cheat_list is dropped, no RDRAM engine, no IPL3
patch). Pre-registered before the run.

## 2q. No bar, no boot: placement confirmed as the fault, and the patcher comes back

5FEE6B -- beacon + tail, watch block and cheat stores gated out -- came back pure black, no bar.
That is decisive in a way the silent trampoline was not: a beacon+tail engine that executed
would either paint (VI set up, the normal case for an OoT interrupt) or, if it somehow skipped
the paint, still replay the preamble and hand off, which boots. It did neither. The engine at
807C5C00 is not executing on the first exception. With the canary proving the cart preamble
rewrite and read-back proving the hook names 807C5C00, entry is not the suspect -- placement is.
The C-side engine copy, written in menu context inside boot() before reboot.S, does not survive
the handoff into the game on the M64. Risk 2 (skip_rdram_reset preserving RDRAM on the clone,
unverified) is now a confirmed negative for menu-context writes.

The fix reverses 2n: the IPL3 patch and its patcher return in rom-hook mode, so the engine is
placed DURING the game's IPL3 -- game context, after the reset decision, the mechanism the Datel
engine has always used and the one upstream ships on real SC64 hardware. The cart hook is kept
for entry (proven), the patcher does placement (survives). The 6105 nop is coupled back on with
it (nop486 true, header recomputed to match). The build stays beacon+tail so the same eye
answers: a bar now means patcher-placement fixed it (next build restores the cheat store and
pins rupees); still black means nothing menu-written survives even via the patcher, and the
engine must move into cart ROM, which is survival-independent. Pre-registered before the run.

## 2p. The silent trampoline was untestable, and the survival hypothesis it points at

D3E6D8 -- the four-word tail-only trampoline -- came back black, and that result carries less
than it looks: a silent trampoline emits no output, so "never executed" and "executed and the
hand-off crashed" produce the identical black screen. The build could not distinguish its own
two outcomes. Recorded as a method error in the same family as the ares-only beacon and the
gated launch.log: a probe whose two branches look alike proves nothing.

Reading boot.c and reboot.S against the rom-hook change surfaced the mechanism the trampoline
was meant to test. The old patcher placed the engine DURING the game's IPL3 (reboot.S runs the
game IPL3 at 0xA4000040; the DMEM J hook diverted it into the patcher, which wrote the engine to
RDRAM in game context, after any reset). Rom-hook mode deleted the patcher and moved placement
into cheats_emit(), which runs in MENU context inside boot(), before reboot.S -- so the engine
at 807C5C00 now survives into the game only if reboot.S's skip_rdram_reset (a0=cheats_installed,
which skips the RI_REFRESH/RI_SELECT writes) genuinely preserves RDRAM contents on the M64. That
is Risk 2 in the plan, explicitly unverified on this clone. A byte-perfect trampoline going
black is exactly the signature of "807C5C00 holds garbage at first exception". The competing
explanation is the cart preamble hook not routing there at all -- but the canary boots, proving
the rewrite mechanism, and read-back proved the hook words name 807C5C00.

The next build (beacon + tail, watch block and cheat stores gated out; see ENGINE_TRAMPOLINE_ONLY
in cheats.c) makes the probe testable by giving it an eye: a bar means the engine executed, its
absence means it did not. If no bar, the fix is to place the engine in game context again --
re-run the patcher for placement while keeping the proven cart hook for entry -- or to move the
engine into cart ROM, which sidesteps RDRAM survival entirely. If a bar appears but the game then
dies, placement is fine and the displaced words or the hand-off are wrong.

## 2o. Still black with a pristine boot chain, so the engine itself goes under the knife

0A51D5 -- no IPL3 patch, no patcher, plain-flavour header -- and the report is the cleanest
split yet: "game does not boot with cheats, does without". §2n's third prediction fires. What
remains in a cheated boot that a cheatless one lacks: the two hook words in the cartridge
image (exercised by the canary, which boots), the recomputed header (plain flavour, the
23-of-24-validated one), and the engine at 807C5C00 -- its code, or its survival there through
reboot.S's skip_rdram_reset on a clone console whose memory init nobody has audited.

Stage 1 of the binary search ships as ENGINE_TRAMPOLINE_ONLY in cheats.c: the engine is
exactly its tail -- displaced word replay, jr, nop -- so the first exception enters 807C5C00,
executes four instructions, and lands in the real __osException. Boots = placement, RDRAM
preservation, hook entry and handoff all proven in one run, and the guilt moves into the
engine body, where the prime suspect is the watch-relocation prologue: it leads with a BNEL,
a branch-likely -- the classic soft spot of FPGA MIPS clones -- and an MTC0 to a watch
register the M64 demonstrably does not implement (§1af: control=1, trapped=0). Still black =
placement is the lie, and the next look is at what the M64's IPL3 actually does to RDRAM when
told to skip the reset. Stage 2, pre-registered: tail plus cheat stores, no watch block, no
beacon -- prediction: boots and pins rupees at 1, after which the watch block is removed from
rom-hook mode permanently rather than debugged, same policy as §2n.

## 2n. The first true engagement goes black, so the unproven link is amputated

Build 7F6AF3, the first with the gate open: Ocarina, cheat ticked, Start -- fade, then black,
held twenty seconds. Persistence still works. And the record reframes itself: the Aug 4 "black
screen" carried the same full cheat path (blamed on the flash hold at the time), and every run
where games visibly ran with cheats ticked was the era when the runtime scan silently missed
and the engine never hooked. Taken together: the full cheat path has plausibly NEVER completed
on this console, and the rupees never changed because the engine never ran, not because it ran
wrong.

What distinguishes a cheated boot from the cheatless boots that work: the ROM hook (verified by
read-back, exercised by the canary), the engine at 807C5C00 (k0/k1-contract emitted code,
placed from C, RDRAM preserved), and the boot-time machinery -- cheats_patch_ipl3() writing a J
into the CIC's bootcode in DMEM, jumping into the emitted patcher. That last link is the one
thing no instrument has ever confirmed on the M64, and in rom-hook mode it is also pure legacy:
the hook is already in the cartridge and the engine is already placed. So it is removed rather
than debugged -- cheats_install() now skips the IPL3 patch entirely in rom-hook mode, the
beacon's green stamp moves to C, and the header checksum flips to the pristine-IPL3 flavour
(ipl3_nop_486 false end to end), because without the DMEM nop a 6105 console verifies the
hooked image against unmodified bootcode.

Accepted costs, recorded: 0xF0/0xF1/0x20/0xEE patcher-time codes are silently skipped in
rom-hook mode; and a cheat list too large for the C-side copy is refused at emission, where
boot.c's false path then boots a hooked cartridge into an empty engine address -- an edge
needing ~seventy maxed 0x50 repeaters ticked at once, pre-existing in kind, kept on the books
rather than hidden. Predictions for the next run, written first: game boots and the rupee
counter pins at 1 (the ticked code is 8111B99C 0001 -- pinning, not 999) = the engine runs and
the goal is met; game boots and rupees count normally = hook present but never entered,
next probe is the vector copy; black again = the engine at first exception is guilty after
all, next probe is a trampoline-only tail.

## 2m. All corners green: every stage proven on hardware, and the gate goes back to opt-in

Build 2E690A, Ocarina, Infinite Rupees ticked, Start. The user reports "all colors, all
corners green" and the photographed page reads: `beacon 1  writable yes  log open`,
`re-probe ok`, `groups 553  ticked 1  emitted 2 words`, `crc ok 1  found 1  written 1  read
back 1  agrees 1`, `site rom+0031f0 ram 800025f0 -> 80002600`, `displaced 3c1a8000 275a2600
cands 1 rej 0`, `crc 693ba2ae b7f14e9f -> 693b8256 cf49cc7d`, `engine 807c5c00` -- the same
site, displaced words and checksum transform as the Aug 4 log, now reproduced with dumps
deleted and BLUE painting without a reset. The install is exonerated ON hardware, not just by
the archival argument. And on the following boot: "tick and recent survived" -- the
capture-time saves hold, closing the persistence finding of §2l with a hardware confirmation.

Also closed by the returning card: console stdio writes persist across power-off (a 2,727-byte
launch.log with three complete cheatless reports came back on the very card once believed
write-hostile), so no firmware write-cache exists and the SummerCart64-firmware excursion is
cancelled; `beacon 0` was a stale backup -- config.ini.orig predates the setting, so every
restored card lacked the key and settings.c's reader was innocent (key appended on-card, page
now reads beacon 1); and the console's RTC runs ~3 days slow, which is what manufactured the
"frozen since Aug 4 14:37" timeline read off mtimes in §2g -- console-written dates are not
wall-clock and never were. The `sc64menu.n64.main`/`.prev` pair never reappeared on any card
the console used since; parked as an old-card curiosity, no longer blocking anything.

The film is removed and `diagnose` reads the L/R latch again -- Start boots, L (canary) and R
(real hook) stop on the page -- because the unconditional gate's question is answered and the
log persists with the full report either way. The gesture-era anomalies ("L just launches
game") are retroactively explained as the latch working and rompatch_dump() resetting the
console two calls later. Remaining unproven, and now the ONLY remaining question: whether the
engine executes inside the running game -- the original rupee test, unblocked at last.

## 2l. The page appears, the dump is convicted, and the persistence mystery was a code path

The diagnostic page rendered on hardware for the first time -- photographed, plate 157AAA on
the page itself -- and one session answered three investigations at once.

**The reset is rompatch_dump(), specifically, and not SD writes in general.** Same card, same
boot, minutes apart: 1080 Snowboarding with no cheats ran the whole film (WHITE, YELLOW red
corner, CYAN red corner, ORANGE, MAGENTA) to the page, whose own lines read `writable yes`,
`log open`, `re-probe ok` -- small console-side writes succeeding end to end. Ocarina with a
cheat ticked, entering the one branch that dumps, cut at CYAN and "booted": a warm reset with
the loaded ROM mapped (film two's shifted-pixels signature). A megabyte of PI reads interleaved
with FatFs writes is what kills the M64; the io_write install is exonerated by the Aug 4 log,
which recorded WRITTEN AND READ BACK before dumps existed, without a reset. The dumps are
deleted. Prediction from §2k ("the cut lands at the io_write") is REFUTED -- the cut preceded
the install both times.

**"No persistence, four cards tested" was never storage.** Every state writer except settings
lived in app_deinit(), and app_deinit() only runs on a clean boot -- but every diagnostic-era
launch ends on the fault page (user powers off) or in the dump reset (never returns). The saves
were not failing; they were never attempted. The tell was the asymmetry sitting in the open:
config.ini kept persisting (settings_save fires on leaving the settings screen, in-session)
while playstate/cheatstate never moved. Fixed by saving at capture time -- do_load() and the
detail sheet's back-out -- with deinit's saves kept as a second chance. The user's "maybe we
arent flushing" was the right neighbourhood: not an unflushed write, an unreached one.

**Open, with discriminators queued:** (1) whether stdio writes actually reach the medium --
log_launch() ran and closed the log before the 1080 page, so `launch.log` with tonight's banner
is either on that card or a firmware-side write cache exists and the SummerCart64 firmware
sources get read next; the deploy script now prints the watched files on every mount. (2) Which
card ran: the user reports the ORIGINAL card, which never received 157AAA -- if the card that
comes back is the 31 GB one, the M64's boot chain runs a cached menu copy when the SD copy is
unreadable, and the `.main`/`.prev` pair finally has a suspect. (3) The page read `beacon 0`
against a config.ini that says `cheat_beacon = true`, and settings.c:98 reads the key with the
same ini_get_bool every working setting uses -- unexplained, parked, and harmless for now
because the launch path arms the beacon unconditionally.

## 2k. Film one names the window: the console leaves the menu while do_load() touches the cart

The first film ran on a plate-confirmed build and the user reported: WHITE, YELLOW, CYAN, then
the game. No BLUE, no ORANGE, no MAGENTA, no RED. The divergence is therefore strictly inside
the dump/install window -- the only stretch of do_load() between those checkpoints, and the only
code in the menu that writes at the cartridge: rompatch_dump() (FatFs → SD write commands) and
rompatch_install_*() (io_write into PI ROM space). Had the window merely failed politely, the
film would have continued to BLUE with a red corner and stopped on the fault page; it did not
continue. Nothing in that window can boot a game in software -- the two `app->running = false`
sites are elsewhere and were re-verified by grep -- so the working theory is that a cartridge
write makes the M64's SC64 variant reset the console with the loaded ROM mapped, which from the
couch is indistinguishable from a boot.

One theory retires several ghosts at once, checked against the record: every "boots straight to
game" run happened on a build whose ticked-cheat path enters this window; cheatless launches
never enter it and boot normally; 69DC83's "L just launches game" was the latch working and the
window firing; and the write-deadness across three cards -- surviving power cycles and media
swaps -- fits persistent state on the CART, not the cards. The boundary remains Aug 4 ~15:00:
the last successful menu-side SD write (launch.log 14:37, sc64menu.rtc 15:17) immediately
precedes the DEV_HARNESS builds accidentally running scripted hardware sessions (§1at era).
Whether one of those runs changed a persisted SC64 config, or a firmware update happened around
then, is not established -- it is the top open question, and worth asking the cart owner's
tooling directly once USB is available anywhere.

Film two splits the window: GREEN after dump-before (corner: it wrote), BLUE after the install
(corner: read back), GRAY after dump-after, ORANGE/MAGENTA renumbered behind them. The last
colour seen now names the exact operation that ends the menu. Predictions, written before the
run: if the SD path fails instantly at fopen() (a write-protected mount would), GREEN arrives
in under a second with a red corner and the cut lands between GREEN and BLUE, at the io_write;
if instead the cut precedes GREEN, the SD write command itself is the trigger.

## 2j. A plate-verified impossible result, and the launch becomes a film strip

The §2i experiment ran clean and returned the strongest negative result of the investigation:
boot plate read 0A0043 (confirmed aloud), fresh single-partition FAT32 on a healthy 64 GB card,
every byte verified after writing, Infinite Rupees ticked, Start pressed on the detail sheet --
and Ocarina booted with no diagnostic page, and neither the tick nor the play survived a
restart. Media is now exonerated twice over, and the format confounds (FAT16, partition-type
mismatch, leftover multi-partition layout -- the "new" card turned out to be one small FAT16
volume on a repurposed 64 GB Pi card) are all gone.

The result contradicts the source. In build 0A0043, `diagnose` is a literal true; the only two
`app->running = false` sites in the tree are do_load()'s emulated-system branch (which needs a
core and a non-N64 system byte) and the line BELOW the diagnose gate; do_load() is the only
caller of the cart loaders; the fault screen is render-only, correctly registered, and cannot
exit the loop; and main() boots whatever is in boot_params the moment app_run() returns. Every
one of those was re-verified by grep tonight, not remembered. Yet the loop exited and the game
booted. Somewhere the model of what executes on the M64 is wrong, and every channel that could
say where -- log, dumps, playstate, the page itself -- is exactly what has never worked there.

So the next build stops deducing and watches: do_load() now paints the screen a solid colour at
each stage and holds it 1.2 s (checkpoint() in screen_launch.c). WHITE cart loaded, YELLOW cheat
list built (green/red corner: non-empty), CYAN at the patch branch (corner: taking it), BLUE
patched-and-dumped (corner: read-back verified), ORANGE N64 boot params set, MAGENTA the
diagnose gate with app_fault() next, RED nine-squares the emulated branch -- the one exit that
boots without passing the gate. The user names the last colour seen before the game appears,
and that sentence is the divergence point. The screen is the only channel that has never failed
on this console; the film needs nothing else. Also still armed from §2i: no
`sc64menu.n64.main`/`.prev` were restored, so their reappearance would convict the boot chain
of managing menu copies itself.

## 2i. The card autopsy, a second card's testimony, and one experiment that splits the theories

The original card came back mountable one more time and gave a clean damage map: the root
directory's entries unreadable to readdir but resolvable by name, `mainmenu/` reduced to
null-character garbage entries, and exactly four casualties among the files -- `sc64menu.n64`,
`cheats.db`'s chain (the §2h FR_INT_ERR), the old `Zelda - Ocarina of Time.z64` (chain truncated
mid-file), and everything in `mainmenu/` -- which is precisely the set of most-recently-written
and most-recently-read regions. Everything else salvaged in one clean pass: 15 N64 ROMs, all of
which verify against their own bootcode checksums via tools/romcrc.py (Pokémon Stadium's CRC2
mismatch is the §romcrc known exception, CRC1 matches), the SNES tree, all saves, emulators,
and the mystery `sc64menu.n64.main`/`.prev` pair, hashes recorded in the backup.

Then the card convicted itself with no console in the room: freshly erased (whole-disk MBR +
FAT32) and restored from the Mac, the write-then-read verify came back with **684,275 wrong
bytes in `1080 Snowboarding.z64`**, a contiguous ~668 KB region starting ~1 MB in. A rewrite of
the same file verified. A card that silently corrupts a bulk write on a fresh filesystem is
failing physical media; the months of history -- writes dying quietly since early August, then
structural rot in the hottest sectors -- is what that failure mode looks like from software.
Every master now lives on the Mac (`save-backup-2026-08-07`); the card is a disposable test
medium until replaced.

Meanwhile the SECOND card (267 MB, FAT16, fresh) reported: cheat ticks and playstate do not
survive a restart there either -- so console-side write failure exists on healthy media too.
Two candidate explanations remain, and the rebuilt original card is the discriminating
experiment, because it is FAT32, freshly laid, and byte-verified -- the same configuration as
the months in which console writes demonstrably worked: if persistence returns on it, the second
card's failures were FAT16 (the SC64 manual recommends FAT32/exFAT and the user suspected the
type first); if persistence is still dead, the console-side write path broke in early August
and the media was never the whole story. Deliberately NOT restored: `sc64menu.n64.main` and
`.prev`. If either reappears on the card after console use, the M64's boot chain manages them,
and the "which build actually runs" question -- still open, since the second card's
no-diagnostic-page run had an unverified boot plate -- gets its answer.

## 2h. The card's filesystem was failing the whole time, and it finally said so out loud

Minutes after §2g was written, the 0A0043 run answered the writer mystery before Start was ever
pressed. Opening Ocarina's detail sheet -- `detail_background()` → `load_cheats_now()` →
`cheatdb_load()` → `fseek()` -- ended in libdragon's Inspector: `ASSERTION FAILED: FatFS
assertion error`, `err != FR_INT_ERR`, backtrace `__fat_lseek (fat.c:279)` →
`__fresult_set_errno (fat.c:111)` → `cheatdb_load (cheatdb.c:270)`. FR_INT_ERR is FatFs finding
its OWN on-disk structures inconsistent while walking cheats.db's cluster chain; libdragon
hard-asserts on exactly that one FRESULT and maps every other to errno. On the next power-up the
SC64 firmware -- an independent FAT implementation -- reported no FAT volume at all. The boot
plate read 0A0043, photographed; the build is not implicated, since the crashing path is
unchanged for weeks and fires on sheet-open in every build.

This unifies the whole §2g evidence blackout: the cache probe's "no" since ~Aug 4 was the card
genuinely refusing console-side writes, not a probe bug -- decided by doing, and the doing was
honestly failing. The politeness gradient also fits: write paths got non-asserting FRESULTs and
died silently for days; the first structural inconsistency on the READ side asserted.

Unresolved and next to measure, in order: whether macOS still mounts it (it verified clean --
fsck_msdos exit 0 -- hours before the assert, so either the decay is fast, which is what a dying
card's FAT-region blocks look like, or the damage is one macOS tolerates and both console-side
implementations do not); a full backup the moment it mounts anywhere, saves first; then a fresh
FAT32 format and restore -- or a different card outright, which is the cleaner experiment.

Two facts worth keeping from the wreckage: the libdragon Inspector works on the M64 and shows
symbolized file:line backtraces over the TV -- a crash channel available all along -- and the
version code on the plate did its job for the first time, binding the photographs to this build
with no memory involved.

## 2g. The card was the witness all along, and three ways it was being silenced

The cheat investigation stalled for four days on "the diagnostic page never appears and the game
boots anyway", burning a hardware round per hypothesis. Reading the returned card and the source
side by side, instead of shipping another build, produced the following. None of it was visible
from ares, and none of it needed the console.

**The card's timestamps are not a timeline.** `SCRIPT_CLOCK_EPOCH` (app.c) pins every
harness-build clock to 2026-08-04 14:30 UTC so scripted frames hash stably. Three DEV_HARNESS
builds reached the console by mistake (§1at era), ran their input scripts against the real card,
and wrote `playstate.dat`, `cheatstate.dat` and `library.idx` stamped 14:36–14:37 "Aug 4" --
whatever the wall clock actually was. Any later analysis that reads card mtimes as history
inherits those forgeries. What IS trustworthy is content: `launch.log` contains no
`---- launch ----` banner, so zero append-era launches ever wrote it, regardless of when.

**One boot-time verdict silences every evidence channel at once.** `launchlog_begin()` gated on
`cache_writable()` -- despite launchlog.h promising plain-stdio independence -- and playstate,
cheatstate, usercheats, libindex, locks, profile, parental and thumbstore all gate on the same
single probe from `cache_init()`. The settings writer is the only one that never asks, and it is
the only one that demonstrably still works on the M64 (`config.ini` rewritten 2026-08-07 18:15,
`cheat_beacon = true` present). Whether the probe's verdict is wrong or the writes genuinely
fail is NOT established; what is established is that the coupling made the failure unreportable.
The log is now ungated with an append→truncate fallback that labels itself, and the diagnostic
page prints `cache_status()` plus a live re-probe with errno, so one photograph carries the
answer either way.

**Ruled out, with measurements:** the card itself. `fsck_msdos` exit 0 (304 files, no faults),
29.9 GB free of 30.5, macOS creates and deletes files in `/mainmenu/cache/` without complaint.
The FAT is healthy; the failure is in what the console-side stack does with it, or in a verdict
about it.

**A round that never ran.** Build 6FA150 -- the first with `diagnose = true` unconditional --
was deployed 2026-08-07 20:38 and the report of "still boots to game" refers to a run made
before that copy existed; the messages crossed. Recorded so the unconditional build is not
counted as having failed a test it never took. Still open from the gesture era: on plate-verified
69DC83, L alone on the detail sheet booted the game where the latched mode says `app_fault()` --
unexplained, and deliberately not pursued, because no current path depends on any gesture.

Also noted: `sc64menu.n64.main` and `.prev` (8,290,304 B each) appeared at the card root on
Aug 7, written by a Mac, from outside this workspace's history. Inert -- the SC64 boots
`sc64menu.n64`, and plate codes have matched fresh deploys throughout -- but unexplained files
on the test medium have cost three rounds before, so they are on the record.

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

## 1ag. The libultra preamble hook — proven under ares, dead on hardware, reverted

Written, verified end-to-end under ares, deployed, and **it did not work on the M64**. Reverted at
the user's direction in favour of upstream's Datel watch hook. Kept here because the mechanism was
sound, the ares evidence was real, and the next person to have this idea should find out what it
cost before spending the day again.

### What it did

1af left the engine hooked on a CPU feature no available machine delivers. The watch exception
solves exactly one problem — the game overwrites 0x80000180 with its own handler — so the patcher
was taught to survive that from the other side. By the time it runs, IPL3 has already copied the
game's first megabyte into RDRAM, and somewhere in it is libultra's `__osExceptionPreamble`, the
four instructions `osInitialize` copies onto every vector:

```
lui   $k0, %hi(__osException)
addiu $k0, $k0, %lo(__osException)
jr    $k0
nop
```

Only the two address halves vary between games, so it is findable by a masked scan. The patcher
scanned `[$t1, $t1 + 1 MB)`, saved the original `lui`/`addiu` into the engine's tail, and rewrote
the pair in place to compute the engine's address — letting the game install our hook for us. The
engine's tail replayed the displaced words to reach the real `__osException`. Loads, stores and a
jump; no COP0 anything. The Datel hook stayed as the fallback when the scan missed.

### The ares evidence, which was not wrong

`src/dev/hooktest.c` ran the production emitter and then **executed the emitted patcher** with
`$t1` aimed at a synthetic megabyte of `.bss` holding a planted preamble, then executed the
patched preamble the way a vector would:

```
HOOKTEST fallback full-window scan: 53304 us
HOOKTEST 14/14 ok
```

8-bit, 16-bit and conditional writes all landed; control chained out through the fake
`__osException` with `$k0` holding its exact address; 0x80000180 and WatchLo untouched on the
found path; the one-word near-miss (`jr $k1`) rejected across all 262,144 words before arming the
fallback. The test could go red — mutating the first scan compare produced 8/14 with six named
FAILs, after a first attempt at that mutation wedged the console instead of reporting (the test
was fixed to restore the vector and watch before re-enabling interrupts).

### And none of that predicted the console

On the M64 the Settings line read "No watch, handler hook" — the intended state, hook armed — and
**cheats still did nothing in game.** The failure mode was not characterised: nothing distinguishes
"the scan matched the wrong sixteen bytes", "it matched nothing and fell back to the dead watch",
"it matched and the game does not route exceptions through the preamble we found", or "the writes
landed and the addresses are wrong for this ROM revision". `/mainmenu/launch.log` records the hook
*order*, not which branch the patcher actually took, because that decision happens after the menu
is gone.

**That is the finding worth keeping: an end-to-end green under ares, on executed emitted code, with
a working mutation control, still did not predict hardware.** It is the strongest form of evidence
this harness can produce and it was not sufficient. The gap is not in the test — the test proved
what it claimed. It is that the synthetic image is a preamble we planted, and a retail game is not.

Anything resuming this needs the patcher to leave a breadcrumb the menu can read on the next boot
— a word written to a known RDRAM address recording which branch it took and what it matched —
because without that, a failure here is indistinguishable from four different failures.

### Reverted

`git revert` of the whole change; `.text` back to 571,320 / `.data` 108,196 / `.bss` 75,944,
identical to 1af. `src/dev/hooktest.c` and `.h` are gone with it. The boot layer is upstream's
again, and §2.6 records exactly how far from upstream it now sits.

---

## 1ah. Icons, a keyboard, a licence screen — and a ROM five times bigger

6 Aug 2026. Three things asked for in one sitting: a credits/licence screen, a settings list that
could outgrow one page, and the profile rework onto a player chip with the handoff's
`Who's playing?` grid behind it. Planned in [NEXT-PROFILES.md](NEXT-PROFILES.md), with the
conflicts found while surveying it in [GOTCHAS-PROFILES.md](GOTCHAS-PROFILES.md).

### What it cost

| | before | after | delta |
|---|---:|---:|---:|
| `.text` | 571,320 | 606,904 | +35,584 |
| `.data` | 108,196 | 109,884 | +1,688 |
| `.bss` | 75,944 | 76,344 | +400 |
| ROM | 1,638,400 | **8,273,920** | +6,635,520 |
| `src/screens/` | 5,106 lines | 6,121 lines | +1,015 |

The ROM is 5.05x its previous size and essentially all of it is one file: `icons.pack` is
6,561,304 bytes of SVG text, 3,894 icons after the IP exclusions. The three new font bakes are
28,564 together, the credits text 4,836, the category index 8,290.

`.text` is +35,584 for svg64 (documented at 14,552), the icon cache, and three screens.

### svg64 runs here

The first icon this program ever rasterised was the player chip, `tools/inputs/chip.txt` dump 2 —
a white shield on a red plate. That single frame proves the pack opened, the index byte-swapped,
the seek landed, the SVG parsed, the recolour hit the right two colours and the cache handed the
RDP something it could blit. Before it, every one of those was unverified in this tree.

Measured from the boot log on a `ICON_LIMIT=200` fixture:

```
icon: 200 icons, index 3200 bytes, largest 7910, scratch 14880
icon: caches 64 x 40 px and 2 x 60 px = 219200 bytes
```

219,200 bytes is exactly the estimate — 64 x 3,200 plus 2 x 7,200 plus a 14,880-byte scratch.

### What an icon costs here, measured

The first version of this section said the cost was entirely unmeasured. It was, and worse than
that: **it could not have been measured, because `icon_pump()` sat outside every timing bracket in
the main loop.** It was called after `bg_us` closed and before nothing, so the one genuinely new
cost in the program appeared in no column of the FRAME line. The picker duly reported `bg_us=42`,
which reads as "icons are free" and is precisely the convincing fake number this document exists
to catch. `app->icon_us` and `app->icons_done` now bracket it.

With that fixed, `tools/inputs/profiles.txt` through the appearance editor, 200-icon fixture:

```
n=480   f1=38 f2=22 f3=0 f4+=0  worst=29195  bg=19  icon_us=504   icons=5
n=540   f1=36 f2=24 f3=0 f4+=0  worst=29250  bg=70  icon_us=5477  icons=60
n=600   f1=34 f2=26 f3=0 f4+=0  worst=28335  bg=70  icon_us=5937  icons=60
n=660   f1=37 f2=23 f3=0 f4+=0  worst=30131  bg=66  icon_us=6164  icons=58
n=720   f1=44 f2=16 f3=0 f4+=0  worst=26364  bg=42  icon_us=677   icons=6
```

`icon_us` is the mean per frame over the window. At peak that is **6,164 us of a 6,000 us budget,
producing 58 icons in 60 frames** -- one icon per frame, with the budget saturated exactly as
intended. Per icon: 6,164 x 60 / 58 = **6,377 us**, against svg64's own documented 4,812 us. Same
order, about a third higher, which is unsurprising for a different build with different flags.

So a full 45-cell page fills in about **0.75 s**, three quarters of a second of cells popping in.
That is the behaviour the progressive fill was designed to produce and it is what it produces.

**No missed fields are attributable to it.** `f3=0` and `f4+=0` across every window with the
picker on screen, worst frame 26-30 ms -- the same as a settled grid. The time budget is doing its
job: one icon per frame is affordable, and the cap is what stops a page trying to be 216 ms.

### The 90 ms frames were the harness, not the picker

Worth recording because it was nearly written down as a finding. An earlier read of the same script
showed `worst_us` of 80,000-97,000 with `f4+=1` in nine consecutive windows, right through the
profile screens, and the obvious conclusion was that the picker was dropping five fields a second.

It was the framebuffer dumps. That run was `FBSCALE=1`, so each `fbdump` is a 640x480 surface
hexdumped to stdout -- about 4 MB of text. Counting windows containing a 4-or-more-field frame:

| run | scale | windows with a 4+ field frame |
|---|---|---:|
| `final-profiles` | `FBSCALE=1` | 10 of 15 |
| `icontime` | default (160x120) | **1 of 15** |

The one remaining is the boot window, which is the library scan and predates all of this. Frame
numbers from a `FBSCALE=1` run describe the harness as much as the program, and this is the second
time that class of mistake has nearly landed in here.

**Still unverified: hardware.** Every number above is ares, and svg64's README is explicit that it
has never run on a console. Given 1ag, an ares green on a new execution path is worth exactly as
much as it was last time.

### The `-j8` font race

Four faces bake from one `Firple-Bold.ttf`. `mkfont` names its output after its *input* and offers
no way to override it, so all four rules wrote `build/fontbake/Firple-Bold.font64` and then renamed
it — and under `make -j8` three won the race and the fourth `mv` found nothing. It presented as
`FirpleSmall.font64` simply not existing after a build that had printed `[FONT]` for it, which
reads as a missing rule rather than as a race. Fixed with a scratch directory per target.

### Two tests that could not fail, caught by trying to make them fail

Both are the house rule earning its keep in one afternoon.

1. **The settings overflow assertion was a tautology.** `_Static_assert(LIST_Y + VISIBLE * ROW_H +
   INFO_H <= FOOTER_Y)` cannot fire, because `VISIBLE` is *derived* from `INFO_H`. Feeding it an
   `INFO_H` of 160 changed nothing. Replaced with one checking the reservation covers what is drawn
   into it, which does fire when `INFO_LINES` goes to 3.

2. **The profile-1 save guard was not really tested.** Relaxing `index <= 0` to `index < 0` in
   `profile_erase_saves()` left the mutant passing — because slot 0's folder name would be `p1`,
   and nothing has ever written to `saves/p1/`. The guard is belt and braces; what actually
   protects profile 1 is that its saves are in the *unsuffixed* `saves/` and that function never
   builds that path. The mutation now also makes the path agree with `profile_save_subdir()`,
   which is the bug somebody would really write, and three checks go red.

### The format 1 roster is read without renumbering

`profiles.ini` gains a version key. Version 1 had `count` profiles in slots 0..count-1 and closed
the gap on delete — and the slot number names `saves/pN/`, so deleting player 2 turned player 3's
folder into player 2's. Version 2 gives every slot a `used` flag and never moves anybody.

Reading version 1 needs no conversion, which is why the old layout could be *adopted*: contiguous
slots are the new format with the first `count` slots marked used. Nothing moves, so no existing
card can lose a save by being read by this build. Covered by `test_migration()` in
`tools/hosttest/test_profile.c`, which writes a real version 1 file and checks all three names,
the active index and the save subdirectory survive both the read and the rewrite.

Deleting a profile now deletes its saves, warned, with the count on screen before the answer.
Profile 1 is refused outright at both the UI and the function.

### Where the handoff and the code disagreed

- **L and R already paged the tabs.** The chip became the last stop on the rail rather than a
  separate control, so nothing was rebound. The rail also stopped wrapping: a modulo wrap would
  put the player plate one press left of RECENT and make "keep pressing R" cycle past it forever.
- **The position bar at x=628** is 10 px outside the safe area (`SAFE_X 16, SAFE_W 608`). The grid
  had already met this and settled on 618; the appearance screen asserts against `POSBAR_X` rather
  than inventing a second answer.
- **The category column at 132 px** clipped its own names at 24 px — "Monsters" drew as "Monster",
  "Abstract" as "Abstrac". Widened to 140 and every display name capped at eight characters.
- **The handoff's 10 x 10 1-bit paged atlas** is the board's stand-in for browsing; svg64 replaces
  the storage model entirely and keeps the sizes.
- **30 categories, 9 rows of space.** The handoff says "flat list, no nesting" and stops there. The
  list scrolls, with the same window-follows-cursor the settings and lock lists use.

### The cache-key bug that made the picker look broken

`icon_get()` is keyed on (index, ink, paper) — the same icon in two colours is two cached things.
The appearance screen requested a page in the profile's swatch and drew it in chrome colours, so
every cell missed forever and the grid stayed empty with nothing in any log to say why. The
request and the draw now sit as mirrored named locals with a comment saying they must not be
factored apart.

### Reversed: the argument against a keyboard

`screen_profiles.c` argued that an on-screen keyboard "would be the largest single piece of UI in
the program in order to be used twice per household". Correct for one screen, wrong for three.
`screen_keyboard.c` is 387 lines against about 80 for the two odometers it replaced, so
`src/screens/` is roughly 300 lines heavier — the comment first written in that file claimed the
directory came out smaller, and measuring said otherwise.

### Second pass, after the first interactive run

Four things came back from actually driving it, and three of them were the design being wrong
rather than the code.

**The chip needed a press it should not have needed.** R lit it up, A opened it. Two presses to a
destination with no other purpose -- and while the chip was lit the last tab still drew as
selected, so the rail showed two selected things at once. `chip_focus` is gone entirely: R past
the last tab *is* the picker.

**An empty keyboard had no exit.** B deleted, DONE refused an empty field, so opening one by
accident was a dead end. B on an empty field now leaves, and the footer hint says which of the two
it currently means.

**DELETE was a key that did what the button under your thumb already did**, sitting exactly where
a thumb going for SPACE would land. Removed; SPACE and DONE are wider for it. Digits are back as a
top row on both charsets -- the handoff says a name is letters only, which is right about most
names and wrong about what a keyboard should refuse when there is no shift key and the cost is
one row.

**A profile had one colour, not two.** The swatch table paired each plate with a fixed artwork
colour by luma, which meant white-on-red was not askable for -- the table had already decided
red-on-white. The palette is now ten (the eight named swatches plus the two neutrals) — *superseded
by the third pass, which found two of the ten were the same white and cut it to nine* — and the
plate and the artwork are separate choices from it, on two labelled rows. The pairing survives as
`profile_default_ink()`, which is what a new slot gets and what a card with no `ink` key reads as,
so nothing already written changes appearance.

Two things fell out of that:

- Picking the same colour twice makes the artwork vanish into its plate. Refused on apply with a
  sentence, not prevented in the cursor -- a swatch you cannot land on is a rule nothing on screen
  explains.
- The dark neutral is `#101019` on a dark panel, which is the same colour as the gap between
  swatches. The palette read as nine colours and a hole, and the cursor could sit on a swatch that
  appeared not to exist. Every swatch now gets a hairline.

Host suite was 98 profile checks at this point, and both profile mutations fired. Superseded: 100
after the third pass.

Uniqueness grew a third field: (icon, plate, ink). Two people sharing a sprite *and* a plate are
still distinguishable if the artwork differs, which is the point of splitting them.

---

### Third pass: the picker was two bugs, and only one of them was the one I looked for

"Not all icons are displaying, as you move around which ones are displaying changes, some icons
are blinking." Two independent faults, both invisible in a single frame, and the obvious one was
not the bigger one.

**The cache was direct-mapped on the pack index**, on a comment that said "the access pattern is a
grid cursor walking a contiguous run of indices, so consecutive icons land in consecutive slots
and a page of 45 into 64 slots collides with nothing". The premise is false. A category is a
scattered list of pack indices baked by `tools/mkiconmeta.py`, not a run. Counted over the shipped
`icons.meta`: **87 of the 100 category pages collide, nine cells to a page on average, eighteen on
the worst** -- Travel page 1 puts 45 icons into 30 slots. Colliding cells evict each other the
moment either is decoded, forever. Both caches are fully associative with least-recently-touched
eviction now; a linear scan of 64 entries costs 64 integer compares against a 6,377 us decode.

**The request loop reached at most 22 cells either side of the cursor.** It folded distance and
direction into one counter -- `cell + (d odd ? -(d+1)/2 : (d+1)/2)` over `d` in 0..44 -- which
tops out at a distance of 22. With the cursor at cell 0 that is cells 0..22 and no others: the
bottom half of the page was never *asked for*, so no cache could have held it. This was the
larger of the two and I would not have found it by reading, because it looks like an ordering
trick and it is one; it is just also a bound. Distance and direction are two loops now.

**And the cursor cell was rasterised in different colours from the rest**, so every press threw
away two cached icons and asked for two new ones at 6,377 us each. That is the blinking. Selection
is geometry now -- a two-pixel lift and a white frame drawn around the cell, touching no pixel
svg64 produced.

Measured with `tools/inputs/iconcache.txt`, reading the FRAME line's `icons=` column, full corpus
(`ICON_LIMIT=0`), on a full 45-cell page:

| window | before | after |
|---|---|---|
| entering the editor | 24 decodes, then **0 for 240 frames with 21 cells still empty** | 79 decodes over two windows (32-cell page + 45-cell page + 2 previews), then **0** |
| 300 frames sitting still | n/a -- never reached a full page | **0** |
| 13 cursor presses | 22 | **13** -- exactly one each, and all of them the 60 px preview |
| 420 frames after that | n/a | **0** |

The 13 are correct and not a residue: the preview is the thing the cursor points at and has to
follow it. The 40 px grid is untouched by cursor movement.

**That measurement can go red.** `CACHE_40` is overridable, and `TUNE=-DCACHE_40=32` pins `icons=`
at 60 per window -- the budget ceiling -- for the entire run and never settles, because 45 cells
do not fit in 32 entries however they are indexed. Without that check the green above would only
be evidence that something was happening.

Also from the same pass:

- **The palette had two whites.** `#E6E6DE` and `#F7F7FF` quantise to (28,28,27) and (30,30,31) in
  RGBA5551 -- three levels apart out of 32, on a palette whose whole job is to be told apart at a
  glance. The bone one is gone; nine colours, and `PROFILE_PLATES` is seven. Five host checks went
  red on that, correctly: they were asserting the palette's size in passing by writing `9` where
  they meant the light neutral. Named now, plus a check that an ink one past the end is refused.
- **The colour rows were unusable and said so.** "I have no idea how to use it, how do I navigate
  to it, it's also not obvious which colour is selected." A 2 px ring around one rectangle in a row
  of rectangles is not a selection mark. Three things say it now: the chosen swatch is a third
  taller than its neighbours, its ring is white when its row has the cursor and grey when it does
  not, and the colour is **named in words** at the end of the row. The footer says which way the
  d-pad goes from wherever the cursor is, because the rows are reached by walking off the bottom of
  the icon grid and nothing on screen said so.
- **The footer hint overlapped the B hint** -- "B Back" and "Left / Right: colour" rendered as
  "BadLeft". The longest of those strings wants about 400 px and the hints already reach x=238,
  leaving 386. It is a second line now rather than a shorter string, and the refusal message takes
  that line when it is showing.
- **`tools/inputs/profiles.txt` was testing nothing on the ink row.** It pressed right 8 times from
  an ink that already defaulted to the second-to-last swatch, so the row moved one square and
  stopped against the end. A row that does not appear to move is not a row that has been tested.

### Fourth pass: a keyboard drawn under its own text field, and a name nobody could change

**The digit row was four pixels underneath the field.** The field is `FIELD_Y 44` plus `FIELD_H 64`
= 108. The handoff gives the first key row y=104, which was correct when the first row was QWERTY
and the field was somewhere else on the board; adding digits above the letters put a row at 104
against a field ending at 108, and the top row of keys was drawn inside the box being typed into.
Nothing caught it because every number in the file was a literal that agreed only with itself.

The rows carry an index now and the Y is computed: `KB_TOP` is the single place the clearance is
stated, and the block is centred in what is left between the field and the footer. That gives the
slack to whichever charset has it -- the 4-row name keyboard opens with a **44 px** gap under the
field where it had **-4**, and the 5-row symbol keyboard gets 20. Keys went 48 px to 42 to make the
five-row case fit, and the glyph baseline is keyed off `KEY_H` rather than being the literal 36.

`_Static_assert(BLOCK_H(ROWS_TEXT_N) <= KB_BOT - KB_TOP)` is what would have caught it. Checked
that it can: `FIELD_H` 64 -> 80 fails the build with "the symbol keyboard does not fit between the
field and the footer".

**`charsetcheck.py` went red on the same change, and correctly.** Its keyboard-row regex matched
`{"QWERTYUIOP", 32, 156}` -- a string and *two* numbers -- so dropping the Y made it match nothing.
It printed "found no keyboard rows to check, has the table changed shape?" and returned failure
rather than reporting a clean run over zero rows. That guard was written on the theory that a
checker which silently checks nothing is worse than no checker; this is the first time it has been
the thing that fired.

**A player's name could be set once and never again.** Only at slot creation -- START, or A on an
empty card. Z went to the appearance editor and the footer called it "Edit", so the button that
claimed to edit a player offered only colours. Z now opens a two-row menu, Name and Icon and
colours, and the footer reads "Z Name or icon".

Not a second button, and specifically not R: R past the last tab is how the grid reaches this
screen, so pressing it twice would land in a rename keyboard pre-filled with your own name, where
B deletes a letter. A mis-press that silently shortens a name is worse than the extra press the
menu costs.

Verified end to end in ares with `tools/inputs/rename.txt`: create a player, name it 111, reopen
via Z, rename to 22, and the roster is still **2 of 10 used** -- a rename and not a second profile.


### Fifth pass: the rail follows you into the picker

**The picker used to be a dead end with an answer as its only exit.** Pressing R one too many
times on the grid put you in front of ten cards, and getting back to a tab meant picking a player.
The rail now stays drawn there, with the chip lit like an active tab and R walking back out onto
the tabs -- so the picker is a place on the rail rather than a modal question.

Which meant moving the chip to the **left** end. It was past SMS, which is fine for a door and
wrong for a cursor position: "keep pressing R" would walk the whole rail to reach the thing you
touch most, and then have nowhere further to go. L from the first tab opens it now, R comes back,
neither end wraps. The chip also lost its name text -- at the right end the name was dropped
whenever the tabs needed the room, and at the left end keeping it would have moved every tab when
somebody switched to a player with a longer name. The name is in the grid's footer beside the Z
hint. The tabs start at a fixed `TAB_X0` past the chip, asserted not to overlap it.

Two accent bars, again. The chip and the first tab are adjacent, so a lit chip beside an underlined
active tab drew one 108 px gold stripe spanning both -- measured, not guessed: gold ran from x=14
to x=121 at y=62. The plate now says "this is the tab you are on" and the accent bar says "the rail
cursor is here", which are the same thing on the grid and two different things on the picker.

**A leftover call drew a second, lit chip on the grid.** `draw_chip` was last in the rail function
because it used to be right-aligned and needed to know where the tabs finished; it took that `x` as
its new `bool selected` argument. Non-zero always, so the grid drew its own chip and then a lit one
on top. Nothing warned -- int to bool is a legal conversion -- and it was found by measuring the
pixel row rather than by reading, because at 640x480 the two chips are the same chip.

Also removed, all as asked: the "Who's playing? / 2 of 10 used" header (the cards say both, and it
cost the rail its place -- kept only for the boot question, where there is no grid behind the
screen and R is refused, so a rail would advertise a move that does not work); the "Not saved to
card" line; and the confirmation's "Slot stays empty. Nobody else is renumbered.", which explained
a numbering scheme to somebody who had asked to delete a row. Both dialogs shrank to their
remaining content -- the confirmation had a third of its panel empty above the buttons.

**Black on white is the worst thing this display renders**, and the popups were full of it. Two
hundred fifty-six levels of nothing: RGBA5551 gives 32 a channel and a CRT blooms a bright field
into the thin dark strokes crossing it. Every selected-row plate is `panel_alt` with a border now
and the text brightens instead of inverting -- the edit menu, both confirmation buttons, and the
appearance screen's category list, which was the largest instance of it anywhere at 24 px.

Not changed, and flagged rather than done quietly: the keyboard's cursor key is still a white plate
with a near-black 32 px bold glyph. The failure mode is much weaker at that weight, and it is the
only mark distinguishing one key from thirty-nine identical ones -- a border there is a visibly
worse cursor. Say the word and it changes.

**The empty cards' dashes stopped six pixels short of the corner.** Stepping a fixed 12 px and
clipping the last dash to whatever was left put a two-pixel stub at the end of a 158 px edge with a
hole in front of it, so the bottom of every empty slot read as unfinished. Measured off the
framebuffer: the vertical run's last full dash ended at y=399 against a bottom edge at 407. The
pattern is stretched to fit now, landing a whole dash on both ends.

### The picker's cursor was the choice, which is why colours could not be tried

A applied the icon under the cursor and left. So walking down to the colour rows dragged the icon
with it: there was no way to hold a face still and see it in a colour you were considering, which
is the only thing the screen is for.

A now *takes* the sprite under the cursor and nothing else -- accent bar under its cell, the same
bar that marks the active tab and the active profile's card, and the 60 px preview holds it. START
commits and leaves. **B also commits**: nothing on the screen shows the stored appearance beside
the working one, so a Back that discarded would be indistinguishable from a Back that saved until
you were already on the card grid. Both go through `apply()`, so neither can leave with a
combination that cannot be stored.

That also took the last per-press decode out of the picker. The preview followed the cursor, so
thirteen presses cost thirteen 60 px rasterisations; it follows the *choice* now, and moving the
cursor costs nothing at all. `tools/inputs/iconcache.txt` expects zero.

Rename reached the appearance editor through Z's new two-row menu, so `tools/inputs/profiles.txt`
needed `z, down, a` where it had `z` -- caught because the frame it dumped was a keyboard.



### Player 10 was called Player 1

Asked in passing -- "what do we call Player 10 by default, that's 9 characters yeah?" -- and it
was a real collision, twice over.

**The string.** The fallback name is built into a buffer sized `PROFILE_NAME_CAP`, which is nine
bytes: eight characters and a terminator, because eight is what the keyboard lets anybody type.
"Player 10" is nine characters, so snprintf truncated it and slot 10 came back as **"Player 1"** --
the same string slot 1 shows, on the screen whose entire job is telling ten people apart. The cap
governs typed names; the fallback is neither typed nor stored and had no business sharing it.
`PROFILE_LABEL_CAP` is ten now, and it is what every layout drawing a name reserves against.

The host test was written first and went red on all three of its checks: the tenth name, no two
slots sharing one, and the longest being nine characters. 104 profile checks now.

**The box.** Fixing the string moved the clipping rather than removing it. The profile card drew
its name into `CARD_W - 8` = 104 px and "Player 10" measures **105 px of ink** -- so the card still
read "Player 1", now by losing a glyph instead of a byte. Found by rendering it, not by reading:
`tools/hosttest` proves the string and only a frame proves the box. The card's name spans the full
112 px now, which leaves 7. Eight wide capitals would still overrun it, which is a thing somebody
does to themselves and sees immediately; "Player 10" is a thing the menu does to a card with ten
players on it.

### The name is back on the rail's chip

It was dropped when the chip moved to the left end, because a box measured from the current name
would move every tab whenever somebody switched to a player with a longer one. `CHIP_NAME_W`
reserves the worst case instead -- nine characters, 108 px -- and the rail had 164 spare. Measured
after the fact: "Player 10" renders 105 px into that box, unclipped.

The tab run now ends at 626 against a rail reaching 624, with the last tab's own edge at 616
because the trailing pad is not drawn. That is close enough that a ninth tab or a longer label
would push SMS off the side of a CRT silently, and the labels come from `library_tab_label()` so
the width is not a compile-time quantity and cannot be asserted. `screen_grid_draw_rail` says it
once instead. Checked that it can fire: a 20-character reservation prints
`GRID tab rail runs to 738, past the rail's 624`.

The grid's footer hint was labelled with the active player's name, because the name had nowhere
else to live. It says "Players" now -- the chip carries the name, and printing it twice on one
screen is not information.



### Sixth pass: a plate is a highlight, and a column index is not a position

**"When on the profile tab, recent tab is still highlighted."** Third go at the same thing. The
active tab first kept both its `panel_alt` plate and its accent bar while the chip was selected,
which drew two gold bars side by side into one 108 px stripe. Dropping the bar was not enough: a
lit plate is still a lit plate, and the tab went on reading as the selected thing on a screen whose
cursor was somewhere else. The tab gives up both now and keeps only its brighter label, which says
"this is where R goes back to" without claiming to be selected.

**Up and Down on the keyboard carried the column index, which is not the same as the position.**
The rows are staggered like a real keyboard -- ASDFGHJKL is inset 30 px from QWERTYUIOP and
ZXCVBNM is inset 88 -- so an index carried across a row change drifts right by half a key per row.
Counted over the letter rows, **16 of the 26 downward moves landed on a key that was not
underneath**: W went to S with A sitting under it, J went to M with N underneath, and the three
keys past the end of a short row all piled onto its last one. Pressing down slid the cursor
sideways.

It moves to the physically nearest key now, measured against a remembered x that only *sideways*
moves update -- the same rule a text editor uses for a line shorter than the one above, and what
makes `P` down-down-up-up come back to `P` rather than drifting to `O`. Entering the action row
picks SPACE or DONE by the same test, so what is below the cursor is what the cursor gets.

Checked twice over: a model of the geometry enumerates every vertical move and every one of them
now names the key underneath, and `tools/inputs/kb-updown.txt` walks the real thing in ares --
`1` down three times reaches `Z`, `N` up twice reaches `I`, and `8` down four times reaches DONE,
each exactly what the model said.

**A category page that was not full could not be left downwards.** Down tested the cursor's row
against the grid's five, and a separate clamp pulled any cursor that had landed past the end of a
short page back to the last real cell. On a page that is not exactly 45 those two fought: Treasure
page 2 holds 32, so Down from cell 27 moved to 36, found it empty, and got dragged back to 31 --
*sideways*, along the same row -- and every Down after that did nothing. The Icon and Plate rows
were unreachable.

Not an obscure page, either. The editor opens on whichever page holds the profile's own sprite, and
for a fresh profile 1 that is Treasure page 2 -- so this was the first thing anybody saw, and it is
part of why the colour rows read as undiscoverable in the first place.

Down now leaves the grid when there is nothing below the cursor, which is not the same question as
being on the fifth row. Enumerated over every page shape -- 1 cell, 5, 9, 32, 45, and a full last
page -- all of them reach the colour rows, and `tools/inputs/iconshort.txt` walks the real one.



### The loading screen came after the thing it was loading for

On a card with more than one player the order was: ten profile cards, cold and unannounced, then
the player picks one, *then* the SC64 plate plays over the grid. The boot screen ran second.

boot_plate.c argues at length that the plate must be an overlay and not a `SCREEN_BOOT`, because a
screen of its own would have to hand over at t=1.64 to something arriving cold -- "one frame of
empty grid before the first tile lands, which is precisely the second fade-in the spec rules out".
That reasoning is intact and unchanged. What was wrong was the unstated assumption inside it: that
the thing underneath is always the grid. It is whichever screen is first, and on a family's card
that is the picker.

So the plate is one instance in boot_plate.c rather than a struct the grid owns, armed by whichever
screen gets there first and a no-op for the second. The grid keeps a flag of its own for choosing
the opening tab, which still has to happen exactly once and is a different question.

**The hold was doing work, and the work had to move with it.** The plate's 1.3-3.0 s hold is when
covers decode at `DECODE_BUDGET_BOOT_US` -- 14,000 us a frame, free because nothing is animating
and no input is accepted. That budget belonged to the grid's `background()`. Left there, moving the
plate would have spent the hold on nothing and dropped the user onto a cold grid behind a curtain
that had already lifted, which is the exact failure the plate exists to prevent, relocated rather
than fixed. The picker spends the same budget on the same work now, and the constant moved to
boot_plate.h because the plate's hold is the only reason it exists.

Measured with `tools/inputs/bootorder.txt`. Four players (`DEMO=1`): the mark is up at frame 40,
the log reads `BOOT plate held 1316 ms, released by the screen`, the curtain lifts on a picker
whose four faces are already drawn, and the grid two presses later shows all four Recent covers
painted -- no second plate and no second fade. One player (the fixture): identical, `1316 ms`,
plate over the grid, which is what it always did. And the plate fires exactly once per boot --
reaching the picker later through the chip logs no second `BOOT plate` line.

Readiness is per screen. The grid waits for its first row of covers; the picker waits for the faces
on its cards, skipping any slot whose sprite the pack does not contain -- `icon_get()` answers NULL
both for "not decoded yet" and for "no such icon", so a capped build would otherwise hold every
boot to the 3.00 s ceiling.


---

### Not done

- **No hardware run at all.** Boot time is the one users feel and the ROM is 5x bigger; nothing
  has measured it. The per-icon figure above is ares.
- **The ~1 icon/frame rate is a consequence of `ICON_BUDGET_US`, not a tuned value.** 6,000 us
  against a 6,377 us icon means the budget admits exactly one and then stops. Whether a page
  should fill in 0.75 s or faster has not been decided by anything but the default.
- **`charsetcheck.py` sees literals, not data.** The sprite-name line on the appearance screen
  draws a string out of the pack, and nothing checks it against the 84-glyph charset.
- **The credits screen's scroll is by block index, not pixel.** Total pixel height is the one
  number that scheme never computes, so the position bar is an approximation over 150 blocks.

---

## 1ai. Half the framebuffer is never displayed, and it is the odd half

7 Aug 2026. "When an empty profile box is not selected, the bottom dashes on the rectangle aren't
displayed." A dashed rectangle missing one edge, and the edge in question had been rewritten a
day earlier (1ah, fifth pass) after a report that it was "cut off a bit at the bottom". That fix
was correct and it did not fix this, which is the tell.

### The dashes were there

`tools/inputs/profiles.txt` frame 0, `FBSCALE=1`, counting `#52525A` pixels across each empty
card's top and bottom edge:

    slot 1  top 60  bot 60      slot 5  top 60  bot 60
    slot 2  top 60  bot 60      slot 6  top 60  bot 60
    ...

Ten dashes of six pixels on both edges of all nine empty cards. The framebuffer is correct and
has been correct the whole time.

### The VI shows one framebuffer row in two

`display_init()` is called with `{640, 480, INTERLACE_OFF}`. The VI output area for the NTSC
preset is 240 lines, so `vi_set_yscale(480)` programs `VI_Y_SCALE = 480/240 = 2048/1024`.
Measured rather than derived -- a probe compiled into `app.c` for one run, reading the register at
frame 60 rather than at `display_init()`, where `vi_write_begin()` has not yet flushed it:

    VIPROBE out y 35..515 lines=240 yscale=2048/1024

Two framebuffer rows per scanline. With interlacing off the sampling offset never alternates
between fields, so the VI scans rows 0, 2, 4, ... forever and **the odd rows of the framebuffer
are never displayed at all.**

A one-pixel horizontal line is therefore a coin flip. A card is 158 tall on a 172 px pitch, so
every top edge lands on an even row (78, 250) and every bottom edge on an odd one (235, 407). The
card was drawn as a four-sided box and displayed as a three-sided one, every frame, on purpose,
by arithmetic.

Selected cards were fine because they lift by 3 px *and* gain a solid two-pixel outline, which
covers all four edges whatever the parity. So the one state that proved the code worked was the
one state that could not show the bug.

### Why the harness could not see it

`DBG_FBDUMP` copies RDRAM. It shows every row and hashes rows nobody can see as though they were
on screen -- which is why measuring the dashes themselves, twice, produced a clean bill of health.
`tools/fbdump2png.py --vi` keeps the even rows only, and is what the console shows. Opt-in, so it
does not silently rewrite every hash in the suite. Run against the *pre-fix* log it reproduces the
report exactly:

    slot 1  top fb y=78   60      bottom fb y=235   2
    slot 6  top fb y=250  60      bottom fb y=407   2

Two, not zero, because the corners of the vertical dashes survive. After the fix, all nine cards
read 60 and 60.

### The fix is a constant

`HAIRLINE` was 1 and is now 2. Two pixels always covers one even row, so a hairline is one visible
scanline wherever it is put, and it costs nothing -- a 1 px line at an even y is also one
scanline. `dashed_rect()` hangs its bottom edge from `y + h - HAIRLINE` so a thicker line grows
inwards and the card keeps the height its neighbours were laid out against.

The other four `HAIRLINE` users -- the settings, parental and cheat-editor separators, and the
footer rule -- all happen to sit at even y today, checked by arithmetic: `LIST_Y + VISIBLE *
ROW_H + INFO_GAP` is 96 + 238 + 24 = 358, and `ROW_H` being even makes that true for any row
count. So none of them was broken, and all of them were one layout change away from it.

### Not resolved

**Every horizontal detail in this program is at half the vertical resolution it is drawn at, and
that is the mode's fault rather than a bug.** `app.c` says progressive-not-`INTERLACE_HALF` is
"the starting point for the A/B described in DESIGN.md, not a settled answer -- measure it, do not
argue it". That A/B has still not been run, and this finding is the first hard number in it: the
cost of `INTERLACE_OFF` at 480 lines is not "some shimmer avoided", it is **half the rendered rows
discarded**. `--vi` is now the tool for looking at the other half of the trade.

The remaining 1 px *horizontal* draws are the favourite triangle's staircase
(`screen_grid.c:227`) and the tab star's row table (`screen_grid.c:302`). Both are shapes made of
many rows rather than a single line, so they degrade to a coarser silhouette instead of vanishing.
Not measured on the VI view. Vertical hairlines are unaffected: the output area is 640 px wide
against a 640 px framebuffer, so x is not decimated.

---

## 1aj. Box art is portrait, and the tile never was

7 Aug 2026. "The rectangle itself isn't ideal for all systems", with a table of front-face
measurements taken off box protectors. Practically two shapes: ~0.70 portrait for every cartridge
box, 1:1 square for Game Boy.

### What the old tile cost

140 x 98 is aspect 1.4286. `image_decoder_start_scaled()` scales to cover and crops -- correctly,
per DESIGN.md section 7 -- so a 0.702 cover was scaled to 140 wide, which makes it 199 tall, and
then had 101 of those rows thrown away. **51 % of every cartridge cover on the card**, top and
bottom, before anything reached the screen. It was not a rendering compromise; it was in the
decoder, where nothing downstream could tell it had happened.

The fixture had been hiding it. `mkfixture.py` drew 280 x 196 landscape art, so the fixture was
the one library on earth whose covers fitted the tile.

### Five columns, and the art got bigger

A portrait tile cannot be four columns wide: 140 wide is 199 tall, and 199 + 12 into a 352 px
window is one and a half rows. Five columns is (596 - 4x12)/5 = 109.2 -> 109, so 109 x 155.

| | old | new |
|---|---:|---:|
| tile | 140 x 98 | 109 x 155 |
| pixels | 13,720 | 16,895 |
| of the cover kept | 49 % | 100 % |
| visible | 12 + 16 px peek | 10 + 12 px peek |

Measured off the rendered frame rather than asserted -- `tools/inputs/boxshapes.txt` frame 1,
scanning for the gaps:

    row y=100:  16..124 art, 12 gap, 137..245, 12, 258..366, 12, 379..487, 12, 500..608
    col x=150:  6 pad, 78..232 art (155), 12 gap, 245..399 art (155), 12 gap, 412..423 peek (12)

Columns 109 wide on a 121 pitch, rows 155 tall on a 167 pitch, and the position bar clear at 618.

### A row is as tall as the tallest box in the tab

Not per row -- that is masonry, and row heights that change as you scroll make the scroll position
stop meaning anything, and make every tile below a short row move when a favourite is added. Per
tab, computed in one pass when the view is rebuilt. So the N64 tab is a tight grid of portrait
boxes, the Game Boy tab a tight grid of squares, and Recent, which mixes them, keeps the portrait
pitch and centres the square cover in it with plate showing above and below. Nothing is ever
cropped to match a neighbour. `Makefile`'s fixture playstate gained `Pocket Racer.gb` for exactly
this: without it Recent held three SNES titles and the mixed case had no frame.

### The atlas had to grow, and the index had to learn the shape

`thumbs.pak`'s slot was 32,768 bytes -- two 16 KB FAT clusters, so a tile is one contiguous read.
The tallest tile is now `TILE_W x TILE_H_MAX` = 109 x 176 = 38,368, which does not fit. Two ways
out: trim the tile until it does (107 x 152 would have) or take a third cluster. Distorting the
art to suit a filesystem is the wrong way round, so the slot is 49,152 and 31 % of it is padding
where 16 % used to be -- 24.6 MB across 500 titles against 16.4 MB, on a card with 29 GB free.

`ti_record_t` grew from 16 to 20 bytes to carry each tile's own dimensions. Deriving them from the
record's system instead would have been free and wrong: a ROM moved between folders, or a region
switched in Settings, changes the shape while the atlas still holds the old tile, and reading
109 x 109 of pixels into a 109 x 155 surface does not fail -- it returns the picture followed by
46 rows of slot padding. A mismatch is a miss now, which costs one decode and produces the right
picture. Two host checks say so and both go red when the comparison is removed:

    FAIL  fetching a square tile into a portrait surface is a miss, not a sheared read
    FAIL  and the other way round

`MENU_CACHE_FORMAT_VER` 2 -> 3. This is a bump that genuinely has to discard art rather than one
honoured on principle: every cached tile is the wrong shape.

### Regions live on the card, because the numbers are not ours

The built-in table is the US retail measurements above. PAL NES boxes are taller and thinner, PAL
Master System stock changed mid-generation, and Japanese SFC, N64 and Game Boy boxes are smaller
than their US counterparts -- and none of that is a number this project has. Inventing five
plausible aspects and shipping them as a `[pal]` table would put fabricated measurements where a
reader takes them for facts, which is the one thing this file exists to prevent.

So `menu/boxart.ini` holds named sections of `system = WxH`, `ini_parser` gained section
enumeration, and the Settings row offers whatever the card defines plus the built-in. A section is
a whole table, which answers "PAL for certain sections" without a per-system setting: a card can
define one that is NTSC for N64 and PAL for NES and pick it.

Only the ratio is read. Every tile is one grid column wide and cannot be anything else, so
`127x181` and `254x362` are the same instruction. Out of range is refused with a line rather than
clamped, because a tile quietly pinned to the ceiling looks like the file being ignored --
exercised by the fixture, which carries a deliberately bad key:

    BOXART region 'NTSC': tallest 155 px
    BOXART region 'tall': tallest 174 px
    BOXART 900x100 is 12 px tall, outside 64..176 -- ignored
    BOXART region 'squat': tallest 152 px

Switching the region drops every resident tile (`thumbcache_reshape`), because they were all cut
to the old shape. The atlas keeps them, so switching back is a read rather than a decode.

### Found on the way

`screen_detail.c` printed "Not supported for this game" into a 318 px column at 12 px a glyph --
324 px of ink -- and the sheet read "Not supported for this gam". Pre-existing and worse before,
since the art panel was 280 wide and the column 256. "ROM" is two characters shorter and the more
accurate word: the engine hooks a ROM image, and the same game in another region may be fine.

### Not done

- **No hardware run.** Every number here is ares. The atlas slot growth is the one that matters
  on a real card and cannot be checked here -- the DFS is read-only, so nothing has ever written
  a 49,152-byte slot to FatFs.
- **Decode cost per tile has not been re-measured.** 16,895 pixels against 13,720 is 23 % more
  destination, and the source rect is bigger too because less is cropped away. The 259,633 us
  figure in 1f is now a lower bound of unknown tightness.
- **The no-art placeholder clips its title.** A 109 px tile leaves 101 px for the name, where a
  140 px one left 132. The tile is 155 tall and could carry three lines, but `ui_text` does not
  wrap.
- **`TILE_H_MAX` is 176 by choice, not by measurement.** It is an aspect of 0.62, taller than any
  box anybody has claimed, and it sets the atlas slot size. A real PAL NES measurement could
  move it.

---

## 1ak. A sample card, and what it says about reading the shape off the art

7 Aug 2026. "Why not variable size box art on every tab? Only support certain sizes, but it would
remove the need to have PAL/Japanese in settings." The right answer depends on how cleanly real
cover aspects cluster, `build/artcache` is empty on this machine, and the only prior measurement
-- the 40-card stratified sample recorded in `image_decoder.c` -- is of a corpus that is landscape
title cards by spec. So: `tools/mksample.py`, a third tree, 115 invented games with mkdemo's
original art at a **stated** spread of aspects.

Not a regression tree. The mix is chosen to contain failures, so anything measured against it
measures the mix; what it is for is looking at layout, and for sensitivity analysis.

### Two bugs in the generator, both of which would have produced a convincing corpus

**`crc32 % 100` never landed in 80..99.** Over the 115 generated titles, the four wrong-shape
kinds had a combined weight of 20 % and were generated **zero times**. The tree came out 76 `true`
and 39 `margin` -- mostly-correct art with some margin on it, which is exactly what the mix is
supposed to produce most of, so nothing looked wrong. CRC32 is linear and its residues mod a small
number stay correlated across inputs differing in a few bytes, which "Amber Drift" and "Basalt
Skipper" do. Fixed with fmix32.

**115 titles were 32 distinct names.** `FIRST[n % 32]` with `SECOND[(n * 7 + 3) % 32]` -- 7 is
coprime with 32 so the second word strides, but with period 32, the same as the first, so the
*pair* repeats every 32. Duplicate filenames across systems and a playstate keyed on a name
meaning several games. Striding the flattened index by 37 mod 1024 makes it a bijection.

Then the sampling itself went: a corpus is not a population. Even with a good hash, 115 draws
against a 3 % weight produced `tall` zero times, so `--mix realistic` claimed to contain a
PAL-shaped cover and did not. Kinds are dealt by exact quota now, in hash order so they interleave
across systems.

Both bugs are the same shape as the harness traps in 1b and 1u, and both are why the histogram is
printed on every run rather than assumed.

### What the sample shows before anything is changed

`tools/inputs/sample-grid.txt`, `SAMPLE=1` against `SAMPLE_MIX=true` as the control. The control
is clean -- every tab compact, nothing cropped, titles readable. The realistic mix visibly is
not: Recent renders "Obsidian Skipper" as `osidian kipper` and "Pewter Harvest" as `ter vest`,
because those covers were drawn landscape and the per-system table cut them into a square Game Boy
tile. That is the failure the proposal removes, and it is in the first frame.

### The measurement

Crop loss is `1 - min(As,At)/max(As,At)` for source aspect As into tile aspect At. Over all 115
covers:

| policy | mean crop | covers losing >10 % | worst |
|---|---:|---:|---:|
| **realistic mix** | | | |
| per-system table (today) | 8.1 % | 19 | 61.4 % |
| snap to {0.70, 1.00} | 5.0 % | 12 | 45.1 % |
| **snap to {0.70, 1.00, 1.43}** | **2.3 %** | **4** | 25.9 % |
| snap to {0.62, 0.70, 1.00, 1.43} | 2.2 % | 4 | 21.5 % |
| **hostile mix** | | | |
| per-system table (today) | 20.3 % | 59 | 61.4 % |
| snap to {0.70, 1.00} | 11.0 % | 36 | 45.1 % |
| **snap to {0.70, 1.00, 1.43}** | **2.9 %** | **8** | 25.9 % |
| snap to {0.62, 0.70, 1.00, 1.43} | 2.4 % | 8 | 21.5 % |

Three conclusions, and the useful thing is that all three hold across both mixes -- so they are
properties of the policy rather than of the mixture I chose:

1. **Snapping beats the table by 3.5x on a realistic card and 7x on a bad one.** The table is
   right about the box and wrong about the file, and the file is what gets drawn.
2. **Two shapes is not enough.** {0.70, 1.00} only halves the loss, because the worst case is a
   landscape "cover" -- a title screen, a cartridge photo, the old asset spec -- cut into a
   portrait tile, and neither bucket is anywhere near it. The landscape bucket is what does the
   work.
3. **A fourth shape buys nothing.** 2.3 % against 2.2 %. A dedicated PAL-tall bucket at 0.62 moves
   the mean by a tenth of a percent and does not reduce the count of covers losing more than 10 %
   at all. So "only support certain sizes" resolves to **exactly three**.

### Still not measured

**The mix is stated, not observed.** Every number above is a sensitivity analysis against
composition I chose. What it cannot say is how often a real pack actually produces each kind --
`build/artcache` is empty here, and until it is pointed at a pack the weights are a guess whose
shape happens not to change the conclusion. The conclusion is robust across a 2x change in the
mixture; it has not been tested against reality.

**Nothing has been implemented.** The shape still comes from the table. Snapping needs the aspect
known before layout and before the decode, which means resolving it on first decode and persisting
it in `library.idx` -- the atlas already carries per-tile dimensions, so a warm card has it free.
The cell height must keep coming from the table or one square-padded scan reflows the whole tab.

**The Settings row should be deleted last.** It is the escape hatch for whoever the snapping gets
wrong, and there is no measurement yet of how often that is.

---

## 1al. The shape comes off the cover, and it is measurably better

7 Aug 2026. Built on 1ak's numbers: a tile's shape is now read from its own art and snapped to one
of three, with the per-system table demoted to a fallback.

### Measured on the card, not on the model

1ak predicted 8.1 % -> 2.3 % mean crop from the mixture. Running the built menu against
`tools/mksample.py`'s realistic card and taking the numbers out of the console's own log -- every
`ART <file>: WxH -> shape` line, 58 covers actually probed during the run -- gives:

| | mean crop | covers losing >10 % |
|---|---:|---:|
| per-system table | 8.9 % | 11 |
| snapped | **2.1 %** | **2** |

Close enough to the prediction to say the model was measuring the right thing. The two survivors
are the deliberate `odd` covers at aspect 1.82, which snap to landscape and still lose 27 % --
there is no bucket for a cover that shape and 1ak established that adding one is not worth it.

Visible rather than only numerical: on the realistic card, Recent used to render "Obsidian
Skipper" as `osidian kipper` and the Game Boy tab rendered "Glass Signal" as `lass Signal`. Both
are landscape covers that were being cut into square tiles. Both read correctly now.

### Log space, not linear

The three aspects are a geometric run -- 0.702, 1.000, 1.4286, each about 1.43x the last -- so a
linear nearest-neighbour puts the portrait/square boundary at 0.851, which is 21 % away from
portrait and 15 % from square. A cover at 0.845 would be called square and cropped harder than the
candidate that lost. Comparing `max(a/b, b/a)` orders identically to `|log(a/b)|` and needs no
libm. Two host checks pin the boundaries at 0.838 and 1.195 and both go red when the comparison is
made linear.

### The probe is its own file because it is the part that fails silently

`image_probe_size()` reads a PNG's IHDR at a fixed offset and walks a JPEG's marker chain. Getting
the JPEG walk wrong does not crash: it returns an error, the record falls back to its system's box
shape, and every JPEG cover on the card is shaped by a table instead of by itself, with nothing on
screen to say so. So it lives in `src/menu/image_probe.c` with no dependency beyond `stdio.h`,
`tools/hosttest/test_boxart.c` compiles it natively, and the suite feeds it a PNG named `.jpg`, a
progressive JPEG, one behind six application segments, one with standalone markers, a truncated
one and an empty one. Deleting the standalone-marker skip turns exactly one check red.

`tools/mksample.py` now writes a quarter of its covers as JPEG for the same reason -- the two
formats are two entirely different pieces of code and only one of them is easy.

### Where the shape is kept

Probed once, in `art_resolve`'s caller, *before* the atlas is consulted -- the shape is part of the
atlas key, so asking first with the wrong one answers the wrong question -- and stored in
`lib_record_t::art_kind`, which persists into `library.idx`. A warm card knows every tile's shape
before it draws a frame.

That byte was `idx_record_t::reserved`, which is zero in an index written by the old build. Zero is
`ART_PORTRAIT`, not "unknown", so every square Game Boy cover on an existing card would have been
declared portrait and never re-probed. `LIBINDEX_MAGIC` bumped 'M64M' -> 'M64N'.

### A version bump that was quietly the wrong one

1aj raised `MENU_CACHE_FORMAT_VER` 2 -> 3 for the atlas rework. That is the shared version, and it
takes **playstate.dat** with it -- every favourite and every play count on the card, and the one
cache here that cannot be rebuilt from the card's contents. Trading those away to re-cut some art
is not a trade, and the fix was already precedent: `libindex.h` bumps its own magic for exactly
this reason and says so. `THUMBSTORE_MAGIC` 'M64T' -> 'M64U', shared version back to 2.

### Row height, and the one thing that could still reflow

`cell_h` is the tallest resolved shape in the tab, recomputed only when the view is rebuilt. Not
per frame: a cover probed since the tab was opened can be taller than the row it is in, and
recomputing live would move every tile under the cursor mid-scroll. The drawn height is clamped to
the cell instead, which scales the tile down rather than cropping it -- a tile briefly a little
small is a far smaller lie than one with its top and bottom removed -- and the next visit to the
tab has the right pitch. Only reachable on a cold card, and only for a cover whose shape is not
its system's.

### The fallback is landscape, by request, and it shows on a cold card

`BOXART_FALLBACK_KIND` is what a record gets under Automatic when it has no cover, or none
measured yet. It is `ART_LANDSCAPE`, set deliberately rather than derived: the measured table still
says every cartridge box is portrait, and that table is still what a *forced* region uses. This is
a separate decision about the case where there is no box to be right about.

It has a visible consequence and it is worth stating rather than discovering. On a cold card the
placeholder tiles are 109 x 76 and each one grows to its cover's shape as that cover is measured,
so the first fill of a tab is tiles changing height as well as gaining art. Captured: entering the
N64 tab 210 frames in shows nine short plates and one finished portrait cover, in 155 px rows --
`cell_h` was already 155 because one N64 title had been measured while the Recent tab was up, and
a single measured portrait record pulls the whole tab's pitch. So the pitch is usually right even
when the tiles are not, which is the better half of the two to get right.

A tab with *nothing* measured pitches from the fallback too, which is short. That is transient --
`measure_cells()` runs when the view is rebuilt, so leaving the tab and coming back fixes it -- and
the drawn height is clamped by scaling rather than cropping, so a tile in an under-pitched row is
small but whole. A warm card never sees any of this: every shape is in `library.idx` before the
first frame.

Taking the tallest shape whenever anything is unknown would remove the transient and add a
permanent one: a tab of genuinely artless games would be tall cells holding short plates forever.
Being briefly wrong is the cheaper mistake, and it is one constant to reverse.

### Still not measured

- **No real corpus.** `build/artcache` is still empty; every number above is against a mixture
  whose composition was chosen. The *ranking* is stable across a 2x change in that mixture
  (1ak), which is the most that can be claimed.
  <br>**Superseded in part by 1am**: there is now one real corpus, the SD card's own 28 covers,
  and its composition is nothing like any of the modelled mixtures.
- **The probe's cost is not measured.** One extra `fopen` and a few hundred bytes per record per
  index rebuild, on a path the AUDIT already records a 180-probe cautionary tale about. Under ares
  the DFS makes it free and the number would be a lie.
- **The Settings row survives on purpose.** "Automatic" is first and is the default; the tables
  are the escape hatch for whoever the snapping gets wrong, and there is no measurement yet of how
  often that is on a card somebody actually owns. It is the thing to delete once there is one.

---

## 1am. Five columns was the wrong answer for the only real card there is

1aj cut the tile from 140 x 98 landscape to 109 x 155 portrait and moved the grid from four
columns to five, on the argument that a box is portrait. The argument is correct about boxes. It
is wrong about the card.

**Every cover on the SD card is landscape.** Measured, all 28 of them:

| aspect | source size | count |
|---:|---|---:|
| 1.369 | 256 x 187 | 24 |
| 1.378 | 357 x 259 | 1 |
| 1.404 | 375 x 267 | 1 |
| 1.457 | 357 x 245 | 1 |
| 1.469 | 357 x 243 | 1 |

Not one portrait scan. Every one of them snaps to `ART_LANDSCAPE`, and at five columns that is a
109 x 76 tile: **8,284 pixels where the four-column tile it replaced gave 13,720**. The snap was
doing exactly what 1al built it to do -- reading the shape off the art and refusing to crop -- and
the result was that the whole screen shrank by 40 %. Which is what the report was: "5 per row
looks too small".

This is the corpus the "Still not measured" note in 1al was asking for, and it does not resemble
any of the mixtures `mksample.py` models. Those were built around the assumption that most covers
are box scans with some proportion of failures. On this card there are no box scans at all.

### The column count follows the shape

One width cannot serve both ends: 109 is the widest a portrait tile can be and still show two rows
in a 352 px window, and 140 is what a landscape tile wants. So there are two of them, and a rule
that picks:

> **the fewest columns that still show two whole rows.**

Two rows and a gap inside the padded window is `(352 - 6 - 10 - 12) / 2 = 162` px, so a tile
earns the wide column if it is 162 px or shorter at 140 wide:

| shape | at 140 wide | verdict | tile | cols | rows visible |
|---|---:|---|---|---:|---:|
| portrait 0.702 | 200 px | too tall | 109 x 155 | 5 | 2.01 |
| square 1.000 | 140 px | fits | 140 x 140 | 4 | 2.21 |
| landscape 1.429 | 98 px | fits | 140 x 98 | 4 | 3.05 |

Landscape lands back on 140 x 98 exactly, which is the tile the four-column grid had before any of
this started. Square gains 65 % more pixels. Portrait is untouched.

Three rather than two rows was considered and rejected: a three-row ceiling is 108 px, which sends
square art back to the narrow column and gives back most of what this buys. Two rows is what a
portrait grid has always shown here, so the rule asks nothing of the wide shapes that the narrow
one does not already accept.

### Why this is safe, and it is one property

A tab is laid out on one cell, taken from the tallest shape it holds. That only works because the
rule is **monotone: a taller shape never gets a wider column.** The tallest shape in a tab
therefore also has the narrowest column in it, so every other shape in that tab is drawn *smaller*
than the size it is cached at — never larger. Nothing is upscaled out of the atlas, which is the
entire point of caching at the drawn size.

It is asserted twice, once at compile time in `theme.h` and once in `test_boxart.c`, because it is
the kind of property that would break silently: the symptom is slightly soft art, not a fault.

The draw also changed from clamping to fitting. It used to pin the height to the cell and leave
the width alone, which squashes; it now scales both. That only shows up in the transient where a
cover is measured after its tab was laid out, but a distorted cover is a worse lie than a small
one, and both are worse than the crop this whole scheme exists to avoid.

### What it cost elsewhere

- **The atlas is invalidated again.** `THUMBSTORE_MAGIC` 'M64U' -> 'M64V'. Every square and
  landscape tile on a card is cached 28 % narrower than it is about to be drawn. Nothing in the
  file is *wrong*, which is why it needs a magic and not a check: the pixels would read back fine
  and be upscaled. `LIBINDEX_MAGIC` is untouched, so `art_kind` survives and a card that has
  already been scanned lays out correctly on the very first frame — only the covers re-decode.
- **The slot did not have to grow.** The largest tile is now a 140 x 140 square at 39,200 bytes
  against the 109 x 176 ceiling's 38,368, and both fit the existing 49,152-byte slot. The bound is
  written per width because a wide tile is capped by the two-row rule at 140 x 162 = 45,360, not
  by `TILE_H_MAX`.
- **The thumbnail pool grew from 1.19 MB to 1.35 MB** at its worst case, 36 slots of 140 x 140.
  Against ~3.8 MB free. Slot count is unchanged: a landscape tab straddling rows shows 16 tiles,
  which is the number 36 was sized for in the first place.
- **The detail sheet did not get the wide column, and that was measured.** Reserving two of the
  widest tile (280 px) is the tidy answer — exact 2x for every shape, no resampling — and it
  squeezes the info column from 318 px to 256. Rendered: the cheat row draws as
  "Not supported for thi" and any title over 21 characters loses its tail. `ui_label` clips rather
  than ellipsising and this sheet has already lost the tail of two strings that way. So the sheet
  keeps its 218 px column and fits a wide tile into it at 1.557x. Still better than before, which
  upscaled a 109 x 76 tile 2x to reach the same box.

### The cold-card transient got bigger

1al recorded that the first visit to a tab on a cold card is pitched from the fallback and
re-pitches on the next visit. That is still true and now moves tiles sideways as well: the
fallback is landscape, which is one of the two *widest* shapes, so a tab whose covers all turn out
portrait opens at four columns of 140 x 98 and re-lays to five of 109 x 155.

Captured on the sample card's Recent tab: four columns of 140 x 140 cells, with the portrait
covers that had been measured drawn at 98 x 140 inside them — whole, centred, and smaller than
they will be next visit. Not cropped, because the draw fits rather than cuts.

This does not reach the SD card. `library.idx` is not invalidated, so every `art_kind` on it is
already known and the layout is right from the first frame. It is a first-scan phenomenon only.

### Checked that it can fail

Three mutations, each producing exactly the predicted failures and nothing else:

| mutation | predicted | got |
|---|---|---|
| `fit_aspect` ignores the two-row ceiling, so every shape takes the wide column | portrait column, the bounds checks, both fit-into cases | 6 red |
| `boxart_fit_into` ignores the cell height | the squeeze case only | 1 red |
| `thumbstore` fetch compares height and not width | the shape-mismatch checks | **0 red** |

The third one is the finding. The atlas suite went entirely green with the width comparison
removed, because every pair of shapes in it differed in *both* dimensions — the width check was
load-bearing and unexercised. A `boxart.ini` can produce a 0.78-aspect box, which the rule puts at
109 x 140, beside a square cover's 140 x 140: same height, different width, and reading one into
the other is a shear rather than a failure. A check for that case was added and the mutation now
produces exactly one red.

Host suite: 245 checks across six binaries, 0 failures. Release ELF text 634,008, up 480 bytes.

---

## 1an. The licence screen was missing two of the four notices, and eating a URL

Read end to end on the console rather than in the editor, which is where both of these were.

### The Appropriate Legal Notices were incomplete

AGPL-3.0 defines "Appropriate Legal Notices" as four things: a copyright notice, a statement that
there is no warranty, a statement that the user may redistribute under this licence, and a way to
view the licence. An interactive program that displays such a notice has to display all four.

The credits screen had two. There was no copyright notice **anywhere in the project** -- not on
the screen, not in `docs/CREDITS.md`, and not in `LICENSE.md`, which is the unmodified AGPL text
with the "Copyright (C) <year> <name of author>" line still unfilled in the appendix. And there
was no warranty disclaimer at all. "You may use it, read it, change it and pass it on" covered the
third only loosely -- it does not say *under this licence* -- and the source URL covered the
fourth.

Both are now on the screen, verified by rendering it: copyright 2026 UsaRandom with portions to
the N64FlashcartMenu contributors, and the sections 15 and 16 disclaimer. The README carries the
same two lines, because the repository was as silent as the cartridge.

### A URL was being truncated, and it was the fork's source address

`ui_text_wrap()` uses `WRAP_WORD`. A URL contains no spaces, so it is one word, and rdpq's
response to a word that will not fit is to ellipsise. The credits screen was therefore drawing

    https://github.com/Polprzewodnikowy/N64Flashca...

with the tail missing -- on a screen whose entire job is to say where the source is, in the one
medium where a reader has to retype what they see. It fits for the 39-character MainMenu address
and does not for the 52-character upstream one, which is why it survived: the URL the AGPL cares
about most happened to be short enough.

`ui_text_wrap_url()` uses `WRAP_CHAR` and is used for `U` blocks only. The address now breaks
across two lines and is complete. Bullets were never affected -- "Name - url" has spaces in it, so
WRAP_WORD breaks after the dash and the URL lands on a line of its own.

### The attribution sweep

Every third-party thing in the cartridge, checked against the tree rather than against the
existing list. Two were missing, both because they arrive inside libdragon rather than being
chosen here and so were invisible to a reading of this project's own dependencies:

- **FatFs, by ChaN** (`libdragon/src/fatfs`), which is the filesystem that reads the card. Its
  licence asks that the copyright notice be retained; strictly that condition binds source
  redistribution and not a binary, but it is in the ROM and it is now named.
- **libcart, by devwizard64** (`libdragon/src/libcart`), the flashcart driver, called directly
  from `flashcart.c`. The copy inside libdragon carries only a URL and states no licence terms,
  which is recorded as the fact it is rather than guessed at.

Also corrected: **ares' licence was not stated.** The database is derived from it, so it now
carries "ISC, copyright (c) 2004-2025 ares team, Near et al" -- read out of
`lithium64/reference/ares/LICENSE`, not assumed.

Checked and already complete: all 36 icon authors (compared name by name against the corpus's
36 directories AND its own `license.txt` -- the two extra names in it are directories that
postdate that file); Firple with Fira Code and IBM Plex; the three Pixabay authors; the 28 CC0
MIDI files; libspng, miniz, picojpeg, midi64, svg64, acutest; all five emulator cores against the
core table in `cart_load.c`; libretro-database. `n64_keys.tsv` needs no attribution -- it is
joined out of `rom_info.c`, not out of a No-Intro DAT, which the module docstring says and which
is worth having checked.

**One residual.** CC BY 3.0 asks for the URI an author specifies as well as their name, and the
corpus supplies one for 20 of the 36. Those 20 are now on the credits screen. The other 16 gave
none, so nothing is owed.

### Odd language, and the one bit that was wrong

The report was that the on-ROM text read oddly. Four rewrites, of which one was a contradiction
rather than a matter of tone: the closing section said the project is "not affiliated with,
endorsed by, or connected to ... the upstream project it is forked from", 160 lines after saying
it is a fork of that project. It cannot be both, and the true half is the fork. It now disclaims
endorsement, which is what was meant, and says plainly that it IS derived from it.

The others: "the person who gave it to you owes you the source" reads as an accusation and is
imprecise about who the licence binds; "This program is free software. You may use it, read it,
change it and pass it on" omits that passing it on is *under the same licence*, which is the whole
condition; and "Some icons in the corpus are left out of this build. That is a trademark question"
raised a question on a console screen and then did not answer it.

### The demo art carries no lettering now

Separate report, same screen family. `tools/mkdemo.py` was printing each invented title across the
bottom of its own cover, an invented studio above it, and a system badge in a pill -- all set in
the menu's UI font at tile size, so a grid of them read as a screenshot of a screenshot. Gone,
along with the scrim, which was a black gradient over the bottom third of every picture whose only
purpose was to be a background for that text. `STUDIOS` and the per-title column that indexed it
went with it, as did `wrap()` and `fit_font()`.

The pictures are unchanged: `draw_card()` still seeds its RNG from the title, so every game draws
the scene it always drew.

What did change is the shape they are drawn at, and that is a separate correction. Every cover in
the demo tree was portrait, so the README led with five columns of 109 x 155 -- a layout no real
card produces. The one real corpus measured here is 28 covers and all 28 are landscape (1am), so
mkdemo now scrapes its systems the way art actually arrives: N64 and Master System landscape at
140 x 98, NES and SNES portrait at 109 x 155, the handhelds square at 140 x 140. All three shapes
therefore appear in one tree, which is also the only way a demo can show a mechanism whose whole
job is reading the shape off the file.

Eight README screenshots regenerated. The grid and the detail sheet moved twice -- once for the
lettering and once for the shapes -- the credits shot for the new notices, and the settings shot
because the clock in it reads a different day. The four with no box art on them did not move.

---

## 1ao. An instrument, before another mechanism

1ag spent a day on a hook that went green end to end under ares -- on *executed emitted code*, with
a working mutation control -- and did nothing on the M64, leaving four explanations that could not
be told apart: the scan matched the wrong sixteen bytes, it matched nothing and fell back to the
dead watch, it matched and the game does not route exceptions through the preamble we found, or the
writes landed at addresses wrong for that ROM revision. The finding recorded there is that the
strongest evidence this harness can produce was not sufficient. The finding recorded *here* is that
the missing thing was never a mechanism. It was a readout.

Everything from the launch screen to the game's first frame runs after the menu is gone: no
display, no filesystem, and on this cart no USB. So the engine reports on itself, through the only
output device guaranteed to exist and guaranteed to be aimed at something.

### The beacon

`VI_ORIGIN` always holds the address of whatever the **game** is currently displaying. The engine
reads it, converts to KSEG1, and writes 256 words of solid colour into the top of that buffer --
emitted at the exact instruction the `bne` lands on when the exception is not a watch, which is the
definition of "the engine ran". Ten instructions plus an unrolled run of stores, no knowledge of the
game, no channel.

| what you see | what it means |
|---|---|
| green bar | the engine is running, hooked through the game's own handler |
| red bar | the engine is running, hooked through the Datel watch -- which 1af measured as absent, so this would be news |
| no bar | the engine never executed |

The colour comes from a word the patcher writes and the engine only displays, so one launch reports
both *that* it ran and *which way* it hooked. Unrolled rather than looped because a loop needs a
third register to hold its limit, and the engine's contract with the game is $k0 and $k1 and
nothing else. Off unless `[menu] cheat_beacon = true` is in config.ini; it draws over the game.

### It found its first bug before hardware, which is the point

`hooktest.c` gained a third scenario: point `VI_ORIGIN` at the test's own megabyte of `.bss`, run
the emitted engine, and compare every word against `BEACON_GREEN`. It refused to run and said why
-- **the arena sits below the beacon's origin floor.**

The floor was one megabyte, on the reasoning that `VI_ORIGIN` that low is `VI_ORIGIN` unset. This
whole ROM is 824 KB, so its own `.bss` is under that line, and so is a small game's framebuffer. A
floor that suppresses the beacon is worse than no floor, because **a bar that never appears is
exactly what "the engine never ran" looks like** -- the instrument would have produced the answer
we already have and we would have believed it. 64 KB clears every real framebuffer and still
catches an unset origin.

That is the whole argument for building the instrument first, arriving one day early.

### Checked that it can fail

| mutation | predicted | got |
|---|---|---|
| drop the unrolled store run | the paint check | **1 red**, exactly that check |
| drop the KSEG1 conversion | the paint check | **the console wedged; no result at all** |

The second one is not a pass and is recorded as what it is. Writing the physical address as if it
were virtual faults into KUSEG, the fault re-enters the engine, and the loop never ends -- a
failure mode `hooktest.c`'s own header already warns about from a previous mutation. It proves the
conversion is load-bearing and it proves nothing about the check. A harness that hangs reports
nothing.

Scenario 3 also asserts what the beacon must *not* disturb: `0x80000180` unchanged, the watch
unarmed, and control still reaching the fake `__osException` with `$k0` exact. 21/21 under ares.

### The first hardware run, and why its answer was worthless

Deployed, `cheat_beacon = true`, Ocarina of Time with Infinite Rupees ticked. The log confirms the
instrument was armed and the mechanism reached:

```
engine   CIC 6105/7105 word[499]=01200008 ok -- will hook
hook     preamble scan first, Datel watch hook as fallback
watch    No watch, handler hook (wrote 000b6f91 read 000b6f91 control=1 trapped=0)
beacon   armed -- green bar = handler hook, red bar = watch fired, none = engine never ran
```

Reported: **no bar.**

That is not the finding it looks like, and the fault is the instrument's. 1 KB at offset zero is
**0.8 to 1.6 pixel rows at the very top of the framebuffer** -- a 320x240 16 bpp row is 640 bytes,
a 640x480 one is 1,280 -- and the top rows of an N64 framebuffer are exactly what overscan eats.

So the beacon committed the one sin an instrument may not: **a bar nobody can see and a bar that
was never drawn produced identical evidence.** The reasoning that put it there was "the top-left
corner is a place every framebuffer has", which is true and which never once asked whether that
place is on the screen.

Two changes, and the second matters more than the first.

**The bar moved to the middle and grew.** 64,000 bytes in and 8 KB long, which lands at row 100 of
240 at 320x240x16, row 50 of 480 at 640x480x16, and row 50 of 240 at 320x240x32 -- the middle of
every geometry a game plausibly uses, twelve rows deep on the commonest. `hooktest` now asserts
that nothing lands at offset zero, so the mistake cannot come back quietly.

That check took two tries to become a check. Mutating the offset back to 0 first **hung the
console** rather than reporting: the scenario used the arena's own base as its pretend
framebuffer, and arena[0..8] is where the fake game entry, the fake `__osException` and the
planted preamble live -- so a beacon aimed at offset zero painted over the fixture and the test
jumped into magenta. The pretend framebuffer now starts half a megabyte in, disjoint from the
fixture, and the mutation produces exactly one red naming the overscan. That is the third
harness-measures-itself failure in this feature and the second one that wedged instead of
reporting.

**The beacon got a positive control, which it should have had first.** `beacon_selftest()` does
what the emitted code does, in C, against the menu's own live framebuffer -- reads VI_ORIGIN,
converts to KSEG1, stores the colour at the beacon's own offset, reads it back uncached, restores
the original pixels -- and writes PAINTED or DID NOT PAINT into the launch log along with the raw
VI_ORIGIN value. Same shape as the `break` control in enginetest.c and for the same reason: until
something proves the instrument works on this console, "no bar" has two explanations and the
harness cannot say which.

This is the second time in one feature that the harness measured itself before it measured the
thing, and both times it was the harness that was wrong -- the one-megabyte floor (above) and now
the offset. Neither would have been visible in the answer. Both would have read as "the engine
never ran".

### What this does not do

It does not make cheats work, and it is not evidence that they will. It makes the next hardware
launch produce a fact instead of a shrug -- and only once the self-test line says PAINTED. The
mechanism it will be measuring is 1ag's, restored unchanged on the `cheats` branch, still believed
exactly as much as 1ag left it.

Sizes: text 635,704 against main's 634,232. Host suite 4,001 checks, no failures. hooktest 22/22, and the overscan mutation turns exactly one of them red.

---

## 1ap. The scan hits on Ocarina. Something after it does not.

The beacon's self-test came back **PAINTED** on the console -- VI_ORIGIN sane, an uncached store
64,000 bytes into it landing and reading back -- so the instrument works here, and the launch that
showed no band and left the rupees alone means what it says. **The engine never executed.**

That leaves two shapes, and the beacon cannot tell them apart because the colour it would have
used only appears if the engine runs: the scan missed and fell back to the dead watch, or the scan
hit and the hook was bypassed.

### Ask the ROM instead of the console

IPL3 copies ROM offset 0x1000 onwards verbatim to the entry address, so `ROM[0x1000 + k]` is
exactly the RDRAM word the patcher will look at. `tools/preamblescan.py` runs the same masked
pattern on a PC, over a whole shelf at once, in seconds. Over the 24 N64 ROMs on the reference
card:

| | ROMs |
|---|---:|
| **real** -- target is KSEG0 and exactly +16, which is how libultra links it | 15 |
| **odd** -- target is KSEG0 but not adjacent | 5 |
| **BOGUS** -- target is not an address at all | 2 |
| **MISS** -- nothing preamble-shaped in the first megabyte | 2 |

**Ocarina of Time is `real`**: CIC 6105, preamble at `0x800025f0`, `__osException` at `0x80002600`,
exactly sixteen bytes on. The scan is not missing. So it is the second shape -- the patch lands and
the game does not route exceptions through the bytes we patched -- and that is a different and
much more interesting problem than the one we thought we had.

### The tool was wrong first, in a way that looked like the games being wrong

Five ROMs first read as preambles whose target sits almost exactly one megabyte away. That is not
a game: it is CIC 6103, which loads a megabyte below the header's entry point, and CIC 6106, which
loads two. `rom_info.c`'s `fix_boot_address()` is where the console applies the same two numbers,
and the tool had not. CIC identified by CRC32 of the 4,032-byte IPL3 rather than by the console's
seeded checksum -- a table lookup against a hundred lines of 64-bit mixing, and they agree on
everything here.

### And then it found a live bug

Two of the twenty-four match a run of data whose reconstructed target is `0x100071e0` (Conker's Bad
Fur Day) and `0x700101a0` (GoldenEye 007). Neither is RDRAM. Neither is a preamble.

**The patcher takes the first match and rewrites two words of live game code at it.** On those two
ROMs it was about to corrupt something arbitrary and hand the result to the game -- one in twelve,
on the only real shelf of ROMs this project has, and neither would have been diagnosable from the
symptom. The pattern fixes eight of sixteen bytes and half the rest, which is simply not enough
over a megabyte.

The emitted scan now checks that `%hi` of the target is `0x80..`, which covers every KSEG0 address
an 8 MB machine has and rejects both. It costs nothing in the common case: the check sits after
the four word compares, so a miss branches away before reaching it and the full-window scan still
measures 53.3 ms. `hooktest` gained a scenario that plants Conker's target and requires the scan
to walk past it; mutating the check away turns exactly two of its checks red.

### Where the pattern is pinned, and the two homes that did not work

The tool carries the four words as literals because Python cannot include `vr4300_asm.h`, and a
tool that agrees with itself and disagrees with the console is worse than no tool.

A host test was written first and cannot work: `vr4300_asm.h` assembles through a **bitfield
union**, and bitfield allocation order follows the target's endianness -- on a little-endian host
`I_JR(REG_K0)` is `0x20000680`, not `0x03400008`. The macros only mean anything compiled for MIPS.
A `_Static_assert` cannot do it either, because a compound literal is not a constant expression.
So the pin is a runtime check in `hooktest`, which is a MIPS build that already exercises the
scan. 32/32 under ares.

### What is still open

Why Ocarina does not route through the preamble we patched. The likeliest answer is that its
runtime libultra is not the copy IPL3 loaded -- OoT's `code` segment is loaded and decompressed
later from ROM, and if `osInitialize` runs from that copy then the boot-segment preamble we
rewrote is never used. Nothing here can see which, because the branch the patcher took happens
after the menu is gone and the beacon that would report it needs the engine to run first.

---

## 1aq. The patcher reports on itself, because everything else needs the game's cooperation

1ap left four stories and one symptom. "The engine never ran" is equally consistent with the
patcher never executing, with the scan missing, with the scan hitting and the game ignoring the
bytes, and with the engine being overwritten before it could run. Nothing that reports from
*inside* the game can separate them, because all four look identical from there.

The patcher can, and it has one advantage the engine does not: **it owns the machine.** Nothing
else is running, nothing will touch the video interface until the game's own `osViSetMode`, and
there are megabytes of RDRAM nobody has a use for. So it paints a 320x240 framebuffer of its own,
programs the VI to display it, holds it, and only then jumps into the game.

    green screen   the scan found libultra's handler and rewrote it
    red screen     the scan came up empty and the Datel watch was armed instead
    no flash       the patcher never executed at all

Plus twenty-four blocks across the middle: bits 23..0 of `(where the scan stopped - game entry)`,
white for one, black for zero, six hex digits read left to right. On a hit that is where in the
loaded megabyte the preamble was, checkable against `preamblescan.py`, which predicts `0x0021f0`
for Ocarina of Time. On a miss the scan pointer has walked the whole window, so it reads
`0x100000` -- the number that means "looked everywhere".

The colour comes from the same `BEACON_STATE_ADDRESS` word the found/notfound paths already
stamped for the in-game bar, so the two instruments cost one branch between them and ride the same
`[menu] cheat_beacon` switch.

### The first idea was wrong, and hardware would not have said so

The plan recorded at the end of 1ap was to paint into `VI_ORIGIN`'s existing buffer -- the menu's
last frame. That would have shown nothing, and not because the patcher failed: `boot.c` writes
`VI->H_LIMITS = 0` on its way past, which blanks the output. An instrument aimed at a blanked
display is the 1 KB-at-offset-zero mistake again with different numbers. Programming all thirteen
timing registers is thirty more emitted instructions and it removes the dependency entirely.

The hold is **four seconds**, not one. The VI was blanked a moment earlier, and an HDMI sink that
has just lost sync can take a second or more to re-acquire; a flash shorter than the resync is a
flash nobody sees.

### Two real bugs, both caught before hardware

**A 13-pixel block pitch is 26 bytes, and the picture is painted with word stores.** The second
block's first `sw` took a misaligned-write exception -- PC `807001B0`, address `A016EC32` -- and
wedged the harness. ares caught it, which is the only place emitted code of this kind still can
be caught. The geometry is 16/8/12 now and six `_Static_assert`s make the alignment, the overlap
and the two frame bounds build failures.

**The framebuffer is dead menu heap, so the CPU may hold dirty lines over it.** One evicted after
the paint writes old heap back over the picture, at whatever moment the game's first allocation
happens to trigger it. ares says so directly -- *"uncached writing to RDRAM address ... which is
cached"*. The flash now invalidates the region (9,600 `cache` ops) before painting it. Invalidate
rather than write back: the contents are precisely what we do not want.

### What the harness proves, and what it cannot

`hooktest` gained a fifth scenario: point the flash at a scratch buffer 640 KB into the arena,
execute the emitted code, and compare the result byte for byte -- the fill, the VI registers, the
twenty-four blocks, and the rows either side of the band. Every pixel of the band rows is
accounted for, so a block loop that overran its pitch is caught as readily as one that never ran.
**42/42 under ares.**

Both mutations report exactly:

| mutation | red |
|---|---|
| shift the value by 31 rather than 32 bits before the block loop | 2 -- both block checks |
| drop the load from the cascade copy | 1 -- only "carries them down every row" |

What it cannot prove is that an FPGA console accepts these thirteen timing values, or that its
HDMI resyncs inside four seconds. Both are one launch away, and both fail visibly rather than
silently.

### Also

`beacon_selftest()` was reporting the wrong readback: the `read %08lx` argument was evaluated
after the restore loop, so it printed the *original* pixel and called it the value read back. The
verdict beside it was computed before the restore and was always right, which is the worse
combination -- a correct answer with fabricated evidence next to it.

### Hardware said "no flash", and that answer could not be read

Ocarina of Time, Infinite Rupees ticked, `cheat_beacon = true`, self-test PAINTED. **No flash, and
no change to the rupees.**

Which settles nothing, because "no flash" is what a patcher that never executed looks like *and*
what an HDMI sink that never re-acquired sync looks like. `boot.c` writes `VI->H_LIMITS = 0` on its
way past -- the display is already blanked when the patcher starts -- so the flash was asking a
just-desynced sink to lock onto thirteen freshly written timing registers inside four seconds. A
one-channel instrument whose silence has two readings is not an instrument, and this is the second
time the same mistake has been made about the same launch (1ao, the bar in overscan).

---

## 1ar. Two channels, and one of them cannot be blinded

The fix is in two halves, and the second half is the one that matters.

**Stop programming the VI.** `boot.c` now skips the `H_LIMITS = 0` write while the flash is armed,
so the menu's own 640x480 mode is still running and still locked when the patcher starts. The
flash paints a 640x480 frame at `0x00600000` and writes **one** register, `VI_ORIGIN`. Nothing
resyncs; the picture changes between one field and the next. The game reprograms the VI from
scratch regardless, so the boot pays nothing for it.

**Add a channel that needs no display at all.** The patcher holds for ten seconds before jumping
into the game, and the miss path holds ten more first. The wall clock between the menu going away
and the game's first frame is then the answer, and it depends on nothing but the CPU clock:

| time to the game's first frame | what ran |
|---|---|
| ~2 s, the usual | the patcher never executed |
| ~10 s | the scan found libultra's handler and rewrote it |
| ~20 s | the scan came up empty; the Datel watch was armed |

Ten seconds rather than three because the reading has to survive being done by eye. "The same as
always" against "ages" against "twice that again" needs no stopwatch.

### The harness measured the wrong thing and said green

`hooktest` gained scenario 5 (hit path) and scenario 6 (miss path, bogus target), both executing
the emitted flash against a malloc'd frame and comparing it byte for byte -- fill, `VI_ORIGIN`,
the twenty-four blocks, the rows either side of the band, and the hold.

The hold check was written asking for a 1 ms hold and requiring 1 ms elapsed. **It passed with the
delay loop mutated to never loop**, because `run_patcher` also contains the 53 ms scan and that
alone cleared the bar. Exactly the failure the house rule exists for: a green result from a
harness measuring something else. The hold is a quarter second now, so it dominates the scan, and
the hit path is bounded at both ends -- at least 200 ms, and under 400 ms, because a hit that paid
the miss path's second hold would collapse the two readings the whole channel is built on.

**47/47 under ares.** Four mutations, all reporting exactly:

| mutation | red |
|---|---|
| shift the value by 31 rather than 32 bits before the block loop | 2 -- both block checks |
| drop the load from the cascade copy | 1 -- "carries them down every row" |
| delay loop never loops | 2 -- both hold checks |
| miss path skips its extra hold | 1 -- "waited out both holds" |

One harness bug of my own on the way: `band_row_ok()` hardcoded green as the gap colour between
blocks, so the miss path's red frame failed the block check for a reason that had nothing to do
with the blocks.

---

## 1as. Patch the cartridge, not the console

Three instruments, three hardware runs, nothing readable. The bar needed the engine to run; the
flash needed a display that had just been blanked; the timing needed the patcher to execute. Every
one of them lived in MIPS assembled word by word into RDRAM and executed microseconds after the
menu, the filesystem and the screen were gone.

The user's question was the right one: *can we just patch the ROM before booting it?*

**Yes, and the plumbing was already there.** `sc64_init()` sets `CFG_ID_ROM_WRITE_ENABLE` and
nothing clears it until `flashcart_deinit()`, which is the last statement of `app_deinit()`. So
cartridge SDRAM is CPU-writable for the entire life of the menu, including every line of
`do_load()` after the ROM has been streamed onto it. `disk_set_thb_mapping()` has been doing
single-word `io_write`s into ROM space all along.

So the hook moves into `src/menu/rompatch.c`, where there is a log to write to, a screen to fail
on, and -- the part that matters -- a **read-back**. Everything that was unobservable is now a
line in `launch.log` written before the console boots.

### The checksum is the whole difficulty

Every retail IPL3 checksums `ROM[0x1000..0x101000]` against CRC1/CRC2 in the header. Change a word
of the boot segment without fixing those and the console stops dead: black screen, no logo, no
sound, no way to tell that from any other failure.

The algorithm is the bootcode's, has no specification, and is six accumulators seeded per CIC with
three different final mixes. A plausible-looking implementation that is subtly wrong produces
exactly that black screen. So it is checked three ways:

1. **Against real games.** `tools/romcrc.py` over the 24 N64 ROMs on the reference card:
   **23 reproduce their stored checksum exactly**, across CIC 6101, 6102, 6103 and 6105. The
   twenty-fourth is Pokemon Stadium, whose CRC1 matches and whose CRC2 does not -- a header that
   disagreed with its own contents before anyone here touched it.
2. **C against Python.** `tools/hosttest/test_romcrc.c` pins `src/menu/romcrc.c` to
   `tools/romcrc.py` over a synthetic LCG image, 17 checks. Arbitrary words rather than a real ROM
   because a real boot segment is mostly zeros and short constants -- the carry counter `t4` never
   increments on sparse data.
3. **Per launch, at runtime.** `romcrc_verify()` recomputes the checksum of the *unmodified*
   cartridge and compares it with the header. **If they disagree, nothing is patched.** That is
   what makes this safe to ship without knowing every ROM in the world: Pokemon Stadium simply
   does not get cheats, and says so in the log.

### The finder is the dangerous part, and ares cannot reach it

`rompatch_find()` picks two words of somebody's game to overwrite, and under ares
`flashcart_is_dummy()` is true so the entire path is skipped. It is therefore split into
`src/menu/rompatch_find.c`, free of libdragon, and `tools/hosttest/test_rompatch.c` runs it
natively -- 26 checks against images built to contain the things that have actually gone wrong:
both of the exact bogus targets (`0x100071e0`, `0x700101a0`), a KSEG0 target that is not +16, a
target above RDRAM, a bogus match followed by a real one, all four straddles of a chunk boundary,
and the last four words of the window.

Run over the reference card's 24 ROMs, the C agrees with `tools/preamblescan.py` and finds
**18 sites**. Ocarina of Time: `rom+0031f0`, RAM `0x800025f0`, `__osException` `0x80002600` --
the address the Python predicted, from the other side.

It finds three more than the Python reported because it keeps scanning past a rejected candidate
rather than stopping at the first thing shaped like a match. Banjo-Kazooie, Mario Party 3 and Star
Fox 64 each have one bogus candidate ahead of a real one.

Mutating the address test away turns 8 of the 26 checks red.

### What this does and does not fix

It fixes the **hook**. The engine body still lives at `0x807C5C00`, staged by `cheats_emit()` and
preserved across IPL3 by boot.c's `skip_rdram_reset` -- untouched, and still unverified. If the
bar does not appear with the hook demonstrably in the cartridge, then it is the engine half that
is wrong, which is a far smaller question than the one this replaced.

With the ROM hook verified the emitted patcher stops scanning entirely: no loop, no branch, no
watch, and the engine's tail is built with the two displaced words already in it rather than
back-patched at runtime. The patcher becomes a memcpy.

### Two harness bugs, both mine

`band_row_ok()` hardcoded green as the gap colour between blocks, so the miss path's red frame
failed the block check for a reason unrelated to blocks.

And `hooktest`'s new scenarios called `data_cache_hit_invalidate` on a `malloc` pointer.
libdragon asserts on anything not 16-byte aligned; malloc's 8 happened to land on 16 until two new
files moved the heap, and then the harness died inside an assert instead of reporting. **47/47**
once it was `memalign`.

---

## 1at. Three hardware runs measured a build nobody meant to ship

The user, unprompted: *"there is a small artifact. when console starts and mainmenu boots, before
the menu launch screen, there is a green and red screen that flash. a bunch of lines on them
across the middle."*

That is `src/dev/hooktest.c` scenarios 5 and 6 -- the handoff flash painting a full 640x480 frame
green, then red, with the twenty-four-block band across the middle -- executing on the real
display at boot. **The card was carrying a DEV_HARNESS build.**

`tools/regress.sh:95` is `make FIXTURE=1 DEV_HARNESS=1 INPUT_SCRIPT="$script" ... sc64`, and it
writes to `output/sc64menu.n64` -- the same path a release build writes to. Run the suite, then
copy `output/sc64menu.n64` to the card, and the card gets hooktest running at boot, the synthetic
fixture packed instead of the real assets, and a compiled-in input script driving the pad. Release
is 8,306,688 bytes and carries no hooktest strings; the harness build is 4,538,368 and carries 65.

Three consecutive deploys did exactly that, because each ran the suite and then copied, in that
order, in the same breath. **The three runs are void:**

| build | intended | actually shipped | verdict |
|---|---|---|---|
| `0a80ff0a` | canary vs pass-through vs full | DEV_HARNESS | void |
| `5879df88` | canary with and without checksum | DEV_HARNESS | void |
| `84c47bde` | bootcode-aware checksum, cartridge dump | DEV_HARNESS | void |

So the canary -- the control this investigation most needed, the one that decides whether the
cartridge may be written at all -- **has never been run.** Everything concluded from "all three
black" is withdrawn.

Two earlier results stand, both release builds: `6f251f7c` and `e0e49f99`, the full hook with the
engine placed by the patcher and then placed from C. Both black.

### What made it invisible

Nothing about a harness build announces itself on hardware. The fixture is packed into the DFS,
but on a real cart `storage_prefix` is `sd:/` and the menu reads the card as usual, so the library
looked right. The input script ran its hundred frames and stopped. hooktest restored every vector
and register it touched. The only tell was two frames of colour before the menu drew, which is
exactly the kind of thing you stop seeing after the fortieth boot -- and which somebody who had
seen the console forty fewer times noticed immediately.

`regress.sh`'s own docstring already warns that it throws away a hand-built ROM. It was read,
quoted in this file, and walked into anyway. A warning that has been read and not heeded is
evidence for a check, not for a louder warning.

### The check

`tools/deploy.sh` builds release unconditionally -- whatever was in `output/` is not trusted --
then refuses to copy anything carrying a hooktest string or smaller than 6 MB, then copies and
ejects. Deploys go through it now.

---

## 1au. Going through the database line by line, and what was in it

Every type byte in the corpus, counted over the 393 games a key table can reach, and asked of each
one: can this engine emit it, and if not, is that because of the cheat or because of us?

| type | lines | what it is | now |
|------|-------|------------|-----|
| `80 81 A0 A1` | 211,402 | 8/16-bit write, cached and uncached | emitted, 3 words |
| `D0`-`D3` | 23,594 | conditionals | emitted, 4 words each, **stacked** |
| `50` | 888 | repeater | emitted as a **loop**, 12 words |
| `F0 F1` | 726 | write once at boot | emitted as ordinary writes |
| `EE` | 23 | disable Expansion Pak | emitted, 4 words |
| `88 89 D8 D9` | 2,804 | the same, but only while the GS button is held | refused |
| `CC DE FF 00` | 29 | specials Datel's own engine emits nothing for | refused |

Four of those rows are new. The measured result at database level, after the merge that collapses
regional variants: **42,366 of 53,653 groups carried, up from 42,196**, and the composition of the
change matters more than the total -- 701 groups newly carried, 531 removed because they would
hang the console.

### The repeater had to stop being an unroll

`cheats_install()` emits `3 * count` instructions for a `50`, and the corpus goes to `count = 254`:
762 words for one cheat. Against that, the measured padding capacity of the fifteen-ROM shelf --
the space the engine actually has, `--list-gaps` over each image:

| game | usable words | game | usable words |
|------|-----|------|-----|
| Star Wars: Shadows of the Empire | 11 | Donkey Kong 64 | 133 |
| GoldenEye 007 | 17 | Mario Kart 64 | 137 |
| Banjo-Kazooie | 20 | AeroFighters Assault | 151 |
| 1080 Snowboarding | 28 | Pokemon Stadium | 238 |
| Ocarina of Time | 30 | Harvest Moon 64 | 279 |
| Tony Hawk's Pro Skater 2 | 35 | Mario Party | 825 |

(Guard words deducted, which is what `tools/cheatshelf.py` reports and what placement actually
has. An earlier draft of this table gave the raw run lengths and called them usable, which
overstated every row by 4 words per run.)

Half the shelf is under 45 words. So an unrolled repeater is not expensive, it is impossible, and
a loop is the only form that fits. **This is also the number that matters more than
`ENGINE_MAX_WORDS`**, which is 128: raising it to 256 buys 77 groups out of 177,579, while the
game's own padding decides everything on the left-hand column. The cap bounds the stack arrays; it
does not express a policy.

A loop needs three live values -- address, value, counter -- and an exception handler has two
registers it may touch. The third is borrowed: `$t0` is parked in one word of reserved padding for
eleven instructions and put back. Safe here and nowhere else, because the cell is a word we held
back out of a run that already passed the padding test, it is KSEG0 so the store cannot take a TLB
miss, and EXL is set for the whole loop so nothing can nest in and find it occupied.

### 1,964 codes in the corpus would not have misbehaved, they would have hung

`sh` and `lhu` off an odd address raise an Address Error. This engine runs *at* the general
exception vector with EXL already set, and a nested exception there does not update EPC: it
vectors straight back to 0x80000180, into the engine, into the same store. The console locks and
only the power switch gets it back.

Measured: **1,964 of 149,687 16-bit writes name an odd address**, in 531 database groups.
AeroGauge's "Name 1" is `8108CD69 8108CD6A 8108CD6B` -- three consecutive bytes typed as 16-bit
writes. Every engine this project has shipped carried them. They are refused now, whole groups at
a time, and the report says which and why.

Deliberately not "fixed" by splitting an unaligned halfword into two `sb`: for AeroGauge that
would write two overlapping bytes per line and produce garbage. The codes are wrong at the source
and inventing a reading for them is worse than declining.

### The 20,154 dangling conditionals are not cheats

The largest single drop, and it stays a drop. `20,142 of 20,154` are named "Activator", "Dual
Activator", "Joker Code" -- a `D0` line on its own, listed separately so a player can combine it
with a code of their choice. TWINE alone carries 96 of them. Applying one on its own does nothing
by construction, and pairing it with whatever group happens to sit next in the list is exactly the
bug the group model exists to prevent (2.2).

### What is left

Of the 43,986 database groups that are actually cheats rather than fragments or no-ops,
**42,366 are carried -- 96.3%**. The remainder is 988 GS-button groups, 543 that would hang, 88
over the engine ceiling, and one repeater that steps a 16-bit write by an odd byte. The GS-button
set is the only one that is a capability gap rather than a refusal, and it is not closable: there
is no button, and no address in RDRAM stands in for one.

### How it is checked

The C suite pins what each line costs and where each conditional branches to, including
`rompatch_test_branch()`, which is out in `rompatch_find.c` for no reason other than that the host
suite can reach it there. Nothing in it executes an instruction.

`tools/rompatch.py --self-test` does: it emits the same shapes, runs them through a small VR4300
interpreter, and checks the bytes written against the reference expansion in `cheats.c`. 87 checks.
Four mutations confirmed red -- shifting the branch by one word either way, starting the loop
counter at `count` instead of `count - 1`, and inverting `beq`/`bne`.

**One of those mutations initially survived**, and the reason is worth keeping: a branch that
overshoots by one word lands on a harmless instruction, so a test that only asks "did the guarded
store happen" cannot see it. The fix is a sentinel write immediately after the guarded atom, *in a
different 64 KB page* -- with both in `0x8011xxxx`, 11 of 12 cases still passed, because the delay
slot the overshooting branch ran on the way past was the guarded write's own `lui`, which loaded
the high half the sentinel happened to need.

**Not verified: any of this running on a console.** ares cannot be screenshotted while the machine
is in use -- two attempts captured a browser window -- and no game has been booted with a repeater
or a stacked conditional in its engine.

---

## 1av. Going through the shelf game by game, and the two lookups that were wrong

Prompted by a report that Star Wars Episode I: Racer's "Infinite Money" did nothing. It did
exactly what it was told; it was told the wrong address.

### The region wildcard put four binaries in one row

`rom_info.c`'s rows are `MATCH_ID`, which matches any region on purpose -- that is the right
granularity for a save type and the wrong one for a cheat address. `mkcheatkeys.py` carried the
wildcard through, so 1,029 of the 1,043 key rows ended in `?` and every regional corpus file for a
game merged into one row. Racer's USA, Europe, Japan and (E) files all became `NEP?`.

The result on a USA cartridge was two entries in the list:

| name | address | from |
|---|---|---|
| Infinite Money | `8111CB1A` / `8111CB18` | Racer (Europe) |
| Infinite Trugets (Money) | `81113E7A` / `81113E78` | Racer (USA) |

0x8CA0 apart. The obvious-sounding one is the European one.

Racer got off lightly, because the two names differ. **183 of the 394 keys are fed by more than
one region, and the merge deduplicates by name keeping the first file alphabetically -- so `(E)`,
`(Europe)` and `(F)` beat `(USA)`. Measured: 11,162 USA cheats across 136 games were being
replaced outright by a same-named foreign one.** Tetrisphere lost 7,183, Turok 640, GoldenEye 390.

Fixed by deriving the header's region byte from the corpus filename and emitting a *second*,
narrower row -- `NEPE` from the USA files alone -- beside the wildcard one, which is left holding
everything merged exactly as before. `find_row()` already ranked an exact four-character code above
a three-character wildcard, so this is strictly better or equal: a release whose region byte the
filename did not predict (a German build is `D`, some PAL builds are `X`) simply misses the narrow
row and lands on the fallback. 802 rows instead of 392, and 3.37 MB instead of 1.75.

### "+16" was a linker-order assumption, and it is wrong three times in fifteen

`__osExceptionPreamble` is four words that jump to `__osException`, and the finder identified it by
the target sitting exactly sixteen bytes on. Measured over the shelf:

| game | candidates | what the old rule did |
|---|---|---|
| 1080 Snowboarding | 1, at +212 | refused; no cheat could ever run |
| Harvest Moon 64 | 1, at +212 | refused; same |
| Mario Party 3 | 3: +212, +16, bogus | **took the +16 one, which is a dispatcher stub** |
| Banjo-Kazooie | 2: +32, +16 | took +16, correctly |
| the other eleven | 1, at +16 | correct |

+212 is the same number in two unrelated games, so it is a build variant and not a coincidence:
disassembly shows a second preamble-shaped stub and an exception dispatcher linked in between.
Mario Party 3 is the bad one -- its +16 target begins `sw $k0,-16($sp)` / `sw $k1,-8($sp)` /
`mfc0 $k0,$13`, a dispatcher, not `__osException`, and not what `osInitialize` copies to
0x80000180. Its cheats were being written into a stub nothing executes.

So the target is identified by what it *is*. `rompatch_is_exception()` reads the four words it
points at and checks them against libultra's prologue -- `lui $k0` / `addiu $k0` / `sd $at,0x20($k0)`
/ `mfc0 $k1,$12`, two of the four exact. It matched the target of all fourteen real candidates
across the shelf and none of the three false ones. Distance survives only as a tie-break, ranked
2 (`__osException` at +16) > 1 (`__osException` anywhere) > 0 (+16, unidentified), so no game that
already worked can change and Banjo-Kazooie's two back-to-back preambles still resolve the way
they always have.

Shelf result: **13 of 15 games can hook, up from 10.**

### The two that still cannot, and why

**GoldenEye 007** has exactly one preamble in twelve megabytes, at rom+0x010D90, and
`__osException` sits at rom+0x010DA0 -- sixteen bytes on, exactly where it should be. But the
preamble reads `lui $k0, 0x7001` where the handler is at `0x800101A0`: it names an address
0x10000000 below its own exception handler. The word is inside the checksummed window and the
header CRC agrees with it, so it is the intended content of the ROM and not a bad dump. Nothing
here explains it and the honest move is to change nothing: replaying those two words in the engine
tail would `jr` to 0x700101A0. 2,268 cheats unreachable. **Open.**

**Star Wars: Shadows of the Empire** has no preamble-shaped block and no `__osException` prologue
anywhere in the file, so it is either not using libultra's exception path or keeps it compressed.
It also has 11 words of padding, which would hold two cheats. Refused. **Open.**

**Pokemon Stadium** still fails the CRC gate, as it has since that gate existed: its header
disagreed with its own contents before this project touched it.

### What the audit is

`tools/cheatshelf.py` -- one line per ROM: header key, which database row it reaches and how, how
many of that row's cheats its own padding could hold, and where the engine would attach or why it
cannot. It imports `cheatlook.py` (which transcribes `find_row`), `preamblescan` and `rompatch`
rather than reimplementing any of them, because a second opinion that shares no code with the
console is worth having and one that quietly differs is worth nothing.

`tools/cheatlook.py` answers the narrower question -- what would this game code be offered -- and
is what turned "Infinite Money does nothing" into a diagnosis in one command.

### Two of the mutation tests had not been mutating anything

Found while adding more of them. `--mutate` prints `MUTATION SURVIVED` when the mutant binary
passes, and it printed exactly the same thing when the `sed` matched nothing at all -- so a
mutation that has rotted looks identical to a check that is missing, and both look like a line of
output nobody reads twice.

Two were dead. The preamble one targeted a line this session rewrote. The 6103 one was
`s/^        case CIC_x103:$/.../`, and **`.gitattributes` checks this tree out CRLF**, so the line
really ends `:\r` and `$` can never match it -- that one had been a no-op for as long as the tree
has been CRLF, quietly asserting that the ROM checksum suite pins the per-CIC final mix without
ever testing it. It does, as it turns out: 21 checks, 1 failure once the pattern fires.

`run.sh` now has a `mutated` helper that compares the mutant against the original and says
`MUTATION DID NOT APPLY` in its own words. Every mutation goes through it.

The third finding is smaller and worth stating anyway: the bogus-target rejection in
`rompatch_find()` turned out to have *three* independent guards, and no single-line mutation could
expose it because two always remained. That is defence in depth and it stays, but the mutation now
removes all three at once, because a survival report that means "the other guards held" is not
what the word survived is for.

### One more thing the numbers say, not yet acted on

**63.5% of carryable groups never reach the database**, and not for any reason above: 99,618 of
156,807 are dropped because another group in the same file has the same name. They are not
duplicates. GoldenEye has 36 cheats called "Clip Size", one per weapon; Turok has 160 called
"Pur-Lin #1", one per enemy slot; Racer has 35 called "Anti Skid". The corpus genuinely fails to
name them apart, and the dedup that exists to collapse the same cheat arriving from three regional
files eats them too.

Not fixed here, because none of the three options is obviously right: keeping them all makes a
menu with 160 identical-looking rows, merging them into one group is wrong wherever the copies are
alternatives rather than a set (GoldenEye's "Number Of Shots Fired Modifier" has 78 copies, two of
which write the same address), and numbering them is honest but unusable. Recorded as the largest
remaining loss.

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

### 2.6 How far the cheat path sits from upstream, measured

Asked directly after 1ag was reverted: are we running upstream's cheat mechanism? **Yes.** Diffed
against the fork point `6407ab15`, and the result is narrow enough to state exhaustively.

**Byte-identical to upstream:** `src/boot/boot.c`, `src/boot/reboot.S`, `src/boot/cic.c`,
`src/boot/cic.h`, `src/boot/vr4300_asm.h`. Every instruction the engine and patcher emit, the
watch arming, the IPL3 patch offsets, the x106 descrambling, the handoff registers — untouched.

**Three deltas in `src/boot/cheats.c`, and nothing else in the boot layer:**

1. **Bound checks** (`ENGINE_WORST_ENTRY_WORDS`, `PATCHER_MAX_WORDS`). Upstream's `*engine_p++`
   had no bound: a `0x50` repeater emits 3 instructions per iteration with a count up to 255, so
   a handful ran off the end and wrote through whatever followed (2.3). Refuses to install rather
   than corrupting memory. Cannot change behaviour for a list that fits — and every list here fits.
2. **`cheats_ipl3_patch_offset()` / `cheats_ipl3_layout_ok()` split out of the static
   `cheats_patch_ipl3()`.** Pure refactor, same values, exposed so `cheatcheck.c` can answer "will
   this hook?" while the display still exists.
3. **The layout-mismatch return value.** Upstream returns `false` when the IPL3 word is not
   `jr $t1` — and `false` is what its caller reads as *success*, so an unrecognised IPL3 was
   reported as patched, the jump was never written, and `cheats_install()` went on to return true
   and have `boot.c` set `skip_rdram_reset` on the strength of it. Engine assembled, never hooked,
   cheats silently dead. We return `true` (abort). Of 23 retail ROMs measured with
   `test_cheatinstall.c`, **22 take the identical path either way**; only Star Fox 64 (CIC 6101)
   differs, and there upstream's behaviour is the broken one.

**The feed is equivalent, not identical.** Upstream's `generate_enabled_cheats_array()` walks a
flat array of independently-toggleable lines; `cheatdb_emit()` walks named groups and writes each
enabled group's lines contiguously. Both produce the same thing the engine reads: `{address,
value}` pairs with the full raw 32-bit word (type byte intact) followed by two zero words. The
group model exists because per-line toggling separates a `D0` from the write it guards (2.2). For
any selection where whole groups are enabled — which is the only selection our UI can express —
the emitted array is what upstream would emit.

**So a normal retail game takes upstream's code path exactly.** If cheats do not work here, they
would not have worked on upstream's menu either, on this console.

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
