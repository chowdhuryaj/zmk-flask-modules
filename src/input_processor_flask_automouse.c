/*
 * flask_automouse — runtime-tunable auto-mouse layer (see flask_automouse.h).
 *
 * In-chain input processor on the cursor ball (replaces &zip_temp_layer):
 * relative motion accumulates |dx|+|dy|; once the accumulator crosses the
 * threshold (with a require-prior-idle typing gate, like the stock
 * processor) the target layer activates. Deactivation:
 *
 *   timeout > 0  — a delayable work item drops the layer `timeout_ms` after
 *                  the last qualifying motion. A key pressed while the layer
 *                  is up either extends that deadline (non-transparent
 *                  binding on the layer, when extend_on_key is set) or drops
 *                  the layer immediately (transparent binding — the user is
 *                  typing; the key still types, from the layer below).
 *   timeout == 0 — LATCH: no timer. The layer stays until a key that is
 *                  TRANSPARENT on the layer is pressed; that press (and its
 *                  release) is swallowed — it never reaches the keymap — and
 *                  the layer drops. Keys with real bindings on the layer
 *                  (clicks, snipe, the gesture key) behave normally.
 *
 * Excluded positions (DT list, e.g. always-on thumb mouse buttons that are
 * transparent ON the layer but resolve to clicks below it) never deactivate
 * or get swallowed — they count as "mousing continues" and extend instead.
 *
 * The layer param is an INDEX (order position — what the app's Target-layer
 * dropdown writes); every core call converts via zmk_keymap_layer_index_to_id
 * so Studio layer reordering keeps the mapping honest.
 *
 * Like the core temp-layer processor, layer activate/deactivate runs on the
 * system work queue (msgq + work), never in the input dispatch context.
 * Deadlines use k_work_reschedule (k_work_schedule keeps a stale deadline —
 * the autoscroll bug class).
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_automouse

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>

#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

#include <flask_automouse/flask_automouse.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Motion accumulator forgets after this much idle — a slow drift across
 * seconds should not eventually "save up" a trigger. Matches the prior-idle
 * feel (150 ms). */
#define ACCUM_RESET_MS 150

/* --- state (module-global: one auto-mouse engine; the node may be wired
 * into several chains — physical + ballswap-out — like &zip_temp_layer was) --- */

static struct k_spinlock am_lock;
static struct flask_automouse_params am_live;
static int16_t am_require_prior_idle_ms;
static const uint16_t *am_excluded;
static size_t am_excluded_len;

static bool am_active;              /* layer is up because WE raised it */
static uint16_t am_accum;           /* motion counts toward the threshold */
static int64_t am_last_motion;      /* accumulator idle-reset clock */
static int64_t am_last_keycode;     /* require-prior-idle typing gate */
static int32_t am_swallow_position; /* latch: swallow this position's release; -1 = none */

static struct k_work_delayable am_off_work;

/* The transparent behavior's device name — resolved once; bindings store
 * behavior_dev strings. */
static const char *am_trans_name;

int flask_automouse_params_get(struct flask_automouse_params *out) {
    if (out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&am_lock) { *out = am_live; }
    return 0;
}

static void am_clamp(struct flask_automouse_params *p) {
    p->timeout_ms = MIN(p->timeout_ms, 60000);
    p->threshold = MIN(p->threshold, 1000);
    if (p->layer >= ZMK_KEYMAP_LAYERS_LEN) {
        p->layer = ZMK_KEYMAP_LAYERS_LEN - 1;
    }
}

int flask_automouse_params_set(const struct flask_automouse_params *in) {
    if (in == NULL) {
        return -EINVAL;
    }

    struct flask_automouse_params p = *in;
    am_clamp(&p);

    bool deactivate = false;
    K_SPINLOCK(&am_lock) {
        /* A live edit that disables the engine, retargets the layer, or
         * leaves latch mode must not strand an active layer. */
        deactivate = am_active && (!p.enabled || p.layer != am_live.layer);
        am_live = p;
        am_accum = 0;
    }
    if (deactivate) {
        k_work_reschedule(&am_off_work, K_NO_WAIT);
    } else if (p.timeout_ms > 0) {
        bool active;
        K_SPINLOCK(&am_lock) { active = am_active; }
        if (active) {
            /* Leaving latch mode (or retuning): give the layer one fresh
             * timeout instead of leaving it timer-less. */
            k_work_reschedule(&am_off_work, K_MSEC(p.timeout_ms));
        }
    }
    return 0;
}

/* --- persistence ("flask/automouse") --- */

