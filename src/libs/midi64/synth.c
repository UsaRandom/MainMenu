/**
 * @file synth.c
 * @brief Voice pool, pitch tables and the procedural (wavetable) backend.
 *
 * There is no floating point and no libm anywhere in this file, including the
 * table generation. That is not a performance argument -- the VR4300 has a
 * usable FPU and none of this is in a hot enough loop to care. It is so that a
 * host render and an N64 render produce byte-identical output: pow() and sinf()
 * are allowed to differ in the last ULP between libm implementations, and a
 * one-ULP difference in a wavetable is a difference in every sample that reads
 * it. Integer tables make `cmp host.wav n64.wav` a real test.
 *
 * Aliasing is handled with mipmapped band-limited tables: nine sets of
 * wavetables, one per octave, each summing only the harmonics that fit under
 * Nyquist for the highest note in its octave. A naive saw is unusable above
 * about MIDI note 60 at 22 kHz -- the aliased partials fold down into the
 * middle of the mix and the whole arrangement sounds detuned.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "midi64_internal.h"

/* ------------------------------------------------------ fixed-point sine -- */

/* Minimax coefficients for sin(pi/2 * u), u in [0,1], in Q30. Error is under
 * 2e-9, well below the 1/32768 the table can represent. */
#define A1   1686629713
#define A3   (-693598668)
#define A5     85569306
#define A7     (-5026995)
#define A9       172272

/**
 * @brief sin(2*pi * phase / 2^32) in Q30.
 *
 * The full circle is one wrap of a uint32, which is exactly the oscillator's
 * phase format, so this doubles as the table generator and the pan law.
 */
static int32_t sin_q30(uint32_t phase) {
    uint32_t q = phase >> 30;                    /* quadrant */
    int64_t  u = (int64_t)(phase & 0x3fffffffu); /* position in quadrant, Q30 */
    if (q & 1) u = (1ll << 30) - u;              /* quadrants 1,3 mirror */

    int64_t u2 = (u * u) >> 30;
    int64_t y = A9;
    y = A7 + ((u2 * y) >> 30);
    y = A5 + ((u2 * y) >> 30);
    y = A3 + ((u2 * y) >> 30);
    y = A1 + ((u2 * y) >> 30);
    y = (u * y) >> 30;

    return (q & 2) ? (int32_t)(-y) : (int32_t)y;
}

/* ------------------------------------------------------- pitch tables ---- */

/** Frequencies of MIDI notes 0..11 in Q16.16 Hz. Every other note is one of
 *  these shifted left by its octave, which is exact. */
static const uint32_t note_base_q16[12] = {
    535809, 567670, 601425, 637188, 675077, 715219,
    757749, 802807, 850544, 901120, 954703, 1011473
};

/** 2^(1/3072) in Q32 -- one 256th of a semitone. Accumulating this 255 times
 *  drifts by 2e-8, which is 0.00003 cents. */
#define SEMITONE_256_Q32  4295936495ULL

/** @brief Frequency of a MIDI note in Q16.16 Hz. */
static uint32_t note_freq_q16(int note) {
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return note_base_q16[note % 12] << (note / 12);
}

static void build_pitch_tables(m64_synth_t *s) {
    for (int n = 0; n < 128; n++) {
        /* inc = freq * 2^32 / samplerate, with freq already Q16, so the shift
         * is 32-16 = 16. Note 127 at 22050 Hz gives 0x91..., comfortably inside
         * a uint32; anything that overflowed would be above Nyquist anyway. */
        uint64_t inc = ((uint64_t)note_freq_q16(n) << 16) / (uint64_t)s->samplerate;
        s->inc_note[n] = (inc > 0xffffffffULL) ? 0xffffffffU : (uint32_t)inc;
    }
    uint64_t r = 1ULL << 32;
    for (int f = 0; f < 256; f++) {
        s->inc_frac[f] = (uint32_t)(r >> 16);          /* Q16 ratio */
        r = (r * SEMITONE_256_Q32) >> 32;
    }
}

