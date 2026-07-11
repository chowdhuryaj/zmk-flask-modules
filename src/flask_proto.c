/*
 * Flask raw-HID tuning protocol for ZMK — the "imprint" family line.
 *
 * Same frame the QMK Flask keyboards speak (mad_hid.c / AdeptProtocol.swift /
 * flaskproto.js): 32-byte reports, [cmd, channel, value_id, u16 BE payload].
 * Commands: 0x07 set, 0x08 get, 0x09 save; unhandled frames echo with the
 * command byte replaced by 0xFF. Setters clamp before applying and echo the
 * applied value — callers adopt the echo.
 *
 * Transport: zzeneg/zmk-raw-hid (USB + BT), which already defaults to the
 * Flask HID identity (usage page 0xFF60, usage 0x61, 32-byte reports).
 *
 * The imprint protocol line is independent of the QMK families' version
 * lines (v3 today — see FLASK_PROTO_VERSION). Value ids are append-only.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/keymap.h>
#include <zmk/events/position_state_changed.h>
#include <raw_hid/events.h>

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL)
#include <flask_scroll/flask_scroll.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOSCROLL)
#include <flask_autoscroll/flask_autoscroll.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL)
#include <flask_accel/flask_accel.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSNAP)
#include <flask_scrollsnap/flask_scrollsnap.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_FLASK_RGB)
#include <flask_rgb/flask_rgb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_FLASK_COMBOS)
#include <flask_combos/flask_combos.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
#include <flask_macros/flask_macros.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_FLASK_LEADER)
#include <flask_leader/flask_leader.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_BALLSWAP)
#include <flask_ballswap/flask_ballswap.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES)
#include <flask_gestures/flask_gestures.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Per-module channels compile only when their module does — a channel whose
 * module is absent answers unhandled (0xFF), which is how the apps discover
 * capability. v2 (2026-07-08): autoscroll channel 0x1A. v3 (2026-07-08):
 * dragscroll became conditional (the Imprint dropped flask_scroll for the
 * stock ZMK scroll chain, so its 0x15 now answers unhandled). v4
 * (2026-07-08): jog mode removed — AS_DEADZONE/AS_RANGE (0x03/0x04) now
 * answer unhandled; autoscroll is stepped-only. v5 (2026-07-08): key-state
 * channel 0x23 — GET 0x01 returns a pressed-position bitmap (bytes, not
 * u16; position N = payload byte N/8 bit N%8), feeding the HUD's live
 * key-press highlight (ZMK has no Vial matrix-state read). v6 (2026-07-09):
 * RGB map channel 0x21 (flask_rgb) — same wire shape as the QMK NLKB16:
 * enabled 0x01 u16, layers 0x02 / leds 0x03 RO u16, LED 0x10 + fill 0x12
 * payload-addressed byte frames; SAVE persists via "flask/rgbmap".
 * v7 (2026-07-09): runtime combos channel 0x24 (flask_combos) — enabled
 * 0x01 u16, slot count 0x02 RO u16, timeout ms 0x03 u16, slot 0x10
 * payload-addressed [slot, pos x4 (0xFF empty), usage u32 BE]; SAVE
 * persists via "flask/combos". v8 (2026-07-09): runtime macros channel
 * 0x25 (flask_macros) — enabled 0x01 u16, slot count 0x02 / step capacity
 * 0x03 RO u16, tap ms 0x04 / wait ms 0x05 u16, live state 0x06 (GET =
 * playing slot+1 or 0; SET nonzero plays slot v-1, 0 stops; never
 * persisted), step 0x10 payload-addressed [slot, step, action, param u32
 * BE]; SAVE persists via "flask/macros". v9 (2026-07-09, parity round):
 * (a) pointer accel channel 0x10 (flask_accel, QMK wire shape — enabled
 * 0x01, takeoff 0x02, growth 0x03, offset 0x04 SIGNED, limit 0x05, all
 * x100; SAVE via "flask/accel"); (b) scroll snap/lock channel 0x26
 * (flask_scrollsnap — enabled 0x01, threshold pct 0x02, samples 0x03,
 * immediate 0x04, lock ms 0x05, lock events 0x06, idle reset ms 0x07;
 * SAVE via "flask/scrollsnap"); (c) rgbmap effect engine values 0x04-0x08
 * (effect / speed / hue / sat / val); (d) combos keys-per-slot RO 0x04 and
 * the slot frame sized by it ([slot, pos x KEYS, usage u32 BE]) — combos/
 * macros capacities are Kconfig now and persist per-slot. v10 (2026-07-09,
 * leader/gestures round): (a) runtime leader channel 0x19 (flask_leader —
 * timeout 0x01 u16 [QMK-shared id], slot count 0x02 / keys-per-seq 0x03
 * RO, enabled 0x04, slot 0x50 payload-addressed [seq, pos x KEYS (0xFF
 * empty), action, param u32 BE] — action 0 none / 1 usage tap / 2 play
 * flask_macros slot; QMK's u16 slot-table ids 0x10-0x4D stay untouched);
 * (b) runtime gestures channel 0x11 (flask_gestures — ratchet step 0x01 +
 * active set 0x02 [QMK-shared ids], enabled 0x03, set count 0x04 RO, slot
 * 0x50 payload-addressed [set, dir 0-7 E..NE-clockwise, action, param u32
 * BE], same typed-output actions; QMK's 0x10-0x4F table ids untouched).
 * v11 (2026-07-11): trackball role-swap channel 0x27 (flask_ballswap —
 * swapped base state 0x01 u16 RW [SET applies live, SAVE or the &bswap 0
 * key persists], effective state 0x02 RO u16 [base XOR momentary holds]). */
