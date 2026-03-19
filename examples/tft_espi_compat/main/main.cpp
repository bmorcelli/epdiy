/**
 * @file main.cpp
 * @brief TFT_eSPI compatibility layer demo for epdiy.
 *
 * Demonstrates EPD_translate — an epdiy v2 wrapper that exposes the TFT_eSPI
 * drawing API — on the LilyGo T5 S3 E-Paper Pro 4.7" display.
 *
 * ## Board selection
 *
 * Uncomment exactly ONE of the two #define lines below:
 *
 *   USE_BOARD_H752    → epd_board_lilygo_t5_s3
 *                       (74HCT4094D shift register, 8-bit LCD bus)
 *                       IMPORTANT: requires STV and LEH hardware wire mod.
 *                       See src/board/epd_board_lilygo_t5_s3.c for details.
 *
 *   USE_BOARD_H752_1  → epd_board_v7
 *                       (PCA9555 I2C expander + TPS65185 PMIC, 16-bit LCD bus)
 *                       Works out of the box with no hardware modification.
 *
 * Both boards use the same ED047TC2 display panel and the same EPD_translate
 * API; only the board pointer passed to tft.init() differs.
 */

#include "EPD_translate.h"
#include "epd_board.h"
#include "epd_display.h"

// ---------------------------------------------------------------------------
// Board selection — uncomment ONE of:
// ---------------------------------------------------------------------------
// #define USE_BOARD_H752      // LilyGo T5 S3 H752   (74HCT4094D, needs HW mod)
#define USE_BOARD_H752_1    // LilyGo T5 S3 H752-1 (PCA9555, out of the box)

#ifdef USE_BOARD_H752
  #define SELECTED_BOARD      (&epd_board_lilygo_t5_s3)
  #define SELECTED_BOARD_NAME "T5 S3 H752 (shift-reg, 8-bit)"
#else
  #define SELECTED_BOARD      (&epd_board_v7)
  #define SELECTED_BOARD_NAME "T5 S3 H752-1 (epd_board_v7)"
#endif

/* VCOM voltage in millivolts — adjust for your specific panel label */
#define VCOM_MV  1560

// ---------------------------------------------------------------------------
// Global instance
// ---------------------------------------------------------------------------
static EPD_translate tft;

// ---------------------------------------------------------------------------
// Demo scenes
// ---------------------------------------------------------------------------

/** Hello World text screen. */
static void demo_hello_world(void) {
    tft.fillScreen(TFT_WHITE);

    tft.setTextColor(TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 20);
    tft.println("Hello epdiy!");

    tft.setTextSize(1);
    tft.setCursor(10, 60);
    tft.println("EPD_translate: TFT_eSPI API on epdiy v2");
    tft.setCursor(10, 78);
    tft.print("Board: ");
    tft.println(SELECTED_BOARD_NAME);

    tft.epdPushImage();
}

/** Basic geometric primitives. */
static void demo_primitives(void) {
    tft.fillScreen(TFT_WHITE);

    // Filled rectangle
    tft.fillRect(20, 20, 200, 80, TFT_BLACK);

    // Outline rectangle
    tft.drawRect(240, 20, 200, 80, TFT_BLACK);

    // Filled circle
    tft.fillCircle(80, 200, 50, TFT_DARKGREY);

    // Outline circle
    tft.drawCircle(220, 200, 50, TFT_BLACK);

    // Triangle
    tft.drawTriangle(320, 150, 420, 250, 220, 250, TFT_BLACK);
    tft.fillTriangle(470, 150, 570, 250, 370, 250, TFT_DARKGREY);

    // Rounded rectangles
    tft.drawRoundRect(20, 280, 180, 80, 15, TFT_BLACK);
    tft.fillRoundRect(220, 280, 180, 80, 15, TFT_DARKGREY);

    // Horizontal and vertical lines
    tft.drawFastHLine(0, 390, tft.width(), TFT_BLACK);
    tft.drawFastVLine(tft.width() / 2, 400, 60, TFT_BLACK);

    tft.epdPushImage();
}

