/*
 * flask_rgb split sync — CENTRAL side (GATT client).
 *
 * Overrides the weak egress fns in flask_rgb.c. On the existing split
 * connection (the central half of a ZMK split is the BT central), discover
 * the peripheral's flask_rgb characteristic once the link is encrypted,
 * then push small write-without-response frames: live layer state, map
 * edits, and a chunked full-map resync on (re)connect.
 *
 * All sends funnel through one work item draining {latest-state flags +
 * a small LED-edit queue + the bulk cursor}; -ENOMEM from the BT buffer
 * pool just reschedules the drain.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <flask_rgb/flask_rgb.h>
#include "flask_rgb_split.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct bt_uuid_128 frgb_svc_uuid = BT_UUID_INIT_128(FLASK_RGB_SVC_UUID);
static const struct bt_uuid_128 frgb_chr_uuid = BT_UUID_INIT_128(FLASK_RGB_CHR_UUID);

static struct {
    struct bt_conn *conn;
    uint16_t handle;
    bool ready;
} frgb_link;

static struct bt_gatt_discover_params frgb_disc_params;
static struct bt_uuid_128 frgb_disc_uuid;

/* --- TX state (latest-wins flags + LED edit queue + bulk cursor) --- */

struct frgb_led_frame {
    uint8_t layer;
    uint16_t led;
    uint8_t hsv[3];
};

K_MSGQ_DEFINE(frgb_led_q, sizeof(struct frgb_led_frame), 64, 4);

static struct k_spinlock frgb_tx_lock;
static bool frgb_send_layers_pending;
static uint32_t frgb_layers_val;
static bool frgb_send_enabled_pending;
static bool frgb_enabled_val;
static bool frgb_send_fill_pending;
static uint8_t frgb_fill_layer;
static uint8_t frgb_fill_hsv[3];
static bool frgb_send_effect_pending;
static bool frgb_bulk_active;
static uint8_t frgb_bulk_layer;
static uint16_t frgb_bulk_led;

static struct k_work_delayable frgb_tx_work;
static struct k_work_delayable frgb_discover_work;

static int frgb_wwr(const uint8_t *buf, uint16_t len) {
    if (!frgb_link.ready || frgb_link.conn == NULL) {
        return -ENOTCONN;
    }
    return bt_gatt_write_without_response(frgb_link.conn, frgb_link.handle, buf, len, false);
}

/* One drain pass; reschedules itself while there is more to send or the
 * buffer pool pushes back. */
