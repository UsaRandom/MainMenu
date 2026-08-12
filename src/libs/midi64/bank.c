/**
 * @file bank.c
 * @brief Sampled synth backend: plays instruments from a .bank64 file.
 *
 * The bank is produced offline by tools/mkbank.py from a SoundFont. Everything
 * expensive -- zone flattening, resampling, requantising -- happened there, so
 * this file does only what has to be fast: pick a region for a note, then step
 * through PCM with linear interpolation.
 *
 * The whole bank is read into RDRAM at load. The alternative, streaming each
 * voice from ROM, would mean up to MIDI64_MAX_VOICES concurrent PI DMAs with
 * random access patterns, which is far worse than spending 2 MB of the 8 MB
 * available. See docs/BANK.md.
 *
 * Format is big-endian throughout, so on the N64 the loaded image is used
 * directly with no byte swapping; the host renderer swaps on read instead,
 * which is the right way round -- the desktop has the cycles to spare.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "midi64_internal.h"

#define BANK_MAGIC   "MIDI64BK"
#define BANK_VERSION 1

#define HDR_SIZE     64
#define INDEX_SIZE   (128 * 2 * 4)
#define REGION_SIZE  20
#define SAMPLE_SIZE  16

/** @brief A playable zone: a key/velocity rectangle mapped to one sample. */
typedef struct {
    uint16_t sample;
    uint8_t  key_lo, key_hi;
    uint8_t  vel_lo, vel_hi;
    uint8_t  root;
    uint8_t  loop;
    int16_t  tune_cents;
    uint16_t atk_ms, dec_ms, rel_ms;
    int16_t  sustain;       /**< Q15 */
    int16_t  atten;         /**< Q15 gain, from SF2 initialAttenuation */
} m64_region_t;

typedef struct {
    uint32_t offset;        /**< Frame offset into the PCM pool */
    uint32_t length;        /**< Frames */
    uint32_t loop_start;
    uint32_t loop_end;      /**< <= loop_start means no loop */
} m64_bsample_t;

struct m64_bank_s {
    uint8_t       *image;   /**< Whole file, owned */
    size_t         size;
    uint32_t       rate;    /**< Rate the PCM was resampled to */
    int            bits;    /**< 8 or 16 */
    int32_t        gain;    /**< Q15 bank-wide makeup, see mkbank --gain */
    char           name[32];

    uint16_t       nregions, nsamples;
    m64_region_t  *regions; /**< Decoded, owned */
    m64_bsample_t *samples; /**< Decoded, owned */
    const void    *pcm;     /**< Points into @ref image */

    uint16_t prog_off[128], prog_cnt[128];
    uint16_t drum_off[128], drum_cnt[128];
};

/* ------------------------------------------------------------ big-endian -- */

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* ---------------------------------------------------------------- loading -- */

static void bank_free(struct m64_bank_s *b) {
    if (!b) return;
    free(b->regions);
    free(b->samples);
    free(b->image);
    free(b);
}

