/**
 * @file epd_board_lilygo_t5_s3.c
 * @brief Board definition for the LilyGo T5 S3 E-Paper Pro H752 variant.
 *
 * This board uses an ESP32-S3 with a 74HCT4094D 8-bit shift register that
 * controls both power rails and EPD timing signals (STV, LEH), and an 8-bit
 * parallel data bus driven by the ESP32-S3 LCD peripheral.
 *
 * ## How STV and LEH are driven (no hardware modification required)
 *
 * On the H752 PCB the EPD's STV (vertical start) and LEH (horizontal latch
 * enable) inputs are connected to Q4 and Q0 of the 74HCT4094D shift register,
 * NOT to direct ESP32-S3 GPIOs.  Two mechanisms are used to drive them via
 * the shift register without any hardware modification:
 *
 *   STV — once per frame:
 *     epd_lcd_start_frame() calls the stv_set_fn callback (registered below)
 *     instead of gpio_set_level().  The callback updates bit 4 of the shift
 *     register and re-latches it (push_cfg).
 *
 *   LEH — once per horizontal line:
 *     A spare GPIO (EPD_BOARD_S3_LEH_GPIO, default GPIO 3) is configured as
 *     both output and input.  The LCD peripheral's HSYNC signal is routed to
 *     this GPIO via the GPIO matrix, so the GPIO toggles in sync with the
 *     LCD's latch-enable timing.  A GPIO rising-edge ISR on the same pin
 *     calls leh_isr_fn_impl(), which pulses bit 0 of the shift register
 *     (ep_latch_enable) — effectively mirroring the HSYNC edge to the EPD's
 *     LEH input through the shift register.
 *
 *   The HSYNC-loopback GPIO (EPD_BOARD_S3_LEH_GPIO) is not physically
 *   connected to the EPD panel on the H752 PCB, so no external wire is needed.
 *
 * ## Shift-register bit layout (MSB-first, same as LilyGo-EPD47 reference)
 *
 *   bit 7 (Q7) → ep_output_enable
 *   bit 6 (Q6) → ep_mode
 *   bit 5 (Q5) → ep_scan_direction  (re-purposed: 1 = system power enable)
 *   bit 4 (Q4) → ep_stv             → EPD STV input
 *   bit 3 (Q3) → neg_power_enable
 *   bit 2 (Q2) → pos_power_enable
 *   bit 1 (Q1) → power_disable      (active-high: 1 = power off)
 *   bit 0 (Q0) → ep_latch_enable    → EPD LEH input
 */

#include "epd_board.h"

#include "../output_common/render_method.h"
#include "../output_lcd/lcd_driver.h"

#include <driver/gpio.h>
#include <sdkconfig.h>

// Compile-guard so the file doesn't break on plain ESP32 targets.
#ifndef CONFIG_IDF_TARGET_ESP32S3
#define GPIO_NUM_38 -1
#define GPIO_NUM_39 -1
#define GPIO_NUM_42 -1
#define GPIO_NUM_45 -1
#define GPIO_NUM_46 -1
#define GPIO_NUM_47 -1
#endif

// -------------------------------------------------------------------------
// EPD data-bus pins (8-bit, LCD peripheral)
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
#define CKV GPIO_NUM_39  // vertical clock (via RMT inside LCD driver)
#define STH GPIO_NUM_9   // horizontal start pulse (DE on LCD peripheral)
#define CKH GPIO_NUM_10  // horizontal clock (PCLK on LCD peripheral)

// -------------------------------------------------------------------------
// 74HCT4094D shift-register pins
// -------------------------------------------------------------------------
#define CFG_DATA GPIO_NUM_2   // serial data in
#define CFG_CLK  GPIO_NUM_42  // shift clock
#define CFG_STR  GPIO_NUM_1   // storage/latch strobe

// -------------------------------------------------------------------------
// Spare GPIO used as HSYNC loopback for LEH ISR timing.
// GPIO 3 (LoRa IRQ on H752 boards without LoRa module) is free and not
// routed to the EPD panel, making it safe to use as a loopback.
// Override at compile time if a different GPIO is preferred:
//   #define EPD_BOARD_S3_LEH_GPIO  GPIO_NUM_8
// -------------------------------------------------------------------------
#ifndef EPD_BOARD_S3_LEH_GPIO
#define EPD_BOARD_S3_LEH_GPIO GPIO_NUM_3
#endif

