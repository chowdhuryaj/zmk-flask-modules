/*
 * Flask rate-capped scroll input processor.
 *
 * Port of the Flask (QMK) drag-scroll conversion — see
 * svalboard-vial-qmk keymaps/flask/support_flask.c, scroll dump block.
 * Semantics preserved:
 *   - integer divisor turns ball counts into notches, remainder carried
 *     between events (truncating division, matching QMK add_to_axis)
 *   - notches accumulate and are dumped on a fixed interval, clamped to
 *     max-notches per axis per interval
 *   - surplus notches stay in the accumulator and drain on later intervals,
 *     so precision survives the cap, slow scroll consumers never see a
 *     flood, and a reversal cancels pending surplus before any output
 *     flips sign
 *
 * The processor swallows REL_X/REL_Y from the listener chain and re-emits
 * REL_HWHEEL/REL_WHEEL from its own device node on the dump timer; a second
 * input listener bound to this node carries the wheel events onward.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_scroll

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <drivers/input_processor.h>

#include <flask_scroll/flask_scroll.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct flask_scroll_data {
    const struct device *dev;
    struct k_work_delayable dump_work;
    struct k_spinlock lock;
    struct flask_scroll_params live; /* runtime-tunable copy of the DT config */
    int32_t rem_x, rem_y;            /* raw counts short of a whole notch */
    int32_t accum_h, accum_v;        /* whole notches awaiting emission */
    bool running;                    /* dump timer armed */
};

/* Singleton handle for the runtime-tuning API (first instance wins; Flask
 * has exactly one scroll ball). */
static struct flask_scroll_data *flask_scroll_singleton;

int flask_scroll_params_get(struct flask_scroll_params *out) {
    struct flask_scroll_data *data = flask_scroll_singleton;

    if (data == NULL || out == NULL) {
        return -ENODEV;
    }
    K_SPINLOCK(&data->lock) { *out = data->live; }
    return 0;
}

int flask_scroll_params_set(const struct flask_scroll_params *in) {
    struct flask_scroll_data *data = flask_scroll_singleton;

    if (data == NULL || in == NULL) {
        return -ENODEV;
    }

    struct flask_scroll_params p = *in;

    p.divisor_x = CLAMP(p.divisor_x, 1, 10000);
    p.divisor_y = CLAMP(p.divisor_y, 1, 10000);
    p.interval_ms = CLAMP(p.interval_ms, 1, 1000);
    p.max_notches = CLAMP(p.max_notches, 1, 127);

    K_SPINLOCK(&data->lock) {
        data->live = p;
        /* Fresh divisors mean stale partial-notch remainders — drop them. */
        data->rem_x = 0;
        data->rem_y = 0;
    }
    return 0;
}

static void flask_scroll_dump(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct flask_scroll_data *data = CONTAINER_OF(dwork, struct flask_scroll_data, dump_work);

    int32_t out_h, out_v;
    bool inv_x, inv_y;

    K_SPINLOCK(&data->lock) {
        out_h = CLAMP(data->accum_h, -data->live.max_notches, data->live.max_notches);
        out_v = CLAMP(data->accum_v, -data->live.max_notches, data->live.max_notches);
        data->accum_h -= out_h;
        data->accum_v -= out_v;
        inv_x = data->live.invert_x;
        inv_y = data->live.invert_y;
        data->running = (data->accum_h != 0 || data->accum_v != 0);
        if (data->running) {
            k_work_schedule(&data->dump_work, K_MSEC(data->live.interval_ms));
        }
    }

    if (inv_x) {
        out_h = -out_h;
    }
    if (inv_y) {
        out_v = -out_v;
    }

    if (out_h != 0) {
        input_report_rel(data->dev, INPUT_REL_HWHEEL, out_h, out_v == 0, K_MSEC(10));
    }
    if (out_v != 0) {
        input_report_rel(data->dev, INPUT_REL_WHEEL, out_v, true, K_MSEC(10));
    }
}

static int flask_scroll_handle_event(const struct device *dev, struct input_event *event,
                                     uint32_t param1, uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    struct flask_scroll_data *data = dev->data;

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    K_SPINLOCK(&data->lock) {
        if (event->code == INPUT_REL_X) {
            data->rem_x += event->value;
            int32_t notches = data->rem_x / data->live.divisor_x;
            data->rem_x -= notches * data->live.divisor_x;
            data->accum_h += notches;
        } else {
            data->rem_y += event->value;
            int32_t notches = data->rem_y / data->live.divisor_y;
            data->rem_y -= notches * data->live.divisor_y;
            data->accum_v += notches;
        }

        if (!data->running && (data->accum_h != 0 || data->accum_v != 0)) {
            data->running = true;
            k_work_schedule(&data->dump_work, K_MSEC(data->live.interval_ms));
        }
    }

    /* Swallow the motion event — wheel notches re-enter the pipeline from
     * this node via the dump timer. */
    event->value = 0;
    return ZMK_INPUT_PROC_STOP;
}

struct flask_scroll_dt_config {
    struct flask_scroll_params defaults;
};

static int flask_scroll_init(const struct device *dev) {
    struct flask_scroll_data *data = dev->data;
    const struct flask_scroll_dt_config *cfg = dev->config;

    data->dev = dev;
    data->live = cfg->defaults;
    k_work_init_delayable(&data->dump_work, flask_scroll_dump);
    if (flask_scroll_singleton == NULL) {
        flask_scroll_singleton = data;
    }
    return 0;
}

static const struct zmk_input_processor_driver_api flask_scroll_api = {
    .handle_event = flask_scroll_handle_event,
};

/* divisor-x / divisor-y of 0 fall back to the shared divisor */
#define FLASK_SCROLL_DIV(n, axis)                                                                  \
    (DT_INST_PROP(n, divisor_##axis) > 0 ? DT_INST_PROP(n, divisor_##axis)                         \
                                         : DT_INST_PROP(n, divisor))

#define FLASK_SCROLL_INST(n)                                                                       \
    BUILD_ASSERT(DT_INST_PROP(n, divisor) > 0, "divisor must be positive");                        \
    BUILD_ASSERT(DT_INST_PROP(n, interval_ms) > 0, "interval-ms must be positive");                \
    BUILD_ASSERT(DT_INST_PROP(n, max_notches) > 0, "max-notches must be positive");                \
    static struct flask_scroll_data flask_scroll_data_##n;                                         \
    static const struct flask_scroll_dt_config flask_scroll_config_##n = {                         \
        .defaults = {                                                                              \
            .divisor_x = FLASK_SCROLL_DIV(n, x),                                                   \
            .divisor_y = FLASK_SCROLL_DIV(n, y),                                                   \
            .interval_ms = DT_INST_PROP(n, interval_ms),                                           \
            .max_notches = DT_INST_PROP(n, max_notches),                                           \
            .invert_x = DT_INST_PROP(n, invert_x),                                                 \
            .invert_y = DT_INST_PROP(n, invert_y),                                                 \
        },                                                                                         \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, flask_scroll_init, NULL, &flask_scroll_data_##n,                      \
                          &flask_scroll_config_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,       \
                          &flask_scroll_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_SCROLL_INST)
