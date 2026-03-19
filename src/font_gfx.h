/**
 * @file font_gfx.h
 * @brief Render Adafruit GFX / TFT_eSPI format fonts into epdiy framebuffers.
 *
 * All functions write into a 4-bit-per-pixel epdiy framebuffer.  Colors follow
 * the epdiy convention: 0x00 = black, 0xF0 = white (upper nibble is the actual
 * level).
 */

#pragma once

#include "gfxfont.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Draw a single character from a GFXfont into the framebuffer.
 *
 * @param x        Cursor X position (left of the character).
 * @param y        Cursor Y position (baseline).
 * @param c        Unicode code point (only BMP range supported).
 * @param font     Pointer to the GFXfont descriptor.
 * @param color    4-bit epdiy color (e.g. 0x00 = black, 0xF0 = white).
 * @param fb_width Framebuffer width in pixels.
 * @param fb       Pointer to the 4-bit-per-pixel framebuffer.
 * @return         Horizontal advance consumed (xAdvance of the glyph), or 0
 *                 if the code point is not in the font's range.
 */
int epd_draw_gfxfont_char(
    int x, int y, uint16_t c,
    const GFXfont* font,
    uint8_t color,
    int fb_width,
    uint8_t* fb
);

/**
 * Draw a null-terminated ASCII/Latin-1 string using a GFXfont.
 *
 * Advances *cursor_x by the total width of the rendered string.
 *
 * @param font     GFXfont descriptor.
 * @param str      Null-terminated string to render.
 * @param cursor_x Pointer to the X cursor (updated on return).
 * @param cursor_y Pointer to the Y cursor (baseline, updated on newline).
 * @param color    4-bit epdiy color.
 * @param fb_width Framebuffer width in pixels.
 * @param fb       Framebuffer pointer.
 */
void epd_draw_gfxfont_string(
    const GFXfont* font,
    const char* str,
    int* cursor_x,
    int* cursor_y,
    uint8_t color,
    int fb_width,
    uint8_t* fb
);

/**
 * Measure the bounding box of a string rendered with a GFXfont without
 * drawing anything.
 *
 * @param font   GFXfont descriptor.
 * @param str    Null-terminated string.
 * @param x      Starting X cursor.
 * @param y      Starting Y cursor (baseline).
 * @param x1     Output: left edge of bounding box.
 * @param y1     Output: top edge of bounding box.
 * @param w      Output: bounding box width.
 * @param h      Output: bounding box height.
 */
void epd_gfxfont_text_bounds(
    const GFXfont* font,
    const char* str,
    int x, int y,
    int* x1, int* y1,
    int* w,  int* h
);

/**
 * Return the line height (yAdvance) of a GFXfont in pixels.
 */
int epd_gfxfont_line_height(const GFXfont* font);

/* -------------------------------------------------------------------------
 * Classic 5×7 column-major font (glcd_bitmap from fonts/glcdfont.h)
 * ------------------------------------------------------------------------- */

/**
 * Draw a single character from the classic column-major 5×7 font.
 *
 * @param x        Top-left pixel X.
 * @param y        Top-left pixel Y.
 * @param c        ASCII code point (0x20–0x7E).
 * @param scale    Integer scale factor (1 = 6×8 cell, 2 = 12×16, ...).
 * @param color    4-bit epdiy color (e.g. 0x00 = black, 0xF0 = white).
 * @param fb_width Framebuffer width in pixels.
 * @param fb       Framebuffer pointer.
 * @return         Advance in pixels (scale * 6).
 */
int epd_draw_glcd_char(
    int x, int y, uint8_t c,
    int scale,
    uint8_t color,
    int fb_width,
    uint8_t* fb
);

/**
 * Draw a null-terminated ASCII string using the classic 5×7 column-major font.
 */
void epd_draw_glcd_string(
    const char* str,
    int* cursor_x,
    int* cursor_y,
    int scale,
    uint8_t color,
    int fb_width,
    uint8_t* fb
);

/**
 * Measure the bounding box of a string rendered with the classic 5×7 font.
 */
void epd_glcd_text_bounds(
    const char* str,
    int x, int y,
    int scale,
    int* x1, int* y1,
    int* w,  int* h
);

#ifdef __cplusplus
}
#endif
