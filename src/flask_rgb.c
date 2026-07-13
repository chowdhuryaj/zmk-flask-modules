/*
 * flask_rgb core — per-key per-layer RGB map, both split halves.
 *
 * Holds the FULL [layers][total_leds] HSV map on each half and renders only
 * the local [offset, offset + led_count) slice to this half's strip. The
 * rendered layer is the highest active one: the central reads ZMK layer
 * state directly (and pushes it to the peripheral via the split GATT egress
 * fns, weak no-ops off-central); the peripheral renders whatever layer the
 * central last synced.
 *
 * V of 0 = LED off (QMK RGBMAP convention). The whole strip blanks while
 * the map is disabled and on activity idle/sleep (battery — same rationale
 * as underglow's auto-off-idle). CONFIG_ZMK_RGB_UNDERGLOW must stay OFF on
 * builds using this module: both would drive the same strip.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT flask_rgb

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/activity.h>

/* Strip power rail: boards like the Cyboard assimilator-bt gate LED VCC
 * behind ZMK's ext-power driver, whose OFF state persists in settings and
 * SURVIVES REFLASHING — firmware that renders perfectly still shows a dark
 * board (bench 2026-07-11). When the board has such a node, kick it ON
 * once settings have loaded; the driver saves the new state itself. */
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
#include <drivers/ext_power.h>
#define FRGB_HAS_EXT_POWER 1
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/keymap.h>
#include <zmk/events/layer_state_changed.h>
#define FLASK_RGB_HAS_LAYER_STATE 1
#endif

#include <flask_rgb/flask_rgb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define FRGB_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(flask_rgb)
#define FRGB_LAYERS DT_PROP(FRGB_NODE, layers)
#define FRGB_TOTAL DT_PROP(FRGB_NODE, total_leds)
#define FRGB_LOCAL DT_PROP(FRGB_NODE, led_count)
#define FRGB_OFFSET CONFIG_ZMK_FLASK_RGB_LED_OFFSET

BUILD_ASSERT(FRGB_LAYERS >= 1 && FRGB_LAYERS <= 32, "flask,rgb layers must be 1..32");
BUILD_ASSERT(FRGB_TOTAL >= FRGB_LOCAL, "flask,rgb total-leds < led-count");
BUILD_ASSERT(FRGB_OFFSET + FRGB_LOCAL <= FRGB_TOTAL,
             "flask,rgb: led-offset + led-count exceeds total-leds");

static const struct device *const frgb_strip = DEVICE_DT_GET(DT_PHANDLE(FRGB_NODE, led_strip));

static struct k_spinlock frgb_lock;
static uint8_t frgb_map[FRGB_LAYERS][FRGB_TOTAL][3];
static bool frgb_enabled = true;
static uint8_t frgb_brightness = 100; /* percent, scales every rendered pixel (v14) */
static uint8_t frgb_layer;      /* rendered layer (central: mirrors keymap) */
static bool frgb_awake = true;  /* activity gate */

/* Whole-strip effect (v9): painted map keys overlay it. The phase clock
 * advances by `speed` per animation tick; both halves run the same clock
 * and the central re-anchors the peripheral's phase on every effect sync. */
static uint8_t frgb_effect;         /* enum flask_rgb_effect */
static uint8_t frgb_effect_speed = 128;
static uint8_t frgb_effect_col[3] = {0, 255, 120};
static uint16_t frgb_phase;

/* Reactive overlay (2026-07-10): one color over a set of LEDs, rendered
 * ABOVE map + effect. Transient (never persisted); flask_leader lights the
 * candidate next keys with it. Position-addressed at the API, resolved to
 * LEDs via the node's key-positions map (identity when absent). */
#define FRGB_MASK_BYTES FLASK_RGB_OVERLAY_MASK_BYTES(FRGB_TOTAL)
static uint8_t frgb_overlay_mask[FRGB_MASK_BYTES];
static uint8_t frgb_overlay_col[3];
static bool frgb_overlay_on;

