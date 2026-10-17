/*
 * &osk behavior — one key, one chord per OS.
 *
 * &osk <mac> <pc>: press sends the Mac chord while the OS mode
 * (mctechnology17/zmk-switch-layout index, the same one &sw_layout toggles
 * and flask_csk reads) is Mac, the PC chord while it is PC. Both params are
 * &kp-encoded usages, modifiers included (LG(C), LC(C), ...). A zero param
 * sends nothing on that OS. Built without zmk-switch-layout, the key acts
 * as Mac.
 *
 * The chord picked at press is remembered per position, so a release after
 * the OS mode flipped mid-hold still releases what was pressed. Two HID
 * usage params with metadata make it assignable from ZMK Studio and the
 * Flask app's picker.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_flask_os_key

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SWITCH_LAYOUT_STATE)
#include <zmk_switch_layout/state.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* zmk-switch-layout index for PC (Totem keymap OS_PC); anything else = Mac. */
#define OSK_OS_PC 0
#define OSK_HELD_MAX 8

struct osk_held {
    uint32_t position;
    uint32_t usage;
    bool used;
};

static struct osk_held held[OSK_HELD_MAX];

static bool os_is_pc(void) {
#if IS_ENABLED(CONFIG_ZMK_SWITCH_LAYOUT_STATE)
    return zmk_switch_layout_get() == OSK_OS_PC;
#else
    return false;
#endif
}

static int on_osk_binding_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    uint32_t usage = os_is_pc() ? binding->param2 : binding->param1;
    if (!usage) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    for (int i = 0; i < OSK_HELD_MAX; i++) {
        if (!held[i].used) {
            held[i] = (struct osk_held){.position = event.position, .usage = usage, .used = true};
            break;
        }
    }
    return raise_zmk_keycode_state_changed_from_encoded(usage, true, event.timestamp);
}

static int on_osk_binding_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    for (int i = 0; i < OSK_HELD_MAX; i++) {
        if (held[i].used && held[i].position == event.position) {
            held[i].used = false;
            return raise_zmk_keycode_state_changed_from_encoded(held[i].usage, false,
                                                                event.timestamp);
        }
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata mac_values[] = {
    {.display_name = "Mac", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE},
};

static const struct behavior_parameter_value_metadata pc_values[] = {
    {.display_name = "Windows", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE},
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = mac_values,
    .param1_values_len = ARRAY_SIZE(mac_values),
    .param2_values = pc_values,
    .param2_values_len = ARRAY_SIZE(pc_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif

static const struct behavior_driver_api behavior_flask_os_key_driver_api = {
    .binding_pressed = on_osk_binding_pressed,
    .binding_released = on_osk_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define OSK_INST(n)                                                                                \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_flask_os_key_driver_api);

DT_INST_FOREACH_STATUS_OKAY(OSK_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
