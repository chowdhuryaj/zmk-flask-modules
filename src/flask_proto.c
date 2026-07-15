/*
 * Flask raw-HID tuning protocol for ZMK — the "imprint" family line.
 *
 * Same frame the QMK Flask keyboards speak (mad_hid.c / AdeptProtocol.swift /
 * flaskproto.js): 32-byte reports, [cmd, channel, value_id, u16 BE payload].
 * Commands: 0x07 set, 0x08 get, 0x09 save; unhandled frames echo with the
 * command byte replaced by 0xFF. Setters clamp before applying and echo the
 * applied value — callers adopt the echo.
 *
 * SAVE is the one command that hits flash (settings/NVS), so it runs on a
 * dedicated work queue and its echo is raised from there once the writes
 * have landed — the received event fires in the transport's delivery
 * context (USB set_report callback on the ~1 KB USB workqueue stack; BT RX
 * thread for BLE), and running a multi-slot settings save inline there
 * wedged the whole board (bench 4b, 2026-07-12: combos save = instant
 * death — still enumerated, nothing functional).
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

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

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

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSCALE)
#include <flask_scrollscale/flask_scrollscale.h>
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

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOMOUSE)
#include <flask_automouse/flask_automouse.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES)
#include <flask_gestures/flask_gestures.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_FLASK_CSK)
#include <flask_csk/flask_csk.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_FLASK_TAPDANCE)
#include <flask_tapdance/flask_tapdance.h>
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
/* v12 (2026-07-12): combos slot v2 (COMBOS_SLOT_V2 0x11 — typed outputs:
 * usage-hold / macro / BEHAVIOR-by-local-id with two params; legacy 0x10
 * keeps the usage-only view) + rgbmap runtime LED order (RGBMAP_LEDORDER
 * 0x0A — chunked LED→position table; the wizard's measured map lives on
 * the device now, so the reactive overlay is right without a reflash).
 * v13 (2026-07-12): auto-mouse channel 0x1B (flask_automouse, QMK autoMouse
 * wire shape — enabled 0x01, timeout ms 0x02 [0 = latch until a transparent
 * key, which is swallowed], threshold 0x03, layer index 0x04, plus the
 * ZMK-line extend-on-key 0x05; SAVE persists via "flask/automouse").
 * Replaces &zip_temp_layer on the Imprint's cursor chains.
 * v14 (2026-07-12, DT-import + shift/tap-dance round):
 * (a) combos slot v3 (COMBOS_SLOT_V3 0x12 — the v2 frame + per-combo
 * timeout u16 / prior-idle u16 / layer index [0xFF = all]); the keymap's
 * devicetree combos moved into runtime slots as compiled defaults
 * (flask,combos-defaults node), and pre-v14 saved slot shapes are DROPPED
 * on restore so the imported defaults land; (b) custom shift keys channel
 * 0x16 (flask_csk, QMK customShift channel — shared enabled 0x01 / slot
 * count 0x02, ZMK slot frame 0x50 [slot, base u32 BE, shifted u32 BE];
 * SAVE via "flask/csk"); (c) tap dance channel 0x28 (flask_tapdance —
 * enabled 0x01, slots 0x02 / taps 0x03 RO, step 0x50 [slot, tap, action,
 * behavior u16 BE, p1 u32 BE, p2 u32 BE], per-slot term 0x51 [slot, term
 * u16 BE]; SAVE via "flask/tapdance"); (d) rgbmap global brightness 0x0B
 * (percent 0-100, scales every rendered pixel on both halves, persisted
 * in the rgbmap blob v3 — v2 blobs restore at 100).
 * v15 (2026-07-15): runtime scroll speed channel 0x29 (flask_scrollscale —
 * speed percent 0x01 u16, 25..400, 100 = the keymap's compiled divisors
 * verbatim; SAVE via "flask/scrollscale"). ZMK's stock scaler divisors are
 * const DT params, so the scroll ball's speed needed a reflash; the module
 * is the stock scaler's math with the divisor scaled live. One knob scales
 * both axes, so the Imprint's 16/12 horizontal:vertical ratio holds at every
 * speed. */
