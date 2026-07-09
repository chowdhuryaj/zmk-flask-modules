/*
 * Flask pointer-acceleration input processor.
 *
 * Port of the Flask (QMK) pd_accel module (itself drashna's
 * pointing_device_accel): a generalized sigmoid maps pointer velocity to a
 * scale factor in (limit, 1], applied in place to REL_X/REL_Y events —
 * slow, precise strokes stay 1:1 (or shrink toward `limit` when limit < 1
 * boosts fast ones relatively), fast flicks accelerate.
 *
 * Semantics preserved from pd_accel.c:
 *   - factor = 1 - (1 - M) / (1 + e^(K * (v - S)))^(G/K)
 *   - velocity normalized for sensor CPI (dpi-correction = 1000 / cpi)
 *   - quantization remainders carried per axis, dropped on direction flip
 *     or after a 200 ms pause (follows the hand, no stale drift)
 *
 * Unlike flask_scroll/flask_autoscroll this processor never swallows or
 * re-emits — it scales event values in the chain and CONTINUEs, so it slots
 * anywhere in the cursor listener chain (after gestures, before scalers).
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_accel

#include <math.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <drivers/input_processor.h>

#include <flask_accel/flask_accel.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define CARRY_TIMEOUT_MS 200 /* QMK ROUNDING_CARRY_TIMEOUT_MS */

struct flask_accel_axis {
    int64_t last_ms; /* last event on this axis */
    float carry;     /* quantization remainder */
};

struct flask_accel_data {
    struct k_spinlock lock;
    struct flask_accel_params live; /* runtime-tunable copy of the DT config */
    float dpi_factor;               /* 1000 / cpi, fixed at init */
    struct flask_accel_axis x, y;
};

/* Singleton handle for the runtime-tuning API (first instance wins; the
 * Imprint has exactly one cursor ball). */
static struct flask_accel_data *flask_accel_singleton;

int flask_accel_params_get(struct flask_accel_params *out) {
    struct flask_accel_data *data = flask_accel_singleton;

    if (data == NULL || out == NULL) {
        return -ENODEV;
    }
    K_SPINLOCK(&data->lock) { *out = data->live; }
    return 0;
}

int flask_accel_params_set(const struct flask_accel_params *in) {
    struct flask_accel_data *data = flask_accel_singleton;

    if (data == NULL || in == NULL) {
        return -ENODEV;
    }

    struct flask_accel_params p = *in;

    /* Same bounds as pd_accel.h (x100 on the wire). */
    p.takeoff_x100 = CLAMP(p.takeoff_x100, 50, 1000);
    p.growth_x100 = MIN(p.growth_x100, 200);
    p.offset_x100 = CLAMP(p.offset_x100, -1000, 1000);
    p.limit_x100 = MIN(p.limit_x100, 100);

    K_SPINLOCK(&data->lock) {
        data->live = p;
        data->x.carry = 0.0f;
        data->y.carry = 0.0f;
    }
    return 0;
}

static int flask_accel_handle_event(const struct device *dev, struct input_event *event,
                                    uint32_t param1, uint32_t param2,
                                    struct zmk_input_processor_state *state) {
    struct flask_accel_data *data = dev->data;

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) || event->value == 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct flask_accel_params p;
    struct flask_accel_axis *axis;
    float carry;
    int64_t now = k_uptime_get();
    int64_t last;

    K_SPINLOCK(&data->lock) {
        p = data->live;
        axis = (event->code == INPUT_REL_X) ? &data->x : &data->y;
        last = axis->last_ms;
        axis->last_ms = now;
        carry = axis->carry;
    }

    if (!p.enabled) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int64_t dt = now - last;

    if (dt > CARRY_TIMEOUT_MS || dt <= 0) {
        carry = 0.0f;
        dt = MAX(dt, 1);
    }
    /* Reset carry when the pointer swaps direction, to follow the hand. */
    if ((float)event->value * carry < 0.0f) {
        carry = 0.0f;
    }

    const float velocity = data->dpi_factor * ((float)abs(event->value) / (float)dt);
    const float k = (float)p.takeoff_x100 / 100.0f;
    const float g = (float)p.growth_x100 / 100.0f;
    const float s = (float)p.offset_x100 / 100.0f;
    const float m = (float)p.limit_x100 / 100.0f;
    /* Generalized sigmoid — see pd_accel.c / desmos.com/calculator/k9vr0y2gev */
    const float factor = 1.0f - (1.0f - m) / powf(1.0f + expf(k * (velocity - s)), g / k);

    const float scaled = carry + factor * (float)event->value;
    int32_t out = (int32_t)scaled;

    K_SPINLOCK(&data->lock) { axis->carry = scaled - (float)out; }

    /* A nonzero input that rounds to zero still syncs downstream (the carry
     * keeps the missing fraction). */
    event->value = CLAMP(out, INT16_MIN, INT16_MAX);
    return ZMK_INPUT_PROC_CONTINUE;
}

struct flask_accel_dt_config {
    struct flask_accel_params defaults;
    uint16_t cpi;
};

static int flask_accel_init(const struct device *dev) {
    struct flask_accel_data *data = dev->data;
    const struct flask_accel_dt_config *cfg = dev->config;

    data->live = cfg->defaults;
    data->dpi_factor = 1000.0f / (float)MAX(cfg->cpi, 1);
    if (flask_accel_singleton == NULL) {
        flask_accel_singleton = data;
    }
    return 0;
}

static const struct zmk_input_processor_driver_api flask_accel_api = {
    .handle_event = flask_accel_handle_event,
};

#define FLASK_ACCEL_INST(n)                                                                        \
    static struct flask_accel_data flask_accel_data_##n;                                           \
    static const struct flask_accel_dt_config flask_accel_config_##n = {                           \
        .defaults = {                                                                              \
            .enabled = !DT_INST_PROP(n, start_disabled),                                           \
            .takeoff_x100 = DT_INST_PROP(n, takeoff),                                              \
            .growth_x100 = DT_INST_PROP(n, growth_rate),                                           \
            .offset_x100 = DT_INST_PROP(n, offset),                                                \
            .limit_x100 = DT_INST_PROP(n, limit),                                                  \
        },                                                                                         \
        .cpi = DT_INST_PROP(n, cpi),                                                               \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, flask_accel_init, NULL, &flask_accel_data_##n,                        \
                          &flask_accel_config_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,        \
                          &flask_accel_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_ACCEL_INST)
