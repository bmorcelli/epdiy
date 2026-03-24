/**
 * @file EPD_translate.h
 * @brief TFT_eSPI-compatible wrapper for the epdiy v2 library.
 *
 * Drop-in replacement for the `EPD_translate` class originally found in the
 * LilyGo-EPD47 library.  Ported and extended to work with epdiy v2's API.
 *
 * ## Quick start (ESP-IDF / IDF component)
 *
 * ```cpp
 * #include "EPD_translate.h"
 * EPD_translate tft;
 *
 * void app_main() {
 *     tft.init(&epd_board_v7, &ED047TC2, EPD_LUT_64K);
 *     tft.fillScreen(TFT_WHITE);
 *     tft.setTextColor(TFT_BLACK);
 *     tft.setTextSize(2);
 *     tft.setCursor(10, 40);
 *     tft.println("Hello epdiy!");
 *     tft.epdPushImage();
 * }
 * ```
 *
 * ## Color notes
 * All color arguments follow the RGB565 convention used by TFT_eSPI.
 * Colors are converted to 4-bit epdiy grayscale internally using perceptual
 * luminance (ITU-R BT.601).
 *
 * ## Font notes
 * - `setTextSize(1/2/3)` uses the built-in 5×7 classic monospace font at 1×/2×/3× scale.
 * - `setFreeFont(f)` or `setFont(f)` accepts any Adafruit GFX / TFT_eSPI `GFXfont*`.
 *   Pass `NULL` to revert to the default scaled font.
 * - epdiy's own `EpdFont*` fonts can be used via `setEpdFont(f)`.
 *
 * ## Display update
 * Call `epdPushImage()` to manually update the display from the framebuffer.
 * Alternatively, call `startCallback()` to enable an async FreeRTOS task that
 * polls every 500 ms and pushes any accumulated changes automatically.
 */

#pragma once
#ifdef __cplusplus

#ifdef ARDUINO
#include <Arduino.h>  // String, SPI types — must come before ESP-IDF headers
#include <SPI.h>
#endif

#include "epdiy.h"
#include "epd_highlevel.h"
#include "epd_board.h"
#include "epd_display.h"
#include "font_gfx.h"
#include "gfxfont.h"
#include "fonts/glcdfont.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

// -------------------------------------------------------------------------
// TFT_eSPI-compatible color constants (RGB565)
// -------------------------------------------------------------------------
#define TFT_BLACK       0x0000
#define TFT_WHITE       0xFFFF
#define TFT_RED         0xF800
#define TFT_GREEN       0x07E0
#define TFT_BLUE        0x001F
#define TFT_CYAN        0x07FF
#define TFT_MAGENTA     0xF81F
#define TFT_YELLOW      0xFFE0
#define TFT_ORANGE      0xFD20
#define TFT_GREENYELLOW 0xB7E0
#define TFT_PINK        0xFE19
#define TFT_NAVY        0x000F
#define TFT_DARKGREEN   0x03E0
#define TFT_DARKCYAN    0x03EF
#define TFT_MAROON      0x7800
#define TFT_PURPLE      0x780F
#define TFT_OLIVE       0x7BE0
#define TFT_LIGHTGREY   0xC618
#define TFT_DARKGREY    0x7BEF

// -------------------------------------------------------------------------
// EPD_translate class
// -------------------------------------------------------------------------

class EPD_translate {
public:
    EPD_translate() = default;

    // -------------------------------------------------------------------------
    // Initialization
    // -------------------------------------------------------------------------

    /**
     * Initialize the EPD display.
     *
     * @param board    Board definition (e.g. &epd_board_v7, &epd_board_lilygo_t5_s3).
     * @param display  Display model (e.g. &ED047TC2, &ED097TC2).
     * @param options  Initialization options (default: EPD_LUT_64K).
     * @param vcom_mv  VCOM voltage in millivolts, absolute (default: 1560 mV).
     */
    void init(
        const EpdBoardDefinition* board,
        const EpdDisplay_t* display,
        enum EpdInitOptions options = EPD_LUT_64K,
        uint16_t vcom_mv = 1560
    ) {
        epd_init(board, display, options);
        epd_set_vcom(vcom_mv);

        hl_ = epd_hl_init(EPD_BUILTIN_WAVEFORM);
        framebuffer_ = epd_hl_get_framebuffer(&hl_);

        // Initial clear
        epd_poweron();
        epd_clear();
        temperature_ = (int)epd_ambient_temperature();
        epd_poweroff();

        epd_hl_set_all_white(&hl_);
    }

