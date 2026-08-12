/**
 * @file midi64_internal.h
 * @brief Structures shared between the parser, sequencer and synth backends.
 *
 * Nothing here is part of the public API. All arithmetic in this engine is
 * fixed-point integer -- see the Q-format conventions below -- because that is
 * what makes a host render and an N64 render produce identical bytes. The
 * VR4300 has a usable FPU; the reason to avoid it is reproducibility, not speed.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MIDI64_INTERNAL_H__
#define MIDI64_INTERNAL_H__

#include "midi64.h"

/* ------------------------------------------------------ Q-format conventions
 *
 *   Q15   gains, envelope levels, pan.  32768 == 1.0.  Range fits int32 math.
 *   Q16   pitch ratios.                 65536 == 1.0.
 *   Q32   oscillator phase.             Wraps naturally in uint32_t.
 *   Q8    pitch in 1/256ths of a semitone. Note 60 exactly == 60*256.
 *
 * A "frame" is one stereo pair. A "sample" is one mono value.
 */

#define M64_Q15             15
#define M64_ONE_Q15         (1 << M64_Q15)
#define M64_Q16             16
#define M64_ONE_Q16         (1 << M64_Q16)

/** @brief Oscillator wavetable size bounds, in samples.
 *
 * Tables are **not** all the same length. Each mipmap level is sized to the
 * harmonics it actually carries -- see m64_wt_len_for() -- so a level holding
 * one harmonic gets M64_WT_MIN_LEN entries and only the densest gets
 * M64_WT_MAX_LEN. Every length is a power of two; the phase accumulator indexes
 * a table by shifting, and the shift that goes with a length travels with the
 * table in an @ref m64_wtdesc_t.
 *
 * A uniform 1024 for every level was the first design and it was expensive in
 * the one place an N64 cannot afford to be. The tables are read by the
 * per-sample inner loop at a stride proportional to their length, so an
 * oversampled level costs cache misses in exact proportion to how oversampled
 * it is -- and level 6 carries a single harmonic, so at 1024 entries it was
 * oversampled by a factor of 1024. The whole set was 71,680 bytes against an
 * 8 KB data cache. Sizing per level brings it to about 16 KB at 22050 Hz and
 * makes the levels a song actually lives in small enough to stay resident. */
#define M64_WT_MAX_BITS     10
#define M64_WT_MAX_LEN      (1 << M64_WT_MAX_BITS)
#define M64_WT_MIN_BITS     6
#define M64_WT_MIN_LEN      (1 << M64_WT_MIN_BITS)

/** @brief Table samples per period of the highest harmonic a level carries.
 *
 * The quality knob the sizing rule is derived from. Linear interpolation error
 * for harmonic k goes as (k/len)^2, so this ratio -- not the absolute length --
 * governs how clean a table sounds, which is why holding the length fixed
 * while the harmonic count fell by 178x was paying for quality already reached.
 *
 * 16 was measured, not reasoned. The first attempt used 4, on the argument
 * that it was the tightest ratio the uniform design ever ran at (level 0 at
 * 44100 Hz), and tools/pitchtest.py said no: the alias floor fell 13.6 dB. The
 * argument was wrong because level 0 is the one level whose harmonic count is
 * clamped by MAX_HARMONICS rather than by Nyquist, so its ratio was never a
 * quality target anyone had chosen. Sweeping the knob against the measurement
 * instead -- alias floor of the left channel, worst of the sixteen pitchtest
 * cases, against a uniform-1024 build:
 *
 *     ratio        4        8       16       32
 *     worst   -13.6dB   -5.5dB   -1.5dB   -0.5dB
 *     bytes    16,512   24,192   31,872   39,040    (22050 Hz; was 71,680)
 *
 * 16 is the knee. Its 1.5 dB is one case in sixteen -- a square lead at note
 * 93 -- and every other case is within 0.2 dB. Going to 32 buys that back for
 * 7 KB and hands level 3 (notes 60..71, where most melodies live) its 2 KB
 * table again, which is the opposite of the point.
 *
 * Measure with tools/aliastest.py, not tools/pitchtest.py: pitchtest collapses
 * to mono, and a mono downmix of two channels whose quantisation errors are
 * independent reads ~2 dB cleaner than either channel. Centring the pan law
 * made L and R bit-identical for un-panned notes, which removed that averaging
 * and looks exactly like a 2 dB regression that is not there. */
