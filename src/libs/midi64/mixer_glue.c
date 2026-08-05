/**
 * @file mixer_glue.c
 * @brief The only file in midi64 that knows libdragon exists.
 *
 * midi64 presents itself to the mixer as a single stereo waveform_t of unknown
 * length. Every voice is summed inside midi64_player_render(), so a 32-note
 * chord costs the same one mixer channel (well, two -- a stereo waveform
 * occupies the given channel and the one after it) as a single note does.
 *
 * The player is a *stateful generator*, not a seekable buffer: it can produce
 * the next N frames and nothing else. That has two consequences the mixer has
 * to be kept away from:
 *
 *   - `len` is WAVEFORM_UNKNOWN_LEN and `loop_len` is 0, so the mixer never
 *     tries to seek backwards to execute a loop. Looping is the sequencer's
 *     job (midi64_config_t::loop), where it can be done musically.
 *   - mixer_ch_set_pos() on a midi64 channel does not work and cannot be made
 *     to work. Use midi64_player_rewind().
 *
 * For the same reason one player must not be played on two mixer channels at
 * once: both would pull from the same generator and each would get half the
 * frames. wav64 can do this because it re-seeks the source file per channel;
 * there is no equivalent here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef N64

#include <libdragon.h>
#include <string.h>

#include "midi64_internal.h"

/** @brief Concurrent midi64 waveforms. Two is already generous -- one for music
 *  and one for a crossfade to the next track. */
#ifndef MIDI64_MIXER_MAX_ATTACH
#define MIDI64_MIXER_MAX_ATTACH 2
#endif

typedef struct {
    waveform_t       wave;
    midi64_player_t *player;
    int              ch;
    bool             used;
} m64_attach_t;

static m64_attach_t attachments[MIDI64_MIXER_MAX_ATTACH];

static m64_attach_t *find_attach(const midi64_player_t *p) {
    for (int i = 0; i < MIDI64_MIXER_MAX_ATTACH; i++)
        if (attachments[i].used && attachments[i].player == p) return &attachments[i];
    return NULL;
}

/**
 * @brief Mixer pull callback: generate @p wlen stereo frames.
 *
 * @p wpos is ignored deliberately. The generator has no random access, and with
 * an unknown length and no loop the mixer only ever asks for consecutive
 * frames, so honouring wpos would mean either lying or asserting. The one
 * `seeking` call that does arrive is the initial one at position 0, which is
 * where the player already is.
 */
static void m64_wave_read(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen,
                          bool seeking) {
    m64_attach_t *a = (m64_attach_t *)ctx;
    (void)wpos;
    (void)seeking;

    /* bps was set from bits*channels = 32, so one "sample" here is one stereo
     * frame and this pointer is good for wlen*4 bytes. */
    int16_t *dst = (int16_t *)samplebuffer_append(sbuf, wlen);
    midi64_player_render(a->player, dst, wlen);
}

void midi64_mixer_play(midi64_player_t *p, int ch) {
    if (!p || !p->seq) return;

    m64_attach_t *a = find_attach(p);
    if (!a) {
        for (int i = 0; i < MIDI64_MIXER_MAX_ATTACH; i++) {
            if (!attachments[i].used) { a = &attachments[i]; break; }
        }
    }
    assertf(a != NULL,
            "midi64: no free mixer attachment (max %d); call midi64_mixer_stop() "
            "on a finished player first", MIDI64_MIXER_MAX_ATTACH);

    memset(a, 0, sizeof(*a));
    a->used   = true;
    a->player = p;
    a->ch     = ch;

    a->wave.name      = "midi64";
    a->wave.bits      = 16;
    a->wave.channels  = 2;
    a->wave.frequency = (float)p->samplerate;
    a->wave.len       = WAVEFORM_UNKNOWN_LEN;
    a->wave.loop_len  = 0;
    a->wave.read      = m64_wave_read;
    a->wave.ctx       = a;

    mixer_ch_play(ch, &a->wave);
}

void midi64_mixer_stop(midi64_player_t *p) {
    m64_attach_t *a = find_attach(p);
    if (!a) return;
    mixer_ch_stop(a->ch);
    /* A stereo waveform occupies two mixer channels; mixer_ch_stop() on the
     * first releases the pair. */
    a->used = false;
    a->player = NULL;
}

#endif /* N64 */