#if DT_NODE_HAS_PROP(FRGB_NODE, key_positions)
static const uint8_t frgb_led_pos[] = DT_PROP(FRGB_NODE, key_positions);
BUILD_ASSERT(ARRAY_SIZE(frgb_led_pos) == FRGB_TOTAL,
             "flask,rgb key-positions must have total-leds entries");
#endif

/* LED → keymap position, RUNTIME-editable since v12 (wire value 0x0A):
 * the app's "Map LEDs → keys" wizard pushes the measured order here, so
 * the reactive overlay (leader candidate lighting) is correct without a
 * keymap edit + reflash. Seeded from the node's key-positions property
 * (identity without it); persisted as its own settings entry
 * ("flask/ledorder") on SAVE. 0xFF = no key under that LED (underglow). */
BUILD_ASSERT(FRGB_TOTAL <= 255, "flask,rgb overlay table indexes LEDs as bytes");
static uint8_t frgb_led_order[FRGB_TOTAL];

/* position → LED (0xFF = no LED under that position); rebuilt whenever
 * frgb_led_order changes. */
static uint8_t frgb_pos2led[256];

/* Callers hold frgb_lock (or run before threads at init). */
static void frgb_rebuild_pos2led(void) {
    memset(frgb_pos2led, 0xFF, sizeof(frgb_pos2led));
    for (uint16_t led = 0; led < FRGB_TOTAL; led++) {
        if (frgb_led_order[led] != 0xFF) {
            frgb_pos2led[frgb_led_order[led]] = led;
        }
    }
}

static struct led_rgb frgb_pixels[FRGB_LOCAL];
static struct k_work frgb_render_work;
static struct k_work_delayable frgb_anim_work;

#define FRGB_ANIM_TICK_MS 40 /* 25 fps */

static bool frgb_effect_animates(uint8_t effect) {
    return effect == FLASK_RGB_EFFECT_BREATHE || effect == FLASK_RGB_EFFECT_SPECTRUM ||
           effect == FLASK_RGB_EFFECT_SWIRL;
}

/* Split-link diagnosis (channel 0x21 value 0x09 RO): has the central found
 * the peripheral's flask_rgb characteristic? Overridden by
 * flask_rgb_split_central.c; false elsewhere (a peripheral answering the
 * protocol would be a config error anyway). */
__weak bool flask_rgb_split_link_ready(void) { return false; }

/* --- weak split egress: overridden by flask_rgb_split_central.c --- */

__weak void flask_rgb_split_send_layers(uint32_t layer_bitmap) { ARG_UNUSED(layer_bitmap); }
__weak void flask_rgb_split_send_led(uint8_t layer, uint16_t led, const uint8_t hsv[3]) {
    ARG_UNUSED(layer);
    ARG_UNUSED(led);
    ARG_UNUSED(hsv);
}
__weak void flask_rgb_split_send_enabled(bool on) { ARG_UNUSED(on); }
__weak void flask_rgb_split_send_fill(uint8_t layer, const uint8_t hsv[3]) {
    ARG_UNUSED(layer);
    ARG_UNUSED(hsv);
}
__weak void flask_rgb_split_send_effect(void) {}
__weak void flask_rgb_split_send_brightness(uint8_t percent) { ARG_UNUSED(percent); }
__weak void flask_rgb_split_send_overlay(void) {}
__weak void flask_rgb_bulk_resync(void) {}

/* --- HSV (0-255 each, QMK convention) → led_rgb --- */

