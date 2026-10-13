#pragma once
/* Both dispatcher nodes "exist"; their names resolve to the compat string. */
#define DT_STUB_zmk_behavior_flask_tapdance 1
#define DT_STUB_zmk_behavior_flask_adaptive 1
#define DT_HAS_COMPAT_STATUS_OKAY(c) DT_STUB_##c
#define DT_COMPAT_GET_ANY_STATUS_OKAY(c) #c
#define DEVICE_DT_NAME(n) n
/* Tap dance instances: &ftd, and a fixed-slot node "sdel_td" of the slot compat. */
#define DT_FOREACH_STATUS_OKAY(c, fn) DT_STUB_FOREACH_##c(fn)
#define DT_STUB_FOREACH_zmk_behavior_flask_tapdance(fn) fn("zmk_behavior_flask_tapdance")
#define DT_STUB_FOREACH_zmk_behavior_flask_tapdance_slot(fn) fn("sdel_td")
#define DT_STUB_zmk_behavior_key_press 1
#define DT_STUB_flask_tapdance_defaults 0
