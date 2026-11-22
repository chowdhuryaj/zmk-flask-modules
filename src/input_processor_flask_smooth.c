/*
 * Flask pointer smoothing input processor.
 *
 * Integer port of drashna's pointing_device_smoothing (GPL-2.0+ in QMK; this
 * is a reimplementation of the same three-line EMA, not a copy): per axis,
 *   ema = f * in + (1 - f) * ema;  out = round(ema + carry)
 * with the rounding carry kept so slow motion is not quantized away, the
 * carry dropped on a direction reversal, and everything reset after a pause.
 *
 * ZMK divergence: QMK smooths whole reports, including all-zero ones; ZMK
 * input processors see one axis event at a time and the mouse host skips
 * zero axes, so an axis that stops just stops reporting (its EMA tail is
 * not emitted, same as QMK, which returns early on zero reports). Q8 fixed
 * point.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_smooth

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>

#include <flask_smooth/flask_smooth.h>

struct smooth_axis {
    int32_t ema_q8;   /* average, x256 */
    int32_t carry_q8; /* rounding carry, x256 */
};

struct flask_smooth_data {
    struct k_spinlock lock;
    struct flask_smooth_params live;
    struct smooth_axis axis[2];
    int64_t last_ms;
};

static struct flask_smooth_data *flask_smooth_singleton;

static void clamp_params(struct flask_smooth_params *p) {
    p->factor_x100 = CLAMP(p->factor_x100, 1, 100);
    p->timeout_ms = MIN(p->timeout_ms, 1000);
}

int flask_smooth_params_get(struct flask_smooth_params *out) {
    struct flask_smooth_data *data = flask_smooth_singleton;

    if (data == NULL || out == NULL) {
        return -ENODEV;
    }
    K_SPINLOCK(&data->lock) { *out = data->live; }
    return 0;
}

int flask_smooth_params_set(const struct flask_smooth_params *in) {
    struct flask_smooth_data *data = flask_smooth_singleton;

    if (data == NULL || in == NULL) {
        return -ENODEV;
    }

    struct flask_smooth_params p = *in;

    clamp_params(&p);
    K_SPINLOCK(&data->lock) {
        data->live = p;
        memset(data->axis, 0, sizeof(data->axis)); /* stale average would lurch */
    }
    return 0;
}

static int flask_smooth_handle_event(const struct device *dev, struct input_event *event,
                                     uint32_t param1, uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    struct flask_smooth_data *data = dev->data;

    /* Zero values too: a drag-scroll layer override zeroes X/Y upstream, and
     * a zero must not drag the average toward rest. */
    if (event->type != INPUT_EV_REL || event->value == 0 ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    K_SPINLOCK(&data->lock) {
        if (!data->live.enabled) {
            K_SPINLOCK_BREAK;
        }

        int64_t now = k_uptime_get();

        if (now - data->last_ms > data->live.timeout_ms) {
            memset(data->axis, 0, sizeof(data->axis));
        }
        data->last_ms = now;

        struct smooth_axis *a = &data->axis[event->code == INPUT_REL_Y];
        int32_t in_q8 = event->value * 256;
        int32_t f = data->live.factor_x100;

        if ((int64_t)in_q8 * a->carry_q8 < 0) {
            a->carry_q8 = 0; /* reversal: follow the hand at once */
        }
        a->ema_q8 = (f * in_q8 + (100 - f) * a->ema_q8) / 100;

        int32_t v = a->ema_q8 + a->carry_q8;
        int32_t out = (v >= 0 ? v + 128 : v - 128) / 256;

        a->carry_q8 = v - out * 256;
        event->value = out;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

struct flask_smooth_dt_config {
    struct flask_smooth_params defaults;
};

static int flask_smooth_init(const struct device *dev) {
    struct flask_smooth_data *data = dev->data;
    const struct flask_smooth_dt_config *cfg = dev->config;

    data->live = cfg->defaults;
    clamp_params(&data->live);
    if (flask_smooth_singleton == NULL) {
        flask_smooth_singleton = data;
    }
    return 0;
}

static const struct zmk_input_processor_driver_api flask_smooth_api = {
    .handle_event = flask_smooth_handle_event,
};

#define FLASK_SMOOTH_INST(n)                                                                       \
    static struct flask_smooth_data flask_smooth_data_##n;                                         \
    static const struct flask_smooth_dt_config flask_smooth_config_##n = {                         \
        .defaults =                                                                                \
            {                                                                                      \
                .enabled = !DT_INST_PROP(n, start_disabled),                                       \
                .factor_x100 = DT_INST_PROP(n, factor),                                            \
                .timeout_ms = DT_INST_PROP(n, timeout_ms),                                         \
            },                                                                                     \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, flask_smooth_init, NULL, &flask_smooth_data_##n,                      \
                          &flask_smooth_config_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,       \
                          &flask_smooth_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_SMOOTH_INST)