static struct led_rgb frgb_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
    if (s == 0) {
        return (struct led_rgb){.r = v, .g = v, .b = v};
    }

    uint8_t region = h / 43;
    uint8_t rem = (h - region * 43) * 6;
    uint8_t p = ((uint16_t)v * (255 - s)) >> 8;
    uint8_t q = ((uint16_t)v * (255 - (((uint16_t)s * rem) >> 8))) >> 8;
    uint8_t t = ((uint16_t)v * (255 - (((uint16_t)s * (255 - rem)) >> 8))) >> 8;

    switch (region) {
    case 0:
        return (struct led_rgb){.r = v, .g = t, .b = p};
    case 1:
        return (struct led_rgb){.r = q, .g = v, .b = p};
    case 2:
        return (struct led_rgb){.r = p, .g = v, .b = t};
    case 3:
        return (struct led_rgb){.r = p, .g = q, .b = v};
    case 4:
        return (struct led_rgb){.r = t, .g = p, .b = v};
    default:
        return (struct led_rgb){.r = v, .g = p, .b = q};
    }
}

/* --- render --- */

/* Callers hold frgb_lock. Effect color for one GLOBAL led index (global so
 * the swirl rainbow lines up across the split). */
static struct led_rgb frgb_effect_pixel(uint16_t global_led) {
    uint8_t h = frgb_effect_col[0], s = frgb_effect_col[1], v = frgb_effect_col[2];
    uint8_t p = frgb_phase >> 8;

    switch (frgb_effect) {
    case FLASK_RGB_EFFECT_SOLID:
        break;
    case FLASK_RGB_EFFECT_BREATHE: {
        uint8_t tri = p < 128 ? p * 2 : (255 - p) * 2;

        v = ((uint16_t)v * tri) >> 8;
        break;
    }
    case FLASK_RGB_EFFECT_SPECTRUM:
        h = p;
        break;
    case FLASK_RGB_EFFECT_SWIRL:
        h = p + (uint8_t)(((uint32_t)global_led * 255) / FRGB_TOTAL);
        break;
    default:
        return (struct led_rgb){0};
    }
    return v ? frgb_hsv_to_rgb(h, s, v) : (struct led_rgb){0};
}

/* Global brightness scale (v14) — applied to the FINAL pixel, so painted
 * map, effect, and overlay all dim together. Caller holds frgb_lock. */
static inline struct led_rgb frgb_scale_pixel(struct led_rgb c) {
    if (frgb_brightness >= 100) {
        return c;
    }
    c.r = ((uint16_t)c.r * frgb_brightness) / 100;
    c.g = ((uint16_t)c.g * frgb_brightness) / 100;
    c.b = ((uint16_t)c.b * frgb_brightness) / 100;
    return c;
}

static void frgb_render(struct k_work *work) {
    ARG_UNUSED(work);

    bool dark;
    uint8_t layer;

    K_SPINLOCK(&frgb_lock) {
        dark = !frgb_enabled || !frgb_awake;
        layer = MIN(frgb_layer, FRGB_LAYERS - 1);
        for (int i = 0; i < FRGB_LOCAL; i++) {
            uint16_t g = FRGB_OFFSET + i;
            const uint8_t *hsv = frgb_map[layer][g];

            if (dark) {
                frgb_pixels[i] = (struct led_rgb){0};
                continue;
            } else if (frgb_overlay_on && (frgb_overlay_mask[g / 8] >> (g % 8)) & 1) {
                /* reactive overlay sits above map AND effect */
                frgb_pixels[i] =
                    frgb_hsv_to_rgb(frgb_overlay_col[0], frgb_overlay_col[1], frgb_overlay_col[2]);
            } else if (hsv[2] != 0) {
                /* painted map key overlays the effect */
                frgb_pixels[i] = frgb_hsv_to_rgb(hsv[0], hsv[1], hsv[2]);
            } else {
                frgb_pixels[i] = frgb_effect_pixel(g);
            }
            frgb_pixels[i] = frgb_scale_pixel(frgb_pixels[i]);
        }
    }

    int err = led_strip_update_rgb(frgb_strip, frgb_pixels, FRGB_LOCAL);

    if (err) {
        LOG_WRN("flask_rgb strip update failed: %d", err);
    }
}

