/*
 * Runtime API for flask_adaptive — live-editable adaptive keys (Flask
 * channel 0x2B, ZMK line; the QMK repeat/alt-repeat idea with app-edited
 * rules, semantics ported from urob/zmk-adaptive-key).
 *
 * `&fak <set>` fires an output that depends on the last key typed before
 * it. A SET is a fallback output plus the rules of the shared rule pool
 * whose `set` field equals it. A RULE is a trigger key (+ optional exact
 * modifiers flag) + a max idle time + an output. An OUTPUT is a sequence
 * of 1..FLASK_ADAPTIVE_STEPS typed steps (the tap-dance / combos-v2
 * typed-output shape: usage / flask_macros slot / behavior by local id).
 *
 * Differences from urob's behavior (spec: flask-adaptive-contract.md §3):
 * modifier keys never count as the last key, right modifiers fold onto
 * left when comparing, and the rule table is runtime data.
 *
 * Central-only on splits.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

#include <zmk/behavior.h>

#define FLASK_ADAPTIVE_SETS CONFIG_ZMK_FLASK_ADAPTIVE_SETS
#define FLASK_ADAPTIVE_RULES CONFIG_ZMK_FLASK_ADAPTIVE_RULES
#define FLASK_ADAPTIVE_STEPS CONFIG_ZMK_FLASK_ADAPTIVE_STEPS

/* Typed step — same action vocabulary as flask_tapdance / flask_combos. */
enum flask_adaptive_action {
    FLASK_AK_OUT_NONE = 0,
    FLASK_AK_OUT_USAGE = 1,
    FLASK_AK_OUT_MACRO = 2,
    FLASK_AK_OUT_BEHAVIOR = 3,
};
#define FLASK_AK_OUT_MAX FLASK_AK_OUT_BEHAVIOR

struct flask_adaptive_step {
    uint8_t action;
    uint16_t behavior_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

#define FLASK_AK_FLAG_EXACT 0x01 /* urob strict-modifiers */
#define FLASK_AK_IDLE_MAX_MS 10000

/* Trigger = ZMK keymap encoding: usage id bits 0-15, page bits 16-23
 * (0 = keyboard page 7), modifiers bits 24-31. 0 = the rule is deleted.
 * A rule is live when trigger != 0 and steps[0].action != NONE; its
 * output is the contiguous prefix of steps up to the first NONE. */
struct flask_adaptive_rule {
    uint8_t set;
    uint32_t trigger;
    uint16_t max_idle_ms; /* 0 = any time */
    uint8_t flags;
    struct flask_adaptive_step steps[FLASK_ADAPTIVE_STEPS];
} __packed;

bool flask_adaptive_enabled(void);
void flask_adaptive_set_enabled(bool on);

uint8_t flask_adaptive_set_count(void);
uint8_t flask_adaptive_rule_count(void);
uint8_t flask_adaptive_step_count(void);

int flask_adaptive_rule_get(uint8_t idx, struct flask_adaptive_rule *out);

/* Header write. trigger 0 deletes the rule (all fields and steps zeroed,
 * `set` is not checked); otherwise set must be < SETS. Normalizes (page
 * 0 -> 7, idle clamped, flags bit0 only). -EINVAL on a bad index / set. */
int flask_adaptive_rule_set(uint8_t idx, uint8_t set, uint32_t trigger, uint16_t max_idle_ms,
                            uint8_t flags);

/* Step write (flask_tapdance_output_set normalization, plus a behavior
 * that is &fak itself becomes NONE). */
int flask_adaptive_step_set(uint8_t rule, uint8_t step, const struct flask_adaptive_step *in);

int flask_adaptive_fallback_get(uint8_t set, struct flask_adaptive_step *out);
int flask_adaptive_fallback_set(uint8_t set, const struct flask_adaptive_step *in);

/* Engine entry points for the &fak behavior driver. */
int flask_adaptive_pressed(uint8_t set, struct zmk_behavior_binding_event event);
int flask_adaptive_released(uint8_t set, struct zmk_behavior_binding_event event);

/* Persist via settings subtree "flask/adaptive": "cfg", "r<idx>" per used
 * rule, "f<set>" per used fallback. CMD_SAVE path — flask_save queue only. */
int flask_adaptive_save(void);

int flask_adaptive_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                    void *cb_arg);

/* Post-settings-load hook: fills every default rule / fallback index (from
 * the keymap's `flask,adaptive-defaults` node) that has no saved entry —
 * a saved edit or tombstoned deletion wins. Returns how many default
 * bindings still lack a behavior local id; the caller retries while
 * nonzero. Idempotent. */
int flask_adaptive_defaults_commit(void);
