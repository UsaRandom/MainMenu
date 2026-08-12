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
 * Aliasing is handled with mipmapped band-limited tables: M64_WT_LEVELS sets
 * of wavetables, one per octave, each summing only the harmonics that fit
 * under Nyquist for the highest note in its octave. A naive saw is unusable
 * above about MIDI note 60 at 22 kHz -- the aliased partials fold down into
 * the middle of the mix and the whole arrangement sounds detuned.
 *
 * The tables are **not** all the same length. Each is sized to the harmonics
 * it carries, and the bands that come out at one harmonic -- every sine level,
 * and the top level of every shape -- share a single 64-entry table. See
 * plan_tables() and M64_WT_MIN_RATIO. Uniform lengths cost 71,680 bytes read
 * at a stride proportional to the length, against an 8 KB data cache; sized
 * lengths cost 31,872 at 22050 Hz.
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

/** 2^(1/3072) -- one 256th of a semitone -- in **Q30**, not Q32.
 *
 * Q32 is the natural format here and it is wrong: the accumulator step is
 * `r = (r * SEMITONE) >> 30` with both operands near full scale, and in Q32
 * that product is 1.8455e19 against a uint64 ceiling of 1.8447e19. It overflows
 * on the very first multiply and every entry after inc_frac[0] came out as 14
 * or 15 instead of ~65536.
 *
 * The effect was invisible for an unbent note, whose sub-semitone index is
 * always 0, and catastrophic for anything else: a bent note played at 1/4369 of
 * its frequency, and every sampled region with fine tuning did the same. It
 * survived because tools/pitchtest.py had no pitch-bend case. It does now.
 *
 * Q30 leaves the product at 1.15e18, comfortably inside the range, and the
 * accumulated drift over 255 steps is one part in 65536 -- 0.026 cents. */
#define SEMITONE_256_Q30  1073984124ULL

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
    uint64_t r = 1ULL << 30;
    for (int f = 0; f < 256; f++) {
        s->inc_frac[f] = (uint32_t)(r >> 14);          /* Q30 -> Q16 ratio */
        r = (r * SEMITONE_256_Q30) >> 30;
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
    if (k > M64_WT_MAX_LEN / 2) k = M64_WT_MAX_LEN / 2;  /* table's own Nyquist */
    return k;
}

/**
 * @brief Harmonics the table for (@p shape, @p level) actually carries.
 *
 * Shape enters into it for exactly one reason, and it is worth the extra
 * function: a sine is one harmonic at *every* level. The band-limit that
 * shrinks a saw as it climbs has nothing left to take off a sine, so all seven
 * sine levels hold the same samples -- and, once the length is derived from
 * this number, the same 64 of them.
 */
static int eff_harmonics(const m64_synth_t *s, int shape, int level) {
    return (shape == M64_SHAPE_SINE) ? 1 : harmonics_for_level(s, level);
}

/** @brief Shortest power-of-two table giving @p nharm harmonics at least
 *  M64_WT_MIN_RATIO samples per period of the top one. */
static int wt_len_for(int nharm) {
    int len = M64_WT_MIN_LEN;
    while (len < nharm * M64_WT_MIN_RATIO && len < M64_WT_MAX_LEN) len <<= 1;
    return len;
}

