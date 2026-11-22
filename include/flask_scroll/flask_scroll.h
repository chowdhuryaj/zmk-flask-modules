/*
 * Runtime-tuning API for the flask_scroll input processor.
 *
 * Values start from the devicetree config and can be changed live (e.g. by
 * the flask_proto raw-HID channel). Setters clamp to sane ranges and reset
 * partial-notch remainders so a divisor change takes effect cleanly.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct flask_scroll_params {
    int32_t divisor_x;
    int32_t divisor_y;
    uint16_t interval_ms;
    int32_t max_notches;
    bool invert_x;
    bool invert_y;
    /* Vertical priority (0 = off, both axes scroll at once as before).
     * Horizontal wins only while recent sideways counts exceed this percent
     * of recent vertical counts (300 = 3x); otherwise X is dropped. While
     * horizontal wins, Y is dropped. Clamped 0..1000. */
    uint16_t h_bias_pct;
};

/* Both return -ENODEV before the processor instance initializes. */
int flask_scroll_params_get(struct flask_scroll_params *out);
int flask_scroll_params_set(const struct flask_scroll_params *in);
