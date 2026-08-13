/**
 * @file music.c
 * @brief Background music implementation.
 * @ingroup menu
 */

#include <stdlib.h>
#include <time.h>
#include <libdragon.h>

#include <midi64.h>

#include "music.h"
#include "sound.h"

/** Music takes this mixer channel and the one after it -- a stereo waveform occupies a pair.
 *  Channel 0 is sound effects (SOUND_SFX_CHANNEL), so the two never contend. */
#define MUSIC_CHANNEL   1

/** Volume step 10 maps to midi64's own default of 16384 rather than to unity.
 *
 * Unity is 32768 and it clips: the loudest song in the set peaks at 58,736 rendered at unity,
 * 5.1 dB into the ceiling. A volume control whose top setting distorts is a broken volume
 * control, so the top setting is the loudest level the set was measured not to clip at. */
#define MUSIC_VOLUME_UNIT   16384

/* The path and the name are kept together deliberately. Deriving one from the other -- prettying
 * up a filename at runtime -- means a track named "Traveling The Sky" has to survive a
 * transformation that also has to get "Battle Theme III" right, for no gain: these files are
 * baked into the ROM, so the list cannot change without a rebuild anyway. */
static const struct { const char *path; const char *name; } TRACKS[] = {
    { "rom:/music/01-battle-theme.mid",      "Battle Theme" },
    { "rom:/music/02-lively-city.mid",       "Lively City" },
    { "rom:/music/03-royal-castle.mid",      "Royal Castle" },
    { "rom:/music/04-peaceful-village.mid",  "Peaceful Village" },
    { "rom:/music/05-long-journey.mid",      "Long Journey" },
    { "rom:/music/06-hidden-cavern.mid",     "Hidden Cavern" },
    { "rom:/music/07-spirit-forest.mid",     "Spirit Forest" },
    { "rom:/music/08-wood-forest-town.mid",  "Wood Forest Town" },
    { "rom:/music/09-battle-theme-ii.mid",   "Battle Theme II" },
    { "rom:/music/10-dwarven-mine.mid",      "Dwarven Mine" },
    { "rom:/music/11-dangerous-cave.mid",    "Dangerous Cave" },
    { "rom:/music/12-goofy-monster.mid",     "Goofy Monster" },
    { "rom:/music/13-magic-temple.mid",      "Magic Temple" },
    { "rom:/music/14-traveling-the-sky.mid", "Traveling The Sky" },
    { "rom:/music/15-volcanic-crater.mid",   "Volcanic Crater" },
    { "rom:/music/16-battle-theme-iii.mid",  "Battle Theme III" },
    { "rom:/music/17-unknown-island.mid",    "Unknown Island" },
    { "rom:/music/18-the-old-magician.mid",  "The Old Magician" },
    { "rom:/music/19-east-town.mid",         "East Town" },
    { "rom:/music/20-military-base.mid",     "Military Base" },
    { "rom:/music/21-malicious-scheme.mid",  "Malicious Scheme" },
    { "rom:/music/22-ancient-library.mid",   "Ancient Library" },
    { "rom:/music/23-pyramid.mid",           "Pyramid" },
    { "rom:/music/24-battle-theme-iv.mid",   "Battle Theme IV" },
    { "rom:/music/25-dark-factory.mid",      "Dark Factory" },
    { "rom:/music/26-demon-king-castle.mid", "Demon King Castle" },
    { "rom:/music/27-the-evil-one.mid",      "The Evil One" },
    { "rom:/music/28-holy-sanctuary.mid",    "Holy Sanctuary" },
};

#define TRACK_COUNT ((int)(sizeof(TRACKS) / sizeof(TRACKS[0])))

static bool tables_ready;       /**< midi64_synth_prepare() has run for SOUND_FREQUENCY */
static bool playing;            /**< song and player below are live and attached to the mixer */
static int  selected = MUSIC_TRACK_SHUFFLE;
static int  sounding = -1;      /**< index into TRACKS of what is actually playing */
static int  volume;             /**< 0..MUSIC_VOLUME_MAX */
static uint32_t rng;            /**< shuffle state; 0 until seeded */

static midi64_song_t   song;
static midi64_player_t player;

