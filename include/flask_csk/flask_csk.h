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
 * Full mod-morph (no proto bump, MORPH_CAPS 0x03 = 1): each slot also
 * carries a trigger modifier SET (side-agnostic bit0 Ctrl, bit1 Shift,
 * bit2 Alt, bit3 GUI; 0 = Shift, so old 8-byte frames and saved entries
 * stay Shift slots) and flags (bit0 keep = trigger mods stay in the
 * report). A slot fires only when the held explicit mods, folded left|right,
 * EQUAL its set: Shift+, and Ctrl+Shift+, are separate slots. Only the
 * trigger's mods (both sides) are masked. Frame grows to [.., mods, flags].
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

#define FLASK_CSK_TRIGGER_SHIFT 0x02 /* what mods == 0 means */
#define FLASK_CSK_FLAG_KEEP 0x01

/* One custom shift pair (ZMK keymap encoding). A slot is live when both
 * base and shifted are nonzero. mods = trigger set (0 reads as Shift),
 * flags = FLASK_CSK_FLAG_*. slot_set refuses mods bits 4-7 / unknown flags. */
struct flask_csk_slot {
    uint32_t base;
    uint32_t shifted;
    uint8_t mods;
    uint8_t flags;
} __packed;

bool flask_csk_enabled(void);
void flask_csk_set_enabled(bool on);

uint8_t flask_csk_slot_count(void);

int flask_csk_slot_get(uint8_t idx, struct flask_csk_slot *out);
int flask_csk_slot_set(uint8_t idx, const struct flask_csk_slot *in);

/* Persist via settings subtree "flask/csk" ("cfg" + "s<idx>" per used
 * slot; Shift/no-flag slots keep the old 8-byte blob, others write 10). CMD_SAVE path — runs on the flask_save queue only. */
int flask_csk_save(void);

int flask_csk_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                               void *cb_arg);