static struct m64_bank_s *bank_load(const char *path, int *err) {
    *err = MIDI64_ERR_IO;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < HDR_SIZE + INDEX_SIZE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); *err = MIDI64_ERR_FORMAT; return NULL;
    }

    struct m64_bank_s *b = calloc(1, sizeof(*b));
    if (!b) { fclose(f); *err = MIDI64_ERR_NOMEM; return NULL; }

    b->image = malloc((size_t)sz);
    if (!b->image) { fclose(f); bank_free(b); *err = MIDI64_ERR_NOMEM; return NULL; }
    b->size = (size_t)sz;
    size_t got = fread(b->image, 1, b->size, f);
    fclose(f);
    if (got != b->size) { bank_free(b); *err = MIDI64_ERR_IO; return NULL; }

    const uint8_t *h = b->image;
    if (memcmp(h, BANK_MAGIC, 8) != 0 || be32(h + 8) != BANK_VERSION) {
        bank_free(b); *err = MIDI64_ERR_FORMAT; return NULL;
    }

    b->rate     = be32(h + 12);
    b->nsamples = be16(h + 16);
    b->nregions = be16(h + 18);
    b->bits     = h[20];
    /* Stored Q12 so the field can express up to 16x; a uint16 of Q15 would
     * saturate at 2.0x and quietly ignore any larger --gain. */
    b->gain = (int32_t)be16(h + 22) << (M64_Q15 - 12);
    if (b->gain <= 0) b->gain = M64_ONE_Q15;
    uint32_t regions_off = be32(h + 24);
    uint32_t samples_off = be32(h + 28);
    uint32_t pcm_off     = be32(h + 32);
    uint32_t pcm_bytes   = be32(h + 36);
    memcpy(b->name, h + 40, 23);
    b->name[23] = '\0';

    if (b->bits != 8 && b->bits != 16) { bank_free(b); *err = MIDI64_ERR_FORMAT; return NULL; }
    if (b->rate == 0 || b->nregions == 0 || b->nsamples == 0) {
        bank_free(b); *err = MIDI64_ERR_FORMAT; return NULL;
    }
    /* Every offset is checked against the actual file length. A bank is an
     * asset that can be truncated by a bad build or a bad SD card, and a
     * malformed one must fail here rather than read past the image later. */
    if ((size_t)regions_off + (size_t)b->nregions * REGION_SIZE > b->size ||
        (size_t)samples_off + (size_t)b->nsamples * SAMPLE_SIZE > b->size ||
        (size_t)pcm_off + (size_t)pcm_bytes > b->size) {
        bank_free(b); *err = MIDI64_ERR_FORMAT; return NULL;
    }

    const uint8_t *ix = b->image + HDR_SIZE;
    for (int i = 0; i < 128; i++) {
        b->prog_off[i] = be16(ix + i * 2);
        b->prog_cnt[i] = be16(ix + 256 + i * 2);
        b->drum_off[i] = be16(ix + 512 + i * 2);
        b->drum_cnt[i] = be16(ix + 768 + i * 2);
    }

    b->regions = calloc(b->nregions, sizeof(m64_region_t));
    b->samples = calloc(b->nsamples, sizeof(m64_bsample_t));
    if (!b->regions || !b->samples) { bank_free(b); *err = MIDI64_ERR_NOMEM; return NULL; }

    for (int i = 0; i < b->nregions; i++) {
        const uint8_t *r = b->image + regions_off + i * REGION_SIZE;
        m64_region_t *d = &b->regions[i];
        d->sample     = be16(r);
        d->key_lo     = r[2];  d->key_hi = r[3];
        d->vel_lo     = r[4];  d->vel_hi = r[5];
        d->root       = r[6];
        d->loop       = r[7];
        d->tune_cents = (int16_t)be16(r + 8);
        d->atk_ms     = be16(r + 10);
        d->dec_ms     = be16(r + 12);
        d->rel_ms     = be16(r + 14);
        d->sustain    = (int16_t)be16(r + 16);
        d->atten      = (int16_t)be16(r + 18);
        if (d->sample >= b->nsamples) d->sample = 0;
    }

    uint32_t pcm_frames = pcm_bytes / (uint32_t)(b->bits / 8);
    for (int i = 0; i < b->nsamples; i++) {
        const uint8_t *s = b->image + samples_off + i * SAMPLE_SIZE;
        m64_bsample_t *d = &b->samples[i];
        d->offset     = be32(s);
        d->length     = be32(s + 4);
        d->loop_start = be32(s + 8);
        d->loop_end   = be32(s + 12);
        /* Clamp rather than reject: one bad sample record should cost that
         * instrument, not the whole bank. */
        if (d->offset > pcm_frames) d->offset = pcm_frames;
        if (d->offset + d->length > pcm_frames) d->length = pcm_frames - d->offset;
        if (d->loop_end > d->length) d->loop_end = d->length;
        if (d->loop_start >= d->loop_end) { d->loop_start = d->loop_end = 0; }
    }

    b->pcm = b->image + pcm_off;
    *err = MIDI64_OK;
    return b;
}

/* --------------------------------------------------------- region lookup -- */

/**
 * @brief First region of @p prog covering (@p key, @p vel).
 *
 * SF2 zones within an instrument are not supposed to overlap, so first match
 * is the right rule. When nothing matches -- which happens for keys outside
 * every zone's range -- the nearest region by key is used instead of falling
 * silent, because a missing note is far more noticeable than a stretched one.
 */