#define FLASK_PROTO_VERSION 15
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
#define CH_CSK 0x16 /* QMK customShift channel — shared enabled/count ids, ZMK slot frame (v14) */
#define CH_LEADER 0x19 /* QMK leader channel — shared timeout id, ZMK slot frame (v10) */
#define CH_AUTOSCROLL 0x1A
#define CH_AUTOMOUSE 0x1B /* QMK autoMouse channel — same wire shape (v13) */

#define CH_RGBMAP 0x21
#define CH_KEYSTATE 0x23
#define CH_COMBOS 0x24
#define CH_MACROS 0x25
#define CH_SCROLLSNAP 0x26 /* ZMK-line: flask_scrollsnap (v9) */
#define CH_BALLSWAP 0x27 /* ZMK-line: flask_ballswap (v11); 0x1C-0x1F stay QMK-only */
#define CH_TAPDANCE 0x28 /* ZMK-line: flask_tapdance (v14) */
#define CH_SCROLLSCALE 0x29 /* ZMK-line: flask_scrollscale (v15) */

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
#define RGBMAP_SPLIT_LINK 0x09 /* RO — central found the peripheral's rgb GATT char */
#define RGBMAP_LEDORDER 0x0A /* payload-addressed (v12): [start, count, pos...] */
#define RGBMAP_BRIGHTNESS 0x0B /* v14: global brightness percent 0-100 */
#define RGBMAP_LED 0x10    /* payload-addressed: [layer, led, h, s, v] */
#define RGBMAP_FILL 0x12   /* payload-addressed: [layer, h, s, v] */

/* Keystate values (read-only) */
#define KEYSTATE_BITMAP 0x01 /* payload = pressed bitmap, byte N/8 bit N%8 */

/* Combos values (channel 0x24) */
#define COMBOS_ENABLED 0x01
#define COMBOS_SLOT_COUNT 0x02 /* RO */
#define COMBOS_TIMEOUT 0x03    /* global candidate window, ms */
#define COMBOS_KEYS 0x04       /* RO (v9) — positions per slot; sizes the slot frame */
#define COMBOS_SLOT 0x10       /* payload-addressed: [slot, pos x KEYS, usage u32 BE]
                                * (legacy view — reads/writes USAGE-action slots) */
#define COMBOS_SLOT_V2 0x11    /* payload-addressed (v12): [slot, pos x KEYS,
                                * action, behavior_id u16 BE, p1 u32 BE, p2 u32 BE] */
#define COMBOS_SLOT_V3 0x12    /* payload-addressed (v14): the v2 frame plus
                                * [timeout u16 BE, prior_idle u16 BE, layer]
                                * — per-combo timing/layer (DT-import round) */

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
#define META_RESET_CAUSE 0x04 /* RO — Zephyr hwinfo reset-cause bits at boot
                               * (crash forensics: SOFTWARE/CPU_LOCKUP/WATCHDOG
                               * after an unexplained reboot); answers
                               * unhandled without CONFIG_HWINFO */

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

/* Ballswap values (channel 0x27, ZMK line — no QMK equivalent) */
#define BSWAP_SWAPPED 0x01   /* base state; the &bswap 0 key also saves */
#define BSWAP_EFFECTIVE 0x02 /* RO — base XOR momentary holds */

/* Auto-mouse values (channel 0x1B) — same wire vocabulary as the QMK
 * families (flaskproto.js V.amEnabled/amTimeout/amThreshold/amLayer);
 * extend-on-key is an imprint-line addition (append-only). */
#define AM_ENABLED 0x01
#define AM_TIMEOUT 0x02   /* ms; 0 = latch until a transparent key (swallowed) */
#define AM_THRESHOLD 0x03 /* accumulated counts before trigger; 0 = any motion */
#define AM_LAYER 0x04     /* layer INDEX */
#define AM_EXTEND 0x05    /* non-transparent key on the layer re-arms the timeout */

/* Accel values — same wire vocabulary as the QMK families (pd_accel over
 * mad_hid.c): x100 fixed-point floats, offset is SIGNED. */
#define ACCEL_ENABLED 0x01
#define ACCEL_TAKEOFF 0x02
#define ACCEL_GROWTH 0x03
#define ACCEL_OFFSET 0x04 /* i16 on the wire */
#define ACCEL_LIMIT 0x05