uint32_t m64_pitch_to_inc(const m64_synth_t *s, int32_t pitch_q8) {
    if (pitch_q8 < 0) pitch_q8 = 0;
    if (pitch_q8 > (127 << 8)) pitch_q8 = 127 << 8;
    uint32_t n = (uint32_t)pitch_q8 >> 8;
    uint32_t f = (uint32_t)pitch_q8 & 0xff;
    return (uint32_t)(((uint64_t)s->inc_note[n] * s->inc_frac[f]) >> 16);
}

/* ----------------------------------------------------- wavetable build --- */

/** @brief Harmonics summed for the lowest octave.
 *
 * Nyquist at 22050 Hz over the 8 Hz fundamental of octave 0 allows 1300-odd
 * harmonics; the table can only represent 512 and the difference above 256 is
 * inaudible against a 1/k rolloff. Capping here is what keeps table generation
 * from dominating init -- see docs/PERF.md for the measurement. */
#define MAX_HARMONICS  256

/** @brief Which mipmap level a note reads from. One level per octave, starting
 *  at M64_WT_BASE_NOTE; anything below that clamps to level 0. */
static inline int level_for_note(int note) {
    int l = (note - M64_WT_BASE_NOTE) / 12;
    if (l < 0) l = 0;
    return (l >= M64_WT_LEVELS) ? M64_WT_LEVELS - 1 : l;
}

/** @brief Highest harmonic that stays under Nyquist for every note in a level. */
static int harmonics_for_level(const m64_synth_t *s, int level) {
    int top_note = (level == M64_WT_LEVELS - 1)
                 ? 127 : (M64_WT_BASE_NOTE + level * 12 + 11);
    uint32_t f_q16 = note_freq_q16(top_note);
    if (f_q16 == 0) return 1;
    /* nyquist / f, both Q16, so the shift cancels */
    uint32_t nyq_q16 = ((uint32_t)s->samplerate / 2) << 16;
    int k = (int)(nyq_q16 / f_q16);
    if (k < 1) k = 1;
    if (k > MAX_HARMONICS) k = MAX_HARMONICS;
    if (k > M64_WT_LEN / 2) k = M64_WT_LEN / 2;   /* Nyquist of the table itself */
    return k;
}

/**
 * @brief Amplitude of harmonic @p k for @p shape, in Q16. Zero means absent.
 *
 * These are the textbook Fourier series. The signs matter for the waveform's
 * shape but not for what it sounds like; they are here so the tables actually
 * look like a saw and a triangle when dumped, which is how the generator was
 * checked in the first place.
 */
static int32_t harmonic_amp(int shape, int k) {
    switch (shape) {
        case M64_SHAPE_SINE:
            return (k == 1) ? 65536 : 0;

        case M64_SHAPE_TRIANGLE:
            if ((k & 1) == 0) return 0;
            /* 1/k^2, sign flipping every other odd harmonic */
            return (((k - 1) / 2) & 1) ? -(65536 / (k * k)) : (65536 / (k * k));

        case M64_SHAPE_SAW:
            return 65536 / k;

        case M64_SHAPE_SQUARE:
            return ((k & 1) == 0) ? 0 : (65536 / k);

        case M64_SHAPE_PULSE25:
            /* Duty d: amplitude ~ sin(pi*k*d)/k. With d = 1/4 that is
             * sin(2*pi * k/8), which sin_q30 gives directly at phase k<<29. */
            return (int32_t)((sin_q30((uint32_t)k << 29) >> 14) / k);

        default:
            return 0;
    }
}

