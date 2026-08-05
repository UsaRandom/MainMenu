/**
 * @file patches.c
 * @brief GM program and percussion key mappings for the procedural backend.
 *
 * Every General MIDI program gets an oscillator shape, an ADSR and a filter
 * corner. This is a synth impression of General MIDI, not an emulation of it:
 * a two-oscillator-free, sample-free voice cannot be a trumpet. What it can do
 * is preserve register, articulation and decay envelope, which is most of what
 * makes a sequenced arrangement legible.
 *
 * All 128 programs are covered so that an arbitrary file plays without falling
 * off a table. The 33 programs the reference corpus actually uses (listed in
 * docs/CORPUS.md) were tuned by ear against the host renderer; the rest are
 * family defaults and are honestly just plausible.
 *
 * Per-patch `gain` exists because the shapes are wildly unequal in power: a
 * band-limited saw carries roughly 3x the RMS of a sine at the same peak, so
 * without a trim the string patches bury everything else in the mix.
 *
 * SPDX-License-Identifier: MIT
 */

#include "midi64_internal.h"

/* shape, attack, decay, sustain, release, noise, cutoff, gain
 *          ms      ms    Q15      ms     Q15      Hz    Q15   */
#define P(sh, a, d, s, r, n, c, g) \
    { M64_SHAPE_##sh, (a), (d), (s), (r), (n), (c), (g) }

static const m64_patch_t gm_patches[128] = {
    /* 0-7  Piano -- fast attack, long decay, no sustain: a struck string does
     * not hold, and giving it sustain is the single most common way a MIDI
     * synth ends up sounding like an organ. */
    P(TRIANGLE,  2, 1200,     0,  260,     0, 4200, 26000), /*  0 Acoustic Grand   */
    P(TRIANGLE,  2, 1100,     0,  240,     0, 5200, 26000), /*  1 Bright Acoustic  */
    P(SAW,       3, 1000,     0,  240,     0, 3000, 15000), /*  2 Electric Grand   */
    P(TRIANGLE,  4, 1400,     0,  300,     0, 3600, 25000), /*  3 Honky-tonk       */
    P(SINE,      3,  900,  3000,  260,     0, 5000, 29000), /*  4 Electric Piano 1 */
    P(SINE,      4, 1000,  4000,  300,     0, 4200, 29000), /*  5 Electric Piano 2 */
    P(TRIANGLE,  6, 1600,     0,  360,     0, 2800, 25000), /*  6 Harpsichord      */
    P(SQUARE,    2,  700,     0,  180,     0, 4000, 13000), /*  7 Clavinet         */

    /* 8-15 Chromatic percussion -- struck metal and wood. Short, bright,
     * no sustain, and a sine core so the decay reads as a ring not a buzz. */
    P(SINE,      1,  700,     0,  180,     0,     0, 30000), /*  8 Celesta         */
    P(SINE,      1,  500,     0,  140,  1200,     0, 29000), /*  9 Glockenspiel    */
    P(SINE,      2,  900,     0,  200,     0,     0, 29000), /* 10 Music Box       */
    P(SINE,      2, 1500,     0,  400,     0,     0, 30000), /* 11 Vibraphone      */
    P(SINE,      1,  420,     0,  120,     0,     0, 30000), /* 12 Marimba         */
    P(TRIANGLE,  1,  600,     0,  160,  2000, 7000, 27000), /* 13 Xylophone       */
    P(SINE,      3, 2600,     0,  900,  1500,     0, 30000), /* 14 Tubular Bells   */
    P(TRIANGLE,  6, 1800,  4000,  400,     0, 3000, 26000), /* 15 Dulcimer        */

    /* 16-23 Organ -- the opposite of piano: full sustain, near-zero decay, and
     * a release short enough that chord changes do not smear. */
    P(SQUARE,    8,   60, 28000,   90,     0, 3200, 14000), /* 16 Drawbar Organ   */
    P(SQUARE,    6,   60, 30000,   70,     0, 4200, 13000), /* 17 Percussive Organ*/
    P(SQUARE,   10,   80, 27000,  110,     0, 2800, 14000), /* 18 Rock Organ      */
    P(SAW,      26,  120, 29000,  260,     0, 2600, 15000), /* 19 Church Organ    */
    P(TRIANGLE, 20,  100, 28000,  200,     0, 2400, 25000), /* 20 Reed Organ      */
    P(SAW,      14,   90, 27000,  150,     0, 3000, 15000), /* 21 Accordion       */
    P(SAW,      18,  110, 26000,  180,   600, 2200, 15000), /* 22 Harmonica       */
    P(SAW,      14,   90, 27000,  150,     0, 3000, 15000), /* 23 Tango Accordion */

    /* 24-31 Guitar -- plucked, so decay-to-zero like the piano family, but with
     * a shorter tail and a darker filter. */
    P(TRIANGLE,  2,  900,     0,  200,   700, 3400, 27000), /* 24 Nylon Guitar    */
    P(SAW,       2,  850,     0,  190,   900, 3800, 15000), /* 25 Steel Guitar    */
    P(SAW,       2,  800,     0,  180,   500, 3400, 15000), /* 26 Jazz Guitar     */
    P(TRIANGLE,  2,  700,     0,  160,   400, 4000, 27000), /* 27 Clean Guitar    */
    P(TRIANGLE,  2,  500,     0,  120,     0, 3000, 27000), /* 28 Muted Guitar    */
    P(SQUARE,    4,  600, 12000,  200,  1400, 2600, 12000), /* 29 Overdriven Gtr  */
    P(SQUARE,    4,  700, 16000,  240,  2200, 2400, 12000), /* 30 Distortion Gtr  */
    P(SINE,      6, 1200,     0,  300,     0, 3000, 30000), /* 31 Guitar Harmonics*/

    /* 32-39 Bass -- low register wants a filtered triangle or saw. The cutoff
     * is deliberately low: unfiltered saw bass on the N64's output stage turns
     * into buzz well before it turns into weight. */
    P(TRIANGLE,  4,  900,  6000,  220,     0, 1100, 30000), /* 32 Acoustic Bass   */
    P(SAW,       3,  800,  8000,  200,     0, 1400, 17000), /* 33 Fingered Bass   */
    P(SAW,       3,  850,  7000,  210,     0, 1300, 17000), /* 34 Finger Bass     */
    P(SINE,      5, 1400, 12000,  320,     0,     0, 32000), /* 35 Fretless Bass  */
    P(SAW,       2,  600,  4000,  180,   500, 1900, 17000), /* 36 Slap Bass 1     */
    P(SAW,       2,  600,  4000,  180,   700, 2100, 17000), /* 37 Slap Bass 2     */
    P(SQUARE,    3,  700,  9000,  200,     0, 1500, 14000), /* 38 Synth Bass 1    */
    P(SAW,       3,  750, 10000,  220,     0, 1300, 17000), /* 39 Synth Bass 2    */

    /* 40-47 Strings -- bowed: slow attack, full sustain. Pizzicato and harp at
     * 45/46 are plucked and break the pattern, and timpani at 47 is a drum. */
    P(SAW,      60,  200, 26000,  280,     0, 2600, 16000), /* 40 Violin          */
    P(SAW,      70,  220, 26000,  300,     0, 2300, 16000), /* 41 Viola           */
    P(SAW,      80,  240, 26000,  340,     0, 1900, 16000), /* 42 Cello           */
    P(SAW,      90,  260, 26000,  380,     0, 1500, 16000), /* 43 Contrabass      */
    P(SAW,      70,  220, 27000,  300,   400, 2400, 16000), /* 44 Tremolo Strings */
    P(TRIANGLE,  2,  380,     0,  110,   600, 3200, 28000), /* 45 Pizzicato       */
    P(TRIANGLE,  2, 1500,     0,  400,     0, 4000, 28000), /* 46 Orchestral Harp */
    P(SINE,      1,  800,     0,  240, 12000,  900, 32000), /* 47 Timpani         */

    /* 48-55 Ensemble -- the same bowed envelope, softened. Choir gets a sine
     * core and a low corner because a saw choir is unlistenable. */
    P(SAW,      70,  240, 27000,  420,     0, 2200, 15000), /* 48 String Ens 1    */
    P(SAW,      90,  260, 27000,  520,     0, 1900, 15000), /* 49 String Ens 2    */
    P(SAW,      50,  200, 28000,  380,     0, 2600, 15000), /* 50 Synth Strings 1 */
    P(SAW,      60,  220, 28000,  420,     0, 2300, 15000), /* 51 Synth Strings 2 */
    P(SINE,     80,  260, 27000,  480,   500, 1600, 31000), /* 52 Choir Aahs      */
    P(SINE,     30,  200, 26000,  300,   700, 2000, 31000), /* 53 Voice Oohs      */
    P(TRIANGLE, 60,  240, 27000,  420,   400, 2200, 26000), /* 54 Synth Voice     */
    P(SAW,       4,  700,     0,  260,  1200, 3000, 15000), /* 55 Orchestra Hit   */

    /* 56-63 Brass -- fast but not instant attack, strong sustain, bright. */
    P(SQUARE,   14,  160, 25000,  180,     0, 3200, 13000), /* 56 Trumpet         */
    P(SQUARE,   18,  180, 25000,  200,     0, 2600, 13000), /* 57 Trombone        */
    P(SQUARE,   22,  200, 25000,  240,     0, 2000, 13000), /* 58 Tuba            */
    P(SQUARE,   10,  140, 26000,  160,   400, 3600, 13000), /* 59 Muted Trumpet   */
    P(SAW,      30,  200, 26000,  260,     0, 2200, 16000), /* 60 French Horn     */
    P(SAW,      16,  170, 25000,  200,     0, 2800, 16000), /* 61 Brass Section   */
    P(SAW,      14,  160, 26000,  190,     0, 3000, 16000), /* 62 Synth Brass 1   */
    P(SAW,      20,  180, 26000,  220,     0, 2600, 16000), /* 63 Synth Brass 2   */

    /* 64-71 Reed -- narrow pulse for the double reeds, square for clarinet
     * (which really does suppress even harmonics), all with breath noise. */
    P(PULSE25,  20,  160, 26000,  180,   500, 2800, 15000), /* 64 Soprano Sax     */
    P(PULSE25,  22,  170, 26000,  200,   500, 2500, 15000), /* 65 Alto Sax        */
    P(PULSE25,  24,  180, 26000,  220,   500, 2200, 15000), /* 66 Tenor Sax       */
    P(PULSE25,  26,  190, 26000,  240,   500, 1900, 15000), /* 67 Baritone Sax    */
    P(PULSE25,  18,  150, 27000,  170,   400, 2600, 15000), /* 68 Oboe            */
    P(PULSE25,  20,  160, 27000,  180,   400, 2300, 15000), /* 69 English Horn    */
    P(PULSE25,  24,  180, 26000,  220,   400, 1700, 15000), /* 70 Bassoon         */
    P(SQUARE,   16,  150, 27000,  170,   400, 2400, 14000), /* 71 Clarinet        */

    /* 72-79 Pipe -- close to sine with prominent breath noise, which is most of
     * what identifies a flute when there is no sample to play. */
    P(SINE,     16,  140, 28000,  150,  1800,    0, 31000), /* 72 Piccolo         */
    P(SINE,     18,  150, 28000,  160,  2000,    0, 31000), /* 73 Flute           */
    P(SINE,     20,  160, 28000,  180,  1600,    0, 31000), /* 74 Recorder        */
    P(TRIANGLE, 18,  150, 28000,  170,  1400, 3000, 27000), /* 75 Pan Flute       */
    P(SINE,     22,  170, 27000,  200,  3000,    0, 31000), /* 76 Blown Bottle    */
    P(SINE,     20,  160, 27000,  190,  2600,    0, 31000), /* 77 Shakuhachi      */
    P(SINE,     14,  130, 28000,  150,  3400,    0, 31000), /* 78 Whistle         */
    P(SINE,     16,  140, 28000,  160,  1200,    0, 31000), /* 79 Ocarina         */

    /* 80-87 Synth lead -- these are the one family where the procedural backend
     * is not an approximation of anything. A square lead is a square lead. */
    P(SQUARE,    4,  120, 26000,  120,     0,    0, 14000), /* 80 Square Lead     */
    P(SAW,       4,  120, 26000,  120,     0,    0, 17000), /* 81 Saw Lead        */
    P(TRIANGLE,  8,  140, 27000,  140,   500, 4000, 27000), /* 82 Calliope Lead   */
    P(PULSE25,   3,  400,  8000,  160,     0, 3000, 15000), /* 83 Chiff Lead      */
    P(SAW,      10,  160, 26000,  180,     0, 2200, 17000), /* 84 Charang Lead    */
    P(SINE,     40,  200, 27000,  300,   600, 2000, 31000), /* 85 Voice Lead      */
    P(SAW,       6,  140, 26000,  140,     0, 3000, 17000), /* 86 Fifths Lead     */
    P(SAW,       8,  200, 22000,  200,     0, 1600, 17000), /* 87 Bass+Lead       */

    /* 88-95 Synth pad -- long attack, long release, dark. */
    P(SAW,     220,  400, 26000,  700,     0, 1800, 15000), /* 88 New Age Pad     */
    P(SAW,     260,  400, 26000,  800,     0, 1600, 15000), /* 89 Warm Pad        */
    P(SQUARE,  180,  360, 25000,  600,     0, 2000, 13000), /* 90 Polysynth Pad   */
    P(SINE,    300,  420, 26000,  900,   500, 1500, 31000), /* 91 Choir Pad       */
    P(SAW,     200,  380, 25000,  700,     0, 1700, 15000), /* 92 Bowed Pad       */
    P(TRIANGLE,240,  400, 26000,  800,     0, 1900, 26000), /* 93 Metallic Pad    */
    P(SINE,    280,  420, 26000,  850,   400, 1600, 31000), /* 94 Halo Pad        */
    P(SAW,     200,  380, 25000,  750,   600, 1500, 15000), /* 95 Sweep Pad       */

    /* 96-103 Synth effects */
    P(TRIANGLE, 20, 1200,     0,  400,  1200, 2600, 26000), /* 96 Rain            */
    P(SAW,     300,  600, 20000, 1000,   800, 1400, 15000), /* 97 Soundtrack      */
    P(SINE,      2,  900,     0,  300,  2400,    0, 30000), /* 98 Crystal         */
    P(SAW,     160,  400, 22000,  600,   600, 1800, 15000), /* 99 Atmosphere      */
    P(TRIANGLE, 40,  800,  8000,  400,  1000, 3000, 26000), /*100 Brightness      */
    P(SINE,    240,  500, 24000,  800,   700, 1500, 30000), /*101 Goblins         */
    P(SINE,     60,  400, 22000,  500,   500, 2200, 30000), /*102 Echoes          */
    P(SAW,     120,  400, 22000,  600,   900, 1700, 15000), /*103 Sci-Fi          */

    /* 104-111 Ethnic -- mostly plucked strings, so the guitar envelope. */
    P(SAW,       2,  800,     0,  240,  1100, 3200, 15000), /*104 Sitar           */
    P(TRIANGLE,  2,  600,     0,  180,   900, 3400, 27000), /*105 Banjo           */
    P(TRIANGLE,  2,  650,     0,  190,   700, 3000, 27000), /*106 Shamisen        */
    P(TRIANGLE,  2,  900,     0,  260,   500, 2800, 28000), /*107 Koto            */
    P(SINE,      1,  500,     0,  140,   600,    0, 30000), /*108 Kalimba         */
    P(PULSE25,  22,  180, 26000,  200,   800, 2200, 15000), /*109 Bagpipe         */
    P(SAW,      60,  220, 26000,  300,     0, 2400, 16000), /*110 Fiddle          */
    P(PULSE25,  24,  180, 26000,  220,   600, 2000, 15000), /*111 Shanai          */

    /* 112-119 Percussive -- struck, so short and noisy. */
    P(SINE,      1,  400,     0,  110,  1600,    0, 30000), /*112 Tinkle Bell     */
    P(TRIANGLE,  1,  300,     0,   90,  6000, 5000, 27000), /*113 Agogo           */
    P(TRIANGLE,  1,  350,     0,  100,  4000, 4000, 27000), /*114 Steel Drums     */
    P(TRIANGLE,  1,  180,     0,   60,  9000, 6000, 26000), /*115 Woodblock       */
    P(SINE,      1,  320,     0,   90,  7000, 1400, 30000), /*116 Taiko Drum      */
    P(SINE,      1,  280,     0,   80,  8000, 1800, 30000), /*117 Melodic Tom     */
    P(SQUARE,    1,  240,     0,   70,  6000, 2600, 13000), /*118 Synth Drum      */
    P(SINE,      1, 1400,     0,  500, 24000, 6000, 26000), /*119 Reverse Cymbal  */

    /* 120-127 Sound effects -- these are noise beds with an envelope. Nothing
     * here pretends to be a helicopter; they exist so a file that uses them
     * makes a plausible noise instead of an out-of-range table read. */
    P(SQUARE,    2,  300,     0,  100, 20000, 5000, 12000), /*120 Fret Noise      */
    P(SINE,     30,  400,     0,  150, 30000, 3000, 26000), /*121 Breath Noise    */
    P(SINE,    400, 1200,  8000,  800, 32000, 1200, 24000), /*122 Seashore        */
    P(SINE,     10,  260,     0,   90, 26000, 6000, 26000), /*123 Bird Tweet      */
    P(SQUARE,    4,  400, 14000,  120,  4000, 3000, 12000), /*124 Telephone Ring  */
    P(SINE,     60,  900,  6000,  300, 30000, 2000, 24000), /*125 Helicopter      */
    P(SINE,    200, 1400, 12000,  700, 32000, 2400, 22000), /*126 Applause        */
    P(SQUARE,    1,  700,     0,  200, 22000, 1500, 13000), /*127 Gunshot         */
};

/** @brief GM percussion, keys 35..81.
 *
 * Percussion ignores the note number as pitch and uses it as a voice selector,
 * so each entry carries the pitch to play at. Pitches are given as MIDI note
 * numbers because that is the unit the oscillator already speaks; the value is
 * chosen for the pitched component, and the noise blend does the rest.
 *
 * Keys outside 35..81 fall back to the closed hi-hat, which is the least
 * intrusive thing to hear when a file addresses a drum this table does not have.
 */
typedef struct { uint8_t pitch; m64_patch_t p; } m64_drum_t;

#define DRUM_LO 35
#define DRUM_HI 81

static const m64_drum_t gm_drums[DRUM_HI - DRUM_LO + 1] = {
    /* pitch,  shape, atk, dec, sus, rel, noise, cutoff, gain */
    { 31, P(SINE,     0, 190, 0,  60,  3000,  260, 32000) }, /* 35 Bass Drum 2    */
    { 33, P(SINE,     0, 170, 0,  55,  2600,  300, 32000) }, /* 36 Bass Drum 1    */
    { 66, P(SQUARE,   0,  60, 0,  25, 20000, 4200, 15000) }, /* 37 Side Stick     */
    { 50, P(TRIANGLE, 0, 200, 0,  70, 26000, 3600, 24000) }, /* 38 Acoustic Snare */
    { 58, P(TRIANGLE, 0, 170, 0,  60, 30000, 3000, 24000) }, /* 39 Hand Clap      */
    { 52, P(TRIANGLE, 0, 180, 0,  60, 28000, 4000, 24000) }, /* 40 Electric Snare */
    { 41, P(SINE,     0, 300, 0, 100,  5000,  900, 31000) }, /* 41 Low Floor Tom  */
    { 78, P(SQUARE,   0,  70, 0,  25, 32000, 9000, 11000) }, /* 42 Closed Hi-Hat  */
    { 45, P(SINE,     0, 290, 0,  95,  5000, 1000, 31000) }, /* 43 High Floor Tom */
    { 76, P(SQUARE,   0, 110, 0,  40, 32000, 8000, 11000) }, /* 44 Pedal Hi-Hat   */
    { 48, P(SINE,     0, 280, 0,  90,  5000, 1200, 31000) }, /* 45 Low Tom        */
    { 78, P(SQUARE,   0, 420, 0, 160, 32000, 8500, 10000) }, /* 46 Open Hi-Hat    */
    { 52, P(SINE,     0, 260, 0,  85,  5000, 1400, 31000) }, /* 47 Low-Mid Tom    */
    { 56, P(SINE,     0, 240, 0,  80,  5000, 1600, 31000) }, /* 48 Hi-Mid Tom     */
    { 84, P(SQUARE,   0,1100, 0, 420, 32000, 7000,  9000) }, /* 49 Crash Cymbal 1 */
    { 60, P(SINE,     0, 220, 0,  75,  5000, 1800, 31000) }, /* 50 High Tom       */
    { 82, P(SQUARE,   0, 700, 0, 260, 32000, 9500,  9000) }, /* 51 Ride Cymbal 1  */
    { 80, P(SQUARE,   0,1000, 0, 380, 32000, 6500,  9000) }, /* 52 Chinese Cymbal */
    { 88, P(TRIANGLE, 0, 500, 0, 190, 14000, 9000, 18000) }, /* 53 Ride Bell      */
    { 84, P(SQUARE,   0, 180, 0,  70, 32000,10000, 12000) }, /* 54 Tambourine     */
    { 86, P(SQUARE,   0, 800, 0, 300, 32000, 7500,  9000) }, /* 55 Splash Cymbal  */
    { 86, P(SQUARE,   0, 200, 0,  70,  6000, 5000, 14000) }, /* 56 Cowbell        */
    { 82, P(SQUARE,   0,1200, 0, 460, 32000, 6800,  9000) }, /* 57 Crash Cymbal 2 */
    { 70, P(SQUARE,   0, 320, 0, 120, 30000, 4000, 12000) }, /* 58 Vibraslap      */
    { 82, P(SQUARE,   0, 650, 0, 240, 32000, 9200,  9000) }, /* 59 Ride Cymbal 2  */
    { 65, P(SINE,     0, 170, 0,  60,  8000, 2600, 30000) }, /* 60 Hi Bongo       */
    { 58, P(SINE,     0, 190, 0,  65,  8000, 2200, 30000) }, /* 61 Low Bongo      */
    { 62, P(SINE,     0, 120, 0,  45, 10000, 2400, 30000) }, /* 62 Mute Hi Conga  */
    { 60, P(SINE,     0, 200, 0,  70,  8000, 2000, 30000) }, /* 63 Open Hi Conga  */
    { 53, P(SINE,     0, 230, 0,  80,  8000, 1600, 30000) }, /* 64 Low Conga      */
    { 68, P(TRIANGLE, 0, 190, 0,  65, 12000, 3400, 25000) }, /* 65 High Timbale   */
    { 62, P(TRIANGLE, 0, 210, 0,  75, 12000, 2800, 25000) }, /* 66 Low Timbale    */
    { 84, P(TRIANGLE, 0, 130, 0,  45,  6000, 5200, 26000) }, /* 67 High Agogo     */
    { 79, P(TRIANGLE, 0, 140, 0,  50,  6000, 4600, 26000) }, /* 68 Low Agogo      */
    { 88, P(SQUARE,   0, 150, 0,  55, 32000,11000, 11000) }, /* 69 Cabasa         */
    { 88, P(SQUARE,   0, 130, 0,  45, 32000,12000, 11000) }, /* 70 Maracas        */
    { 96, P(SINE,     0, 260, 0,  90, 16000, 6000, 24000) }, /* 71 Short Whistle  */
    { 96, P(SINE,     0, 520, 0, 190, 16000, 6000, 24000) }, /* 72 Long Whistle   */
    { 84, P(SQUARE,   0, 190, 0,  70, 32000, 8000, 12000) }, /* 73 Short Guiro    */
    { 84, P(SQUARE,   0, 480, 0, 180, 32000, 7000, 12000) }, /* 74 Long Guiro     */
    { 88, P(TRIANGLE, 0,  90, 0,  30, 14000, 7000, 24000) }, /* 75 Claves         */
    { 82, P(TRIANGLE, 0, 110, 0,  40, 10000, 6000, 25000) }, /* 76 Hi Wood Block  */
    { 76, P(TRIANGLE, 0, 130, 0,  45, 10000, 5000, 25000) }, /* 77 Low Wood Block */
    { 74, P(SQUARE,   0, 140, 0,  50, 26000, 4400, 14000) }, /* 78 Mute Cuica     */
    { 68, P(SQUARE,   0, 300, 0, 110, 22000, 3400, 14000) }, /* 79 Open Cuica     */
    { 92, P(SINE,     0, 200, 0,  70,  4000, 8000, 28000) }, /* 80 Mute Triangle  */
    { 92, P(SINE,     0, 900, 0, 340,  4000, 8000, 28000) }, /* 81 Open Triangle  */
};

const m64_patch_t *m64_patch_for_program(int program) {
    if (program < 0 || program > 127) program = 0;
    return &gm_patches[program];
}

const m64_patch_t *m64_patch_for_drum(int key, int32_t *pitch_q8_out) {
    /* Key 42 (closed hi-hat) is the fallback: it is short, quiet and present in
     * essentially every kit, so an unmapped key is inaudible rather than wrong. */
    if (key < DRUM_LO || key > DRUM_HI) key = 42;
    const m64_drum_t *d = &gm_drums[key - DRUM_LO];
    if (pitch_q8_out) *pitch_q8_out = (int32_t)d->pitch << 8;
    return &d->p;
}