/* Scroll snap values (channel 0x26, ZMK line — no QMK equivalent) */
/* Channel 0x29 — flask_scrollscale. Percent of the keymap's compiled
 * divisors, NOT an absolute rate: 100 = the benched default, 200 = twice as
 * fast. One knob, both axes, so the base 16:12 ratio survives. */
#define SCROLLSCALE_SPEED 0x01

#define SNAP_ENABLED 0x01
#define SNAP_THRESHOLD 0x02 /* axis dominance percent, 50..99 */
#define SNAP_SAMPLES 0x03
#define SNAP_IMMEDIATE 0x04
#define SNAP_LOCK_MS 0x05
#define SNAP_LOCK_EVENTS 0x06
#define SNAP_IDLE_RESET 0x07

/* Custom shift key values (channel 0x16; 0x01/0x02 = QMK cskEnabled/
 * cskSlotCount, slot frame at 0x50 clear of QMK's u16 pair tables at
 * 0x10+/0x30+ which cannot carry 32-bit ZMK usages) */
#define CSK_ENABLED 0x01
#define CSK_SLOT_COUNT 0x02 /* RO */
#define CSK_SLOT 0x50 /* payload-addressed: [slot, base u32 BE, shifted u32 BE] */

/* Tap dance values (channel 0x28, ZMK line — Vial serves QMK tap dance
 * over its own protocol, so there is no QMK Flask channel to mirror) */
#define TD_ENABLED 0x01
#define TD_SLOT_COUNT 0x02 /* RO */
#define TD_TAPS 0x03       /* RO — outputs per slot (max tap count) */
#define TD_STEP 0x50 /* payload-addressed: [slot, tap, action, behavior u16 BE,
                      * p1 u32 BE, p2 u32 BE] */
#define TD_CFG 0x51  /* payload-addressed: [slot, term u16 BE (0 = default 200)] */

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

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSCALE)
struct flask_scrollscale_saved {
    uint8_t version;
    struct flask_scrollscale_params params;
} __packed;

#define SCROLLSCALE_SETTINGS_VERSION 1
#endif

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

static uint16_t rd_u16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

#if IS_ENABLED(CONFIG_HWINFO)
/* Latched once at init: hwinfo reset-cause bits describe the LAST reset —
 * read early and hold so late GETs (or a hwinfo clear elsewhere) can't
 * muddy the story. Zephyr bits: 0 pin, 1 software, 2 brownout, 3 POR,
 * 4 watchdog, 5 debug, 6 security, 7 low-power wake, 8 CPU lockup, ...
 *
 * The register behind this (nRF RESETREAS) is STICKY: bits accumulate
 * across resets until written-clear, and the register sits in an
 * always-on domain — one historical watchdog fault would read as
 * "crashed last session" on every boot forever. Clear after latching so
 * each boot reports only the causes since the previous one. */
static uint16_t boot_reset_cause;

static int flask_reset_cause_init(void) {
    uint32_t cause = 0;

    if (hwinfo_get_reset_cause(&cause) == 0) {
        boot_reset_cause = (uint16_t)cause;
        (void)hwinfo_clear_reset_cause();
    }
    return 0;
}