static void frgb_tx_drain(struct k_work *work) {
    ARG_UNUSED(work);

    if (!frgb_link.ready) {
        return;
    }

    uint8_t buf[5 + 3 * FRGB_CHUNK_MAX];
    int err = 0;

    for (int budget = 0; budget < 16 && err == 0; budget++) {
        bool sent = false;

        /* latest-state flags first: enabled, then layer state */
        bool do_enabled = false, do_layers = false, do_fill = false, do_effect = false;
        bool enabled_val = false;
        uint32_t layers_val = 0;
        uint8_t fill_layer = 0, fill_hsv[3];

        K_SPINLOCK(&frgb_tx_lock) {
            if (frgb_send_enabled_pending) {
                do_enabled = true;
                enabled_val = frgb_enabled_val;
                frgb_send_enabled_pending = false;
            } else if (frgb_send_layers_pending) {
                do_layers = true;
                layers_val = frgb_layers_val;
                frgb_send_layers_pending = false;
            } else if (frgb_send_fill_pending) {
                do_fill = true;
                fill_layer = frgb_fill_layer;
                memcpy(fill_hsv, frgb_fill_hsv, 3);
                frgb_send_fill_pending = false;
            } else if (frgb_send_effect_pending) {
                do_effect = true;
                frgb_send_effect_pending = false;
            }
        }

        if (do_enabled) {
            buf[0] = FRGB_OP_ENABLED;
            buf[1] = enabled_val ? 1 : 0;
            err = frgb_wwr(buf, 2);
            if (err) {
                K_SPINLOCK(&frgb_tx_lock) {
                    frgb_send_enabled_pending = true;
                    frgb_enabled_val = enabled_val;
                }
            }
            sent = true;
        } else if (do_layers) {
            buf[0] = FRGB_OP_LAYERS;
            sys_put_le32(layers_val, &buf[1]);
            err = frgb_wwr(buf, 5);
            if (err) {
                K_SPINLOCK(&frgb_tx_lock) {
                    frgb_send_layers_pending = true;
                    frgb_layers_val = layers_val;
                }
            }
            sent = true;
        } else if (do_fill) {
            buf[0] = FRGB_OP_FILL;
            buf[1] = fill_layer;
            memcpy(&buf[2], fill_hsv, 3);
            err = frgb_wwr(buf, 5);
            if (err) {
                K_SPINLOCK(&frgb_tx_lock) {
                    frgb_send_fill_pending = true;
                    frgb_fill_layer = fill_layer;
                    memcpy(frgb_fill_hsv, fill_hsv, 3);
                }
            }
            sent = true;
        } else if (do_effect) {
            /* Snapshot at send time — phase is a live clock, and a stale
             * anchor defeats the point of sending one. */
            uint16_t phase;

            buf[0] = FRGB_OP_EFFECT;
            flask_rgb_effect_snapshot(&buf[1], &buf[2], &buf[3], &phase);
            sys_put_le16(phase, &buf[6]);
            err = frgb_wwr(buf, 8);
            if (err) {
                K_SPINLOCK(&frgb_tx_lock) { frgb_send_effect_pending = true; }
            }
            sent = true;
        } else {
            /* LED edit queue */
            struct frgb_led_frame f;

            if (k_msgq_peek(&frgb_led_q, &f) == 0) {
                buf[0] = FRGB_OP_LED;
                buf[1] = f.layer;
                sys_put_le16(f.led, &buf[2]);
                memcpy(&buf[4], f.hsv, 3);
                err = frgb_wwr(buf, 7);
                if (err == 0) {
                    k_msgq_get(&frgb_led_q, &f, K_NO_WAIT);
                }
                sent = true;
            } else if (frgb_bulk_active) {
                /* chunked full-map resync, FRGB_CHUNK_MAX LEDs per frame */
                uint16_t total = flask_rgb_total_leds();
                uint8_t n = MIN(FRGB_CHUNK_MAX, total - frgb_bulk_led);

                buf[0] = FRGB_OP_CHUNK;
                buf[1] = frgb_bulk_layer;
                sys_put_le16(frgb_bulk_led, &buf[2]);
                buf[4] = n;
                for (uint8_t i = 0; i < n; i++) {
                    flask_rgb_get_led(frgb_bulk_layer, frgb_bulk_led + i, &buf[5 + 3 * i]);
                }
                err = frgb_wwr(buf, 5 + 3 * n);
                if (err == 0) {
                    frgb_bulk_led += n;
                    if (frgb_bulk_led >= total) {
                        frgb_bulk_led = 0;
                        frgb_bulk_layer++;
                        if (frgb_bulk_layer >= flask_rgb_layers()) {
                            frgb_bulk_active = false;
                            LOG_INF("flask_rgb: peripheral map resync complete");
                        }
                    }
                }
                sent = true;
            }
        }

        if (!sent) {
            return;     /* fully drained */
        }
    }

    /* More pending (budget spent) or buffer pool pushback — come back. */
    k_work_schedule(&frgb_tx_work, err ? K_MSEC(20) : K_NO_WAIT);
}

/* --- egress fns (override the weak no-ops in flask_rgb.c) --- */

void flask_rgb_split_send_layers(uint32_t layer_bitmap) {
    K_SPINLOCK(&frgb_tx_lock) {
        frgb_send_layers_pending = true;
        frgb_layers_val = layer_bitmap;
    }
    k_work_schedule(&frgb_tx_work, K_NO_WAIT);
}

void flask_rgb_split_send_enabled(bool on) {
    K_SPINLOCK(&frgb_tx_lock) {
        frgb_send_enabled_pending = true;
        frgb_enabled_val = on;
    }
    k_work_schedule(&frgb_tx_work, K_NO_WAIT);
}

void flask_rgb_split_send_fill(uint8_t layer, const uint8_t hsv[3]) {
    K_SPINLOCK(&frgb_tx_lock) {
        frgb_send_fill_pending = true;
        frgb_fill_layer = layer;
        memcpy(frgb_fill_hsv, hsv, 3);
    }
    k_work_schedule(&frgb_tx_work, K_NO_WAIT);
}

void flask_rgb_split_send_effect(void) {
    K_SPINLOCK(&frgb_tx_lock) { frgb_send_effect_pending = true; }
    k_work_schedule(&frgb_tx_work, K_NO_WAIT);
}

