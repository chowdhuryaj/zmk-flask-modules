/*
 * &ftd behavior — keymap trigger for flask_tapdance.
 *
 * One param: the dance slot index (0..FLASK_TAPDANCE_SLOTS-1). Press and
 * release feed the runtime dance engine keyed by the key's position; the
 * engine owns counting, the per-slot term, and firing the tap-count
 * output. Parameter metadata advertises the slot range so ZMK Studio
 * (and the app's composer/wizard) render a bounded picker.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_flask_tapdance

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <flask_tapdance/flask_tapdance.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_ftd_binding_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    flask_tapdance_pressed((uint8_t)binding->param1, event.position, event.timestamp);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_ftd_binding_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    flask_tapdance_released((uint8_t)binding->param1, event.position, event.timestamp);
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {
    {
        .display_name = "Tap dance slot",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = 0, .max = FLASK_TAPDANCE_SLOTS - 1},
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

static const struct behavior_driver_api behavior_flask_tapdance_driver_api = {
    .binding_pressed = on_ftd_binding_pressed,
    .binding_released = on_ftd_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

static int behavior_flask_tapdance_init(const struct device *dev) { return 0; }

#define FTD_INST(n)                                                                                \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_flask_tapdance_init, NULL, NULL, NULL, POST_KERNEL,        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_flask_tapdance_driver_api);

DT_INST_FOREACH_STATUS_OKAY(FTD_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
