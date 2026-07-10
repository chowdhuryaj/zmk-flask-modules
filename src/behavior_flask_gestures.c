/*
 * &fges behavior — keymap trigger for flask_gestures.
 *
 * One param: the gesture set to use while held (0..sets-1), or 255 to
 * follow the runtime active set (the app's Set dropdown / QMK wire value
 * 0x02). Hold-style: press opens the gesture, release closes it — the
 * ratchet fires incrementally in between.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_flask_gestures

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <flask_gestures/flask_gestures.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_fges_binding_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    flask_gestures_begin((uint8_t)binding->param1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_fges_binding_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    flask_gestures_end();
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {
    {
        .display_name = "Gesture set (255 = active)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = 0, .max = 255},
    },
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif

static const struct behavior_driver_api behavior_flask_gestures_driver_api = {
    .binding_pressed = on_fges_binding_pressed,
    .binding_released = on_fges_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

static int behavior_flask_gestures_init(const struct device *dev) { return 0; }

#define FGES_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_flask_gestures_init, NULL, NULL, NULL, POST_KERNEL,        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_flask_gestures_driver_api);

DT_INST_FOREACH_STATUS_OKAY(FGES_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