static void frgb_schedule_render(void) { k_work_submit(&frgb_render_work); }

/* --- strip power kick (see the ext-power comment up top) --- */

#if defined(FRGB_HAS_EXT_POWER)
static bool frgb_power_rekicked; /* second pass 12 s later (slow peripherals) */

static void frgb_power_kick(struct k_work *work) {
    const struct device *ext = DEVICE_DT_GET_ANY(zmk_ext_power_generic);

    if (device_is_ready(ext) && frgb_enabled && ext_power_get(ext) < 1) {
        LOG_INF("flask_rgb: enabling ext power for the LED strip");
        ext_power_enable(ext); /* driver persists the new state itself */
    }
    if (!frgb_power_rekicked) {
        frgb_power_rekicked = true;
        k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(12));
    }
}
static K_WORK_DELAYABLE_DEFINE(frgb_power_work, frgb_power_kick);

/* Delayed past settings_load(): the ext-power driver restores a saved OFF
 * state AFTER our SYS_INIT runs, so an immediate enable would be undone. */
static void frgb_power_kick_schedule(void) { k_work_schedule(&frgb_power_work, K_SECONDS(3)); }
#else
static void frgb_power_kick_schedule(void) {}
#endif

/* Animation clock: advance the phase and re-render while an animated effect
 * is visible; stops itself otherwise (any effect/enable/wake change calls
 * frgb_anim_kick to restart it). */
static void frgb_anim_tick(struct k_work *work) {
    ARG_UNUSED(work);

    bool running;

    K_SPINLOCK(&frgb_lock) {
        running = frgb_enabled && frgb_awake && frgb_effect_animates(frgb_effect);
        if (running) {
            frgb_phase += frgb_effect_speed;
        }
    }
    if (running) {
        frgb_schedule_render();
        k_work_schedule(&frgb_anim_work, K_MSEC(FRGB_ANIM_TICK_MS));
    }
}

static void frgb_anim_kick(void) {
    bool running;

    K_SPINLOCK(&frgb_lock) {
        running = frgb_enabled && frgb_awake && frgb_effect_animates(frgb_effect);
    }
    if (running) {
        k_work_schedule(&frgb_anim_work, K_MSEC(FRGB_ANIM_TICK_MS));
    }
}

/* --- public map API --- */

uint8_t flask_rgb_layers(void) { return FRGB_LAYERS; }
uint16_t flask_rgb_total_leds(void) { return FRGB_TOTAL; }

int flask_rgb_get_led(uint8_t layer, uint16_t led, uint8_t hsv[3]) {
    if (layer >= FRGB_LAYERS || led >= FRGB_TOTAL || hsv == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&frgb_lock) { memcpy(hsv, frgb_map[layer][led], 3); }
    return 0;
}

int flask_rgb_set_led(uint8_t layer, uint16_t led, const uint8_t hsv[3]) {
    if (layer >= FRGB_LAYERS || led >= FRGB_TOTAL || hsv == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&frgb_lock) { memcpy(frgb_map[layer][led], hsv, 3); }
    flask_rgb_split_send_led(layer, led, hsv);
    frgb_schedule_render();
    return 0;
}

int flask_rgb_fill(uint8_t layer, const uint8_t hsv[3]) {
    if (layer >= FRGB_LAYERS || hsv == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&frgb_lock) {
        for (int led = 0; led < FRGB_TOTAL; led++) {
            memcpy(frgb_map[layer][led], hsv, 3);
        }
    }
    /* One frame on the wire too — the peripheral replays the fill. */
    flask_rgb_split_send_fill(layer, hsv);
    frgb_schedule_render();
    return 0;
}

bool flask_rgb_enabled(void) { return frgb_enabled; }

void flask_rgb_set_enabled(bool on) {
    K_SPINLOCK(&frgb_lock) { frgb_enabled = on; }
    flask_rgb_split_send_enabled(on);
    frgb_schedule_render();
    frgb_anim_kick();
    if (on) {
        frgb_power_kick_schedule();
    }
}

