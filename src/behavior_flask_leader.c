/*
 * &fled behavior — keymap trigger for flask_leader.
 *
 * No params. Press opens (or re-arms) a runtime-leader capture; release is
 * a no-op — the capture lives until a match, a dead key, or the timeout.
 * Coexists with urob's compile-time &leader on a different key.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_flask_leader

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <flask_leader/flask_leader.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_fled_binding_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    flask_leader_begin();
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_fled_binding_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_flask_leader_driver_api = {
    .binding_pressed = on_fled_binding_pressed,
    .binding_released = on_fled_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    /* No params — but WITHOUT this, behavior_get_parameter_metadata returns
     * -ENODEV and Studio rejects every &fled assignment with INVALID
     * PARAMETERS (bench 2026-07-11; core 0-param behaviors all carry it). */
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

static int behavior_flask_leader_init(const struct device *dev) { return 0; }

#define FLED_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_flask_leader_init, NULL, NULL, NULL, POST_KERNEL,          \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_flask_leader_driver_api);

DT_INST_FOREACH_STATUS_OKAY(FLED_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
