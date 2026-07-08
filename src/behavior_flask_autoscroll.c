/*
 * &asc behavior — keymap entry points for flask autoscroll.
 *
 * One param (dt-bindings/flask/autoscroll.h): ASC_UP / ASC_DOWN step the
 * signed speed level (through zero = stop), ASC_STOP force-stops. Mirrors
 * the QMK ASC_UP/ASC_DOWN keycodes.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_flask_autoscroll

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <dt-bindings/flask/autoscroll.h>
#include <flask_autoscroll/flask_autoscroll.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_asc_binding_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case ASC_UP:
        flask_autoscroll_step(1);
        break;
    case ASC_DOWN:
        flask_autoscroll_step(-1);
        break;
    case ASC_STOP:
    default:
        flask_autoscroll_stop();
        break;
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_asc_binding_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_flask_autoscroll_driver_api = {
    .binding_pressed = on_asc_binding_pressed,
    .binding_released = on_asc_binding_released,
};

static int behavior_flask_autoscroll_init(const struct device *dev) { return 0; }

#define ASC_INST(n)                                                                                \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_flask_autoscroll_init, NULL, NULL, NULL, POST_KERNEL,      \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_flask_autoscroll_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ASC_INST)
