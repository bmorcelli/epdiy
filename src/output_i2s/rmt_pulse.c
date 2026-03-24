#include "../output_common/render_method.h"
#include "esp_intr_alloc.h"

#ifdef RENDER_METHOD_I2S

#include "rmt_pulse.h"

// ---------------------------------------------------------------------------
// ESP32-S3 path: use the IDF5 new RMT TX API (driver/rmt_tx.h).
// The old register-direct / rmt_ll approach is not compatible with IDF 5.5
// on S3 (rmt_mem_t, rmt_item32_t and rmt_ll_tx_is_idle no longer exist).
// ---------------------------------------------------------------------------
#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include <driver/rmt_tx.h>    // rmt_new_tx_channel, rmt_transmit, etc.
#include <driver/rmt_encoder.h>  // rmt_new_copy_encoder
#include <esp_err.h>
#include <esp_log.h>

// 10 MHz resolution → 1 tick = 0.1 µs, matching the ESP32 legacy path.
#define CKV_RMT_RESOLUTION_HZ  10000000UL

static rmt_channel_handle_t s_ckv_chan    = NULL;
static rmt_encoder_handle_t s_ckv_encoder = NULL;

// Volatile flag set in the TX-done callback (ISR context).
static volatile bool s_rmt_done = true;

// Static symbol buffer — two symbols: the pulse + end-of-sequence sentinel.
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

void rmt_pulse_deinit() {
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

bool IRAM_ATTR rmt_busy() {
    return !s_rmt_done;
}

// ---------------------------------------------------------------------------
// Original ESP32 path: legacy direct-register implementation.
// ---------------------------------------------------------------------------
#else  // CONFIG_IDF_TARGET_ESP32

#include "driver/rmt.h"
#include "soc/rmt_struct.h"

static intr_handle_t gRMT_intr_handle = NULL;

// the RMT channel configuration object
static rmt_config_t row_rmt_config;

// keep track of wether the current pulse is ongoing
volatile bool rmt_tx_done = true;

/**
 * Remote peripheral interrupt. Used to signal when transmission is done.
 */
static void IRAM_ATTR rmt_interrupt_handler(void* arg) {
    rmt_tx_done = true;
    RMT.int_clr.val = RMT.int_st.val;
}

// The extern line is declared in esp-idf/components/driver/deprecated/rmt_legacy.c. It has access
// to RMTMEM through the rmt_private.h header which we can't access outside the sdk. Declare our own
// extern here to properly use the RMTMEM smybol defined in
// components/soc/[target]/ld/[target].peripherals.ld Also typedef the new rmt_mem_t struct to the
// old rmt_block_mem_t struct. Same data fields, different names
typedef rmt_mem_t rmt_block_mem_t;
extern rmt_block_mem_t RMTMEM;

void rmt_pulse_init(gpio_num_t pin) {
    row_rmt_config.rmt_mode = RMT_MODE_TX;
    // currently hardcoded: use channel 0
    row_rmt_config.channel = RMT_CHANNEL_1;

    row_rmt_config.gpio_num = pin;
    row_rmt_config.mem_block_num = 2;

    // Divide 80MHz APB Clock by 8 -> .1us resolution delay
    row_rmt_config.clk_div = 8;

    row_rmt_config.tx_config.loop_en = false;
    row_rmt_config.tx_config.carrier_en = false;
    row_rmt_config.tx_config.carrier_level = RMT_CARRIER_LEVEL_LOW;
    row_rmt_config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    row_rmt_config.tx_config.idle_output_en = true;

    esp_intr_alloc(
        ETS_RMT_INTR_SOURCE, ESP_INTR_FLAG_LEVEL3, rmt_interrupt_handler, 0, &gRMT_intr_handle
    );

    rmt_config(&row_rmt_config);
    rmt_set_tx_intr_en(row_rmt_config.channel, true);
}

void rmt_pulse_deinit() {
    esp_intr_disable(gRMT_intr_handle);
    esp_intr_free(gRMT_intr_handle);
}

void IRAM_ATTR pulse_ckv_ticks(uint16_t high_time_ticks, uint16_t low_time_ticks, bool wait) {
    while (!rmt_tx_done) {
    };
    volatile rmt_item32_t* rmt_mem_ptr = &(RMTMEM.chan[row_rmt_config.channel].data32[0]);
    if (high_time_ticks > 0) {
        rmt_mem_ptr->level0 = 1;
        rmt_mem_ptr->duration0 = high_time_ticks;
        rmt_mem_ptr->level1 = 0;
        rmt_mem_ptr->duration1 = low_time_ticks;
    } else {
        rmt_mem_ptr->level0 = 1;
        rmt_mem_ptr->duration0 = low_time_ticks;
        rmt_mem_ptr->level1 = 0;
        rmt_mem_ptr->duration1 = 0;
    }
    RMTMEM.chan[row_rmt_config.channel].data32[1].val = 0;
    rmt_tx_done = false;
    RMT.conf_ch[row_rmt_config.channel].conf1.mem_rd_rst = 1;
    RMT.conf_ch[row_rmt_config.channel].conf1.mem_owner = RMT_MEM_OWNER_TX;
    RMT.conf_ch[row_rmt_config.channel].conf1.tx_start = 1;
    while (wait && !rmt_tx_done) {
    };
}

void IRAM_ATTR pulse_ckv_us(uint16_t high_time_us, uint16_t low_time_us, bool wait) {
    pulse_ckv_ticks(10 * high_time_us, 10 * low_time_us, wait);
}

bool IRAM_ATTR rmt_busy() {
    return !rmt_tx_done;
}

#endif  // CONFIG_IDF_TARGET_ESP32
#endif  // RENDER_METHOD_I2S
