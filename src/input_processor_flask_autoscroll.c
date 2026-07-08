/*
 * Flask autoscroll input processor — hands-free continuous scrolling.
 *
 * Port of the Flask (QMK) autoscroll module (Ben White stepped intervals +
 * Contour Shuttle jog model); see flask_autoscroll.h for the behavior
 * contract. Semantics preserved from the QMK source:
 *   - stepped |level| 1..9 picks a tick interval from the canonical table,
 *     scaled by speed_scale_x100 (min 5 ms)
 *   - jog interval interpolates linearly from the slowest to the fastest
 *     stepped interval across [deadzone .. deadzone+range] deflection, so
 *     full deflection scrolls exactly as fast as stepped level 9
 *   - ball down (y+) scrolls down (wheel negative); `inverted` flips
 *   - entering either mode re-arms a full interval before the first tick
 *
 * As an input processor it is transparent while idle or stepped; while
 * jogging it swallows REL_X/REL_Y and winds REL_Y into the deflection.
 * Wheel ticks are emitted as REL_WHEEL from this node on a self-scheduled
 * timer — attach a second zmk,input-listener to this node to route them
 * onward (flask_scroll's IP-that-emits pattern).
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_autoscroll

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <drivers/input_processor.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#include <flask_autoscroll/flask_autoscroll.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Ben White's canonical table: ms per wheel notch for stepped |level| 1..9. */
static const uint16_t step_intervals[9] = {1000, 500, 200, 100, 67, 50, 40, 33, 25};

struct flask_autoscroll_data {
    const struct device *dev;
    struct k_work_delayable tick_work;
    struct k_spinlock lock;
    struct flask_autoscroll_params live;
    int8_t level;       /* stepped mode: signed speed level, 0 = stopped */
    bool jogging;       /* jog mode */
    int32_t deflection; /* jog-wheel position, sensor counts, + = down */
    bool running;       /* tick timer armed */
};

/* Singleton handle (one scroll ball, same convention as flask_scroll). */
static struct flask_autoscroll_data *flask_autoscroll_singleton;

int flask_autoscroll_params_get(struct flask_autoscroll_params *out) {
    struct flask_autoscroll_data *data = flask_autoscroll_singleton;

    if (data == NULL || out == NULL) {
        return -ENODEV;
    }
    K_SPINLOCK(&data->lock) { *out = data->live; }
    return 0;
}

int flask_autoscroll_params_set(const struct flask_autoscroll_params *in) {
    struct flask_autoscroll_data *data = flask_autoscroll_singleton;

    if (data == NULL || in == NULL) {
        return -ENODEV;
    }

    struct flask_autoscroll_params p = *in;

    p.speed_scale_x100 = CLAMP(p.speed_scale_x100, 25, 400);
    p.jog_deadzone = CLAMP(p.jog_deadzone, 0, 200);
    p.jog_range = CLAMP(p.jog_range, 50, 2000);

    K_SPINLOCK(&data->lock) { data->live = p; }
    return 0;
}

/* Effective interval for stepped |level| 1..9, speed-scale applied. */
static uint32_t stepped_interval(struct flask_autoscroll_data *data, uint8_t magnitude) {
    uint32_t base = step_intervals[magnitude - 1];
    uint32_t scaled = base * 100 / data->live.speed_scale_x100;

    return scaled < 5 ? 5 : scaled;
}

/* Jog interval: lerp slowest → fastest across [0 .. range] counts past the
 * deadzone. */
static uint32_t jog_interval(struct flask_autoscroll_data *data, uint32_t past) {
    uint32_t slowest = stepped_interval(data, 1);
    uint32_t fastest = stepped_interval(data, 9);

    if (past >= data->live.jog_range) {
        return fastest;
    }
    return slowest - (slowest - fastest) * past / data->live.jog_range;
}

/* Direction/interval for the current state; dir 0 = idle. Caller holds the
 * lock. */
