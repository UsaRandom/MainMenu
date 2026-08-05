/**
 * @file seq.c
 * @brief The sequencer: tick clock, tempo map, controller state, event dispatch.
 *
 * Playback is a k-way merge over the track cursors in smf.c. There is no merged
 * event list, so seeking backwards means resetting every cursor and replaying,
 * which is why midi64_player_rewind() is the only seek offered.
 *
 * Timing runs off a Q32 fixed-point tick accumulator rather than a
 * frames-per-tick integer. That matters at 22050 Hz: at 120 BPM and 96 PPQN a
 * tick is 114.8 frames, and rounding that to 115 every tick drifts by a full
 * beat about every 90 seconds. Keeping the residue in @ref tick_frac makes the
 * error bounded rather than cumulative, and it survives tempo changes.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "midi64_internal.h"

/**
 * @brief Compute num/den as a Q32 fixed-point value without overflowing 64 bits.
 *
 * The obvious `(num << 32) / den` overflows for any realistic PPQN: at 32767
 * PPQN the numerator alone is 1.4e20 against a 1.8e19 ceiling. Long division a
 * byte at a time keeps every intermediate under 2^56.
 */
static uint64_t div_q32(uint64_t num, uint64_t den) {
    if (den == 0) return 0;
    uint64_t ip  = num / den;
    uint64_t rem = num % den;
    uint64_t frac = 0;
    for (int i = 0; i < 4; i++) {
        rem <<= 8;
        frac = (frac << 8) | (rem / den);
        rem %= den;
    }
    /* Saturate rather than wrap: a nonsense tempo should run the song fast, not
     * make the tick clock jump backwards. */
    if (ip > 0xffffffffULL) return 0xffffffffffffffffULL;
    return (ip << 32) | frac;
}

/** @brief Recompute the tick clock after a tempo change or a rate change. */
static void seq_retime(m64_seq_t *seq) {
    /* ticks per frame = (PPQN ticks/quarter * 1e6 usec/sec)
     *                 / (usec_per_qn usec/quarter * samplerate frames/sec) */
    uint64_t num = (uint64_t)seq->song->ppqn * 1000000ULL;
    uint64_t den = (uint64_t)seq->usec_per_qn * (uint64_t)seq->samplerate;
    uint64_t inc = div_q32(num, den);
    /* One tick per frame is already absurdly fast; clamping keeps the event
     * dispatch loop bounded no matter how corrupt the tempo bytes are. */
    if (inc > 0xffffffffULL) inc = 0xffffffffULL;
    seq->tick_inc_q32 = (uint32_t)inc;
}

/** @brief Reset one channel to the General MIDI power-on defaults. */
static void chan_reset(m64_channel_t *c) {
    c->program    = 0;
    c->volume     = 100;   /* GM default, not 127: files that never send CC7
                            * expect to sit below full scale. */
    c->expression = 127;
    c->pan        = 64;
    c->sustain    = false;
    c->bend       = 0;
    c->bend_range = 2;
    c->rpn        = 0x3fff;  /* "none selected" */
}

int m64_seq_init(m64_seq_t *seq, const midi64_song_t *song, int samplerate) {
    memset(seq, 0, sizeof(*seq));
    seq->song = song;
    seq->samplerate = samplerate;
    seq->tracks = calloc(song->ntracks, sizeof(m64_track_t));
    if (!seq->tracks) return MIDI64_ERR_NOMEM;
    m64_seq_rewind(seq);
    return MIDI64_OK;
}

void m64_seq_close(m64_seq_t *seq) {
    if (!seq) return;
    free(seq->tracks);
    seq->tracks = NULL;
}

void m64_seq_rewind(m64_seq_t *seq) {
    for (int i = 0; i < seq->song->ntracks; i++) {
        seq->tracks[i] = seq->song->tracks[i];
        m64_track_reset(&seq->tracks[i]);
    }
    for (int i = 0; i < MIDI64_CHANNELS; i++) chan_reset(&seq->chan[i]);
    seq->tick = 0;
    seq->tick_frac = 0;
    seq->usec_per_qn = 500000;   /* 120 BPM until a tempo meta says otherwise */
    seq->ended = false;
    seq->frames = 0;
    seq_retime(seq);
}

/* ------------------------------------------------------------ controllers */

