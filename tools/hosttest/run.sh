#!/bin/sh
# Compile the portable parts of the cache layer natively and round-trip them.
#
# The write half of src/library/cache.c cannot run under ares -- the storage prefix there is the
# ROM's own read-only DFS, so cache_writable() is false and cache_store() returns before touching
# any serialisation code. This runs the real file on the host instead, against real files.
#
#     tools/hosttest/run.sh              # run the tests
#     tools/hosttest/run.sh --mutate     # also prove the suite can fail
#
# --mutate is the house rule made executable: it breaks the CRC seed in a copy of cache.c and
# checks that the suite notices. Note which tests survive that mutation -- every round-trip check
# still passes, because writer and reader share the broken function. That is why the suite also
# pins CRC32 against the published IEEE check value for "123456789", which is the only assertion
# in it that a self-consistent mistake cannot satisfy.

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

CC=${CC:-cc}
OUT=build/hosttest
mkdir -p "$OUT"

CFLAGS="-std=c11 -Wall -Wextra -Werror -Itools/hosttest/shim -Isrc -Isrc/library"

# Did the mutation actually change anything? Call it right after every sed, before compiling.
#
# "MUTATION SURVIVED" and "the sed matched nothing" print the same green-looking nothing, and this
# harness has been reporting the second as the first. Two of its mutations were no-ops when this
# was written: one because the line it targeted had been rewritten, and one because .gitattributes
# checks the tree out CRLF, so a pattern anchored with `$` can never match a line that really ends
# `:\r`. A mutation test that cannot mutate is worse than no mutation test, because it is a green
# tick over an unasked question.
mutated () {   # mutated MUTANT ORIGINAL "what it was meant to break"
    if cmp -s "$1" "$2"; then
        echo "  MUTATION DID NOT APPLY -- the pattern matched nothing in $2, so this proves nothing"
        echo "  (was meant to break: $3)"
        return 1
    fi
    return 0
}

echo "== cache round trip"
rm -rf "$OUT/dir"
# paths.c comes along because cache_init() calls menu_path(). It did not always: the /menu ->
# /mainmenu rename moved that call into cache.c and nothing here was updated, so this suite
# stopped LINKING and every later run reported nothing at all. A suite that cannot build is worse
# than one that fails, because the failure is at the top of a log nobody reads to the bottom of.
$CC $CFLAGS tools/hosttest/test_cache.c src/library/cache.c src/menu/paths.c tools/hosttest/shim/fs_probe.c -o "$OUT/test_cache"
TESTDIR="$OUT/dir" "$OUT/test_cache" 2>/dev/null

echo
echo "== thumbnail atlas round trip"
rm -rf "$OUT/thumbdir" "$OUT/thumbdir-ro"
$CC $CFLAGS tools/hosttest/test_thumbstore.c src/library/cache.c src/library/thumbstore.c \
    src/menu/paths.c tools/hosttest/shim/fs_probe.c -o "$OUT/test_thumbstore"
TESTDIR="$OUT/thumbdir" "$OUT/test_thumbstore" 2>/dev/null

echo
echo "== box art: reading a size, and snapping it to a shape"
rm -rf "$OUT/boxartdir"
mkdir -p "$OUT/boxartdir"
# boxart.c pulls in the ini parser and paths because a card may override the fallback table; the
# probe pulls in nothing, which is the whole reason it is its own file. library.c is NOT linked --
# library_tab_label() is the only symbol boxart.c wants from it and the stub below is a tenth of
# the dependency the real one drags in.
$CC $CFLAGS tools/hosttest/test_boxart.c src/library/boxart.c src/menu/image_probe.c \
    src/menu/ini_parser.c src/menu/paths.c tools/hosttest/shim/tab_label.c \
    tools/hosttest/shim/fs_probe.c -o "$OUT/test_boxart"
TESTDIR="$OUT/boxartdir" "$OUT/test_boxart" 2>/dev/null

echo
echo "== the library index: round trip, and what invalidates it"
rm -rf "$OUT/idxdir"
mkdir -p "$OUT/idxdir"
# libindex.c had no coverage of any kind until this existed -- it was one of the three files AUDIT
# lists as verified only by compile-time size assertions, and it is the file that decides whether a
# boot costs 0.72 s or 14.4. The staleness matrix is the half worth having: which card changes force
# a rescan was argued from the source during a hardware session and got answered wrongly twice.
$CC $CFLAGS tools/hosttest/test_libindex.c src/library/libindex.c src/library/library.c \
    src/library/cache.c src/menu/paths.c tools/hosttest/shim/fs_probe.c -o "$OUT/test_libindex"