/** Grayscale gradient using drawPixel. */
static void demo_gradient(void) {
    tft.fillScreen(TFT_WHITE);

    const int w   = tft.width();
    const int h   = 120;
    const int top = 40;

    // 16-step grayscale ramp
    for (int step = 0; step < 16; step++) {
        uint8_t  lum   = (uint8_t)(step * 17);
        uint16_t rgb   = EPD_translate::color565(lum, lum, lum);
        int      x0    = step * (w / 16);
        int      x1    = (step + 1) * (w / 16);
        for (int x = x0; x < x1; x++) {
            for (int y = top; y < top + h; y++) {
                tft.drawPixel(x, y, rgb);
            }
        }
    }

    tft.setTextColor(TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(10, top + h + 10);
    tft.println("16-step grayscale gradient (drawPixel)");

    tft.epdPushImage();
}

/** Text rendering at various sizes. */
static void demo_text(void) {
    tft.fillScreen(TFT_WHITE);

    tft.setFreeFont(NULL);   // revert to built-in glcd font

    tft.setTextColor(TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(10, 10);
    tft.println("Scale 1 (default 5x7 font)");

    tft.setTextSize(2);
    tft.setCursor(10, 30);
    tft.println("Scale 2 — 10x14");

    tft.setTextSize(3);
    tft.setCursor(10, 60);
    tft.println("Scale 3 — 15x21");

    tft.setTextSize(1);
    tft.drawCentreString("Centred text", tft.width() / 2, 120, 0);
    tft.drawRightString("Right-aligned", tft.width() - 5, 140, 0);

    // Ellipse
    tft.drawEllipse(tft.width() / 2, 230, 150, 60, TFT_BLACK);
    tft.fillEllipse(tft.width() / 2, 340, 80, 40, TFT_DARKGREY);

    // Arc: outer r=90, inner r=60, from 0° to 270°
    tft.drawArc(tft.width() / 2, 470, 90, 60, 0, 270, TFT_BLACK);

    tft.epdPushImage();
}

/** Simple bar chart. */
static void demo_bar_chart(void) {
    tft.fillScreen(TFT_WHITE);

    static const char* labels[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    static const int   values[] = {  60,    85,    45,   100,    70,    30,    90  };
    const int n       = 7;
    const int chart_x = 40;
    const int chart_y = 280;
    const int bar_w   = 60;
    const int gap     = 20;
    const int max_h   = 220;
    const int max_val = 100;

    // Axes
    tft.drawFastVLine(chart_x - 5, chart_y - max_h - 10, max_h + 10, TFT_BLACK);
    tft.drawFastHLine(chart_x - 5, chart_y, n * (bar_w + gap) + 10, TFT_BLACK);

    for (int i = 0; i < n; i++) {
        int bar_h = (values[i] * max_h) / max_val;
        int bar_x = chart_x + i * (bar_w + gap);
        int bar_y = chart_y - bar_h;

        tft.fillRect(bar_x, bar_y, bar_w, bar_h, TFT_DARKGREY);
        tft.drawRect(bar_x, bar_y, bar_w, bar_h, TFT_BLACK);

        tft.setTextSize(1);
        tft.setTextColor(TFT_BLACK);
        // Label below bar
        tft.setCursor(bar_x + (bar_w - 18) / 2, chart_y + 5);
        tft.print(labels[i]);
        // Value above bar
        tft.setCursor(bar_x + (bar_w - 18) / 2, bar_y - 12);
        tft.print(values[i]);
    }

    tft.setCursor(chart_x, 10);
    tft.println("Weekly activity (bar chart demo)");

    tft.epdPushImage();
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
extern "C" void app_main(void) {
    // Initialize display.
    // EPD_LUT_64K gives the best grayscale quality (slower).
    // Use EPD_LUT_1K for faster but coarser updates.
    tft.init(SELECTED_BOARD, &ED047TC2, EPD_LUT_64K, VCOM_MV);

    demo_hello_world();
    vTaskDelay(pdMS_TO_TICKS(3000));

    demo_primitives();
    vTaskDelay(pdMS_TO_TICKS(3000));

    demo_gradient();
    vTaskDelay(pdMS_TO_TICKS(3000));

    demo_text();
    vTaskDelay(pdMS_TO_TICKS(3000));

    demo_bar_chart();
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Clear and power off
    tft.fillScreen(TFT_WHITE);
    tft.epdPushImage();
    tft.end();
}