SYS_INIT(flask_reset_cause_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif

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
#if IS_ENABLED(CONFIG_HWINFO)
    case META_RESET_CAUSE:
        wr_u16(payload, boot_reset_cause);
        return true;
#endif
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

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSCALE)
static bool handle_scrollscale(uint8_t cmd, uint8_t value_id, uint8_t *payload) {
    struct flask_scrollscale_params p;

    if (flask_scrollscale_params_get(&p) < 0) {
        return false;
    }

    if (cmd == CMD_SET) {
        uint16_t v = rd_u16(payload);

        switch (value_id) {
        case SCROLLSCALE_SPEED:
            p.speed_pct = v;
            break;
        default:
            return false;
        }
        flask_scrollscale_params_set(&p);
        /* fall through to GET so the echo carries the clamped value */
        if (flask_scrollscale_params_get(&p) < 0) {
            return false;
        }
    } else if (cmd != CMD_GET) {
        return false;
    }

    switch (value_id) {
    case SCROLLSCALE_SPEED:
        wr_u16(payload, p.speed_pct);
        return true;
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSCALE */

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

#if IS_ENABLED(CONFIG_ZMK_FLASK_CSK)
/* Channel 0x16 — the slot value is a PAYLOAD-ADDRESSED byte frame:
 * [slot, base u32 BE, shifted u32 BE]. Slot byte echoes untouched. */
static bool handle_csk(uint8_t cmd, uint8_t value_id, uint8_t *payload, size_t payload_len) {
    switch (value_id) {
    case CSK_ENABLED:
        if (cmd == CMD_SET) {
            flask_csk_set_enabled(rd_u16(payload) != 0);
        }
        wr_u16(payload, flask_csk_enabled() ? 1 : 0);
        return true;
    case CSK_SLOT_COUNT:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_csk_slot_count());
        return true;
    case CSK_SLOT: {
        if (payload_len < 1 + 4 + 4) {
            return false;
        }
        uint8_t slot = payload[0];
        struct flask_csk_slot s;

        if (cmd == CMD_SET) {
            s.base = ((uint32_t)payload[1] << 24) | ((uint32_t)payload[2] << 16) |
                     ((uint32_t)payload[3] << 8) | payload[4];
            s.shifted = ((uint32_t)payload[5] << 24) | ((uint32_t)payload[6] << 16) |
                        ((uint32_t)payload[7] << 8) | payload[8];
            if (flask_csk_slot_set(slot, &s) != 0) {
                return false;
            }
        }
        if (flask_csk_slot_get(slot, &s) != 0) {
            return false;
        }
        payload[1] = s.base >> 24;
        payload[2] = s.base >> 16;
        payload[3] = s.base >> 8;
        payload[4] = s.base;
        payload[5] = s.shifted >> 24;
        payload[6] = s.shifted >> 16;
        payload[7] = s.shifted >> 8;
        payload[8] = s.shifted;
        return true;
    }
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_FLASK_CSK */

#if IS_ENABLED(CONFIG_ZMK_FLASK_TAPDANCE)
/* Channel 0x28 — step frame [slot, tap, action, behavior u16 BE, p1 u32
 * BE, p2 u32 BE]; per-slot config frame [slot, term u16 BE]. Address
 * bytes echo untouched. */
static bool handle_tapdance(uint8_t cmd, uint8_t value_id, uint8_t *payload, size_t payload_len) {
    switch (value_id) {
    case TD_ENABLED:
        if (cmd == CMD_SET) {
            flask_tapdance_set_enabled(rd_u16(payload) != 0);
        }
        wr_u16(payload, flask_tapdance_enabled() ? 1 : 0);
        return true;
    case TD_SLOT_COUNT:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_tapdance_slot_count());
        return true;
    case TD_TAPS:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_tapdance_tap_count());
        return true;
    case TD_STEP: {
        if (payload_len < 2 + 1 + 2 + 4 + 4) {
            return false;
        }
        uint8_t slot = payload[0];
        uint8_t tap = payload[1];
        struct flask_tapdance_slot s;

        if (cmd == CMD_SET) {
            struct flask_tapdance_output out = {
                .action = payload[2],
                .behavior_id = ((uint16_t)payload[3] << 8) | payload[4],
                .param1 = ((uint32_t)payload[5] << 24) | ((uint32_t)payload[6] << 16) |
                          ((uint32_t)payload[7] << 8) | payload[8],
                .param2 = ((uint32_t)payload[9] << 24) | ((uint32_t)payload[10] << 16) |
                          ((uint32_t)payload[11] << 8) | payload[12],
            };

            if (flask_tapdance_output_set(slot, tap, &out) != 0) {
                return false;
            }
        }
        if (flask_tapdance_slot_get(slot, &s) != 0 || tap >= FLASK_TAPDANCE_TAPS) {
            return false;
        }
        const struct flask_tapdance_output *out = &s.taps[tap];

        payload[2] = out->action;
        payload[3] = out->behavior_id >> 8;
        payload[4] = out->behavior_id;
        payload[5] = out->param1 >> 24;
        payload[6] = out->param1 >> 16;
        payload[7] = out->param1 >> 8;
        payload[8] = out->param1;
        payload[9] = out->param2 >> 24;
        payload[10] = out->param2 >> 16;
        payload[11] = out->param2 >> 8;
        payload[12] = out->param2;
        return true;
    }
    case TD_CFG: {
        if (payload_len < 1 + 2) {
            return false;
        }
        uint8_t slot = payload[0];
        struct flask_tapdance_slot s;

        if (cmd == CMD_SET) {
            uint16_t term = ((uint16_t)payload[1] << 8) | payload[2];

            if (flask_tapdance_term_set(slot, term) != 0) {
                return false;
            }
        }
        if (flask_tapdance_slot_get(slot, &s) != 0) {
            return false;
        }
        payload[1] = s.term_ms >> 8;
        payload[2] = s.term_ms;
        return true;
    }
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_FLASK_TAPDANCE */

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

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOMOUSE)
static bool handle_automouse(uint8_t cmd, uint8_t value_id, uint8_t *payload) {
    struct flask_automouse_params p;

    if (flask_automouse_params_get(&p) < 0) {
        return false;
    }

    if (cmd == CMD_SET) {
        uint16_t v = rd_u16(payload);

        switch (value_id) {
        case AM_ENABLED:
            p.enabled = (v != 0);
            break;
        case AM_TIMEOUT:
            p.timeout_ms = v;
            break;
        case AM_THRESHOLD:
            p.threshold = v;
            break;
        case AM_LAYER:
            p.layer = (uint8_t)MIN(v, 255);
            break;
        case AM_EXTEND:
            p.extend_on_key = (v != 0);
            break;
        default:
            return false;
        }
        flask_automouse_params_set(&p);
        /* fall through to GET so the echo carries the clamped value */
        if (flask_automouse_params_get(&p) < 0) {
            return false;
        }
    } else if (cmd != CMD_GET) {
        return false;
    }

    switch (value_id) {
    case AM_ENABLED:
        wr_u16(payload, p.enabled ? 1 : 0);
        return true;
    case AM_TIMEOUT:
        wr_u16(payload, p.timeout_ms);
        return true;
    case AM_THRESHOLD:
        wr_u16(payload, p.threshold);
        return true;
    case AM_LAYER:
        wr_u16(payload, p.layer);
        return true;
    case AM_EXTEND:
        wr_u16(payload, p.extend_on_key ? 1 : 0);
        return true;
    default:
        return false;
    }
}
#endif /* CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOMOUSE */

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
    case RGBMAP_SPLIT_LINK:
        if (cmd != CMD_GET) {
            return false;
        }
        wr_u16(payload, flask_rgb_split_link_ready() ? 1 : 0);
        return true;
    case RGBMAP_BRIGHTNESS:
        if (cmd == CMD_SET) {
            flask_rgb_set_brightness((uint8_t)MIN(rd_u16(payload), 100));
        }
        wr_u16(payload, flask_rgb_brightness());
        return true;
    case RGBMAP_LEDORDER: {
        /* Chunked LED→position table (v12): [start, count, pos...]. The
         * start+count prefix echoes UNTOUCHED (the app's reply matcher
         * requires it) — out-of-range chunks answer unhandled instead of
         * clamping, so the app always requests exact windows. */
        if (payload_len < 3) {
            return false;
        }
        uint16_t start = payload[0];
        uint8_t count = payload[1];

        if (count == 0 || count > payload_len - 2) {
            return false;
        }
        if (cmd == CMD_SET) {
            if (flask_rgb_led_order_set(start, &payload[2], count) != count) {
                return false;
            }
        } else if (flask_rgb_led_order_get(start, &payload[2], count) != count) {
            return false;
        }
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
        /* Legacy usage-only view (pre-v12 apps). Frame is sized by
         * COMBOS_KEYS: usage sits right after the position block. GET
         * reports the usage for USAGE-action slots and 0 for anything a
         * v12 app configured (a legacy app must not misread a behavior id
         * as a usage); SET writes a USAGE-action slot. */
        const size_t u = 1 + FLASK_COMBOS_KEYS;

        if (payload_len < u + 4) {
            return false;
        }
        uint8_t slot = payload[0];
        struct flask_combo_slot s;

        if (cmd == CMD_SET) {
            memset(&s, 0, sizeof(s));
            memcpy(s.pos, &payload[1], FLASK_COMBOS_KEYS);
            s.param1 = ((uint32_t)payload[u] << 24) | ((uint32_t)payload[u + 1] << 16) |
                       ((uint32_t)payload[u + 2] << 8) | payload[u + 3];
            s.action = s.param1 ? FLASK_COMBO_OUT_USAGE : FLASK_COMBO_OUT_NONE;
            if (flask_combos_slot_set(slot, &s) != 0) {
                return false;
            }
        }
        if (flask_combos_slot_get(slot, &s) != 0) {
            return false;
        }
        uint32_t usage = s.action == FLASK_COMBO_OUT_USAGE ? s.param1 : 0;

        memcpy(&payload[1], s.pos, FLASK_COMBOS_KEYS);
        payload[u] = usage >> 24;
        payload[u + 1] = usage >> 16;
        payload[u + 2] = usage >> 8;
        payload[u + 3] = usage;
        return true;
    }
    case COMBOS_SLOT_V2: {
        /* Typed view (v12): [slot, pos x KEYS, action, behavior_id u16 BE,
         * param1 u32 BE, param2 u32 BE]. */
        const size_t a = 1 + FLASK_COMBOS_KEYS;

        if (payload_len < a + 11) {
            return false;
        }
        uint8_t slot = payload[0];
        struct flask_combo_slot s;

        if (cmd == CMD_SET) {
            /* The v12 frame carries no timing — read-modify so the slot's
             * v14 timeout/prior-idle/layer survive instead of taking stack
             * garbage (bench-5 bug A: the recurring phantom "38"). */
            if (flask_combos_slot_get(slot, &s) != 0) {
                return false;
            }
            memcpy(s.pos, &payload[1], FLASK_COMBOS_KEYS);
            s.action = payload[a];
            s.behavior_id = ((uint16_t)payload[a + 1] << 8) | payload[a + 2];
            s.param1 = ((uint32_t)payload[a + 3] << 24) | ((uint32_t)payload[a + 4] << 16) |
                       ((uint32_t)payload[a + 5] << 8) | payload[a + 6];
            s.param2 = ((uint32_t)payload[a + 7] << 24) | ((uint32_t)payload[a + 8] << 16) |
                       ((uint32_t)payload[a + 9] << 8) | payload[a + 10];
            if (flask_combos_slot_set(slot, &s) != 0) {
                return false;
            }
        }
        if (flask_combos_slot_get(slot, &s) != 0) {
            return false;
        }
        memcpy(&payload[1], s.pos, FLASK_COMBOS_KEYS);
        payload[a] = s.action;
        payload[a + 1] = s.behavior_id >> 8;
        payload[a + 2] = s.behavior_id;
        payload[a + 3] = s.param1 >> 24;
        payload[a + 4] = s.param1 >> 16;
        payload[a + 5] = s.param1 >> 8;
        payload[a + 6] = s.param1;
        payload[a + 7] = s.param2 >> 24;
        payload[a + 8] = s.param2 >> 16;
        payload[a + 9] = s.param2 >> 8;
        payload[a + 10] = s.param2;
        return true;
    }
    case COMBOS_SLOT_V3: {
        /* v14 view: the v2 frame + [timeout u16 BE, prior_idle u16 BE,
         * layer index (0xFF = all)]. Slot byte echoes untouched. */
        const size_t a = 1 + FLASK_COMBOS_KEYS;
        const size_t t = a + 11;

        if (payload_len < t + 5) {
            return false;
        }
        uint8_t slot = payload[0];
        struct flask_combo_slot s;

        if (cmd == CMD_SET) {
            memcpy(s.pos, &payload[1], FLASK_COMBOS_KEYS);
            s.action = payload[a];
            s.behavior_id = ((uint16_t)payload[a + 1] << 8) | payload[a + 2];
            s.param1 = ((uint32_t)payload[a + 3] << 24) | ((uint32_t)payload[a + 4] << 16) |
                       ((uint32_t)payload[a + 5] << 8) | payload[a + 6];
            s.param2 = ((uint32_t)payload[a + 7] << 24) | ((uint32_t)payload[a + 8] << 16) |
                       ((uint32_t)payload[a + 9] << 8) | payload[a + 10];
            s.timeout_ms = ((uint16_t)payload[t] << 8) | payload[t + 1];
            s.prior_idle_ms = ((uint16_t)payload[t + 2] << 8) | payload[t + 3];
            s.layer = payload[t + 4];
            if (flask_combos_slot_set(slot, &s) != 0) {
                return false;
            }
        }
        if (flask_combos_slot_get(slot, &s) != 0) {
            return false;
        }
        memcpy(&payload[1], s.pos, FLASK_COMBOS_KEYS);
        payload[a] = s.action;
        payload[a + 1] = s.behavior_id >> 8;
        payload[a + 2] = s.behavior_id;
        payload[a + 3] = s.param1 >> 24;
        payload[a + 4] = s.param1 >> 16;
        payload[a + 5] = s.param1 >> 8;
        payload[a + 6] = s.param1;
        payload[a + 7] = s.param2 >> 24;
        payload[a + 8] = s.param2 >> 16;
        payload[a + 9] = s.param2 >> 8;
        payload[a + 10] = s.param2;
        payload[t] = s.timeout_ms >> 8;
        payload[t + 1] = s.timeout_ms;
        payload[t + 2] = s.prior_idle_ms >> 8;
        payload[t + 3] = s.prior_idle_ms;
        payload[t + 4] = s.layer;
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
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSCALE)
    case CH_SCROLLSCALE: {
        struct flask_scrollscale_saved saved = {.version = SCROLLSCALE_SETTINGS_VERSION};

        if (flask_scrollscale_params_get(&saved.params) < 0) {
            return false;
        }
        int err = settings_save_one("flask/scrollscale", &saved, sizeof(saved));
        if (err) {
            LOG_ERR("flask/scrollscale settings save failed: %d", err);
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
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOMOUSE)
    case CH_AUTOMOUSE: {
        struct flask_automouse_saved saved = {.version = AUTOMOUSE_SETTINGS_VERSION};

        if (flask_automouse_params_get(&saved.params) < 0) {
            return false;
        }
        int err = settings_save_one("flask/automouse", &saved, sizeof(saved));
        if (err) {
            LOG_ERR("flask/automouse settings save failed: %d", err);
            return false;
        }
        return true;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES)
    case CH_GESTURES:
        return flask_gestures_save() == 0;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_CSK)
    case CH_CSK:
        return flask_csk_save() == 0;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_TAPDANCE)
    case CH_TAPDANCE:
        return flask_tapdance_save() == 0;
#endif
    default:
        return false;
    }
}

