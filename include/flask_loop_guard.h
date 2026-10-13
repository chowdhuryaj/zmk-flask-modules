/*
 * Recursion guards shared by flask_tapdance and flask_adaptive.
 *
 * Both engines fire a stored behavior at the key position that triggered
 * them. If that behavior is &ftd or &fak again, the engine re-enters
 * itself (or the other one) until the stack overflows. Two layers:
 *  - SET-time checks, store NONE: flask_tapdance refuses &ftd
 *    (flask_behavior_id_is_tapdance), flask_adaptive refuses both
 *    (flask_behavior_id_is_dispatcher). &fak is a legal dance output: fak
 *    can never fire &ftd/&fak back, so ftd -> fak ends there.
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

/* ONE depth counter shared by every engine (cap FLASK_GUARD_MAX_DEPTH
 * overall, so ftd -> fak -> ftd nests 2 frames, not 2 per engine). A weak
 * definition in the header merges into a single object at link time and
 * works whichever engines are built. ENTER returns false (and the caller
 * must skip the fire) once the cap is hit; releases never call it. */
uint8_t flask_guard_depth __attribute__((weak));

#define FLASK_GUARD_ENTER() (flask_guard_depth < FLASK_GUARD_MAX_DEPTH ? (++flask_guard_depth, true) : false)
#define FLASK_GUARD_LEAVE() (--flask_guard_depth)

static inline bool flask_behavior_id_is_tapdance(uint16_t id) {
    if (id == 0 || id == UINT16_MAX) {
        return false;
    }
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_flask_tapdance)
    return zmk_behavior_get_local_id(DEVICE_DT_NAME(
               DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_flask_tapdance))) == id;
#else
    return false;
#endif
}

static inline bool flask_behavior_id_is_dispatcher(uint16_t id) {
    if (id == 0 || id == UINT16_MAX) {
        return false;
    }
    if (flask_behavior_id_is_tapdance(id)) {
        return true;
    }
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_flask_adaptive)
    return zmk_behavior_get_local_id(DEVICE_DT_NAME(
               DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_flask_adaptive))) == id;
#else
    return false;
#endif
}
