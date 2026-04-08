#include "render_method.h"
#include "sdkconfig.h"

#ifdef CONFIG_IDF_TARGET_ESP32
enum EpdRenderMethod EPD_CURRENT_RENDER_METHOD = EPD_RENDER_METHOD_I2S;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#if defined(EPDIY_RENDER_DUAL_S3)
enum EpdRenderMethod EPD_CURRENT_RENDER_METHOD = EPD_RENDER_METHOD_LCD;
#elif defined(EPDIY_RENDER_I80)
enum EpdRenderMethod EPD_CURRENT_RENDER_METHOD = EPD_RENDER_METHOD_I80;
#else
enum EpdRenderMethod EPD_CURRENT_RENDER_METHOD = EPD_RENDER_METHOD_LCD;
#endif
#else
#error "unknown chip, cannot choose render method!"
#endif

void epd_set_render_method(enum EpdRenderMethod method) {
#ifdef CONFIG_IDF_TARGET_ESP32
    EPD_CURRENT_RENDER_METHOD = EPD_RENDER_METHOD_I2S;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#if defined(RENDER_METHOD_LCD) && defined(RENDER_METHOD_I80)
    if (method == EPD_RENDER_METHOD_I80 || method == EPD_RENDER_METHOD_LCD) {
        EPD_CURRENT_RENDER_METHOD = method;
    }
#elif defined(RENDER_METHOD_I80)
    (void)method;
    EPD_CURRENT_RENDER_METHOD = EPD_RENDER_METHOD_I80;
#else
    (void)method;
    EPD_CURRENT_RENDER_METHOD = EPD_RENDER_METHOD_LCD;
#endif
#else
    (void)method;
#endif
}
