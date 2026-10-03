/*
 * flask_autoshift — global auto shift for plain keys (see
 * include/flask_autoshift/flask_autoshift.h). QMK process_auto_shift.c
 * semantics: hold a letter / digit / punctuation key past a timeout and its
 * SHIFTED character is typed; release earlier and the plain one is typed.
 * Retro shift for hold-taps lives in behavior_flask_holdtap.c and calls
 * back into this file.
 *
 * How it hooks in (no ZMK patch):
 *  - A zmk_position_state_changed listener, linked AFTER behavior_flask_
 *    holdtap.c (see CMakeLists.txt), resolves the pressed position's binding
 *    with the keymap's own layer walk. Only a resolved `&kp X` with an
 *    eligible X is taken: it swallows both position events and raises the
 *    keycode events itself, at release (plain) or at the timeout (shifted).
 *  - "Another key pressed" is seen at the RAW position, before combos:
 *    flask_proto.c's keystate listener calls flask_autoshift_flush() on
 *    every press, so a pending key is emitted before any later press takes
 *    effect (combo output, &fak, tap dance, layer key, ...).
 *  - flask_combos routes a combo's &kp / usage output through
 *    flask_autoshift_key_press/release (id 0x8000|slot).
 *
 * Threading: key state lives on the system workqueue (position events and
 * the timeout work item); the HID thread only writes `cfg` under a
 * spinlock. Each key snapshots timeout / flags at key-down.
 *
 * ===================== WIRE (Flask channel 0x2C, proto v19) =============
 * Frame: 32-byte raw-HID report [cmd, 0x2C, value_id, u16 BE, 0...]. cmd
 * 07 SET / 08 GET / 09 SAVE. Every value is a plain u16. SET applies to RAM
 * (next press) and echoes the APPLIED (clamped) value. Unknown id or module
 * absent -> byte 0 = FF.
 *   0x01 ENABLED      0/1 (nonzero -> 1)                      default 0
 *   0x02 TIMEOUT ms   50..1000                                default 175
 *   0x03 GROUPS       bit0 alpha 1 numeric 2 symbols 3 tab 4 enter
 *                     (other bits forced 0)                   default 0x0F
 *   0x04 MODIFIERS    0/1                                     default 0
 *   0x05 REPEAT       0/1                                     default 0
 *   0x06 NO_AUTO_REPEAT 0/1                                   default 0
 *   0x07 RETRO        0/1                                     default 1
 *   0x08 RETRO_LIMIT ms  0 = no limit, else 50..5000          default 500
 * SAVE writes one settings blob "flask/autoshift" and echoes after the write
 * lands (FF = write failed or a save already in flight).
 * Probe: proto >= 19 and `08 2C 02` answers.
 * ========================================================================
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/modifiers.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/keys.h>
#include <zmk/matrix.h>

#include <flask_autoshift/flask_autoshift.h>

#if IS_ENABLED(CONFIG_ZMK_FLASK_CSK)
#include <flask_csk/flask_csk.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AS_MAX_KEYS CONFIG_ZMK_FLASK_AUTOSHIFT_MAX_KEYS
#define AS_REPEAT_TERM_MS CONFIG_ZMK_FLASK_AUTOSHIFT_REPEAT_TERM_MS

#define SHIFT_MASK (MOD_LSFT | MOD_RSFT)

/* ZMK keymap encoding (usage id 0-15, page 16-23, modifiers 24-31). */
#define ENC_ID(v) ((uint16_t)((v) & 0xFFFF))
#define ENC_PAGE(v) ((uint8_t)(((v) >> 16) & 0xFF))
#define ENC_MODS(v) ((uint8_t)(((v) >> 24) & 0xFF))

