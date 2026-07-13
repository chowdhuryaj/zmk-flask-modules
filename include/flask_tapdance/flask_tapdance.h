/*
 * Runtime API for flask_tapdance — live-editable tap dances (Flask channel
 * 0x28, ZMK line; the Vial dynamic tap-dance analog built on ZMK's native
 * tap-dance semantics).
 *
 * ZMK's own zmk,behavior-tap-dance is const devicetree config — every
 * dance is its own compiled behavior node. This module keeps a runtime
 * slot table instead: `&ftd <slot>` (one param, Studio-assignable with a
 * range picker) runs the slot's dance with the engine cloned from core
 * behavior_tap_dance.c — count taps inside the per-slot tapping term,
 * resolve on term expiry / max count / interrupting key, then fire the
 * output for that tap count with real press/release pairing (still held =
 * output held).
 *
 * Outputs are typed like combos v12: encoded HID usage, flask_macros
 * slot, or any Studio-addressable behavior by local id with two params —
 * fired with the dance's key position, so a hold-tap output behaves like
 * a real key at that position.
 *
 * Wire (channel 0x28): enabled 0x01, slot count 0x02 RO, taps-per-slot
 * 0x03 RO, step 0x50 payload-addressed [slot, tap, action, behavior u16
 * BE, p1 u32 BE, p2 u32 BE], per-slot config 0x51 payload-addressed
 * [slot, term u16 BE]. Central-only on splits.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

#define FLASK_TAPDANCE_SLOTS CONFIG_ZMK_FLASK_TAPDANCE_SLOTS
#define FLASK_TAPDANCE_TAPS CONFIG_ZMK_FLASK_TAPDANCE_TAPS

/* Typed output — same action vocabulary as flask_combos v12. */
enum flask_tapdance_action {
    FLASK_TD_OUT_NONE = 0,
    FLASK_TD_OUT_USAGE = 1,
    FLASK_TD_OUT_MACRO = 2,
    FLASK_TD_OUT_BEHAVIOR = 3,
};
#define FLASK_TD_OUT_MAX FLASK_TD_OUT_BEHAVIOR

struct flask_tapdance_output {
    uint8_t action;
    uint16_t behavior_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

/* One dance: tapping term + one output per tap count (index 0 = single
 * tap). The dance length is the contiguous configured prefix — the first
 * NONE output ends it (core tap-dance "behavior_count"). */
struct flask_tapdance_slot {
    uint16_t term_ms;
    struct flask_tapdance_output taps[FLASK_TAPDANCE_TAPS];
} __packed;

bool flask_tapdance_enabled(void);
void flask_tapdance_set_enabled(bool on);

uint8_t flask_tapdance_slot_count(void);
uint8_t flask_tapdance_tap_count(void);

int flask_tapdance_slot_get(uint8_t idx, struct flask_tapdance_slot *out);
int flask_tapdance_term_set(uint8_t idx, uint16_t term_ms);
int flask_tapdance_output_set(uint8_t idx, uint8_t tap, const struct flask_tapdance_output *in);

/* Engine entry points for the &ftd behavior driver. */
int flask_tapdance_pressed(uint8_t slot, uint32_t position, int64_t timestamp);
int flask_tapdance_released(uint8_t slot, uint32_t position, int64_t timestamp);

/* Persist via settings subtree "flask/tapdance" ("cfg" + "s<idx>" per
 * used slot). CMD_SAVE path — flask_save queue only. */
int flask_tapdance_save(void);

int flask_tapdance_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                    void *cb_arg);
