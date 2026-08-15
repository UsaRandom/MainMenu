/**
 * @file sound.c
 * @brief Sound component implementation
 * @ingroup ui_components
 */

#include <stdbool.h>
#include <libdragon.h>
#include "sound.h"
#include "menu/memprofile.h"

/** Audio buffers the mixer keeps QUEUED ahead of the DAC, as opposed to allocated.
 *
 * Eight, not the four this shipped with. The synth is not the bottleneck -- `snd_us` says
 * the mixer costs about 1 ms of a 16.7 ms frame, 6% of wall clock -- but that work is
 * delivered in bursts, and the menu's frame time is not: a third of frames take two fields with
 * music on, and art decoding in background() can hold the loop for longer still. Four buffers is
 * roughly 67 ms of slack, which a run of long frames eats, and the result is audible as
 * choppiness even though the CPU has plenty of headroom left.
 *
 * Latency rises with the depth, and the sound effects are what cares: an effect is mixed into
 * the next buffer rendered, so it reaches the ear after everything already queued. Eight
 * buffers is ~316 ms at 16 kHz, which nothing is triggered tightly enough to notice. This is
 * why the ALLOCATED count (mem_audio_buffers(), up to 128 on the full profile) is a separate
 * number: capacity beyond eight exists only for sound_prefill() at boot, and sound_poll()
 * lets it drain back to this depth rather than keeping it full. */
#define QUEUE_TARGET        (8)

/** Mixer channels. Three are used: 0 is effects, 1 and 2 are music's stereo pair.
 *
 * Sixteen was upstream's number and it was never revisited. The RSP mixer ucode walks every
 * channel it was given for every buffer it mixes, so thirteen permanently silent ones are work
 * the RSP does on the same queue rdpq draws the grid through. Four leaves one spare. */
#ifndef NUM_CHANNELS
#define NUM_CHANNELS        (4)
#endif

/** Half of full scale, which is where this sat as a hard-coded constant before there was a
 *  volume control. It is the level every effect in the set was chosen against, so it stays the
 *  default rather than becoming the maximum. */
#define SFX_VOLUME_UNIT     (0.5f)

static wav64_t sfx_cursor, sfx_error, sfx_enter, sfx_exit, sfx_setting;

static bool sound_initialized = false;

/** How long the AI has gone unfed, at worst, since the last time anybody asked.
 *
 * A buffer is CALC_BUFFER(SOUND_FREQUENCY) samples and there are NUM_BUFFERS of them: at 16 kHz
 * that is 640 samples each, eight of them, 320 ms of slack in total. Go past that and the AI does
 * not fall silent, it repeats the last buffer it was handed -- which is what a stalled boot sounds
 * like, and what nothing in this program could measure until now. ares cannot reproduce the case
 * that prompted it, a card with a warm index revalidating for about a second, because its storage
 * is read-only and there is never a warm index -- so the number has to come off the console, which
 * means it has to be recorded on the console. */
static uint32_t last_poll_ticks;
static uint32_t worst_gap_us;
static bool sfx_enabled = false;
static float sfx_volume = SFX_VOLUME_UNIT;

/** The queue's depth, estimated: samples written minus samples the DAC has had time to play.
 *
 * libdragon says whether ONE buffer is free (audio_can_write()) and nothing else, so the depth
 * is accounted here: every fill adds to `samples_written`, and consumption is elapsed time
 * times the sample rate, which is exact on the console -- the AI and the CPU counter run off
 * the same crystal. The estimate self-corrects at both rails: a full queue refuses the next
 * write (depth is then capacity by definition), and the clamp below stops a long-starved
 * queue from going negative. */
static uint64_t samples_written;
static uint32_t consume_base_ticks;     /* when the DAC started draining, i.e. first write */
static uint64_t consumed_us_hi;         /* whole-second carry, so the microsecond math never overflows */