#define M64_WT_MIN_RATIO    16

/** @brief Lowest MIDI note the mipmap is built for.
 *
 * Notes below this read level 0, which is band-limited for note 35 and so is
 * *more* band-limited than they strictly need -- duller, never aliased, which
 * is the safe direction to err.
 *
 * 24 is C1, 32.7 Hz. Below it a fundamental stops reading as pitch, and the
 * reference corpus never goes there (measured: its melodic range is 24..102).
 * Building bands for notes 0..23 meant two extra levels pinned at the 256
 * harmonic cap, which was 59% of the entire table build cost. */
#define M64_WT_BASE_NOTE    24

/** @brief Band-limited mipmap levels. Level n covers one octave starting at
 *  M64_WT_BASE_NOTE; the last level absorbs everything up to note 127 and
 *  degenerates to near-sine, which is what a properly band-limited saw *should*
 *  look like up there. */
#define M64_WT_LEVELS       7

/** @brief Frames rendered between envelope/LFO/pan updates.
 *
 * Envelopes are piecewise-linear and evaluated per control block, not per
 * sample. 32 frames at 22050 Hz is 1.45 ms, far below the ~10 ms where stepping
 * becomes audible as zipper noise, and it keeps the per-sample inner loop down
 * to a multiply-accumulate.
 *
 * Vendored change, carried across the update from upstream 1c79dc8: made
 * overridable so the value can be swept from the build without editing this
 * file. See MainMenu docs/AUDIT.md on the cost of music, which sweeps it to 64.
 *
 * The shift is overridable with it, and it has to be: upstream pairs the two
 * and the #error below is what stops a half-override -- setting the block from
 * the command line and leaving the shift at 5 would otherwise divide every
 * envelope ramp by 32 while rendering 64 frames, silently, in a build that
 * compiled. That check is upstream's and it is exactly what this needs. */
#ifndef M64_CTRL_BLOCK
#define M64_CTRL_BLOCK      32
#endif
#ifndef M64_CTRL_SHIFT
#define M64_CTRL_SHIFT      5
#endif
#if (1 << M64_CTRL_SHIFT) != M64_CTRL_BLOCK
#error "M64_CTRL_SHIFT must be log2(M64_CTRL_BLOCK)"
#endif

/**
 * @brief Divide a per-block gain delta by the block length.
 *
 * Every rendered block but the last of a render call is exactly
 * M64_CTRL_BLOCK frames, and a VR4300 `div` is 37 cycles against about one for
 * the shift that replaces it. Two of these per voice per block was measurable.
 *
 * The `+ (d >> 31 & 31)` is what keeps this bit-identical to `d / n` rather
 * than merely close: a right shift floors, C division truncates toward zero,
 * and the difference is not academic here. A delta of -1 floors to a step of
 * -1 instead of 0, which over 32 samples walks a quiet voice's gain 32 units
 * past where it should stop -- through zero and out the other side, inverting
 * the tail of the block.
 */
static inline int32_t m64_ramp_step(int32_t d, int n) {
    if (n == M64_CTRL_BLOCK)
        return (d + ((d >> 31) & (M64_CTRL_BLOCK - 1))) >> M64_CTRL_SHIFT;
    return d / n;
}

/** @brief Oscillator shapes the procedural backend can select.
 *
 * Declared here rather than beside the patch table because m64_synth_s sizes
 * its wavetable descriptor array with M64_WT_SHAPES. */
typedef enum {
    M64_SHAPE_SINE = 0,
    M64_SHAPE_TRIANGLE,
    M64_SHAPE_SAW,
    M64_SHAPE_SQUARE,
    M64_SHAPE_PULSE25,
    M64_SHAPE_COUNT,
} m64_shape_t;
#define M64_WT_SHAPES M64_SHAPE_COUNT

/** @brief Where one (shape, level) wavetable lives and how to index it.
 *
 * Carried on the voice rather than looked up per sample: the inner loop needs
 * @ref shift and @ref mask on every iteration, and both are constant for the
 * life of a note. */
typedef struct {
    uint16_t off;    /**< Entry offset into m64_synth_s::wt */
    uint16_t mask;   /**< len - 1, for the wrap in the interpolator */
    uint8_t  shift;  /**< 32 - log2(len); turns Q32 phase into an index */
} m64_wtdesc_t;

/* ------------------------------------------------------------- track cursor */

