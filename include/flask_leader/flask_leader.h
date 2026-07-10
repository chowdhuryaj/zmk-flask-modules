/*
 * Runtime API for flask_leader — live-editable leader sequences, the ZMK
 * analog of the QMK Flask leader table (proto channel 0x19).
 *
 * A press of the &fled behavior opens a capture: subsequent key POSITIONS
 * are swallowed and matched prefix-wise against the runtime table. An
 * unambiguous full match fires the slot's typed output (usage tap or
 * flask_macros slot — flask_output.h); a key that matches nothing, or the
 * per-key timeout, ends the capture (a timeout on an exact-but-extendable
 * match fires it, QMK leader semantics). Sequence keys never type.
 *
 * Coexists with urob's compile-time leader key (different trigger key) —
 * the compiled email/USB/BLE sequences stay untouched; this module is the
 * app-editable path.
 *
 * Central-only on splits (the central sees both halves' positions).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

#include <flask_output/flask_output.h>

#define FLASK_LEADER_SLOTS CONFIG_ZMK_FLASK_LEADER_SLOTS
#define FLASK_LEADER_KEYS CONFIG_ZMK_FLASK_LEADER_KEYS
#define FLASK_LEADER_POS_NONE 0xFF

/* One leader sequence: up to FLASK_LEADER_KEYS positions (0xFF = unused,
 * sequences are the leading prefix) firing a typed output. A slot is live
 * when it has >= 1 position and a non-empty output. */
struct flask_leader_slot {
    uint8_t pos[FLASK_LEADER_KEYS];
    struct flask_output out;
} __packed;

bool flask_leader_enabled(void);
void flask_leader_set_enabled(bool on);

/* Per-key capture timeout, ms (QMK leaderTimeout, wire id 0x01). Clamped
 * 100..5000; default 1000. */
uint16_t flask_leader_timeout_ms(void);
void flask_leader_set_timeout_ms(uint16_t ms);

uint8_t flask_leader_slot_count(void);

int flask_leader_slot_get(uint8_t idx, struct flask_leader_slot *out);
int flask_leader_slot_set(uint8_t idx, const struct flask_leader_slot *in);

/* Behavior entry point: open (or re-arm) a capture. */
void flask_leader_begin(void);

/* Persist ("flask/leader": cfg + per-slot entries, empties deleted). */
int flask_leader_save(void);
int flask_leader_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                  void *cb_arg);
