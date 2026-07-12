/*
 * flask_automouse — runtime-tunable auto-mouse layer (Flask channel 0x1B).
 *
 * Replaces the stock &zip_temp_layer on the cursor chain: ball motion past a
 * movement threshold activates a layer; the layer drops after a timeout, or
 * — timeout 0 — LATCHES until a key that is transparent on that layer is
 * pressed (that key is swallowed and the layer drops, so the press never
 * types). Keys with real bindings on the layer (clicks, snipe, gestures)
 * optionally extend the timeout instead.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/settings/settings.h>

struct flask_automouse_params {
    bool enabled;
    uint16_t timeout_ms;   /* 0 = latch until a transparent key */
    uint16_t threshold;    /* accumulated |dx|+|dy| counts before trigger; 0 = any motion */
    uint8_t layer;         /* layer INDEX (order position, matches the app dropdown) */
    bool extend_on_key;    /* non-transparent key on the layer re-arms the timeout */
};

int flask_automouse_params_get(struct flask_automouse_params *out);
int flask_automouse_params_set(const struct flask_automouse_params *in);

struct flask_automouse_saved {
    uint8_t version;
    struct flask_automouse_params params;
} __packed;

#define AUTOMOUSE_SETTINGS_VERSION 1

int flask_automouse_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg);
