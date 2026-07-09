/*
 * Flask scroll axis snap/lock input processor.
 *
 * Runtime-tunable port of kot149/zmk-scroll-snap v1 (MIT, engine logic
 * preserved): a ring buffer of recent REL_WHEEL/REL_HWHEEL magnitudes
 * decides the dominant axis; the minority axis is zeroed ("snap") and the
 * decision can be held for a duration and/or event count ("lock"). Sits at
 * the END of the scroll chain, after the xy→wheel mapper.
 *
 * Differences from the DT-const original:
 *   - every knob lives in a runtime params struct (Flask channel 0x26)
 *   - the x/y/diagonal threshold fractions collapse into one symmetric
 *     dominance percent (the original shipped symmetric defaults; the
 *     diagonal pass-through band falls out of the same number)
 *   - remainders always accumulate while the window fills (the original's
 *     track_remainders=false dropped sub-window motion on the floor)
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_scrollsnap

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <drivers/input_processor.h>

#include <flask_scrollsnap/flask_scrollsnap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SNAP_MAX_SAMPLES 32

#define DIR_NONE 0
#define DIR_X 1
#define DIR_Y 2

struct snap_sample {
    int32_t dx, dy;
};

struct flask_scrollsnap_data {
    struct k_spinlock lock;
    struct flask_scrollsnap_params live;

    struct snap_sample samples[SNAP_MAX_SAMPLES];
    uint16_t head;
    uint16_t sample_count;
    struct snap_sample sum; /* window |dx| / |dy| sums */
    struct snap_sample rem; /* signed motion awaiting a decision */

    int64_t last_event_ms;
    uint8_t lock_dir;
    uint16_t lock_events_left;
    int64_t lock_expires_ms;
};

static struct flask_scrollsnap_data *flask_scrollsnap_singleton;

static void snap_reset(struct flask_scrollsnap_data *data) {
    data->head = 0;
    data->sample_count = 0;
    data->sum.dx = 0;
    data->sum.dy = 0;
    data->rem.dx = 0;
    data->rem.dy = 0;
    data->lock_dir = DIR_NONE;
    data->lock_events_left = 0;
    data->lock_expires_ms = 0;
}

int flask_scrollsnap_params_get(struct flask_scrollsnap_params *out) {
    struct flask_scrollsnap_data *data = flask_scrollsnap_singleton;

    if (data == NULL || out == NULL) {
        return -ENODEV;
    }
    K_SPINLOCK(&data->lock) { *out = data->live; }
    return 0;
}

int flask_scrollsnap_params_set(const struct flask_scrollsnap_params *in) {
    struct flask_scrollsnap_data *data = flask_scrollsnap_singleton;

    if (data == NULL || in == NULL) {
        return -ENODEV;
    }

    struct flask_scrollsnap_params p = *in;

    p.threshold_pct = CLAMP(p.threshold_pct, 50, 99);
    p.samples = CLAMP(p.samples, 1, SNAP_MAX_SAMPLES);
    p.lock_ms = MIN(p.lock_ms, 10000);
    p.lock_events = MIN(p.lock_events, 1000);
    p.idle_reset_ms = MIN(p.idle_reset_ms, 10000);

    K_SPINLOCK(&data->lock) {
        data->live = p;
        snap_reset(data); /* window shape changed — stale sums lie */
    }
    return 0;
}

