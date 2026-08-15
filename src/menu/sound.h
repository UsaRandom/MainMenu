/**
 * @file sound.h
 * @brief Menu Sound
 * @ingroup menu
 */

#ifndef SOUND_H__
#define SOUND_H__

#include <stdbool.h>

#define SOUND_SFX_CHANNEL           (0) /**< Channel for sound effects */

/** @brief Mixer output rate, shared by sound effects and music.
 *
 * 16000, down from the 44100 this used to run at and then from 22050. The effects are 44.1 kHz
 * wav64 files and are resampled down by the mixer, which is what SOUND_SFX_MAX_FREQUENCY below
 * sizes the buffer for.
 *
 * The rate is the only lever that does anything. Six were measured against the same idle script
 * -- rate, midi64's control block, its voice cap, audio buffer depth, mid-frame mixer pumping and
 * the mixer channel count -- and five of them land within noise of each other. Halving the rate
 * recovers about a quarter of the missed fields; nothing else recovers any. See AUDIT.md.
 *
 * 16000 rather than 11025 because midi64's own docs/PERF.md puts it above what its patch set's
 * low-pass corners need, so it is the last step down that costs nothing audible.
 *
 * Anything using this must pass the same number to midi64_player_init(): a player whose rate
 * disagrees with the mixer's plays at the wrong pitch rather than failing.
 *
 * Overridable so the rate can be swept without editing this file:
 *     make TUNE='-DSOUND_FREQUENCY=22050' ...
 * which is how the cost below was established as proportional to the rate rather than fixed. */
#ifndef SOUND_FREQUENCY
#define SOUND_FREQUENCY             (16000)
#endif

/** @brief Highest source rate a sound effect may have. Sizes the SFX channel's buffer. */
#define SOUND_SFX_MAX_FREQUENCY     (44100)

/** @brief Volume steps for sound effects. 0 is off. */
#define SOUND_SFX_VOLUME_MAX        (10)


/**
 * @brief Enumeration of available sound effects for menu interactions.
 * 
 * This enumeration defines the different sound effects that can be used
 * for menu interactions.
 */
typedef enum {
    SFX_CURSOR,  /**< Sound effect for cursor movement */
    SFX_ERROR,   /**< Sound effect for error */
    SFX_ENTER,   /**< Sound effect for entering a menu */
    SFX_EXIT,    /**< Sound effect for exiting a menu */
    SFX_SETTING, /**< Sound effect for changing a setting */
} sound_effect_t;

/**
 * @brief Initialize the default sound system.
 * 
 * This function initializes the default sound system, setting up
 * necessary resources and configurations.
 */
void sound_init_default(void);

/**
 * @brief Initialize the sound effects system.
 * 
 * This function initializes the sound effects system, setting up
 * necessary resources and configurations for playing sound effects.
 */
void sound_init_sfx(void);

/**
 * @brief Enable or disable sound effects.
 *
 * @param enable True to enable sound effects, false to disable.
 */
void sound_use_sfx(bool enable);

/**
 * @brief Set sound effect volume in steps of 0..#SOUND_SFX_VOLUME_MAX.
 *
 * 0 switches effects off. Anything above it both enables them and sets the level, so a caller
 * never has to keep a separate bool in step with a number.
 */
void sound_set_sfx_volume(int volume);

/**
 * @brief Play a specified sound effect.
 * 
 * @param sfx The sound effect to play, as defined in sound_effect_t.
 */
void sound_play_effect(sound_effect_t sfx);

/**
 * @brief Deinitialize the sound system.
 * 
 * This function deinitializes the sound system, releasing any resources
 * that were allocated.
 */
void sound_deinit(void);

/**
 * @brief Poll the sound system.
 * 
 * This function polls the sound system, updating its state as necessary.
 */
void sound_poll(void);


/**
 * @brief The longest gap between two sound_poll() calls since this was last asked, in us. Resets.
 *
 * Compare against sound_slack_us(). A gap larger than that is not a dropped frame, it is the AI
 * repeating its last buffer -- audible, and the reason this exists: music started at boot, nothing
 * fed the mixer through the fonts or the index revalidation, and the boot plate came up to a
 * stalled fragment of a song. There is no other way to ask that question from a console.
 */
unsigned sound_worst_gap_us (void);

/** @brief How much audio is buffered ahead of the DAC, in us. The budget sound_worst_gap_us()
 *         has to stay under. */
unsigned sound_slack_us (void);

#endif /* SOUND_H__ */
