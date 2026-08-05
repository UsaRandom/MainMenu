/**
 * @file libindex.c
 * @brief The library index cache. See libindex.h for the format and the staleness rules.
 * @ingroup library
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libdragon.h>

#include "cache.h"
#include "libindex.h"
#include "library.h"

#define LIBINDEX_FILE   "library.idx"

/** Depth limit, matching library.c's scan. A signature walk that went deeper than the scan would
 *  invalidate on directories the index never contained. */
#define SIG_MAX_DEPTH   6

/** Refuse to walk a card with an absurd number of directories rather than growing without bound.
 *  500 titles in a tidy tree is tens of directories; 1024 is far past anything real. */
#define SIG_MAX_DIRS    1024

/** @brief Per-directory fingerprint. 24 bytes. */
typedef struct __attribute__((packed)) {
    uint64_t path_hash;
    uint64_t size_sum;
    uint32_t entries;
    uint32_t reserved;
} dirsig_t;

/**
 * @brief One indexed title. Strings are offsets into the strtab.
 *
 * 40 bytes, and the last field is padding to make it so. The natural size of these members is 36,
 * which the static assert below caught on the first build -- and 36 is a bad number here: the
 * record array starts at offset 24 in the payload, so with a 36-byte stride every other record
 * lands 4-byte aligned and every `check_code` becomes an unaligned 64-bit load. Rounding to 40
 * keeps both the array start and the stride multiples of 8, so every record is aligned.
 */
typedef struct __attribute__((packed)) {
    uint64_t check_code;
    char     game_code[4];      /**< NOT NUL-terminated here; the record widens it on load */
    uint8_t  version;
    uint8_t  system;
    uint8_t  save_type;
    uint8_t  reserved;
    uint16_t feat;
    uint16_t flags;
    uint16_t dominant;
    uint16_t reserved2;
    uint32_t path_off;
    uint32_t title_off;
    uint32_t art_off;           /**< STR_NONE when there is no resolved art path */
    uint32_t reserved3;         /**< keeps the stride at 40; see above */
} idx_record_t;

/** @brief Payload header. */
typedef struct __attribute__((packed)) {
    uint32_t record_count;
    uint32_t dirsig_count;
    uint32_t records_off;
    uint32_t dirsigs_off;
    uint32_t strtab_off;
    uint32_t strtab_bytes;
} idx_payload_t;

/** Offset meaning "no string". Zero cannot be used: offset 0 is a real, reachable byte. */
#define STR_NONE  0xFFFFFFFFu

/* Every on-disk struct's size is asserted, because the one bug this whole family of files
 * cannot survive is silent padding. A compiler that inserts two bytes somewhere writes a
 * file that passes its own magic, version and CRC and is then read back with every field
 * after the padding shifted -- which looks like corrupt data from a working card. __packed
 * should prevent it; this is what proves it did. */
_Static_assert(sizeof(idx_record_t) == 40, "index record must stay 40 bytes");
_Static_assert(sizeof(dirsig_t) == 24, "directory signature must stay 24 bytes");
_Static_assert(sizeof(idx_payload_t) == 24, "index payload header must stay 24 bytes");

/* ------------------------------------------------------------------ the signature walk */

typedef struct {
    dirsig_t *sigs;
    int count;
    bool overflow;
} sigwalk_t;

/**
 * @brief Fingerprint @p dir and recurse. Enumeration only -- nothing is opened.
 *
 * The entry count and size sum together catch every change that matters: a ROM added, removed,
 * replaced with a different build, or art dropped alongside it. They are taken over the same
 * entries library.c's scan_dir() visits -- the dotfile skip AND library_scan_skipped() -- so the
 * two agree on what "this directory" means.
 *
 * That second half is load-bearing rather than tidy. `mainmenu/cache` is rewritten on every boot
 * and the writability probe is created and deleted inside it, so a signature that counted it would
 * differ from the stored one every single time and the index would be thrown away on every start
 * of a console that can write to its card. Nothing under ares can catch that: the DFS is read-only,
 * so those directories never move and the walk looks stable.
 */
