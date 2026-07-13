/*
 * flask_rgb split sync — PERIPHERAL side.
 *
 * Hosts the module's GATT characteristic (write-without-response) and feeds
 * incoming frames into the local map/render. The central half is the GATT
 * client on the existing split connection; writes arrive encrypted (the
 * split link is bonded).
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <flask_rgb/flask_rgb.h>
#include "flask_rgb_split.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct bt_uuid_128 frgb_svc_uuid = BT_UUID_INIT_128(FLASK_RGB_SVC_UUID);
static const struct bt_uuid_128 frgb_chr_uuid = BT_UUID_INIT_128(FLASK_RGB_CHR_UUID);

static ssize_t frgb_chr_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    const uint8_t *p = buf;

    if (len < 2) {
        return len;
    }

    switch (p[0]) {
    case FRGB_OP_LAYERS:
        if (len >= 5) {
            flask_rgb_sync_layers(sys_get_le32(&p[1]));
        }
        break;
    case FRGB_OP_LED:
        if (len >= 7) {
            flask_rgb_sync_led(p[1], sys_get_le16(&p[2]), &p[4]);
        }
        break;
    case FRGB_OP_ENABLED:
        flask_rgb_sync_enabled(p[1] != 0);
        break;
    case FRGB_OP_FILL:
        if (len >= 5) {
            flask_rgb_sync_fill(p[1], &p[2]);
        }
        break;
    case FRGB_OP_CHUNK:
        if (len >= 5) {
            uint16_t start = sys_get_le16(&p[2]);
            uint8_t n = MIN(p[4], FRGB_CHUNK_MAX);

            if (len >= 5 + 3 * (uint16_t)n) {
                for (uint8_t i = 0; i < n; i++) {
                    flask_rgb_sync_led(p[1], start + i, &p[5 + 3 * i]);
                }
            }
        }
        break;
    case FRGB_OP_EFFECT:
        if (len >= 8) {
            flask_rgb_sync_effect(p[1], p[2], &p[3], sys_get_le16(&p[6]));
        }
        break;
    case FRGB_OP_OVERLAY:
        if (len >= 5) {
            flask_rgb_sync_overlay(p[1], &p[2], &p[5], len - 5);
        }
        break;
    case FRGB_OP_BRIGHT:
        flask_rgb_sync_brightness(p[1]);
        break;
    default:
        break;
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(flask_rgb_svc,
    BT_GATT_PRIMARY_SERVICE(&frgb_svc_uuid),
    BT_GATT_CHARACTERISTIC(&frgb_chr_uuid.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, frgb_chr_write, NULL));