/**
 * @brief Additively synthesise one band-limited table.
 *
 * @p sinq30 is a precomputed one-period sine in Q30, M64_WT_LEN entries.
 *
 * The phase step for harmonic k is (2^32 / M64_WT_LEN) * k, which for a
 * power-of-two table is exactly 2^22 * k -- so every phase this loop visits is
 * a multiple of 2^22 and the sine argument only ever takes M64_WT_LEN distinct
 * values. Indexing a table is therefore not an approximation of calling
 * sin_q30() here, it is the identical result.
 *
 * The first version called sin_q30() in the inner loop: ~2.4 million polynomial
 * evaluations of five 64-bit multiplies each, which measured 3.1 SECONDS to
 * build the full set in ares. The table reduces that to M64_WT_LEN evaluations
 * total, done once by the caller.
 */
static void build_table(int16_t *dst, int shape, int nharm, const int32_t *sinq30) {
    static int64_t acc[M64_WT_LEN];   /* 8 KB, init-only; the alternative is a
                                       * malloc that can fail after the caller
                                       * has already committed to this level. */
    memset(acc, 0, sizeof(acc));

    for (int k = 1; k <= nharm; k++) {
        int32_t amp = harmonic_amp(shape, k);
        if (amp == 0) continue;
        /* Walking the index by k and masking is the same sequence the phase
         * accumulator produced, without the accumulator. */
        int idx = 0;
        for (int i = 0; i < M64_WT_LEN; i++) {
            acc[i] += ((int64_t)sinq30[idx] * amp) >> 16;
            idx = (idx + k) & (M64_WT_LEN - 1);
        }
    }

    /* Normalise to a fixed peak. Per-table normalisation (rather than a shared
     * scale) is why the patch table carries its own gain trim: two tables that
     * both peak at 32000 do not carry the same energy. */
    int64_t peak = 1;
    for (int i = 0; i < M64_WT_LEN; i++) {
        int64_t a = acc[i] < 0 ? -acc[i] : acc[i];
        if (a > peak) peak = a;
    }
    for (int i = 0; i < M64_WT_LEN; i++) {
        int64_t v = (acc[i] * 32000) / peak;
        dst[i] = (int16_t)v;
    }
}

/**
 * @brief Shared wavetable set, refcounted across players.
 *
 * The tables are a pure function of (shape, level, sample rate) -- nothing about
 * a song or a patch enters into them -- so every player at a given rate wants
 * byte-identical 90 KB. Building them per player cost 90 KB per player and,
 * more importantly, a full table generation every time the caller switched
 * songs. On the host that is ~3.5 ms; scaled to a 93 MHz VR4300 it is a hitch
 * measured in hundreds of milliseconds, on exactly the code path a menu uses
 * to change track.
 *
 * Not thread-safe, and does not need to be: libdragon calls the mixer from one
 * context, and midi64_player_init() is not something to do from an interrupt.
 */
static struct {
    int16_t *wt;
    int      samplerate;
    int      refcount;
} wt_cache;