static int64_t queue_depth_samples (void) {
    if (samples_written == 0) {
        return 0;
    }
    uint32_t elapsed = TIMER_MICROS(TICKS_SINCE(consume_base_ticks));
    /* Fold whole seconds out of the tick distance before it wraps: TICKS_SINCE is only good for
     * about 91 seconds, and this program runs for hours. */
    while (elapsed >= 1000000) {
        elapsed -= 1000000;
        consumed_us_hi += 1000000;
        consume_base_ticks += TICKS_FROM_MS(1000);
    }
    uint64_t consumed = (consumed_us_hi + elapsed) * (uint64_t)audio_get_frequency() / 1000000ull;
    int64_t depth = (int64_t)samples_written - (int64_t)consumed;
    return depth > 0 ? depth : 0;
}

/** Render and queue buffers until the depth reaches @p target_buffers or the queue is full. */
static void fill_to (int target_buffers) {
    int len = audio_get_buffer_length();
    int64_t target = (int64_t)target_buffers * len;

    while (queue_depth_samples() < target && audio_can_write()) {
        short *out = audio_write_begin();
        mixer_poll(out, len);
        audio_write_end();
        if (samples_written == 0) {
            consume_base_ticks = TICKS_READ();
        }
        samples_written += len;
    }
}

/**
 * @brief Reconfigure the sound system with the specified frequency.
 * 
 * @param frequency The audio frequency.
 */
static void sound_reconfigure (int frequency) {
    if ((frequency > 0) && (audio_get_frequency() != frequency)) {
        
        sound_deinit();

        /* A new queue knows nothing of the old one's accounting. Without this reset a
         * reconfigure mid-session would carry samples_written across, the depth estimate would
         * read as seconds of phantom backlog, and sound_poll() would refuse to feed a queue
         * that is actually empty. */
        samples_written = 0;
        consumed_us_hi = 0;

        audio_init(frequency, mem_audio_buffers());
        mixer_init(NUM_CHANNELS);

        // Attempt to initialize wav64 compression level 1
        wav64_init_compression(1);

        sound_initialized = true;

        if (sfx_enabled) {
            sound_init_sfx();
        }
    }
}

/**
 * @brief Initialize the default sound system.
 */
void sound_init_default (void) {
    sound_reconfigure(SOUND_FREQUENCY);
}

/**
 * @brief Initialize the sound effects.
 */
void sound_init_sfx (void) {
    // The effects are 44.1 kHz files and the mixer now runs at 22050 for music's sake, so the
    // channel has to be told the higher source rate or its buffer is sized for the lower one.
    mixer_ch_set_limits(SOUND_SFX_CHANNEL, 16, SOUND_SFX_MAX_FREQUENCY, 0);
    mixer_ch_set_vol(SOUND_SFX_CHANNEL, sfx_volume, sfx_volume);
    wav64_open(&sfx_cursor, "rom:/cursorsound.wav64");
    wav64_open(&sfx_exit, "rom:/back.wav64");
    wav64_open(&sfx_setting, "rom:/settings.wav64");
    wav64_open(&sfx_enter, "rom:/enter.wav64");
    wav64_open(&sfx_error, "rom:/error.wav64");
    sfx_enabled = true;
}

/**
 * @brief Enable or disable sound effects.
 * 
 * @param state True to enable, false to disable.
 */
void sound_use_sfx(bool state) {
    sfx_enabled = state;
}

/**
 * @brief Set sound effect volume in steps.
 *
 * @param volume 0 (off) to SOUND_SFX_VOLUME_MAX.
 */
void sound_set_sfx_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > SOUND_SFX_VOLUME_MAX) volume = SOUND_SFX_VOLUME_MAX;

    sfx_enabled = (volume > 0);
    sfx_volume = (SFX_VOLUME_UNIT * (float)volume) / (float)SOUND_SFX_VOLUME_MAX;

    /* Only meaningful once the mixer exists. sound_init_sfx() applies whatever is set here, so
     * calling this before initialisation records the level rather than losing it. */
    if (sound_initialized) {
        mixer_ch_set_vol(SOUND_SFX_CHANNEL, sfx_volume, sfx_volume);
    }
}