static void sig_dir (sigwalk_t *w, const char *dir, int depth) {
    if (depth > SIG_MAX_DEPTH || w->overflow) {
        return;
    }
    if (w->count >= SIG_MAX_DIRS) {
        w->overflow = true;
        return;
    }

    dirsig_t *sig = &w->sigs[w->count++];
    sig->path_hash = cache_hash64(dir);
    sig->size_sum  = 0;
    sig->entries   = 0;
    sig->reserved  = 0;

    /* Children are collected first and recursed afterwards, because dir_findnext() keeps its
     * position in the directory being enumerated and recursing mid-walk would resume the parent
     * in whatever state the child left it. */
    char (*kids)[256] = NULL;
    int kid_count = 0, kid_cap = 0;

    dir_t info;
    int result = dir_findfirst(dir, &info);
    while (result == 0) {
        if (info.d_name[0] != '.' &&
            !(info.d_type == DT_DIR && library_scan_skipped(info.d_name))) {
            sig->entries++;
            if (info.d_type == DT_DIR) {
                if (kid_count == kid_cap) {
                    int want = kid_cap ? kid_cap * 2 : 8;
                    void *bigger = realloc(kids, (size_t)want * 256);
                    if (bigger == NULL) {
                        w->overflow = true;
                        break;
                    }
                    kids = bigger;
                    kid_cap = want;
                }
                snprintf(kids[kid_count++], 256, "%s", info.d_name);
            } else {
                sig->size_sum += (uint64_t)(info.d_size > 0 ? info.d_size : 0);
            }
        }
        result = dir_findnext(dir, &info);
    }

    for (int i = 0; i < kid_count && !w->overflow; i++) {
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", dir, kids[i]);
        sig_dir(w, child, depth + 1);
    }
    free(kids);
}

/**
 * @brief Fingerprint the whole tree under @p root.
 * @return the signature array, or NULL. Caller frees. @p out_count receives the length.
 */
static dirsig_t *sig_collect (const char *storage_prefix, const char *root, int *out_count) {
    char base[512];
    library_join(base, sizeof(base), storage_prefix, root);

    sigwalk_t w = { .sigs = calloc(SIG_MAX_DIRS, sizeof(dirsig_t)) };
    if (w.sigs == NULL) {
        *out_count = 0;
        return NULL;
    }

    sig_dir(&w, base, 0);

    if (w.overflow || w.count == 0) {
        free(w.sigs);
        *out_count = 0;
        return NULL;
    }
    *out_count = w.count;
    return w.sigs;
}

/* ------------------------------------------------------------------ load */

/** @brief Bounds-checked strtab lookup. Returns NULL for STR_NONE or anything out of range. */
static const char *str_at (const char *strtab, uint32_t bytes, uint32_t off) {
    if (off == STR_NONE || off >= bytes) {
        return NULL;
    }
    return strtab + off;
}

