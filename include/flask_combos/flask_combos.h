/*
 * Runtime API for flask_combos — live-editable combos, the ZMK analog of
 * Vial's dynamic combo entries (proto channel 0x24).
 *
 * ZMK's own combos are const devicetree config; ZMK Studio has no combo RPC.
 * This module keeps a runtime slot table (32 slots x up to 4 key positions +
 * one encoded HID usage output) and matches it with the same capture engine
 * as zmk's app/src/combo.c: position events are captured while a combo
 * candidate is alive, released/re-raised on non-match, swallowed on match.
 * A matched combo raises zmk_keycode_state_changed from the slot's encoded
 * usage (keycode + modifiers — Vial output parity; no behavior plumbing).
 *
 * Coexists with devicetree combos: this module's listener runs before the
 * core combo listener (module sources link before app sources), so runtime
 * slots get first look and everything they release or re-raise still reaches
 * the core combo engine untouched. A runtime slot that duplicates a DT
 * combo's positions simply shadows it.
 *
 * Central-only on splits: the central raises position events for peripheral
 * keys too (absolute positions), and it owns the HID endpoint the output
 * keycode leaves through.
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
#define FLASK_COMBOS_SLOTS CONFIG_ZMK_FLASK_COMBOS_SLOTS
#define FLASK_COMBOS_KEYS CONFIG_ZMK_FLASK_COMBOS_KEYS
#define FLASK_COMBOS_POS_NONE 0xFF

/* One runtime combo: up to FLASK_COMBOS_KEYS key positions (0xFF = unused)
 * and the encoded HID usage it emits (ZMK keymap encoding: usage id bits
 * 0-15, usage page bits 16-23, modifiers bits 24-31). A slot is live when
 * usage != 0 and at least two positions are set. */
struct flask_combo_slot {
    uint8_t pos[FLASK_COMBOS_KEYS];
    uint32_t usage;
} __packed;

bool flask_combos_enabled(void);
void flask_combos_set_enabled(bool on);

/* Global candidate window, ms (Vial's combo term). Setter clamps. */
uint16_t flask_combos_timeout_ms(void);
void flask_combos_set_timeout_ms(uint16_t ms);

uint8_t flask_combos_slot_count(void);

/* Slot access. set normalizes (out-of-range positions become POS_NONE) and
 * rebuilds the position lookup. Return -EINVAL on a bad index. */
int flask_combos_slot_get(uint8_t idx, struct flask_combo_slot *out);
int flask_combos_slot_set(uint8_t idx, const struct flask_combo_slot *in);

/* Persist the live table (CMD_SAVE path; settings subtree "flask/combos":
 * "cfg" globals + one "s<idx>" entry per used slot, empties deleted). */
int flask_combos_save(void);

/* Restore hook — called from flask_proto.c's "flask" settings handler for
 * every "combos..." entry; sub is the name past the subtree ("cfg",
 * "s<idx>", NULL for the retired v1 whole-table blob). Size/version
 * mismatches are ignored (table stays empty; defaults ARE empty). */
int flask_combos_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                  void *cb_arg);