#define FLASK_PROTO_VERSION 11
#define FLASK_FAMILY_IMPRINT 4 /* 1=adept 2=svalboard 3=nlkb16 4=imprint */

/* Commands (VIA custom-value ids, reused raw like the QMK side) */
#define CMD_SET 0x07
#define CMD_GET 0x08
#define CMD_SAVE 0x09
#define CMD_UNHANDLED 0xFF

/* Channels (same numbering as the QMK families; 0x20-0x22 are NLKB16's —
 * stay clear so channel ids keep meaning one thing across the ecosystem) */
#define CH_META 0x00
#define CH_ACCEL 0x10 /* QMK accel channel — same wire shape (v9) */
#define CH_GESTURES 0x11 /* QMK gestures channel — shared knob ids, ZMK slot frame (v10) */
#define CH_DRAGSCROLL 0x15
#define CH_LEADER 0x19 /* QMK leader channel — shared timeout id, ZMK slot frame (v10) */
#define CH_AUTOSCROLL 0x1A

#define CH_RGBMAP 0x21
#define CH_KEYSTATE 0x23
#define CH_COMBOS 0x24
#define CH_MACROS 0x25
#define CH_SCROLLSNAP 0x26 /* ZMK-line: flask_scrollsnap (v9) */
#define CH_BALLSWAP 0x27 /* ZMK-line: flask_ballswap (v11); 0x1B-0x1F are QMK's */

/* RGB map values (channel 0x21, QMK NLKB16 wire shape; 0x04-0x08 are
 * imprint-line effect-engine additions, v9 — append-only ids) */
#define RGBMAP_ENABLED 0x01
#define RGBMAP_LAYERS 0x02 /* RO */
#define RGBMAP_LEDS 0x03   /* RO */
#define RGBMAP_EFFECT 0x04 /* 0 off / 1 solid / 2 breathe / 3 spectrum / 4 swirl */
#define RGBMAP_EFFECT_SPEED 0x05
#define RGBMAP_EFFECT_HUE 0x06
#define RGBMAP_EFFECT_SAT 0x07
#define RGBMAP_EFFECT_VAL 0x08
#define RGBMAP_LED 0x10    /* payload-addressed: [layer, led, h, s, v] */
#define RGBMAP_FILL 0x12   /* payload-addressed: [layer, h, s, v] */

/* Keystate values (read-only) */
#define KEYSTATE_BITMAP 0x01 /* payload = pressed bitmap, byte N/8 bit N%8 */

/* Combos values (channel 0x24) */
#define COMBOS_ENABLED 0x01
#define COMBOS_SLOT_COUNT 0x02 /* RO */
#define COMBOS_TIMEOUT 0x03    /* global candidate window, ms */
#define COMBOS_KEYS 0x04       /* RO (v9) — positions per slot; sizes the slot frame */
#define COMBOS_SLOT 0x10       /* payload-addressed: [slot, pos x KEYS, usage u32 BE] */

/* Macros values (channel 0x25) */
#define MACROS_ENABLED 0x01
#define MACROS_SLOT_COUNT 0x02 /* RO */
#define MACROS_STEP_COUNT 0x03 /* RO — steps per slot */
#define MACROS_TAP_MS 0x04     /* tap step down→up, ms */
#define MACROS_WAIT_MS 0x05    /* between steps, ms */
#define MACROS_STATE 0x06      /* live: GET = playing slot+1 (0 idle); SET v>0 plays v-1, 0 stops */
#define MACROS_STEP 0x10       /* payload-addressed: [slot, step, action, param u32 BE] */

/* Meta values */
#define META_PROTOCOL_VERSION 0x01
#define META_ACTIVE_LAYER 0x02
#define META_FAMILY 0x03

/* Dragscroll values — same wire vocabulary as the QMK families
 * (flaskproto.js V.dragDivH/dragDivV/dragInverted/dragInterval/
 * dragMaxNotches); invert-x is an imprint-line addition (append-only). */
#define DRAG_DIVISOR_H 0x01
#define DRAG_DIVISOR_V 0x02
#define DRAG_INVERTED 0x03 /* natural-scroll preference (vertical) */
#define DRAG_INTERVAL 0x06
#define DRAG_MAX_NOTCHES 0x07
#define DRAG_INVERT_X 0x0A /* orientation correction (horizontal) */

/* Autoscroll values — same wire vocabulary as the QMK families
 * (mad_hid_autoscroll_value in the Adept keymap.c). */
#define AS_INVERTED 0x01
#define AS_SPEED_SCALE 0x02 /* x100; 100 = Ben White's table as-is */
/* 0x03 AS_DEADZONE / 0x04 AS_RANGE retired with jog mode (proto v4) */
#define AS_STATE 0x05       /* live: GET = stepped level; SET stops */
#define AS_STOP_ON_KEY 0x06

/* Ballswap values (channel 0x1B, ZMK line — no QMK equivalent) */
#define BSWAP_SWAPPED 0x01   /* base state; the &bswap 0 key also saves */
#define BSWAP_EFFECTIVE 0x02 /* RO — base XOR momentary holds */

/* Accel values — same wire vocabulary as the QMK families (pd_accel over
 * mad_hid.c): x100 fixed-point floats, offset is SIGNED. */
#define ACCEL_ENABLED 0x01
#define ACCEL_TAKEOFF 0x02
#define ACCEL_GROWTH 0x03
#define ACCEL_OFFSET 0x04 /* i16 on the wire */
#define ACCEL_LIMIT 0x05

