/*
 * Runtime API for the flask_smooth input processor.
 *
 * Integer port of drashna's pointing_device_smoothing (the QMK Flask
 * families' EMA, channel 0x13), same wire: enabled 0x01, factor 0x02 x100
 * (100 = raw passthrough, lower = heavier smoothing, more lag), reset
 * timeout 0x03 ms.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct flask_smooth_params {
    bool enabled;
    uint16_t factor_x100; /* EMA weight of the new sample; clamped 1..100 */
    uint16_t timeout_ms;  /* pause that resets the EMA; clamped 0..1000 */
};

/* Both return -ENODEV before the processor instance initializes. */
int flask_smooth_params_get(struct flask_smooth_params *out);
int flask_smooth_params_set(const struct flask_smooth_params *in);
