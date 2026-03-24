#pragma once

#include "../output_common/render_context.h"
#include "epdiy.h"

/**
 * Lighten / darken pixels using the i80 driving method.
 */
void epd_push_pixels_i80(RenderContext_t* ctx, EpdRect area, short time, int color);

/**
 * Do a full update cycle with a configured context.
 */
void i80_do_update(RenderContext_t* ctx);

/**
 * Worker to fetch framebuffer data and write into a queue for processing.
 */
void i80_fetch_frame_data(RenderContext_t* ctx, int thread_id);

/**
 * Worker to output frame data to the display.
 */
void i80_output_frame(RenderContext_t* ctx, int thread_id);

/**
 * Deinitialize the i80 peripheral and RMT.
 */
void i80_deinit(void);
