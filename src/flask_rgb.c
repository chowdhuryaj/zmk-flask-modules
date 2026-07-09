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
static uint8_t frgb_layer;      /* rendered layer (central: mirrors keymap) */
static bool frgb_awake = true;  /* activity gate */

/* Whole-strip effect (v9): painted map keys overlay it. The phase clock
 * advances by `speed` per animation tick; both halves run the same clock
 * and the central re-anchors the peripheral's phase on every effect sync. */
static uint8_t frgb_effect;         /* enum flask_rgb_effect */
static uint8_t frgb_effect_speed = 128;
static uint8_t frgb_effect_col[3] = {0, 255, 120};
static uint16_t frgb_phase;

static struct led_rgb frgb_pixels[FRGB_LOCAL];
static struct k_work frgb_render_work;
static struct k_work_delayable frgb_anim_work;

#define FRGB_ANIM_TICK_MS 40 /* 25 fps */

static bool frgb_effect_animates(uint8_t effect) {
    return effect == FLASK_RGB_EFFECT_BREATHE || effect == FLASK_RGB_EFFECT_SPECTRUM ||
           effect == FLASK_RGB_EFFECT_SWIRL;
}

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

static void frgb_render(struct k_work *work) {
    ARG_UNUSED(work);

    bool dark;
    uint8_t layer;

    K_SPINLOCK(&frgb_lock) {
        dark = !frgb_enabled || !frgb_awake;
        layer = MIN(frgb_layer, FRGB_LAYERS - 1);
        for (int i = 0; i < FRGB_LOCAL; i++) {
            const uint8_t *hsv = frgb_map[layer][FRGB_OFFSET + i];

            if (dark) {
                frgb_pixels[i] = (struct led_rgb){0};
            } else if (hsv[2] != 0) {
                /* painted map key overlays the effect */
                frgb_pixels[i] = frgb_hsv_to_rgb(hsv[0], hsv[1], hsv[2]);
            } else {
                frgb_pixels[i] = frgb_effect_pixel(FRGB_OFFSET + i);
            }
        }
    }

    int err = led_strip_update_rgb(frgb_strip, frgb_pixels, FRGB_LOCAL);

    if (err) {
        LOG_WRN("flask_rgb strip update failed: %d", err);
    }
}

static void frgb_schedule_render(void) { k_work_submit(&frgb_render_work); }

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
} __packed;

#define FLASK_RGB_SETTINGS_VERSION 2

struct flask_rgb_saved {
    struct flask_rgb_saved_hdr hdr;
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
    };
    K_SPINLOCK(&frgb_lock) {
        memcpy(saved.hdr.effect_hsv, frgb_effect_col, 3);
        memcpy(saved.map, frgb_map, sizeof(saved.map));
    }

    int err = settings_save_one("flask/rgbmap", &saved, sizeof(saved));

    if (err) {
        LOG_ERR("flask/rgbmap settings save failed: %d", err);
    }
    return err;
}

/* Restore entry — called from flask_proto.c's "flask" settings handler.
 * v1 blobs (no effect fields) are a size mismatch and reseed dark — they
 * never reached benched hardware. */
int flask_rgb_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg) {
    static struct flask_rgb_saved saved;

    if (len != sizeof(saved)) {
        LOG_WRN("flask/rgbmap settings size mismatch (%d != %d)", (int)len, (int)sizeof(saved));
        return -EINVAL;
    }
    if (read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
        return -EIO;
    }
    if (saved.hdr.version != FLASK_RGB_SETTINGS_VERSION || saved.hdr.layers != FRGB_LAYERS ||
        saved.hdr.total != FRGB_TOTAL) {
        LOG_WRN("flask/rgbmap settings shape mismatch ignored");
        return 0;
    }
    K_SPINLOCK(&frgb_lock) {
        memcpy(frgb_map, saved.map, sizeof(frgb_map));
        frgb_enabled = saved.hdr.enabled != 0;
        frgb_effect = MIN(saved.hdr.effect, FLASK_RGB_EFFECT_MAX);
        frgb_effect_speed = MAX(saved.hdr.effect_speed, 1);
        memcpy(frgb_effect_col, saved.hdr.effect_hsv, 3);
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
    k_work_init(&frgb_render_work, frgb_render);
    k_work_init_delayable(&frgb_anim_work, frgb_anim_tick);
    if (!device_is_ready(frgb_strip)) {
        LOG_ERR("flask_rgb: LED strip not ready");
        return -ENODEV;
    }
    frgb_schedule_render();     /* dark until the map says otherwise */
    return 0;
}

SYS_INIT(flask_rgb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
