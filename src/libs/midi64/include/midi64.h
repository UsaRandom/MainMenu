/**
 * @file midi64.h
 * @brief midi64 -- a Standard MIDI File player for the Nintendo 64
 *
 * midi64 parses a Standard MIDI File and renders it to interleaved stereo
 * 16-bit PCM. The core (parser, sequencer, synth) has no libdragon dependency
 * and no floating point, so the exact same object code runs on the host under
 * tools/render.c. That is deliberate: host renders and N64 renders are
 * bit-identical, which makes a WAV diff a real regression test rather than an
 * approximation of one.
 *
 * Typical libdragon use:
 * @code
 *     midi64_song_t song;
 *     midi64_song_load(&song, "rom:/bgm/01.mid");
 *     midi64_player_t player;
 *     midi64_player_init(&player, &song, 22050, NULL);
 *     midi64_mixer_play(&player, 1);   // occupy mixer channel 1
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MIDI64_H__
#define MIDI64_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum simultaneously sounding voices.
 *
 * The reference corpus peaks at 15 *notes*, but a voice outlives its note-off
 * by its release tail, and at 24 slots five of the 28 songs hit the cap and
 * started stealing. 32 clears the whole corpus with room to spare.
 *
 * Cost is linear in *sounding* voices, not in this number -- an idle slot costs
 * one predictable branch per control block -- so raising it is cheap and
 * lowering it does not buy much. */
#ifndef MIDI64_MAX_VOICES
#define MIDI64_MAX_VOICES   32
#endif

/** @brief MIDI channels. Always 16; channel 9 (0-based) is percussion. */
#define MIDI64_CHANNELS     16

/** @brief The percussion channel, 0-based. */
#define MIDI64_DRUM_CHANNEL 9

/** @brief Error codes. Negative values are failures. */
typedef enum {
    MIDI64_OK             =  0,  /**< Success */
    MIDI64_ERR_IO         = -1,  /**< File could not be read */
    MIDI64_ERR_FORMAT     = -2,  /**< Not a Standard MIDI File, or malformed */
    MIDI64_ERR_UNSUPPORTED= -3,  /**< Valid SMF this build cannot play (eg. SMPTE timing) */
    MIDI64_ERR_NOMEM      = -4,  /**< Allocation failed */
} midi64_error_t;

/** @brief A parsed song.
 *
 * Holds the whole file in RAM (the reference corpus tops out at 21 KB) and
 * points into it. No event list is built: playback is a k-way merge across the
 * track cursors, so memory is the file size and nothing more. */
typedef struct midi64_song_s {
    uint8_t  *data;         /**< Owned file image, freed by midi64_song_close() */
    size_t    size;         /**< Bytes in @ref data */
    uint16_t  format;       /**< SMF format, 0 or 1 */
    uint16_t  ntracks;      /**< Number of MTrk chunks */
    uint16_t  ppqn;         /**< Ticks per quarter note */
    bool      owns_data;    /**< False when built by midi64_song_open_mem() borrowing */
    struct m64_track_s *tracks; /**< Per-track chunk bounds, ntracks entries */
} midi64_song_t;

/** @brief Which synthesis backend renders the voices. */
typedef enum {
    MIDI64_SYNTH_PROCEDURAL = 0, /**< Band-limited wavetables generated at init. No assets. */
    MIDI64_SYNTH_BANK       = 1, /**< Sampled instruments from a .bank64 file. */
} midi64_synth_kind_t;

/** @brief Player configuration. Pass NULL to midi64_player_init() for defaults. */
typedef struct midi64_config_t {
    midi64_synth_kind_t synth;   /**< Default MIDI64_SYNTH_PROCEDURAL */
    const char *bank_path;       /**< Required when synth == MIDI64_SYNTH_BANK */
    bool  loop;                  /**< Restart at end of song. Default true. */
    int   master_volume;         /**< Q15 gain, 32768 == unity. 0 selects the default,
                                  *   which is 16384 -- see src/midi64.c for why. */
} midi64_config_t;

/** Opaque synth state; size is known so the player can embed it. */
typedef struct m64_synth_s m64_synth_t;

/** @brief A song being played. Embed it or heap-allocate it, your choice. */
typedef struct midi64_player_s {
    const midi64_song_t *song;
    struct m64_seq_s    *seq;    /**< Sequencer state (owned) */
    m64_synth_t         *synth;  /**< Synth state (owned) */
    int      samplerate;
    bool     playing;
    bool     loop;
    int      master_volume;
} midi64_player_t;

/* ---------------------------------------------------------------- songs -- */

/**
 * @brief Load and parse a Standard MIDI File.
 *
 * @param song  Destination, uninitialised on entry.
 * @param path  Any path stdio can open. Under libdragon that includes
 *              "rom:/" (DragonFS) and "sd:/" paths.
 * @return MIDI64_OK, or a negative @ref midi64_error_t.
 */
int midi64_song_load(midi64_song_t *song, const char *path);

/**
 * @brief Parse a Standard MIDI File already in memory.
 *
 * @param song   Destination, uninitialised on entry.
 * @param data   File image. Borrowed, not copied: it must outlive @p song.
 * @param size   Bytes in @p data.
 */