static void current_tick(struct flask_autoscroll_data *data, int8_t *dir, uint32_t *interval) {
    *dir = 0;
    *interval = 0;

    if (data->jogging) {
        int32_t mag = data->deflection < 0 ? -data->deflection : data->deflection;
        int32_t past = mag - data->live.jog_deadzone;

        if (past > 0) {
            /* Ball down (y+) scrolls down — wheel negative. */
            *dir = data->deflection > 0 ? -1 : 1;
            *interval = jog_interval(data, (uint32_t)past);
        }
    } else if (data->level != 0) {
        *dir = data->level > 0 ? 1 : -1;
        *interval = stepped_interval(data, data->level > 0 ? data->level : -data->level);
    }

    if (data->live.inverted) {
        *dir = -*dir;
    }
}

static void flask_autoscroll_tick(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct flask_autoscroll_data *data =
        CONTAINER_OF(dwork, struct flask_autoscroll_data, tick_work);

    int8_t dir;
    uint32_t interval;

    K_SPINLOCK(&data->lock) {
        current_tick(data, &dir, &interval);
        data->running = (dir != 0);
        if (data->running) {
            k_work_schedule(&data->tick_work, K_MSEC(interval));
        }
    }

    if (dir != 0) {
        input_report_rel(data->dev, INPUT_REL_WHEEL, dir, true, K_MSEC(10));
    }
}

/* (Re)arm the timer for a full interval of the current state; cancels when
 * idle. QMK resets last_tick on every mode change — a full interval always
 * precedes the first notch. */
static void rearm(struct flask_autoscroll_data *data) {
    int8_t dir;
    uint32_t interval;
    bool arm = false;

    K_SPINLOCK(&data->lock) {
        current_tick(data, &dir, &interval);
        data->running = (dir != 0);
        arm = data->running;
    }

    if (arm) {
        k_work_reschedule(&data->tick_work, K_MSEC(interval));
    } else {
        k_work_cancel_delayable(&data->tick_work);
    }
}

void flask_autoscroll_stop(void) {
    struct flask_autoscroll_data *data = flask_autoscroll_singleton;

    if (data == NULL) {
        return;
    }
    K_SPINLOCK(&data->lock) {
        data->level = 0;
        data->jogging = false;
        data->deflection = 0;
        data->running = false;
    }
    k_work_cancel_delayable(&data->tick_work);
}

void flask_autoscroll_step(int8_t direction) {
    struct flask_autoscroll_data *data = flask_autoscroll_singleton;

    if (data == NULL) {
        return;
    }
    K_SPINLOCK(&data->lock) {
        if (data->jogging) { /* stepping while jogging exits jog first */
            data->jogging = false;
            data->deflection = 0;
            data->level = 0;
        }
        int8_t next = data->level + direction;

        data->level = CLAMP(next, -9, 9);
    }
    rearm(data);
}

void flask_autoscroll_jog_toggle(void) {
    struct flask_autoscroll_data *data = flask_autoscroll_singleton;

    if (data == NULL) {
        return;
    }

    K_SPINLOCK(&data->lock) {
        if (data->jogging) {
            data->jogging = false;
        } else {
            data->level = 0; /* clears any stepped level */
            data->jogging = true;
        }
        data->deflection = 0;
        data->running = false;
    }
    /* Cancel any pending tick either way. Exiting stops output; entering
     * starts from center with no ticks owed — a leftover stepped-mode
     * deadline must not pace the first jog notch (k_work_schedule on a
     * still-pending item keeps the old deadline, so handle_event's re-arm
     * at the correct jog interval would silently no-op). */
    k_work_cancel_delayable(&data->tick_work);
}

int16_t flask_autoscroll_live_state(void) {
    struct flask_autoscroll_data *data = flask_autoscroll_singleton;

    if (data == NULL) {
        return 0;
    }
    return data->jogging ? 100 : data->level;
}