void flask_rgb_split_send_led(uint8_t layer, uint16_t led, const uint8_t hsv[3]) {
    struct frgb_led_frame f = {.layer = layer, .led = led};

    memcpy(f.hsv, hsv, 3);
    if (k_msgq_put(&frgb_led_q, &f, K_NO_WAIT) != 0) {
        /* Queue full (paint storm) — fall back to a full resync. */
        flask_rgb_bulk_resync();
        return;
    }
    k_work_schedule(&frgb_tx_work, K_NO_WAIT);
}

void flask_rgb_bulk_resync(void) {
    K_SPINLOCK(&frgb_tx_lock) {
        frgb_bulk_active = true;
        frgb_bulk_layer = 0;
        frgb_bulk_led = 0;
    }
    k_work_schedule(&frgb_tx_work, K_NO_WAIT);
}

/* --- discovery --- */

static uint8_t frgb_discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  struct bt_gatt_discover_params *params) {
    if (attr == NULL) {
        /* Service absent — peripheral runs firmware without flask_rgb. */
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        const struct bt_gatt_service_val *svc = attr->user_data;

        memcpy(&frgb_disc_uuid, &frgb_chr_uuid, sizeof(frgb_disc_uuid));
        params->uuid = &frgb_disc_uuid.uuid;
        params->start_handle = attr->handle + 1;
        params->end_handle = svc->end_handle;
        params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
        if (bt_gatt_discover(conn, params) < 0) {
            LOG_WRN("flask_rgb: characteristic discovery failed to start");
        }
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_chrc *chrc = attr->user_data;

    frgb_link.conn = bt_conn_ref(conn);
    frgb_link.handle = chrc->value_handle;
    frgb_link.ready = true;
    LOG_INF("flask_rgb: peripheral characteristic found (handle %u)", frgb_link.handle);

    /* Rehydrate the peripheral: enabled + effect + layer state + full map. */
    flask_rgb_split_send_enabled(flask_rgb_enabled());
    flask_rgb_split_send_effect();
    flask_rgb_bulk_resync();
    return BT_GATT_ITER_STOP;
}

/* bt_conn_foreach visitor: grab the first central-role LE conn (the split
 * link; the host link is peripheral-role). */
static void frgb_find_central_conn(struct bt_conn *c, void *data) {
    struct bt_conn **out = data;
    struct bt_conn_info info;

    if (*out == NULL && bt_conn_get_info(c, &info) == 0 &&
        info.role == BT_CONN_ROLE_CENTRAL) {
        *out = c;
    }
}

static void frgb_start_discovery(struct k_work *work) {
    ARG_UNUSED(work);

    struct bt_conn *conn = NULL;

    bt_conn_foreach(BT_CONN_TYPE_LE, frgb_find_central_conn, &conn);

    if (conn == NULL || frgb_link.ready) {
        return;
    }

    memcpy(&frgb_disc_uuid, &frgb_svc_uuid, sizeof(frgb_disc_uuid));
    frgb_disc_params = (struct bt_gatt_discover_params){
        .uuid = &frgb_disc_uuid.uuid,
        .func = frgb_discover_func,
        .start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE,
        .end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE,
        .type = BT_GATT_DISCOVER_PRIMARY,
    };
    int err = bt_gatt_discover(conn, &frgb_disc_params);

    if (err) {
        LOG_WRN("flask_rgb: service discovery failed: %d", err);
    }
}

static void frgb_security_changed(struct bt_conn *conn, bt_security_t level,
                                  enum bt_security_err err) {
    struct bt_conn_info info;

    if (err != BT_SECURITY_ERR_SUCCESS || level < BT_SECURITY_L2) {
        return;
    }
    if (bt_conn_get_info(conn, &info) < 0 || info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }
    /* Let ZMK's own split discovery finish first; ours can wait. */
    k_work_schedule(&frgb_discover_work, K_SECONDS(2));
}

static void frgb_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);

    if (conn != frgb_link.conn) {
        return;
    }
    bt_conn_unref(frgb_link.conn);
    frgb_link.conn = NULL;
    frgb_link.ready = false;
    K_SPINLOCK(&frgb_tx_lock) { frgb_bulk_active = false; }
    k_msgq_purge(&frgb_led_q);
}

BT_CONN_CB_DEFINE(flask_rgb_conn_cb) = {
    .security_changed = frgb_security_changed,
    .disconnected = frgb_disconnected,
};

static int frgb_split_central_init(void) {
    k_work_init_delayable(&frgb_tx_work, frgb_tx_drain);
    k_work_init_delayable(&frgb_discover_work, frgb_start_discovery);
    return 0;
}

SYS_INIT(frgb_split_central_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
