/**
 * @file fonts.h
 * @brief Menu fonts
 * @ingroup menu 
 */

#ifndef FONTS_H__
#define FONTS_H__

#include <stdint.h>

/**
 * @brief Font type enumeration.
 * 
 * This enumeration defines the different types of fonts that can be used
 * in the menu system.
 */
typedef enum {
    FNT_DEFAULT = 1, /**< Body text, 15 px */
    FNT_BOOT = 2,    /**< 32 px, boot plate only; 41-glyph charset, see the Makefile */
} menu_font_type_t;

/**
 * @brief Font style enumeration.
 * 
 * This enumeration defines the different styles of fonts that can be used
 * in the menu system.
 */
typedef enum {
    STL_DEFAULT = 0, /**< Body text. Follows the theme -- see fonts_set_palette(). */
    STL_GREEN,       /**< Green font style */
    STL_BLUE,        /**< Blue font style */
    STL_YELLOW,      /**< Accent. Follows the theme. */
    STL_ORANGE,      /**< Accent. Follows the theme. */
    STL_RED,         /**< Red font style */
    STL_GRAY,        /**< Secondary text. Follows the theme. */
    STL_ONBTN,       /**< Always white: the glyph inside a controller-colour disc, which is not
                      *   a theme surface. Binding this to the theme's text colour turned the
                      *   letters black on the blue A button under a light theme. */
} menu_font_style_t;

/**
 * @brief Initialize fonts.
 *
 * This function initializes the fonts used in the menu system. It can load
 * custom fonts from the specified path.
 *
 * @param custom_font_path Path to the custom font file.
 */
void fonts_init(char *custom_font_path);

/**
 * @brief Point the text styles at a theme's colours. Call once at boot and on every change.
 *
 * The styles used to be registered once, at load, in fixed colours -- STL_DEFAULT white and
 * STL_GRAY 0xA0A0A0. That is invisible on a light theme: under `cartridge`, whose panels are
 * 0xF7F7EF, every label on every screen was white on near-white. Colours are packed RGBA5551,
 * the same words the theme holds.
 */
void fonts_set_palette(uint16_t text, uint16_t dim, uint16_t accent);

#endif /* FONTS_H__ */