TESTDIR="$OUT/idxdir" "$OUT/test_libindex" 2>/dev/null

echo
echo "== loose art: sorted, deduplicated, found by bsearch"
rm -rf "$OUT/artdir"
mkdir -p "$OUT/artdir"
# The whole of library.c compiles here, which is new -- the shim grew dir_findfirst/dir_findnext
# over POSIX readdir so the real scan can walk a real directory tree. That matters because which
# duplicate survives is decided by traversal order, so a test that faked the traversal would be
# asserting against its own fake. The five ROM-side symbols are stubbed inside the test; a tree
# of .png files never reaches them, and the test fails if it ever does.
$CC $CFLAGS tools/hosttest/test_library_art.c src/library/library.c -o "$OUT/test_library_art"
TESTDIR="$OUT/artdir" "$OUT/test_library_art" 2>/dev/null

echo
echo "== fonts: every literal drawn in a restricted charset"
# Three of the five faces carry an 84-glyph charset rather than the body font's 7,931, because a
# full-charset bake at 40 px is about 2.7 MB. A character outside it draws as nothing, silently,
# on a console. --self-test proves the checker can say no before its green is believed.
python3 tools/charsetcheck.py --self-test

echo
echo "== icons: nothing blocked is vendored, and every author is credited"
# assets/icons is 3,894 SVGs of somebody else's CC BY artwork. The IP exclusions used to be
# re-applied on every build by mkpack --exclude; vendoring applied them once, so this is what
# keeps them applied. --self-test proves it can say no. Pass ICON_FULL_DIR to additionally check
# the blocklist has not rotted against an unfiltered corpus -- see the tool's docstring.
python3 tools/iconcheck.py --self-test

echo
echo "== profiles, paths and the shared lock list"
rm -rf "$OUT/profiledir"
$CC $CFLAGS tools/hosttest/test_profile.c src/library/cache.c src/library/locks.c src/library/playstate.c \
    src/menu/profile.c src/menu/ini_parser.c src/menu/paths.c tools/hosttest/shim/fs_probe.c \
    -o "$OUT/test_profile"
TESTDIR="$OUT/profiledir" "$OUT/test_profile" 2>/dev/null

echo
echo "== cheat database, read three ways"
# Against the REAL build/cheats.db rather than a constructed one, because the thing under test is
# whether the C reader and the Python writer agree about a layout, and a fixture written by this
# suite would only prove the suite agrees with itself. Needs the corpus; says so when it is absent
# rather than reporting a pass over nothing.
if [ -f build/cheats.db ]; then
    rm -rf "$OUT/cheatdir"
    mkdir -p "$OUT/cheatdir/mainmenu"
    cp build/cheats.db "$OUT/cheatdir/mainmenu/cheats.db"
    python3 tools/hosttest/cheatdb_expect.py build/cheats.db "$OUT/cheatdb-expect.txt"
    $CC $CFLAGS tools/hosttest/test_cheatdb.c src/cheats/cheatdb.c src/menu/paths.c \
        tools/hosttest/shim/fs_probe.c -o "$OUT/test_cheatdb"
    TESTDIR="$OUT/cheatdir/" EXPECT="$OUT/cheatdb-expect.txt" "$OUT/test_cheatdb" 2>/dev/null
else
    echo "  SKIPPED -- no build/cheats.db; run tools/mkcheatdb.py --fetch"
fi

echo
echo "== JPEG variants picojpeg does and does not accept"
if python3 tools/hosttest/mkjpegs.py "$OUT/jpegs" >"$OUT/mkjpegs.log" 2>&1; then
    sed 's/^/  /' "$OUT/mkjpegs.log"
    # -w for picojpeg itself: it is vendored third-party code with shift-of-negative warnings
    # that -Werror would otherwise turn into a build failure of somebody else's file.
    $CC $CFLAGS -Isrc/libs/picojpeg -w -o "$OUT/test_jpeg" \
        tools/hosttest/test_jpeg.c src/libs/picojpeg/picojpeg.c
    TESTDIR="$OUT/jpegs" "$OUT/test_jpeg"
