#include "font_gfx.h"
#include "gfxfont.h"
#include "fonts/glcdfont.h"
#include "epdiy.h"

#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helper: set a single 4-bit pixel in the epdiy framebuffer.
 * Delegates to epd_draw_pixel so that:
 *   1. The current display rotation is applied.
 *   2. The nibble-packing convention matches the rest of epdiy
 *      (lower nibble = even x, upper nibble = odd x).
 * fb_width is kept as a parameter for API compatibility but is unused here
 * (epd_draw_pixel uses epd_width() internally).
 * color: epdiy convention — only the upper nibble [7:4] carries the value.
 * ------------------------------------------------------------------------- */
static inline void set_pixel(int x, int y, uint8_t color, int fb_width, uint8_t* fb) {
    (void)fb_width;
    epd_draw_pixel(x, y, color, fb);
}

/* -------------------------------------------------------------------------
 * epd_draw_gfxfont_char
 * ------------------------------------------------------------------------- */
int epd_draw_gfxfont_char(
    int x, int y, uint16_t c,
    const GFXfont* font,
    uint8_t color,
    int fb_width,
    uint8_t* fb
) {
    if (font == NULL || fb == NULL)
        return 0;

    if (c < pgm_read_word(&font->first) || c > pgm_read_word(&font->last))
        return 0;

    uint16_t idx = c - pgm_read_word(&font->first);
    const GFXglyph* glyph_ptr = &(((const GFXglyph*)pgm_read_dword(&font->glyph))[idx]);

    uint8_t  w       = pgm_read_byte(&glyph_ptr->width);
    uint8_t  h       = pgm_read_byte(&glyph_ptr->height);
    int8_t   xo      = (int8_t)pgm_read_byte(&glyph_ptr->xOffset);
    int8_t   yo      = (int8_t)pgm_read_byte(&glyph_ptr->yOffset);
    uint8_t  xa      = pgm_read_byte(&glyph_ptr->xAdvance);
    uint16_t bo      = pgm_read_word(&glyph_ptr->bitmapOffset);

    const uint8_t* bitmap = (const uint8_t*)pgm_read_dword(&font->bitmap);

    /* Draw the 1-bpp bitmap into the framebuffer. */
    uint8_t bits = 0, bit = 0;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            if (bit == 0) {
                bits = pgm_read_byte(&bitmap[bo++]);
                bit  = 0x80;
            }
            if (bits & bit) {
                set_pixel(x + xo + col, y + yo + row, color, fb_width, fb);
            }
            bit >>= 1;
        }
    }

    return (int)xa;
}

/* -------------------------------------------------------------------------
 * epd_draw_gfxfont_string
 * ------------------------------------------------------------------------- */
void epd_draw_gfxfont_string(
    const GFXfont* font,
    const char* str,
    int* cursor_x,
    int* cursor_y,
    uint8_t color,
    int fb_width,
    uint8_t* fb
) {
    if (font == NULL || str == NULL)
        return;

    uint8_t ya = pgm_read_byte(&font->yAdvance);

    while (*str) {
        uint8_t c = (uint8_t)*str++;
        if (c == '\n') {
            *cursor_x  = 0;
            *cursor_y += (int)ya;
        } else if (c != '\r') {
            int advance = epd_draw_gfxfont_char(
                *cursor_x, *cursor_y, (uint16_t)c,
                font, color, fb_width, fb
            );
            *cursor_x += advance;
        }
    }
}

/* -------------------------------------------------------------------------
 * epd_gfxfont_text_bounds
 * ------------------------------------------------------------------------- */
