/* Host-test shim. boxart.c asks library.c for one string per system -- the tab label, lowercased
 * into the key it looks for in boxart.ini -- and that is the only thing it wants from a file that
 * drags in the scanner, the ROM database and the filesystem. Kept in step by construction: the
 * table below is asserted against SYS_COUNT, so adding a system without adding its name here is a
 * compile error rather than a lookup that quietly returns "?" and reads no override. */
#include "library/library.h"

static const char *LABELS[SYS_COUNT] = { "N64", "NES", "SNES", "GB", "GBC", "SMS" };

_Static_assert(sizeof(LABELS) / sizeof(LABELS[0]) == SYS_COUNT,
               "the shim's system names are out of step with system_t");

const char *library_tab_label (tab_t tab) {
    int sys = (int)tab - (int)TAB_N64;
    return (sys >= 0 && sys < SYS_COUNT) ? LABELS[sys] : "?";
}