/* Behavior names, resolved from devicetree (NULL when the keymap lacks the
 * node: then nothing is ever recognised as &kp and the engine stays idle). */
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_key_press)
#define AS_KP_NAME_INIT DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_key_press))
#else
#define AS_KP_NAME_INIT NULL
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_transparent)
#define AS_TRANS_NAME_INIT DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_transparent))
#else
#define AS_TRANS_NAME_INIT NULL
#endif
static const char *const AS_KP_NAME = AS_KP_NAME_INIT;
static const char *const AS_TRANS_NAME = AS_TRANS_NAME_INIT;

bool flask_autoshift_is_kp(const char *behavior_dev) {
    return behavior_dev != NULL && AS_KP_NAME != NULL && strcmp(behavior_dev, AS_KP_NAME) == 0;
}

/* ===================== config ===================== */

static struct flask_autoshift_cfg cfg = {
    .enabled = false,
    .timeout_ms = 175,
    .groups = FLASK_AUTOSHIFT_GRP_ALPHA | FLASK_AUTOSHIFT_GRP_NUMERIC |
              FLASK_AUTOSHIFT_GRP_SYMBOLS | FLASK_AUTOSHIFT_GRP_TAB,
    .modifiers = false,
    .repeat = false,
    .no_auto_repeat = false,
    .retro = true,
    .retro_limit_ms = 500,
};
static struct k_spinlock cfg_lock;

static void clamp_cfg(struct flask_autoshift_cfg *c) {
    c->enabled = c->enabled != 0;
    c->modifiers = c->modifiers != 0;
    c->repeat = c->repeat != 0;
    c->no_auto_repeat = c->no_auto_repeat != 0;
    c->retro = c->retro != 0;
    c->timeout_ms = CLAMP(c->timeout_ms, FLASK_AUTOSHIFT_TIMEOUT_MIN_MS,
                          FLASK_AUTOSHIFT_TIMEOUT_MAX_MS);
    c->groups &= FLASK_AUTOSHIFT_GRP_MASK;
    if (c->retro_limit_ms != 0) {
        c->retro_limit_ms = CLAMP(c->retro_limit_ms, FLASK_AUTOSHIFT_RETRO_LIMIT_MIN_MS,
                                  FLASK_AUTOSHIFT_RETRO_LIMIT_MAX_MS);
    }
}

void flask_autoshift_get(struct flask_autoshift_cfg *out) {
    K_SPINLOCK(&cfg_lock) { *out = cfg; }
}

void flask_autoshift_set(const struct flask_autoshift_cfg *in) {
    struct flask_autoshift_cfg c = *in;

    clamp_cfg(&c);
    K_SPINLOCK(&cfg_lock) { cfg = c; }
}

/* --- persistence: one 8 B blob "flask/autoshift" --- */

struct as_saved {
    uint8_t version;
    uint8_t enabled;
    uint16_t timeout_ms;
    uint8_t groups;
    uint8_t flags; /* bit0 modifiers, bit1 repeat, bit2 no_auto_repeat, bit3 retro */
    uint16_t retro_limit_ms;
} __packed;

BUILD_ASSERT(sizeof(struct as_saved) == 8, "settings blob is 8 bytes");

#define AS_SETTINGS_VERSION 1

int flask_autoshift_save(void) {
    struct flask_autoshift_cfg c;
    struct as_saved s = {.version = AS_SETTINGS_VERSION};

    flask_autoshift_get(&c);
    s.enabled = c.enabled;
    s.timeout_ms = c.timeout_ms;
    s.groups = c.groups;
    s.flags = (c.modifiers ? 1 : 0) | (c.repeat ? 2 : 0) | (c.no_auto_repeat ? 4 : 0) |
              (c.retro ? 8 : 0);
    s.retro_limit_ms = c.retro_limit_ms;

    int err = settings_save_one("flask/autoshift", &s, sizeof(s));

    if (err) {
        LOG_ERR("flask/autoshift settings save failed: %d", err);
    }
    return err;
}

