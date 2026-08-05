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
echo "== profiles, paths and the shared lock list"
rm -rf "$OUT/profiledir"
$CC $CFLAGS tools/hosttest/test_profile.c src/library/cache.c src/library/locks.c src/library/playstate.c \
    src/menu/profile.c src/menu/ini_parser.c src/menu/paths.c tools/hosttest/shim/fs_probe.c \
    -o "$OUT/test_profile"
TESTDIR="$OUT/profiledir" "$OUT/test_profile" 2>/dev/null

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

if [ "${1:-}" = "--mutate" ]; then
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