int midi64_song_open_mem(midi64_song_t *song, const uint8_t *data, size_t size);

/** @brief Release a song. Safe on an already-closed song. */
void midi64_song_close(midi64_song_t *song);

/**
 * @brief Song duration in milliseconds.
 *
 * Walks the whole event stream applying the tempo map, so it is O(events) --
 * a few milliseconds for a typical file. Returns 0 for an empty song.
 */
uint32_t midi64_song_duration_ms(const midi64_song_t *song);

/**
 * @brief Copy the sequence name into @p out.
 *
 * Read from track 0 only, where format 1 puts the *sequence* name; in later
 * tracks the same meta event is the track or instrument name. Most files carry
 * no sequence name at all, so this returns false more often than not -- prefer
 * the filename. See docs/LIMITS.md.
 */
bool midi64_song_title(const midi64_song_t *song, char *out, size_t outsz);

/* -------------------------------------------------------------- players -- */

/**
 * @brief Prepare a player for a song.
 *
 * @param p           Destination, uninitialised on entry.
 * @param song        Borrowed; must outlive the player.
 * @param samplerate  Output rate in Hz. Must match the mixer's rate.
 * @param cfg         Optional; NULL selects the documented defaults.
 */
int  midi64_player_init(midi64_player_t *p, const midi64_song_t *song,
                        int samplerate, const midi64_config_t *cfg);

/** @brief Release a player. Safe on an already-closed player. */
void midi64_player_close(midi64_player_t *p);

/**
 * @brief Render @p nframes interleaved stereo frames.
 *
 * This is the whole engine: it advances the sequencer, applies events and
 * mixes every sounding voice. Output is *replaced*, not accumulated.
 *
 * When the song ends and looping is off, the remainder of the buffer is
 * filled with silence and midi64_player_done() starts returning true.
 *
 * @param out      Destination, 2 * @p nframes int16 samples (L,R,L,R...).
 * @param nframes  Frames to produce.
 */
void midi64_player_render(midi64_player_t *p, int16_t *out, int nframes);

/** @brief Rewind to the start and silence all voices. */
void midi64_player_rewind(midi64_player_t *p);

/** @brief Pause or resume. A paused player renders silence but keeps its state. */
void midi64_player_set_paused(midi64_player_t *p, bool paused);

/** @brief True once a non-looping song has run past its last event. */
bool midi64_player_done(const midi64_player_t *p);

/** @brief Set master gain. @p vol is Q15: 32768 is unity, 0 is silence. */
void midi64_player_set_volume(midi64_player_t *p, int vol);

/** @brief Playback position in milliseconds. */
uint32_t midi64_player_position_ms(const midi64_player_t *p);

/** @brief Voices currently sounding. Useful for budgeting; see docs/PERF.md. */
int  midi64_player_active_voices(const midi64_player_t *p);

/**
 * @brief Build the shared oscillator tables ahead of time.
 *
 * The procedural synth needs ~90 KB of band-limited wavetables before it can
 * make a sound. They are built on the first midi64_player_init() at a given
 * sample rate, which puts the entire cost on whatever moment the user pressed
 * play -- a visible stall of hundreds of milliseconds on hardware.
 *
 * Call this once at boot, or behind a loading screen, with the same sample rate
 * you will pass to midi64_player_init(). Every player at that rate then starts
 * in microseconds. Calling it twice for the same rate is nearly free.
 *
 * @param samplerate  Must match what you later pass to midi64_player_init();
 *                    a different rate rebuilds and this call is wasted.
 * @return MIDI64_OK, or MIDI64_ERR_NOMEM.
 */
int  midi64_synth_prepare(int samplerate);

/**
 * @brief Free the shared oscillator tables once no player is using them.
 *
 * The procedural synth's ~90 KB of band-limited wavetables depend only on the
 * sample rate, so they are built once and shared by every player at that rate.
 * Closing the last player deliberately keeps them, because the common case is
 * closing one song to open the next and rebuilding costs hundreds of
 * milliseconds on hardware.
 *
 * Call this when you are done with music for a while and want the memory back.
 * It is a no-op while any player still exists, so it is always safe to call.
 */
void midi64_synth_free_tables(void);

/* ------------------------------------------------- libdragon integration -- */
#ifdef N64

/**
 * @brief Attach a player to a libdragon mixer channel and start it.
 *
 * midi64 renders every voice itself into one stereo waveform_t, so it costs a
 * single mixer channel no matter how many notes sound at once. The channel's
 * sample rate is set to the player's rate.
 *
 * Call mixer_ch_stop() to stop, exactly as with wav64.
 *
 * @param p   An initialised player. Must outlive the playback.
 * @param ch  Mixer channel index. Stereo uses this channel and the next one.
 */
void midi64_mixer_play(midi64_player_t *p, int ch);

/** @brief Stop a player attached with midi64_mixer_play() and free its channel. */
void midi64_mixer_stop(midi64_player_t *p);

#endif /* N64 */

#ifdef __cplusplus
}
#endif

#endif /* MIDI64_H__ */
