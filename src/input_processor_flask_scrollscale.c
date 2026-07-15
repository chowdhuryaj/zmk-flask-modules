/*
 * Flask runtime scroll-speed scaler.
 *
 * A divisor-for-divisor copy of ZMK's stock scaler (app/src/pointing/
 * input_processor_scaler.c) with one change: the divisor is the keymap's
 * compiled param2 scaled by a live speed percent.
 *
 * Why a copy instead of the rate-capped flask_scroll module: flask_scroll
 * accumulates notches and dumps them on a timer, which is a different FEEL,
 * and AJ benched it off the Imprint on 2026-07-08 in favour of the stock
 * chain. Matching the stock math exactly means speed 100 is bit-identical to
 * `&zip_x_scaler 1 16` / `&zip_y_scaler 1 12` — the knob adds a range without
 * touching the default.
 *
 * Same contract as stock, deliberately:
 *   - scales IN PLACE and returns CONTINUE (no swallow, no re-emit, no
 *     second listener, no work item, no added latency)
 *   - carries the truncation remainder through state->remainder, so slow
 *     rolls keep their sub-notch precision; the listener allocates one
 *     remainder slot per chain entry per axis, which is why the axes are
 *     two instances rather than one node with two divisors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_scrollscale

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <drivers/input_processor.h>

#include <flask_scrollscale/flask_scrollscale.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Module-global, atomic: read on every scroll event from the input dispatch
 * context and written from the Flask HID handler. A single word needs no
 * lock, and taking one in the dispatch path is exactly what the re-emit
 * deadlock rule warns about. */
static atomic_t speed_pct = ATOMIC_INIT(FLASK_SCROLLSCALE_SPEED_DEFAULT);

int flask_scrollscale_params_get(struct flask_scrollscale_params *out) {
    if (out == NULL) {
        return -EINVAL;
    }
    out->speed_pct = (uint16_t)atomic_get(&speed_pct);
    return 0;
}

int flask_scrollscale_params_set(const struct flask_scrollscale_params *in) {
    if (in == NULL) {
        return -EINVAL;
    }
    atomic_set(&speed_pct, CLAMP(in->speed_pct, FLASK_SCROLLSCALE_SPEED_MIN,
                                 FLASK_SCROLLSCALE_SPEED_MAX));
    return 0;
}

struct flask_scrollscale_config {
    uint8_t type;
    size_t codes_len;
    uint16_t codes[];
};

static int flask_scrollscale_handle_event(const struct device *dev, struct input_event *event,
                                          uint32_t param1, uint32_t param2,
                                          struct zmk_input_processor_state *state) {
    const struct flask_scrollscale_config *cfg = dev->config;

    if (event->type != cfg->type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    bool match = false;
    for (int i = 0; i < cfg->codes_len; i++) {
        if (cfg->codes[i] == event->code) {
            match = true;
            break;
        }
    }
    if (!match) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Live divisor = compiled divisor / speed. Faster speed, smaller
     * divisor. Clamped to at least 1 (speed_pct can never make it 0, but a
     * divisor of 0 would fault) and to int16 range, since the scaler math
     * below is 16-bit and a pathological keymap divisor at min speed would
     * otherwise overflow the cast. */
    uint32_t speed = (uint32_t)atomic_get(&speed_pct);
    uint32_t div = CLAMP((param2 * (uint32_t)FLASK_SCROLLSCALE_SPEED_DEFAULT) / speed, 1U,
                         (uint32_t)INT16_MAX);

    /* Verbatim from the stock scaler, so the feel cannot drift. */
    int16_t value_mul = event->value * (int16_t)param1;

    if (state && state->remainder) {
        value_mul += *state->remainder;
    }

    int16_t scaled = value_mul / (int16_t)div;

    if (state && state->remainder) {
        *state->remainder = value_mul - (scaled * (int16_t)div);
    }

    event->value = scaled;

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api flask_scrollscale_api = {
    .handle_event = flask_scrollscale_handle_event,
};

#define FLASK_SCROLLSCALE_INST(n)                                                                  \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, codes) > 0, "codes must name at least one event code");       \
    static const struct flask_scrollscale_config flask_scrollscale_config_##n = {                  \
        .type = DT_INST_PROP_OR(n, type, INPUT_EV_REL),                                            \
        .codes_len = DT_INST_PROP_LEN(n, codes),                                                   \
        .codes = DT_INST_PROP(n, codes),                                                           \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, &flask_scrollscale_config_##n, POST_KERNEL,         \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &flask_scrollscale_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_SCROLLSCALE_INST)
