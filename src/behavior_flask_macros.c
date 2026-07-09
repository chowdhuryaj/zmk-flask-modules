/*
 * &fmac behavior — keymap trigger for flask_macros.
 *
 * One param: the macro slot index (0..FLASK_MACROS_SLOTS-1). Press plays the
 * slot; release is a no-op (playback pacing is the module's, not the key
 * hold's). Parameter metadata advertises the slot range so ZMK Studio (and
 * the Flask app's behavior composer) render a bounded picker.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_flask_macros

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <flask_macros/flask_macros.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_fmac_binding_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    flask_macros_play(binding->param1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_fmac_binding_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {
    {
        .display_name = "Macro slot",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = 0, .max = FLASK_MACROS_SLOTS - 1},
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

static const struct behavior_driver_api behavior_flask_macros_driver_api = {
    .binding_pressed = on_fmac_binding_pressed,
    .binding_released = on_fmac_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

static int behavior_flask_macros_init(const struct device *dev) { return 0; }

#define FMAC_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_flask_macros_init, NULL, NULL, NULL, POST_KERNEL,          \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_flask_macros_driver_api);

DT_INST_FOREACH_STATUS_OKAY(FMAC_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
