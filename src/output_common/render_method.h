#pragma once

#include "sdkconfig.h"

/**
 * Rendering Method / Hardware to use.
 */
#ifdef __cplusplus
extern "C" {
#endif

enum EpdRenderMethod {
    /// Use the I2S peripheral on ESP32 chips.
    EPD_RENDER_METHOD_I2S = 1,
    /// Use the CAM/LCD peripheral in ESP32-S3 chips.
    EPD_RENDER_METHOD_LCD = 2,
    /// Use the Intel 8080 (i80) parallel bus on ESP32-S3 chips.
    EPD_RENDER_METHOD_I80 = 3,
};

extern enum EpdRenderMethod EPD_CURRENT_RENDER_METHOD;
void epd_set_render_method(enum EpdRenderMethod method);

#ifdef CONFIG_IDF_TARGET_ESP32
#define RENDER_METHOD_I2S 1
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
/**
 * Set -DEPDIY_RENDER_I80=1 in build flags to use the Intel 8080 (i80)
 * per-line rendering path on ESP32-S3.  Required for boards like the
 * LilyGo T5 S3 E-Paper Pro H752 that are wired to the i80 interface
 * of the LCD_CAM peripheral.
 *
 * Without this flag the default RGB LCD streaming path is used.
 */
#if defined(EPDIY_RENDER_DUAL_S3)
#define RENDER_METHOD_LCD 1
#define RENDER_METHOD_I80 1
#elif defined(EPDIY_RENDER_I80)
#define RENDER_METHOD_I80 1
#else
#define RENDER_METHOD_LCD 1
#endif
#else
#error "unknown chip, cannot choose render method!"
#endif

#ifdef __clang__
#define IRAM_ATTR
// define this if we're using clangd to make it accept the GCC builtin
void __assert_func(const char* file, int line, const char* func, const char* failedexpr);
#endif

#ifdef __cplusplus
}
#endif