/* --- global brightness (channel 0x21 value 0x0B, v14) --- */

uint8_t flask_rgb_brightness(void) { return frgb_brightness; }

void flask_rgb_set_brightness(uint8_t percent) {
    uint8_t pct = MIN(percent, 100);

    K_SPINLOCK(&frgb_lock) { frgb_brightness = pct; }
    flask_rgb_split_send_brightness(pct);
    frgb_schedule_render();
}

void flask_rgb_sync_brightness(uint8_t percent) {
    K_SPINLOCK(&frgb_lock) { frgb_brightness = MIN(percent, 100); }
    frgb_schedule_render();
}

/* --- effect API (channel 0x21 values 0x04-0x08) --- */

uint8_t flask_rgb_effect(void) { return frgb_effect; }

void flask_rgb_set_effect(uint8_t effect) {
    K_SPINLOCK(&frgb_lock) {
        frgb_effect = MIN(effect, FLASK_RGB_EFFECT_MAX);
        frgb_phase = 0;
    }
    flask_rgb_split_send_effect();
    frgb_schedule_render();
    frgb_anim_kick();
}

uint8_t flask_rgb_effect_speed(void) { return frgb_effect_speed; }

void flask_rgb_set_effect_speed(uint8_t speed) {
    K_SPINLOCK(&frgb_lock) { frgb_effect_speed = MAX(speed, 1); }
    flask_rgb_split_send_effect();
}

void flask_rgb_effect_hsv(uint8_t hsv[3]) {
    K_SPINLOCK(&frgb_lock) { memcpy(hsv, frgb_effect_col, 3); }
}

void flask_rgb_set_effect_hsv(const uint8_t hsv[3]) {
    K_SPINLOCK(&frgb_lock) { memcpy(frgb_effect_col, hsv, 3); }
    flask_rgb_split_send_effect();
    frgb_schedule_render();
}

void flask_rgb_effect_snapshot(uint8_t *effect, uint8_t *speed, uint8_t hsv[3],
                               uint16_t *phase) {
    K_SPINLOCK(&frgb_lock) {
        *effect = frgb_effect;
        *speed = frgb_effect_speed;
        memcpy(hsv, frgb_effect_col, 3);
        *phase = frgb_phase;
    }
}

/* --- runtime LED order (v12, wire value 0x0A) --- */

int flask_rgb_led_order_get(uint16_t start, uint8_t *out, uint8_t count) {
    if (out == NULL || start >= FRGB_TOTAL) {
        return -EINVAL;
    }
    count = MIN(count, FRGB_TOTAL - start);
    K_SPINLOCK(&frgb_lock) { memcpy(out, &frgb_led_order[start], count); }
    return count;
}

int flask_rgb_led_order_set(uint16_t start, const uint8_t *pos, uint8_t count) {
    if (pos == NULL || start >= FRGB_TOTAL) {
        return -EINVAL;
    }
    count = MIN(count, FRGB_TOTAL - start);
    K_SPINLOCK(&frgb_lock) {
        memcpy(&frgb_led_order[start], pos, count);
        frgb_rebuild_pos2led();
    }
    return count;
}

/* --- reactive overlay (position-addressed; transient, never persisted) --- */

void flask_rgb_overlay_positions(const uint8_t *positions, uint8_t count, const uint8_t hsv[3]) {
    if (positions == NULL || count == 0 || hsv == NULL) {
        flask_rgb_overlay_clear();
        return;
    }
    K_SPINLOCK(&frgb_lock) {
        memset(frgb_overlay_mask, 0, sizeof(frgb_overlay_mask));
        for (uint8_t i = 0; i < count; i++) {
            uint8_t led = frgb_pos2led[positions[i]];

            if (led != 0xFF) {
                frgb_overlay_mask[led / 8] |= BIT(led % 8);
            }
        }
        memcpy(frgb_overlay_col, hsv, 3);
        frgb_overlay_on = true;
    }
    flask_rgb_split_send_overlay();
    frgb_schedule_render();
}

