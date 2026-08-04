/**
 * @file inputscript.h
 * @brief Deterministic joypad replay for headless regression runs.
 * @ingroup dev
 *
 * ares has no input-replay facility, so the script lives in the ROM. It is compiled from a
 * plain-text file by tools/mkinput.py into $(BUILD_DIR)/inputscript_generated.h.
 *
 * Events are keyed on FRAME NUMBER, never on elapsed time. ares runs at whatever speed the
 * host allows -- a shader compile stall alone costs tens of milliseconds -- so a time-based
 * script would visit different UI states on different runs and the framebuffer hashes would
 * never converge. Frame counting makes a run byte-reproducible, which is the whole point.
 *
 * Compiles to nothing without DEV_HARNESS.
 */

#ifndef INPUTSCRIPT_H__
#define INPUTSCRIPT_H__

#include <stdbool.h>
#include <stdint.h>
#include <libdragon.h>

/** @brief Buttons a script can press. Edge-triggered: reported only on an event's first frame. */
typedef enum {
    ISCRIPT_BTN_A        = (1 << 0),
    ISCRIPT_BTN_B        = (1 << 1),
    ISCRIPT_BTN_START    = (1 << 2),
    ISCRIPT_BTN_L        = (1 << 3),
    ISCRIPT_BTN_R        = (1 << 4),
    ISCRIPT_BTN_Z        = (1 << 5),
    ISCRIPT_BTN_CRIGHT   = (1 << 6),   /**< the Fav button; see input.c on why C-right is not a
                                        *   direction */
} iscript_button_t;

/** @brief Side effects a script step can trigger. */
typedef enum {
    ISCRIPT_ACT_NONE       = 0,
    ISCRIPT_ACT_FBDUMP     = 1, /**< dump the frame just rendered */
    ISCRIPT_ACT_EXIT       = 2, /**< ask ares to quit, ending the run without a timeout */
    ISCRIPT_ACT_RECORD_ON  = 3, /**< start dumping every frame; see inputscript_recording() */
    ISCRIPT_ACT_RECORD_OFF = 4, /**< stop */
} iscript_action_t;

/** @brief One scripted step, held for @c hold_frames starting at @c at_frame. */
typedef struct {
    uint16_t at_frame;
    uint16_t hold_frames;
    uint8_t  dir;      /**< joypad_8way_t for D-pad/stick, or 0xFF for none */
    uint8_t  cdir;     /**< joypad_8way_t for the C-pad (drives go_fast), or 0xFF */
    uint8_t  buttons;  /**< iscript_button_t bitmask */
    uint8_t  action;   /**< iscript_action_t */
} input_event_t;

#ifdef DEV_HARNESS

/** @brief True when a script was compiled in and has not finished. */
bool inputscript_active (void);

/** @brief Advance one frame. Call once per main-loop iteration, before reading input. */
void inputscript_tick (void);

/** @brief Direction the script is holding. @p c_pad selects the C-pad channel. */
joypad_8way_t inputscript_direction (bool c_pad);

/** @brief True on the single frame a button transitions to pressed. */
bool inputscript_pressed (iscript_button_t button);

/** @brief Action due after this frame is rendered, then cleared. */
iscript_action_t inputscript_take_action (void);

/**
 * @brief True while `record on` is in force, so every frame should be dumped.
 *
 * Separate from the one-shot actions because recording has to survive events that carry their
 * own direction and buttons. Modelling it as a per-frame FBDUMP action instead would mean one
 * event per recorded frame, and an event holds a direction -- so a recorded frame could never
 * also be a frame the script was pressing something on, and the video would stutter over every
 * button press.
 */
bool inputscript_recording (void);

#else

#define inputscript_active()          (false)
#define inputscript_tick()            ((void)0)
#define inputscript_direction(c)      (JOYPAD_8WAY_NONE)
#define inputscript_pressed(b)        (false)
#define inputscript_take_action()     (ISCRIPT_ACT_NONE)
#define inputscript_recording()       (false)

#endif /* DEV_HARNESS */

#endif /* INPUTSCRIPT_H__ */