/** @brief One MTrk chunk mid-playback.
 *
 * Playback is a k-way merge over these: whichever track has the smallest
 * @ref next_tick is the next to fire. That is why no merged event list is ever
 * built, and why memory cost is exactly the file size. */
typedef struct m64_track_s {
    const uint8_t *base;    /**< First byte of event data (after the MTrk header) */
    const uint8_t *end;     /**< One past the last byte of this chunk */
    const uint8_t *cur;     /**< Read cursor; at a delta-time when between events */
    uint32_t       next_tick; /**< Absolute tick of the event at @ref cur */
    uint8_t        running_status; /**< Last channel status byte seen, for running status */
    uint8_t        meta_type; /**< Type of the meta event last returned by m64_track_next() */
    bool           done;    /**< Cursor reached @ref end */
} m64_track_t;

/* ------------------------------------------------------------ channel state */

/** @brief Per-MIDI-channel controller state. */
typedef struct {
    uint8_t  program;       /**< GM program number 0..127 */
    uint8_t  volume;        /**< CC7,  0..127. Default 100 per GM. */
    uint8_t  expression;    /**< CC11, 0..127. Default 127. */
    uint8_t  pan;           /**< CC10, 0..127, 64 centre */
    bool     sustain;       /**< CC64 >= 64 */
    int16_t  bend;          /**< Pitch bend, -8192..8191 */
    uint8_t  bend_range;    /**< Semitones, from RPN 0/0. Default 2. */
    uint16_t rpn;           /**< Currently selected RPN (MSB<<7|LSB), 0x3FFF == none */
} m64_channel_t;

/* -------------------------------------------------------------------- voice */

/** @brief Envelope stage. Release is entered on note-off, or immediately for
 *  one-shot percussion which has no sustain to hold. */
typedef enum {
    M64_ENV_IDLE = 0,
    M64_ENV_ATTACK,
    M64_ENV_DECAY,
    M64_ENV_SUSTAIN,
    M64_ENV_RELEASE,
} m64_env_stage_t;

/** @brief A sounding note.
 *
 * Fields above @ref bk are backend-independent; the sampled backend will fill
 * @ref bk differently from the procedural one but the sequencer never looks
 * inside it. */
typedef struct m64_voice_s {
    /* identity -- what the sequencer matches note-offs against */
    uint8_t  channel;
    uint8_t  note;
    uint8_t  velocity;
    bool     held;          /**< Note-on seen, note-off not yet */
    bool     sustained;     /**< Note-off seen but held by the sustain pedal */
    bool     one_shot;      /**< Percussion: rings out on its own, note-off ignored */
    uint32_t age;           /**< Sequence number at note-on; oldest is stolen first */

    /* pitch, recomputed when the note bends or the channel's bend range changes */
    int32_t  base_pitch_q8; /**< Unbent pitch. For drums this is the kit pitch, not the key. */
    int32_t  pitch_q8;      /**< Note number in 1/256ths of a semitone, incl. bend */
    uint32_t phase;         /**< Q32 oscillator phase */
    uint32_t phase_inc;     /**< Q32 phase increment per frame */

    /* amplitude */
    m64_env_stage_t stage;
    int32_t  env;           /**< Current envelope level, Q15 */
    int32_t  env_step;      /**< Per-control-block delta applied to @ref env, Q15 */
    int32_t  env_target;    /**< Level the current stage is heading for, Q15 */
    int32_t  gain_l, gain_r;/**< Post-pan, post-velocity gains, Q15 */

    /* Envelope shape, copied in at note-on by whichever backend owns the voice.
     *
     * These used to be read back out of the procedural patch table on every
     * control block, which was both a per-voice table lookup in the hot path
     * and a hard dependency on that table -- a sampled voice's envelope comes
     * from its SF2 region and has no patch to look up. Storing the four
     * numbers on the voice makes the envelope code backend-agnostic. */
    uint16_t atk_ms, dec_ms, rel_ms;
    int32_t  sus_level;     /**< Q15 level held while the key is down */
    int32_t  patch_gain;    /**< Q15 per-voice trim. The procedural backend uses
                             *   it to balance shapes of unequal power; the bank
                             *   backend uses it for the region's attenuation. */

    /* backend-private. The sequencer never looks inside; only the backend that
     * started the voice may touch its arm of this union. */
    union {
        struct {
            const int16_t *table;/**< Selected mipmap level of the selected shape */
            uint32_t mask;       /**< table length - 1; lengths vary per level */
            uint32_t shift;      /**< Q32 phase -> table index shift */
            uint32_t noise_state;/**< LFSR, non-zero when the voice uses noise */
            int32_t  noise_mix;  /**< Q15 blend of noise into the tonal output */
            int32_t  lp_state;   /**< One-pole low-pass memory */
            int32_t  lp_coeff;   /**< One-pole low-pass coefficient, Q15 */
        } proc;
        struct {
            const void *pcm;     /**< First byte of this sample within the pool */
            uint32_t length;     /**< Frames in the sample */
            uint32_t loop_start; /**< Loop start, in frames */
            uint32_t loop_end;   /**< Loop end, in frames; 0 disables looping */
            uint64_t pos;        /**< Playback position, Q32 fixed point */
            uint64_t step;       /**< Position increment per output frame, Q32 */
        } bank;
    } bk;
} m64_voice_t;

