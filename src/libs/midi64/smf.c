/**
 * @file smf.c
 * @brief Standard MIDI File parsing and track cursor management.
 *
 * The file image is kept whole and never copied into an event list. Everything
 * here works on cursors pointing into that image. A malformed file must fail
 * the parse rather than fault during playback, so every cursor advance is
 * bounds-checked against the chunk end -- including the ones that "cannot"
 * overrun, because a truncated download is a perfectly ordinary way to get a
 * chunk whose declared length exceeds the bytes actually present.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "midi64_internal.h"

/* ------------------------------------------------------------------ helpers */

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

bool m64_read_vlq(const uint8_t **p, const uint8_t *end, uint32_t *out) {
    uint32_t v = 0;
    const uint8_t *q = *p;
    /* The spec caps a VLQ at four bytes. Enforcing that matters: without it a
     * run of 0x80 bytes in a corrupt file spins until it hits `end`, and the
     * accumulated shift is undefined long before that. */
    for (int i = 0; i < 4; i++) {
        if (q >= end) return false;
        uint8_t b = *q++;
        v = (v << 7) | (uint32_t)(b & 0x7f);
        if (!(b & 0x80)) { *p = q; *out = v; return true; }
    }
    return false;
}

/** @brief Bytes of parameter data an event occupies, excluding its status byte.
 *
 * Returns -1 for meta and sysex, which carry an explicit length and are handled
 * by the caller. */
static int channel_event_len(uint8_t status) {
    switch (status & 0xf0) {
        case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0: return 2;
        case 0xc0: case 0xd0: return 1;
        default: return -1;
    }
}

/* ------------------------------------------------------------ track cursors */

/** @brief Read the delta-time at the cursor and fold it into @ref next_tick.
 *
 * Leaves @ref cur pointing at the event's status byte. Marks the track done on
 * a truncated or malformed delta, which is the only sane response: we cannot
 * know when the next event fires, so there is no next event. */
static void track_read_delta(m64_track_t *t) {
    if (t->done || t->cur >= t->end) { t->done = true; return; }
    uint32_t dt;
    if (!m64_read_vlq(&t->cur, t->end, &dt)) { t->done = true; return; }
    t->next_tick += dt;
}

void m64_track_reset(m64_track_t *t) {
    t->cur = t->base;
    t->next_tick = 0;
    t->running_status = 0;
    t->done = (t->base >= t->end);
    track_read_delta(t);
}

/** @brief Decode the event at the cursor far enough to know its extent.
 *
 * On return @p *status holds the effective status byte (resolving running
 * status), @p *data points at the parameter bytes and @p *len is their count.
 * Returns false and marks the track done if the event runs past the chunk. */
static bool track_peek(m64_track_t *t, uint8_t *status,
                       const uint8_t **data, uint32_t *len) {
    if (t->done || t->cur >= t->end) { t->done = true; return false; }

    const uint8_t *p = t->cur;
    uint8_t st = *p;

    if (st & 0x80) {
        p++;
        /* Running status is cancelled by system messages but survives sysex's
         * cousin F7 only by accident in the wild; the spec says cancel, so we
         * cancel. Channel statuses become the new running status. */
        if (st < 0xf0) t->running_status = st;
        else t->running_status = 0;
    } else {
        st = t->running_status;
        /* A data byte with no preceding status is unrecoverable: we cannot know
         * how many bytes to consume, so the rest of the chunk is unreadable. */
        if (st == 0) { t->done = true; return false; }
    }

    if (st == 0xff) {                       /* meta: FF <type> <vlq len> <data> */
        if (p >= t->end) { t->done = true; return false; }
        uint8_t type = *p++;
        uint32_t l;
        if (!m64_read_vlq(&p, t->end, &l)) { t->done = true; return false; }
        if ((uint32_t)(t->end - p) < l)    { t->done = true; return false; }
        /* The length VLQ sits between the type byte and the payload, so the
         * type cannot be handed back as data[0] -- an earlier version returned
         * `p - 1` for that and every song title came out as the length byte
         * followed by the payload's first n-1 bytes. */
        t->meta_type = type;
        *status = 0xff;
        *data = p;
        *len = l;
        t->cur = p + l;
        return true;
    }

    if (st == 0xf0 || st == 0xf7) {         /* sysex: F0/F7 <vlq len> <data> */
        uint32_t l;
        if (!m64_read_vlq(&p, t->end, &l)) { t->done = true; return false; }
        if ((uint32_t)(t->end - p) < l)    { t->done = true; return false; }
        *status = st; *data = p; *len = l;
        t->cur = p + l;
        return true;
    }

    int n = channel_event_len(st);
    if (n < 0) { t->done = true; return false; }   /* F1..F6: not valid in an SMF */
    if ((int)(t->end - p) < n) { t->done = true; return false; }
    *status = st; *data = p; *len = (uint32_t)n;
    t->cur = p + n;
    return true;
}

