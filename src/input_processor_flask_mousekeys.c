/*
 * Flask runtime mouse-key speed + ramp (channel 0x2D). See
 * include/flask_mousekeys/flask_mousekeys.h for the model.
 *
 * Contract, same as flask_scrollscale: scales IN PLACE and returns CONTINUE
 * (no swallow, no re-emit, no work item, no added latency). The remainder is
 * kept in the instance's own data (int32, per axis) rather than the
 * listener's int16 slot: the math is in 1/1,000,000 units (percent x ramp
 * fraction x10000), which an int16 cannot hold.
 *
 * Movement start: ZMK's two-axis behavior reports on every tick (16 ms) it
 * has a non-zero integer step, so a quiet gap longer than gap-ms means the
 * key was released and this event begins a new movement. Event structs carry
 * no timestamp, so time is k_uptime_get() at process time. The first event
 * is treated as one tick (16 ms) into the movement, like the stock ramp,
 * whose first tick also lands one period after the press.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_mousekeys

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <drivers/input_processor.h>

#include <flask_mousekeys/flask_mousekeys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define FIRST_TICK_MS 16
#define RAMP_ONE 10000 /* ramp fraction fixed point */

/* Module-global, atomic per field: read on every event from the input path,
 * written from the Flask HID handler. Boot values come from the instances'
 * DT props (init), then settings restore overrides them. */
static atomic_t speed_pct[FLASK_MOUSEKEYS_KINDS] = {ATOMIC_INIT(100), ATOMIC_INIT(100)};
static atomic_t ramp_ms[FLASK_MOUSEKEYS_KINDS];
static atomic_t exponent[FLASK_MOUSEKEYS_KINDS];

int flask_mousekeys_params_get(uint8_t kind, struct flask_mousekeys_params *out) {
    if (out == NULL || kind >= FLASK_MOUSEKEYS_KINDS) {
        return -EINVAL;
    }
    out->speed_pct = (uint16_t)atomic_get(&speed_pct[kind]);
    out->ramp_ms = (uint16_t)atomic_get(&ramp_ms[kind]);
    out->exponent = (uint16_t)atomic_get(&exponent[kind]);
    return 0;
}

int flask_mousekeys_params_set(uint8_t kind, const struct flask_mousekeys_params *in) {
    if (in == NULL || kind >= FLASK_MOUSEKEYS_KINDS) {
        return -EINVAL;
    }
    atomic_set(&speed_pct[kind],
               CLAMP(in->speed_pct, FLASK_MOUSEKEYS_SPEED_MIN, FLASK_MOUSEKEYS_SPEED_MAX));
    atomic_set(&ramp_ms[kind], MIN(in->ramp_ms, FLASK_MOUSEKEYS_RAMP_MAX));
    atomic_set(&exponent[kind], MIN(in->exponent, FLASK_MOUSEKEYS_EXP_MAX));
    return 0;
}

struct flask_mousekeys_config {
    uint8_t kind;
    uint16_t code_a, code_b; /* X/Y or WHEEL/HWHEEL */
    uint16_t gap_ms;
    struct flask_mousekeys_params boot;
};

struct flask_mousekeys_data {
    int64_t start_ms;
    int64_t last_ms;
    int32_t remainder[2];
};

/* min(1, (t/ramp)^exp) in RAMP_ONE units. t < ramp <= 3000 here, so the
 * products stay well inside uint32. */
static uint32_t ramp_fraction(uint32_t t_ms, uint32_t ramp, uint32_t exp) {
    if (ramp == 0 || exp == 0 || t_ms >= ramp) {
        return RAMP_ONE;
    }
    uint32_t f = t_ms * RAMP_ONE / ramp;
    uint32_t r = RAMP_ONE;
    for (uint32_t i = 0; i < exp; i++) {
        r = r * f / RAMP_ONE;
    }
    return r;
}

static int flask_mousekeys_handle_event(const struct device *dev, struct input_event *event,
                                        uint32_t param1, uint32_t param2,
                                        struct zmk_input_processor_state *state) {
    const struct flask_mousekeys_config *cfg = dev->config;
    struct flask_mousekeys_data *data = dev->data;
    int axis;

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    if (event->code == cfg->code_a) {
        axis = 0;
    } else if (event->code == cfg->code_b) {
        axis = 1;
    } else {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int64_t now = k_uptime_get();
    if (data->last_ms == 0 || now - data->last_ms > cfg->gap_ms) {
        data->start_ms = now - FIRST_TICK_MS;
        data->remainder[0] = data->remainder[1] = 0;
    }
    data->last_ms = now;

    uint32_t t = (uint32_t)MIN(now - data->start_ms, (int64_t)UINT16_MAX);
    uint32_t ramp = ramp_fraction(t, atomic_get(&ramp_ms[cfg->kind]),
                                  atomic_get(&exponent[cfg->kind]));
    int64_t num = (int64_t)event->value * atomic_get(&speed_pct[cfg->kind]) * ramp +
                  data->remainder[axis];
    int64_t scaled = num / (100 * RAMP_ONE);

    data->remainder[axis] = (int32_t)(num - scaled * (100 * RAMP_ONE));
    event->value = (int32_t)CLAMP(scaled, INT16_MIN, INT16_MAX);

    return ZMK_INPUT_PROC_CONTINUE;
}

static int flask_mousekeys_init(const struct device *dev) {
    const struct flask_mousekeys_config *cfg = dev->config;

    return flask_mousekeys_params_set(cfg->kind, &cfg->boot);
}

static const struct zmk_input_processor_driver_api flask_mousekeys_api = {
    .handle_event = flask_mousekeys_handle_event,
};

#define FLASK_MOUSEKEYS_INST(n)                                                                    \
    static struct flask_mousekeys_data flask_mousekeys_data_##n;                                   \
    static const struct flask_mousekeys_config flask_mousekeys_config_##n = {                      \
        .kind = DT_INST_PROP(n, scroll) ? FLASK_MOUSEKEYS_SCROLL : FLASK_MOUSEKEYS_POINTER,        \
        .code_a = DT_INST_PROP(n, scroll) ? INPUT_REL_WHEEL : INPUT_REL_X,                         \
        .code_b = DT_INST_PROP(n, scroll) ? INPUT_REL_HWHEEL : INPUT_REL_Y,                        \
        .gap_ms = DT_INST_PROP(n, gap_ms),                                                         \
        .boot =                                                                                    \
            {                                                                                      \
                .speed_pct = DT_INST_PROP(n, speed_pct),                                           \
                .ramp_ms = DT_INST_PROP(n, ramp_ms),                                               \
                .exponent = DT_INST_PROP(n, exponent),                                             \
            },                                                                                     \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, flask_mousekeys_init, NULL, &flask_mousekeys_data_##n,                \
                          &flask_mousekeys_config_##n, POST_KERNEL,                                \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &flask_mousekeys_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_MOUSEKEYS_INST)
