/*
 * Runtime API for the flask_scrollscale input processor.
 *
 * One live scroll-speed knob (Flask protocol channel 0x29) for a scroll ball
 * whose chain is otherwise the stock ZMK one. ZMK's own zip_x_scaler /
 * zip_y_scaler carry their divisor as devicetree params — const config, so
 * scroll speed could only change by reflashing. This processor is a drop-in
 * replacement with identical math (see input_processor_flask_scrollscale.c);
 * the ONLY difference is that the compiled divisor is divided by a live
 * speed percent before use.
 *
 * speed_pct is a PERCENT OF THE KEYMAP'S COMPILED DIVISORS, not an absolute
 * rate: 100 means "exactly what the keymap says" (so the default is, by
 * construction, the feel that was already benched), 200 is twice as fast
 * (half the divisor), 50 is half as fast. That keeps a single knob honest
 * across axes with different base divisors — the Imprint's 16 (horizontal)
 * and 12 (vertical) keep their ratio at every speed.
 *
 * The speed is module-global rather than per-instance: the two instances in
 * a chain (one per axis) are the same knob, and a device has one scroll
 * ball. Instances differ only in which code they scale.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Percent of the compiled divisors. 100 = the keymap's values verbatim. */
#define FLASK_SCROLLSCALE_SPEED_MIN 25
#define FLASK_SCROLLSCALE_SPEED_MAX 400
#define FLASK_SCROLLSCALE_SPEED_DEFAULT 100

struct flask_scrollscale_params {
    uint16_t speed_pct; /* clamped MIN..MAX on set */
};

/* Never fail once the module is compiled in — the speed is module state, not
 * device state, so there is no "processor not ready" window. -EINVAL on NULL. */
int flask_scrollscale_params_get(struct flask_scrollscale_params *out);
int flask_scrollscale_params_set(const struct flask_scrollscale_params *in);