void flask_rgb_overlay_clear(void) {
    bool was_on;

    K_SPINLOCK(&frgb_lock) {
        was_on = frgb_overlay_on;
        frgb_overlay_on = false;
    }
    if (was_on) {
        flask_rgb_split_send_overlay();
        frgb_schedule_render();
    }
}

void flask_rgb_overlay_snapshot(uint8_t *on, uint8_t hsv[3], uint8_t *mask, size_t mask_len) {
    K_SPINLOCK(&frgb_lock) {
        *on = frgb_overlay_on ? 1 : 0;
        memcpy(hsv, frgb_overlay_col, 3);
        memcpy(mask, frgb_overlay_mask, MIN(mask_len, sizeof(frgb_overlay_mask)));
    }
}

void flask_rgb_sync_overlay(uint8_t on, const uint8_t hsv[3], const uint8_t *mask,
                            size_t mask_len) {
    K_SPINLOCK(&frgb_lock) {
        frgb_overlay_on = on != 0;
        memcpy(frgb_overlay_col, hsv, 3);
        memset(frgb_overlay_mask, 0, sizeof(frgb_overlay_mask));
        memcpy(frgb_overlay_mask, mask, MIN(mask_len, sizeof(frgb_overlay_mask)));
    }
    frgb_schedule_render();
}

/* --- peripheral ingest (GATT write handler → here) --- */

void flask_rgb_sync_layers(uint32_t layer_bitmap) {
    uint8_t highest = 0;

    for (int i = 31; i >= 0; i--) {
        if (layer_bitmap & BIT(i)) {
            highest = i;
            break;
        }
    }
    K_SPINLOCK(&frgb_lock) { frgb_layer = highest; }
    frgb_schedule_render();
}

void flask_rgb_sync_led(uint8_t layer, uint16_t led, const uint8_t hsv[3]) {
    if (layer >= FRGB_LAYERS || led >= FRGB_TOTAL) {
        return;
    }
    K_SPINLOCK(&frgb_lock) { memcpy(frgb_map[layer][led], hsv, 3); }
    frgb_schedule_render();
}

void flask_rgb_sync_enabled(bool on) {
    K_SPINLOCK(&frgb_lock) { frgb_enabled = on; }
    frgb_schedule_render();
    if (on) {
        frgb_power_kick_schedule();
    }
}

void flask_rgb_sync_fill(uint8_t layer, const uint8_t hsv[3]) {
    if (layer >= FRGB_LAYERS) {
        return;
    }
    K_SPINLOCK(&frgb_lock) {
        for (int led = 0; led < FRGB_TOTAL; led++) {
            memcpy(frgb_map[layer][led], hsv, 3);
        }
    }
    frgb_schedule_render();
}

void flask_rgb_sync_effect(uint8_t effect, uint8_t speed, const uint8_t hsv[3],
                           uint16_t phase) {
    K_SPINLOCK(&frgb_lock) {
        frgb_effect = MIN(effect, FLASK_RGB_EFFECT_MAX);
        frgb_effect_speed = MAX(speed, 1);
        memcpy(frgb_effect_col, hsv, 3);
        frgb_phase = phase; /* re-anchor: halves animate in step */
    }
    frgb_schedule_render();
    frgb_anim_kick();
}

/* --- persistence (central; settings key "flask/rgbmap") --- */

struct flask_rgb_saved_hdr {
    uint8_t version;
    uint8_t layers;
    uint16_t total;
    uint8_t enabled;
    /* v2 (proto v9): effect engine */
    uint8_t effect;
    uint8_t effect_speed;
    uint8_t effect_hsv[3];
    /* v3 (proto v14): global brightness percent */
    uint8_t brightness;
} __packed;

