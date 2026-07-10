/*
 * Runtime API for flask_gestures — live-editable mouse gestures, the ZMK
 * port of the QMK Flask pd_gestures module (proto channel 0x11).
 *
 * Ratchet model (drashna's pointing_device_gestures): while a gesture is
 * open (the &fges behavior held) ball motion is swallowed and one output
 * fires per ratchet-step of accumulated travel, in the dominant of 8
 * directions (E SE S SW W NW N NE — East then clockwise, mouse +y =
 * south). An empty diagonal falls back to the nearest cardinal, so a set
 * that only fills E/S/W/N behaves like 90° sectors.
 *
 * Direction outputs live in runtime SETS (QMK parity: 8 sets of 8
 * directions; the active set is a live wire value, and &fges 255 follows
 * it while &fges <n> pins a specific set). Outputs are typed
 * (flask_output.h): usage tap or flask_macros slot.
 *
 * Sets 0-3 seed AJ's Adept/Svalboard defaults (arrows / editing / media /
 * tab-nav) when no settings exist — the same table QMK's
 * gesture_set_defaults ships.
 *
 * Replaces kot149/zmk-mouse-gesture on the Imprint (stroke-pattern model
 * emulated sets by stroke count; this restores the QMK set model with live
 * set switching). Central-only on splits.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

#include <flask_output/flask_output.h>

#define FLASK_GESTURES_SETS CONFIG_ZMK_FLASK_GESTURES_SETS
#define FLASK_GESTURES_DIRS 8
#define FLASK_GESTURES_FOLLOW_ACTIVE 255 /* &fges param: use the active set */

bool flask_gestures_enabled(void);
void flask_gestures_set_enabled(bool on);

/* Counts-per-fire travel (QMK ratchet step, wire id 0x01). Clamped
 * 50..2000; default 150 (the benched stroke feel). */
uint16_t flask_gestures_ratchet_step(void);
void flask_gestures_set_ratchet_step(uint16_t step);

/* Which set &fges 255 uses (wire id 0x02). Clamped to the set count. */
uint8_t flask_gestures_active_set(void);
void flask_gestures_set_active_set(uint8_t set);

uint8_t flask_gestures_set_count(void);

int flask_gestures_output_get(uint8_t set, uint8_t dir, struct flask_output *out);
int flask_gestures_output_set(uint8_t set, uint8_t dir, const struct flask_output *in);

/* Behavior entry points. begin(FOLLOW_ACTIVE) resolves the live set. */
void flask_gestures_begin(uint8_t set);
void flask_gestures_end(void);

/* Persist ("flask/gestures": cfg + per-set entries). */
int flask_gestures_save(void);
int flask_gestures_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                    void *cb_arg);
