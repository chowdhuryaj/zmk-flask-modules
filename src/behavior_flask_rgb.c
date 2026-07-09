/*
 * &frgb behavior — keymap control of the flask_rgb map.
 *
 * One param (dt-bindings/flask/rgb.h): FRGB_TOG toggles the per-layer map,
 * FRGB_ON / FRGB_OFF force it. The QMK parity keycode is RGBMAP_TOG.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_flask_rgb

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <dt-bindings/flask/rgb.h>
#include <flask_rgb/flask_rgb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_frgb_binding_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case FRGB_ON:
        flask_rgb_set_enabled(true);
        break;
    case FRGB_OFF:
        flask_rgb_set_enabled(false);
        break;
    case FRGB_TOG:
    default:
        flask_rgb_set_enabled(!flask_rgb_enabled());
        break;
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_frgb_binding_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_flask_rgb_driver_api = {
    .binding_pressed = on_frgb_binding_pressed,
    .binding_released = on_frgb_binding_released,
};

static int behavior_flask_rgb_init(const struct device *dev) { return 0; }

#define FRGB_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_flask_rgb_init, NULL, NULL, NULL, POST_KERNEL,             \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_flask_rgb_driver_api);

DT_INST_FOREACH_STATUS_OKAY(FRGB_INST)