static int build_wavetables(m64_synth_t *s) {
    /* Reuse whenever the cached set matches the rate, *regardless of refcount*.
     * Gating this on refcount > 0 only helps players that overlap in time, and
     * the case that actually matters is the opposite one: close song A, open
     * song B. That path drops the count to zero between the two, so a
     * refcount-gated check rebuilt the tables on every single track change --
     * measured as 2x cheaper instead of the 500x it should be. */
    if (wt_cache.wt && wt_cache.samplerate == s->samplerate) {
        wt_cache.refcount++;
        s->wt = wt_cache.wt;
        s->sine = &s->wt[(size_t)M64_SHAPE_SINE * M64_WT_LEVELS * M64_WT_LEN];
        return MIDI64_OK;
    }
    /* A live set at a different rate cannot be shared and cannot be freed
     * either -- someone is still reading it. This one gets its own copy and
     * frees it directly; only the cached set is refcounted. */

    size_t n = (size_t)M64_WT_SHAPES * M64_WT_LEVELS * M64_WT_LEN;
    s->wt = malloc(n * sizeof(int16_t));
    if (!s->wt) return MIDI64_ERR_NOMEM;

    /* One period of sine in Q30, the generator for every table below. Built
     * with M64_WT_LEN polynomial evaluations instead of the ~2.4 million the
     * inner loops would otherwise perform. Static rather than on the stack:
     * 4 KB is more than libdragon's default stack wants to give up. */
    static int32_t sinq30[M64_WT_LEN];
    for (int i = 0; i < M64_WT_LEN; i++)
        sinq30[i] = sin_q30((uint32_t)i << M64_WT_SHIFT);

    for (int shape = 0; shape < M64_WT_SHAPES; shape++) {
        for (int lvl = 0; lvl < M64_WT_LEVELS; lvl++) {
            int nharm = harmonics_for_level(s, lvl);
            /* A pure sine needs no mipmapping, and building it nine times is
             * nine times the cost for identical bytes. */
            if (shape == M64_SHAPE_SINE && lvl > 0) {
                memcpy(&s->wt[((size_t)shape * M64_WT_LEVELS + lvl) * M64_WT_LEN],
                       &s->wt[((size_t)shape * M64_WT_LEVELS) * M64_WT_LEN],
                       M64_WT_LEN * sizeof(int16_t));
                continue;
            }
            build_table(&s->wt[((size_t)shape * M64_WT_LEVELS + lvl) * M64_WT_LEN],
                        shape, nharm, sinq30);
        }
    }
    s->sine = &s->wt[(size_t)M64_SHAPE_SINE * M64_WT_LEVELS * M64_WT_LEN];

    if (wt_cache.refcount == 0) {
        /* Nobody is reading the old set, so this rate's tables become the
         * cached ones. If a player at the old rate is still live, its tables
         * stay private and it frees them itself. */
        free(wt_cache.wt);
        wt_cache.wt = s->wt;
        wt_cache.samplerate = s->samplerate;
        wt_cache.refcount = 1;
    }
    return MIDI64_OK;
}

/** @brief Release this synth's claim on the wavetables.
 *
 * The last reference does *not* free: the tables are kept so the next player at
 * the same rate starts instantly, which is the whole point. They are freed only
 * when a player at a different rate displaces them, or by
 * midi64_synth_free_tables(). */
static void release_wavetables(m64_synth_t *s) {
    if (s->wt == wt_cache.wt) {
        if (wt_cache.refcount > 0) wt_cache.refcount--;
    } else {
        free(s->wt);                    /* private copy at an odd rate */
    }
    s->wt = NULL;
    s->sine = NULL;
}

int midi64_synth_prepare(int samplerate) {
    if (wt_cache.wt && wt_cache.samplerate == samplerate) return MIDI64_OK;

    /* Build through a throwaway synth rather than duplicating build_wavetables'
     * caching rules here. Closing it drops the refcount to zero, which by
     * design leaves the tables in the cache for the next real player. */
    m64_synth_t *s = malloc(sizeof(m64_synth_t));
    if (!s) return MIDI64_ERR_NOMEM;

    midi64_config_t cfg = { MIDI64_SYNTH_PROCEDURAL, NULL, false, 0 };
    int err = m64_synth_init(s, samplerate, &cfg);
    if (err == MIDI64_OK) m64_synth_close(s);
    free(s);
    return err;
}

void midi64_synth_free_tables(void) {
    if (wt_cache.refcount == 0) {
        free(wt_cache.wt);
        wt_cache.wt = NULL;
        wt_cache.samplerate = 0;
    }
}

static const int16_t *table_for(const m64_synth_t *s, int shape, int note) {
    int lvl = level_for_note(note);
    return &s->wt[((size_t)shape * M64_WT_LEVELS + lvl) * M64_WT_LEN];
}

/* ------------------------------------------------------------ envelopes -- */

/** @brief Envelope steps for a stage lasting @p ms, at least one block long. */
static int32_t env_blocks(const m64_synth_t *s, int ms) {
    int32_t b = ((int32_t)ms * s->samplerate) / (1000 * M64_CTRL_BLOCK);
    return b < 1 ? 1 : b;
}

