/*
 * flask_layer_autoexit, press half. Separate file only for its LINK
 * POSITION (after flask_holdtap, before core hold-tap and the keymap); the
 * logic and the raw-key/layer tracking live in flask_layer_autoexit.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

void flask_layer_autoexit_press(uint32_t position); /* flask_layer_autoexit.c */

static int autoexit_press_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev && ev->state) {
        flask_layer_autoexit_press(ev->position);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_layer_autoexit_press, autoexit_press_listener);
ZMK_SUBSCRIPTION(flask_layer_autoexit_press, zmk_position_state_changed);
