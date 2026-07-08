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
};

/* Both return -ENODEV before the processor instance initializes. */
int flask_scroll_params_get(struct flask_scroll_params *out);
int flask_scroll_params_set(const struct flask_scroll_params *in);
