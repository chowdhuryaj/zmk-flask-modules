/*
 * Runtime API for flask_holdtap — hold-tap timing editable per KEY
 * POSITION (Flask channel 0x2A, proto v17). Wire contract: the comment
 * block at the top of src/behavior_flask_holdtap.c.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

/* Flavor ids = core hold-tap's DT enum order (wire values too). */
enum flask_holdtap_flavor {
    FLASK_HT_HOLD_PREFERRED = 0,
    FLASK_HT_BALANCED = 1,
    FLASK_HT_TAP_PREFERRED = 2,
    FLASK_HT_TAP_UNLESS_INTERRUPTED = 3,
};
#define FLASK_HT_FLAVOR_MAX FLASK_HT_TAP_UNLESS_INTERRUPTED

#define FLASK_HT_TERM_MIN_MS 50
#define FLASK_HT_TERM_MAX_MS 1000
#define FLASK_HT_IDLE_MAX_MS 1000 /* quick-tap and prior-idle ceiling; 0 = off */

/* One slot = one key position's timing, or a virtual slot's. */
struct flask_holdtap_timing {
    uint16_t term_ms;
    uint16_t quick_tap_ms;
    uint16_t prior_idle_ms;
    uint8_t flavor;
} __packed;

uint8_t flask_holdtap_slot_count(void);

/* Slot kind: *key_pos = the key position for a physical slot, 0xFF for a
 * virtual one (past the keymap). name gets the defaults child's
 * display-name, NUL-padded (empty when none). -EINVAL past the count. */
int flask_holdtap_slot_info(uint8_t slot, uint8_t *key_pos, char *name, size_t name_len);

/* Live value / compiled default for a slot. -EINVAL past the slot count. */
int flask_holdtap_get(uint8_t slot, struct flask_holdtap_timing *out);
int flask_holdtap_default_get(uint8_t slot, struct flask_holdtap_timing *out);

/* Clamps term/quick/idle; -EINVAL for a bad slot or flavor (nothing
 * applied). term_ms == 0 resets the slot to its compiled default. */
int flask_holdtap_set(uint8_t slot, const struct flask_holdtap_timing *in);

/* Persist via settings subtree "flask/holdtap" ("p<slot>" for each slot
 * that differs from its compiled default). flask_save queue only. */
int flask_holdtap_save(void);

int flask_holdtap_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                   void *cb_arg);
