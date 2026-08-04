/**
 * @file test_cache.c
 * @brief Host-side round-trip test for src/library/cache.c.
 *
 * This exists because of a specific gap: **the write half of the cache layer cannot be executed
 * on the machine it is developed on.** Under ares the storage prefix is the ROM's own DFS, which
 * is read only, so `cache_writable()` is false and every `cache_store()` returns immediately
 * without touching a byte of the serialisation code. The regression suite therefore proves only
 * that the menu degrades gracefully -- which matters, and is not the same as proving the format
 * works.
 *
 * cache.c is portable C: stdio, stdlib, string, and two things it does not implement itself
 * (`debugf` and `directory_create`), both shimmed in tools/hosttest/shim. So the real file --
 * not a copy, not a reimplementation -- is compiled natively here and made to write and read
 * actual files in a temporary directory.
 *
 * What this covers: the header layout, the CRC, and every rejection path. What it does not, and
 * cannot: FatFs, the SC64, alignment, and whether an N64 writes the same bytes an x86 does. The
 * struct sizes are pinned by _Static_assert in the target build for exactly that reason, and the
 * endianness question is settled by both ends being the same machine -- the card is only ever
 * read by the console that wrote it.
 *
 *     cc -std=c11 -Wall -Wextra -Werror -Itools/hosttest/shim -Isrc \
 *        tools/hosttest/test_cache.c src/library/cache.c -o build/test_cache && build/test_cache
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/cache.h"

/* ------------------------------------------------------------------ shims */

bool directory_create (char *path) {
    /* mkdir -p, one level deep, which is all cache_init() asks for here. */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return mkdir(tmp, 0777) == 0;
}

/* ------------------------------------------------------------------ harness */

static int failures;
static int checks;

static void check (bool ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

#define MAGIC_A  0x41414141u
#define MAGIC_B  0x42424242u

static char root[512];

static void path_of (char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/menu/cache/%s", root, name);
}

/** @brief Corrupt one byte at @p off. Returns false if the file is shorter than that. */
static bool poke (const char *name, long off, uint8_t value) {
    char p[600];
    path_of(p, sizeof(p), name);
    FILE *f = fopen(p, "rb+");
    if (f == NULL) {
        return false;
    }
    bool ok = (fseek(f, off, SEEK_SET) == 0) && (fwrite(&value, 1, 1, f) == 1);
    fclose(f);
    return ok;
}

/** @brief Chop @p name down to @p bytes. */
static bool chop (const char *name, long bytes) {
    char p[600];
    path_of(p, sizeof(p), name);
    return truncate(p, bytes) == 0;
}

static bool exists (const char *name) {
    char p[600];
    path_of(p, sizeof(p), name);
    struct stat st;
    return stat(p, &st) == 0;
}

int main (void) {
    snprintf(root, sizeof(root), "%s", getenv("TESTDIR") ? getenv("TESTDIR") : "build/test_cache_dir");

    char prefix[520];
    snprintf(prefix, sizeof(prefix), "%s/", root);      /* mimic "sd:/" -- trailing slash */

    printf("cache.c round trip, root=%s\n", root);
    cache_init(prefix);
    check(cache_writable(), "a writable directory probes as writable");

    /* --- the happy path ------------------------------------------------ */
    uint8_t payload[1000];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7 + 3);
    }

    check(cache_store("a.dat", MAGIC_A, payload, sizeof(payload)), "store succeeds");

    void *got = NULL;
    uint32_t got_bytes = 0;
    check(cache_load("a.dat", MAGIC_A, &got, &got_bytes), "load succeeds");
    check(got_bytes == sizeof(payload), "length round trips");
    check(got != NULL && memcmp(got, payload, sizeof(payload)) == 0, "payload round trips exactly");
    free(got);

    /* --- the rejections. Each must fail AND delete, so a bad cache costs one boot, not every
     *     boot. That second half is the part worth testing: a reader that rejects without
     *     deleting looks identical until you notice it never recovers. ------------------- */
    check(!cache_load("a.dat", MAGIC_B, &got, &got_bytes), "wrong magic is rejected");
    check(!exists("a.dat"), "wrong magic deletes the file");

    check(cache_store("b.dat", MAGIC_A, payload, sizeof(payload)), "store b");
    check(poke("b.dat", 4, (uint8_t)(MENU_CACHE_FORMAT_VER + 1)), "bump the version byte");
    check(!cache_load("b.dat", MAGIC_A, &got, &got_bytes), "wrong version is rejected");
    check(!exists("b.dat"), "wrong version deletes the file");

    check(cache_store("c.dat", MAGIC_A, payload, sizeof(payload)), "store c");
    check(poke("c.dat", 16 + 500, 0xFF), "flip a payload byte");
    check(!cache_load("c.dat", MAGIC_A, &got, &got_bytes), "CRC mismatch is rejected");
    check(!exists("c.dat"), "CRC mismatch deletes the file");

    check(cache_store("d.dat", MAGIC_A, payload, sizeof(payload)), "store d");
    check(chop("d.dat", 16 + 400), "truncate mid-payload, as a power cut would");
    check(!cache_load("d.dat", MAGIC_A, &got, &got_bytes), "truncation is rejected");
    check(!exists("d.dat"), "truncation deletes the file");

    check(chop("e.dat", 0) == false, "e.dat does not exist yet");
    check(!cache_load("e.dat", MAGIC_A, &got, &got_bytes), "an absent file is a quiet miss");

    /* --- read-only storage, which is the ares case and the one that must never fault ---- */
    char ro[600];
    snprintf(ro, sizeof(ro), "%s/readonly", root);
    mkdir(ro, 0555);
    char ro_prefix[620];
    snprintf(ro_prefix, sizeof(ro_prefix), "%s/", ro);

    cache_init(ro_prefix);
    check(!cache_writable(), "an unwritable directory probes as unwritable");
    check(!cache_store("x.dat", MAGIC_A, payload, sizeof(payload)), "store is refused, not attempted");
    check(!cache_load("x.dat", MAGIC_A, &got, &got_bytes), "load of a missing file is quiet");
    cache_drop("x.dat");                    /* must not crash */
    check(true, "drop on read-only storage does not fault");

    /* --- hashing is stable, since playstate keys and thumb slots depend on it ----------- */
    check(cache_hash64("Super Mario 64.z64") == cache_hash64("Super Mario 64.z64"), "hash64 is stable");
    check(cache_hash64("a") != cache_hash64("b"), "hash64 distinguishes");
    check(cache_hash32("Infinite Lives") != cache_hash32("Infinite Health"), "hash32 distinguishes");
    check(cache_crc32("123456789", 9) == 0xCBF43926u, "crc32 matches the IEEE check value");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
