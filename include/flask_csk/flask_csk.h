/*
 * Runtime API for flask_csk — custom shift keys, the ZMK analog of QMK's
 * custom_shift_keys (getreuer) that the Flask QMK families expose on
 * channel 0x16 (flaskproto.js CH.customShift).
 *
 * While a physical Shift is held, pressing a key whose BASE usage matches
 * a slot sends the slot's SHIFTED usage instead — with the held Shift
 * masked out of the HID report (mod-morph's masked-modifiers mechanism),
 * so the replacement types exactly what it encodes. The replacement's own
 * modifier bits apply normally:
 *
 *   base COMMA  → shifted SEMI       ⇧, types ;   (shift masked)
 *   base H      → shifted LS(R)      ⇧h types R   (its own shift)
 *   base BSPC   → shifted DEL        ⇧⌫ deletes forward
 *
 * The engine is a keycode-event hook, not a behavior — no keymap edits,
 * no per-key mod-morph nodes; it applies to whatever usage the keymap
 * emits (incl. hold-tap taps). Wire: shares the QMK channel's scalar ids
 * (enabled 0x01, slot count 0x02) and puts the ZMK slot frame at 0x50
 * ([slot, base u32 BE, shifted u32 BE] — ZMK keymap encoding: usage id
 * bits 0-15, page 16-23, modifiers 24-31), clear of QMK's u16 tables at
 * 0x10+/0x30+ which cannot carry 32-bit ZMK usages.
 *
 * Central-only on splits: the central owns the HID endpoint and sees
 * every keycode event.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

#define FLASK_CSK_SLOTS CONFIG_ZMK_FLASK_CSK_SLOTS

/* One custom shift pair (ZMK keymap encoding). A slot is live when both
 * base and shifted are nonzero. */
struct flask_csk_slot {
    uint32_t base;
    uint32_t shifted;
} __packed;

bool flask_csk_enabled(void);
void flask_csk_set_enabled(bool on);

uint8_t flask_csk_slot_count(void);

int flask_csk_slot_get(uint8_t idx, struct flask_csk_slot *out);
int flask_csk_slot_set(uint8_t idx, const struct flask_csk_slot *in);

/* Persist via settings subtree "flask/csk" ("cfg" + "s<idx>" per used
 * slot). CMD_SAVE path — runs on the flask_save queue only. */
int flask_csk_save(void);

int flask_csk_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                               void *cb_arg);