/** A 32-bit xorshift, local to this file on purpose.
 *
 * Not `rand()`: seeding the C library generator from here would silently reseed every other
 * caller of it in the program, and a shuffle is not worth owning global state for. */
static uint32_t next_random (void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/** Seed from the clock, mixed with libdragon's entropy source.
 *
 * The clock is the seed that was asked for and it is the right one when there is a clock. It is
 * not sufficient on its own here: with no RTC wired up `time()` returns the same epoch on every
 * switch-on, so a clock-only seed picks the same "random" song forever on exactly the hardware
 * most likely to have no clock. `getentropy32()` covers that case -- libdragon collects it during
 * IPL3 and documents it as differing on every boot on hardware.
 *
 * An earlier version mixed `TICKS_READ()` instead, on the reasoning that the boot path's cycle
 * count varies. **Measured, and it does not**: four scripted boots picked the same song every
 * time, because a scripted run pins the clock and ares' cycle counts are reproducible. That is
 * the whole reason this now uses the real entropy source rather than a plausible-looking one.
 *
 * Note the honest limit. `entropy.h` says outright that on an emulator the numbers are
 * consistent, so shuffle picks the same opening song on every ares run no matter what is done
 * here. There is no way to observe this working short of a console. */
static void seed_random (void) {
    if (rng != 0) return;
    rng = (uint32_t)time(NULL) * 2654435761u;
    rng ^= getentropy32();
    if (rng == 0) rng = 1;      /* xorshift is stuck at zero forever */
}

/** Pick a track at random, never the one already playing.
 *
 * Repeating a song immediately is the one shuffle outcome everybody notices and nobody wants, and
 * with 28 tracks it would come up about once every 28 changes. The retry cannot spin: `sounding`
 * is one value and there are 28 to choose from. */
static int pick_random (void) {
    int n = TRACK_COUNT;
    if (n <= 1) return 0;
    int pick;
    do {
        pick = (int)(next_random() % (uint32_t)n);
    } while (pick == sounding);
    return pick;
}

int music_track_count (void) {
    return TRACK_COUNT;
}

const char *music_track_name (int track) {
    if (track < 0 || track >= TRACK_COUNT) {
        return "Shuffle";
    }
    return TRACKS[track].name;
}

static int q15_volume (void) {
    if (volume <= 0) return 0;
    if (volume >= MUSIC_VOLUME_MAX) return MUSIC_VOLUME_UNIT;
    return (volume * MUSIC_VOLUME_UNIT) / MUSIC_VOLUME_MAX;
}

/** Tear down the player and the song, leaving the shared tables alone.
 *
 * The tables survive on purpose: they depend only on the sample rate, and rebuilding them is the
 * expensive thing in this whole file. Closing one song to open the next is the common case here
 * -- it is what the track row does on every press -- and it must not cost hundreds of
 * milliseconds. midi64's own cache regressed on exactly this once; see its docs/PERF.md. */
static void release (void) {
    if (!playing) return;
    midi64_mixer_stop(&player);
    midi64_player_close(&player);
    midi64_song_close(&song);
    playing = false;
}

/** Load and start TRACKS[index]. Returns false and leaves music off if the file will not play. */
static bool begin (int index) {
    release();

    if (index < 0 || index >= TRACK_COUNT) return false;

    int err = midi64_song_load(&song, TRACKS[index].path);
    if (err != MIDI64_OK) {
        debugf("[MUSIC] %s failed to load (%d)\n", TRACKS[index].path, err);
        return false;
    }

    /* Loop only when one track was chosen. On Shuffle the song has to be allowed to END so that
     * music_poll() sees midi64_player_done() and picks the next one; a looping player never
     * finishes and the setting would silently behave as "this track forever". */
    midi64_config_t cfg = {
        .synth         = MIDI64_SYNTH_PROCEDURAL,
        .bank_path     = NULL,
        .loop          = (selected != MUSIC_TRACK_SHUFFLE),
        .master_volume = q15_volume(),
    };
    err = midi64_player_init(&player, &song, SOUND_FREQUENCY, &cfg);
    if (err != MIDI64_OK) {
        debugf("[MUSIC] player init failed (%d)\n", err);
        midi64_song_close(&song);
        return false;
    }

    midi64_mixer_play(&player, MUSIC_CHANNEL);
    playing  = true;
    sounding = index;
    /* Named, not numbered, and logged on every start rather than only on failure: it is the only
     * way to see which song shuffle picked, since nothing on screen says. */
    debugf("[MUSIC] playing %s\n", TRACKS[index].name);
    return true;
}

void music_start (int track, int vol) {
    selected = (track < 0 || track >= TRACK_COUNT) ? MUSIC_TRACK_SHUFFLE : track;
    volume   = vol < 0 ? 0 : (vol > MUSIC_VOLUME_MAX ? MUSIC_VOLUME_MAX : vol);

    if (volume == 0) return;

    if (!tables_ready) {
        /* Timed because this is the one cost music adds to boot and nobody has a hardware number
         * for it. In ares it is 614 ms. It cannot be spread over several frames -- the build is a
         * single pass over 35 wavetables -- so the only two places it can go are here, behind the
         * boot plate where the library scan already lives, or on the first keypress, where a
         * half-second freeze would read as a crash. If the hardware number turns out to be
         * unacceptable, midi64's docs/PERF.md notes the tables are a pure function of (shape,
         * level, sample rate) and could be baked into the ROM as 71,680 bytes. */
        uint32_t t0 = TICKS_READ();
        if (midi64_synth_prepare(SOUND_FREQUENCY) != MIDI64_OK) {
            debugf("[MUSIC] out of memory building oscillator tables\n");
            volume = 0;
            return;
        }
        debugf("[MUSIC] tables built in %lu us\n",
               (unsigned long)TIMER_MICROS(TICKS_SINCE(t0)));
        tables_ready = true;
    }

    /* Random from the first note, not just between songs. Starting shuffle on track 1 every time
     * and only randomising the second song would mean everyone who ever switches the console on
     * and browses for a minute hears the same song, which is the case the setting exists for. */
    if (selected == MUSIC_TRACK_SHUFFLE) {
        seed_random();
        begin(pick_random());
    } else {
        begin(selected);
    }
}

void music_stop (void) {
    release();
}

void music_resume (void) {
    /* Restart from the stored selection rather than taking arguments, so the caller cannot
     * accidentally disagree with what boot chose -- under a scripted run that choice was the
     * pinned track, not the settings file, and a resume that re-read the settings would silently
     * unpin it. On shuffle this picks a fresh song, which is right: the grid is being arrived at
     * again, not un-paused. */
    if (playing || volume == 0) {
        return;
    }
    music_start(selected, volume);
}

void music_set_track (int track) {
    selected = (track < 0 || track >= TRACK_COUNT) ? MUSIC_TRACK_SHUFFLE : track;
    if (volume == 0) return;
    music_start(selected, volume);
}

void music_set_volume (int vol) {
    int next = vol < 0 ? 0 : (vol > MUSIC_VOLUME_MAX ? MUSIC_VOLUME_MAX : vol);
    if (next == volume) return;

    /* Zero stops rather than mixing silence. A muted player still renders every voice and throws
     * the result away, which is the full CPU cost of music for none of the music. */
    if (next == 0) {
        volume = 0;
        release();
        return;
    }

    bool was_off = (volume == 0);
    volume = next;
    /* `!playing` is not the same as `was_off`: a track whose file would not load leaves the
     * volume set and nothing playing, and setting the gain on a closed player would be reaching
     * into freed state. Restart instead, which also gives a failed load a second chance. */
    if (was_off || !playing) {
        music_start(selected, volume);
    } else {
        midi64_player_set_volume(&player, q15_volume());
    }
}

void music_fade (float gain) {
    if (!playing) {
        return;
    }
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    midi64_player_set_volume(&player, (int)(gain * (float)q15_volume()));
}

void music_poll (void) {
    if (!playing || selected != MUSIC_TRACK_SHUFFLE) return;
    if (!midi64_player_done(&player)) return;

    begin(pick_random());
}

void music_shutdown (void) {
    release();
    /* Only now, on the way out to a game, is giving the 71,680 bytes back worth anything. */
    midi64_synth_free_tables();
    tables_ready = false;
}