bool libindex_load (library_t *lib, const char *storage_prefix, const char *root) {
    void *buf = NULL;
    uint32_t bytes = 0;

    if (!cache_load(LIBINDEX_FILE, LIBINDEX_MAGIC, &buf, &bytes)) {
        return false;
    }

    /* Every offset below comes off the card, so each one is range-checked before use. A cache
     * file is not hostile input, but it is input that survives a power cut mid-write, and the
     * CRC only proves the bytes are the bytes that were written -- not that they were sane. */
    if (bytes < sizeof(idx_payload_t)) {
        free(buf);
        return false;
    }

    const idx_payload_t *h = buf;
    uint64_t rec_end = (uint64_t)h->records_off + (uint64_t)h->record_count * sizeof(idx_record_t);
    uint64_t sig_end = (uint64_t)h->dirsigs_off + (uint64_t)h->dirsig_count * sizeof(dirsig_t);
    uint64_t str_end = (uint64_t)h->strtab_off + h->strtab_bytes;

    if (h->record_count == 0 || rec_end > bytes || sig_end > bytes || str_end > bytes ||
        h->strtab_bytes == 0) {
        debugf("LIBINDEX: header out of range -- rebuilding\n");
        free(buf);
        return false;
    }

    const char *strtab = (const char *)buf + h->strtab_off;
    /* The strtab must end in a NUL or a title running off the end becomes an unterminated read
     * through the rest of the heap. */
    if (strtab[h->strtab_bytes - 1] != '\0') {
        debugf("LIBINDEX: strtab unterminated -- rebuilding\n");
        free(buf);
        return false;
    }

    /* Revalidate BEFORE populating: the walk is the expensive half of this path and there is no
     * point paying for hundreds of strdups first. */
    uint32_t t0 = TICKS_READ();
    int now_count = 0;
    dirsig_t *now = sig_collect(storage_prefix, root, &now_count);
    uint32_t walk_us = TIMER_MICROS(TICKS_SINCE(t0));

    bool fresh = (now != NULL && now_count == (int)h->dirsig_count);
    if (fresh) {
        const dirsig_t *was = (const dirsig_t *)((const char *)buf + h->dirsigs_off);
        for (int i = 0; i < now_count; i++) {
            /* Position-independent comparison would be more robust to a directory being renamed,
             * but the walk is deterministic and depth-first over the same tree, so an ordering
             * change IS a change and rescanning is the right answer. */
            if (now[i].path_hash != was[i].path_hash ||
                now[i].entries   != was[i].entries   ||
                now[i].size_sum  != was[i].size_sum) {
                fresh = false;
                break;
            }
        }
    }
    free(now);

    if (!fresh) {
        debugf("LIBINDEX: card changed (%lu us to check) -- rescanning\n", (unsigned long)walk_us);
        free(buf);
        return false;
    }

    const idx_record_t *recs = (const idx_record_t *)((const char *)buf + h->records_off);
    for (uint32_t i = 0; i < h->record_count; i++) {
        lib_record_t *r = library_push(lib);
        if (r == NULL) {
            break;
        }
        memset(r, 0, sizeof(*r));

        r->check_code = recs[i].check_code;
        memcpy(r->game_code, recs[i].game_code, 4);
        r->game_code[4] = '\0';
        r->version   = recs[i].version;
        r->system    = recs[i].system;
        r->save_type = recs[i].save_type;
        r->feat      = recs[i].feat;
        r->flags     = recs[i].flags;
        r->dominant  = recs[i].dominant;

        const char *s;
        if ((s = str_at(strtab, h->strtab_bytes, recs[i].path_off))  != NULL) r->path     = strdup(s);
        if ((s = str_at(strtab, h->strtab_bytes, recs[i].title_off)) != NULL) r->title    = strdup(s);
        if ((s = str_at(strtab, h->strtab_bytes, recs[i].art_off))   != NULL) r->art_file = strdup(s);

        /* Art state is rebuilt, never restored. ART_READY refers to a slot in a RAM pool that
         * does not exist yet at this point in the boot, and restoring it would hand the grid a
         * record claiming to have art with nothing behind it. The one thing worth carrying over
         * is the negative: LIBF_ART_MISSING means the search already ran and found nothing. */
        r->art_state = (r->flags & LIBF_ART_MISSING) ? ART_NONE : ART_PENDING;
        r->art_age   = 1.0e9f;      /* already settled; no arrival pop for a cached record */
    }

    uint32_t total_us = TIMER_MICROS(TICKS_SINCE(t0));
    debugf("LIBINDEX loaded %d titles in %lu us (%lu us of that revalidating %d dirs)\n",
           lib->count, (unsigned long)total_us, (unsigned long)walk_us, (int)h->dirsig_count);

    free(buf);
    return lib->count > 0;
}

/* ------------------------------------------------------------------ save */

/**
 * @brief Append @p s to the growing strtab, writing its offset to @p off. STR_NONE for NULL.
 *
 * @return false only on allocation failure, which the caller must treat as fatal to the save.
 *
 * The failure is reported rather than folded into the offset because STR_NONE already means
 * "this record has no such string", and a version of this that returned STR_NONE for both would
 * write a perfectly valid, CRC-correct index in which some records had lost their path. Next
 * boot those records load with `path == NULL`, and grid_open() -- correctly -- refuses to launch
 * a record with no path. The game would simply stop responding to A, with a cache file that
 * passes every integrity check saying it should.
 */
static bool str_push (char **tab, uint32_t *len, uint32_t *cap, const char *s, uint32_t *off) {
    if (s == NULL) {
        *off = STR_NONE;
        return true;
    }
    uint32_t need = (uint32_t)strlen(s) + 1;
    if (*len + need > *cap) {
        uint32_t want = *cap ? *cap * 2 : 4096;
        while (want < *len + need) {
            want *= 2;
        }
        char *bigger = realloc(*tab, want);
        if (bigger == NULL) {
            return false;
        }
        *tab = bigger;
        *cap = want;
    }
    *off = *len;
    memcpy(*tab + *off, s, need);
    *len += need;
    return true;
}