/* Scroll snap values (channel 0x26, ZMK line — no QMK equivalent) */
#define SNAP_ENABLED 0x01
#define SNAP_THRESHOLD 0x02 /* axis dominance percent, 50..99 */
#define SNAP_SAMPLES 0x03
#define SNAP_IMMEDIATE 0x04
#define SNAP_LOCK_MS 0x05
#define SNAP_LOCK_EVENTS 0x06
#define SNAP_IDLE_RESET 0x07

/* Leader values (channel 0x19; 0x01 = QMK leaderTimeout, slot frame at
 * 0x50 clear of QMK's u16 table 0x10-0x4D) */
#define LEADER_TIMEOUT 0x01
#define LEADER_SLOT_COUNT 0x02 /* RO */
#define LEADER_KEYS 0x03       /* RO — positions per sequence */
#define LEADER_ENABLED 0x04
#define LEADER_SLOT 0x50 /* payload-addressed: [seq, pos x KEYS, action, param u32 BE] */

/* Gestures values (channel 0x11; 0x01/0x02 = QMK ratchetStep/activeSet,
 * slot frame at 0x50 clear of QMK's u16 table 0x10-0x4F) */
#define GES_RATCHET_STEP 0x01
#define GES_ACTIVE_SET 0x02
#define GES_ENABLED 0x03
#define GES_SET_COUNT 0x04 /* RO */
#define GES_SLOT 0x50 /* payload-addressed: [set, dir, action, param u32 BE] */

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL)
struct flask_scroll_saved {
    uint8_t version;
    struct flask_scroll_params params;
} __packed;

#define SCROLL_SETTINGS_VERSION 1
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOSCROLL)
struct flask_autoscroll_saved {
    uint8_t version;
    struct flask_autoscroll_params params;
} __packed;

/* v2: params struct lost jog_deadzone/jog_range — a v1 blob is a size
 * mismatch and is ignored (tunables reseed from DT defaults). */
#define AUTOSCROLL_SETTINGS_VERSION 2
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL)
struct flask_accel_saved {
    uint8_t version;
    struct flask_accel_params params;
} __packed;

#define ACCEL_SETTINGS_VERSION 1
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSNAP)
struct flask_scrollsnap_saved {
    uint8_t version;
    struct flask_scrollsnap_params params;
} __packed;

#define SCROLLSNAP_SETTINGS_VERSION 1
#endif

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

static uint16_t rd_u16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

static bool handle_meta(uint8_t cmd, uint8_t value_id, uint8_t *payload) {
    if (cmd != CMD_GET) {
        return false; /* meta is read-only */
    }
    switch (value_id) {
    case META_PROTOCOL_VERSION:
        wr_u16(payload, FLASK_PROTO_VERSION);
        return true;
    case META_ACTIVE_LAYER:
        wr_u16(payload, zmk_keymap_highest_layer_active());
        return true;
    case META_FAMILY:
        wr_u16(payload, FLASK_FAMILY_IMPRINT);
        return true;
    default:
        return false;
    }
}

/* --- key-state bitmap (HUD live press highlight) ---
 *
 * Physical pressed positions, tracked from zmk_position_state_changed —
 * pre-keymap, so it mirrors what QMK's Vial matrix-state read shows. On a
 * split the central raises these events for peripheral keys too (absolute
 * transformed positions — the same numbering combos use). 128 positions of
 * headroom; the Imprint uses 0-69. */
#define KEYSTATE_BYTES 16

static uint8_t keystate_bitmap[KEYSTATE_BYTES];

static int flask_keystate_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || ev->position >= KEYSTATE_BYTES * 8) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->state) {
        keystate_bitmap[ev->position / 8] |= 1 << (ev->position % 8);
    } else {
        keystate_bitmap[ev->position / 8] &= ~(1 << (ev->position % 8));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_keystate, flask_keystate_listener);
ZMK_SUBSCRIPTION(flask_keystate, zmk_position_state_changed);

/* Payload is raw bitmap BYTES (not a u16 frame) — same payload-addressed
 * convention as the NLKB16 display mirror. Read-only. */