// -------------------------------------------------------------------------
// Control register (power rails + scan direction)
// -------------------------------------------------------------------------
typedef struct {
    bool power_disable     : 1;
    bool pos_power_enable  : 1;
    bool neg_power_enable  : 1;
    bool ep_scan_direction : 1;  // re-purposed as global power enable
} epd_config_register_t;

static epd_config_register_t config_reg;

// Current full EPD control state — kept in sync so the LEH ISR can use it.
static epd_ctrl_state_t current_state;

// -------------------------------------------------------------------------
// Shift-register helpers (IRAM-resident for ISR safety)
// -------------------------------------------------------------------------

static IRAM_ATTR void push_cfg_bit(bool bit) {
    gpio_set_level(CFG_CLK, 0);
    gpio_set_level(CFG_DATA, bit);
    gpio_set_level(CFG_CLK, 1);
}

/** Latch the full 8-bit control state into the shift register. */
static IRAM_ATTR void push_cfg(const epd_ctrl_state_t* state) {
    gpio_set_level(CFG_STR, 0);

    push_cfg_bit(state->ep_output_enable);
    push_cfg_bit(state->ep_mode);
    push_cfg_bit(config_reg.ep_scan_direction);
    push_cfg_bit(state->ep_stv);

    push_cfg_bit(config_reg.neg_power_enable);
    push_cfg_bit(config_reg.pos_power_enable);
    push_cfg_bit(config_reg.power_disable);
    push_cfg_bit(state->ep_latch_enable);

    gpio_set_level(CFG_STR, 1);
}

// -------------------------------------------------------------------------
// LCD driver callbacks (registered in LcdEpdConfig_t)
// -------------------------------------------------------------------------

/**
 * stv_set_fn — called from epd_lcd_start_frame() instead of gpio_set_level().
 * Pulsing STV through the shift register once per frame is fast enough.
 */
static IRAM_ATTR void stv_set_fn_impl(bool level, void* arg) {
    (void)arg;
    current_state.ep_stv = level;
    push_cfg(&current_state);
}

/**
 * leh_isr_fn — called from the GPIO ISR on each rising HSYNC edge.
 * Mirrors the LCD peripheral's per-line latch timing through the shift
 * register so the EPD panel receives the correct LEH pulse.
 */
static IRAM_ATTR void leh_isr_fn_impl(void* arg) {
    (void)arg;
    current_state.ep_latch_enable = true;
    push_cfg(&current_state);
    current_state.ep_latch_enable = false;
    push_cfg(&current_state);
}

// -------------------------------------------------------------------------
// LCD bus configuration
// -------------------------------------------------------------------------

static lcd_bus_config_t lcd_config = {
    .clock       = CKH,
    .ckv         = CKV,
    .leh         = EPD_BOARD_S3_LEH_GPIO,  // HSYNC loopback GPIO (spare pin)
    .start_pulse  = STH,
    .stv         = GPIO_NUM_NC,             // not used directly; see stv_set_fn
    .data = {
        [0]  = D0,  [1]  = D1,  [2]  = D2,  [3]  = D3,
        [4]  = D4,  [5]  = D5,  [6]  = D6,  [7]  = D7,
        /* upper 8 bits unused in 8-bit bus mode */
        [8]  = -1,  [9]  = -1,  [10] = -1,  [11] = -1,
        [12] = -1,  [13] = -1,  [14] = -1,  [15] = -1,
    },
};

// -------------------------------------------------------------------------
// Board callbacks
// -------------------------------------------------------------------------