static int flask_autoscroll_handle_event(const struct device *dev, struct input_event *event,
                                         uint32_t param1, uint32_t param2,
                                         struct zmk_input_processor_state *state) {
    struct flask_autoscroll_data *data = dev->data;

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Idle/stepped fast path: no lock on the per-event hot path (this runs
     * for every REL event the scroll ball emits). An unlocked bool read is
     * atomic on this hardware; a racing jog toggle costs at most one
     * unswallowed event — the locked re-check below keeps the jog path
     * itself consistent. */
    if (!data->jogging) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    bool jogging = false;
    bool arm = false;
    uint32_t interval = 0;

    K_SPINLOCK(&data->lock) {
        jogging = data->jogging;
        if (!jogging) {
            K_SPINLOCK_BREAK;
        }

        if (event->code == INPUT_REL_Y) {
            int32_t limit = (int32_t)data->live.jog_deadzone + data->live.jog_range;

            data->deflection += event->value;
            data->deflection = CLAMP(data->deflection, -limit, limit);

            if (!data->running) {
                int8_t dir;

                current_tick(data, &dir, &interval);
                if (dir != 0) {
                    data->running = true;
                    arm = true;
                }
            }
        }

        /* Swallow ball motion while jogging (cursor/scroll frozen, same
         * contract as QMK drag scroll). */
        event->value = 0;
    }

    if (arm) {
        k_work_schedule(&data->tick_work, K_MSEC(interval));
    }
    return jogging ? ZMK_INPUT_PROC_STOP : ZMK_INPUT_PROC_CONTINUE;
}

/* Stop-on-any-key: the QMK firmwares stop autoscroll from process_record on
 * any other key press. The ZMK analog is the keycode event — the &asc
 * behavior raises no keycode, so it never stops itself. */
static int flask_autoscroll_keycode_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    struct flask_autoscroll_data *data = flask_autoscroll_singleton;

    if (ev == NULL || !ev->state || data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool stop = false;

    K_SPINLOCK(&data->lock) {
        stop = data->live.stop_on_key && (data->level != 0 || data->jogging);
    }
    if (stop) {
        flask_autoscroll_stop();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_autoscroll_stopkey, flask_autoscroll_keycode_listener);
ZMK_SUBSCRIPTION(flask_autoscroll_stopkey, zmk_keycode_state_changed);

struct flask_autoscroll_dt_config {
    struct flask_autoscroll_params defaults;
};

static int flask_autoscroll_init(const struct device *dev) {
    struct flask_autoscroll_data *data = dev->data;
    const struct flask_autoscroll_dt_config *cfg = dev->config;

    data->dev = dev;
    data->live = cfg->defaults;
    k_work_init_delayable(&data->tick_work, flask_autoscroll_tick);
    if (flask_autoscroll_singleton == NULL) {
        flask_autoscroll_singleton = data;
    }
    return 0;
}

static const struct zmk_input_processor_driver_api flask_autoscroll_api = {
    .handle_event = flask_autoscroll_handle_event,
};

/* DT defaults feed the divisions in stepped_interval/jog_interval unclamped —
 * reject out-of-range values at build time (runtime setters clamp). */
#define FLASK_AUTOSCROLL_INST(n)                                                                   \
    BUILD_ASSERT(DT_INST_PROP(n, speed_scale) >= 25 && DT_INST_PROP(n, speed_scale) <= 400,       \
                 "speed-scale must be 25..400");                                                   \
    BUILD_ASSERT(DT_INST_PROP(n, jog_range) >= 50 && DT_INST_PROP(n, jog_range) <= 2000,          \
                 "jog-range must be 50..2000");                                                    \
    BUILD_ASSERT(DT_INST_PROP(n, jog_deadzone) >= 0 && DT_INST_PROP(n, jog_deadzone) <= 200,      \
                 "jog-deadzone must be 0..200");                                                   \
    static struct flask_autoscroll_data flask_autoscroll_data_##n;                                 \
    static const struct flask_autoscroll_dt_config flask_autoscroll_config_##n = {                 \
        .defaults = {                                                                              \
            .speed_scale_x100 = DT_INST_PROP(n, speed_scale),                                      \
            .jog_deadzone = DT_INST_PROP(n, jog_deadzone),                                         \
            .jog_range = DT_INST_PROP(n, jog_range),                                               \
            .inverted = DT_INST_PROP(n, inverted),                                                 \
            .stop_on_key = DT_INST_PROP(n, stop_on_key) != 0,                                      \
        },                                                                                         \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, flask_autoscroll_init, NULL, &flask_autoscroll_data_##n,              \
                          &flask_autoscroll_config_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,   \
                          &flask_autoscroll_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_AUTOSCROLL_INST)
