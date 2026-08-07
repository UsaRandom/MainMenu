/**
 * @file cheats.h
 * @brief Header file for cheat installation functions.
 * @ingroup boot
 */

#ifndef CHEATS_H__
#define CHEATS_H__

#include <stdint.h>
#include "cic.h"

/**
 * @brief Word offset into the IPL3 that the engine hooks itself into, or -1 if unsupported.
 *
 * Exposed so the menu can answer "will cheats actually work for this game?" while the filesystem
 * and the display still exist. The install itself runs inside boot(), after everything has been
 * torn down and microseconds before jumping into the game, where there is nothing left to report
 * a failure to. See src/menu/cheatcheck.h.
 */
int cheats_ipl3_patch_offset (cic_type_t cic_type);

/**
 * @brief Is @p word_at_offset the instruction the engine expects to overwrite?
 *
 * @p word_at_offset is the IPL3 word at #cheats_ipl3_patch_offset. Handles the x106 descrambling.
 * False means the engine cannot hook this ROM and cheats will not run.
 */
bool cheats_ipl3_layout_ok (cic_type_t cic_type, uint32_t word_at_offset);

/**
 * @brief Assemble the patcher and engine into RDRAM without touching the IPL3.
 *
 * Everything cheats_install() does except the one-word IPL3 patch that arms it. Split out so the
 * dev harness can execute the emitted patcher against a synthetic game image and prove the
 * preamble hook end-to-end under ares -- the IPL3 patch is the single step that cannot happen
 * inside the menu, because the menu's own IPL3 is libdragon's and the layout check refuses it.
 *
 * @param cheat_list The cheat list, terminated by a zero pair.
 * @return The patcher's entry point (jump here with $t1 holding the game entry point), or NULL
 *         if the list was NULL or would overflow the engine or patcher regions.
 */
uint32_t *cheats_emit(uint32_t *cheat_list);

/**
 * @brief Installs cheats based on the CIC type.
 *
 * This function installs the cheats provided in the cheat list based on the
 * specified CIC type.
 *
 * @param cic_type The type of CIC (Copy Protection Chip) used.
 * @param cheat_list A pointer to an array of cheats to be installed.
 * @return true if the cheats were successfully installed, false otherwise.
 */
bool cheats_install(cic_type_t cic_type, uint32_t *cheat_list);

#endif // CHEATS_H__