static const m64_region_t *pick_region(const struct m64_bank_s *b,
                                       int prog, bool drum, int key, int vel) {
    uint16_t off = drum ? b->drum_off[prog] : b->prog_off[prog];
    uint16_t cnt = drum ? b->drum_cnt[prog] : b->prog_cnt[prog];
    if (cnt == 0 || off >= b->nregions) return NULL;
    if ((uint32_t)off + cnt > b->nregions) cnt = (uint16_t)(b->nregions - off);

    const m64_region_t *best = NULL;
    int best_dist = 0x7fffffff;

    for (int i = 0; i < cnt; i++) {
        const m64_region_t *r = &b->regions[off + i];
        if (key >= r->key_lo && key <= r->key_hi &&
            vel >= r->vel_lo && vel <= r->vel_hi)
            return r;
        int d = (key < r->key_lo) ? (r->key_lo - key)
              : (key > r->key_hi) ? (key - r->key_hi) : 0;
        if (d < best_dist) { best_dist = d; best = r; }
    }
    return best;
}

/* ------------------------------------------------------------- backend ---- */

static int be_init(m64_synth_t *s, const midi64_config_t *cfg) {
    if (!cfg || !cfg->bank_path) return MIDI64_ERR_FORMAT;
    int err = MIDI64_OK;
    s->bank = bank_load(cfg->bank_path, &err);
    if (!s->bank) return err;
    return MIDI64_OK;
}

static void be_close(m64_synth_t *s) {
    bank_free(s->bank);
    s->bank = NULL;
}

static void be_start(m64_synth_t *s, m64_voice_t *v, const m64_channel_t *c) {
    const struct m64_bank_s *b = s->bank;
    bool drum = (v->channel == MIDI64_DRUM_CHANNEL);
    /* GM addresses a single drum kit as preset 0 of bank 128. Banks that ship
     * several kits still put the standard one there. */
    int prog = drum ? 0 : c->program;

    const m64_region_t *r = pick_region(b, prog, drum, v->note, v->velocity);
    if (!r) { v->stage = M64_ENV_IDLE; return; }

    const m64_bsample_t *sm = &b->samples[r->sample];
    if (sm->length == 0) { v->stage = M64_ENV_IDLE; return; }

    v->atk_ms     = r->atk_ms;
    v->dec_ms     = r->dec_ms;
    v->rel_ms     = r->rel_ms;
    v->sus_level  = r->sustain;
    /* Region attenuation times the bank's makeup, clamped. Q15 * Q15 >> 15 can
     * exceed unity here on purpose: the makeup is what brings a heavily
     * attenuated SoundFont up to the procedural backend's level. */
    int32_t g = (int32_t)(((int64_t)r->atten * b->gain) >> M64_Q15);
    v->patch_gain = g > 4 * M64_ONE_Q15 ? 4 * M64_ONE_Q15 : g;

    v->bk.bank.pcm = (b->bits == 8)
        ? (const void *)((const int8_t *)b->pcm + sm->offset)
        : (const void *)((const int16_t *)b->pcm + sm->offset);
    v->bk.bank.length = sm->length;
    /* Percussion never loops, whatever the region says.
     *
     * A one-shot voice gets no note-off -- a drum rings for as long as its
     * decay says -- so a looping region with a non-zero sustain level has
     * nothing that will ever end it. Against Aspirin that left 21 voices stuck
     * on after a single song and the player never reported done. Melodic voices
     * are safe because their note-off arrives and drives the release. */
    int looping = r->loop && !v->one_shot;
    v->bk.bank.loop_start = looping ? sm->loop_start : 0;
    v->bk.bank.loop_end   = looping ? sm->loop_end : 0;
    v->bk.bank.pos        = 0;

    /* Playback rate = (note frequency / root frequency) * (bank rate / output
     * rate). Both frequencies come from the same table so the output rate
     * cancels out of the first term; the second term is 1 whenever the bank was
     * converted at the playback rate, which is the intended case.
     *
     * The shift is done on the numerator before dividing, which is safe here:
     * inc values top out around 2.4e9 at 22 kHz, and 2.4e9 << 32 is 1.05e19
     * against a uint64 ceiling of 1.84e19. */
    int32_t pitch = v->pitch_q8 + ((int32_t)r->tune_cents * 256) / 100;
    uint32_t inc_note = m64_pitch_to_inc(s, pitch);
    uint32_t inc_root = m64_pitch_to_inc(s, (int32_t)r->root << 8);
    if (inc_root == 0) inc_root = 1;

    uint64_t step = ((uint64_t)inc_note << 32) / inc_root;
    if (b->rate != (uint32_t)s->samplerate)
        step = step * b->rate / (uint32_t)s->samplerate;
    /* 32x is five octaves up; beyond that the read stride outruns anything the
     * sample can usefully represent and only costs cache misses. */
    if (step > (32ULL << 32)) step = 32ULL << 32;
    v->bk.bank.step = step;

}

