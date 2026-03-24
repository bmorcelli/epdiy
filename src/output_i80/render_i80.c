#include "render_i80.h"

#include "../output_common/render_method.h"

#ifdef RENDER_METHOD_I80

#include <stdint.h>
#include <string.h>

#include <esp_log.h>

static const char* TAG = "render_i80";

#include "epd_board.h"
#include "epd_internals.h"
#include "epdiy.h"

#include "../output_common/lut.h"
#include "../output_common/render_context.h"
#include "i80_bus.h"
#include "rmt_pulse_s3.h"

static const epd_ctrl_state_t NoChangeState = { 0 };

/**
 * Waits until all previously submitted data has been written.
 * Then:
 *  - Latches the previously loaded row to the output register (LEH pulse).
 *  - Pulses CKV via RMT for the row output time.
 *  - Triggers the i80 DMA transfer of the current line buffer.
 */
static void IRAM_ATTR i80_output_row(uint32_t output_time_dus) {
    while (i80_is_busy() || rmt_busy()) {}

    const EpdBoardDefinition* epd_board = epd_current_board();
    epd_ctrl_state_t* ctrl_state = epd_ctrl_state();
    epd_ctrl_state_t mask = NoChangeState;

    ctrl_state->ep_sth = true;
    ctrl_state->ep_latch_enable = true;
    mask.ep_sth = true;
    mask.ep_latch_enable = true;
    epd_board->set_ctrl(ctrl_state, &mask);

    mask = NoChangeState;
    ctrl_state->ep_latch_enable = false;
    mask.ep_latch_enable = true;
    epd_board->set_ctrl(ctrl_state, &mask);

    if (epd_get_display()->display_type == DISPLAY_TYPE_ED097TC2) {
        pulse_ckv_ticks(output_time_dus, 1, false);
    } else {
        pulse_ckv_ticks(output_time_dus, 50, false);
    }

    i80_start_line_output();
    i80_switch_buffer();
}

/** Like i80_output_row, but resets the skip indicator. */
static void IRAM_ATTR i80_write_row(RenderContext_t* ctx, uint32_t output_time_dus) {
    i80_output_row(output_time_dus);
    ctx->skipping = 0;
}

/** Skip a row without writing to it. */
static void IRAM_ATTR i80_skip_row(RenderContext_t* ctx, uint8_t pipeline_finish_time) {
    int line_bytes = ctx->display_width / 4;
    if (ctx->skipping < 2) {
        memset((void*)i80_get_current_buffer(), 0x00, line_bytes);
        i80_output_row(pipeline_finish_time);
    } else {
        if (epd_get_display()->display_type == DISPLAY_TYPE_ED097TC2) {
            pulse_ckv_ticks(5, 5, false);
        } else {
            pulse_ckv_ticks(45, 5, false);
        }
    }
    ctx->skipping++;
}

/** Start a draw cycle. Timing matches LilyGo-EPD47 ed047tc1.c exactly. */
static void i80_start_frame(void) {
    ESP_LOGI(TAG, "start_frame: waiting for bus...");
    while (i80_is_busy() || rmt_busy()) {}
    ESP_LOGI(TAG, "start_frame: bus ready");

    const EpdBoardDefinition* epd_board = epd_current_board();
    epd_ctrl_state_t* ctrl_state = epd_ctrl_state();
    epd_ctrl_state_t mask = NoChangeState;

    ctrl_state->ep_mode = true;
    mask.ep_mode = true;
    epd_board->set_ctrl(ctrl_state, &mask);

    pulse_ckv_us(1, 1, true);

    // This is very timing-sensitive! (from LilyGo-EPD47 ed047tc1.c)
    mask = NoChangeState;
    ctrl_state->ep_stv = false;
    mask.ep_stv = true;
    epd_board->set_ctrl(ctrl_state, &mask);
    // 1µs pause before CKV strobe (LilyGo-EPD47: busy_delay(240))
    epd_busy_delay(240);
    // Short 10µs CKV strobe (LilyGo-EPD47 uses 10,10 not 1000,100)
    pulse_ckv_us(10, 10, false);
    mask = NoChangeState;
    ctrl_state->ep_stv = true;
    mask.ep_stv = true;
    epd_board->set_ctrl(ctrl_state, &mask);
    // Wait for CKV strobe to complete, then one low period
    pulse_ckv_us(0, 10, true);

    mask = NoChangeState;
    ctrl_state->ep_output_enable = true;
    mask.ep_output_enable = true;
    epd_board->set_ctrl(ctrl_state, &mask);

    pulse_ckv_us(1, 1, true);
    ESP_LOGI(TAG, "start_frame: done (OE=1)");
}