static void epd_board_init(uint32_t epd_row_width) {
    gpio_set_direction(CFG_DATA, GPIO_MODE_OUTPUT);
    gpio_set_direction(CFG_CLK,  GPIO_MODE_OUTPUT);
    gpio_set_direction(CFG_STR,  GPIO_MODE_OUTPUT);
    gpio_set_level(CFG_STR, 0);

    /* Initialise power-control register and control state. */
    config_reg.power_disable     = true;
    config_reg.pos_power_enable  = false;
    config_reg.neg_power_enable  = false;
    config_reg.ep_scan_direction = true;

    current_state = (epd_ctrl_state_t){ 0 };
    push_cfg(&current_state);

    const EpdDisplay_t* display = epd_get_display();

    LcdEpdConfig_t config = {
        .pixel_clock      = display->bus_speed * 1000 * 1000,
        .ckv_high_time    = 60,
        .line_front_porch = 4,
        .le_high_time     = 4,
        .bus_width        = display->bus_width,
        .bus              = lcd_config,
        /* STV via shift register callback — no hardware mod needed */
        .stv_set_fn       = stv_set_fn_impl,
        .stv_arg          = NULL,
        /* LEH via GPIO ISR loopback — no hardware mod needed */
        .leh_isr_fn       = leh_isr_fn_impl,
        .leh_arg          = NULL,
    };
    epd_lcd_init(&config, display->width, display->height);
}

static void epd_board_deinit() {
    epd_lcd_deinit();
}

static void epd_board_set_ctrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
    /* Keep current_state in sync for the LEH ISR. */
    if (mask->ep_output_enable || mask->ep_mode || mask->ep_stv || mask->ep_latch_enable) {
        current_state = *state;
        push_cfg(state);
    }
}

static void epd_board_poweroff_common(epd_ctrl_state_t* state) {
    config_reg.pos_power_enable = false;
    current_state = *state;
    push_cfg(state);
    epd_busy_delay(10 * 240);

    config_reg.neg_power_enable = false;
    push_cfg(state);
    epd_busy_delay(100 * 240);

    state->ep_stv           = false;
    state->ep_output_enable = false;
    state->ep_mode          = false;
    config_reg.power_disable = true;
    current_state = *state;
    push_cfg(state);
}

static void epd_board_poweron(epd_ctrl_state_t* state) {
    /* ep_scan_direction is re-purposed as the global power enable. */
    config_reg.ep_scan_direction = true;

    config_reg.power_disable = false;
    current_state = *state;
    push_cfg(state);
    epd_busy_delay(100 * 240);

    config_reg.neg_power_enable = true;
    push_cfg(state);
    epd_busy_delay(500 * 240);

    config_reg.pos_power_enable = true;
    push_cfg(state);
    epd_busy_delay(100 * 240);

    state->ep_stv = true;
    state->ep_sth = true;
    current_state = *state;
    epd_board_set_ctrl(state, &(epd_ctrl_state_t){ .ep_stv = true, .ep_sth = true });
}

static void epd_board_poweroff(epd_ctrl_state_t* state) {
    config_reg.ep_scan_direction = false;
    epd_board_poweroff_common(state);
}

static void epd_board_poweroff_touch(epd_ctrl_state_t* state) {
    /* Keep ep_scan_direction high to keep touch controller powered. */
    config_reg.ep_scan_direction = true;
    epd_board_poweroff_common(state);
}

// -------------------------------------------------------------------------
// Board definitions
// -------------------------------------------------------------------------

/** LilyGo T5 S3 E-Paper Pro H752 — no hardware modification required. */
const EpdBoardDefinition epd_board_lilygo_t5_s3 = {
    .init            = epd_board_init,
    .deinit          = epd_board_deinit,
    .set_ctrl        = epd_board_set_ctrl,
    .poweron         = epd_board_poweron,
    .poweroff        = epd_board_poweroff,
    .get_temperature = NULL,
    .set_vcom        = NULL,
};

/** LilyGo T5 S3 E-Paper Pro H752 — keeps the touch controller powered
 *  across display power-off cycles. */
const EpdBoardDefinition epd_board_lilygo_t5_s3_touch = {
    .init            = epd_board_init,
    .deinit          = epd_board_deinit,
    .set_ctrl        = epd_board_set_ctrl,
    .poweron         = epd_board_poweron,
    .poweroff        = epd_board_poweroff_touch,
    .get_temperature = NULL,
    .set_vcom        = NULL,
};
