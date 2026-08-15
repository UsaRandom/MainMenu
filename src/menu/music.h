/**
 * @file music.h
 * @brief Background music: the shipped MIDI set, played by midi64.
 * @ingroup menu
 *
 * Every track is a Standard MIDI File baked into the ROM and synthesised on the fly. That is not
 * an aesthetic choice, it is the only one that fits: the whole 28-track set is 295,760 bytes of
 * .mid against 591,854 for two of them rendered to Opus, and the engine that plays it is 17,938
 * bytes of code. The alternative this replaced -- one 536,219-byte wav64 nothing ever opened --
 * cost more than all of it for no music at all.
 *
 * The second reason matters more on hardware than in an emulator. wav64 streams continuously off
 * storage during playback, which would have put a permanent second reader on the same
 * FatFs-over-SC64 pipe the thumbnail streamer already competes for during a scroll. midi64 reads
 * the file once into RAM at load and never touches storage again, so music costs nothing on the
 * one budget AUDIT.md says ares cannot measure.
 */

#ifndef MUSIC_H__
#define MUSIC_H__

#include <stdbool.h>

/** @brief Volume steps, for both music and sound effects. 0 is off. */
#define MUSIC_VOLUME_MAX    10

/** @brief The value of settings_t::music_track that plays the set in a random order.
 *
 * Seeded from the clock, so which song greets you is different from one switch-on to the next.
 * That deliberately makes the shuffle path non-reproducible, which is why nothing scripted ever
 * takes it: app.c pins a scripted run to one fixed looping track instead. A regression run has to
 * be a pure function of its script, and the shuffle is the one thing here that is a function of
 * the time of day. */
#define MUSIC_TRACK_SHUFFLE (-1)

/** @brief Number of shipped tracks. */
int music_track_count (void);

/** @brief Display name of a track, or "Shuffle" for #MUSIC_TRACK_SHUFFLE. */
const char *music_track_name (int track);

/**
 * @brief Store the selection and build the oscillator tables, without starting playback.
 *
 * The build is a few hundred milliseconds and cannot be chunked, so it belongs at boot behind
 * the boot plate, never on a keypress. Playback is a separate matter: on hardware the first
 * second of a song started under the plate skipped audibly, because the library scan and the
 * icon pump starve the mixer exactly then -- so boot prepares here and music_resume() starts
 * the song once the plate has lifted, when the console has nothing better to do than play it.
 * A no-op when @p volume is 0, which is what keeps the build cost off a console whose owner
 * does not want music.
 */
void music_prepare (int track, int volume);

/** @brief Start music, building the oscillator tables if this is the first call. */
void music_start (int track, int volume);

/** @brief Stop playback and release the player. The tables are kept; see music_shutdown(). */
void music_stop (void);

/**
 * @brief Start again after music_stop(), from the selection and volume already stored.
 *
 * For the one launch path that comes back: on the dummy flashcart a "launch" fades out, stops the
 * music, pretends to load, and returns to the grid -- which then sat silent for the rest of the
 * session, because only boot ever called music_start(). A real launch never returns and never
 * gets here. No-op while playing or when the volume is 0.
 */
void music_resume (void);

/** @brief Switch tracks, starting music if it was off. */
void music_set_track (int track);

/** @brief Set volume in steps of 0..#MUSIC_VOLUME_MAX. 0 stops, non-zero from 0 starts. */
void music_set_volume (int volume);

/**
 * @brief Scale the current volume by @p gain (0..1) without changing the setting.
 *
 * For the launch fade, which needs the music to arrive at silence at the same moment the picture
 * does. music_set_volume() is the wrong tool: it works in ten steps, it persists, and it releases
 * the player at zero -- so a fade through it would be audibly stepped and would leave the user's
 * volume set to nothing the next time they switched on.
 *
 * Nothing restores the scaled gain on a live player. The only caller is on its way out of the
 * program; the one path that comes back (the dummy-cart launch) goes through music_stop() and
 * music_resume(), which builds a fresh player at full gain.
 */
void music_fade (float gain);

/** @brief Advance to the next track when a shuffled song ends. Call once per frame. */
void music_poll (void);

/** @brief Stop and free everything, including the oscillator tables. */
void music_shutdown (void);

#endif /* MUSIC_H__ */