/* ---------------------------------------------------------------- sequencer */

/** @brief Sequencer state: where we are in the song and what the channels hold. */
typedef struct m64_seq_s {
    const midi64_song_t *song;
    m64_track_t   *tracks;       /**< ntracks cursors, owned */
    uint32_t       tick;         /**< Current absolute tick */
    uint32_t       usec_per_qn;  /**< Tempo, from meta 0x51. 500000 == 120 BPM. */

    /* Tick clock. frac accumulates Q32 fractions of a tick per frame so tempo
     * changes do not drift: the residue survives across tempo changes. */
    uint32_t       tick_inc_q32; /**< Q32 ticks advanced per output frame */
    uint32_t       tick_frac;    /**< Q32 residue */

    m64_channel_t  chan[MIDI64_CHANNELS];
    bool           ended;        /**< All tracks exhausted */
    uint64_t       frames;       /**< Frames rendered since the last rewind */
    int            samplerate;
} m64_seq_t;

/* ------------------------------------------------------------------- synth */

/** @brief Synth backend vtable.
 *
 * The split is: the sequencer owns channel state and decides *which* voice
 * plays *what*; the backend owns how that voice makes sound. Adding the
 * sampled backend means implementing this and nothing else.
 */
typedef struct m64_backend_s {
    const char *name;
    int  (*init)   (m64_synth_t *s, const midi64_config_t *cfg);
    void (*close)  (m64_synth_t *s);
    /** Configure @p v for a new note. Pitch and gains are already set. */
    void (*start)  (m64_synth_t *s, m64_voice_t *v, const m64_channel_t *ch);
    /** Note-off: move to the release stage. */
    void (*release)(m64_synth_t *s, m64_voice_t *v);
    /** Mix all sounding voices into @p out (Q15 stereo, accumulated). */
    void (*render) (m64_synth_t *s, int32_t *out, int nframes);
} m64_backend_t;

/** @brief Synth state. Owns the voice pool and whatever tables the backend needs. */
struct m64_synth_s {
    const m64_backend_t *be;
    int          samplerate;
    m64_voice_t  voices[MIDI64_MAX_VOICES];
    uint32_t     age_counter;

    /* Pitch tables, built once at init.
     * inc_note[n]   = Q32 phase increment for MIDI note n at this sample rate
     * inc_frac[f]   = Q16 ratio 2^(f/(12*256)) for the sub-semitone remainder */
    uint32_t     inc_note[128];
    uint32_t     inc_frac[256];

    /* Procedural backend tables. Allocated only by that backend.
     *
     * One blob of variable-length tables, addressed through @ref wt_desc.
     * Entries that would hold identical samples share one range: every band
     * that carries a single harmonic is the same sine whatever shape asked for
     * it, and a sine is a single harmonic at every level. */
    int16_t     *wt;
    m64_wtdesc_t wt_desc[M64_WT_SHAPES][M64_WT_LEVELS];

    /* Sampled backend state. Allocated only by that backend. */
    struct m64_bank_s *bank;
};

/* ---------------------------------------------------------------- internals */