static bool handle_keystate(uint8_t cmd, uint8_t value_id, uint8_t *payload,
                            size_t payload_len) {
    if (cmd != CMD_GET || value_id != KEYSTATE_BITMAP) {
        return false;
    }
    memcpy(payload, keystate_bitmap, MIN((size_t)KEYSTATE_BYTES, payload_len));
    return true;
}

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL)
static bool handle_accel(uint8_t cmd, uint8_t value_id, uint8_t *payload) {
    struct flask_accel_params p;

    if (flask_accel_params_get(&p) < 0) {
        return false;
    }

    if (cmd == CMD_SET) {
        uint16_t v = rd_u16(payload);

        switch (value_id) {
        case ACCEL_ENABLED:
            p.enabled = (v != 0);
            break;
        case ACCEL_TAKEOFF:
            p.takeoff_x100 = v;
            break;
        case ACCEL_GROWTH:
            p.growth_x100 = v;
            break;
        case ACCEL_OFFSET:
            p.offset_x100 = (int16_t)v; /* signed on the wire */
            break;
        case ACCEL_LIMIT:
            p.limit_x100 = v;
            break;
        default:
            return false;
        }
        flask_accel_params_set(&p);
        /* fall through to GET so the echo carries the clamped value */
        if (flask_accel_params_get(&p) < 0) {
            return false;
        }
    } else if (cmd != CMD_GET) {
        return false;
    }

    switch (value_id) {
    case ACCEL_ENABLED:
        wr_u16(payload, p.enabled ? 1 : 0);
        return true;
    case ACCEL_TAKEOFF:
        wr_u16(payload, p.takeoff_x100);
        return true;
    case ACCEL_GROWTH:
        wr_u16(payload, p.growth_x100);
        return true;
    case ACCEL_OFFSET:
        wr_u16(payload, (uint16_t)p.offset_x100);
        return true;
    case ACCEL_LIMIT:
        wr_u16(payload, p.limit_x100);
        return true;
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL */

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSNAP)
static bool handle_scrollsnap(uint8_t cmd, uint8_t value_id, uint8_t *payload) {
    struct flask_scrollsnap_params p;

    if (flask_scrollsnap_params_get(&p) < 0) {
        return false;
    }

    if (cmd == CMD_SET) {
        uint16_t v = rd_u16(payload);

        switch (value_id) {
        case SNAP_ENABLED:
            p.enabled = (v != 0);
            break;
        case SNAP_THRESHOLD:
            p.threshold_pct = (uint8_t)MIN(v, 255);
            break;
        case SNAP_SAMPLES:
            p.samples = (uint8_t)MIN(v, 255);
            break;
        case SNAP_IMMEDIATE:
            p.immediate_thresh = v;
            break;
        case SNAP_LOCK_MS:
            p.lock_ms = v;
            break;
        case SNAP_LOCK_EVENTS:
            p.lock_events = v;
            break;
        case SNAP_IDLE_RESET:
            p.idle_reset_ms = v;
            break;
        default:
            return false;
        }
        flask_scrollsnap_params_set(&p);
        /* fall through to GET so the echo carries the clamped value */
        if (flask_scrollsnap_params_get(&p) < 0) {
            return false;
        }
    } else if (cmd != CMD_GET) {
        return false;
    }

    switch (value_id) {
    case SNAP_ENABLED:
        wr_u16(payload, p.enabled ? 1 : 0);
        return true;
    case SNAP_THRESHOLD:
        wr_u16(payload, p.threshold_pct);
        return true;
    case SNAP_SAMPLES:
        wr_u16(payload, p.samples);
        return true;
    case SNAP_IMMEDIATE:
        wr_u16(payload, p.immediate_thresh);
        return true;
    case SNAP_LOCK_MS:
        wr_u16(payload, p.lock_ms);
        return true;
    case SNAP_LOCK_EVENTS:
        wr_u16(payload, p.lock_events);
        return true;
    case SNAP_IDLE_RESET:
        wr_u16(payload, p.idle_reset_ms);
        return true;
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSNAP */

#if IS_ENABLED(CONFIG_ZMK_FLASK_LEADER)
/* Channel 0x19 — the slot value is a PAYLOAD-ADDRESSED byte frame:
 * [seq, pos x KEYS (0xFF = empty), action, param u32 BE]. GET reads [seq]
 * and answers in place; SET applies (normalized) and echoes what stuck. */
static bool handle_leader(uint8_t cmd, uint8_t value_id, uint8_t *payload, size_t payload_len) {
    switch (value_id) {
    case LEADER_ENABLED:
        if (cmd == CMD_SET) {
            flask_leader_set_enabled(rd_u16(payload) != 0);
        }
        wr_u16(payload, flask_leader_enabled() ? 1 : 0);
        return true;
    case LEADER_TIMEOUT:
        if (cmd == CMD_SET) {
            flask_leader_set_timeout_ms(rd_u16(payload));
        }
        wr_u16(payload, flask_leader_timeout_ms());
        return true;
    case LEADER_SLOT_COUNT:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_leader_slot_count());
        return true;
    case LEADER_KEYS:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, FLASK_LEADER_KEYS);
        return true;
    case LEADER_SLOT: {
        const size_t a = 1 + FLASK_LEADER_KEYS; /* action offset */

        if (payload_len < a + 1 + 4) {
            return false;
        }
        uint8_t seq = payload[0];
        struct flask_leader_slot s;

        if (cmd == CMD_SET) {
            memcpy(s.pos, &payload[1], FLASK_LEADER_KEYS);
            s.out.action = payload[a];
            s.out.param = ((uint32_t)payload[a + 1] << 24) | ((uint32_t)payload[a + 2] << 16) |
                          ((uint32_t)payload[a + 3] << 8) | payload[a + 4];
            if (flask_leader_slot_set(seq, &s) != 0) {
                return false;
            }
        }
        if (flask_leader_slot_get(seq, &s) != 0) {
            return false;
        }
        memcpy(&payload[1], s.pos, FLASK_LEADER_KEYS);
        payload[a] = s.out.action;
        payload[a + 1] = s.out.param >> 24;
        payload[a + 2] = s.out.param >> 16;
        payload[a + 3] = s.out.param >> 8;
        payload[a + 4] = s.out.param;
        return true;
    }
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_FLASK_LEADER */

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES)
/* Channel 0x11 — the slot value is a PAYLOAD-ADDRESSED byte frame:
 * [set, dir (0-7, E..NE clockwise), action, param u32 BE]. */
static bool handle_gestures(uint8_t cmd, uint8_t value_id, uint8_t *payload,
                            size_t payload_len) {
    switch (value_id) {
    case GES_ENABLED:
        if (cmd == CMD_SET) {
            flask_gestures_set_enabled(rd_u16(payload) != 0);
        }
        wr_u16(payload, flask_gestures_enabled() ? 1 : 0);
        return true;
    case GES_RATCHET_STEP:
        if (cmd == CMD_SET) {
            flask_gestures_set_ratchet_step(rd_u16(payload));
        }
        wr_u16(payload, flask_gestures_ratchet_step());
        return true;
    case GES_ACTIVE_SET:
        if (cmd == CMD_SET) {
            flask_gestures_set_active_set((uint8_t)rd_u16(payload));
        }
        wr_u16(payload, flask_gestures_active_set());
        return true;
    case GES_SET_COUNT:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_gestures_set_count());
        return true;
    case GES_SLOT: {
        if (payload_len < 2 + 1 + 4) {
            return false;
        }
        uint8_t set = payload[0];
        uint8_t dir = payload[1];
        struct flask_output out;

        if (cmd == CMD_SET) {
            out.action = payload[2];
            out.param = ((uint32_t)payload[3] << 24) | ((uint32_t)payload[4] << 16) |
                        ((uint32_t)payload[5] << 8) | payload[6];
            if (flask_gestures_output_set(set, dir, &out) != 0) {
                return false;
            }
        }
        if (flask_gestures_output_get(set, dir, &out) != 0) {
            return false;
        }
        payload[2] = out.action;
        payload[3] = out.param >> 24;
        payload[4] = out.param >> 16;
        payload[5] = out.param >> 8;
        payload[6] = out.param;
        return true;
    }
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES */

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_BALLSWAP)
static bool handle_ballswap(uint8_t cmd, uint8_t value_id, uint8_t *payload) {
    switch (value_id) {
    case BSWAP_SWAPPED:
        if (cmd == CMD_SET) {
            flask_ballswap_set_swapped(rd_u16(payload) != 0);
        }
        wr_u16(payload, flask_ballswap_swapped() ? 1 : 0);
        return true;
    case BSWAP_EFFECTIVE:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_ballswap_effective() ? 1 : 0);
        return true;
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_INPUT_PROCESSOR_FLASK_BALLSWAP */

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL)
static bool handle_dragscroll(uint8_t cmd, uint8_t value_id, uint8_t *payload) {
    struct flask_scroll_params p;

    if (flask_scroll_params_get(&p) < 0) {
        return false;
    }

    if (cmd == CMD_SET) {
        uint16_t v = rd_u16(payload);

        switch (value_id) {
        case DRAG_DIVISOR_H:
            p.divisor_x = v;
            break;
        case DRAG_DIVISOR_V:
            p.divisor_y = v;
            break;
        case DRAG_INVERTED:
            p.invert_y = (v != 0);
            break;
        case DRAG_INTERVAL:
            p.interval_ms = v;
            break;
        case DRAG_MAX_NOTCHES:
            p.max_notches = v;
            break;
        case DRAG_INVERT_X:
            p.invert_x = (v != 0);
            break;
        default:
            return false;
        }
        flask_scroll_params_set(&p);
        /* fall through to GET so the echo carries the clamped value */
        if (flask_scroll_params_get(&p) < 0) {
            return false;
        }
    } else if (cmd != CMD_GET) {
        return false;
    }

    switch (value_id) {
    case DRAG_DIVISOR_H:
        wr_u16(payload, (uint16_t)p.divisor_x);
        return true;
    case DRAG_DIVISOR_V:
        wr_u16(payload, (uint16_t)p.divisor_y);
        return true;
    case DRAG_INVERTED:
        wr_u16(payload, p.invert_y ? 1 : 0);
        return true;
    case DRAG_INVERT_X:
        wr_u16(payload, p.invert_x ? 1 : 0);
        return true;
    case DRAG_INTERVAL:
        wr_u16(payload, p.interval_ms);
        return true;
    case DRAG_MAX_NOTCHES:
        wr_u16(payload, (uint16_t)p.max_notches);
        return true;
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL */

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOSCROLL)
static bool handle_autoscroll(uint8_t cmd, uint8_t value_id, uint8_t *payload) {
    struct flask_autoscroll_params p;

    if (flask_autoscroll_params_get(&p) < 0) {
        return false;
    }

    if (cmd == CMD_SET) {
        uint16_t v = rd_u16(payload);

        switch (value_id) {
        case AS_INVERTED:
            p.inverted = (v != 0);
            break;
        case AS_SPEED_SCALE:
            p.speed_scale_x100 = v;
            break;
        case AS_STOP_ON_KEY:
            p.stop_on_key = (v != 0);
            break;
        case AS_STATE:
            /* Rescue switch: any SET stops autoscroll (never persisted). */
            flask_autoscroll_stop();
            wr_u16(payload, 0);
            return true;
        default:
            return false;
        }
        flask_autoscroll_params_set(&p);
        /* fall through to GET so the echo carries the clamped value */
        if (flask_autoscroll_params_get(&p) < 0) {
            return false;
        }
    } else if (cmd != CMD_GET) {
        return false;
    }

    switch (value_id) {
    case AS_INVERTED:
        wr_u16(payload, p.inverted ? 1 : 0);
        return true;
    case AS_SPEED_SCALE:
        wr_u16(payload, p.speed_scale_x100);
        return true;
    case AS_STOP_ON_KEY:
        wr_u16(payload, p.stop_on_key ? 1 : 0);
        return true;
    case AS_STATE:
        wr_u16(payload, (uint16_t)flask_autoscroll_live_state());
        return true;
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOSCROLL */

#if IS_ENABLED(CONFIG_ZMK_FLASK_RGB)
/* Channel 0x21 — the LED/fill values are PAYLOAD-ADDRESSED byte frames
 * (the app's getBytes/setBytes path), the rest are u16. GET of the LED
 * value reads [layer, led] from the payload and answers in place with
 * [layer, led, h, s, v] — via.c-style echo semantics. */
static bool handle_rgbmap(uint8_t cmd, uint8_t value_id, uint8_t *payload,
                          size_t payload_len) {
    switch (value_id) {
    case RGBMAP_ENABLED:
        if (cmd == CMD_SET) {
            flask_rgb_set_enabled(rd_u16(payload) != 0);
        }
        wr_u16(payload, flask_rgb_enabled() ? 1 : 0);
        return true;
    case RGBMAP_LAYERS:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_rgb_layers());
        return true;
    case RGBMAP_LEDS:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_rgb_total_leds());
        return true;
    case RGBMAP_EFFECT:
        if (cmd == CMD_SET) {
            flask_rgb_set_effect((uint8_t)rd_u16(payload));
        }
        wr_u16(payload, flask_rgb_effect());
        return true;
    case RGBMAP_EFFECT_SPEED:
        if (cmd == CMD_SET) {
            flask_rgb_set_effect_speed((uint8_t)MIN(rd_u16(payload), 255));
        }
        wr_u16(payload, flask_rgb_effect_speed());
        return true;
    case RGBMAP_EFFECT_HUE:
    case RGBMAP_EFFECT_SAT:
    case RGBMAP_EFFECT_VAL: {
        uint8_t hsv[3];
        int idx = value_id - RGBMAP_EFFECT_HUE;

        flask_rgb_effect_hsv(hsv);
        if (cmd == CMD_SET) {
            hsv[idx] = (uint8_t)MIN(rd_u16(payload), 255);
            flask_rgb_set_effect_hsv(hsv);
        }
        wr_u16(payload, hsv[idx]);
        return true;
    }
    case RGBMAP_LED: {
        if (payload_len < 5) {
            return false;
        }
        uint8_t layer = payload[0];
        uint16_t led = payload[1];

        if (cmd == CMD_SET) {
            return flask_rgb_set_led(layer, led, &payload[2]) == 0;
        }
        return flask_rgb_get_led(layer, led, &payload[2]) == 0;
    }
    case RGBMAP_FILL:
        if (cmd != CMD_SET || payload_len < 4) {
            return false;
        }
        return flask_rgb_fill(payload[0], &payload[1]) == 0;
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_FLASK_RGB */

#if IS_ENABLED(CONFIG_ZMK_FLASK_COMBOS)
/* Channel 0x24 — the slot value is a PAYLOAD-ADDRESSED byte frame:
 * [slot, pos0..pos3 (0xFF = empty), usage u32 BE]. GET reads [slot] and
 * answers in place; SET applies (normalized) and echoes what stuck. */
static bool handle_combos(uint8_t cmd, uint8_t value_id, uint8_t *payload,
                          size_t payload_len) {
    switch (value_id) {
    case COMBOS_ENABLED:
        if (cmd == CMD_SET) {
            flask_combos_set_enabled(rd_u16(payload) != 0);
        }
        wr_u16(payload, flask_combos_enabled() ? 1 : 0);
        return true;
    case COMBOS_SLOT_COUNT:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_combos_slot_count());
        return true;
    case COMBOS_TIMEOUT:
        if (cmd == CMD_SET) {
            flask_combos_set_timeout_ms(rd_u16(payload));
        }
        wr_u16(payload, flask_combos_timeout_ms());
        return true;
    case COMBOS_KEYS:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, FLASK_COMBOS_KEYS);
        return true;
    case COMBOS_SLOT: {
        /* Frame is sized by COMBOS_KEYS (the app reads it first): usage
         * sits right after the position block. */
        const size_t u = 1 + FLASK_COMBOS_KEYS;

        if (payload_len < u + 4) {
            return false;
        }
        uint8_t slot = payload[0];
        struct flask_combo_slot s;

        if (cmd == CMD_SET) {
            memcpy(s.pos, &payload[1], FLASK_COMBOS_KEYS);
            s.usage = ((uint32_t)payload[u] << 24) | ((uint32_t)payload[u + 1] << 16) |
                      ((uint32_t)payload[u + 2] << 8) | payload[u + 3];
            if (flask_combos_slot_set(slot, &s) != 0) {
                return false;
            }
        }
        if (flask_combos_slot_get(slot, &s) != 0) {
            return false;
        }
        memcpy(&payload[1], s.pos, FLASK_COMBOS_KEYS);
        payload[u] = s.usage >> 24;
        payload[u + 1] = s.usage >> 16;
        payload[u + 2] = s.usage >> 8;
        payload[u + 3] = s.usage;
        return true;
    }
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_FLASK_COMBOS */

#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
/* Channel 0x25 — the step value is a PAYLOAD-ADDRESSED byte frame:
 * [slot, step, action, param u32 BE]. GET reads [slot, step] and answers in
 * place; SET applies (normalized) and echoes what stuck. MACROS_STATE is
 * live-only: playing slot+1 on GET (0 = idle), SET nonzero plays that
 * slot-1, SET 0 stops — never persisted. */
static bool handle_macros(uint8_t cmd, uint8_t value_id, uint8_t *payload,
                          size_t payload_len) {
    switch (value_id) {
    case MACROS_ENABLED:
        if (cmd == CMD_SET) {
            flask_macros_set_enabled(rd_u16(payload) != 0);
        }
        wr_u16(payload, flask_macros_enabled() ? 1 : 0);
        return true;
    case MACROS_SLOT_COUNT:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_macros_slot_count());
        return true;
    case MACROS_STEP_COUNT:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_macros_step_count());
        return true;
    case MACROS_TAP_MS:
        if (cmd == CMD_SET) {
            flask_macros_set_tap_ms(rd_u16(payload));
        }
        wr_u16(payload, flask_macros_tap_ms());
        return true;
    case MACROS_WAIT_MS:
        if (cmd == CMD_SET) {
            flask_macros_set_wait_ms(rd_u16(payload));
        }
        wr_u16(payload, flask_macros_wait_ms());
        return true;
    case MACROS_STATE:
        if (cmd == CMD_SET) {
            uint16_t v = rd_u16(payload);

            if (v == 0) {
                flask_macros_stop();
            } else if (flask_macros_play(v - 1) != 0) {
                return false;
            }
        }
        wr_u16(payload, (uint16_t)(flask_macros_playing_slot() + 1));
        return true;
    case MACROS_STEP: {
        if (payload_len < 2 + 1 + 4) {
            return false;
        }
        uint8_t slot = payload[0];
        uint8_t step = payload[1];
        struct flask_macro_step s;

        if (cmd == CMD_SET) {
            s.action = payload[2];
            s.param = ((uint32_t)payload[3] << 24) | ((uint32_t)payload[4] << 16) |
                      ((uint32_t)payload[5] << 8) | payload[6];
            if (flask_macros_step_set(slot, step, &s) != 0) {
                return false;
            }
        }
        if (flask_macros_step_get(slot, step, &s) != 0) {
            return false;
        }
        payload[2] = s.action;
        payload[3] = s.param >> 24;
        payload[4] = s.param >> 16;
        payload[5] = s.param >> 8;
        payload[6] = s.param;
        return true;
    }
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_FLASK_MACROS */

static bool handle_save(uint8_t channel) {
    switch (channel) {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL)
    case CH_DRAGSCROLL: {
        struct flask_scroll_saved saved = {.version = SCROLL_SETTINGS_VERSION};

        if (flask_scroll_params_get(&saved.params) < 0) {
            return false;
        }
        int err = settings_save_one("flask/scroll", &saved, sizeof(saved));
        if (err) {
            LOG_ERR("flask/scroll settings save failed: %d", err);
            return false;
        }
        return true;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOSCROLL)
    case CH_AUTOSCROLL: {
        struct flask_autoscroll_saved saved = {.version = AUTOSCROLL_SETTINGS_VERSION};

        if (flask_autoscroll_params_get(&saved.params) < 0) {
            return false;
        }
        int err = settings_save_one("flask/autoscroll", &saved, sizeof(saved));
        if (err) {
            LOG_ERR("flask/autoscroll settings save failed: %d", err);
            return false;
        }
        return true;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL)
    case CH_ACCEL: {
        struct flask_accel_saved saved = {.version = ACCEL_SETTINGS_VERSION};

        if (flask_accel_params_get(&saved.params) < 0) {
            return false;
        }
        int err = settings_save_one("flask/accel", &saved, sizeof(saved));
        if (err) {
            LOG_ERR("flask/accel settings save failed: %d", err);
            return false;
        }
        return true;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSNAP)
    case CH_SCROLLSNAP: {
        struct flask_scrollsnap_saved saved = {.version = SCROLLSNAP_SETTINGS_VERSION};

        if (flask_scrollsnap_params_get(&saved.params) < 0) {
            return false;
        }
        int err = settings_save_one("flask/scrollsnap", &saved, sizeof(saved));
        if (err) {
            LOG_ERR("flask/scrollsnap settings save failed: %d", err);
            return false;
        }
        return true;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_RGB)
    case CH_RGBMAP:
        return flask_rgb_save() == 0;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_COMBOS)
    case CH_COMBOS:
        return flask_combos_save() == 0;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
    case CH_MACROS:
        return flask_macros_save() == 0;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_LEADER)
    case CH_LEADER:
        return flask_leader_save() == 0;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_BALLSWAP)
    case CH_BALLSWAP:
        return flask_ballswap_save() == 0;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES)
    case CH_GESTURES:
        return flask_gestures_save() == 0;
#endif
    default:
        return false;
    }
}

