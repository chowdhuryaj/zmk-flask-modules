/*
 * Runtime API for flask_rgb — per-key per-layer RGB map, the ZMK analog of
 * the QMK Flask firmwares' RGBMAP feature (channel 0x21).
 *
 * The map is [layers][total_leds] of HSV triples (QMK convention: each
 * component 0-255; V of 0 = LED off). Both split halves hold the full map;
 * each renders only its own [led_offset, led_offset + led_count) slice.
 * Rendering shows the highest active layer's colors — the central half reads
 * ZMK layer state, the peripheral renders the layer bitmap the central
 * pushes over the module's split GATT characteristic (flask_rgb_split_*.c),
 * which also carries live map edits and a bulk resync on connect.
 *
 * Only the central persists (settings key "flask/rgbmap"); the peripheral is
 * RAM-only and rehydrated by the connect-time bulk sync.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

/* Map dimensions (DT: layers / total-leds). */
uint8_t flask_rgb_layers(void);
uint16_t flask_rgb_total_leds(void);

/* Map access. set/fill re-render and (central) forward to the peripheral.
 * Return -EINVAL out of range, -ENODEV before init. hsv = {h, s, v}. */
int flask_rgb_get_led(uint8_t layer, uint16_t led, uint8_t hsv[3]);
int flask_rgb_set_led(uint8_t layer, uint16_t led, const uint8_t hsv[3]);
int flask_rgb_fill(uint8_t layer, const uint8_t hsv[3]);

/* Map enable (the &frgb behavior + channel 0x21 value 0x01). */
bool flask_rgb_enabled(void);
void flask_rgb_set_enabled(bool on);

/* Persist the live map + enabled flag (central; CMD_SAVE path). */
int flask_rgb_save(void);

/* Restore hook — called from flask_proto.c's "flask" settings handler for
 * the "rgbmap" leaf. Shape mismatches are ignored (map reseeds dark). */
int flask_rgb_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg);

/* --- split-sync internal surface (module-private in spirit) --- */

/* Peripheral ingest: called by the GATT write handler. */
void flask_rgb_sync_layers(uint32_t layer_bitmap);
void flask_rgb_sync_led(uint8_t layer, uint16_t led, const uint8_t hsv[3]);
void flask_rgb_sync_enabled(bool on);
void flask_rgb_sync_fill(uint8_t layer, const uint8_t hsv[3]);

/* Central egress: implemented by flask_rgb_split_central.c (weak no-ops
 * otherwise) — forward state to the peripheral. */
void flask_rgb_split_send_layers(uint32_t layer_bitmap);
void flask_rgb_split_send_led(uint8_t layer, uint16_t led, const uint8_t hsv[3]);
void flask_rgb_split_send_enabled(bool on);
void flask_rgb_split_send_fill(uint8_t layer, const uint8_t hsv[3]);

/* Full-map walk for the connect-time bulk sync. */
void flask_rgb_bulk_resync(void);