void epd_gfxfont_text_bounds(
    const GFXfont* font,
    const char* str,
    int x, int y,
    int* x1, int* y1,
    int* w, int* h
) {
    if (font == NULL || str == NULL) {
        *x1 = x; *y1 = y; *w = 0; *h = 0;
        return;
    }

    int cx = x, cy = y;
    int min_x = x, min_y = y, max_x = x, max_y = y;
    uint8_t ya = pgm_read_byte(&font->yAdvance);

    while (*str) {
        uint8_t c = (uint8_t)*str++;
        if (c == '\n') {
            cx  = x;
            cy += (int)ya;
            continue;
        }
        if (c == '\r')
            continue;

        uint16_t first = pgm_read_word(&font->first);
        uint16_t last  = pgm_read_word(&font->last);
        if (c < first || c > last)
            continue;

        uint16_t idx = c - first;
        const GFXglyph* gp = &(((const GFXglyph*)pgm_read_dword(&font->glyph))[idx]);

        uint8_t gw = pgm_read_byte(&gp->width);
        uint8_t gh = pgm_read_byte(&gp->height);
        int8_t  xo = (int8_t)pgm_read_byte(&gp->xOffset);
        int8_t  yo = (int8_t)pgm_read_byte(&gp->yOffset);
        uint8_t xa = pgm_read_byte(&gp->xAdvance);

        int gx0 = cx + xo;
        int gy0 = cy + yo;
        int gx1 = gx0 + (int)gw;
        int gy1 = gy0 + (int)gh;

        if (gx0 < min_x) min_x = gx0;
        if (gy0 < min_y) min_y = gy0;
        if (gx1 > max_x) max_x = gx1;
        if (gy1 > max_y) max_y = gy1;

        cx += (int)xa;
    }

    *x1 = min_x;
    *y1 = min_y;
    *w  = max_x - min_x;
    *h  = max_y - min_y;
}

/* -------------------------------------------------------------------------
 * epd_gfxfont_line_height
 * ------------------------------------------------------------------------- */
int epd_gfxfont_line_height(const GFXfont* font) {
    if (font == NULL)
        return 0;
    return (int)pgm_read_byte(&font->yAdvance);
}

/* =========================================================================
 * Classic 5×7 column-major font renderer
 * Column-major: each byte = one vertical column, LSB = top pixel.
 * Cell size: 6×8 (5×7 glyph + 1 px right spacing + 1 px bottom spacing).
 * ========================================================================= */

int epd_draw_glcd_char(
    int x, int y, uint8_t c,
    int scale,
    uint8_t color,
    int fb_width,
    uint8_t* fb
) {
    if (fb == NULL || scale < 1)
        return 0;
    if (c < GLCD_FIRST_CHAR || c > GLCD_LAST_CHAR)
        return scale * 6;

    int idx = (c - GLCD_FIRST_CHAR) * GLCD_BYTES_PER_CHAR;

    for (int col = 0; col < 5; col++) {
        uint8_t col_data = pgm_read_byte(&glcd_bitmap[idx + col]);
        for (int row = 0; row < 7; row++) {
            if (col_data & (1 << row)) {
                /* Scale: draw a scale×scale block for each pixel. */
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        set_pixel(
                            x + col * scale + sx,
                            y + row * scale + sy,
                            color, fb_width, fb
                        );
                    }
                }
            }
        }
    }
    return scale * 6;
}

void epd_draw_glcd_string(
    const char* str,
    int* cursor_x,
    int* cursor_y,
    int scale,
    uint8_t color,
    int fb_width,
    uint8_t* fb
) {
    if (str == NULL || scale < 1)
        return;

    while (*str) {
        uint8_t c = (uint8_t)*str++;
        if (c == '\n') {
            *cursor_x  = 0;
            *cursor_y += scale * 8;
        } else if (c != '\r') {
            *cursor_x += epd_draw_glcd_char(
                *cursor_x, *cursor_y, c, scale, color, fb_width, fb
            );
        }
    }
}

void epd_glcd_text_bounds(
    const char* str,
    int x, int y,
    int scale,
    int* x1, int* y1,
    int* w, int* h
) {
    if (str == NULL || scale < 1) {
        *x1 = x; *y1 = y; *w = 0; *h = 0;
        return;
    }

    int cx = x;
    int total_w = 0;
    int lines   = 1;
    int line_w  = 0;

    while (*str) {
        uint8_t c = (uint8_t)*str++;
        if (c == '\n') {
            lines++;
            if (line_w > total_w) total_w = line_w;
            line_w = 0;
        } else if (c != '\r') {
            line_w += scale * 6;
        }
    }
    if (line_w > total_w) total_w = line_w;

    *x1 = x;
    *y1 = y;
    *w  = total_w;
    *h  = lines * scale * 8;
    (void)cx;
}
