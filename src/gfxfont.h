/**
 * @file gfxfont.h
 * @brief Adafruit GFX / TFT_eSPI compatible font structures.
 *
 * This header defines the GFXfont and GFXglyph structures that are identical
 * to those used by Adafruit_GFX and TFT_eSPI.  Fonts in that format can be
 * used directly with epd_draw_gfxfont_string() and friends declared in
 * font_gfx.h.
 *
 * On AVR (Arduino) these structures use PROGMEM.  On ESP32/ESP32-S3 PROGMEM
 * is a no-op, so pgm_read_byte / pgm_read_word just dereference the pointer.
 */

#pragma once

#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#endif

#ifndef pgm_read_word
#define pgm_read_word(addr) (*(const uint16_t *)(addr))
#endif

#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#endif

/** Per-character glyph metrics, compatible with Adafruit GFX. */
typedef struct {
    uint16_t bitmapOffset; /**< Byte offset into the font bitmap array. */
    uint8_t  width;        /**< Glyph bitmap width in pixels. */
    uint8_t  height;       /**< Glyph bitmap height in pixels. */
    uint8_t  xAdvance;     /**< Horizontal advance (cursor step) in pixels. */
    int8_t   xOffset;      /**< Horizontal draw offset from cursor position. */
    int8_t   yOffset;      /**< Vertical draw offset from cursor baseline. */
} GFXglyph;

/** Font descriptor, compatible with Adafruit GFX. */
typedef struct {
    uint8_t  *bitmap;   /**< Glyph bitmaps (1 bit per pixel, packed). */
    GFXglyph *glyph;    /**< Per-character metrics array. */
    uint16_t  first;    /**< First Unicode code-point in glyph array. */
    uint16_t  last;     /**< Last Unicode code-point in glyph array. */
    uint8_t   yAdvance; /**< Newline distance (vertical advance) in pixels. */
} GFXfont;
