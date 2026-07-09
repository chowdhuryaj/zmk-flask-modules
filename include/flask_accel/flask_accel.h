/*
 * Runtime API for the flask_accel input processor.
 *
 * Port of Flask (QMK) pointer acceleration — drashna's pointing_device_accel
 * generalized sigmoid (pd_accel.c), the curve the whole Flask ecosystem
 * tunes over channel 0x10:
 *
 *   f(v) = 1 - (1 - limit) / (1 + e^(takeoff * (v - offset)))^(growth/takeoff)
 *
 * Params ride the wire x100 (fixed-point "percent" floats, offset signed),
 * exactly like the QMK families, so the same app card drives both lines.
 *
 * ZMK divergence note: QMK computes velocity from the euclidean distance of
 * a whole (x, y) report; ZMK input processors see one axis event at a time,
 * so velocity here is per-event |value| / dt-per-axis. Diagonal strokes read
 * a touch slower than QMK would see them — the curve params absorb it.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct flask_accel_params {
    bool enabled;
    uint16_t takeoff_x100; /* K: curve steepness; clamped 50..1000 (0.5..10) */
    uint16_t growth_x100;  /* G: growth rate; clamped 0..200 (0..2) */
    int16_t offset_x100;   /* S: sigmoid midpoint, SIGNED; clamped -1000..1000 */
    uint16_t limit_x100;   /* M: lower asymptote; clamped 0..100 (0..1) */
};

/* Both return -ENODEV before the processor instance initializes. */
int flask_accel_params_get(struct flask_accel_params *out);
int flask_accel_params_set(const struct flask_accel_params *in);