/* Dedicated context for handle_save(): its settings writes are synchronous
 * flash ops (radio-synced against the BLE controller, NVS GC can amplify
 * one save into a sector copy) and must never run on the thread that
 * delivered the HID frame. One save in flight is the legal maximum — the
 * app transport is single-in-flight and waits for the echo. */
K_THREAD_STACK_DEFINE(flask_save_stack, 2048);
static struct k_work_q flask_save_q;
static struct k_work flask_save_work;
static uint8_t save_frame[CONFIG_RAW_HID_REPORT_SIZE];
static atomic_t save_busy;

static void save_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!handle_save(save_frame[1])) {
        save_frame[0] = CMD_UNHANDLED;
    }

    /* Clear BEFORE raising: the app sends nothing until it sees this echo,
     * so save_frame cannot be overwritten mid-send — but if the clear came
     * after and this low-priority thread got preempted between the two,
     * the app's next save would bounce off a stale busy flag. */
    atomic_clear(&save_busy);

    struct raw_hid_sent_event sent = {.data = save_frame, .length = sizeof(save_frame)};
    raise_raw_hid_sent_event(sent);
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
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOMOUSE)
        case CH_AUTOMOUSE:
            ok = handle_automouse(cmd, value_id, payload);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL)
        case CH_ACCEL:
            ok = handle_accel(cmd, value_id, payload);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSCALE)
        case CH_SCROLLSCALE:
            ok = handle_scrollscale(cmd, value_id, payload);
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
#if IS_ENABLED(CONFIG_ZMK_FLASK_CSK)
        case CH_CSK:
            ok = handle_csk(cmd, value_id, payload, sizeof(reply) - 3);
            break;
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_TAPDANCE)
        case CH_TAPDANCE:
            ok = handle_tapdance(cmd, value_id, payload, sizeof(reply) - 3);
            break;