/** End a draw cycle. Sequence matches LilyGo-EPD47 ed047tc1.c exactly. */
static void i80_end_frame(void) {
    ESP_LOGI(TAG, "end_frame: start (dma_count=%u)", (unsigned)i80_get_transfer_count());
    const EpdBoardDefinition* epd_board = epd_current_board();
    epd_ctrl_state_t* ctrl_state = epd_ctrl_state();
    epd_ctrl_state_t mask = NoChangeState;

    ctrl_state->ep_output_enable = false;
    mask.ep_output_enable = true;
    epd_board->set_ctrl(ctrl_state, &mask);

    mask = NoChangeState;
    ctrl_state->ep_mode = false;
    mask.ep_mode = true;
    epd_board->set_ctrl(ctrl_state, &mask);

    pulse_ckv_us(1, 1, true);
    pulse_ckv_us(1, 1, true);
    ESP_LOGI(TAG, "end_frame: done");
}

void i80_do_update(RenderContext_t* ctx) {
    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        prepare_context_for_next_frame(ctx);

        // start both feeder tasks
        xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
        xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);

        // wait until transmission is done
        xSemaphoreTake(ctx->frame_done, portMAX_DELAY);

        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            xSemaphoreTake(ctx->feed_done_smphr[i], portMAX_DELAY);
        }

        ctx->current_frame++;

        // make the watchdog happy
        if (k % 10 == 0) {
            vTaskDelay(0);
        }
    }
}

void IRAM_ATTR epd_push_pixels_i80(RenderContext_t* ctx, EpdRect area, short time, int color) {
    int line_bytes = ctx->display_width / 4;
    uint8_t row[line_bytes];
    memset(row, 0, line_bytes);

    const uint8_t color_choice[4] = { DARK_BYTE, CLEAR_BYTE, 0x00, 0xFF };
    for (uint32_t i = 0; i < area.width; i++) {
        uint32_t position = i + area.x % 4;
        uint8_t mask = color_choice[color] & (0b00000011 << (2 * (position % 4)));
        row[area.x / 4 + position / 4] |= mask;
    }
    // NOTE: no reorder_line_buffer here — i80 LCD_CAM sends bytes in memory
    // order (no I2S byte-swap), so no compensation is needed.

    i80_start_frame();

    for (int i = 0; i < ctx->display_height; i++) {
        if (i < area.y) {
            i80_skip_row(ctx, time);
        } else if (i == area.y) {
            i80_switch_buffer();
            memcpy((void*)i80_get_current_buffer(), row, line_bytes);
            i80_switch_buffer();
            memcpy((void*)i80_get_current_buffer(), row, line_bytes);

            i80_write_row(ctx, time * 10);
        } else if (i >= area.y + area.height) {
            i80_skip_row(ctx, time);
        } else {
            i80_write_row(ctx, time * 10);
        }
    }
    // Pipeline: latch out the last row.
    i80_write_row(ctx, time * 10);

    i80_end_frame();
}

void IRAM_ATTR i80_output_frame(RenderContext_t* ctx, int thread_id) {
    uint8_t* line_buf = ctx->feed_line_buffers[thread_id];

    ctx->skipping = 0;
    EpdRect area = ctx->area;
    int frame_time = ctx->frame_time;

    ESP_LOGI(TAG, "output_frame[%d]: start (h=%d, frame_time=%d)", thread_id, ctx->display_height, frame_time);
    i80_start_frame();
    for (int i = 0; i < ctx->display_height; i++) {
        if (i % 100 == 0) {
            ESP_LOGI(TAG, "output_frame[%d]: row %d/%d (dma_done=%u)", thread_id, i, ctx->display_height, (unsigned)i80_get_transfer_count());
        }
        LineQueue_t* lq = &ctx->line_queues[0];

        memset(line_buf, 0, ctx->display_width);
        while (lq_read(lq, line_buf) < 0) {}

        ctx->lines_consumed += 1;

        if (ctx->drawn_lines != NULL && !ctx->drawn_lines[i - area.y]) {
            i80_skip_row(ctx, frame_time);
            continue;
        }

        ctx->lut_lookup_func(
            (uint32_t*)line_buf,
            (uint8_t*)i80_get_current_buffer(),
            ctx->conversion_lut,
            ctx->display_width
        );

        epd_apply_line_mask(i80_get_current_buffer(), ctx->line_mask, ctx->display_width / 4);

        // No reorder_line_buffer — i80 sends in memory order, no I2S compensation needed.
        i80_write_row(ctx, frame_time);
    }
    if (!ctx->skipping) {
        // Pipeline: latch out the last row.
        i80_write_row(ctx, frame_time);
    }
    i80_end_frame();

    xSemaphoreGive(ctx->feed_done_smphr[thread_id]);
    xSemaphoreGive(ctx->frame_done);
}

