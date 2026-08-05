/**
 * @file sound.c
 * @brief Sound component implementation
 * @ingroup ui_components
 */

#include <stdbool.h>
#include <libdragon.h>
#include "sound.h"

/** Audio buffers held ahead of the DAC.
 *
 * Eight, not the four this shipped with. The synth is not the bottleneck -- `snd_us` says
 * mixer_try_play() costs about 1 ms of a 16.7 ms frame, 6% of wall clock -- but that work is
 * delivered in bursts, and the menu's frame time is not: a third of frames take two fields with
 * music on, and art decoding in background() can hold the loop for longer still. Four buffers is
 * roughly 67 ms of slack, which a run of long frames eats, and the result is audible as
 * choppiness even though the CPU has plenty of headroom left.
 *
 * The cost is bounded and small: audio_get_buffer_length() stereo 16-bit samples per buffer, so
 * four more at 22050 Hz is on the order of 12 KB. Latency rises with the count, which nothing
 * here cares about -- no sound effect is triggered tightly enough for 60 ms to be noticed. */
#define NUM_BUFFERS         (8)

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
static bool sfx_enabled = false;
static float sfx_volume = SFX_VOLUME_UNIT;

/**
 * @brief Reconfigure the sound system with the specified frequency.
 * 
 * @param frequency The audio frequency.
 */
static void sound_reconfigure (int frequency) {
    if ((frequency > 0) && (audio_get_frequency() != frequency)) {
        
        sound_deinit();

        audio_init(frequency, NUM_BUFFERS);
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
        
        // Check whether one audio buffer is ready, otherwise wait for next
        // frame to perform mixing.
        mixer_try_play();
    }
}
