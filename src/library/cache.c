/**
 * @file cache.c
 * @brief Shared discipline for every file the menu writes. See cache.h for the rules.
 * @ingroup library
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "cache.h"
#include "menu/paths.h"
#include "utils/fs.h"

#define CACHE_SUBDIR    "cache"
#define PROBE_NAME      "writable.probe"

/** Refuse anything absurd rather than trying to allocate it. The largest real payload is the
 *  library index at ~84 KB for 500 titles; 4 MB is far past any of them and still short of the
 *  free heap, so a corrupt length field cannot turn into an out-of-memory fault. */
#define CACHE_MAX_PAYLOAD   (4u * 1024u * 1024u)

/* Every on-disk struct's size is asserted, because the one bug this whole family of files
 * cannot survive is silent padding. A compiler that inserts two bytes somewhere writes a
 * file that passes its own magic, version and CRC and is then read back with every field
 * after the padding shifted -- which looks like corrupt data from a working card. __packed
 * should prevent it; this is what proves it did. */
_Static_assert(sizeof(cache_header_t) == 16, "cache header must stay 16 bytes");

static char cache_dir[256];
static bool writable;
static const char *status = "not initialised";

/* ------------------------------------------------------------------ hashing */

uint32_t cache_crc32 (const void *data, size_t bytes) {
    /* Nibble table: 16 entries instead of 256, two lookups per byte. The 1 KB table version is
     * about 40 % faster and costs 1 KB of .data on a machine where the largest payload this
     * runs over takes 7 ms either way. Not worth the rodata. */
    static const uint32_t tab[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (bytes--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ tab[crc & 0x0F];
        crc = (crc >> 4) ^ tab[crc & 0x0F];
    }
    return ~crc;
}

uint64_t cache_hash64 (const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ull;
    }
    return h;
}

uint32_t cache_hash32 (const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ paths and the probe */

void cache_path (char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s", cache_dir, name);
}

void cache_init (const char *storage_prefix) {
    /* menu_path() strips the slash the prefix already ends in: concatenating naively gives
     * "sd://mainmenu/cache", which FatFs has never been asked to accept -- the same
     * doubled-separator bug AUDIT.md 1n records for every library path. */
    menu_path(cache_dir, sizeof(cache_dir), storage_prefix, CACHE_SUBDIR);

    writable = false;

    char probe[300];
    cache_path(probe, sizeof(probe), PROBE_NAME);

    /* directory_create() walks and creates each component. It returning failure is not itself
     * conclusive -- the directory may already exist -- so the probe below is what actually
     * decides, and it decides by doing the thing we care about rather than by asking. */
    directory_create(cache_dir);

    FILE *f = fopen(probe, "wb");
    if (f == NULL) {
        status = "read-only storage";
        debugf("CACHE %s: %s (no cache will be written)\n", cache_dir, status);
        return;
    }

    /* Write a real byte and check it landed. An fopen that succeeds and an fwrite that silently
     * writes nothing is exactly what a full card looks like, and a probe that only opened would
     * report that card as writable and then fail on every single cache afterwards. */
    const char payload = 'M';
    bool ok = (fwrite(&payload, 1, 1, f) == 1);
    if (fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        status = "storage full or failing";
        debugf("CACHE %s: %s (no cache will be written)\n", cache_dir, status);
        remove(probe);
        return;
    }

    remove(probe);
    writable = true;
    status = "writable";
    debugf("CACHE %s: writable\n", cache_dir);
}

bool cache_writable (void) {
    return writable;
}

const char *cache_status (void) {
    return status;
}

void cache_drop (const char *name) {
    if (!writable) {
        return;
    }
    char path[300];
    cache_path(path, sizeof(path), name);
    remove(path);
}

/* ------------------------------------------------------------------ load and store */

bool cache_load (const char *name, uint32_t magic, void **payload, uint32_t *bytes) {
    char path[300];
    cache_path(path, sizeof(path), name);

    *payload = NULL;
    *bytes = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;               /* absent is the normal first-boot case, not a fault */
    }

    cache_header_t h;
    bool bad = (fread(&h, 1, sizeof(h), f) != sizeof(h));

    if (!bad && h.magic != magic) {
        debugf("CACHE %s: magic %08lx, wanted %08lx -- rebuilding\n",
               name, (unsigned long)h.magic, (unsigned long)magic);
        bad = true;
    }
    if (!bad && h.format_ver != MENU_CACHE_FORMAT_VER) {
        debugf("CACHE %s: format v%u, wanted v%u -- rebuilding\n",
               name, h.format_ver, MENU_CACHE_FORMAT_VER);
        bad = true;
    }
    if (!bad && (h.payload_bytes == 0 || h.payload_bytes > CACHE_MAX_PAYLOAD)) {
        debugf("CACHE %s: implausible length %lu -- rebuilding\n",
               name, (unsigned long)h.payload_bytes);
        bad = true;
    }

    void *buf = NULL;
    if (!bad) {
        buf = malloc(h.payload_bytes);
        if (buf == NULL) {
            /* Out of memory is NOT a corrupt file. Bail without deleting -- the cache is
             * probably fine and will be readable on a boot that is not this tight. */
            fclose(f);
            debugf("CACHE %s: no memory for %lu bytes\n", name, (unsigned long)h.payload_bytes);
            return false;
        }
        if (fread(buf, 1, h.payload_bytes, f) != h.payload_bytes) {
            debugf("CACHE %s: truncated -- rebuilding\n", name);
            bad = true;
        }
    }
    fclose(f);

    if (!bad && cache_crc32(buf, h.payload_bytes) != h.payload_crc) {
        debugf("CACHE %s: CRC mismatch -- rebuilding\n", name);
        bad = true;
    }

    if (bad) {
        free(buf);
        remove(path);               /* no-op on read-only storage, which is fine */
        return false;
    }

    *payload = buf;
    *bytes = h.payload_bytes;
    return true;
}

bool cache_store (const char *name, uint32_t magic, const void *payload, uint32_t bytes) {
    if (!writable || bytes == 0 || bytes > CACHE_MAX_PAYLOAD) {
        return false;
    }

    char path[300];
    cache_path(path, sizeof(path), name);

    cache_header_t h = {
        .magic         = magic,
        .format_ver    = MENU_CACHE_FORMAT_VER,
        .flags         = 0,
        .payload_bytes = bytes,
        .payload_crc   = cache_crc32(payload, bytes),
    };

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        debugf("CACHE %s: open for write failed\n", name);
        return false;
    }

    bool ok = (fwrite(&h, 1, sizeof(h), f) == sizeof(h)) &&
              (fwrite(payload, 1, bytes, f) == bytes);
    if (fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        /* A half-written cache would pass its magic and version check and fail only on the CRC,
         * which is survivable -- but leaving it there means paying the read and the CRC on every
         * boot to reach the same conclusion. Delete it now, while we know. */
        debugf("CACHE %s: write failed, discarding\n", name);
        remove(path);
        return false;
    }

    debugf("CACHE %s: wrote %lu bytes\n", name, (unsigned long)(bytes + sizeof(h)));
    return true;
}
