/*
 * flask_tapdance, replay half. Separate file only for its LINK POSITION
 * (after flask_holdtap, before autoshift and the keymap): flask_holdtap
 * replays captured presses with ZMK_EVENT_RAISE_AT(..., its own listener),
 * so they skip flask_tapdance.c's listener. Without this, a dance started
 * by a replayed press is not interrupted by the next replayed press, and
 * its output lands after that key. Interrupting is idempotent, so presses
 * both listeners see are fine.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include <flask_tapdance/flask_tapdance.h>

static int tapdance_replay_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev && ev->state) {
        flask_tapdance_interrupt(ev->position, ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_tapdance_replay, tapdance_replay_listener);
ZMK_SUBSCRIPTION(flask_tapdance_replay, zmk_position_state_changed);