static void env_enter(m64_synth_t *s, m64_voice_t *v, m64_env_stage_t st,
                      const m64_patch_t *p) {
    v->stage = st;
    switch (st) {
        case M64_ENV_ATTACK:
            v->env_target = M64_ONE_Q15;
            v->env_step = (v->env_target - v->env) / env_blocks(s, p->attack_ms);
            if (v->env_step <= 0) v->env_step = 1;
            break;
        case M64_ENV_DECAY:
            v->env_target = p->sustain;
            v->env_step = (v->env_target - v->env) / env_blocks(s, p->decay_ms);
            if (v->env_step >= 0) v->env_step = -1;
            break;
        case M64_ENV_SUSTAIN:
            v->env_target = p->sustain;
            v->env_step = 0;
            break;
        case M64_ENV_RELEASE:
            v->env_target = 0;
            v->env_step = -v->env / env_blocks(s, p->release_ms);
            if (v->env_step >= 0) v->env_step = -1;
            break;
        case M64_ENV_IDLE:
            v->env = 0; v->env_step = 0; v->env_target = 0;
            break;
    }
}

/** @brief Advance one voice's envelope by one control block.
 *  @return the level at the end of the block; the caller ramps to it. */
static int32_t env_tick(m64_synth_t *s, m64_voice_t *v) {
    const m64_patch_t *p = (v->channel == MIDI64_DRUM_CHANNEL)
                         ? m64_patch_for_drum(v->note, NULL)
                         : m64_patch_for_program(v->bk.patch);

    if (v->stage == M64_ENV_IDLE || v->stage == M64_ENV_SUSTAIN) return v->env;

    int32_t e = v->env + v->env_step;

    if (v->env_step > 0 ? (e >= v->env_target) : (e <= v->env_target)) {
        e = v->env_target;
        switch (v->stage) {
            case M64_ENV_ATTACK:
                v->env = e; env_enter(s, v, M64_ENV_DECAY, p);
                break;
            case M64_ENV_DECAY:
                v->env = e;
                /* A patch with no sustain level is one-shot: the decay *is* the
                 * whole note, and reaching the floor frees the voice. This is
                 * what makes pianos and drums release their voices without ever
                 * seeing a note-off. */
                if (p->sustain > 0) env_enter(s, v, M64_ENV_SUSTAIN, p);
                else                env_enter(s, v, M64_ENV_IDLE, p);
                break;
            case M64_ENV_RELEASE:
                v->env = e; env_enter(s, v, M64_ENV_IDLE, p);
                break;
            default: break;
        }
        return v->env;
    }

    v->env = e;
    return e;
}

/* ---------------------------------------------------------------- gains -- */

/** @brief Recompute a voice's stereo gains from velocity, channel and patch. */
static void update_gains(m64_synth_t *s, m64_voice_t *v, const m64_channel_t *c) {
    const m64_patch_t *p = (v->channel == MIDI64_DRUM_CHANNEL)
                         ? m64_patch_for_drum(v->note, NULL)
                         : m64_patch_for_program(v->bk.patch);
    (void)s;

    /* Velocity is squared, which is much closer to how loudness is perceived
     * than the linear mapping. Without it, quiet accompaniment tracks sit far
     * too close to the melody. */
    int32_t vel = ((int32_t)v->velocity * M64_ONE_Q15) / 127;
    vel = (vel * vel) >> M64_Q15;

    int32_t vol = ((int32_t)c->volume * M64_ONE_Q15) / 127;
    int32_t exp = ((int32_t)c->expression * M64_ONE_Q15) / 127;

    int32_t g = (vel * vol) >> M64_Q15;
    g = (g * exp) >> M64_Q15;
    g = (g * p->gain) >> M64_Q15;

    /* Constant-power pan straight off the sine polynomial: pan 0 reads sin(0)
     * and pan 127 reads sin(pi/2), so the pair sums to unity power at every
     * point including the centre, where linear panning dips by 3 dB.
     *
     * The shift must happen in 64 bits. `pan << 30` overflows uint32_t for any
     * pan >= 4, and the wrapped value put centre-panned channels at roughly
     * -25 dBFS while still sounding perfectly normal -- just very quiet. */
    uint32_t pan = c->pan > 127 ? 127 : c->pan;
    int32_t pr = sin_q30((uint32_t)(((uint64_t)pan << 30) / 127)) >> 15;
    int32_t pl = sin_q30((uint32_t)(((uint64_t)(127 - pan) << 30) / 127)) >> 15;

    v->gain_l = (g * pl) >> M64_Q15;
    v->gain_r = (g * pr) >> M64_Q15;
}

