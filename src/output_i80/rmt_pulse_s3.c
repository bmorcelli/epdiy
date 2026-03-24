#include "rmt_pulse_s3.h"

#include "../output_common/render_method.h"

#ifdef RENDER_METHOD_I80

#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_err.h>
#include <esp_log.h>

// 10 MHz resolution → 1 tick = 0.1 µs, matching the classic ESP32 I2S path.
#define CKV_RMT_RESOLUTION_HZ  10000000UL

static rmt_channel_handle_t s_ckv_chan    = NULL;
static rmt_encoder_handle_t s_ckv_encoder = NULL;

// Volatile flag set in the TX-done callback (ISR context).
static volatile bool s_rmt_done = true;

// Static symbol buffer: pulse symbol + end-of-sequence sentinel.
static rmt_symbol_word_t s_pulse_sym[2];

static bool IRAM_ATTR ckv_tx_done_cb(
    rmt_channel_handle_t chan,
    const rmt_tx_done_event_data_t* edata,
    void* user_ctx
) {
    s_rmt_done = true;
    return false;  // no higher-priority task awakened
}

void rmt_pulse_init(gpio_num_t pin) {
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num          = pin,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = CKV_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .flags.invert_out  = false,
        .flags.with_dma    = false,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &s_ckv_chan));

    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, &s_ckv_encoder));

    rmt_tx_event_callbacks_t cbs = { .on_trans_done = ckv_tx_done_cb };
    ESP_ERROR_CHECK(rmt_tx_register_event_callbacks(s_ckv_chan, &cbs, NULL));

    ESP_ERROR_CHECK(rmt_enable(s_ckv_chan));
}

void rmt_pulse_deinit(void) {
    if (s_ckv_chan) {
        rmt_disable(s_ckv_chan);
        rmt_del_encoder(s_ckv_encoder);
        rmt_del_channel(s_ckv_chan);
        s_ckv_chan    = NULL;
        s_ckv_encoder = NULL;
    }
}

void IRAM_ATTR pulse_ckv_ticks(uint16_t high_time_ticks, uint16_t low_time_ticks, bool wait) {
    while (!s_rmt_done) {}  // wait for previous pulse to finish

    if (high_time_ticks > 0) {
        s_pulse_sym[0].level0    = 1;
        s_pulse_sym[0].duration0 = high_time_ticks;
        s_pulse_sym[0].level1    = 0;
        s_pulse_sym[0].duration1 = low_time_ticks;
    } else {
        // high_time == 0: emit a single low-only segment
        s_pulse_sym[0].level0    = 1;
        s_pulse_sym[0].duration0 = low_time_ticks;
        s_pulse_sym[0].level1    = 0;
        s_pulse_sym[0].duration1 = 0;
    }
    s_pulse_sym[1].val = 0;  // end-of-sequence sentinel

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    s_rmt_done = false;
    rmt_transmit(s_ckv_chan, s_ckv_encoder, s_pulse_sym, sizeof(s_pulse_sym), &tx_cfg);

    while (wait && !s_rmt_done) {}
}

void IRAM_ATTR pulse_ckv_us(uint16_t high_time_us, uint16_t low_time_us, bool wait) {
    pulse_ckv_ticks(10 * high_time_us, 10 * low_time_us, wait);
}

bool IRAM_ATTR rmt_busy(void) {
    return !s_rmt_done;
}

#endif  // RENDER_METHOD_I80