#define FLASK_RGB_SETTINGS_VERSION 3

struct flask_rgb_saved {
    struct flask_rgb_saved_hdr hdr;
    uint8_t map[FRGB_LAYERS][FRGB_TOTAL][3];
} __packed;

/* v2 shape (no brightness byte) — still restorable so AJ's painted layers
 * survive the v14 flash; brightness seeds 100. */
struct flask_rgb_saved_hdr_v2 {
    uint8_t version;
    uint8_t layers;
    uint16_t total;
    uint8_t enabled;
    uint8_t effect;
    uint8_t effect_speed;
    uint8_t effect_hsv[3];
} __packed;

struct flask_rgb_saved_v2 {
    struct flask_rgb_saved_hdr_v2 hdr;
    uint8_t map[FRGB_LAYERS][FRGB_TOTAL][3];
} __packed;

int flask_rgb_save(void) {
    static struct flask_rgb_saved saved;    /* 2.1 KB — keep off the stack */

    saved.hdr = (struct flask_rgb_saved_hdr){
        .version = FLASK_RGB_SETTINGS_VERSION,
        .layers = FRGB_LAYERS,
        .total = FRGB_TOTAL,
        .enabled = frgb_enabled ? 1 : 0,
        .effect = frgb_effect,
        .effect_speed = frgb_effect_speed,
        .brightness = frgb_brightness,
    };
    K_SPINLOCK(&frgb_lock) {
        memcpy(saved.hdr.effect_hsv, frgb_effect_col, 3);
        memcpy(saved.map, frgb_map, sizeof(saved.map));
    }

    int err = settings_save_one("flask/rgbmap", &saved, sizeof(saved));

    if (err) {
        LOG_ERR("flask/rgbmap settings save failed: %d", err);
        return err;
    }

    /* Runtime LED order rides its own entry (v12) — small, and the big
     * map blob keeps its shape (no migration). */
    uint8_t order[FRGB_TOTAL];

    K_SPINLOCK(&frgb_lock) { memcpy(order, frgb_led_order, sizeof(order)); }
    err = settings_save_one("flask/ledorder", order, sizeof(order));
    if (err) {
        LOG_ERR("flask/ledorder settings save failed: %d", err);
    }
    return err;
}

/* Restore the runtime LED order ("flask/ledorder", v12). Size mismatch =
 * a different board shape — keep the DT/identity seed. */
int flask_rgb_ledorder_restore(size_t len, settings_read_cb read_cb, void *cb_arg) {
    uint8_t order[FRGB_TOTAL];

    if (len != sizeof(order)) {
        LOG_WRN("flask/ledorder settings size mismatch (%d != %d)", (int)len, (int)sizeof(order));
        return 0;
    }
    if (read_cb(cb_arg, order, sizeof(order)) < 0) {
        return -EIO;
    }
    K_SPINLOCK(&frgb_lock) {
        memcpy(frgb_led_order, order, sizeof(frgb_led_order));
        frgb_rebuild_pos2led();
    }
    return 0;
}

/* Restore entry — called from flask_proto.c's "flask" settings handler.
 * v1 blobs (no effect fields) are a size mismatch and reseed dark — they
 * never reached benched hardware. v2 blobs (no brightness byte) restore
 * with brightness 100 so painted layers survive the v14 flash. */
