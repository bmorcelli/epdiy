/**
 * @file epd_board_lilygo_t5_s3.c
 * @brief Board definition for the LilyGo T5 S3 E-Paper Pro H752 variant.
 */

#include <stdint.h>
#include <string.h>

#include "epd_board.h"
#include "epdiy.h"

#include "../output_common/render_method.h"
#include "../output_i80/i80_bus.h"
#include "../output_i80/rmt_pulse_s3.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <hal/gpio_ll.h>
#include <sdkconfig.h>

static const char* TAG = "t5s3";

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "epd_board_lilygo_t5_s3.c requires CONFIG_IDF_TARGET_ESP32S3"
#endif
#if !defined(RENDER_METHOD_I80)
#error "epd_board_lilygo_t5_s3.c requires EPDIY_RENDER_I80=1 in build flags"
#endif

// -------------------------------------------------------------------------
// EPD data-bus pins (8-bit, i80)
// -------------------------------------------------------------------------
#define D0 GPIO_NUM_11
#define D1 GPIO_NUM_12
#define D2 GPIO_NUM_13
#define D3 GPIO_NUM_14
#define D4 GPIO_NUM_21
#define D5 GPIO_NUM_47
#define D6 GPIO_NUM_45
#define D7 GPIO_NUM_38

// -------------------------------------------------------------------------
// EPD timing / control pins
// -------------------------------------------------------------------------
#define CKV GPIO_NUM_39  // vertical clock (RMT TX channel 0)
#define STH GPIO_NUM_9   // horizontal start pulse (i80 DC pin)
#define CKH GPIO_NUM_10  // horizontal clock (i80 WR pin)

// -------------------------------------------------------------------------
// 74HCT4094D shift-register pins
// GPIO 42 = MTMS (JTAG) — must be reset before use
// -------------------------------------------------------------------------
#define CFG_DATA GPIO_NUM_2   // serial data in
#define CFG_CLK  GPIO_NUM_42  // shift clock  (JTAG MTMS — reset required)
#define CFG_STR  GPIO_NUM_1   // storage/latch strobe

// -------------------------------------------------------------------------
// Control register (power rails, stored locally)
// -------------------------------------------------------------------------
typedef struct {
    bool power_disable     : 1;
    bool pos_power_enable  : 1;
    bool neg_power_enable  : 1;
    bool ep_scan_direction : 1;
} epd_config_register_t;

static epd_config_register_t config_reg;

// -------------------------------------------------------------------------
// ISR-safe GPIO helper (no flash access)
// -------------------------------------------------------------------------
static IRAM_ATTR inline void iram_gpio_set(int pin, bool level) {
    gpio_ll_set_level(&GPIO, pin, level);
}

// -------------------------------------------------------------------------
// Shift-register helpers
// -------------------------------------------------------------------------

static IRAM_ATTR void push_cfg_bit(bool bit) {
    iram_gpio_set(CFG_CLK, 0);
    iram_gpio_set(CFG_DATA, bit);
    iram_gpio_set(CFG_CLK, 1);
}

/**
 * Latch the full 8-bit control state into the 74HCT4094D.
 * Bit layout (MSB first, Q7..Q0):
 *   Q7=ep_output_enable, Q6=ep_mode, Q5=ep_scan_direction, Q4=ep_stv,
 *   Q3=neg_power_enable, Q2=pos_power_enable, Q1=power_disable, Q0=ep_latch_enable
 */
static IRAM_ATTR void push_cfg(const epd_ctrl_state_t* state) {
    iram_gpio_set(CFG_STR, 0);

    push_cfg_bit(state->ep_output_enable);   // Q7
    push_cfg_bit(state->ep_mode);             // Q6
    push_cfg_bit(config_reg.ep_scan_direction); // Q5
    push_cfg_bit(state->ep_stv);             // Q4

    push_cfg_bit(config_reg.neg_power_enable);  // Q3
    push_cfg_bit(config_reg.pos_power_enable);  // Q2
    push_cfg_bit(config_reg.power_disable);     // Q1
    push_cfg_bit(state->ep_latch_enable);    // Q0

    iram_gpio_set(CFG_STR, 1);
}

/** Compute the 8-bit shift register value for logging. */
static uint8_t push_cfg_byte(const epd_ctrl_state_t* state) {
    return (uint8_t)(
        (state->ep_output_enable        ? 0x80 : 0) |
        (state->ep_mode                 ? 0x40 : 0) |
        (config_reg.ep_scan_direction   ? 0x20 : 0) |
        (state->ep_stv                  ? 0x10 : 0) |
        (config_reg.neg_power_enable    ? 0x08 : 0) |
        (config_reg.pos_power_enable    ? 0x04 : 0) |
        (config_reg.power_disable       ? 0x02 : 0) |
        (state->ep_latch_enable         ? 0x01 : 0)
    );
}

// -------------------------------------------------------------------------
// i80 bus configuration
// -------------------------------------------------------------------------
static i80_bus_config i80_cfg = {
    .clock       = CKH,
    .start_pulse = STH,
    .data_0 = D0, .data_1 = D1, .data_2 = D2, .data_3 = D3,
    .data_4 = D4, .data_5 = D5, .data_6 = D6, .data_7 = D7,
};

// -------------------------------------------------------------------------
// Board callbacks
// -------------------------------------------------------------------------

