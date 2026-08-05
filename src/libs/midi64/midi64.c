/**
 * @file midi64.c
 * @brief Player lifecycle and the render loop that drives sequencer and synth.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "midi64_internal.h"

static const midi64_config_t default_config = {
    .synth      = MIDI64_SYNTH_PROCEDURAL,
    .bank_path  = NULL,
    .loop       = true,
    /* 0.5, not unity, and not a guess. Rendered at unity across the 28-song
     * reference corpus the loudest peak is 58736 (Goofy Monster), so unity
     * clips by 5.1 dB; 0.75 still clipped 12 of the 28. 16384 puts that worst
     * case at 29368, which is 90% of full scale, and leaves the quietest song
     * near 50%. Reproduce with:
     *     tools/render song.mid /dev/null -v 2048 --stats   (peak x16) */
    .master_volume = 16384,
};

int midi64_player_init(midi64_player_t *p, const midi64_song_t *song,
                       int samplerate, const midi64_config_t *cfg) {
    if (!p || !song || !song->tracks || samplerate <= 0) return MIDI64_ERR_FORMAT;
    if (!cfg) cfg = &default_config;

    memset(p, 0, sizeof(*p));
    p->song = song;
    p->samplerate = samplerate;
    p->loop = cfg->loop;
    p->master_volume = cfg->master_volume > 0 ? cfg->master_volume
                                              : default_config.master_volume;

    p->synth = malloc(sizeof(m64_synth_t));
    if (!p->synth) return MIDI64_ERR_NOMEM;

    int err = m64_synth_init(p->synth, samplerate, cfg);
    if (err != MIDI64_OK) { free(p->synth); p->synth = NULL; return err; }

    p->seq = malloc(sizeof(m64_seq_t));
    if (!p->seq) { m64_synth_close(p->synth); free(p->synth); p->synth = NULL;
                   return MIDI64_ERR_NOMEM; }

    err = m64_seq_init(p->seq, song, samplerate);
    if (err != MIDI64_OK) {
        free(p->seq); p->seq = NULL;
        m64_synth_close(p->synth); free(p->synth); p->synth = NULL;
        return err;
    }

    p->playing = true;
    return MIDI64_OK;
}

void midi64_player_close(midi64_player_t *p) {
    if (!p) return;
    if (p->seq)   { m64_seq_close(p->seq);   free(p->seq);   p->seq = NULL; }
    if (p->synth) { m64_synth_close(p->synth); free(p->synth); p->synth = NULL; }
    p->playing = false;
}

void midi64_player_rewind(midi64_player_t *p) {
    if (!p || !p->seq) return;
    m64_synth_all_off(p->synth, -1, true);
    m64_seq_rewind(p->seq);
}

void midi64_player_set_paused(midi64_player_t *p, bool paused) {
    if (!p) return;
    p->playing = !paused;
}

bool midi64_player_done(const midi64_player_t *p) {
    if (!p || !p->seq) return true;
    /* Not done until the tails have rung out, or a fade-out chord gets cut off
     * the instant the last note-off is dispatched. */
    return p->seq->ended && !p->loop && m64_synth_active(p->synth) == 0;
}

void midi64_player_set_volume(midi64_player_t *p, int vol) {
    if (!p) return;
    if (vol < 0) vol = 0;
    p->master_volume = vol;
}

uint32_t midi64_player_position_ms(const midi64_player_t *p) {
    if (!p || !p->seq) return 0;
    return (uint32_t)(p->seq->frames * 1000ULL / (uint64_t)p->samplerate);
}

int midi64_player_active_voices(const midi64_player_t *p) {
    if (!p || !p->synth) return 0;
    return m64_synth_active(p->synth);
}

/* ------------------------------------------------------------- rendering -- */

static inline int16_t clip16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void midi64_player_render(midi64_player_t *p, int16_t *out, int nframes) {
    if (!p || !p->seq || !out) {
        if (out && nframes > 0) memset(out, 0, (size_t)nframes * 2 * sizeof(int16_t));
        return;
    }

    /* One control block of stereo accumulators. Voices sum here at full int32
     * width and only get clipped once, on the way out; clipping per voice would
     * distort a loud voice even when the mix as a whole had room. */
    int32_t acc[2 * M64_CTRL_BLOCK];

    while (nframes > 0) {
        int n = nframes < M64_CTRL_BLOCK ? nframes : M64_CTRL_BLOCK;

        if (!p->playing) {
            memset(out, 0, (size_t)n * 2 * sizeof(int16_t));
            out += 2 * n;
            nframes -= n;
            continue;
        }

        /* Note that a tempo change inside this block takes effect at the next
         * block, not at the event. The error is bounded by one control block
         * (1.45 ms at 22050 Hz) and does not accumulate, because the tick
         * residue in the sequencer survives the rate change. */
        m64_seq_advance(p->seq, p->synth, n);

        memset(acc, 0, (size_t)n * 2 * sizeof(int32_t));
        m64_synth_render(p->synth, acc, n);

        for (int i = 0; i < n * 2; i++)
            out[i] = clip16((acc[i] * p->master_volume) >> M64_Q15);

        out += 2 * n;
        nframes -= n;

        if (p->seq->ended) {
            if (p->loop) {
                /* Release rather than cut: the tails of the last bar overlap
                 * the first bar of the repeat, which is what makes a loop sound
                 * like a loop instead of a splice. */
                m64_synth_all_off(p->synth, -1, false);
                m64_seq_rewind(p->seq);
            } else if (m64_synth_active(p->synth) == 0) {
                /* Song over and silent: fill the rest and stop working. */
                if (nframes > 0) {
                    memset(out, 0, (size_t)nframes * 2 * sizeof(int16_t));
                    nframes = 0;
                }
            }
        }
    }
}