#endif
        default:
            break;
        }
        break;
    case CMD_SAVE:
        /* Deferred: the echo comes from the save queue after the flash
         * writes land. A save already in flight (app retry after a
         * timeout) echoes unhandled instead of queueing behind it. */
        if (atomic_cas(&save_busy, 0, 1)) {
            memcpy(save_frame, reply, sizeof(save_frame));
            if (k_work_submit_to_queue(&flask_save_q, &flask_save_work) >= 0) {
                return ZMK_EV_EVENT_BUBBLE;
            }
            atomic_clear(&save_busy);
        }
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
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSCALE)
    if (settings_name_steq(name, "scrollscale", NULL)) {
        struct flask_scrollscale_saved saved;

        if (len != sizeof(saved)) {
            LOG_WRN("flask/scrollscale settings size mismatch (%d != %d)", (int)len,
                    (int)sizeof(saved));
            return -EINVAL;
        }
        if (read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            return -EIO;
        }
        if (saved.version != SCROLLSCALE_SETTINGS_VERSION) {
            LOG_WRN("flask/scrollscale settings version %d ignored", saved.version);
            return 0;
        }
        flask_scrollscale_params_set(&saved.params);
        return 0;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_RGB)
    if (settings_name_steq(name, "rgbmap", NULL)) {
        return flask_rgb_settings_restore(len, read_cb, cb_arg);
    }
    if (settings_name_steq(name, "ledorder", NULL)) {
        return flask_rgb_ledorder_restore(len, read_cb, cb_arg);
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_BALLSWAP)
    if (settings_name_steq(name, "ballswap", NULL)) {
        return flask_ballswap_settings_restore(len, read_cb, cb_arg);
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOMOUSE)
    if (settings_name_steq(name, "automouse", NULL)) {
        return flask_automouse_settings_restore(len, read_cb, cb_arg);
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
#if IS_ENABLED(CONFIG_ZMK_FLASK_CSK)
    {
        const char *sub = NULL;

        if (settings_name_steq(name, "csk", &sub)) {
            return flask_csk_settings_restore(sub && sub[0] ? sub : NULL, len, read_cb, cb_arg);
        }
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_FLASK_TAPDANCE)
    {
        const char *sub = NULL;

        if (settings_name_steq(name, "tapdance", &sub)) {
            return flask_tapdance_settings_restore(sub && sub[0] ? sub : NULL, len, read_cb,
                                                   cb_arg);
        }
    }
#endif
    return -ENOENT;
}

