/*
 * Runtime API for flask_autoshift — global auto shift + retro shift (Flask
 * channel 0x2C, proto v19; QMK process_auto_shift.c semantics). Spec:
 * flask-autoshift-contract.md. Wire and engine notes: src/flask_autoshift.c.
 *
 * Central-only on splits. No ZMK patch: plain &kp keys are taken at the
 * position level, retro shift is a variant of the flask_holdtap engine.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

#define FLASK_AUTOSHIFT_TIMEOUT_MIN_MS 50
#define FLASK_AUTOSHIFT_TIMEOUT_MAX_MS 1000
#define FLASK_AUTOSHIFT_RETRO_LIMIT_MIN_MS 50
#define FLASK_AUTOSHIFT_RETRO_LIMIT_MAX_MS 5000

/* GROUPS bitmask. */
#define FLASK_AUTOSHIFT_GRP_ALPHA (1u << 0)   /* A-Z */
#define FLASK_AUTOSHIFT_GRP_NUMERIC (1u << 1) /* 1-0 */
#define FLASK_AUTOSHIFT_GRP_SYMBOLS (1u << 2) /* - = [ ] \ NonUS# ; ' ` , . / NonUS\ */
#define FLASK_AUTOSHIFT_GRP_TAB (1u << 3)
#define FLASK_AUTOSHIFT_GRP_ENTER (1u << 4)
#define FLASK_AUTOSHIFT_GRP_MASK 0x1F

struct flask_autoshift_cfg {
    bool enabled;
    uint16_t timeout_ms;
    uint8_t groups;
    bool modifiers, repeat, no_auto_repeat, retro;
    uint16_t retro_limit_ms; /* 0 = no limit */
};

void flask_autoshift_get(struct flask_autoshift_cfg *out);
/* Clamps (spec §2). Applies from the next key press. */
void flask_autoshift_set(const struct flask_autoshift_cfg *in);
/* Persist via settings "flask/autoshift" (one 8 B blob). flask_save queue only. */
int flask_autoshift_save(void);
int flask_autoshift_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg);

/* flask_proto's raw keystate hook: a key went down at `position`. Resolves
 * the pending key (any later press takes effect after it) and drops the
 * tap-then-hold memory of other keys. */
void flask_autoshift_flush(uint32_t position, int64_t ts);

/* Combo outputs (id = FLASK_AUTOSHIFT_ID_COMBO(slot)): true = taken. */
bool flask_autoshift_key_press(uint16_t id, uint32_t param, int64_t ts);
bool flask_autoshift_key_release(uint16_t id, int64_t ts);
#define FLASK_AUTOSHIFT_ID_COMBO(slot) ((uint16_t)(0x8000 | (slot)))

/* flask_holdtap retro shift. */
bool flask_autoshift_is_kp(const char *behavior_dev);
/* enabled && retro && tap_param eligible; fills the snapshot values. */
bool flask_autoshift_retro_snapshot(uint32_t tap_param, uint16_t *timeout_ms, uint16_t *limit_ms);
/* Held non-shift mods block shifting unless MODIFIERS is on. */
bool flask_autoshift_mods_allow(void);
/* The shifted form of an eligible param: the custom-shift pair when one is
 * live, else param | LS. */
uint32_t flask_autoshift_shifted(uint32_t param);
