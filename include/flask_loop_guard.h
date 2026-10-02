/*
 * Recursion guards shared by flask_tapdance and flask_adaptive.
 *
 * Both engines fire a stored behavior at the key position that triggered
 * them. If that behavior is &ftd or &fak again, the engine re-enters
 * itself (or the other one) until the stack overflows. Two layers:
 *  - flask_behavior_id_is_dispatcher(): SET-time check, store NONE.
 *  - FLASK_GUARD_ENTER/LEAVE: runtime depth cap, so a stale saved table
 *    (or a chain this check does not know about) cannot loop either.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zmk/behavior.h>

#define FLASK_GUARD_MAX_DEPTH 2

/* Each engine owns one `static uint8_t depth`. ENTER returns false (and
 * the caller must skip the fire) once the cap is hit. */
#define FLASK_GUARD_ENTER(depth) ((depth) < FLASK_GUARD_MAX_DEPTH ? (++(depth), true) : false)
#define FLASK_GUARD_LEAVE(depth) (--(depth))

static inline bool flask_behavior_id_is_dispatcher(uint16_t id) {
    zmk_behavior_local_id_t found;

    if (id == 0 || id == UINT16_MAX) {
        return false;
    }
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_flask_tapdance)
    found = zmk_behavior_get_local_id(
        DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_flask_tapdance)));
    if (found == id) {
        return true;
    }
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_flask_adaptive)
    found = zmk_behavior_get_local_id(
        DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_flask_adaptive)));
    if (found == id) {
        return true;
    }
#endif
    (void)found;
    return false;
}
