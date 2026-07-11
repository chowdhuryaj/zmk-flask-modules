/*
 * &bswap behavior — keymap trigger for flask_ballswap.
 *
 * One param, the mode:
 *   0 = TOGGLE: press flips the persisted base state and saves it
 *       (survives power cycle and reflashing); release is a no-op.
 *   1 = MOMENTARY: swapped while held — press adds a momentary hold,
 *       release removes it. Never persisted.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_flask_ballswap

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <flask_ballswap/flask_ballswap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define BSWAP_MODE_TOGGLE 0
#define BSWAP_MODE_MOMENTARY 1

static int on_bswap_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    if (binding->param1 == BSWAP_MODE_MOMENTARY) {
        flask_ballswap_momentary_press();
    } else {
        flask_ballswap_set_swapped(!flask_ballswap_swapped());
        flask_ballswap_save();
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_bswap_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (binding->param1 == BSWAP_MODE_MOMENTARY) {
        flask_ballswap_momentary_release();
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {
    {
        .display_name = "Toggle (saved)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = BSWAP_MODE_TOGGLE,
    },
    {
        .display_name = "While held",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = BSWAP_MODE_MOMENTARY,
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

static const struct behavior_driver_api behavior_flask_ballswap_driver_api = {
    .binding_pressed = on_bswap_binding_pressed,
    .binding_released = on_bswap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

static int behavior_flask_ballswap_init(const struct device *dev) { return 0; }

#define BSWAP_INST(n)                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_flask_ballswap_init, NULL, NULL, NULL, POST_KERNEL,        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_flask_ballswap_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BSWAP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
