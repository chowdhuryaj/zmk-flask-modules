/*
 * flask_ballswap — live trackball role swap (see flask_ballswap.h).
 *
 * One processor instance sits at the front of EACH ball's physical listener
 * chain (after gestures, which must always see raw counts). While a swap is
 * in effect every relative event is swallowed (ZMK_INPUT_PROC_STOP) and
 * re-emitted 1:1 from this instance's own DT node — the flask_autoscroll
 * IP-that-emits pattern — where a second zmk,input-listener carries the
 * OTHER role's processor chain. No cross-node wiring, no loops: the out
 * listeners don't include a ballswap processor.
 *
 * Swap inactive = transparent pass-through (ZMK_INPUT_PROC_CONTINUE); the
 * physical chains behave exactly as compiled.
 *
 * Re-emission uses K_NO_WAIT: the handler runs in the input dispatch
 * context, and blocking there on a full queue would deadlock the very
 * thread that drains it. A dropped event under pathological load costs a
 * tick of motion, nothing more.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_ballswap

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>

#include <flask_ballswap/flask_ballswap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* --- swap state (module-global: both instances share it) --- */

static struct k_spinlock bswap_lock;
static bool bswap_base;         /* persisted */
static uint8_t bswap_momentary; /* held &bswap 1 keys, never persisted */

bool flask_ballswap_swapped(void) {
    bool on;

    K_SPINLOCK(&bswap_lock) { on = bswap_base; }
    return on;
}

void flask_ballswap_set_swapped(bool on) {
    K_SPINLOCK(&bswap_lock) { bswap_base = on; }
}

bool flask_ballswap_effective(void) {
    bool on;

    K_SPINLOCK(&bswap_lock) { on = bswap_base ^ (bswap_momentary > 0); }
    return on;
}

void flask_ballswap_momentary_press(void) {
    K_SPINLOCK(&bswap_lock) {
        if (bswap_momentary < UINT8_MAX) {
            bswap_momentary++;
        }
    }
}

void flask_ballswap_momentary_release(void) {
    K_SPINLOCK(&bswap_lock) {
        if (bswap_momentary > 0) {
            bswap_momentary--;
        }
    }
}

/* --- persistence (settings leaf "flask/ballswap") --- */

struct flask_ballswap_saved {
    uint8_t version;
    uint8_t swapped;
} __packed;

#define BALLSWAP_SETTINGS_VERSION 1

int flask_ballswap_save(void) {
    struct flask_ballswap_saved saved = {
        .version = BALLSWAP_SETTINGS_VERSION,
        .swapped = flask_ballswap_swapped() ? 1 : 0,
    };
    int err = settings_save_one("flask/ballswap", &saved, sizeof(saved));

    if (err) {
        LOG_ERR("flask/ballswap settings save failed: %d", err);
    }
    return err;
}

int flask_ballswap_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg) {
    struct flask_ballswap_saved saved;

    if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
        LOG_WRN("flask/ballswap settings unreadable (len %d)", (int)len);
        return 0;
    }
    if (saved.version != BALLSWAP_SETTINGS_VERSION) {
        LOG_WRN("flask/ballswap settings version %d ignored", saved.version);
        return 0;
    }
    flask_ballswap_set_swapped(saved.swapped != 0);
    return 0;
}

/* --- processor --- */

static int flask_ballswap_handle_event(const struct device *dev, struct input_event *event,
                                       uint32_t param1, uint32_t param2,
                                       struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != INPUT_EV_REL || !flask_ballswap_effective()) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Re-emit 1:1 (code, value, sync) from our own node; the out listener
     * bound to this node applies the other ball's chain. */
    input_report(dev, INPUT_EV_REL, event->code, event->value, event->sync, K_NO_WAIT);

    event->value = 0;
    event->sync = false;
    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api flask_ballswap_api = {
    .handle_event = flask_ballswap_handle_event,
};

static int flask_ballswap_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

#define FLASK_BALLSWAP_INST(n)                                                                     \
    DEVICE_DT_INST_DEFINE(n, flask_ballswap_init, NULL, NULL, NULL, POST_KERNEL,                   \
                          CONFIG_INPUT_INIT_PRIORITY, &flask_ballswap_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_BALLSWAP_INST)