/* h_commit runs once after the whole settings tree loads — the hook for
 * anything that must wait for "restore is complete" (combos DT defaults:
 * a saved edit or tombstoned deletion of a default must win the slot).
 *
 * SETTINGS_TABLE behavior local ids are assigned in zmk's OWN handler
 * commit, whose order relative to this one is link-dependent — on a fresh
 * device the first pass can see unassigned ids. Retry on the flask_save
 * queue until every default resolves (bounded; idempotent). */
#if IS_ENABLED(CONFIG_ZMK_FLASK_COMBOS)
static void combos_defaults_retry(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(combos_defaults_work, combos_defaults_retry);
static int combos_defaults_tries;

static void combos_defaults_retry(struct k_work *work) {
    ARG_UNUSED(work);

    if (flask_combos_defaults_commit() > 0 && ++combos_defaults_tries < 5) {
        k_work_reschedule_for_queue(&flask_save_q, &combos_defaults_work, K_MSEC(500));
    }
}
#endif

static int flask_settings_commit(void) {
#if IS_ENABLED(CONFIG_ZMK_FLASK_COMBOS)
    if (flask_combos_defaults_commit() > 0) {
        combos_defaults_tries = 0;
        k_work_reschedule_for_queue(&flask_save_q, &combos_defaults_work, K_MSEC(500));
    }
#endif
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(flask, "flask", NULL, flask_settings_set, flask_settings_commit,
                               NULL);

static int flask_proto_init(void) {
    static const struct k_work_queue_config qcfg = {.name = "flask_save"};

    k_work_init(&flask_save_work, save_work_handler);
    k_work_queue_start(&flask_save_q, flask_save_stack, K_THREAD_STACK_SIZEOF(flask_save_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, &qcfg);
    return 0;
}

SYS_INIT(flask_proto_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
