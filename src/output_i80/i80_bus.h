/**
 * 8-bit parallel Intel 8080 (i80) bus driver for ESP32-S3.
 * Uses the ESP-IDF LCD panel IO API (esp_lcd_new_i80_bus).
 * Only compiled when RENDER_METHOD_I80 is active.
 */

#pragma once

#include <driver/gpio.h>
#include <esp_attr.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * i80 bus configuration parameters.
 */
typedef struct {
    // GPIO numbers of the 8 parallel data pins.
    gpio_num_t data_0;
    gpio_num_t data_1;
    gpio_num_t data_2;
    gpio_num_t data_3;
    gpio_num_t data_4;
    gpio_num_t data_5;
    gpio_num_t data_6;
    gpio_num_t data_7;

    // WR (write clock) pin — connected to CKH on the display.
    gpio_num_t clock;

    // DC pin — connected to STH on the display.
    // Driven HIGH for 10 WR cycles as the horizontal start pulse,
    // then LOW for pixel data.
    gpio_num_t start_pulse;
} i80_bus_config;

/**
 * Initialize the i80 bus.
 * @param epd_row_width  Display width in pixels (2 bits per pixel).
 */
void i80_bus_init(i80_bus_config* cfg, uint32_t epd_row_width);

/** No-op — the i80 peripheral keeps the pins permanently routed. */
void i80_gpio_attach(i80_bus_config* cfg);

/** No-op — the i80 peripheral keeps the pins permanently routed. */
void i80_gpio_detach(i80_bus_config* cfg);

/** Returns a pointer to the single line buffer. */
uint8_t* i80_get_current_buffer(void);

/** Blocks until the current DMA transfer completes (single-buffer safety). */
void i80_switch_buffer(void);

/** Transmit the current line buffer to the display. */
void i80_start_line_output(void);

/** Returns true while a DMA transfer is in flight. */
bool i80_is_busy(void);

/** Returns the number of completed DMA transfers (for debug). */
uint32_t i80_get_transfer_count(void);

/** Free all resources. */
void i80_bus_deinit(void);
