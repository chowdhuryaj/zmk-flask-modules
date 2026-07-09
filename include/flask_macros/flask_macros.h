/*
 * Runtime API for flask_macros — live-editable macros, the ZMK analog of
 * Vial's dynamic macros (proto channel 0x25).
 *
 * ZMK's own macros are const devicetree config and ZMK Studio has no macro
 * RPC. This module keeps a runtime step table (16 slots x 16 steps) played
 * back on the system workqueue: tap/press/release steps raise
 * zmk_keycode_state_changed from an encoded HID usage (keycode + modifiers —
 * the same encoding flask_combos outputs use), wait steps pause. Global
 * tap-ms / wait-ms pacing mirrors ZMK's devicetree macro defaults.
 *
 * Trigger paths: the &fmac <slot> behavior (Studio-assignable) and the
 * protocol's live-state value (the app's "play" button). One macro plays at
 * a time; a trigger while busy is ignored. Stop (or the end of playback)
 * releases anything a press step left held, so a stopped macro can't wedge
 * a key down.
 *
 * Central-only on splits: playback raises keycode events on the half that
 * owns the HID endpoint.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

/* Capacities are Kconfig-driven (ZMK is storage-bound, not Vial-bound);
 * the wire advertises both so the app never hardcodes them. */
#define FLASK_MACROS_SLOTS CONFIG_ZMK_FLASK_MACROS_SLOTS
#define FLASK_MACROS_STEPS CONFIG_ZMK_FLASK_MACROS_STEPS

/* Step actions. EMPTY ends playback — steps after the first empty never
 * run, so deleting a step from the app means compacting the tail up. */
enum flask_macro_action {
    FLASK_MACRO_ACTION_EMPTY = 0,
    FLASK_MACRO_ACTION_TAP = 1,     /* param = encoded usage */
    FLASK_MACRO_ACTION_PRESS = 2,   /* param = encoded usage */
    FLASK_MACRO_ACTION_RELEASE = 3, /* param = encoded usage */
    FLASK_MACRO_ACTION_WAIT = 4,    /* param = milliseconds */
};

struct flask_macro_step {
    uint8_t action;
    uint32_t param;
} __packed;

bool flask_macros_enabled(void);
void flask_macros_set_enabled(bool on);

/* Global pacing, ms. Setters clamp (tap 1-500, wait 0-2000); defaults mirror
 * ZMK's devicetree macros (tap 30, wait 15). */
uint16_t flask_macros_tap_ms(void);
void flask_macros_set_tap_ms(uint16_t ms);
uint16_t flask_macros_wait_ms(void);
void flask_macros_set_wait_ms(uint16_t ms);

uint8_t flask_macros_slot_count(void);
uint8_t flask_macros_step_count(void);

/* Step access. set normalizes (unknown action = EMPTY, wait param clamped)
 * — -EINVAL on a bad index. */
int flask_macros_step_get(uint8_t slot, uint8_t step, struct flask_macro_step *out);
int flask_macros_step_set(uint8_t slot, uint8_t step, const struct flask_macro_step *in);

/* Playback. play is a no-op while another macro runs or the module is
 * disabled (-EBUSY / -EACCES); stop also releases held press-step usages.
 * playing_slot returns the active slot or -1. */
int flask_macros_play(uint8_t slot);
void flask_macros_stop(void);
int flask_macros_playing_slot(void);

/* Persist the live table (CMD_SAVE path; settings subtree "flask/macros":
 * "cfg" globals + one "s<idx>" used-steps-prefix entry per used slot,
 * empties deleted). */
int flask_macros_save(void);

/* Restore hook — called from flask_proto.c's "flask" settings handler for
 * every "macros..." entry; sub is the name past the subtree ("cfg",
 * "s<idx>", NULL for the retired v1 whole-table blob). Size/version
 * mismatches are ignored (table stays empty; defaults ARE empty). */
int flask_macros_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                  void *cb_arg);
