/**
 * @file screens.h
 * @brief Screen registry.
 * @ingroup screens
 */

#ifndef SCREENS_H__
#define SCREENS_H__

#include "app.h"

extern const screen_t SCREEN_GRID_DEF;
extern const screen_t SCREEN_DETAIL_DEF;
extern const screen_t SCREEN_CHEATS_DEF;
extern const screen_t SCREEN_SETTINGS_DEF;
extern const screen_t SCREEN_LAUNCH_DEF;
extern const screen_t SCREEN_FAULT_DEF;

/** @brief Populate @p table, indexed by screen_id_t. */
void screens_register (const screen_t **table);

#endif /* SCREENS_H__ */