static void epd_board_init(uint32_t epd_row_width) {
    ESP_LOGI(TAG, "board_init: row_width=%u", (unsigned)epd_row_width);

    // GPIO 42 is JTAG MTMS — reset before use (same as LilyGo-EPD47)
    gpio_reset_pin(CFG_CLK);
    gpio_set_direction(CFG_DATA, GPIO_MODE_OUTPUT);
    gpio_set_direction(CFG_CLK,  GPIO_MODE_OUTPUT);
    gpio_set_direction(CFG_STR,  GPIO_MODE_OUTPUT);
    iram_gpio_set(CFG_STR, 0);

    config_reg.power_disable     = true;
    config_reg.pos_power_enable  = false;
    config_reg.neg_power_enable  = false;
    config_reg.ep_scan_direction = true;

    epd_ctrl_state_t init_state = { 0 };
    ESP_LOGI(TAG, "board_init: initial push_cfg = 0x%02X", push_cfg_byte(&init_state));
    push_cfg(&init_state);

    i80_bus_init(&i80_cfg, epd_row_width + 32);
    rmt_pulse_init(CKV);

    ESP_LOGI(TAG, "board_init: done");
}

static void epd_board_deinit() {
    rmt_pulse_deinit();
    i80_bus_deinit();
}

static IRAM_ATTR void epd_board_set_ctrl(
    epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask
) {
    if (mask->ep_output_enable || mask->ep_mode || mask->ep_stv || mask->ep_latch_enable) {
        ESP_LOGD(TAG, "set_ctrl: 0x%02X (oe=%d mode=%d stv=%d latch=%d pwr_dis=%d neg=%d pos=%d)",
            push_cfg_byte(state),
            state->ep_output_enable, state->ep_mode, state->ep_stv, state->ep_latch_enable,
            config_reg.power_disable, config_reg.neg_power_enable, config_reg.pos_power_enable);
        push_cfg(state);
    }
}

static void epd_board_poweroff_common(epd_ctrl_state_t* state) {
    ESP_LOGI(TAG, "poweroff: disabling pos rail");
    config_reg.pos_power_enable = false;
    push_cfg(state);
    epd_busy_delay(10 * 240);

    ESP_LOGI(TAG, "poweroff: disabling neg rail");
    config_reg.neg_power_enable = false;
    push_cfg(state);
    epd_busy_delay(100 * 240);

    state->ep_stv           = false;
    state->ep_output_enable = false;
    state->ep_mode          = false;
    config_reg.power_disable = true;
    ESP_LOGI(TAG, "poweroff: power_disable=1, push_cfg=0x%02X", push_cfg_byte(state));
    push_cfg(state);

    i80_gpio_detach(&i80_cfg);
    ESP_LOGI(TAG, "poweroff: done");
}

static void epd_board_poweron(epd_ctrl_state_t* state) {
    ESP_LOGI(TAG, "poweron: start");
    i80_gpio_attach(&i80_cfg);

    config_reg.ep_scan_direction = true;

    config_reg.power_disable = false;
    ESP_LOGI(TAG, "poweron: power_disable=0, push_cfg=0x%02X", push_cfg_byte(state));
    push_cfg(state);
    epd_busy_delay(100 * 240);

    config_reg.neg_power_enable = true;
    ESP_LOGI(TAG, "poweron: neg_power_enable=1, push_cfg=0x%02X", push_cfg_byte(state));
    push_cfg(state);
    epd_busy_delay(500 * 240);

    config_reg.pos_power_enable = true;
    ESP_LOGI(TAG, "poweron: pos_power_enable=1, push_cfg=0x%02X", push_cfg_byte(state));
    push_cfg(state);
    epd_busy_delay(100 * 240);

    state->ep_stv = true;
    state->ep_sth = true;
    epd_ctrl_state_t mask = { .ep_stv = true, .ep_sth = true };
    ESP_LOGI(TAG, "poweron: ep_stv=1, push_cfg=0x%02X", push_cfg_byte(state));
    epd_board_set_ctrl(state, &mask);

    ESP_LOGI(TAG, "poweron: done");
}

static void epd_board_poweroff(epd_ctrl_state_t* state) {
    config_reg.ep_scan_direction = false;
    epd_board_poweroff_common(state);
}

static void epd_board_poweroff_touch(epd_ctrl_state_t* state) {
    config_reg.ep_scan_direction = true;
    epd_board_poweroff_common(state);
}

// -------------------------------------------------------------------------
// Board definitions
// -------------------------------------------------------------------------

const EpdBoardDefinition epd_board_lilygo_t5_s3 = {
    .init            = epd_board_init,
    .deinit          = epd_board_deinit,
    .set_ctrl        = epd_board_set_ctrl,
    .poweron         = epd_board_poweron,
    .poweroff        = epd_board_poweroff,
    .get_temperature = NULL,
    .set_vcom        = NULL,
};

const EpdBoardDefinition epd_board_lilygo_t5_s3_touch = {
    .init            = epd_board_init,
    .deinit          = epd_board_deinit,
    .set_ctrl        = epd_board_set_ctrl,
    .poweron         = epd_board_poweron,
    .poweroff        = epd_board_poweroff_touch,
    .get_temperature = NULL,
    .set_vcom        = NULL,
};