/* smf.c */
int  m64_smf_parse(midi64_song_t *song);
/** Read a variable-length quantity. Advances @p p. Returns false past @p end. */
bool m64_read_vlq(const uint8_t **p, const uint8_t *end, uint32_t *out);
/** Position a track cursor at its first event, reading that event's delta. */
void m64_track_reset(m64_track_t *t);
/**
 * @brief Decode the event at the cursor, resolving running status.
 *
 * On success @p status is the effective status byte, @p data points at the
 * parameter bytes and @p len counts them. For meta events @p status is 0xff,
 * @p data / @p len describe the payload alone, and the type lands in
 * @ref m64_track_s::meta_type -- the type byte and its payload are separated by
 * the length VLQ in the file, so no single pointer can cover both.
 * Returns false when the track is exhausted or the event is malformed; either
 * way the track is marked done.
 *
 * The cursor does not move to the next event until m64_track_advance().
 */
bool m64_track_next(m64_track_t *t, uint8_t *status, const uint8_t **data, uint32_t *len);
/** Consume the event returned by m64_track_next() and queue the next delta. */
void m64_track_advance(m64_track_t *t);

/* seq.c */
int  m64_seq_init(m64_seq_t *seq, const midi64_song_t *song, int samplerate);
void m64_seq_close(m64_seq_t *seq);
void m64_seq_rewind(m64_seq_t *seq);
/** Advance the tick clock by @p nframes, dispatching every event that falls due. */
void m64_seq_advance(m64_seq_t *seq, m64_synth_t *synth, int nframes);

/* synth.c */
int  m64_synth_init(m64_synth_t *s, int samplerate, const midi64_config_t *cfg);
void m64_synth_close(m64_synth_t *s);
void m64_synth_note_on (m64_synth_t *s, const m64_channel_t *ch, int chn, int note, int vel);
void m64_synth_note_off(m64_synth_t *s, const m64_channel_t *ch, int chn, int note);
void m64_synth_all_off (m64_synth_t *s, int chn, bool immediate);
/** Recompute pitch and gains for every voice on @p chn after a controller change.
 *
 * Split into halves because most controllers only move one of them and the
 * other half is not free: gains cost a pan lookup and four multiplies per
 * voice, pitch costs a table lookup and a 64-bit multiply. Pitch bend is the
 * case that matters -- it arrives in dense streams and cannot change a gain --
 * and it used to recompute both for every voice on the channel. */
void m64_synth_update_pitch  (m64_synth_t *s, const m64_channel_t *ch, int chn);
void m64_synth_update_gains  (m64_synth_t *s, const m64_channel_t *ch, int chn);
void m64_synth_update_channel(m64_synth_t *s, const m64_channel_t *ch, int chn);
void m64_synth_render(m64_synth_t *s, int32_t *out, int nframes);
int  m64_synth_active(const m64_synth_t *s);
/** Q32 phase increment for a pitch given in 1/256ths of a semitone. */
uint32_t m64_pitch_to_inc(const m64_synth_t *s, int32_t pitch_q8);

/**
 * @brief Enter an envelope stage, taking its shape from the voice.
 *
 * Shared with the sampled backend, whose envelopes come from SF2 regions rather
 * than from the procedural patch table.
 */
void    m64_env_enter(m64_synth_t *s, m64_voice_t *v, m64_env_stage_t st);
/** Advance one voice's envelope by a control block; returns the end level. */
int32_t m64_env_tick(m64_synth_t *s, m64_voice_t *v);

/* Backends */
extern const m64_backend_t m64_backend_procedural;
extern const m64_backend_t m64_backend_bank;

/** Name stored in the loaded bank, or "" when the procedural backend is active. */
const char *m64_bank_name(const m64_synth_t *s);

/* patches.c -- GM program and drum key mappings, see that file's header. */

/** @brief A synth recipe. Times are milliseconds, levels Q15. */
typedef struct {
    uint8_t  shape;         /**< m64_shape_t */
    uint16_t attack_ms;
    uint16_t decay_ms;
    int16_t  sustain;       /**< Q15 level held while the key is down */
    uint16_t release_ms;
    int16_t  noise_mix;     /**< Q15 amount of noise blended in; drums use 32767 */
    uint16_t cutoff_hz;     /**< One-pole low-pass corner; 0 disables the filter */
    int16_t  gain;          /**< Q15 per-patch trim, so a saw does not swamp a sine */
} m64_patch_t;

/** Look up the patch for a melodic GM program 0..127. */
const m64_patch_t *m64_patch_for_program(int program);
/** Look up the patch for a percussion key 0..127, and its fixed playback pitch. */
const m64_patch_t *m64_patch_for_drum(int key, int32_t *pitch_q8_out);

#endif /* MIDI64_INTERNAL_H__ */