else
    # Pillow is the only third-party dependency anywhere in this suite. Skipping is loud rather
    # than silent, because a skipped section that reads as a pass is the failure mode this whole
    # file exists to avoid.
    echo "  SKIPPED -- $(tail -1 "$OUT/mkjpegs.log")"
fi

echo
echo "== the checksum IPL3 puts in a ROM header"
# Pinned against tools/romcrc.py rather than against a real ROM, because the suite may not assume
# anyone's ROMs are on the machine. The shelf check -- `tools/romcrc.py roms/n64/*.z64`, 23 of 24
# reproducing their stored checksum -- is what established the algorithm; this is what keeps the
# two implementations of it from drifting.
$CC $CFLAGS tools/hosttest/test_romcrc.c src/menu/romcrc.c -o "$OUT/test_romcrc"
"$OUT/test_romcrc"

echo
echo "== finding the preamble to patch in a cartridge image"
# ares has no cartridge -- flashcart_is_dummy() is true there and the whole ROM-patch path is
# skipped -- so this is the ONLY place the code that picks two words of somebody's game to
# overwrite ever runs against anything. src/menu/rompatch_find.c is split out of rompatch.c
# precisely so it can be compiled here without libdragon.
$CC $CFLAGS -Isrc/menu tools/hosttest/test_rompatch.c src/menu/rompatch_find.c -o "$OUT/test_rompatch"
"$OUT/test_rompatch"

echo
echo "== running the emitted engine against Datel's"
# The C suite above pins what a line costs and where a conditional branches to; nothing in it
# executes an instruction. This does: tools/rompatch.py emits the same shapes and runs them through
# a small VR4300 interpreter, then checks the bytes against the reference expansion in
# src/boot/cheats.c. It is the only check on the repeater, which is the one shape here that is a
# rewrite rather than a transcription -- a loop where Datel emits `count` copies of the write.
python3 tools/rompatch.py --self-test

