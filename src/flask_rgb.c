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

static struct led_rgb frgb_pixels[FRGB_LOCAL];
static struct k_work frgb_render_work;

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

static void frgb_render(struct k_work *work) {
    ARG_UNUSED(work);

    bool dark;
    uint8_t layer;

    K_SPINLOCK(&frgb_lock) {
        dark = !frgb_enabled || !frgb_awake;
        layer = MIN(frgb_layer, FRGB_LAYERS - 1);
        for (int i = 0; i < FRGB_LOCAL; i++) {
            const uint8_t *hsv = frgb_map[layer][FRGB_OFFSET + i];

            frgb_pixels[i] = (dark || hsv[2] == 0)
                                 ? (struct led_rgb){0}
                                 : frgb_hsv_to_rgb(hsv[0], hsv[1], hsv[2]);
        }
    }

    int err = led_strip_update_rgb(frgb_strip, frgb_pixels, FRGB_LOCAL);

    if (err) {
        LOG_WRN("flask_rgb strip update failed: %d", err);
    }
}

static void frgb_schedule_render(void) { k_work_submit(&frgb_render_work); }

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

/* --- persistence (central; settings key "flask/rgbmap") --- */

struct flask_rgb_saved_hdr {
    uint8_t version;
    uint8_t layers;
    uint16_t total;
    uint8_t enabled;
} __packed;

#define FLASK_RGB_SETTINGS_VERSION 1

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
    };
    K_SPINLOCK(&frgb_lock) { memcpy(saved.map, frgb_map, sizeof(saved.map)); }

    int err = settings_save_one("flask/rgbmap", &saved, sizeof(saved));

    if (err) {
        LOG_ERR("flask/rgbmap settings save failed: %d", err);
    }
    return err;
}

/* Restore entry — called from flask_proto.c's "flask" settings handler. */
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
    }
    frgb_schedule_render();
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
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_rgb_activity, frgb_activity_listener);
ZMK_SUBSCRIPTION(flask_rgb_activity, zmk_activity_state_changed);

/* --- init --- */

static int flask_rgb_init(void) {
    k_work_init(&frgb_render_work, frgb_render);
    if (!device_is_ready(frgb_strip)) {
        LOG_ERR("flask_rgb: LED strip not ready");
        return -ENODEV;
    }
    frgb_schedule_render();     /* dark until the map says otherwise */
    return 0;
}

SYS_INIT(flask_rgb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
