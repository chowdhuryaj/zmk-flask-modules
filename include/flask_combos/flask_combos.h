/*
 * Runtime API for flask_combos — live-editable combos, the ZMK analog of
 * Vial's dynamic combo entries (proto channel 0x24).
 *
 * ZMK's own combos are const devicetree config; ZMK Studio has no combo RPC.
 * This module keeps a runtime slot table (Kconfig-sized positions per slot)
 * and matches it with the same capture engine as zmk's app/src/combo.c:
 * position events are captured while a combo candidate is alive,
 * released/re-raised on non-match, swallowed on match.
 *
 * Outputs are TYPED since v12: a matched slot can (1) hold an encoded HID
 * usage for the combo's duration (Vial parity — press on match, release on
 * the first combo key up), (2) play a flask_macros slot, or (3) invoke ANY
 * Studio-addressable behavior by local id with two params (tap-holds,
 * layer keys — the behavior gets pressed on match and released on the
 * first combo key up, with the first combo position as its event
 * position, so hold-tap timing works).
 *
 * Coexists with devicetree combos: this module's listener runs before the
 * core combo listener (module sources link before app sources), so runtime
 * slots get first look and everything they release or re-raise still reaches
 * the core combo engine untouched. A runtime slot that duplicates a DT
 * combo's positions simply shadows it. The runtime enable flag governs ONLY
 * runtime slots — devicetree combos have no off switch.
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

/* Typed combo output (v12). USAGE holds param1 (ZMK keymap encoding:
 * usage id bits 0-15, page bits 16-23, modifiers bits 24-31) for the
 * combo's duration. MACRO plays flask_macros slot param1 on match.
 * BEHAVIOR invokes the Studio-addressable behavior `behavior_id`
 * (zmk_behavior_local_id_t) with param1/param2 — pressed on match,
 * released on the first combo key up. */
enum flask_combo_action {
    FLASK_COMBO_OUT_NONE = 0,
    FLASK_COMBO_OUT_USAGE = 1,
    FLASK_COMBO_OUT_MACRO = 2,
    FLASK_COMBO_OUT_BEHAVIOR = 3,
};
#define FLASK_COMBO_OUT_MAX FLASK_COMBO_OUT_BEHAVIOR

/* One runtime combo: up to FLASK_COMBOS_KEYS key positions (0xFF = unused)
 * plus a typed output. A slot is live when action != NONE and at least two
 * positions are set. */
struct flask_combo_slot {
    uint8_t pos[FLASK_COMBOS_KEYS];
    uint8_t action;
    uint16_t behavior_id;
    uint32_t param1;
    uint32_t param2;
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
