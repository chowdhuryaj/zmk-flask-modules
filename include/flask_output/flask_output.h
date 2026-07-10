/*
 * Typed runtime output — the one thing a leader sequence or a gesture
 * direction FIRES. Shared by flask_leader and flask_gestures (and any
 * future runtime-slot module) so the app edits one shape everywhere:
 *
 *   action 0 = none (empty slot)
 *   action 1 = tap an encoded usage (ZMK keymap encoding: usage id bits
 *              0-15, usage page bits 16-23, implicit modifiers bits 24-31 —
 *              the same encoding flask_combos outputs and flask_macros
 *              steps use)
 *   action 2 = play flask_macros slot <param> (needs CONFIG_ZMK_FLASK_MACROS;
 *              ignored otherwise)
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
#include <flask_macros/flask_macros.h>
#endif

enum flask_output_action {
    FLASK_OUTPUT_NONE = 0,
    FLASK_OUTPUT_USAGE = 1,
    FLASK_OUTPUT_MACRO = 2,
};
#define FLASK_OUTPUT_ACTION_MAX FLASK_OUTPUT_MACRO

struct flask_output {
    uint8_t action;
    uint32_t param;
} __packed;

/** Fire one output. Usage taps raise press+release back-to-back (the same
 * synchronous raise pair core behaviors use); macro plays are best-effort
 * (busy/disabled engines just ignore them). */
static inline void flask_output_fire(const struct flask_output *out, int64_t timestamp) {
    switch (out->action) {
    case FLASK_OUTPUT_USAGE:
        if (out->param != 0) {
            raise_zmk_keycode_state_changed_from_encoded(out->param, true, timestamp);
            raise_zmk_keycode_state_changed_from_encoded(out->param, false, timestamp);
        }
        break;
    case FLASK_OUTPUT_MACRO:
#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
        flask_macros_play((uint8_t)out->param);
#endif
        break;
    default:
        break;
    }
}

/** Normalize an app-written output in place (unknown action = empty). */
static inline void flask_output_normalize(struct flask_output *out) {
    if (out->action > FLASK_OUTPUT_ACTION_MAX) {
        out->action = FLASK_OUTPUT_NONE;
    }
    if (out->action == FLASK_OUTPUT_NONE) {
        out->param = 0;
    }
}