int flask_automouse_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg) {
    struct flask_automouse_saved saved;

    if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
        LOG_WRN("flask/automouse settings unreadable (len %d)", (int)len);
        return 0;
    }
    if (saved.version != AUTOMOUSE_SETTINGS_VERSION) {
        LOG_WRN("flask/automouse settings version %d ignored", saved.version);
        return 0;
    }
    flask_automouse_params_set(&saved.params);
    return 0;
}

/* --- layer plumbing (system work queue, never the input/event context) --- */

static zmk_keymap_layer_id_t am_layer_id(void) {
    uint8_t index;
    K_SPINLOCK(&am_lock) { index = am_live.layer; }
    return zmk_keymap_layer_index_to_id(index);
}

static void am_off_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    bool was_active;

    K_SPINLOCK(&am_lock) {
        was_active = am_active;
        am_active = false;
        am_accum = 0;
    }
    if (was_active) {
        zmk_keymap_layer_deactivate(am_layer_id(), false);
        LOG_DBG("auto-mouse layer down");
    }
}

static void am_on_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    bool activate = false;

    K_SPINLOCK(&am_lock) {
        if (am_live.enabled && !am_active) {
            am_active = true;
            activate = true;
        }
    }
    if (activate) {
        zmk_keymap_layer_activate(am_layer_id(), false);
        LOG_DBG("auto-mouse layer up");
    }
}

static K_WORK_DEFINE(am_on_work, am_on_work_cb);

/* --- input processor --- */