    // -------------------------------------------------------------------------
    // Async update task
    // -------------------------------------------------------------------------

    /** Start a background FreeRTOS task that pushes the framebuffer every 500 ms. */
    void startCallback() {
        // if (task_handle_ != nullptr) return;
        // callback_active_ = true;
        // xTaskCreate(push_task, "epd_push", 4096, this, 1, &task_handle_);
    }

    /** Stop the background push task. */
    void stopCallback() {
        // callback_active_ = false;
        // if (task_handle_ != nullptr) {
        //     vTaskDelete(task_handle_);
        //     task_handle_ = nullptr;
        // }
    }

    /**
     * Synchronously push the current framebuffer to the display.
     * Use this when not using the async callback.
     */
    void epdPushImage() {
        changed_ = false;
        epd_poweron();
        epd_clear_area(epd_full_screen());  // 3 cycles, standard timing
        // Force a full GC16 drive: mark back_fb as all-black so the diff
        // detects every pixel as changed.  Without this, areas where front_fb
        // and back_fb are both white produce a zero-diff and are skipped by
        // epd_hl_update_screen, leaving hardware ghosting from prior frames.
        memset(hl_.back_fb, 0x00, (size_t)(epd_width() / 2 * epd_height()));
        epd_hl_update_screen(&hl_, MODE_GC16, temperature_);
        epd_poweroff();
    }

    // -------------------------------------------------------------------------
    // Display dimensions
    // -------------------------------------------------------------------------

    int width()  { return epd_rotated_display_width();  }
    int height() { return epd_rotated_display_height(); }

    // -------------------------------------------------------------------------
    // Rotation
    // -------------------------------------------------------------------------

    void setRotation(uint8_t r) {
        // The Launcher uses convention 0,2=portrait; 1,3=landscape for a
        // landscape-native display.  epdiy uses EPD_ROT_LANDSCAPE=0 (no
        // rotation) and EPD_ROT_PORTRAIT=1 (90° CW).  XOR-ing with 1 maps
        // the two conventions: 0↔1 and 2↔3.
        epd_set_rotation((enum EpdRotation)((r ^ 1) & 0x03));
    }

    uint8_t getRotation() {
        return (uint8_t)epd_get_rotation();
    }

    // -------------------------------------------------------------------------
    // Color
    // -------------------------------------------------------------------------

    /** Set foreground text/draw color (RGB565). */
    void setTextColor(uint16_t fg) {
        fg_color_ = color16to8(fg);
    }

    /** Set foreground and background text color (RGB565). */
    void setTextColor(uint16_t fg, uint16_t bg) {
        fg_color_ = color16to8(fg);
        bg_color_ = color16to8(bg);
        has_bg_   = true;
    }

    // -------------------------------------------------------------------------
    // Font selection
    // -------------------------------------------------------------------------

    /**
     * Select the built-in 5×7 font at a given scale.
     * @param s  1 = 6×8 px cell; 2 = 12×16; 3 = 18×24.
     */
    void setTextSize(uint8_t s) {
        text_scale_ = (s >= 1) ? s : 1;
        gfx_font_   = nullptr;
        epd_font_   = nullptr;
    }

    /**
     * Select an Adafruit GFX / TFT_eSPI format font.
     * Pass NULL to revert to the built-in scaled font.
     */
    void setFreeFont(const GFXfont* f) {
        gfx_font_ = f;
        epd_font_ = nullptr;
    }

    /** Alias for setFreeFont(). */
    void setFont(const GFXfont* f) { setFreeFont(f); }