static inline int min(int x, int y) { return x < y ? x : y; }
static inline int max(int x, int y) { return x > y ? x : y; }

void IRAM_ATTR i80_fetch_frame_data(RenderContext_t* ctx, int thread_id) {
    uint8_t* input_line = ctx->feed_line_buffers[thread_id];

    memset(input_line, 0x00, ctx->display_width);

    LineQueue_t* lq = &ctx->line_queues[thread_id];

    EpdRect area = ctx->area;

    int min_y, max_y, bytes_per_line, pixels_per_byte;
    const uint8_t* ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &pixels_per_byte);

    const EpdRect crop_to = ctx->crop_to;
    const bool horizontally_cropped = !(crop_to.x == 0 && crop_to.width == area.width);
    int crop_x = (horizontally_cropped ? crop_to.x : 0);
    int crop_w = (horizontally_cropped ? crop_to.width : 0);

    int line_start_x = area.x + (horizontally_cropped ? crop_to.x : 0);
    int line_end_x = line_start_x + (horizontally_cropped ? crop_to.width : area.width);
    line_start_x = min(max(line_start_x, 0), ctx->display_width);
    line_end_x = min(max(line_end_x, 0), ctx->display_width);

    int l = 0;
    while (l = atomic_fetch_add(&ctx->lines_prepared, 1), l < ctx->display_height) {
        ctx->line_threads[l] = thread_id;

        if (l < min_y || l >= max_y
            || (ctx->drawn_lines != NULL && !ctx->drawn_lines[l - area.y])) {
            uint8_t* buf = NULL;
            while (buf == NULL)
                buf = lq_current(lq);
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            continue;
        }

        uint32_t* lp = (uint32_t*)input_line;
        bool shifted = false;
        const uint8_t* ptr = ptr_start + bytes_per_line * (l - min_y);

        if (area.width == ctx->display_width && area.x == 0 && !ctx->error) {
            lp = (uint32_t*)ptr;
        } else if (!ctx->error) {
            uint8_t* buf_start = (uint8_t*)input_line;
            uint32_t line_bytes = bytes_per_line;

            int min_x = area.x + crop_x;
            if (min_x >= 0) {
                buf_start += min_x / pixels_per_byte;
            } else {
                line_bytes += min_x / pixels_per_byte;
            }
            line_bytes = min(
                line_bytes,
                ctx->display_width / pixels_per_byte - (uint32_t)(buf_start - input_line)
            );

            memcpy(buf_start, ptr, line_bytes);

            int cropped_width = (horizontally_cropped ? crop_w : area.width);
            if (pixels_per_byte == 2) {
                if (cropped_width % 2 == 1
                    && min_x / 2 + cropped_width / 2 + 1 < ctx->display_width) {
                    *(buf_start + line_bytes - 1) |= 0xF0;
                }
                if (area.x % 2 == 1 && !(crop_x % 2 == 1) && min_x < ctx->display_width) {
                    shifted = true;
                    uint32_t remaining
                        = (uint32_t)input_line + ctx->display_width / 2 - (uint32_t)buf_start;
                    uint32_t to_shift = min(line_bytes + 1, remaining);
                    nibble_shift_buffer_right(buf_start, to_shift);
                }
            } else if (pixels_per_byte == 8) {
                if (cropped_width % 8 != 0 && bytes_per_line + 1 < ctx->display_width) {
                    uint8_t mask = 0;
                    for (int s = 0; s < cropped_width % 8; s++) {
                        mask = (mask << 1) | 1;
                    }
                    *(buf_start + line_bytes - 1) |= ~mask;
                }
                if (min_x % 8 != 0 && min_x < ctx->display_width) {
                    shifted = true;
                    uint32_t remaining
                        = (uint32_t)input_line + ctx->display_width / 8 - (uint32_t)buf_start;
                    uint32_t to_shift = min(line_bytes + 1, remaining);
                    bit_shift_buffer_right(buf_start, to_shift, min_x % 8);
                }
            }
            lp = (uint32_t*)input_line;
        }

        uint8_t* buf = NULL;
        while (buf == NULL)
            buf = lq_current(lq);

        memcpy(buf, lp, lq->element_size);
        lq_commit(lq);

        if (shifted) {
            memset(input_line, 255, ctx->display_width / pixels_per_byte);
        }
    }
}

void i80_deinit(void) {
    rmt_pulse_deinit();
    i80_bus_deinit();
}

#endif  // RENDER_METHOD_I80