static int flask_scrollsnap_handle_event(const struct device *dev, struct input_event *event,
                                         uint32_t param1, uint32_t param2,
                                         struct zmk_input_processor_state *state) {
    struct flask_scrollsnap_data *data = dev->data;

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_WHEEL && event->code != INPUT_REL_HWHEEL)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    bool is_x = (event->code == INPUT_REL_HWHEEL);
    int ret = ZMK_INPUT_PROC_CONTINUE;

    K_SPINLOCK(&data->lock) {
        struct flask_scrollsnap_params *p = &data->live;

        if (!p->enabled) {
            K_SPINLOCK_BREAK;
        }

        int64_t now = k_uptime_get();

        if (p->idle_reset_ms > 0 && (now - data->last_event_ms) >= p->idle_reset_ms) {
            snap_reset(data);
        }
        data->last_event_ms = now;

        /* Expire a time-based lock. */
        if (data->lock_dir != DIR_NONE && p->lock_ms > 0 && data->lock_expires_ms > 0 &&
            now >= data->lock_expires_ms) {
            data->lock_dir = DIR_NONE;
            data->lock_expires_ms = 0;
            data->lock_events_left = 0;
        }

        /* Ring-buffer the incoming magnitude. */
        struct snap_sample in = {.dx = is_x ? event->value : 0, .dy = is_x ? 0 : event->value};

        if (data->sample_count >= p->samples) {
            struct snap_sample old = data->samples[data->head];
            data->sum.dx -= abs(old.dx);
            data->sum.dy -= abs(old.dy);
        } else {
            data->sample_count++;
        }
        data->samples[data->head] = in;
        data->sum.dx += abs(in.dx);
        data->sum.dy += abs(in.dy);
        data->rem.dx += in.dx;
        data->rem.dy += in.dy;
        data->head = (data->head + 1) % p->samples;

        uint32_t ax = (uint32_t)data->sum.dx;
        uint32_t ay = (uint32_t)data->sum.dy;

        /* Not enough signal yet: swallow, keep accumulating. */
        if (data->sample_count < p->samples &&
            (p->immediate_thresh == 0 ||
             (ax <= p->immediate_thresh && ay <= p->immediate_thresh))) {
            event->value = 0;
            event->sync = false;
            ret = ZMK_INPUT_PROC_STOP;
            K_SPINLOCK_BREAK;
        }

        /* Dominance test: axis wins with more than threshold_pct of the
         * window's total motion. Between the two cutoffs, no decision —
         * motion passes through diagonally. */
        uint8_t detected = DIR_NONE;
        uint32_t total = ax + ay;

        if (total > 0) {
            if (ay * 100 > total * p->threshold_pct) {
                detected = DIR_Y;
            } else if (ax * 100 > total * p->threshold_pct) {
                detected = DIR_X;
            }
        }

        bool lock_active = (data->lock_dir != DIR_NONE) &&
                           ((p->lock_ms > 0 && data->lock_expires_ms > now) ||
                            data->lock_events_left > 0);
        uint8_t decided = lock_active ? data->lock_dir : detected;

        /* Emit the decided axis's pending motion through THIS event when it
         * matches; zero the other axis's backlog. */
        switch (decided) {
        case DIR_X:
            event->value = is_x ? data->rem.dx : 0;
            data->rem.dy = 0;
            if (is_x) {
                data->rem.dx = 0;
            }
            break;
        case DIR_Y:
            event->value = is_x ? 0 : data->rem.dy;
            data->rem.dx = 0;
            if (!is_x) {
                data->rem.dy = 0;
            }
            break;
        default:
            /* No decision: pass the event's own backlog through. */
            event->value = is_x ? data->rem.dx : data->rem.dy;
            if (is_x) {
                data->rem.dx = 0;
            } else {
                data->rem.dy = 0;
            }
            break;
        }

        /* Lock bookkeeping (kot149 semantics): refresh on agreement, start
         * on a fresh decision, decay event-locks on disagreement. */
        if (p->lock_ms > 0 || p->lock_events > 0) {
            if (lock_active) {
                if (detected != DIR_NONE && detected == data->lock_dir) {
                    if (p->lock_ms > 0) {
                        data->lock_expires_ms = now + p->lock_ms;
                    }
                    if (p->lock_events > 0) {
                        data->lock_events_left = p->lock_events;
                    }
                } else if (p->lock_ms == 0 && p->lock_events > 0 &&
                           data->lock_events_left > 0) {
                    if (--data->lock_events_left == 0) {
                        data->lock_dir = DIR_NONE;
                    }
                }
            } else if (decided != DIR_NONE) {
                data->lock_dir = decided;
                if (p->lock_ms > 0) {
                    data->lock_expires_ms = now + p->lock_ms;
                }
                if (p->lock_events > 0) {
                    data->lock_events_left = p->lock_events;
                }
            }
        }
    }

    return ret;
}

struct flask_scrollsnap_dt_config {
    struct flask_scrollsnap_params defaults;
};

static int flask_scrollsnap_init(const struct device *dev) {
    struct flask_scrollsnap_data *data = dev->data;
    const struct flask_scrollsnap_dt_config *cfg = dev->config;

    data->live = cfg->defaults;
    snap_reset(data);
    data->last_event_ms = k_uptime_get();
    if (flask_scrollsnap_singleton == NULL) {
        flask_scrollsnap_singleton = data;
    }
    return 0;
}

static const struct zmk_input_processor_driver_api flask_scrollsnap_api = {
    .handle_event = flask_scrollsnap_handle_event,
};

#define FLASK_SCROLLSNAP_INST(n)                                                                   \
    static struct flask_scrollsnap_data flask_scrollsnap_data_##n;                                 \
    static const struct flask_scrollsnap_dt_config flask_scrollsnap_config_##n = {                 \
        .defaults = {                                                                              \
            .enabled = !DT_INST_PROP(n, start_disabled),                                           \
            .threshold_pct = DT_INST_PROP(n, threshold_pct),                                       \
            .samples = DT_INST_PROP(n, samples),                                                   \
            .immediate_thresh = DT_INST_PROP(n, immediate_threshold),                              \
            .lock_ms = DT_INST_PROP(n, lock_ms),                                                   \
            .lock_events = DT_INST_PROP(n, lock_events),                                           \
            .idle_reset_ms = DT_INST_PROP(n, idle_reset_ms),                                       \
        },                                                                                         \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, flask_scrollsnap_init, NULL, &flask_scrollsnap_data_##n,              \
                          &flask_scrollsnap_config_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,   \
                          &flask_scrollsnap_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_SCROLLSNAP_INST)