int flask_autoshift_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg) {
    struct as_saved s;

    if (len != sizeof(s)) {
        LOG_WRN("flask/autoshift ignored (len %d)", (int)len);
        return 0;
    }
    if (read_cb(cb_arg, &s, sizeof(s)) < 0) {
        return -EIO;
    }
    if (s.version != AS_SETTINGS_VERSION) {
        LOG_WRN("flask/autoshift ignored (version %d)", s.version);
        return 0;
    }

    struct flask_autoshift_cfg c = {
        .enabled = s.enabled,
        .timeout_ms = s.timeout_ms,
        .groups = s.groups,
        .modifiers = s.flags & 1,
        .repeat = s.flags & 2,
        .no_auto_repeat = s.flags & 4,
        .retro = s.flags & 8,
        .retro_limit_ms = s.retro_limit_ms,
    };

    flask_autoshift_set(&c);
    return 0;
}

/* ===================== eligibility / shifted form ===================== */

/* Param is a bare keyboard-page usage (no mods, page 0 or 7) in an enabled
 * group. `&kp LS(X)`, `&kp EXCL`, consumer keys, modifiers: never. */
static bool eligible(uint32_t param, uint8_t groups) {
    if (ENC_MODS(param) != 0) {
        return false;
    }
    uint8_t page = ENC_PAGE(param);

    if (page != 0 && page != 7) {
        return false;
    }

    uint16_t id = ENC_ID(param);

    if (id >= 0x04 && id <= 0x1D) {
        return groups & FLASK_AUTOSHIFT_GRP_ALPHA;
    }
    if (id >= 0x1E && id <= 0x27) {
        return groups & FLASK_AUTOSHIFT_GRP_NUMERIC;
    }
    if ((id >= 0x2D && id <= 0x38) || id == 0x64) {
        return groups & FLASK_AUTOSHIFT_GRP_SYMBOLS;
    }
    if (id == 0x2B) {
        return groups & FLASK_AUTOSHIFT_GRP_TAB;
    }
    if (id == 0x28) {
        return groups & FLASK_AUTOSHIFT_GRP_ENTER;
    }
    return false;
}

uint32_t flask_autoshift_shifted(uint32_t param) {
#if IS_ENABLED(CONFIG_ZMK_FLASK_CSK)
    uint32_t repl;

    if (flask_csk_lookup(param, &repl)) {
        return repl;
    }
#endif
    return param | ((uint32_t)MOD_LSFT << 24);
}

bool flask_autoshift_mods_allow(void) {
    struct flask_autoshift_cfg c;

    flask_autoshift_get(&c);
    return c.modifiers || !(zmk_hid_get_explicit_mods() & ~SHIFT_MASK);
}

bool flask_autoshift_retro_snapshot(uint32_t tap_param, uint16_t *timeout_ms, uint16_t *limit_ms) {
    struct flask_autoshift_cfg c;

    flask_autoshift_get(&c);
    if (!c.enabled || !c.retro || !eligible(tap_param, c.groups)) {
        return false;
    }
    *timeout_ms = c.timeout_ms;
    *limit_ms = c.retro_limit_ms;
    return true;
}

/* ===================== plain-key engine ===================== */

enum ks_state {
    KS_FREE = 0, /* zero-initialised slots are free */
    KS_PENDING,  /* down, waiting for release / timeout */
    KS_HELD,     /* an output press is down; released on the physical release */
    KS_DONE,     /* output already tapped; swallow the physical release */
};

struct key_slot {
    uint16_t id; /* physical position, or 0x8000|combo slot */
    uint8_t state;
    bool repeat, no_auto_repeat;
    uint16_t timeout_ms;
    uint32_t param;
    uint32_t out; /* HELD: what to release */
    int64_t press_ts;
};

static struct key_slot keys[AS_MAX_KEYS];
static struct key_slot *pending; /* at most one */

/* Tap-then-hold memory of the last plain key. */
static struct {
    bool valid;
    uint16_t id;
    uint32_t param;
    bool shifted;
    int64_t release_ts;
} last;