static void update_pitch(m64_synth_t *s, m64_voice_t *v, const m64_channel_t *c) {
    int32_t p = v->base_pitch_q8;
    /* Percussion keys select a drum, they do not name a pitch, so bending the
     * channel must not retune the kit. */
    if (v->channel != MIDI64_DRUM_CHANNEL && c->bend != 0) {
        p += ((int32_t)c->bend * (int32_t)c->bend_range * 256) / 8192;
    }
    v->pitch_q8 = p;
    v->phase_inc = m64_pitch_to_inc(s, p);
}

/* ------------------------------------------------------ voice allocation -- */

static m64_voice_t *alloc_voice(m64_synth_t *s) {
    m64_voice_t *best = NULL;

    /* 1. An idle voice, if there is one. */
    for (int i = 0; i < MIDI64_MAX_VOICES; i++)
        if (s->voices[i].stage == M64_ENV_IDLE) return &s->voices[i];

    /* 2. Otherwise the quietest voice already in release. Stealing a note that
     * is already fading is the steal nobody hears. */
    int32_t quietest = 0x7fffffff;
    for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
        m64_voice_t *v = &s->voices[i];
        if (v->stage == M64_ENV_RELEASE && v->env < quietest) {
            quietest = v->env; best = v;
        }
    }
    if (best) return best;

    /* 3. Last resort: the oldest sounding note. Stealing by age rather than by
     * amplitude keeps sustained chords intact and takes the note most likely to
     * have already been superseded. */
    uint32_t oldest = 0xffffffffu;
    for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
        if (s->voices[i].age < oldest) { oldest = s->voices[i].age; best = &s->voices[i]; }
    }
    return best;
}

void m64_synth_note_on(m64_synth_t *s, const m64_channel_t *c, int chn, int note, int vel) {
    const m64_patch_t *p;
    int32_t pitch;

    if (chn == MIDI64_DRUM_CHANNEL) {
        p = m64_patch_for_drum(note, &pitch);
    } else {
        p = m64_patch_for_program(c->program);
        pitch = (int32_t)note << 8;
    }

    m64_voice_t *v = alloc_voice(s);
    if (!v) return;

    memset(v, 0, sizeof(*v));
    v->channel  = (uint8_t)chn;
    v->note     = (uint8_t)note;
    v->velocity = (uint8_t)vel;
    v->age      = ++s->age_counter;
    v->one_shot = (chn == MIDI64_DRUM_CHANNEL);
    v->held     = !v->one_shot;
    v->base_pitch_q8 = pitch;
    v->bk.patch = (uint8_t)(chn == MIDI64_DRUM_CHANNEL ? note : c->program);

    update_pitch(s, v, c);
    update_gains(s, v, c);

    v->bk.table = table_for(s, p->shape, pitch >> 8);
    v->bk.noise_mix = p->noise_mix;
    /* Seeding from the age counter rather than a constant stops every snare hit
     * from being the identical waveform, which is audible as a machine-gun
     * quality on fast rolls. Any non-zero seed works; an LFSR that reaches zero
     * stays there. */
    v->bk.noise_state = 0x9e3779b9u ^ (v->age * 2654435761u);
    if (v->bk.noise_state == 0) v->bk.noise_state = 1;

    if (p->cutoff_hz > 0) {
        /* One-pole corner: coeff = 2*pi*fc/sr, valid while fc is well under
         * Nyquist. 205887 is 2*pi in Q15. */
        int32_t coeff = (int32_t)(((int64_t)p->cutoff_hz * 205887) / s->samplerate);
        if (coeff > M64_ONE_Q15) coeff = M64_ONE_Q15;
        if (coeff < 1) coeff = 1;
        v->bk.lp_coeff = coeff;
    } else {
        v->bk.lp_coeff = 0;
    }

    v->env = 0;
    env_enter(s, v, M64_ENV_ATTACK, p);
}

