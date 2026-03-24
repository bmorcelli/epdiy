#include "i80_bus.h"

#include "../output_common/render_method.h"

#ifdef RENDER_METHOD_I80

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "epdiy_i80";

/// Handle for the i80 panel IO.
static esp_lcd_panel_io_handle_t s_io_handle = NULL;

/// Single line buffer used for all row transfers.
static uint8_t* s_line_buf      = NULL;
static size_t   s_line_buf_size = 0;

/// Set to true by the transfer-done callback.
static volatile bool s_output_done = true;

/// Counts completed DMA transfers; incremented in ISR context (safe: atomic on S3).
static volatile uint32_t s_transfer_count = 0;

static bool IRAM_ATTR i80_trans_done_cb(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* edata,
    void* user_ctx
) {
    s_output_done = true;
    s_transfer_count++;
    return false;  // no higher-priority task awakened
}

void i80_bus_init(i80_bus_config* cfg, uint32_t epd_row_width) {
    // epd_row_width is in pixels (2 bits each) → bytes = / 4
    s_line_buf_size = epd_row_width / 4;
    s_line_buf = heap_caps_calloc(1, s_line_buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(s_line_buf != NULL);

    ESP_LOGI(TAG, "init i80 bus, %u bytes/line", (unsigned)s_line_buf_size);

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = cfg->start_pulse,  // STH used as DC
        .wr_gpio_num = cfg->clock,         // CKH = WR (pixel clock)
        .clk_src     = LCD_CLK_SRC_DEFAULT,
        /* The bit ordering {D6,D7,D4,D5,D2,D3,D0,D1} matches the waveform
         * byte layout produced by the epdiy LUT, as used in LilyGo-EPD47. */
        .data_gpio_nums = {
            cfg->data_6, cfg->data_7,
            cfg->data_4, cfg->data_5,
            cfg->data_2, cfg->data_3,
            cfg->data_0, cfg->data_1,
        },
        .bus_width          = 8,
        .max_transfer_bytes = s_line_buf_size,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num       = -1,
        .pclk_hz           = 10 * 1000 * 1000,  // 10 MHz pixel clock
        .trans_queue_depth = 4,
        .dc_levels = {
            .dc_idle_level  = 0,  // STH LOW when idle
            .dc_cmd_level   = 1,  // STH HIGH during command phase (= horizontal start pulse)
            .dc_dummy_level = 0,
            .dc_data_level  = 0,  // STH LOW during pixel data
        },
        .on_color_trans_done = i80_trans_done_cb,
        .user_ctx            = NULL,
        /* 10 command bits → STH stays HIGH for 10 WR cycles, then drops LOW
         * for the pixel data.  No extra parameter phase between cmd and data. */
        .lcd_cmd_bits   = 10,
        .lcd_param_bits = 0,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &s_io_handle));
}

// gpio_attach / detach are no-ops: the i80 peripheral keeps the pins routed.
void i80_gpio_attach(i80_bus_config* cfg) { (void)cfg; }
void i80_gpio_detach(i80_bus_config* cfg) { (void)cfg; }

uint8_t* IRAM_ATTR i80_get_current_buffer(void) {
    return s_line_buf;
}

// Single-buffer implementation: block until the current DMA transfer completes
// so the caller can safely overwrite the buffer for the next row.
void IRAM_ATTR i80_switch_buffer(void) {
    while (!s_output_done) {}
}

void IRAM_ATTR i80_start_line_output(void) {
    s_output_done = false;
    // cmd=0 triggers the STH start pulse (dc_cmd_level=1 for 10 WR cycles),
    // then pixel data follows with dc_data_level=0.
    esp_lcd_panel_io_tx_color(s_io_handle, 0, s_line_buf, s_line_buf_size);
}

bool IRAM_ATTR i80_is_busy(void) {
    return !s_output_done;
}

uint32_t i80_get_transfer_count(void) {
    return s_transfer_count;
}

void i80_bus_deinit(void) {
    free(s_line_buf);
    s_line_buf      = NULL;
    s_line_buf_size = 0;
}

#endif  // RENDER_METHOD_I80