/** @brief Consume the event at the cursor and read the next delta-time.
 *
 * track_peek() has already advanced @ref cur past the event body, so this only
 * has to queue up the next one. */
static void track_commit(m64_track_t *t) {
    if (t->cur >= t->end) { t->done = true; return; }
    track_read_delta(t);
}

bool m64_track_next(m64_track_t *t, uint8_t *status, const uint8_t **data, uint32_t *len) {
    return track_peek(t, status, data, len);
}

void m64_track_advance(m64_track_t *t) { track_commit(t); }

/* ------------------------------------------------------------------- parse */

int m64_smf_parse(midi64_song_t *song) {
    const uint8_t *d = song->data;
    size_t n = song->size;

    if (n < 14 || memcmp(d, "MThd", 4) != 0) return MIDI64_ERR_FORMAT;

    uint32_t hlen = rd32(d + 4);
    /* The header is defined as 6 bytes but the spec explicitly allows it to
     * grow, so trust the declared length rather than assuming 6. */
    if (hlen < 6 || hlen > n - 8) return MIDI64_ERR_FORMAT;

    song->format  = rd16(d + 8);
    song->ntracks = rd16(d + 10);
    uint16_t div  = rd16(d + 12);

    if (song->format > 2) return MIDI64_ERR_FORMAT;
    /* Format 2 is a bag of independent sequences with no defined simultaneous
     * playback. Rendering it as if it were format 1 would silently produce
     * something that is not the file, so refuse it instead. */
    if (song->format == 2) return MIDI64_ERR_UNSUPPORTED;
    if (song->ntracks == 0) return MIDI64_ERR_FORMAT;

    /* Bit 15 set selects SMPTE timecode division. No file in the reference
     * corpus uses it and supporting it properly means a second clock domain,
     * so it is refused rather than misinterpreted as PPQN. */
    if (div & 0x8000) return MIDI64_ERR_UNSUPPORTED;
    if (div == 0) return MIDI64_ERR_FORMAT;
    song->ppqn = div;

    song->tracks = calloc(song->ntracks, sizeof(m64_track_t));
    if (!song->tracks) return MIDI64_ERR_NOMEM;

    const uint8_t *p = d + 8 + hlen;
    const uint8_t *fend = d + n;
    unsigned found = 0;

    while (found < song->ntracks && (size_t)(fend - p) >= 8) {
        uint32_t clen = rd32(p + 4);
        if (memcmp(p, "MTrk", 4) != 0) {
            /* Unknown chunk types are to be skipped, per the spec. */
            if ((size_t)(fend - p - 8) < clen) break;
            p += 8 + clen;
            continue;
        }
        /* A chunk claiming more bytes than the file holds is truncation. Clamp
         * to what is actually there and play what we have -- refusing the whole
         * song would be worse, and every cursor advance is bounds-checked. */
        if ((size_t)(fend - p - 8) < clen) clen = (uint32_t)(fend - p - 8);

        m64_track_t *t = &song->tracks[found++];
        t->base = p + 8;
        t->end  = p + 8 + clen;
        p = t->end;
    }

    if (found == 0) { free(song->tracks); song->tracks = NULL; return MIDI64_ERR_FORMAT; }

    /* Header count and actual chunk count disagreeing is common in files
     * written by buggy exporters. The chunks present are the truth. */
    song->ntracks = (uint16_t)found;
    return MIDI64_OK;
}

