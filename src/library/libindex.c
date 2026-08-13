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

/** Depth limit, and it is library.c's, not a copy of it.
 *
 *  This said 6 while the scan stopped at 5, under a comment claiming the two matched. The cost was
 *  small and entirely one-directional -- a change at the bottom level invalidated an index that
 *  could not have contained anything from there, so the card was rescanned in full and the rescan
 *  found nothing new. Incremental revalidation is what made it matter: a directory one level below
 *  the scan's reach would have been handed to library_scan_dir() and its games indexed, giving the
 *  incremental library titles that a full rescan of the same card does not have. The two walks
 *  agreeing is now load-bearing, so they share the number instead of each declaring one. */
#define SIG_MAX_DEPTH   LIBRARY_SCAN_MAX_DEPTH

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
    uint8_t  art_kind;          /**< art_kind_t, or ART_KIND_UNKNOWN; was a reserved byte */
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

/**
 * @brief The result of one signature walk.
 *
 * `sigs` alone is what staleness needs. `paths` and `art` are the extra the incremental path
 * needs and the save path does not: the full path of each directory, so a changed one can be
 * handed to library_scan_dir(), and the full path of every loose image on the card, so a game
 * added to one folder can still resolve a cover kept in another.
 *
 * Both are collected on every load, before it is known whether the incremental path will be taken
 * at all, because the walk is single-pass and asking again costs the whole walk over. On the
 * 278-cover card that is about 310 extra strdup/free pairs on a path that was already doing the
 * work of reading every one of those names -- and on the fully-fresh boot they are freed
 * immediately and nothing else changes.
 */
typedef struct {
    dirsig_t *sigs;
    char **paths;               /**< one per sig; NULL when not collected */
    int count;

    char **art;                 /**< full paths of loose images, in walk order; NULL when not */
    int art_count;
    int art_cap;

    bool detail;                /**< collect paths and art */
    bool overflow;
} sigwalk_t;

static void sig_free (sigwalk_t *w) {
    if (w->paths != NULL) {
        for (int i = 0; i < w->count; i++) {
            free(w->paths[i]);
        }
        free(w->paths);
    }
    for (int i = 0; i < w->art_count; i++) {
        free(w->art[i]);
    }
    free(w->art);
    free(w->sigs);
    memset(w, 0, sizeof(*w));
}

/** @brief Remember a loose image's full path. Silent on allocation failure; see sig_note_art(). */
static void sig_note_art (sigwalk_t *w, const char *full_path) {
    if (w->art_count == w->art_cap) {
        int want = w->art_cap ? w->art_cap * 2 : 64;
        char **bigger = realloc(w->art, (size_t)want * sizeof(*bigger));
        if (bigger == NULL) {
            /* Losing one art path costs a cover, not a game. Refusing the whole incremental
             * revalidation over it would cost a 14-second rescan instead, which is worse. */
            return;
        }
        w->art = bigger;
        w->art_cap = want;
    }
    char *copy = strdup(full_path);
    if (copy != NULL) {
        w->art[w->art_count++] = copy;
    }
}

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
static void sig_dir (sigwalk_t *w, const char *dir, int depth,
                     library_scan_progress_t on_tick) {
    if (depth > SIG_MAX_DEPTH || w->overflow) {
        return;
    }
    if (w->count >= SIG_MAX_DIRS) {
        w->overflow = true;
        return;
    }

    int self = w->count++;
    dirsig_t *sig = &w->sigs[self];
    sig->path_hash = cache_hash64(dir);
    sig->size_sum  = 0;
    sig->entries   = 0;
    sig->reserved  = 0;
    if (w->detail) {
        w->paths[self] = strdup(dir);
    }

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
                if (w->detail && library_is_art_name(info.d_name)) {
                    char full[512];
                    snprintf(full, sizeof(full), "%s/%s", dir, info.d_name);
                    sig_note_art(w, full);
                }
            }
        }
        /* Once per entry, exactly where library_scan_dir() ticks, and for a reason that has
         * nothing to do with progress: this walk is one blocking call that reads every directory
         * on the card, and until this existed nothing fed the mixer for the whole of it. The audio
         * buffers hold 8 x 640 samples at 16 kHz -- 320 ms -- so a revalidation of a full card ran
         * the AI dry and the music stalled on the boot plate for about a second. The callback
         * throttles itself; see boot_tick() in app.c. */
        if (on_tick != NULL) {
            on_tick(-1);
        }
        result = dir_findnext(dir, &info);
    }

    for (int i = 0; i < kid_count && !w->overflow; i++) {
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", dir, kids[i]);
        sig_dir(w, child, depth + 1, on_tick);
    }
    free(kids);
}