void m64_synth_note_off(m64_synth_t *s, const m64_channel_t *c, int chn, int note) {
    /* Drum note-offs are meaningless: the hit rings for as long as its decay
     * says. Files routinely send a note-off one tick after the note-on, and
     * honouring it would truncate every cymbal to nothing. */
    if (chn == MIDI64_DRUM_CHANNEL) return;

    for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
        m64_voice_t *v = &s->voices[i];
        if (v->stage == M64_ENV_IDLE || v->stage == M64_ENV_RELEASE) continue;
        if (v->channel != chn || v->note != note || !v->held) continue;

        v->held = false;
        if (c->sustain) {
            v->sustained = true;      /* pedal holds it until CC64 goes down */
        } else {
            s->be->release(s, v);
        }
        /* No break: a file that sends two note-ons for the same key before any
         * note-off has two voices sounding, and one note-off should not leave
         * the other stuck on forever. */
    }
}

void m64_synth_all_off(m64_synth_t *s, int chn, bool immediate) {
    for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
        m64_voice_t *v = &s->voices[i];
        if (v->stage == M64_ENV_IDLE) continue;
        if (chn >= 0 && v->channel != chn) continue;
        if (immediate) {
            v->stage = M64_ENV_IDLE;
            v->env = 0;
            v->held = false;
            v->sustained = false;
        } else {
            v->held = false;
            v->sustained = false;
            s->be->release(s, v);
        }
    }
}

void m64_synth_update_channel(m64_synth_t *s, const m64_channel_t *c, int chn) {
    for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
        m64_voice_t *v = &s->voices[i];
        if (v->stage == M64_ENV_IDLE || v->channel != chn) continue;
        update_pitch(s, v, c);
        update_gains(s, v, c);
    }
}

int m64_synth_active(const m64_synth_t *s) {
    int n = 0;
    for (int i = 0; i < MIDI64_MAX_VOICES; i++)
        if (s->voices[i].stage != M64_ENV_IDLE) n++;
    return n;
}

/* ------------------------------------------------- procedural backend ----- */

static int be_init(m64_synth_t *s, const midi64_config_t *cfg) {
    (void)cfg;
    return build_wavetables(s);
}

static void be_close(m64_synth_t *s) {
    release_wavetables(s);
}

static void be_start(m64_synth_t *s, m64_voice_t *v, const m64_channel_t *c) {
    /* Everything a procedural voice needs is set up in m64_synth_note_on(); the
     * hook exists so the sampled backend has somewhere to open its region. */
    (void)s; (void)v; (void)c;
}

static void be_release(m64_synth_t *s, m64_voice_t *v) {
    const m64_patch_t *p = (v->channel == MIDI64_DRUM_CHANNEL)
                         ? m64_patch_for_drum(v->note, NULL)
                         : m64_patch_for_program(v->bk.patch);
    if (v->stage != M64_ENV_IDLE && v->stage != M64_ENV_RELEASE)
        env_enter(s, v, M64_ENV_RELEASE, p);
}

/** @brief 32-bit xorshift. Cheap, and its spectrum is flat enough for a
 *  percussion source at this sample rate. */
static inline uint32_t noise_next(uint32_t *st) {
    uint32_t x = *st;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *st = x;
    return x;
}