static int flask_proto_received(const zmk_event_t *eh) {
    struct raw_hid_received_event *event = as_raw_hid_received_event(eh);

    if (event == NULL || event->length < 5) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    static uint8_t reply[CONFIG_RAW_HID_REPORT_SIZE];
    memset(reply, 0, sizeof(reply));
    memcpy(reply, event->data, MIN(event->length, sizeof(reply)));

    uint8_t cmd = reply[0];
    uint8_t channel = reply[1];
    uint8_t value_id = reply[2];
    uint8_t *payload = &reply[3];
    bool ok = false;

    switch (cmd) {
    case CMD_GET:
    case CMD_SET:
        switch (channel) {
        case CH_META:
            ok = handle_meta(cmd, value_id, payload);
            break;
        case CH_KEYSTATE:
            ok = handle_keystate(cmd, value_id, payload, sizeof(reply) - 3);
            break;
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL)
        case CH_DRAGSCROLL:
            ok = handle_dragscroll(cmd, value_id, payload);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOSCROLL)
        case CH_AUTOSCROLL:
            ok = handle_autoscroll(cmd, value_id, payload);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_BALLSWAP)
        case CH_BALLSWAP:
            ok = handle_ballswap(cmd, value_id, payload);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL)
        case CH_ACCEL:
            ok = handle_accel(cmd, value_id, payload);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSNAP)
        case CH_SCROLLSNAP:
            ok = handle_scrollsnap(cmd, value_id, payload);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_LEADER)
        case CH_LEADER:
            ok = handle_leader(cmd, value_id, payload, sizeof(reply) - 3);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES)
        case CH_GESTURES:
            ok = handle_gestures(cmd, value_id, payload, sizeof(reply) - 3);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_RGB)
        case CH_RGBMAP:
            ok = handle_rgbmap(cmd, value_id, payload, sizeof(reply) - 3);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_COMBOS)
        case CH_COMBOS:
            ok = handle_combos(cmd, value_id, payload, sizeof(reply) - 3);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
        case CH_MACROS:
            ok = handle_macros(cmd, value_id, payload, sizeof(reply) - 3);
            break;