/**
 * @brief Fingerprint the whole tree under @p root into @p out.
 *
 * @param detail  Also collect each directory's path and every loose image's path. See sigwalk_t.
 * @return false if the walk could not be trusted, in which case @p out is empty.
 *
 * Images come out in exactly the order a scan would have pushed them: sig_dir() and scan_dir()
 * both take a directory's files before descending into its subdirectories, so "the shallowest
 * duplicate wins" means the same thing whichever walk built the table.
 */
static bool sig_collect (const char *storage_prefix, const char *root, bool detail,
                         library_scan_progress_t on_tick, sigwalk_t *out) {
    char base[512];
    library_join(base, sizeof(base), storage_prefix, root);

    memset(out, 0, sizeof(*out));
    out->detail = detail;
    out->sigs = calloc(SIG_MAX_DIRS, sizeof(dirsig_t));
    if (out->sigs == NULL) {
        return false;
    }
    if (detail) {
        out->paths = calloc(SIG_MAX_DIRS, sizeof(char *));
        if (out->paths == NULL) {
            sig_free(out);
            return false;
        }
    }

    sig_dir(out, base, 0, on_tick);

    if (out->overflow || out->count == 0) {
        sig_free(out);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ load */

/** @brief Bounds-checked strtab lookup. Returns NULL for STR_NONE or anything out of range. */
static const char *str_at (const char *strtab, uint32_t bytes, uint32_t off) {
    if (off == STR_NONE || off >= bytes) {
        return NULL;
    }
    return strtab + off;
}

/**
 * @brief Push one stored record onto @p lib, resolving its strings out of @p strtab.
 * @return false if the library could not grow, which the caller treats as the end of the load.
 */
static bool record_load (library_t *lib, const idx_record_t *rec,
                         const char *strtab, uint32_t strbytes) {
    lib_record_t *r = library_push(lib);
    if (r == NULL) {
        return false;
    }
    memset(r, 0, sizeof(*r));

    r->check_code = rec->check_code;
    memcpy(r->game_code, rec->game_code, 4);
    r->game_code[4] = '\0';
    r->version   = rec->version;
    r->system    = rec->system;
    r->save_type = rec->save_type;
    r->feat      = rec->feat;
    r->flags     = rec->flags;
    r->dominant  = rec->dominant;
    r->art_kind  = rec->art_kind;

    const char *s;
    if ((s = str_at(strtab, strbytes, rec->path_off))  != NULL) r->path     = strdup(s);
    if ((s = str_at(strtab, strbytes, rec->title_off)) != NULL) r->title    = strdup(s);
    if ((s = str_at(strtab, strbytes, rec->art_off))   != NULL) r->art_file = strdup(s);

    /* Art state is rebuilt, never restored. ART_READY refers to a slot in a RAM pool that does
     * not exist yet at this point in the boot, and restoring it would hand the grid a record
     * claiming to have art with nothing behind it. The one thing worth carrying over is the
     * negative: LIBF_ART_MISSING means the search already ran and found nothing. */
    r->art_state = (r->flags & LIBF_ART_MISSING) ? ART_NONE : ART_PENDING;
    r->art_age   = 1.0e9f;      /* already settled; no arrival pop for a cached record */
    return true;
}

/**
 * @brief Hash of the directory @p path sits in, as the walk would have hashed it.
 *
 * The record's own path is what attributes it to a directory, rather than a directory number
 * stored in the record. There is a spare 16-bit field that could have carried one, and not using
 * it is the point: a stored number is a second copy of a fact, and the failure it invites is an
 * index written by one build and read by another where the numbering shifted, which would show up
 * as records quietly assigned to the wrong directory and dropped or kept for the wrong reason.
 * The path is already there, it is already exact -- scan_dir() and sig_dir() build directory paths
 * with the same "%s/%s" join off the same root -- and it cannot go stale relative to itself.
 */
static uint64_t path_dir_hash (const char *path) {
    if (path == NULL) {
        return 0;
    }
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return 0;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) {
        n = 1;                  /* "/game.z64" sits in "/" */
    }
    char dir[512];
    if (n >= sizeof(dir)) {
        return 0;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';
    return cache_hash64(dir);
}

/**
 * @brief Find @p hash in @p arr, starting the search at @p hint and wrapping.
 *
 * Both walks are deterministic depth-first over mostly the same tree, so the directory that was
 * i-th last time is almost always i-th this time and the hint makes this a single comparison. The
 * wrap is what makes it correct anyway when a directory has been added or removed in the middle.
 */
static int sig_find (const dirsig_t *arr, int n, uint64_t hash, int hint) {
    for (int k = 0; k < n; k++) {
        int j = (hint + k) % n;
        if (arr[j].path_hash == hash) {
            return j;
        }
    }
    return -1;
}

static bool save_with_sigs (const library_t *lib, const dirsig_t *sigs, int sig_count);

/**
 * @brief Rebuild @p lib from the stored records plus a rescan of only the directories that moved.
 *
 * The whole reason this exists: adding one game to one folder used to cost a full rescan of the
 * card. Measured on the M64 at 289 titles that is 14.4 seconds, and the change it is reacting to
 * is confined to a single directory whose signature is the only one that moved -- the file already
 * knows which one, and threw that away to answer a yes/no question.
 *
 * Records are attributed to directories by their own path, kept when their directory's signature
 * matches, and dropped when it does not; every directory that is new or changed is then indexed
 * with no recursion, because its children are separately signed and separately decided. Union of
 * the two covers the tree exactly once.
 *
 * The result is merged, re-sorted with the same comparator a scan ends on, and written back with
 * the signatures this walk already produced -- so the next boot is a plain revalidation again.
 *
 * @return false if nothing could be salvaged, which leaves @p lib untouched for a full scan.
 */
static bool merge_incremental (library_t *lib, const idx_payload_t *h, const void *buf,
                               const char *strtab, const sigwalk_t *now,
                               library_scan_progress_t on_progress, libindex_result_t *res) {
    const dirsig_t *was = (const dirsig_t *)((const char *)buf + h->dirsigs_off);
    const idx_record_t *recs = (const idx_record_t *)((const char *)buf + h->records_off);

    bool *unchanged = calloc((size_t)now->count, sizeof(bool));
    if (unchanged == NULL) {
        return false;
    }

    int unchanged_n = 0;
    bool complete = true;
    for (int i = 0; i < now->count; i++) {
        int j = sig_find(was, (int)h->dirsig_count, now->sigs[i].path_hash, i);
        if (j >= 0 && was[j].entries == now->sigs[i].entries &&
                      was[j].size_sum == now->sigs[i].size_sum) {
            unchanged[i] = true;
            unchanged_n++;
        } else if (now->paths[i] == NULL) {
            /* A changed directory whose path could not be copied during the walk. There is no way
             * to read it again, so the repair would come out short by whatever is in it -- and
             * then write that shortfall back over a good index, where it would look settled on
             * every boot afterwards. A missing cover is worth accepting quietly; a missing game
             * is not. Checked here, before a single record is pushed, so falling back is free. */
            complete = false;
        }
    }

    /* Nothing survives, so there is nothing to be incremental about: fall back and let the caller
     * run the scan it would have run anyway, rather than reimplementing it one directory at a
     * time with the extra risk and none of the saving. */
    if (unchanged_n == 0 || !complete) {
        free(unchanged);
        return false;
    }

    /* Every image on the card, not just the ones in the directories about to be rescanned. A game
     * added to /M can perfectly well have its cover kept in /art, and resolving against a table
     * holding only /M would find nothing, mark the record LIBF_ART_MISSING, and write that answer
     * into the index where it would survive every later boot. */
    for (int i = 0; i < now->art_count; i++) {
        const char *slash = strrchr(now->art[i], '/');
        library_art_note(lib, slash ? slash + 1 : now->art[i], now->art[i]);
    }

    /* One pass per record over the directory list -- 289 records against 27 directories on the
     * card that prompted this, so about 7,800 64-bit comparisons. Sorting them to bisect would
     * save microseconds off a path whose other half opens ROM headers. */
    for (uint32_t i = 0; i < h->record_count; i++) {
        const char *path = str_at(strtab, h->strtab_bytes, recs[i].path_off);
        uint64_t dir = path_dir_hash(path);
        if (dir == 0) {
            /* No path, or one nothing could have written: the strtab offset did not resolve. Such
             * a record cannot be launched either -- grid_open() refuses a record with no path --
             * so it is dropped rather than carried forward. */
            continue;
        }
        int j = sig_find(now->sigs, now->count, dir, 0);
        if (j >= 0 && unchanged[j]) {
            if (!record_load(lib, &recs[i], strtab, h->strtab_bytes)) {
                /* Out of memory part way through. Same reasoning as the missing path above, and
                 * the same answer, except that records have been pushed by now -- so the library
                 * is emptied rather than merely abandoned, or the caller's full scan would append
                 * to it and every game kept so far would appear twice. */
                library_clear(lib);
                res->records_kept = 0;
                free(unchanged);
                return false;
            }
            res->records_kept++;
        }
    }

    for (int i = 0; i < now->count; i++) {
        if (unchanged[i]) {
            continue;
        }
        res->records_scanned += library_scan_dir(lib, now->paths[i], on_progress);
        res->dirs_rescanned++;
    }
    free(unchanged);

    /* Records loaded from the index already carry their art path; this is for the ones the rescan
     * just produced, and for any kept record whose cover has only now appeared somewhere else. */
    library_resolve_loose_art(lib);
    library_sort(lib);

    res->incremental = true;
    debugf("LIBINDEX incremental: kept %d, rescanned %d dirs for %d titles\n",
           res->records_kept, res->dirs_rescanned, res->records_scanned);

    /* Written back now, with the signatures already in hand, so the next boot is a plain
     * revalidation. Passing them in rather than letting libindex_save() take its own walk matters:
     * that walk is the expensive half of this path and doing it twice would halve the saving. */
    save_with_sigs(lib, now->sigs, now->count);
    return lib->count > 0;
}

bool libindex_load (library_t *lib, const char *storage_prefix, const char *root,
                    library_scan_progress_t on_progress, libindex_result_t *out) {
    libindex_result_t res;
    memset(&res, 0, sizeof(res));

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
     * point paying for hundreds of strdups first. The walk ticks -1 and the plate holds the last
     * real count, so seed it now or a warm revalidation paints 0 TITLES for the whole walk --
     * seconds, on the card this exists to cover -- and then jumps. Incremental rescans overwrite
     * it with a live lib->count. */
    if (on_progress != NULL) {
        on_progress((int)h->record_count);
    }
    uint32_t t0 = TICKS_READ();
    sigwalk_t now;
    bool walked = sig_collect(storage_prefix, root, true, on_progress, &now);
    uint32_t walk_us = TIMER_MICROS(TICKS_SINCE(t0));
    res.dirs_total = walked ? now.count : 0;
    if (walked) {
        for (int i = 0; i < now.count; i++) {
            library_note_dir(lib, now.paths != NULL ? now.paths[i] : NULL,
                             (int)now.sigs[i].entries);
        }
    }

    bool fresh = (walked && now.count == (int)h->dirsig_count);
    if (fresh) {
        const dirsig_t *was = (const dirsig_t *)((const char *)buf + h->dirsigs_off);
        for (int i = 0; i < now.count; i++) {
            /* Position-dependent, and it stays that way for the yes/no answer: the walk is
             * deterministic and depth-first over the same tree, so if every directory is where it
             * was with the contents it had, this is the fastest way to say so. The incremental
             * path below is the one that has to match directories by hash, because by then the
             * tree really has moved. */
            if (now.sigs[i].path_hash != was[i].path_hash ||
                now.sigs[i].entries   != was[i].entries   ||
                now.sigs[i].size_sum  != was[i].size_sum) {
                fresh = false;
                break;
            }
        }
    }

    if (!fresh) {
        /* Not "rescan the card" any more. Something moved, and the signatures say which
         * directories -- so the ones that did not move keep their records and only the rest are
         * read off the card again. A full scan is what happens when even that cannot be salvaged:
         * the walk itself failed, or nothing at all matched. */
        bool merged = walked && merge_incremental(lib, h, buf, strtab, &now, on_progress, &res);
        if (!merged) {
            debugf("LIBINDEX: card changed (%lu us to check) -- rescanning in full\n",
                   (unsigned long)walk_us);
        }
        sig_free(&now);
        free(buf);
        if (out != NULL) {
            *out = res;
        }
        return merged;
    }
    sig_free(&now);

    const idx_record_t *recs = (const idx_record_t *)((const char *)buf + h->records_off);
    for (uint32_t i = 0; i < h->record_count; i++) {
        if (!record_load(lib, &recs[i], strtab, h->strtab_bytes)) {
            break;
        }
    }
    res.records_kept = lib->count;

    uint32_t total_us = TIMER_MICROS(TICKS_SINCE(t0));
    debugf("LIBINDEX loaded %d titles in %lu us (%lu us of that revalidating %d dirs)\n",
           lib->count, (unsigned long)total_us, (unsigned long)walk_us, (int)h->dirsig_count);

    free(buf);
    if (out != NULL) {
        *out = res;
    }
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

bool libindex_save (const library_t *lib, const char *storage_prefix, const char *root,
                    library_scan_progress_t on_progress) {
    if (!cache_writable() || lib->count == 0) {
        return false;
    }

    sigwalk_t w;
    if (!sig_collect(storage_prefix, root, false, on_progress, &w)) {
        debugf("LIBINDEX: could not fingerprint the tree, not caching\n");
        return false;
    }
    bool ok = save_with_sigs(lib, w.sigs, w.count);
    sig_free(&w);
    return ok;
}

/**
 * @brief Write @p lib against signatures the caller already has.
 *
 * Split out for the incremental path, which has just taken the walk and would otherwise pay for a
 * second identical one -- the walk being the whole cost it exists to avoid. Safe to reuse them:
 * nothing between the two writes to any directory the walk looks at. The index itself lands in
 * `mainmenu/cache`, which library_scan_skipped() keeps out of the walk for exactly this reason.
 */
static bool save_with_sigs (const library_t *lib, const dirsig_t *sigs, int sig_count) {
    if (!cache_writable() || lib->count == 0) {
        return false;
    }

    idx_record_t *recs = calloc((size_t)lib->count, sizeof(idx_record_t));
    char *strtab = NULL;
    uint32_t str_len = 0, str_cap = 0;

    if (recs == NULL) {
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
        recs[i].art_kind  = r->art_kind;

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
        return false;
    }

    uint32_t rec_bytes = (uint32_t)lib->count * sizeof(idx_record_t);
    uint32_t sig_bytes = (uint32_t)sig_count * sizeof(dirsig_t);
    uint32_t total = sizeof(idx_payload_t) + rec_bytes + sig_bytes + str_len;

    char *payload = malloc(total);
    if (payload == NULL) {
        free(strtab);
        free(recs);
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
    return ok;
}
