/**
 * @file enginetest.c
 * @brief See enginetest.h for why the menu has to be its own test subject.
 * @ingroup menu
 */

#include <stdio.h>

#include "menu/enginetest.h"

/**
 * @brief The probe.
 *
 * `volatile` because the whole point is that it changes behind the compiler's back -- the writer
 * is an exception handler assembled at runtime into a region this program knows nothing about,
 * so every static analysis says this variable is written nowhere and read pointlessly. Without
 * it, `-flto` folds `probe == ENGINETEST_VALUE` to false and the self-test reports "not running"
 * on a console where the engine is running perfectly.
 *
 * Initialised to a non-zero value rather than left in `.bss`. Zero is what uninitialised memory
 * and a cleared page both look like, and the one thing this file must never do is mistake either
 * for a working cheat engine. 0x0000 would also be indistinguishable from the halfword the
 * engine writes if a user mistyped the value as 0000.
 */
volatile uint16_t enginetest_probe = 0x5A5A;

/** Latched, never re-read. See enginetest_seen(). */
static bool seen;

uint32_t enginetest_address (void) {
    return (uint32_t)(uintptr_t)&enginetest_probe;
}

void enginetest_code (char *out, size_t cap) {
    /* Type 0x81: a 16-bit constant write, applied on every pass of the engine. The address is
     * printed whole, including the type byte, because that is what gets typed in -- the editor
     * takes a Datel line, not an address and a width.
     *
     * The engine masks what it is given with 0xA07FFFFF, which clears the segment bits. Any KSEG0
     * address in the low 8 MB survives that unchanged, and every address this variable can have
     * is one. */
    snprintf(out, cap, "%08lX %04X",
             (unsigned long)((enginetest_address() & 0x00FFFFFFu) | 0x81000000u),
             ENGINETEST_VALUE);
}

bool enginetest_seen (void) {
    return seen;
}

void enginetest_poll (void) {
    if (!seen && enginetest_probe == ENGINETEST_VALUE) {
        seen = true;
    }
}
