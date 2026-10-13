#pragma once
/* Both dispatcher nodes "exist"; their names resolve to the compat string. */
#define DT_STUB_zmk_behavior_flask_tapdance 1
#define DT_STUB_zmk_behavior_flask_adaptive 1
#define DT_HAS_COMPAT_STATUS_OKAY(c) DT_STUB_##c
#define DT_COMPAT_GET_ANY_STATUS_OKAY(c) #c
#define DEVICE_DT_NAME(n) n