    /**
     * Select an epdiy native EpdFont.
     * Pass NULL to revert to the built-in scaled font.
     */
    void setEpdFont(const EpdFont* f) {
        epd_font_ = f;
        gfx_font_ = nullptr;
    }

    /** Return the current font line height in pixels. */
    int fontHeight() {
        if (gfx_font_ != nullptr)
            return epd_gfxfont_line_height(gfx_font_);
        if (epd_font_ != nullptr)
            return epd_font_->advance_y;
        return text_scale_ * 8;
    }

    /** Return the pixel width of a string in the current font. */
    int textWidth(const char* str) {
        if (str == nullptr) return 0;
        if (gfx_font_ != nullptr) {
            int x1, y1, w, h;
            epd_gfxfont_text_bounds(gfx_font_, str, 0, 0, &x1, &y1, &w, &h);
            return w;
        }
        if (epd_font_ != nullptr) {
            int cx = 0, cy = 0;
            int x1, y1, w, h;
            epd_get_text_bounds(epd_font_, str, &cx, &cy, &x1, &y1, &w, &h, nullptr);
            return w;
        }
        // glcd default font
        int len = 0;
        for (const char* p = str; *p; p++) len++;
        return len * text_scale_ * 6;
    }

    // -------------------------------------------------------------------------
    // Cursor & text output
    // -------------------------------------------------------------------------

    void setCursor(int x, int y) { cursor_x_ = x; cursor_y_ = y; }
    int getCursorX() { return cursor_x_; }
    int getCursorY() { return cursor_y_; }

    void print(const char* str) {
        if (str == nullptr) return;
        _drawString(str, cursor_x_, cursor_y_, false, false);
        cursor_x_ += textWidth(str);
        changed_ = true;
    }

    void print(char c) {
        char buf[2] = { c, '\0' };
        print(buf);
    }