int flask_rgb_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg) {
    static union {
        struct flask_rgb_saved v3;
        struct flask_rgb_saved_v2 v2;
    } saved; /* one 2.1 KB buffer for both shapes — keep off the stack */

    if (len == sizeof(saved.v3)) {
        if (read_cb(cb_arg, &saved.v3, sizeof(saved.v3)) < 0) {
            return -EIO;
        }
        if (saved.v3.hdr.version != FLASK_RGB_SETTINGS_VERSION ||
            saved.v3.hdr.layers != FRGB_LAYERS || saved.v3.hdr.total != FRGB_TOTAL) {
            LOG_WRN("flask/rgbmap settings shape mismatch ignored");
            return 0;
        }
        K_SPINLOCK(&frgb_lock) {
            memcpy(frgb_map, saved.v3.map, sizeof(frgb_map));
            frgb_enabled = saved.v3.hdr.enabled != 0;
            frgb_effect = MIN(saved.v3.hdr.effect, FLASK_RGB_EFFECT_MAX);
            frgb_effect_speed = MAX(saved.v3.hdr.effect_speed, 1);
            memcpy(frgb_effect_col, saved.v3.hdr.effect_hsv, 3);
            frgb_brightness = MIN(saved.v3.hdr.brightness, 100);
        }
    } else if (len == sizeof(saved.v2)) {
        if (read_cb(cb_arg, &saved.v2, sizeof(saved.v2)) < 0) {
            return -EIO;
        }
        if (saved.v2.hdr.version != 2 || saved.v2.hdr.layers != FRGB_LAYERS ||
            saved.v2.hdr.total != FRGB_TOTAL) {
            LOG_WRN("flask/rgbmap settings shape mismatch ignored");
            return 0;
        }
        K_SPINLOCK(&frgb_lock) {
            memcpy(frgb_map, saved.v2.map, sizeof(frgb_map));
            frgb_enabled = saved.v2.hdr.enabled != 0;
            frgb_effect = MIN(saved.v2.hdr.effect, FLASK_RGB_EFFECT_MAX);
            frgb_effect_speed = MAX(saved.v2.hdr.effect_speed, 1);
            memcpy(frgb_effect_col, saved.v2.hdr.effect_hsv, 3);
            frgb_brightness = 100;
        }
    } else {
        LOG_WRN("flask/rgbmap settings size mismatch (%d)", (int)len);
        return -EINVAL;
    }
    frgb_schedule_render();
    frgb_anim_kick();
    return 0;
}

/* --- layer + activity tracking --- */

#if defined(FLASK_RGB_HAS_LAYER_STATE)
static int frgb_layer_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    uint8_t highest = zmk_keymap_highest_layer_active();

    K_SPINLOCK(&frgb_lock) { frgb_layer = highest; }
    flask_rgb_split_send_layers(BIT(highest));
    frgb_schedule_render();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_rgb_layer, frgb_layer_listener);
ZMK_SUBSCRIPTION(flask_rgb_layer, zmk_layer_state_changed);
#endif

static int frgb_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    K_SPINLOCK(&frgb_lock) { frgb_awake = (ev->state == ZMK_ACTIVITY_ACTIVE); }
    frgb_schedule_render();
    frgb_anim_kick();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_rgb_activity, frgb_activity_listener);
ZMK_SUBSCRIPTION(flask_rgb_activity, zmk_activity_state_changed);

/* --- init --- */

static int flask_rgb_init(void) {
    /* Seed the LED→position order from the node's key-positions property
     * (identity fallback), then build the reverse map. A stored runtime
     * order overrides both when settings load. */
#if DT_NODE_HAS_PROP(FRGB_NODE, key_positions)
    memcpy(frgb_led_order, frgb_led_pos, FRGB_TOTAL);
#else
    for (uint16_t led = 0; led < FRGB_TOTAL; led++) {
        frgb_led_order[led] = led < 255 ? (uint8_t)led : 0xFF;
    }
#endif
    frgb_rebuild_pos2led();
    k_work_init(&frgb_render_work, frgb_render);
    k_work_init_delayable(&frgb_anim_work, frgb_anim_tick);
    if (!device_is_ready(frgb_strip)) {
        LOG_ERR("flask_rgb: LED strip not ready");
        return -ENODEV;
    }
    frgb_schedule_render();     /* dark until the map says otherwise */
    frgb_power_kick_schedule(); /* un-gate strip VCC once settings settle */
    return 0;
}

SYS_INIT(flask_rgb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