/* ---------------------------------------------------------- public loading */

int midi64_song_open_mem(midi64_song_t *song, const uint8_t *data, size_t size) {
    memset(song, 0, sizeof(*song));
    song->data = (uint8_t *)data;   /* borrowed; owns_data stays false */
    song->size = size;
    song->owns_data = false;
    int err = m64_smf_parse(song);
    if (err != MIDI64_OK) { song->data = NULL; song->size = 0; }
    return err;
}

int midi64_song_load(midi64_song_t *song, const char *path) {
    memset(song, 0, sizeof(*song));

    FILE *f = fopen(path, "rb");
    if (!f) return MIDI64_ERR_IO;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return MIDI64_ERR_IO; }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return MIDI64_ERR_IO; }

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return MIDI64_ERR_NOMEM; }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return MIDI64_ERR_IO; }

    song->data = buf;
    song->size = (size_t)sz;
    song->owns_data = true;

    int err = m64_smf_parse(song);
    if (err != MIDI64_OK) { free(buf); memset(song, 0, sizeof(*song)); }
    return err;
}

void midi64_song_close(midi64_song_t *song) {
    if (!song) return;
    if (song->owns_data) free(song->data);
    free(song->tracks);
    memset(song, 0, sizeof(*song));
}

/* --------------------------------------------------------------- metadata */

bool midi64_song_title(const midi64_song_t *song, char *out, size_t outsz) {
    if (!out || outsz == 0) return false;
    out[0] = '\0';
    if (!song || !song->tracks) return false;

    /* Only track 0. In format 1 that is the conductor track and meta 0x03 there
     * is the *sequence* name; in every later track the same event is the track
     * or instrument name. Searching all tracks for the first 0x03 is what an
     * earlier version did, and it reported the reference corpus as songs called
     * "Bass", "Drumkit" and "Marimba" -- those files carry no sequence name at
     * all, and the honest answer for them is that they have no title.
     * Format 0 has a single track, which is both conductor and content. */
    m64_track_t t = song->tracks[0];
    m64_track_reset(&t);
    while (!t.done) {
        uint8_t st; const uint8_t *d; uint32_t len;
        if (!m64_track_next(&t, &st, &d, &len)) break;
        if (st == 0xff && t.meta_type == 0x03 && len > 0) {
            size_t n = len;
            if (n >= outsz) n = outsz - 1;
            memcpy(out, d, n);
            out[n] = '\0';
            return true;
        }
        /* The name lives at tick 0; once notes start there is no point looking. */
        if ((st & 0xf0) == 0x90) break;
        m64_track_advance(&t);
    }
    return false;
}

uint32_t midi64_song_duration_ms(const midi64_song_t *song) {
    if (!song || !song->tracks) return 0;

    m64_track_t *tr = calloc(song->ntracks, sizeof(m64_track_t));
    if (!tr) return 0;
    for (int i = 0; i < song->ntracks; i++) {
        tr[i] = song->tracks[i];
        m64_track_reset(&tr[i]);
    }

    uint32_t usec_per_qn = 500000;   /* 120 BPM until told otherwise */
    uint32_t tick = 0;
    uint64_t usec = 0;

    for (;;) {
        /* k-way merge: soonest track wins, ties broken by track order so the
         * result does not depend on iteration direction. */
        int best = -1;
        uint32_t best_tick = 0;
        for (int i = 0; i < song->ntracks; i++) {
            if (tr[i].done) continue;
            if (best < 0 || tr[i].next_tick < best_tick) {
                best = i; best_tick = tr[i].next_tick;
            }
        }
        if (best < 0) break;

        if (best_tick > tick) {
            usec += (uint64_t)(best_tick - tick) * usec_per_qn / song->ppqn;
            tick = best_tick;
        }

        uint8_t st; const uint8_t *d; uint32_t len;
        if (!m64_track_next(&tr[best], &st, &d, &len)) continue;
        if (st == 0xff && tr[best].meta_type == 0x51 && len >= 3) {
            uint32_t t = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
            if (t) usec_per_qn = t;
        }
        m64_track_advance(&tr[best]);
    }

    free(tr);
    return (uint32_t)(usec / 1000);
}