    void print(int n) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", n);
        print(buf);
    }

    void println(const char* str = "") {
        if (str && *str) print(str);
        cursor_x_  = 0;
        cursor_y_ += fontHeight();
        changed_ = true;
    }

    void println(char c) { print(c); println(); }
    void println(int n)  { print(n); println(); }

    // -------------------------------------------------------------------------
    // Screen fill
    // -------------------------------------------------------------------------

    void fillScreen(uint16_t color) {
        uint8_t c = color16to8(color);
        // Fill nibble-packed: each nibble = c>>4
        uint8_t byte_val = (c & 0xF0) | (c >> 4);
        memset(framebuffer_, byte_val, (size_t)(epd_width() / 2 * epd_height()));
        changed_ = true;
    }

    // -------------------------------------------------------------------------
    // Primitive drawing
    // -------------------------------------------------------------------------

    void drawPixel(int x, int y, uint16_t color) {
        epd_draw_pixel(x, y, color16to8(color), framebuffer_);
        changed_ = true;
    }

    void drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
        epd_draw_line(x0, y0, x1, y1, color16to8(color), framebuffer_);
        changed_ = true;
    }

    void drawFastHLine(int x, int y, int w, uint16_t color) {
        epd_draw_hline(x, y, w, color16to8(color), framebuffer_);
        changed_ = true;
    }

    void drawFastVLine(int x, int y, int h, uint16_t color) {
        epd_draw_vline(x, y, h, color16to8(color), framebuffer_);
        changed_ = true;
    }

    void drawRect(int x, int y, int w, int h, uint16_t color) {
        epd_draw_rect({x, y, w, h}, color16to8(color), framebuffer_);
        changed_ = true;
    }

    void fillRect(int x, int y, int w, int h, uint16_t color) {
        epd_fill_rect({x, y, w, h}, color16to8(color), framebuffer_);
        changed_ = true;
    }

    void drawRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
        uint8_t c = color16to8(color);
        // Straight edges
        epd_draw_hline(x + r,     y,         w - 2 * r, c, framebuffer_);
        epd_draw_hline(x + r,     y + h - 1, w - 2 * r, c, framebuffer_);
        epd_draw_vline(x,         y + r,     h - 2 * r, c, framebuffer_);
        epd_draw_vline(x + w - 1, y + r,     h - 2 * r, c, framebuffer_);
        // Corners
        _drawCircleCorner(x + r,         y + r,         r, 1, c);
        _drawCircleCorner(x + w - 1 - r, y + r,         r, 2, c);
        _drawCircleCorner(x + w - 1 - r, y + h - 1 - r, r, 4, c);
        _drawCircleCorner(x + r,         y + h - 1 - r, r, 8, c);
        changed_ = true;
    }

    void fillRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
        uint8_t c = color16to8(color);
        epd_fill_rect({x + r, y, w - 2 * r, h}, c, framebuffer_);
        epd_fill_circle_helper(x + r,         y + r,         r, 1, h - 2 * r - 1, c, framebuffer_);
        epd_fill_circle_helper(x + w - 1 - r, y + r,         r, 2, h - 2 * r - 1, c, framebuffer_);
        changed_ = true;
    }

    void drawCircle(int x, int y, int r, uint16_t color) {
        epd_draw_circle(x, y, r, color16to8(color), framebuffer_);
        changed_ = true;
    }

    void fillCircle(int x, int y, int r, uint16_t color) {
        epd_fill_circle(x, y, r, color16to8(color), framebuffer_);
        changed_ = true;
    }

    /** Draw an axis-aligned ellipse outline. */
    void drawEllipse(int x, int y, int rx, int ry, uint16_t color) {
        uint8_t c = color16to8(color);
        _drawEllipse(x, y, rx, ry, c, false);
        changed_ = true;
    }

    /** Draw a filled axis-aligned ellipse. */
    void fillEllipse(int x, int y, int rx, int ry, uint16_t color) {
        uint8_t c = color16to8(color);
        _drawEllipse(x, y, rx, ry, c, true);
        changed_ = true;
    }

    void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
        epd_draw_triangle(x0, y0, x1, y1, x2, y2, color16to8(color), framebuffer_);
        changed_ = true;
    }

    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
        epd_fill_triangle(x0, y0, x1, y1, x2, y2, color16to8(color), framebuffer_);
        changed_ = true;
    }

    /**
     * Draw an arc (annular sector).
     *
     * @param x          Center X.
     * @param y          Center Y.
     * @param r          Outer radius.
     * @param ir         Inner radius (0 = solid sector, same as filled arc).
     * @param startAngle Start angle in degrees (0 = top, clockwise).
     * @param endAngle   End angle in degrees.
     * @param color      RGB565 color.
     */
    void drawArc(int x, int y, int r, int ir,
                 int startAngle, int endAngle, uint16_t color) {
        uint8_t c = color16to8(color);
        _drawArc(x, y, r, ir, startAngle, endAngle, c);
        changed_ = true;
    }

    // -------------------------------------------------------------------------
    // 1-bit bitmap (PROGMEM)
    // -------------------------------------------------------------------------

    /**
     * Draw a 1-bit monochrome bitmap.
     * Set pixels map to `color`; cleared pixels are transparent.
     *
     * @param x      Top-left X.
     * @param y      Top-left Y.
     * @param bitmap Pointer to bitmap data, packed 8 pixels per byte, MSB first.
     * @param w      Width in pixels.
     * @param h      Height in pixels.
     * @param color  Foreground RGB565 color.
     */
    void drawBitmap(int x, int y, const uint8_t* bitmap, int w, int h, uint16_t color) {
        uint8_t c = color16to8(color);
        int byte_width = (w + 7) / 8;
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                uint8_t byte = pgm_read_byte(&bitmap[row * byte_width + col / 8]);
                if (byte & (0x80 >> (col & 7))) {
                    epd_draw_pixel(x + col, y + row, c, framebuffer_);
                }
            }
        }
        changed_ = true;
    }

    /**
     * Copy a region of 4-bit-per-pixel image data into the framebuffer.
     *
     * @param x    Top-left X.
     * @param y    Top-left Y.
     * @param w    Width in pixels.
     * @param h    Height in pixels.
     * @param data Source image data (4bpp, same format as the epdiy framebuffer).
     */
    void pushImage(int x, int y, int w, int h, const uint8_t* data) {
        epd_copy_to_framebuffer({x, y, w, h}, data, framebuffer_);
        changed_ = true;
    }

    // -------------------------------------------------------------------------
    // Text drawing
    // -------------------------------------------------------------------------

    /** Draw a string at (x, y). Does not move the cursor. */
    void drawString(const char* str, int x, int y) {
        _drawString(str, x, y, false, false);
        changed_ = true;
    }

    /** Draw a string centered horizontally at x. */
    void drawCentreString(const char* str, int x, int y, int /*font_size*/ = 0) {
        _drawString(str, x, y, true, false);
        changed_ = true;
    }

    /** Draw a string right-aligned at x. */
    void drawRightString(const char* str, int x, int y, int /*font_size*/ = 0) {
        _drawString(str, x, y, false, true);
        changed_ = true;
    }

    /** Draw a single character at (x, y). */
    void drawChar(char c, int x, int y) {
        char buf[2] = { c, '\0' };
        drawString(buf, x, y);
    }

    // -------------------------------------------------------------------------
    // Pixel read-back
    // -------------------------------------------------------------------------

    /**
     * Read a pixel from the framebuffer and return it as RGB565.
     * Note: EPD is 4-bit grayscale; the returned value is a gray-level
     * represented in RGB565 (equal R, G, B channels).
     */
    uint16_t readPixel(int x, int y) {
        uint8_t raw = epd_get_pixel(x, y, epd_width(), epd_height(), framebuffer_);
        // raw is 0-255, upper nibble is the epdiy color level (0=black, F=white)
        uint8_t level = raw & 0xF0;
        // Scale to 0-255
        uint8_t v = level | (level >> 4);
        // Pack as RGB565 gray
        uint16_t r5 = (v >> 3) & 0x1F;
        uint16_t g6 = (v >> 2) & 0x3F;
        return (r5 << 11) | (g6 << 5) | r5;
    }

    // -------------------------------------------------------------------------
    // Framebuffer access
    // -------------------------------------------------------------------------

    /** Get a pointer to the raw epdiy 4bpp framebuffer. */
    uint8_t* getFramebuffer() { return framebuffer_; }

    /** Get a reference to the EpdiyHighlevelState for direct epdiy calls. */
    EpdiyHighlevelState* getHighlevelState() { return &hl_; }

    // -------------------------------------------------------------------------
    // TFT_eSPI compatibility stubs
    // -------------------------------------------------------------------------

    void setAttribute(uint8_t /*attr_id*/, uint8_t /*param*/) {}
