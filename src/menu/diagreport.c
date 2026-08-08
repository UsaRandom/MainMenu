/**
 * @file diagreport.c
 * @brief See diagreport.h for the five reporting channels this replaces.
 * @ingroup menu
 */

#include <stdarg.h>
#include <stdio.h>

#include "diagreport.h"
#include "launchlog.h"

static char lines[DIAG_REPORT_MAX][72];
static int  used;

void diag_reportf (const char *fmt, ...) {
    if (used >= DIAG_REPORT_MAX) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lines[used], sizeof(lines[0]), fmt, ap);
    va_end(ap);
    used++;

    /* And to the card, if the card is listening. It has not been, twice, which is why this
     * function's real output is the screen -- but a line that reaches both is free. */
    launchlog_line("%s", lines[used - 1]);
}

int diag_report_count (void) {
    return used;
}

const char *diag_report_line (int i) {
    return (i >= 0 && i < used) ? lines[i] : "";
}