#endif
        default:
            break;
        }
        break;
    case CMD_SAVE:
        ok = handle_save(channel);
        break;
    default:
        break;
    }

    if (!ok) {
        reply[0] = CMD_UNHANDLED;
    }

    struct raw_hid_sent_event sent = {.data = reply, .length = sizeof(reply)};
    raise_raw_hid_sent_event(sent);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_proto, flask_proto_received);
ZMK_SUBSCRIPTION(flask_proto, raw_hid_received_event);

/* --- boot-time restore of saved tuning --- */

static int flask_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL)
    if (settings_name_steq(name, "scroll", NULL)) {
        struct flask_scroll_saved saved;

        if (len != sizeof(saved)) {
            LOG_WRN("flask/scroll settings size mismatch (%d != %d)", (int)len,
                    (int)sizeof(saved));
            return -EINVAL;
        }
        if (read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            return -EIO;
        }
        if (saved.version != SCROLL_SETTINGS_VERSION) {
            LOG_WRN("flask/scroll settings version %d ignored", saved.version);
            return 0;
        }
        if (flask_scroll_params_set(&saved.params) < 0) {
            LOG_WRN("flask/scroll restore: processor not ready");
        }
        return 0;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOSCROLL)
    if (settings_name_steq(name, "autoscroll", NULL)) {
        struct flask_autoscroll_saved saved;

        if (len != sizeof(saved)) {
            LOG_WRN("flask/autoscroll settings size mismatch (%d != %d)", (int)len,
                    (int)sizeof(saved));
            return -EINVAL;
        }
        if (read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            return -EIO;
        }
        if (saved.version != AUTOSCROLL_SETTINGS_VERSION) {
            LOG_WRN("flask/autoscroll settings version %d ignored", saved.version);
            return 0;
        }
        if (flask_autoscroll_params_set(&saved.params) < 0) {
            LOG_WRN("flask/autoscroll restore: processor not ready");
        }
        return 0;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL)
    if (settings_name_steq(name, "accel", NULL)) {
        struct flask_accel_saved saved;

        if (len != sizeof(saved)) {
            LOG_WRN("flask/accel settings size mismatch (%d != %d)", (int)len,
                    (int)sizeof(saved));
            return -EINVAL;
        }
        if (read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            return -EIO;
        }
        if (saved.version != ACCEL_SETTINGS_VERSION) {
            LOG_WRN("flask/accel settings version %d ignored", saved.version);
            return 0;
        }
        if (flask_accel_params_set(&saved.params) < 0) {
            LOG_WRN("flask/accel restore: processor not ready");
        }
        return 0;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSNAP)
    if (settings_name_steq(name, "scrollsnap", NULL)) {
        struct flask_scrollsnap_saved saved;

        if (len != sizeof(saved)) {
            LOG_WRN("flask/scrollsnap settings size mismatch (%d != %d)", (int)len,
                    (int)sizeof(saved));
            return -EINVAL;
        }
        if (read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            return -EIO;
        }
        if (saved.version != SCROLLSNAP_SETTINGS_VERSION) {
            LOG_WRN("flask/scrollsnap settings version %d ignored", saved.version);
            return 0;
        }
        if (flask_scrollsnap_params_set(&saved.params) < 0) {
            LOG_WRN("flask/scrollsnap restore: processor not ready");
        }
        return 0;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_RGB)
    if (settings_name_steq(name, "rgbmap", NULL)) {
        return flask_rgb_settings_restore(len, read_cb, cb_arg);
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_BALLSWAP)
    if (settings_name_steq(name, "ballswap", NULL)) {
        return flask_ballswap_settings_restore(len, read_cb, cb_arg);
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_COMBOS)
    {
        const char *sub = NULL;

        /* Subtree since v9: "combos/cfg" + "combos/s<idx>" (sub = NULL for
         * the retired v1 whole-table leaf — the module logs and ignores). */
        if (settings_name_steq(name, "combos", &sub)) {
            return flask_combos_settings_restore(sub && sub[0] ? sub : NULL, len, read_cb,
                                                 cb_arg);
        }
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
    {
        const char *sub = NULL;

        if (settings_name_steq(name, "macros", &sub)) {
            return flask_macros_settings_restore(sub && sub[0] ? sub : NULL, len, read_cb,
                                                 cb_arg);
        }
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_LEADER)
    {
        const char *sub = NULL;

        if (settings_name_steq(name, "leader", &sub)) {
            return flask_leader_settings_restore(sub && sub[0] ? sub : NULL, len, read_cb,
                                                 cb_arg);
        }
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES)
    {
        const char *sub = NULL;

        if (settings_name_steq(name, "gestures", &sub)) {
            return flask_gestures_settings_restore(sub && sub[0] ? sub : NULL, len, read_cb,
                                                   cb_arg);
        }
    }
#endif
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(flask, "flask", NULL, flask_settings_set, NULL, NULL);