/**
 * @brief Mix every sounding voice into @p out for one control block.
 *
 * @p out is interleaved stereo int32 accumulators, already zeroed by the caller.
 * @p n must not exceed M64_CTRL_BLOCK: the envelope is evaluated once per call
 * and ramped linearly across the block.
 */
static void be_render(m64_synth_t *s, int32_t *out, int n) {
    for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
        m64_voice_t *v = &s->voices[i];
        if (v->stage == M64_ENV_IDLE) continue;

        int32_t e0 = v->env;
        int32_t e1 = env_tick(s, v);

        /* Gain is ramped per sample rather than held per block. A 2 ms attack
         * spans barely one block at 22 kHz, so a stepped envelope puts an
         * audible click on the front of every piano note. */
        int32_t gl0 = (e0 * v->gain_l) >> M64_Q15;
        int32_t gr0 = (e0 * v->gain_r) >> M64_Q15;
        int32_t gl1 = (e1 * v->gain_l) >> M64_Q15;
        int32_t gr1 = (e1 * v->gain_r) >> M64_Q15;
        int32_t dgl = (gl1 - gl0) / n;
        int32_t dgr = (gr1 - gr0) / n;

        const int16_t *tab = v->bk.table;
        uint32_t ph  = v->phase;
        uint32_t inc = v->phase_inc;
        uint32_t ns  = v->bk.noise_state;
        int32_t  nm  = v->bk.noise_mix;
        int32_t  lp  = v->bk.lp_state;
        int32_t  lpc = v->bk.lp_coeff;
        int32_t  gl  = gl0, gr = gr0;

        for (int k = 0; k < n; k++) {
            uint32_t idx  = ph >> M64_WT_SHIFT;
            int32_t  frac = (int32_t)((ph >> (M64_WT_SHIFT - 16)) & 0xffff);
            int32_t  a = tab[idx];
            int32_t  b = tab[(idx + 1) & (M64_WT_LEN - 1)];
            int32_t  smp = a + (((b - a) * frac) >> 16);
            ph += inc;

            if (nm) {
                int32_t nz = (int32_t)(noise_next(&ns) >> 16) - 32768;
                smp += ((nz - smp) * nm) >> M64_Q15;
            }
            if (lpc) {
                lp += ((smp - lp) * lpc) >> M64_Q15;
                smp = lp;
            }

            out[2 * k]     += (smp * gl) >> M64_Q15;
            out[2 * k + 1] += (smp * gr) >> M64_Q15;
            gl += dgl;
            gr += dgr;
        }

        v->phase = ph;
        v->bk.noise_state = ns;
        v->bk.lp_state = lp;
    }
}

const m64_backend_t m64_backend_procedural = {
    .name    = "procedural",
    .init    = be_init,
    .close   = be_close,
    .start   = be_start,
    .release = be_release,
    .render  = be_render,
};

/* ------------------------------------------------------------ synth API -- */

int m64_synth_init(m64_synth_t *s, int samplerate, const midi64_config_t *cfg) {
    memset(s, 0, sizeof(*s));
    s->samplerate = samplerate;

    switch (cfg ? cfg->synth : MIDI64_SYNTH_PROCEDURAL) {
        case MIDI64_SYNTH_BANK:
            /* Not built yet. Falling back silently would ship a build that
             * ignores the bank_path it was handed, so say no instead. */
            return MIDI64_ERR_UNSUPPORTED;
        case MIDI64_SYNTH_PROCEDURAL:
        default:
            s->be = &m64_backend_procedural;
            break;
    }

    build_pitch_tables(s);
    return s->be->init(s, cfg);
}

void m64_synth_close(m64_synth_t *s) {
    if (!s || !s->be) return;
    s->be->close(s);
    s->be = NULL;
}

void m64_synth_render(m64_synth_t *s, int32_t *out, int nframes) {
    s->be->render(s, out, nframes);
}
