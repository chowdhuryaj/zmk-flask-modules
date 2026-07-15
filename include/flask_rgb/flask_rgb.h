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

/* Split-link diagnosis (channel 0x21 value 0x09 RO): true once the central
 * has discovered the peripheral's flask_rgb characteristic. */
bool flask_rgb_split_link_ready(void);
void flask_rgb_set_enabled(bool on);

/* Global brightness percent 0-100 (channel 0x21 value 0x0B, proto v14 —
 * the native rgb_underglow BRI knob's analog). Scales EVERY rendered
 * pixel — painted map, effect, and reactive overlay — on both halves
 * (synced over the split link); persisted in the rgbmap blob. */
uint8_t flask_rgb_brightness(void);
void flask_rgb_set_brightness(uint8_t percent);

/* Idle blank timeout in SECONDS (channel 0x21 value 0x0C, proto v16).
 *
 * The strip renders dark once ZMK's activity state leaves ACTIVE, which by
 * default is CONFIG_ZMK_IDLE_TIMEOUT = 30 s after the last KEY/ball input.
 * App writes still land while dark — the map updates and simply is not
 * rendered — which reads as "RGB updates lag, if at all" when the keyboard
 * is idle (2026-07-15: AJ, painting from a workstation whose own mouse he
 * was using).
 *
 * 0 = never blank (stays lit until deep sleep). Values below
 * CONFIG_ZMK_IDLE_TIMEOUT/1000 cannot be honoured — that event is the
 * earliest signal we get — so they behave as that floor.
 *
 * Synced to the peripheral (each half runs its own activity clock) and
 * persisted in its OWN settings entry, "flask/rgbidle", so the 2.1 KB map
 * blob keeps its shape and needs no migration (same reasoning as
 * "flask/ledorder"). */
#define FLASK_RGB_IDLE_NEVER 0
uint16_t flask_rgb_idle_timeout(void);
void flask_rgb_set_idle_timeout(uint16_t seconds);
int flask_rgb_idle_restore(size_t len, settings_read_cb read_cb, void *cb_arg);

/* Whole-strip effect engine (channel 0x21 values 0x04-0x08, proto v9).
 * Painted map keys (V > 0) OVERLAY the effect — same layering the QMK
 * NLKB16 tab describes. Effects: 0 off, 1 solid, 2 breathe, 3 spectrum,
 * 4 swirl. Speed 1-255 scales the animation clock; the effect HSV is the
 * base color (solid/breathe) or the S/V floor (spectrum/swirl). */
enum flask_rgb_effect {
    FLASK_RGB_EFFECT_OFF = 0,
    FLASK_RGB_EFFECT_SOLID = 1,
    FLASK_RGB_EFFECT_BREATHE = 2,
    FLASK_RGB_EFFECT_SPECTRUM = 3,
    FLASK_RGB_EFFECT_SWIRL = 4,
};
#define FLASK_RGB_EFFECT_MAX FLASK_RGB_EFFECT_SWIRL

uint8_t flask_rgb_effect(void);
void flask_rgb_set_effect(uint8_t effect);
uint8_t flask_rgb_effect_speed(void);
void flask_rgb_set_effect_speed(uint8_t speed);
void flask_rgb_effect_hsv(uint8_t hsv[3]);
void flask_rgb_set_effect_hsv(const uint8_t hsv[3]);

/* Persist the live map + enabled flag (central; CMD_SAVE path). */
int flask_rgb_save(void);

/* Restore hook — called from flask_proto.c's "flask" settings handler for
 * the "rgbmap" leaf. Shape mismatches are ignored (map reseeds dark). */
int flask_rgb_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg);

/* Runtime LED→keymap-position order (v12, wire value 0x0A): the app's
 * measured map, chunked reads/writes. 0xFF = no key under that LED.
 * Setter rebuilds the overlay's position→LED table live; persisted by
 * flask_rgb_save as "flask/ledorder". Both return the count served, or
 * -EINVAL past the end. */
int flask_rgb_led_order_get(uint16_t start, uint8_t *out, uint8_t count);
int flask_rgb_led_order_set(uint16_t start, const uint8_t *pos, uint8_t count);

/* Restore hook for the "ledorder" leaf. */
int flask_rgb_ledorder_restore(size_t len, settings_read_cb read_cb, void *cb_arg);

/* --- split-sync internal surface (module-private in spirit) --- */

/* Peripheral ingest: called by the GATT write handler. */
void flask_rgb_sync_layers(uint32_t layer_bitmap);
void flask_rgb_sync_led(uint8_t layer, uint16_t led, const uint8_t hsv[3]);
void flask_rgb_sync_enabled(bool on);
void flask_rgb_sync_fill(uint8_t layer, const uint8_t hsv[3]);
void flask_rgb_sync_effect(uint8_t effect, uint8_t speed, const uint8_t hsv[3], uint16_t phase);
void flask_rgb_sync_brightness(uint8_t percent);
void flask_rgb_sync_idle_timeout(uint16_t seconds);

/* Effect state snapshot for the central's egress (params + current phase,
 * so the peripheral's animation clock re-anchors on every sync). */
void flask_rgb_effect_snapshot(uint8_t *effect, uint8_t *speed, uint8_t hsv[3], uint16_t *phase);

/* Central egress: implemented by flask_rgb_split_central.c (weak no-ops
 * otherwise) — forward state to the peripheral. */
void flask_rgb_split_send_layers(uint32_t layer_bitmap);
void flask_rgb_split_send_led(uint8_t layer, uint16_t led, const uint8_t hsv[3]);
void flask_rgb_split_send_enabled(bool on);
void flask_rgb_split_send_fill(uint8_t layer, const uint8_t hsv[3]);
void flask_rgb_split_send_effect(void);
void flask_rgb_split_send_brightness(uint8_t percent);
void flask_rgb_split_send_idle_timeout(uint16_t seconds);

/* Full-map walk for the connect-time bulk sync. */
void flask_rgb_bulk_resync(void);

/* --- reactive overlay (2026-07-10) ---
 *
 * A transient one-color highlight rendered ABOVE the painted map and the
 * effect: position-addressed (keymap positions, resolved to LEDs via the
 * node's key-positions map), never persisted, cleared as a whole. Built for
 * reactive UI cues — flask_leader lights the candidate next keys during a
 * capture. Central-side calls sync to the peripheral automatically. */
void flask_rgb_overlay_positions(const uint8_t *positions, uint8_t count, const uint8_t hsv[3]);
void flask_rgb_overlay_clear(void);

/* Overlay snapshot for the central's egress + peripheral ingest.
 * mask = total-leds bits, byte i/8 bit i%8. */
#define FLASK_RGB_OVERLAY_MASK_BYTES(total) (((total) + 7) / 8)
void flask_rgb_overlay_snapshot(uint8_t *on, uint8_t hsv[3], uint8_t *mask, size_t mask_len);
void flask_rgb_sync_overlay(uint8_t on, const uint8_t hsv[3], const uint8_t *mask, size_t mask_len);
void flask_rgb_split_send_overlay(void);