static int wt_bits_for(int len) {
    int b = 0;
    while ((1 << b) < len) b++;
    return b;
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
static void build_table(int16_t *dst, int len, int shape, int nharm,
                        const int32_t *sinq30) {
    static int64_t acc[M64_WT_MAX_LEN]; /* 8 KB, init-only; the alternative is a
                                         * malloc that can fail after the caller
                                         * has already committed to this level. */
    memset(acc, 0, (size_t)len * sizeof(acc[0]));

    /* sinq30 is one period at M64_WT_MAX_LEN resolution and every table length
     * is a power-of-two divisor of it, so harmonic k of a len-point table
     * steps through it by exactly k * (MAX_LEN / len). Still not an
     * approximation: every phase this visits is one sinq30 holds exactly. */
    const int gen = M64_WT_MAX_LEN / len;

    for (int k = 1; k <= nharm; k++) {
        int32_t amp = harmonic_amp(shape, k);
        if (amp == 0) continue;
        /* Walking the index by k and masking is the same sequence the phase
         * accumulator produced, without the accumulator. */
        int idx = 0;
        const int step = k * gen;
        for (int i = 0; i < len; i++) {
            acc[i] += ((int64_t)sinq30[idx] * amp) >> 16;
            idx = (idx + step) & (M64_WT_MAX_LEN - 1);
        }
    }

    /* Normalise to a fixed peak. Per-table normalisation (rather than a shared
     * scale) is why the patch table carries its own gain trim: two tables that
     * both peak at 32000 do not carry the same energy. */
    int64_t peak = 1;
    for (int i = 0; i < len; i++) {
        int64_t a = acc[i] < 0 ? -acc[i] : acc[i];
        if (a > peak) peak = a;
    }

    /* One reciprocal and len multiplies rather than len divides. A VR4300
     * `ddiv` is 69 cycles against 8 for `dmult`, and this ran once per sample
     * of every table at boot. peak is shifted down into 30 bits first so the
     * reciprocal is never coarser than one part in 64000 -- well under one LSB
     * of a 32000 full scale, and the shift is 3 or 0 in practice. */
    int sh = 0;
    while ((peak >> sh) > 0x3fffffff) sh++;
    const int64_t recip = ((int64_t)32000 << 31) / (peak >> sh);
    for (int i = 0; i < len; i++)
        dst[i] = (int16_t)(((acc[i] >> sh) * recip) >> 31);
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
    int16_t     *wt;
    m64_wtdesc_t desc[M64_WT_SHAPES][M64_WT_LEVELS];
    int          samplerate;
    int          refcount;
} wt_cache;

/**
 * @brief Decide where every table lives, and return the total entry count.
 *
 * Two kinds of sharing happen here, and neither is a special case bolted on:
 * both fall out of asking each band how many harmonics it carries.
 *
 *   - A band with one harmonic is a sine. After per-table normalisation the
 *     amplitude that harmonic started with is gone, so a one-harmonic saw, a
 *     one-harmonic square and a sine are the *same samples*. At every sample
 *     rate tried, level 6 is one harmonic for all five shapes (the top of that
 *     band is note 127, whose second harmonic is above Nyquist even at 44100),
 *     and a sine is one harmonic at all seven levels. That is 11 of the 35
 *     (shape, level) pairs pointing at a single 64-entry table.
 *   - The rest are sized by wt_len_for(), so the length falls with the band.
 *
 * Together those took the set from 71,680 bytes of which 20 KB was one sine
 * stored eleven times, to about 16 KB at 22050 Hz.
 *
 * The offsets are uint16_t, which cannot overflow: the absolute worst case is
 * every one of the 35 pairs at M64_WT_MAX_LEN, or 35,840 entries.
 */
static size_t plan_tables(m64_synth_t *s) {
    size_t off = 0;
    int    shared = -1;   /* where the single-harmonic sine went, once placed */

    for (int shape = 0; shape < M64_WT_SHAPES; shape++) {
        for (int lvl = 0; lvl < M64_WT_LEVELS; lvl++) {
            int nh  = eff_harmonics(s, shape, lvl);
            int len = wt_len_for(nh);
            size_t at;

            if (nh == 1) {
                if (shared < 0) { shared = (int)off; off += (size_t)len; }
                at = (size_t)shared;
            } else {
                at = off;
                off += (size_t)len;
            }

            s->wt_desc[shape][lvl].off   = (uint16_t)at;
            s->wt_desc[shape][lvl].mask  = (uint16_t)(len - 1);
            s->wt_desc[shape][lvl].shift = (uint8_t)(32 - wt_bits_for(len));
        }
    }
    return off;
}

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
        memcpy(s->wt_desc, wt_cache.desc, sizeof(s->wt_desc));
        return MIDI64_OK;
    }
    /* A live set at a different rate cannot be shared and cannot be freed
     * either -- someone is still reading it. This one gets its own copy and
     * frees it directly; only the cached set is refcounted. */

    size_t n = plan_tables(s);
    s->wt = malloc(n * sizeof(int16_t));
    if (!s->wt) return MIDI64_ERR_NOMEM;

    /* One period of sine in Q30, the generator for every table below. Built
     * with M64_WT_MAX_LEN polynomial evaluations instead of the ~2.4 million
     * the inner loops would otherwise perform. Static rather than on the
     * stack: 4 KB is more than libdragon's default stack wants to give up. */
    static int32_t sinq30[M64_WT_MAX_LEN];
    for (int i = 0; i < M64_WT_MAX_LEN; i++)
        sinq30[i] = sin_q30((uint32_t)i << (32 - M64_WT_MAX_BITS));

    /* The shared single-harmonic table, built through the sine's own level 0
     * because that is the pair plan_tables() placed it at. Every other band
     * that came out at one harmonic aliases this range. */
    const m64_wtdesc_t *sd = &s->wt_desc[M64_SHAPE_SINE][0];
    build_table(&s->wt[sd->off], sd->mask + 1, M64_SHAPE_SINE, 1, sinq30);

    for (int shape = 0; shape < M64_WT_SHAPES; shape++) {
        for (int lvl = 0; lvl < M64_WT_LEVELS; lvl++) {
            int nharm = eff_harmonics(s, shape, lvl);
            if (nharm == 1) continue;      /* shares the table built above */
            const m64_wtdesc_t *d = &s->wt_desc[shape][lvl];
            build_table(&s->wt[d->off], d->mask + 1, shape, nharm, sinq30);
        }
    }

    if (wt_cache.refcount == 0) {
        /* Nobody is reading the old set, so this rate's tables become the
         * cached ones. If a player at the old rate is still live, its tables
         * stay private and it frees them itself. */
        free(wt_cache.wt);
        wt_cache.wt = s->wt;
        memcpy(wt_cache.desc, s->wt_desc, sizeof(wt_cache.desc));
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

/** @brief Point a voice at the band-limited table for its shape and note, and
 *  give it the shift and mask that go with that table's length. */
static void select_table(const m64_synth_t *s, m64_voice_t *v, int shape, int note) {
    const m64_wtdesc_t *d = &s->wt_desc[shape][level_for_note(note)];
    v->bk.proc.table = &s->wt[d->off];
    v->bk.proc.mask  = d->mask;
    v->bk.proc.shift = d->shift;
}

/* ------------------------------------------------------------ envelopes -- */

/** @brief Envelope steps for a stage lasting @p ms, at least one block long. */
static int32_t env_blocks(const m64_synth_t *s, int ms) {
    int32_t b = ((int32_t)ms * s->samplerate) / (1000 * M64_CTRL_BLOCK);
    return b < 1 ? 1 : b;
}

void m64_env_enter(m64_synth_t *s, m64_voice_t *v, m64_env_stage_t st) {
    v->stage = st;
    switch (st) {
        case M64_ENV_ATTACK:
            v->env_target = M64_ONE_Q15;
            v->env_step = (v->env_target - v->env) / env_blocks(s, v->atk_ms);
            if (v->env_step <= 0) v->env_step = 1;
            break;
        case M64_ENV_DECAY:
            v->env_target = v->sus_level;
            v->env_step = (v->env_target - v->env) / env_blocks(s, v->dec_ms);
            if (v->env_step >= 0) v->env_step = -1;
            break;
        case M64_ENV_SUSTAIN:
            v->env_target = v->sus_level;
            v->env_step = 0;
            break;
        case M64_ENV_RELEASE:
            v->env_target = 0;
            v->env_step = -v->env / env_blocks(s, v->rel_ms);
            if (v->env_step >= 0) v->env_step = -1;
            break;
        case M64_ENV_IDLE:
            v->env = 0; v->env_step = 0; v->env_target = 0;
            break;
    }
}

/** @brief Advance one voice's envelope by one control block.
 *  @return the level at the end of the block; the caller ramps to it. */
int32_t m64_env_tick(m64_synth_t *s, m64_voice_t *v) {
    if (v->stage == M64_ENV_IDLE || v->stage == M64_ENV_SUSTAIN) return v->env;

    int32_t e = v->env + v->env_step;

    if (v->env_step > 0 ? (e >= v->env_target) : (e <= v->env_target)) {
        e = v->env_target;
        switch (v->stage) {
            case M64_ENV_ATTACK:
                v->env = e; m64_env_enter(s, v, M64_ENV_DECAY);
                break;
            case M64_ENV_DECAY:
                v->env = e;
                /* A voice with no sustain level is one-shot: the decay *is* the
                 * whole note, and reaching the floor frees the voice. This is
                 * what makes pianos and drums release without a note-off. */
                if (v->sus_level > 0) m64_env_enter(s, v, M64_ENV_SUSTAIN);
                else                  m64_env_enter(s, v, M64_ENV_IDLE);
                break;
            case M64_ENV_RELEASE:
                v->env = e; m64_env_enter(s, v, M64_ENV_IDLE);
                break;
            default: break;
        }
        return v->env;
    }

    v->env = e;
    return e;
}

/* ---------------------------------------------------------------- gains -- */

/** @brief Constant-power pan gains in Q15, indexed by CC10. Held in uint16_t
 *  because hard-over is sin(pi/2) == M64_ONE_Q15 == 32768, one past int16_t. */
static uint16_t pan_l_q15[128], pan_r_q15[128];
static bool     pan_ready;

/**
 * @brief Build the pan table.
 *
 * This used to be two sin_q30() evaluations and two 64-bit divisions *per
 * voice*, run on every note-on, every CC7/10/11 and -- until the update split
 * -- every pitch bend. Pan takes 128 values; there was never a reason to
 * compute it.
 *
 * The position mapping changed with the table, and deliberately. `pan / 127`
 * puts CC10 64 at 0.50394 rather than dead centre: a 0.1 dB channel imbalance
 * on every channel that never sends CC10, which is most of them. Centring on
 * 64 instead (`0.5 + (pan - 64) / 127`, clamped) is both more correct -- GM
 * says 64 is centre -- and what lets gain_l == gain_r, which is the test
 * be_render() uses to drop to one multiply per sample. The ends still go hard
 * over: pan 0 clamps to 0 and pan 127 lands at 0.996.
 */
static void build_pan_table(void) {
    if (pan_ready) return;
    for (int p = 0; p < 128; p++) {
        /* In 64 bits, for the same reason the code this replaced said so:
         * `(p - 64) << 30` overflows an int32 for any p >= 66, and the wrap
         * put pan 127 at 0.492 instead of 0.996 -- a hard-right channel
         * rendered very nearly centred. Caught by bisecting a +5.7 dB band
         * shift in the left channel of "14. Traveling The Sky", which pans a
         * lead to 127. */
        int64_t pos = ((((int64_t)p - 64) << 30) / 127) + (1 << 29);
        if (pos < 0) pos = 0;
        if (pos > (1 << 30)) pos = 1 << 30;
        pan_r_q15[p] = (uint16_t)(sin_q30((uint32_t)pos) >> 15);
        pan_l_q15[p] = (uint16_t)(sin_q30((uint32_t)((1 << 30) - pos)) >> 15);
    }
    pan_ready = true;
}

/** @brief Recompute a voice's stereo gains from velocity, channel and patch. */
static void update_gains(m64_synth_t *s, m64_voice_t *v, const m64_channel_t *c) {
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
    g = (g * v->patch_gain) >> M64_Q15;

    /* Constant-power pan: the pair sums to unity power at every point,
     * including the centre, where linear panning dips by 3 dB.
     *
     * The product has to be taken in 64 bits. g carries patch_gain, which the
     * bank backend clamps at 4x unity precisely so a quiet SoundFont can be
     * brought up, and 4 * 32768 * 32768 is one past what an int32 holds. The
     * old code did this multiply in 32 bits and would have wrapped a
     * hard-panned voice on a loud region straight to a negative gain. */
    uint32_t pan = c->pan > 127 ? 127 : c->pan;
    v->gain_l = (int32_t)(((int64_t)g * pan_l_q15[pan]) >> M64_Q15);
    v->gain_r = (int32_t)(((int64_t)g * pan_r_q15[pan]) >> M64_Q15);
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
    m64_voice_t *v = alloc_voice(s);
    if (!v) return;

    memset(v, 0, sizeof(*v));
    v->channel  = (uint8_t)chn;
    v->note     = (uint8_t)note;
    v->velocity = (uint8_t)vel;
    v->age      = ++s->age_counter;
    v->one_shot = (chn == MIDI64_DRUM_CHANNEL);
    v->held     = !v->one_shot;

    /* Default pitch and trim. A backend may override either in start(): the
     * procedural one retunes percussion to its kit pitch, the sampled one
     * leaves the key alone and lets the region's root key do the work. */
    v->base_pitch_q8 = (int32_t)note << 8;
    v->patch_gain = M64_ONE_Q15;
    update_pitch(s, v, c);

    s->be->start(s, v, c);

    /* A backend with nothing to play for this note says so by parking the
     * voice; do not start an envelope on silence. */
    if (v->stage == M64_ENV_IDLE && v->atk_ms == 0 && v->sus_level == 0 &&
        v->dec_ms == 0 && v->rel_ms == 0)
        return;

    update_gains(s, v, c);
    v->env = 0;
    m64_env_enter(s, v, M64_ENV_ATTACK);
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

void m64_synth_update_pitch(m64_synth_t *s, const m64_channel_t *c, int chn) {
    for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
        m64_voice_t *v = &s->voices[i];
        if (v->stage == M64_ENV_IDLE || v->channel != chn) continue;
        update_pitch(s, v, c);
    }
}

void m64_synth_update_gains(m64_synth_t *s, const m64_channel_t *c, int chn) {
    for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
        m64_voice_t *v = &s->voices[i];
        if (v->stage == M64_ENV_IDLE || v->channel != chn) continue;
        update_gains(s, v, c);
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
    const m64_patch_t *p;

    if (v->channel == MIDI64_DRUM_CHANNEL) {
        /* A percussion key selects a drum, it does not name a pitch, so the
         * kit's own pitch replaces the key and the voice is retuned. */
        int32_t pitch;
        p = m64_patch_for_drum(v->note, &pitch);
        v->base_pitch_q8 = pitch;
        update_pitch(s, v, c);
    } else {
        p = m64_patch_for_program(c->program);
    }

    select_table(s, v, p->shape, v->base_pitch_q8 >> 8);
    v->bk.proc.noise_mix = p->noise_mix;
    /* Seeding from the age counter rather than a constant stops every snare hit
     * from being the identical waveform, which is audible as a machine-gun
     * quality on fast rolls. Any non-zero seed works; an LFSR that reaches zero
     * stays there. */
    v->bk.proc.noise_state = 0x9e3779b9u ^ (v->age * 2654435761u);
    if (v->bk.proc.noise_state == 0) v->bk.proc.noise_state = 1;

    if (p->cutoff_hz > 0) {
        /* One-pole corner: coeff = 2*pi*fc/sr, valid while fc is well under
         * Nyquist. 205887 is 2*pi in Q15. */
        int32_t coeff = (int32_t)(((int64_t)p->cutoff_hz * 205887) / s->samplerate);
        if (coeff > M64_ONE_Q15) coeff = M64_ONE_Q15;
        if (coeff < 1) coeff = 1;
        v->bk.proc.lp_coeff = coeff;
    } else {
        v->bk.proc.lp_coeff = 0;
    }

    v->atk_ms     = p->attack_ms;
    v->dec_ms     = p->decay_ms;
    v->rel_ms     = p->release_ms;
    v->sus_level  = p->sustain;
    v->patch_gain = p->gain;
}

static void be_release(m64_synth_t *s, m64_voice_t *v) {
    if (v->stage != M64_ENV_IDLE && v->stage != M64_ENV_RELEASE)
        m64_env_enter(s, v, M64_ENV_RELEASE);
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
 * @brief One voice's contribution to a control block.
 *
 * @p use_noise, @p use_lp and @p mono are compile-time constants at every call
 * site, which is the whole point of the always_inline: they were per-sample
 * branches on values that cannot change within a block, and a VR4300 has no
 * branch predictor to hide them behind. The dispatch below instantiates the
 * eight combinations and picks one per voice per block.
 *
 * @p mono is the case where gain_l == gain_r, which since the pan table
 * centres properly on CC10 64 is most voices in most files -- nothing has to
 * send CC10 for it to hold. It drops one 32-bit multiply out of the per-sample
 * work, and on a VR4300 a `mult` is five cycles plus the `mflo` interlock.
 */
static inline __attribute__((always_inline))
void render_voice(m64_voice_t *v, int32_t *out, int n,
                  int32_t gl, int32_t gr, int32_t dgl, int32_t dgr,
                  int use_noise, int use_lp, int mono) {
    const int16_t *tab  = v->bk.proc.table;
    const uint32_t mask = v->bk.proc.mask;
    const uint32_t sh   = v->bk.proc.shift;
    uint32_t ph  = v->phase;
    uint32_t inc = v->phase_inc;
    uint32_t ns  = v->bk.proc.noise_state;
    int32_t  nm  = v->bk.proc.noise_mix;
    int32_t  lp  = v->bk.proc.lp_state;
    int32_t  lpc = v->bk.proc.lp_coeff;

    for (int k = 0; k < n; k++) {
        uint32_t idx  = ph >> sh;
        int32_t  frac = (int32_t)((ph >> (sh - 16)) & 0xffff);
        int32_t  a = tab[idx];
        int32_t  b = tab[(idx + 1) & mask];
        int32_t  smp = a + (((b - a) * frac) >> 16);
        ph += inc;

        if (use_noise) {
            int32_t nz = (int32_t)(noise_next(&ns) >> 16) - 32768;
            smp += ((nz - smp) * nm) >> M64_Q15;
        }
        if (use_lp) {
            lp += ((smp - lp) * lpc) >> M64_Q15;
            smp = lp;
        }

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

    v->phase = ph;
    v->bk.proc.noise_state = ns;
    v->bk.proc.lp_state = lp;
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
        int32_t e1 = m64_env_tick(s, v);

        /* Gain is ramped per sample rather than held per block. A 2 ms attack
         * spans barely one block at 22 kHz, so a stepped envelope puts an
         * audible click on the front of every piano note. */
        int32_t gl0 = (e0 * v->gain_l) >> M64_Q15;
        int32_t gr0 = (e0 * v->gain_r) >> M64_Q15;
        int32_t gl1 = (e1 * v->gain_l) >> M64_Q15;
        int32_t gr1 = (e1 * v->gain_r) >> M64_Q15;
        int32_t dgl = m64_ramp_step(gl1 - gl0, n);
        int32_t dgr = m64_ramp_step(gr1 - gr0, n);

        int sel = (v->bk.proc.noise_mix ? 4 : 0)
                | (v->bk.proc.lp_coeff  ? 2 : 0)
                | (v->gain_l == v->gain_r ? 1 : 0);

        switch (sel) {
            case 0: render_voice(v, out, n, gl0, gr0, dgl, dgr, 0, 0, 0); break;
            case 1: render_voice(v, out, n, gl0, gr0, dgl, dgr, 0, 0, 1); break;
            case 2: render_voice(v, out, n, gl0, gr0, dgl, dgr, 0, 1, 0); break;
            case 3: render_voice(v, out, n, gl0, gr0, dgl, dgr, 0, 1, 1); break;
            case 4: render_voice(v, out, n, gl0, gr0, dgl, dgr, 1, 0, 0); break;
            case 5: render_voice(v, out, n, gl0, gr0, dgl, dgr, 1, 0, 1); break;
            case 6: render_voice(v, out, n, gl0, gr0, dgl, dgr, 1, 1, 0); break;
            default:render_voice(v, out, n, gl0, gr0, dgl, dgr, 1, 1, 1); break;
        }
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
            if (!cfg->bank_path) return MIDI64_ERR_FORMAT;
            s->be = &m64_backend_bank;
            break;
        case MIDI64_SYNTH_PROCEDURAL:
        default:
            s->be = &m64_backend_procedural;
            break;
    }

    build_pitch_tables(s);
    build_pan_table();          /* both backends pan through update_gains() */
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