static void pending_timeout(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(flask_as_work, pending_timeout);

static void emit(uint32_t out, bool pressed, int64_t ts) {
    raise_zmk_keycode_state_changed_from_encoded(out, pressed, ts);
}

static void tap(uint32_t out, int64_t ts) {
    emit(out, true, ts);
    emit(out, false, ts);
}

static struct key_slot *find_key(uint16_t id) {
    for (int i = 0; i < AS_MAX_KEYS; i++) {
        if (keys[i].state != KS_FREE && keys[i].id == id) {
            return &keys[i];
        }
    }
    return NULL;
}

static struct key_slot *alloc_key(void) {
    for (int i = 0; i < AS_MAX_KEYS; i++) {
        if (keys[i].state == KS_FREE) {
            return &keys[i];
        }
    }
    return NULL;
}

static void note_release(uint16_t id, int64_t ts) {
    if (last.valid && last.id == id) {
        last.release_ts = ts;
    }
}

/* Another press arrived (or the keystate hook saw one): the pending key is
 * typed now, shifted only if its timeout had already passed. */
static void flush_pending(int64_t ts) {
    struct key_slot *k = pending;

    if (k == NULL) {
        return;
    }
    pending = NULL;
    k_work_cancel_delayable(&flask_as_work);
    tap(ts - k->press_ts >= k->timeout_ms ? flask_autoshift_shifted(k->param) : k->param, ts);
    k->state = KS_DONE;
}

static void pending_timeout(struct k_work *work) {
    struct key_slot *k = pending;

    ARG_UNUSED(work);
    if (k == NULL) {
        return;
    }
    pending = NULL;

    int64_t now = k_uptime_get();
    uint32_t out = flask_autoshift_shifted(k->param);

    emit(out, true, now);
    if (k->repeat && !k->no_auto_repeat) {
        k->out = out;
        k->state = KS_HELD; /* the OS auto-repeats the shifted key */
    } else {
        emit(out, false, now);
        k->state = KS_DONE;
    }
    last.valid = true;
    last.id = k->id;
    last.param = k->param;
    last.shifted = true;
    last.release_ts = INT64_MIN / 2; /* key still down; keeps ts - release_ts in range */
}

/* Common press path (§4 steps 1, 3b-7). true = taken: the caller swallows
 * the press. `c` is the config snapshot for this press. */
static bool take_press(uint16_t id, uint32_t param, int64_t ts,
                       const struct flask_autoshift_cfg *c) {
    if (pending != NULL && pending->id != id) {
        flush_pending(ts);
    }

    if (!c->enabled || !eligible(param, c->groups)) {
        if (!(last.valid && last.id == id)) {
            last.valid = false;
        }
        return false;
    }

    /* QMK evaluates held mods at press. Physical / sticky Shift types by
     * itself (csk applies as today); other mods skip auto shift unless
     * MODIFIERS is on. */
    zmk_mod_flags_t mods = zmk_hid_get_explicit_mods();

    if ((mods & SHIFT_MASK) || ((mods & ~SHIFT_MASK) && !c->modifiers)) {
        return false;
    }

    struct key_slot *k = find_key(id);

    if (k != NULL) {
        /* Lost release (the keymap pressed it, or a repeat press): drop the
         * stale entry so its output cannot stick. */
        if (k == pending) {
            pending = NULL;
            k_work_cancel_delayable(&flask_as_work);
        } else if (k->state == KS_HELD) {
            emit(k->out, false, ts);
        }
        k->state = KS_FREE;
    }
    k = alloc_key();
    if (k == NULL) {
        return false; /* behaves as if auto shift were off */
    }
    k->id = id;
    k->param = param;

    /* Tap-then-hold: re-press within the repeat term holds the same output. */
    if (c->repeat && last.valid && last.id == id && last.param == param &&
        ts - last.release_ts < AS_REPEAT_TERM_MS && (c->no_auto_repeat || !last.shifted)) {
        k->out = last.shifted ? flask_autoshift_shifted(param) : param;
        k->state = KS_HELD;
        emit(k->out, true, ts);
        return true;
    }

    k->state = KS_PENDING;
    k->press_ts = ts;
    k->timeout_ms = c->timeout_ms;
    k->repeat = c->repeat;
    k->no_auto_repeat = c->no_auto_repeat;
    last.valid = false;
    pending = k;

    int64_t delay = ts + c->timeout_ms - k_uptime_get();

    k_work_schedule(&flask_as_work, K_MSEC(delay > 0 ? delay : 0));
    return true;
}

static bool take_release(uint16_t id, int64_t ts) {
    struct key_slot *k = find_key(id);

    if (k == NULL) {
        return false;
    }

    switch (k->state) {
    case KS_PENDING: {
        bool sh = ts - k->press_ts >= k->timeout_ms;

        pending = NULL;
        k_work_cancel_delayable(&flask_as_work);
        tap(sh ? flask_autoshift_shifted(k->param) : k->param, ts);
        last.valid = true;
        last.id = id;
        last.param = k->param;
        last.shifted = sh;
        last.release_ts = ts;
        break;
    }
    case KS_HELD:
        emit(k->out, false, ts);
        note_release(id, ts);
        break;
    default: /* KS_DONE: QMK rolls the repeat clock forward */
        note_release(id, ts);
        break;
    }
    k->state = KS_FREE;
    return true;
}

bool flask_autoshift_key_press(uint16_t id, uint32_t param, int64_t ts) {
    struct flask_autoshift_cfg c;

    flask_autoshift_get(&c);
    return take_press(id, param, ts, &c);
}

bool flask_autoshift_key_release(uint16_t id, int64_t ts) { return take_release(id, ts); }

void flask_autoshift_flush(uint32_t position, int64_t ts) {
    if (pending != NULL && pending->id != position) {
        flush_pending(ts);
    }
    if (last.valid && last.id != position) {
        last.valid = false;
    }
}

/* The keymap's own layer walk (zmk_keymap_position_state_changed): topmost
 * active layer whose binding is not &trans. */
static const struct zmk_behavior_binding *resolve(uint32_t position) {
    for (int idx = ZMK_KEYMAP_LAYERS_LEN - 1; idx >= 0; idx--) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(idx);

        if (id == ZMK_KEYMAP_LAYER_ID_INVAL || !zmk_keymap_layer_active(id)) {
            continue;
        }

        const struct zmk_behavior_binding *b = zmk_keymap_get_layer_binding_at_idx(id, position);

        if (b == NULL || b->behavior_dev == NULL) {
            return NULL;
        }
        if (AS_TRANS_NAME != NULL && strcmp(b->behavior_dev, AS_TRANS_NAME) == 0) {
            if (id == zmk_keymap_layer_default()) {
                return NULL;
            }
            continue;
        }
        return b;
    }
    return NULL;
}

static int autoshift_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!ev->state) {
        /* No entry: the keymap pressed it, the keymap releases it. */
        return take_release(ev->position, ev->timestamp) ? ZMK_EV_EVENT_HANDLED
                                                         : ZMK_EV_EVENT_BUBBLE;
    }

    /* Presses replayed by flask_holdtap / flask_combos did not pass the
     * keystate hook with this timestamp: flush again (no-op normally). */
    if (pending != NULL && pending->id != ev->position) {
        flush_pending(ev->timestamp);
    }

    struct flask_autoshift_cfg c;

    flask_autoshift_get(&c);
    if (ev->position >= ZMK_KEYMAP_LEN || !c.enabled) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_behavior_binding *b = resolve(ev->position);

    if (b == NULL || !flask_autoshift_is_kp(b->behavior_dev)) {
        if (!(last.valid && last.id == ev->position)) {
            last.valid = false;
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    return take_press(ev->position, b->param1, ev->timestamp, &c) ? ZMK_EV_EVENT_HANDLED
                                                                  : ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_autoshift, autoshift_position_listener);
ZMK_SUBSCRIPTION(flask_autoshift, zmk_position_state_changed);