if [ "${1:-}" = "--mutate" ]; then
    echo
    echo "== mutation: accept a target that is neither __osException nor +16"
    # Conker's Bad Fur Day and GoldenEye 007 both match a run of data whose reconstructed target
    # is 0x100071e0 and 0x700101a0. Neither is RDRAM, and without a rejection those two get two
    # words of live game code rewritten at a coincidence -- one in twelve of the reference card.
    #
    # This used to mutate the explicit `target < 0x80000000 || target >= ram_top` test, and that
    # stopped proving anything: since the target is identified by reading the four words it points
    # at, a bogus one fails the identification too and the explicit test is now the second of two
    # guards. Removing either alone changes nothing, so the mutation has to remove the decision
    # both feed. There turned out to be three of them -- the explicit range test, the `rank < 0`
    # continue, and the -1 floor `best_rank` starts at -- each of which alone is enough, so the
    # mutation removes all three. Defence in depth is worth having and worth saying out loud;
    # what is not worth having is a mutation that reports a survival because two guards remain.
    sed -e 's/if (target < 0x80000000u || target >= ram_top) {/if (false) {/' \
        -e 's/if (rank < 0) {/if (false) {/' \
        -e 's/int best_rank = -1;/int best_rank = -2;/' \
        src/menu/rompatch_find.c > "$OUT/rompatch_mut.c"
    if ! mutated "$OUT/rompatch_mut.c" src/menu/rompatch_find.c "the bogus-target rejection"; then
        :
    else
        $CC $CFLAGS -Wno-unused-parameter -Isrc/menu tools/hosttest/test_rompatch.c \
            "$OUT/rompatch_mut.c" -o "$OUT/test_rompatch_mut"
        if "$OUT/test_rompatch_mut" >"$OUT/rompatch_mut.log" 2>&1; then
            echo "  MUTATION SURVIVED -- the suite does not check the bogus-target rejection"
        else
            sed 's/^/  /' "$OUT/rompatch_mut.log"
        fi
    fi
    echo
    echo "== mutation: shift every conditional's branch by one word, the suite must go red"
    # The most dangerous number in the engine. One word late and the branch lands on the store it
    # exists to skip, doing the thing the conditional was there to prevent; one word early and it
    # lands in the middle of the body. The formula is out in rompatch_find.c only so that this can
    # be done to it.
    sed -e 's/return 4 \* (tests - k - 1) + (int)body;/return 4 * (tests - k) + (int)body;/' \
        src/menu/rompatch_find.c > "$OUT/rompatch_branch_mut.c"
    $CC $CFLAGS -Isrc/menu tools/hosttest/test_rompatch.c \
        "$OUT/rompatch_branch_mut.c" -o "$OUT/test_rompatch_branch_mut"
    if "$OUT/test_rompatch_branch_mut" >"$OUT/rompatch_branch_mut.log" 2>&1; then
        echo "  MUTATION SURVIVED -- nothing pins where a conditional branches to"
    else
        sed 's/^/  /' "$OUT/rompatch_branch_mut.log"
    fi

    echo
    echo "== mutation: let a 16-bit write take an odd address, the suite must go red"
    # 1,964 of the corpus's 149,687 16-bit writes name one, and `sh` off an odd address takes an
    # Address Error at the exception vector with EXL set: it vectors straight back in and the
    # console locks. This is the check that stops a bad cheat being a dead machine.
    sed -e 's/return (((word >> 24) & 0x01u) != 0) && ((word & 1u) != 0);/return false;/' \
        src/menu/rompatch_find.c > "$OUT/rompatch_align_mut.c"
    $CC $CFLAGS -Wno-unused-parameter -Isrc/menu tools/hosttest/test_rompatch.c \
        "$OUT/rompatch_align_mut.c" -o "$OUT/test_rompatch_align_mut"
    if "$OUT/test_rompatch_align_mut" >"$OUT/rompatch_align_mut.log" 2>&1; then
        echo "  MUTATION SURVIVED -- the alignment refusal is not checked"
    else
        sed 's/^/  /' "$OUT/rompatch_align_mut.log"
    fi

    echo
    echo "== mutation: give 6103 the ordinary final mix, the ROM checksum suite must go red"
    # The seeds differ per CIC, so every per-CIC vector would still pass if the three final mixes
    # collapsed into one -- which is why the suite pins them as distinct as well as as correct.
    # No `$` anchor: .gitattributes checks the tree out CRLF, so the line really ends `:\r` and an
    # anchored pattern silently matches nothing -- which is what this one did, reporting a
    # survival that was really a sed that never fired.
    sed -e 's/case CIC_x103:\r*$/case CIC_5167:/' src/menu/romcrc.c > "$OUT/romcrc_mut.c"
    if ! mutated "$OUT/romcrc_mut.c" src/menu/romcrc.c "the per-CIC final mix"; then
        :
    else
        $CC $CFLAGS -Isrc/menu tools/hosttest/test_romcrc.c "$OUT/romcrc_mut.c" -o "$OUT/test_romcrc_mut"
        if "$OUT/test_romcrc_mut" >"$OUT/romcrc_mut.log" 2>&1; then
            echo "  MUTATION SURVIVED -- the suite does not check the 6103 mix"
        else
            sed 's/^/  /' "$OUT/romcrc_mut.log"
        fi
    fi

    echo
    echo
    echo "== mutation: move every slot down by one, the atlas suite must go red"
    # slot_offset() is the arithmetic the whole file rests on and the one thing no other test can
    # reach. Dropping the +1 puts slot 0 on top of the header, which is exactly the class of
    # mistake that would have shipped: it still writes, still indexes, still reports a hit.
    sed 's/return (long)((slot + 1) \* SLOT_BYTES);/return (long)(slot * SLOT_BYTES);/' \
        src/library/thumbstore.c > "$OUT/thumbstore_mutant.c"
    grep -q 'return (long)(slot \* SLOT_BYTES);' "$OUT/thumbstore_mutant.c" ||
        { echo "slot_offset mutation did not apply" >&2; exit 1; }

    rm -rf "$OUT/mutant_thumbdir" "$OUT/mutant_thumbdir-ro"
    $CC -std=c11 -Wall -Itools/hosttest/shim -Isrc -Isrc/library \
        tools/hosttest/test_thumbstore.c src/library/cache.c "$OUT/thumbstore_mutant.c" \
        src/menu/paths.c tools/hosttest/shim/fs_probe.c -o "$OUT/test_thumbstore_mutant"

    if TESTDIR="$OUT/mutant_thumbdir" "$OUT/test_thumbstore_mutant" \
            >"$OUT/thumb_mutant.log" 2>/dev/null; then
        echo "MUTANT PASSED -- the atlas suite cannot detect a wrong slot offset" >&2
        exit 1
    fi
    grep -E 'FAIL|failures' "$OUT/thumb_mutant.log"
    echo "mutation detected, so a green atlas run above means something"

    if [ -x "$OUT/test_jpeg" ]; then
        echo
        echo "== mutation: let picojpeg accept progressive, the JPEG suite must go red"
        # The four progressive checks are the only ones in that suite asserting a REFUSAL, and a
        # refusal is the easiest thing in the world to assert by accident -- any error at all
        # would satisfy a looser test. Making the decoder stop refusing proves they are pinned to
        # UNSUPPORTED_MODE specifically, which is the value image_decoder keys on.
        sed 's/return PJPG_UNSUPPORTED_MODE;/return 0;/' \
            src/libs/picojpeg/picojpeg.c > "$OUT/picojpeg_mutant.c"
        grep -q 'return 0;' "$OUT/picojpeg_mutant.c" || { echo "mutation did not apply" >&2; exit 1; }
        $CC $CFLAGS -Isrc/libs/picojpeg -w -o "$OUT/test_jpeg_mutant" \
            tools/hosttest/test_jpeg.c "$OUT/picojpeg_mutant.c"
        if TESTDIR="$OUT/jpegs" "$OUT/test_jpeg_mutant" >"$OUT/jpeg_mutant.log" 2>/dev/null; then
            echo "MUTANT PASSED -- the JPEG suite cannot tell refused from accepted" >&2
            exit 1
        fi
        grep -E 'FAIL|failures' "$OUT/jpeg_mutant.log"
        echo "mutation detected, so a green JPEG run above means something"
    fi

    echo
    echo "== mutation: give profile 1 a suffix, the profile suite must go red"
    # The one mistake in this feature that is silent, unrecoverable and shaped exactly like
    # working software: profile 1 must write to `playstate.dat` and `<romdir>/saves/`, the paths
    # every card already uses. Give it a `p1/` prefix and the menu still boots, still saves, still
    # loads -- into a folder nothing has ever written, so every favourite and every save on every
    # existing card is simply gone. Nothing in a frame would show it.
    # awk rather than sed: the line to change is not unique -- profile_save_subdir() opens with
    # the same `if (active == 0) {` -- so the mutation has to count occurrences, and sed cannot.
    awk '{ if ($0 == "    if (active == 0) {") { seen++ } ;
           if (seen == 2 && $0 == "        snprintf(out, cap, \"%s\", name);")
               { print "        snprintf(out, cap, \"p1/%s\", name);" } else { print } }' \
        src/menu/profile.c > "$OUT/profile_mutant.c"
    grep -q 'p1/%s' "$OUT/profile_mutant.c" || { echo "profile mutation did not apply" >&2; exit 1; }

    rm -rf "$OUT/mutant_profiledir"
    $CC -std=c11 -Wall -Itools/hosttest/shim -Isrc -Isrc/library \
        tools/hosttest/test_profile.c src/library/cache.c src/library/locks.c \
        src/library/playstate.c "$OUT/profile_mutant.c" src/menu/ini_parser.c src/menu/paths.c tools/hosttest/shim/fs_probe.c \
        -o "$OUT/test_profile_mutant"

    if TESTDIR="$OUT/mutant_profiledir" "$OUT/test_profile_mutant" \
            >"$OUT/profile_mutant.log" 2>/dev/null; then
        echo "MUTANT PASSED -- the profile suite cannot tell profile 1 from profile 2" >&2
        exit 1
    fi
    grep -E 'FAIL|failures' "$OUT/profile_mutant.log"
    echo "mutation detected, so a green profile run above means something"

    echo
    echo "== mutation: let profile 1's saves be deleted, the profile suite must go red"
    # The most dangerous thing in the program, and the mutation took two tries to get right.
    #
    # The first attempt only relaxed the `index <= 0` guard, and the mutant PASSED -- because slot
    # 0's folder name would be `p1`, and nothing has ever written to `saves/p1/`. The guard alone
    # is belt and braces; what really protects profile 1 is that its saves are in the UNSUFFIXED
    # `saves/` and this function never builds that path.
    #
    # So the mutation is the bug somebody would actually write: making erase agree with
    # profile_save_subdir(), which returns NULL for slot 0 so profile 1 uses `saves/` directly --
    # and forgetting that the guard was the only thing standing between that and every save on a
    # card older than this feature. Two edits, because it takes two to be dangerous.
    sed -e 's/if (index <= 0 || index >= PROFILE_MAX || lib == NULL)/if (index < 0 || index >= PROFILE_MAX || lib == NULL)/' \
        -e 's|snprintf(folder, sizeof(folder), "%s/%s/%s", dir, SAVE_DIRECTORY_NAME, sub);|snprintf(folder, sizeof(folder), index ? "%s/%s/%s" : "%s/%s", dir, SAVE_DIRECTORY_NAME, sub);|' \
        src/menu/profile.c > "$OUT/profile_mutant2.c"
    grep -q 'index ? ' "$OUT/profile_mutant2.c" \
        || { echo "erase-guard mutation did not apply" >&2; exit 1; }

    rm -rf "$OUT/mutant_profiledir2"
    $CC -std=c11 -Wall -Itools/hosttest/shim -Isrc -Isrc/library \
        tools/hosttest/test_profile.c src/library/cache.c src/library/locks.c \
        src/library/playstate.c "$OUT/profile_mutant2.c" src/menu/ini_parser.c src/menu/paths.c tools/hosttest/shim/fs_probe.c \
        -o "$OUT/test_profile_mutant2"

    if TESTDIR="$OUT/mutant_profiledir2" "$OUT/test_profile_mutant2" \
            >"$OUT/profile_mutant2.log" 2>/dev/null; then
        echo "MUTANT PASSED -- nothing here checks that profile 1's saves are protected" >&2
        exit 1
    fi
    grep -E 'FAIL|failures' "$OUT/profile_mutant2.log"
    echo "mutation detected, so the guard on profile 1's saves is really tested"

    if [ -f build/cheats.db ]; then
        echo
        echo "== mutation: read a game's codes from its group table, the cheat suite must go red"
        # Where the code lines start inside a blob is the one number the format 2 rewrite
        # introduced, and getting it wrong is invisible from the console: the detail sheet shows
        # the right names and the right count, and the engine is handed addresses taken from the
        # group table. Cheats that write to arbitrary memory, presented as the cheats you picked.
        sed 's/size_t codes_off = (size_t)n \* sizeof(cheatdb_group_t);/size_t codes_off = 0;/' \
            src/cheats/cheatdb.c > "$OUT/cheatdb_mutant.c"
        grep -q 'size_t codes_off = 0;' "$OUT/cheatdb_mutant.c" ||
            { echo "cheatdb mutation did not apply" >&2; exit 1; }

        $CC -std=c11 -Wall -Itools/hosttest/shim -Isrc -Isrc/library \
            tools/hosttest/test_cheatdb.c "$OUT/cheatdb_mutant.c" src/menu/paths.c \
            tools/hosttest/shim/fs_probe.c -o "$OUT/test_cheatdb_mutant"

        if TESTDIR="$OUT/cheatdir/" EXPECT="$OUT/cheatdb-expect.txt" \
                "$OUT/test_cheatdb_mutant" >"$OUT/cheatdb_mutant.log" 2>/dev/null; then
            echo "MUTANT PASSED -- the cheat suite cannot tell codes from group headers" >&2
            exit 1
        fi
        tail -1 "$OUT/cheatdb_mutant.log"
        echo "mutation detected, so a green cheat run above means something"
    fi

    echo
    echo "== mutation: break the CRC seed, the suite must go red"
    sed 's/uint32_t crc = 0xFFFFFFFFu;/uint32_t crc = 0x00000000u;/' \
        src/library/cache.c > "$OUT/cache_mutant.c"
    grep -q '0x00000000u;' "$OUT/cache_mutant.c" || { echo "mutation did not apply" >&2; exit 1; }

    rm -rf "$OUT/mutant_dir"
    # -Werror dropped: the point is to run the mutant, not to lint it.
    $CC -std=c11 -Wall -Itools/hosttest/shim -Isrc -Isrc/library \
        tools/hosttest/test_cache.c "$OUT/cache_mutant.c" src/menu/paths.c tools/hosttest/shim/fs_probe.c \
        -o "$OUT/test_cache_mutant"

    if TESTDIR="$OUT/mutant_dir" "$OUT/test_cache_mutant" >"$OUT/mutant.log" 2>/dev/null; then
        echo "MUTANT PASSED -- the suite cannot detect a broken CRC, which makes it worthless" >&2
        exit 1
    fi
    grep -E 'FAIL|failures' "$OUT/mutant.log"
    echo "mutation detected, so a green run above means something"
fi
