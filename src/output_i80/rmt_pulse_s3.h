/**
 * RMT-based CKV pulse generator for ESP32-S3 (IDF 5.x new TX API).
 * Used exclusively by the output_i80 render path.
 */

#pragma once

#include <driver/gpio.h>
#include <esp_attr.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize an RMT TX channel on the given pin for CKV pulsing.
 * Resolution: 10 MHz → 1 tick = 0.1 µs.
 */
void rmt_pulse_init(gpio_num_t pin);

/**
 * Disable and free the RMT TX channel.
 */
void rmt_pulse_deinit(void);

/**
 * Output a single high→low pulse.
 * Always waits for any previous pulse to finish first.
 *
 * @param high_time_ticks  HIGH phase in 0.1 µs ticks.
 * @param low_time_ticks   LOW  phase in 0.1 µs ticks.
 * @param wait             If true, blocks until the pulse is complete.
 */
void pulse_ckv_ticks(uint16_t high_time_ticks, uint16_t low_time_ticks, bool wait);

/**
 * Output a single high→low pulse (times in microseconds).
 */
void pulse_ckv_us(uint16_t high_time_us, uint16_t low_time_us, bool wait);

/**
 * Returns true while the RMT peripheral is transmitting.
 */
bool rmt_busy(void);
