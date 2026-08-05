/**
 * @file cheatcheck.c
 * @brief The cheat-engine pre-flight. See cheatcheck.h for why it is not in boot/cheats.c.
 * @ingroup menu
 */

#include <stdio.h>
#include <string.h>
#include <libdragon.h>

#include "boot/cheats.h"
#include "boot/cic.h"
#include "menu/cheatcheck.h"

/** IPL3 is words 16..1023 of the ROM. boot.c copies exactly that range into DMEM index for
 *  index, so a DMEM word offset is also a ROM word offset and no adjustment is needed here. */
#define ROM_HEAD_WORDS  1024
#define IPL3_BYTE_OFF   0x40

static const char *cic_name (cic_type_t t) {
    switch (t) {
        case CIC_5101: return "5101";
        case CIC_6101: return "6101";
        case CIC_7102: return "7102";
        case CIC_x102: return "6102/7101";
        case CIC_x103: return "6103/7103";
        case CIC_x105: return "6105/7105";
        case CIC_x106: return "6106/7106";
        case CIC_5167: return "5167";
        case CIC_8301: return "8301";
        case CIC_8302: return "8302";
        case CIC_8303: return "8303";
        case CIC_8401: return "8401";
        case CIC_8501: return "8501";
        default:       return "unknown";
    }
}

cheatfit_t cheatcheck_rom (const char *rom_path, char *detail, size_t cap) {
    if (detail != NULL && cap > 0) {
        detail[0] = '\0';
    }
    if (rom_path == NULL) {
        return CHEATFIT_UNREADABLE;
    }

    /* 4 KB on the stack would be rude in a menu that has already been careful about this, but the
     * alternative is a heap allocation on the launch path for something read once and discarded.
     * Static: this runs from one place, never re-entrantly, and never after boot() starts. */
    static uint32_t head[ROM_HEAD_WORDS];

    FILE *f = fopen(rom_path, "rb");
    if (f == NULL) {
        return CHEATFIT_UNREADABLE;
    }
    size_t got = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (got < sizeof(head)) {
        return CHEATFIT_UNREADABLE;
    }

    /* The real detector, not a copy of it. If this ever disagreed with the console about which
     * CIC a ROM has, it would be worse than having no check at all. */
    cic_type_t cic = cic_detect((uint8_t *)head + IPL3_BYTE_OFF);
    int offset = cheats_ipl3_patch_offset(cic);

    if (offset < 0) {
        if (detail != NULL) {
            snprintf(detail, cap, "CIC %s has no patch offset", cic_name(cic));
        }
        return CHEATFIT_UNKNOWN_CIC;
    }

    uint32_t word = head[offset];
    bool ok = cheats_ipl3_layout_ok(cic, word);

    if (detail != NULL) {
        snprintf(detail, cap, "CIC %s word[%d]=%08lx %s",
                 cic_name(cic), offset, (unsigned long)word, ok ? "ok" : "UNEXPECTED");
    }
    return ok ? CHEATFIT_OK : CHEATFIT_BAD_LAYOUT;
}

const char *cheatcheck_message (cheatfit_t fit) {
    switch (fit) {
        /* Nothing at all on the happy path. A line saying cheats will work is noise on every
         * launch of every game; the only thing worth interrupting someone for is the case where
         * they have ticked cheats that are not going to run. */
        case CHEATFIT_OK:           return NULL;
        case CHEATFIT_UNREADABLE:   return NULL;
        case CHEATFIT_UNKNOWN_CIC:  return "Cheats cannot run on this game.";
        case CHEATFIT_BAD_LAYOUT:   return "Cheats cannot run on this copy of the game.";
        default:                    return NULL;
    }
}