static int flask_automouse_handle_event(const struct device *dev, struct input_event *event,
                                        uint32_t param1, uint32_t param2,
                                        struct zmk_input_processor_state *state) {
    ARG_UNUSED(dev);
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) || event->value == 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct flask_automouse_params p;
    bool active;
    bool trigger = false;
    int64_t now = k_uptime_get();

    K_SPINLOCK(&am_lock) {
        p = am_live;
        active = am_active;
        if (p.enabled && !active) {
            if (now - am_last_motion > ACCUM_RESET_MS) {
                am_accum = 0;
            }
            am_accum = MIN((uint32_t)am_accum + abs(event->value), UINT16_MAX);
            trigger = am_accum > p.threshold ||
                      (p.threshold == 0 && am_accum > 0);
        }
        am_last_motion = now;
    }

    if (!p.enabled) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* require-prior-idle: motion mid-typing must not pop the layer. */
    if (trigger && (am_last_keycode + am_require_prior_idle_ms) <= now) {
        k_work_submit(&am_on_work);
        active = true;
    }

    if (active && p.timeout_ms > 0) {
        k_work_reschedule(&am_off_work, K_MSEC(p.timeout_ms));
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

/* --- event listeners: typing gate, key-driven deactivate/extend/latch --- */

static bool am_position_excluded(uint32_t position) {
    for (size_t i = 0; i < am_excluded_len; i++) {
        if (am_excluded[i] == position) {
            return true;
        }
    }
    return false;
}

/* Is this position TRANSPARENT on the auto-mouse layer? Resolved against the
 * LIVE keymap (Studio edits included). Unknown/missing bindings count as
 * transparent — nothing on the layer would handle them. */
static bool am_binding_transparent(uint32_t position) {
    const struct zmk_behavior_binding *b =
        zmk_keymap_get_layer_binding_at_idx(am_layer_id(), (uint16_t)position);

    if (b == NULL || b->behavior_dev == NULL) {
        return true;
    }
    return am_trans_name != NULL && strcmp(b->behavior_dev, am_trans_name) == 0;
}

static int am_handle_position(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    struct flask_automouse_params p;
    bool active;
    int32_t swallow;

    K_SPINLOCK(&am_lock) {
        p = am_live;
        active = am_active;
        swallow = am_swallow_position;
    }

    /* Latch swallow, part 2: the release of the key that dropped the layer. */
    if (!ev->state) {
        if (swallow >= 0 && (uint32_t)swallow == ev->position) {
            K_SPINLOCK(&am_lock) { am_swallow_position = -1; }
            return ZMK_EV_EVENT_HANDLED;
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!active || !p.enabled) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (am_position_excluded(ev->position)) {
        /* Always-on mouse buttons: mousing continues. */
        if (p.timeout_ms > 0 && p.extend_on_key) {
            k_work_reschedule(&am_off_work, K_MSEC(p.timeout_ms));
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!am_binding_transparent(ev->position)) {
        /* A real binding on the layer (click, snipe, gesture key): keep the
         * layer up, optionally re-arm the timer. */
        if (p.timeout_ms > 0 && p.extend_on_key) {
            k_work_reschedule(&am_off_work, K_MSEC(p.timeout_ms));
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Transparent on the layer = the user is typing again. */
    if (p.timeout_ms == 0) {
        /* Latch mode: drop the layer AND swallow this key — its only job
         * was to end auto-mouse. */
        K_SPINLOCK(&am_lock) { am_swallow_position = (int32_t)ev->position; }
        k_work_reschedule(&am_off_work, K_NO_WAIT);
        return ZMK_EV_EVENT_HANDLED;
    }

    /* Timed mode: drop the layer immediately; the key types from below. */
    k_work_reschedule(&am_off_work, K_NO_WAIT);
    return ZMK_EV_EVENT_BUBBLE;
}

static int am_handle_keycode(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

    if (ev->state) {
        K_SPINLOCK(&am_lock) { am_last_keycode = ev->timestamp; }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

/* Someone else (a &to, panic key, Studio) took the layer down: resync so the
 * next motion can re-raise it. */
static int am_handle_layer_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    bool ours;

    K_SPINLOCK(&am_lock) { ours = am_active; }
    if (ours && !zmk_keymap_layer_active(am_layer_id())) {
        K_SPINLOCK(&am_lock) {
            am_active = false;
            am_swallow_position = -1;
        }
        k_work_cancel_delayable(&am_off_work);
        LOG_DBG("auto-mouse layer deactivated externally");
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int am_event_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return am_handle_position(eh);
    }
    if (as_zmk_keycode_state_changed(eh) != NULL) {
        return am_handle_keycode(eh);
    }
    if (as_zmk_layer_state_changed(eh) != NULL) {
        return am_handle_layer_state(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_automouse, am_event_listener);
ZMK_SUBSCRIPTION(flask_automouse, zmk_position_state_changed);
ZMK_SUBSCRIPTION(flask_automouse, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(flask_automouse, zmk_layer_state_changed);

/* --- driver --- */

static const struct zmk_input_processor_driver_api flask_automouse_api = {
    .handle_event = flask_automouse_handle_event,
};

struct flask_automouse_dt_config {
    struct flask_automouse_params defaults;
    int16_t require_prior_idle_ms;
    const uint16_t *excluded_positions;
    size_t num_positions;
};

static int flask_automouse_init(const struct device *dev) {
    const struct flask_automouse_dt_config *cfg = dev->config;

    am_live = cfg->defaults;
    am_clamp(&am_live);
    am_require_prior_idle_ms = cfg->require_prior_idle_ms;
    am_excluded = cfg->excluded_positions;
    am_excluded_len = cfg->num_positions;
    am_swallow_position = -1;
    k_work_init_delayable(&am_off_work, am_off_work_cb);

    /* The transparent behavior device — present in every keymap that has a
     * &trans anywhere (the Imprint's has dozens). */
    const struct device *trans = zmk_behavior_get_binding("transparent");
    am_trans_name = trans != NULL ? trans->name : "transparent";
    return 0;
}

#define AM_HAS_EXCLUDED(n) DT_INST_PROP_HAS_IDX(n, excluded_positions, 0)

#define FLASK_AUTOMOUSE_INST(n)                                                                    \
    static const uint16_t am_excluded_positions_##n[] =                                            \
        COND_CODE_1(AM_HAS_EXCLUDED(n), (DT_INST_PROP(n, excluded_positions)), ({0}));             \
    static const struct flask_automouse_dt_config flask_automouse_config_##n = {                   \
        .defaults = {                                                                              \
            .enabled = !DT_INST_PROP(n, start_disabled),                                           \
            .timeout_ms = DT_INST_PROP(n, timeout_ms),                                             \
            .threshold = DT_INST_PROP(n, threshold),                                               \
            .layer = DT_INST_PROP(n, layer),                                                       \
            .extend_on_key = !DT_INST_PROP(n, no_extend_on_key),                                   \
        },                                                                                         \
        .require_prior_idle_ms = DT_INST_PROP_OR(n, require_prior_idle_ms, 0),                     \
        .excluded_positions = am_excluded_positions_##n,                                           \
        .num_positions = COND_CODE_1(AM_HAS_EXCLUDED(n),                                           \
                                     (DT_INST_PROP_LEN(n, excluded_positions)), (0)),              \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, flask_automouse_init, NULL, NULL, &flask_automouse_config_##n,        \
                          POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, &flask_automouse_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_AUTOMOUSE_INST)