static void handle_cc(m64_seq_t *seq, m64_synth_t *synth, int ch, int cc, int val) {
    m64_channel_t *c = &seq->chan[ch];

    switch (cc) {
        case 7:   c->volume     = (uint8_t)val; break;
        case 10:  c->pan        = (uint8_t)val; break;
        case 11:  c->expression = (uint8_t)val; break;

        case 64:  /* sustain pedal */
            c->sustain = (val >= 64);
            if (!c->sustain) {
                /* Releasing the pedal releases every voice that was only being
                 * held by it. Voices whose key is still down keep sounding. */
                for (int i = 0; i < MIDI64_MAX_VOICES; i++) {
                    m64_voice_t *v = &synth->voices[i];
                    if (v->stage != M64_ENV_IDLE && v->channel == ch && v->sustained) {
                        v->sustained = false;
                        if (!v->held) synth->be->release(synth, v);
                    }
                }
            }
            break;

        /* RPN selection and data entry. The corpus only ever uses RPN 0/0
         * (pitch bend range), but tracking the selector properly is three lines
         * and stops a stray CC6 from being applied to whatever was last set. */
        case 100: c->rpn = (uint16_t)((c->rpn & 0x3f80) | (val & 0x7f)); break;
        case 101: c->rpn = (uint16_t)((c->rpn & 0x007f) | ((val & 0x7f) << 7)); break;
        case 6:
            if (c->rpn == 0) {   /* RPN 0/0: pitch bend sensitivity, in semitones */
                c->bend_range = (uint8_t)(val ? val : 1);
                m64_synth_update_channel(synth, c, ch);
            }
            break;
        case 38: break;  /* data entry LSB: cents. Ignored -- see docs/LIMITS.md */

        case 120: m64_synth_all_off(synth, ch, true);  break;  /* all sound off */
        case 123: m64_synth_all_off(synth, ch, false); break;  /* all notes off */

        case 121:  /* reset all controllers -- note that this does NOT stop notes */
            c->expression = 127;
            c->bend = 0;
            c->sustain = false;
            c->rpn = 0x3fff;
            m64_synth_update_channel(synth, c, ch);
            break;

        default: return;   /* nothing else changes voice state */
    }

    if (cc == 7 || cc == 10 || cc == 11) m64_synth_update_channel(synth, c, ch);
}

/** @brief Act on one decoded event. @p meta_type is only meaningful when
 *  @p status is 0xff; see m64_track_next() for why it travels separately. */
static void dispatch(m64_seq_t *seq, m64_synth_t *synth, uint8_t status,
                     uint8_t meta_type, const uint8_t *d, uint32_t len) {
    int ch = status & 0x0f;
    m64_channel_t *c = &seq->chan[ch];

    switch (status & 0xf0) {
        case 0x80:   /* note off */
            if (len >= 2) m64_synth_note_off(synth, c, ch, d[0] & 0x7f);
            break;

        case 0x90:   /* note on; velocity 0 is the idiomatic note off */
            if (len >= 2) {
                if (d[1] == 0) m64_synth_note_off(synth, c, ch, d[0] & 0x7f);
                else           m64_synth_note_on(synth, c, ch, d[0] & 0x7f, d[1] & 0x7f);
            }
            break;

        case 0xb0:
            if (len >= 2) handle_cc(seq, synth, ch, d[0] & 0x7f, d[1] & 0x7f);
            break;

        case 0xc0:
            if (len >= 1) c->program = d[0] & 0x7f;
            break;

        case 0xe0:   /* pitch bend, 14-bit, centred at 8192 */
            if (len >= 2) {
                c->bend = (int16_t)((((int)(d[1] & 0x7f) << 7) | (d[0] & 0x7f)) - 8192);
                m64_synth_update_channel(synth, c, ch);
            }
            break;

        case 0xa0:   /* polyphonic aftertouch */
        case 0xd0:   /* channel aftertouch */
            /* Deliberately ignored. No file in the corpus sends either, and
             * mapping them to anything would be inventing an interpretation. */
            break;

        case 0xf0:
            if (status == 0xff && meta_type == 0x51 && len >= 3) {
                uint32_t t = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
                /* A zero tempo would divide by zero in seq_retime(). */
                if (t) { seq->usec_per_qn = t; seq_retime(seq); }
            }
            /* 0x2f (end of track) needs no action: the cursor hits `end` on the
             * next advance and the track marks itself done. */
            break;

        default: break;
    }
}

/* -------------------------------------------------------------- advancing */

void m64_seq_advance(m64_seq_t *seq, m64_synth_t *synth, int nframes) {
    if (seq->ended) return;

    /* Fold this block's fractional ticks in, then take the whole ticks out. */
    uint64_t acc = (uint64_t)seq->tick_frac + (uint64_t)seq->tick_inc_q32 * (uint64_t)nframes;
    uint32_t whole = (uint32_t)(acc >> 32);
    seq->tick_frac = (uint32_t)(acc & 0xffffffffULL);
    uint32_t target = seq->tick + whole;

    for (;;) {
        /* Pick the track whose next event is soonest. Ties go to the
         * lowest-numbered track, which is the order the file lists them in and
         * therefore the order a different player would also pick. */
        int best = -1;
        uint32_t best_tick = 0;
        int live = 0;
        for (int i = 0; i < seq->song->ntracks; i++) {
            if (seq->tracks[i].done) continue;
            live++;
            if (best < 0 || seq->tracks[i].next_tick < best_tick) {
                best = i;
                best_tick = seq->tracks[i].next_tick;
            }
        }
        if (best < 0) { seq->ended = true; break; }
        if (best_tick > target) break;
        (void)live;

        uint8_t st; const uint8_t *d; uint32_t len;
        if (m64_track_next(&seq->tracks[best], &st, &d, &len)) {
            dispatch(seq, synth, st, seq->tracks[best].meta_type, d, len);
            m64_track_advance(&seq->tracks[best]);
        }
        /* When m64_track_next() fails it has already marked the track done, so
         * the next iteration drops it from the merge. */
    }

    seq->tick = target;
    seq->frames += (uint64_t)nframes;
}