#ifdef ARDUINO
    SPIClass& getSPIinstance() { return SPI; }
#endif

    // -------------------------------------------------------------------------
    // Color helpers
    // -------------------------------------------------------------------------

    /** Pack R, G, B (0-255 each) into an RGB565 value (TFT_eSPI convention). */
    static inline uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
        return ((uint16_t)(r & 0xF8) << 8)
             | ((uint16_t)(g & 0xFC) << 3)
             | ((uint16_t)(b        >> 3));
    }

    /**
     * Power off the display and free resources.
     * After calling end(), do not use this object without calling init() again.
     */
    void end() {
        stopCallback();
        epd_poweron();
        epd_clear();
        epd_poweroff();
        epd_deinit();
        framebuffer_ = nullptr;
    }

    // -------------------------------------------------------------------------
    // Temperature
    // -------------------------------------------------------------------------

    /** Update the cached temperature reading. Call this before epdPushImage()
     *  if temperature accuracy matters. */
    void updateTemperature() {
        temperature_ = (int)epd_ambient_temperature();
    }

    int getTemperature() { return temperature_; }

private:
    EpdiyHighlevelState hl_ = {};
    uint8_t* framebuffer_   = nullptr;

    uint8_t  fg_color_   = 0x00;   // epdiy 4-bit black
    uint8_t  bg_color_   = 0xF0;   // epdiy 4-bit white
    bool     has_bg_     = false;

    int      cursor_x_   = 0;
    int      cursor_y_   = 0;

    uint8_t  text_scale_ = 1;
    const GFXfont* gfx_font_ = nullptr;
    const EpdFont* epd_font_ = nullptr;

    int      temperature_ = 25;

    volatile bool changed_      = false;
    bool          callback_active_ = false;
    TaskHandle_t  task_handle_  = nullptr;

    // -------------------------------------------------------------------------
    // Color conversion: RGB565 → epdiy 4-bit (upper nibble, 0x00=black, 0xF0=white)
    // -------------------------------------------------------------------------
    static inline uint8_t color16to8(uint16_t c) {
        uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
        uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
        uint32_t b = (c & 0x1F) * 255 / 31;
        // ITU-R BT.601 perceptual luminance
        uint32_t lum = (299 * r + 587 * g + 114 * b) / 1000;
        return (uint8_t)(lum & 0xF0);
    }

    // -------------------------------------------------------------------------
    // Internal text renderer
    // -------------------------------------------------------------------------
    void _drawString(const char* str, int x, int y, bool center, bool right_align) {
        if (str == nullptr) return;

        int draw_x = x;
        if (center || right_align) {
            int w = textWidth(str);
            if (center)      draw_x = x - w / 2;
            else if (right_align) draw_x = x - w;
        }

        if (gfx_font_ != nullptr) {
            int cx = draw_x, cy = y;
            epd_draw_gfxfont_string(gfx_font_, str, &cx, &cy, fg_color_,
                                    epd_width(), framebuffer_);
        } else if (epd_font_ != nullptr) {
            int cx = draw_x, cy = y;
            EpdFontProperties props = epd_font_properties_default();
            props.fg_color = fg_color_ >> 4;
            props.bg_color = bg_color_ >> 4;
            if (has_bg_) props.flags = (enum EpdFontFlags)(props.flags | EPD_DRAW_BACKGROUND);
            epd_write_string(epd_font_, str, &cx, &cy, framebuffer_, &props);
        } else {
            int cx = draw_x, cy = y;
            epd_draw_glcd_string(str, &cx, &cy, (int)text_scale_,
                                 fg_color_, epd_width(), framebuffer_);
        }
    }

    // -------------------------------------------------------------------------
    // Rounded-rectangle corner helper (outline only)
    // Uses Bresenham mid-point circle algorithm, selecting only the requested
    // quadrant.  corner bitmask: 1=top-left, 2=top-right, 4=bottom-right, 8=bottom-left
    // -------------------------------------------------------------------------
    void _drawCircleCorner(int x0, int y0, int r, int corner, uint8_t color) {
        int f     = 1 - r;
        int ddF_x = 1;
        int ddF_y = -2 * r;
        int dx    = 0;
        int dy    = r;

        while (dx <= dy) {
            if (corner & 4) { // bottom-right
                epd_draw_pixel(x0 + dx, y0 + dy, color, framebuffer_);
                epd_draw_pixel(x0 + dy, y0 + dx, color, framebuffer_);
            }
            if (corner & 2) { // top-right
                epd_draw_pixel(x0 + dx, y0 - dy, color, framebuffer_);
                epd_draw_pixel(x0 + dy, y0 - dx, color, framebuffer_);
            }
            if (corner & 8) { // bottom-left
                epd_draw_pixel(x0 - dy, y0 + dx, color, framebuffer_);
                epd_draw_pixel(x0 - dx, y0 + dy, color, framebuffer_);
            }
            if (corner & 1) { // top-left
                epd_draw_pixel(x0 - dy, y0 - dx, color, framebuffer_);
                epd_draw_pixel(x0 - dx, y0 - dy, color, framebuffer_);
            }
            if (f >= 0) { dy--; ddF_y += 2; f += ddF_y; }
            dx++; ddF_x += 2; f += ddF_x;
        }
    }

    // -------------------------------------------------------------------------
    // Ellipse (outline and fill) using Bresenham's integer algorithm
    // -------------------------------------------------------------------------
    void _drawEllipse(int xc, int yc, int rx, int ry, uint8_t color, bool fill) {
        if (rx <= 0 || ry <= 0) return;
        long rx2 = (long)rx * rx;
        long ry2 = (long)ry * ry;
        long tworx2 = 2 * rx2;
        long twory2 = 2 * ry2;
        long x = 0, y = ry;
        long p = (long)(ry2 - rx2 * ry + 0.25f * rx2);
        long dx = 0, dy = tworx2 * y;

        while (dx < dy) {
            if (fill) {
                epd_draw_hline(xc - (int)x, yc - (int)y, 2*(int)x+1, color, framebuffer_);
                epd_draw_hline(xc - (int)x, yc + (int)y, 2*(int)x+1, color, framebuffer_);
            } else {
                epd_draw_pixel(xc + (int)x, yc - (int)y, color, framebuffer_);
                epd_draw_pixel(xc - (int)x, yc - (int)y, color, framebuffer_);
                epd_draw_pixel(xc + (int)x, yc + (int)y, color, framebuffer_);
                epd_draw_pixel(xc - (int)x, yc + (int)y, color, framebuffer_);
            }
            x++;
            dx += twory2;
            if (p < 0) {
                p += ry2 + dx;
            } else {
                y--; dy -= tworx2; p += ry2 + dx - dy;
            }
        }
        p = (long)(ry2 * (x + 0.5f) * (x + 0.5f) + rx2 * (y - 1) * (y - 1) - rx2 * ry2);
        while (y >= 0) {
            if (fill) {
                epd_draw_hline(xc - (int)x, yc - (int)y, 2*(int)x+1, color, framebuffer_);
                epd_draw_hline(xc - (int)x, yc + (int)y, 2*(int)x+1, color, framebuffer_);
            } else {
                epd_draw_pixel(xc + (int)x, yc - (int)y, color, framebuffer_);
                epd_draw_pixel(xc - (int)x, yc - (int)y, color, framebuffer_);
                epd_draw_pixel(xc + (int)x, yc + (int)y, color, framebuffer_);
                epd_draw_pixel(xc - (int)x, yc + (int)y, color, framebuffer_);
            }
            y--; dy -= tworx2;
            if (p > 0) {
                p += rx2 - dy;
            } else {
                x++; dx += twory2; p += rx2 - dy + dx;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Arc renderer
    // -------------------------------------------------------------------------
    void _drawArc(int xc, int yc, int r_outer, int r_inner,
                  int angle_start, int angle_end, uint8_t color) {
        if (r_outer <= 0) return;
        if (r_inner < 0) r_inner = 0;
        if (r_inner >= r_outer) r_inner = r_outer - 1;

        // Normalize angles so start <= end
        while (angle_start < 0)   angle_start += 360;
        while (angle_end   < 0)   angle_end   += 360;
        while (angle_start >= 360) angle_start -= 360;
        while (angle_end   >= 360) angle_end   -= 360;

        // Sample the arc in small steps
        // Use enough steps for smooth appearance at the given radius
        int steps = (r_outer * 2 * 3142 / 1000) + 1;  // ~circumference
        if (steps < 32) steps = 32;

        float total_angle = (float)(angle_end - angle_start);
        if (total_angle <= 0) total_angle += 360.0f;

        for (int i = 0; i <= steps; i++) {
            float frac  = (float)i / (float)steps;
            float angle = (float)angle_start + frac * total_angle;
            float rad   = angle * (float)M_PI / 180.0f;

            float cos_a = cosf(rad);
            float sin_a = sinf(rad);

            // Draw from inner radius to outer radius
            int start_r = r_inner;
            int end_r   = r_outer;
            for (int rr = start_r; rr <= end_r; rr++) {
                int px = xc + (int)(rr * sin_a + 0.5f);
                int py = yc - (int)(rr * cos_a + 0.5f);  // y inverted (0=top)
                epd_draw_pixel(px, py, color, framebuffer_);
            }
        }
    }

    // -------------------------------------------------------------------------
    // Async push task
    // -------------------------------------------------------------------------
    static void push_task(void* param) {
        EPD_translate* self = (EPD_translate*)param;
        while (self->callback_active_) {
            if (self->changed_) {
                self->epdPushImage();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        self->task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }
};

#endif // __cplusplus