/**
 * @brief Play a sound effect.
 * 
 * @param sfx The sound effect to play.
 */
void sound_play_effect(sound_effect_t sfx) {
    if(sfx_enabled) {
        switch (sfx) {
            case SFX_CURSOR:
                wav64_play(&sfx_cursor, SOUND_SFX_CHANNEL);
                break;
            case SFX_EXIT:
                wav64_play(&sfx_exit, SOUND_SFX_CHANNEL);
                break;
            case SFX_SETTING:
                wav64_play(&sfx_setting, SOUND_SFX_CHANNEL);
                break;
            case SFX_ENTER:
                wav64_play(&sfx_enter, SOUND_SFX_CHANNEL);
                break;
            case SFX_ERROR:
                wav64_play(&sfx_error, SOUND_SFX_CHANNEL);
                break;
            default:
                break;
        } 
    }
}

/**
 * @brief Deinitialize the sound system.
 */
void sound_deinit (void) {
    if (sound_initialized) {
        if (sfx_enabled) {
            wav64_close(&sfx_cursor);
            wav64_close(&sfx_exit);
            wav64_close(&sfx_setting);
            wav64_close(&sfx_enter);
            wav64_close(&sfx_error);
        }
        mixer_close();
        audio_close();
        sound_initialized = false;
    }
}

/**
 * @brief Poll the sound system to process audio playback.
 */
void sound_poll (void) {
    if (sound_initialized) {
        uint32_t now = TICKS_READ();
        if (last_poll_ticks != 0) {
            uint32_t gap = TIMER_MICROS(TICKS_DISTANCE(last_poll_ticks, now));
            if (gap > worst_gap_us) {
                worst_gap_us = gap;
            }
        }
        last_poll_ticks = now;

        /* Top the queue up to the working depth and no further. On the small profile the queue
         * IS the working depth and this behaves exactly like the mixer_try_play() it replaced;
         * on the full profile it is what lets the boot prefill drain back down instead of being
         * topped up to five seconds forever -- which would delay every sound effect by the whole
         * queue, since an effect is mixed into the next buffer rendered, not into the ones
         * already waiting. */
        fill_to(QUEUE_TARGET);
    }
}

void sound_prefill (void) {
    if (!sound_initialized) {
        return;
    }
    /* Fill the whole allocation, not the working depth: this is the one caller allowed past
     * QUEUE_TARGET. On the small profile the two are the same and this is a cheap no-op-ish
     * top-up. Timed because it is boot cost on the boot path -- the synth renders the whole
     * bank here, and nobody has a hardware number for that yet. */
    uint32_t t0 = TICKS_READ();
    fill_to(mem_audio_buffers());
    debugf("[SOUND] prefill to %lld ms took %lu us\n",
           (long long)(queue_depth_samples() * 1000 / audio_get_frequency()),
           (unsigned long)TIMER_MICROS(TICKS_SINCE(t0)));
}

unsigned sound_worst_gap_us (void) {
    unsigned worst = worst_gap_us;
    worst_gap_us = 0;
    return worst;
}

unsigned sound_slack_us (void) {
    /* Asked of the audio subsystem rather than recomputed from SOUND_FREQUENCY, because
     * audio_init() rounds the buffer length down to a multiple of eight and a second copy of that
     * arithmetic here would drift the moment either number moved. */
    int freq = audio_get_frequency();
    if (freq <= 0) {
        return 0;
    }
    /* The WORKING depth, not the allocation: this number is read against worst-gap to say
     * whether ordinary polling can starve, and ordinary polling never holds more than the
     * target. The prefill bank above it is boot's business and boot logs it separately. */
    return (unsigned)((uint64_t)audio_get_buffer_length() * QUEUE_TARGET * 1000000ull /
                      (uint64_t)freq);
}