bool libindex_save (const library_t *lib, const char *storage_prefix, const char *root) {
    if (!cache_writable() || lib->count == 0) {
        return false;
    }

    int sig_count = 0;
    dirsig_t *sigs = sig_collect(storage_prefix, root, &sig_count);
    if (sigs == NULL) {
        debugf("LIBINDEX: could not fingerprint the tree, not caching\n");
        return false;
    }

    idx_record_t *recs = calloc((size_t)lib->count, sizeof(idx_record_t));
    char *strtab = NULL;
    uint32_t str_len = 0, str_cap = 0;

    if (recs == NULL) {
        free(sigs);
        return false;
    }

    /* Offset 0 is a lone NUL, so a zero offset is a valid empty string rather than an ambiguity
     * with "absent" -- absent is STR_NONE. */
    uint32_t scratch;
    bool strings_ok = str_push(&strtab, &str_len, &str_cap, "", &scratch);

    for (int i = 0; i < lib->count && strings_ok; i++) {
        const lib_record_t *r = &lib->records[i];
        recs[i].check_code = r->check_code;
        memcpy(recs[i].game_code, r->game_code, 4);
        recs[i].version   = r->version;
        recs[i].system    = r->system;
        recs[i].save_type = r->save_type;
        recs[i].feat      = r->feat;
        recs[i].dominant  = r->dominant;

        /* The favourite bit is playstate.dat's to own. Writing it here too would give two files
         * an opinion, and on the boot after the user un-favourites something the loser would win.
         * ART_MISSING is recorded here because a record whose art search came up empty at runtime
         * should not repeat the search next boot. */
        uint16_t flags = (uint16_t)(r->flags & ~LIBF_FAVORITE);
        if (r->art_state == ART_NONE) {
            flags |= LIBF_ART_MISSING;
        }
        recs[i].flags = flags;

        /* Through locals, not straight into the record. idx_record_t is __packed, so &recs[i]
         * .path_off is a pointer the compiler must assume is unaligned -- and on MIPS a 32-bit
         * store through one traps. -Werror=address-of-packed-member caught this on the first
         * build. The fields happen to be aligned here (40-byte stride, 8-byte-aligned base), but
         * "happens to be" is not something to write an unaligned store against. */
        uint32_t path_off = STR_NONE, title_off = STR_NONE, art_off = STR_NONE;
        strings_ok = str_push(&strtab, &str_len, &str_cap, r->path,     &path_off)  &&
                     str_push(&strtab, &str_len, &str_cap, r->title,    &title_off) &&
                     str_push(&strtab, &str_len, &str_cap, r->art_file, &art_off);
        recs[i].path_off  = path_off;
        recs[i].title_off = title_off;
        recs[i].art_off   = art_off;
    }

    if (!strings_ok || strtab == NULL) {
        /* No cache is a slow boot. A cache missing paths is a library whose games do not launch,
         * and it would pass its own CRC. Write nothing. */
        debugf("LIBINDEX: could not build the string table, not caching\n");
        free(strtab);
        free(recs);
        free(sigs);
        return false;
    }

    uint32_t rec_bytes = (uint32_t)lib->count * sizeof(idx_record_t);
    uint32_t sig_bytes = (uint32_t)sig_count * sizeof(dirsig_t);
    uint32_t total = sizeof(idx_payload_t) + rec_bytes + sig_bytes + str_len;

    char *payload = malloc(total);
    if (payload == NULL) {
        free(strtab);
        free(recs);
        free(sigs);
        return false;
    }

    idx_payload_t h = {
        .record_count = (uint32_t)lib->count,
        .dirsig_count = (uint32_t)sig_count,
        .records_off  = sizeof(idx_payload_t),
        .dirsigs_off  = sizeof(idx_payload_t) + rec_bytes,
        .strtab_off   = sizeof(idx_payload_t) + rec_bytes + sig_bytes,
        .strtab_bytes = str_len,
    };
    memcpy(payload, &h, sizeof(h));
    memcpy(payload + h.records_off, recs, rec_bytes);
    memcpy(payload + h.dirsigs_off, sigs, sig_bytes);
    memcpy(payload + h.strtab_off,  strtab, str_len);

    bool ok = cache_store(LIBINDEX_FILE, LIBINDEX_MAGIC, payload, total);
    if (ok) {
        debugf("LIBINDEX saved %d titles, %d dirs, %lu bytes\n",
               lib->count, sig_count, (unsigned long)total);
    }

    free(payload);
    free(strtab);
    free(recs);
    free(sigs);
    return ok;
}