static void be_release(m64_synth_t *s, m64_voice_t *v) {
    if (v->stage != M64_ENV_IDLE && v->stage != M64_ENV_RELEASE)
        m64_env_enter(s, v, M64_ENV_RELEASE);
}

/**
 * @brief One sampled voice's contribution to a control block.
 *
 * @p bits8 and @p mono are constants at every call site. The first was a
 * per-sample test of a property of the whole bank; the second is the
 * centre-panned case, which drops one multiply per sample. See the same
 * treatment in synth.c's render_voice() for why a VR4300 cares.
 */
static inline __attribute__((always_inline))
void render_bank_voice(m64_voice_t *v, int32_t *out, int n,
                       int32_t gl, int32_t gr, int32_t dgl, int32_t dgr,
                       int bits8, int mono) {
    const int8_t  *p8  = (const int8_t *)v->bk.bank.pcm;
    const int16_t *p16 = (const int16_t *)v->bk.bank.pcm;
    uint64_t pos  = v->bk.bank.pos;
    uint64_t step = v->bk.bank.step;
    uint32_t len  = v->bk.bank.length;
    uint32_t ls   = v->bk.bank.loop_start;
    uint32_t le   = v->bk.bank.loop_end;

    for (int k = 0; k < n; k++) {
        uint32_t idx = (uint32_t)(pos >> 32);

        if (le > ls) {
            while (idx >= le) {
                pos -= (uint64_t)(le - ls) << 32;
                idx = (uint32_t)(pos >> 32);
            }
        } else if (idx >= len) {
            /* One-shot finished. Silence the voice and stop reading; the
             * envelope may still be in sustain if the file never sent a
             * note-off, and without this it would read past the sample. */
            v->stage = M64_ENV_IDLE;
            break;
        }

        uint32_t nxt = idx + 1;
        if (le > ls) { if (nxt >= le) nxt = ls; }
        else if (nxt >= len) nxt = idx;

        int32_t a, c2;
        if (bits8) { a = (int32_t)p8[idx] << 8;  c2 = (int32_t)p8[nxt] << 8; }
        else       { a = p16[idx];               c2 = p16[nxt]; }

        int32_t frac = (int32_t)((pos >> 16) & 0xffff);
        int32_t smp = a + (((c2 - a) * frac) >> 16);
        pos += step;

        if (mono) {
            int32_t o = (smp * gl) >> M64_Q15;
            out[2 * k]     += o;
            out[2 * k + 1] += o;
        } else {
            out[2 * k]     += (smp * gl) >> M64_Q15;
            out[2 * k + 1] += (smp * gr) >> M64_Q15;
            gr += dgr;
        }
        gl += dgl;
    }

    v->bk.bank.pos = pos;
}

static void be_render(m64_synth_t *s, int32_t *out, int n) {
    const struct m64_bank_s *b = s->bank;
    const int bits8 = (b->bits == 8);

    for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
        m64_voice_t *v = &s->voices[i];
        if (v->stage == M64_ENV_IDLE) continue;
        if (!v->bk.bank.pcm) { v->stage = M64_ENV_IDLE; continue; }

        int32_t e0 = v->env;
        int32_t e1 = m64_env_tick(s, v);

        int32_t gl0 = (e0 * v->gain_l) >> M64_Q15;
        int32_t gr0 = (e0 * v->gain_r) >> M64_Q15;
        int32_t gl1 = (e1 * v->gain_l) >> M64_Q15;
        int32_t gr1 = (e1 * v->gain_r) >> M64_Q15;
        int32_t dgl = m64_ramp_step(gl1 - gl0, n);
        int32_t dgr = m64_ramp_step(gr1 - gr0, n);

        const int mono = (v->gain_l == v->gain_r);

        if (bits8) {
            if (mono) render_bank_voice(v, out, n, gl0, gr0, dgl, dgr, 1, 1);
            else      render_bank_voice(v, out, n, gl0, gr0, dgl, dgr, 1, 0);
        } else {
            if (mono) render_bank_voice(v, out, n, gl0, gr0, dgl, dgr, 0, 1);
            else      render_bank_voice(v, out, n, gl0, gr0, dgl, dgr, 0, 0);
        }
    }
}

const m64_backend_t m64_backend_bank = {
    .name    = "bank",
    .init    = be_init,
    .close   = be_close,
    .start   = be_start,
    .release = be_release,
    .render  = be_render,
};

const char *m64_bank_name(const m64_synth_t *s) {
    return (s && s->bank) ? s->bank->name : "";
}
