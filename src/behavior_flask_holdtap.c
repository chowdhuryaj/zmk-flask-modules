/*
 * flask_holdtap — hold-tap with RUNTIME timing, one slot per key position.
 *
 * ZMK's zmk,behavior-hold-tap keeps tapping-term / flavor / quick-tap /
 * prior-idle in a const devicetree config, so Studio can assign a
 * hold-tap but nothing can tune it without a reflash. This file is core
 * behavior_hold_tap.c (pinned zmk rev 484a0547) ported verbatim — same
 * decision functions, positional hold-trigger, retro-tap,
 * hold-while-undecided, captured-event replay — with ONE change: the four
 * timing values come from a runtime table indexed by the pressed key's
 * position, copied into the active hold-tap at key-down (an edit mid-hold
 * applies from the next press). Everything else (hold/tap behaviors,
 * hold-trigger-key-positions, hold-trigger-on-release, retro-tap,
 * hold-while-undecided[-linger]) stays per-node devicetree, exactly like
 * core.
 *
 * Why a port and not a wrapper: core's config lives in flash (const) and
 * its engine functions are static, so there is no hook to feed it a
 * runtime term. The port runs as a SECOND engine beside core's: its own
 * active/undecided/captured state and its own listener, linked (module
 * sources come first) before core's hold-tap listener and after every
 * other Flask listener — the same relative order core hold-tap has.
 * ONE ENGINE PER KEYMAP: if a CORE hold-tap is undecided and a flask
 * hold-tap key plus another key land inside that window, core replays its
 * captured events from its own listener (after ours), the third key skips
 * us and a second flask hold-tap in that window is lost. So a keymap that
 * binds flask hold-taps should bind NO core hold-tap (&mt, &lt, wrapped
 * ones too); unbound core nodes cost nothing.
 *
 * Deviation from core: a hold-tap press that did not come through our
 * listener (a flask_combos output, a macro step) while another flask
 * hold-tap is undecided is QUEUED with the captured events and replayed
 * after the decision. Core drops it ("another hold-tap is undecided"),
 * which with flask_combos ahead of us would lose a layer-tap combo pressed
 * right after a home-row mod.
 *
 * Keymap: &fht_x <hold-param> <tap-param>, same two-param shape as &mt;
 * each node fixes the hold/tap behaviors and positional options. Slot =
 * key position, so the configurator's per-key slider writes the slot of
 * the key it is drawing. A node with `slot = <n>` reads slot n instead
 * (VIRTUAL slots past the key positions: combo and macro hold-taps get
 * their own timing instead of borrowing a thumb's). Slots without a
 * compiled default boot at term 200 / balanced / quick-tap 0 /
 * prior-idle 0; a flask,holdtap-defaults node seeds per-slot defaults
 * (each child: key-positions = slots, timing, optional display-name).
 *
 * Retro shift (flask_autoshift, proto v19): a node whose hold and tap are
 * `&kp` (hold may also be `&mo`) takes the global auto shift setting: past
 * the autoshift timeout a lone release types the SHIFTED tap, and past the
 * tapping term the hold is deferred (never pressed) until the retro limit,
 * another key press, or release alone. With autoshift off every added line
 * is a no-op and the engine behaves exactly as without it.
 *
 * ===================== WIRE (Flask channel 0x2A, proto v17) =============
 * Frame: 32-byte raw-HID report [cmd, channel, value_id, payload...].
 * cmd 0x07 SET / 0x08 GET / 0x09 SAVE. Unhandled frames echo with cmd
 * replaced by 0xFF. All multi-byte ints are big-endian.
 *
 *  0x01 SLOT_COUNT  RO u16  = key positions + virtual slots (Totem: 44).
 *  0x50 SLOT        payload-addressed, RW:
 *         [0] slot (key position)
 *         [1..2] tapping-term ms   (SET clamps 50..1000; 0 = RESET slot to
 *                                    its compiled default, other fields
 *                                    ignored)
 *         [3..4] quick-tap ms      (clamps 0..1000; 0 = off)
 *         [5..6] require-prior-idle ms (clamps 0..1000; 0 = off)
 *         [7] flavor  0 hold-preferred / 1 balanced / 2 tap-preferred /
 *                     3 tap-unless-interrupted  (>3 → whole frame 0xFF,
 *                     nothing applied)
 *         [8] flags   RO on echo: bit0 = live value differs from the
 *                     compiled default (SET ignores this byte)
 *       SET applies live (next press) and echoes the APPLIED values; GET
 *       fills [1..8]. Slot >= SLOT_COUNT → 0xFF.
 *  0x51 DEFAULT     payload-addressed, RO: same layout as 0x50, the
 *       compiled default for [0]; flags 0.
 *  0x52 SLOT_INFO   payload-addressed, RO: [slot, kind (0 key / 1 virtual),
 *       key position (= slot for kind 0, 0xFF for virtual), name ASCII
 *       NUL-padded (bytes 3..28, the defaults child's display-name; may be
 *       empty)].
 *  SAVE (cmd 0x09, channel 0x2A): persists every slot that differs from
 *       its default ("flask/holdtap/p<slot>") and deletes saved entries
 *       that were reset. Echo arrives after the flash writes land.
 * Central-only on splits. Absent module → every 0x2A frame echoes 0xFF.
 * Full contract with byte examples: flask-holdtap-contract.md.
 * ========================================================================
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_flask_hold_tap

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <drivers/behavior.h>
#include <zmk/keys.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/behavior.h>
#include <zmk/matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>

#include <flask_holdtap/flask_holdtap.h>

#if IS_ENABLED(CONFIG_ZMK_FLASK_AUTOSHIFT)
#include <flask_autoshift/flask_autoshift.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ===================== runtime timing table ===================== */

/* Slot count = key positions, grown to cover the highest `slot = <n>` any
 * flask hold-tap node names (sizeof a union of char arrays = the max). */
#define FHT_SLOT_MEMBER(n) char s##n[DT_INST_PROP_OR(n, slot, 0) + 1];
union fht_slot_span {
    char keymap[ZMK_KEYMAP_LEN];
    DT_INST_FOREACH_STATUS_OKAY(FHT_SLOT_MEMBER)
};
#define HT_SLOTS ((int)sizeof(union fht_slot_span))
BUILD_ASSERT(HT_SLOTS <= 255, "slot index is one wire byte");
#define HT_NAME_LEN 26

static const struct flask_holdtap_timing builtin_default = {
    .term_ms = 200,
    .quick_tap_ms = 0,
    .prior_idle_ms = 0,
    .flavor = FLASK_HT_BALANCED,
};

static struct flask_holdtap_timing live[HT_SLOTS];
static struct flask_holdtap_timing defaults[HT_SLOTS];
static bool dirty[HT_SLOTS];
static bool on_flash[HT_SLOTS];
static const char *names[HT_SLOTS];
static struct k_spinlock cfg_lock;

#if DT_HAS_COMPAT_STATUS_OKAY(flask_holdtap_defaults)
#define FHD_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(flask_holdtap_defaults)

struct fhd_entry {
    uint16_t pos;
    const char *name;
    struct flask_holdtap_timing t;
};

#define FHD_CHECK(n, prop, i)                                                                      \
    BUILD_ASSERT(DT_PROP_BY_IDX(n, prop, i) < HT_SLOTS,                                            \
                 "flask,holdtap-defaults slot past the slot count");
#define FHD_CHECKS(n) DT_FOREACH_PROP_ELEM(n, key_positions, FHD_CHECK)
DT_FOREACH_CHILD(FHD_NODE, FHD_CHECKS)

#define FHD_POS(n, prop, i)                                                                        \
    {.pos = DT_PROP_BY_IDX(n, prop, i),                                                            \
     .name = DT_PROP_OR(n, display_name, ""),                                                      \
     .t = {.term_ms = DT_PROP_OR(n, tapping_term_ms, 200),                                         \
           .quick_tap_ms = DT_PROP_OR(n, quick_tap_ms, 0),                                         \
           .prior_idle_ms = DT_PROP_OR(n, require_prior_idle_ms, 0),                               \
           .flavor = DT_ENUM_IDX_OR(n, flavor, FLASK_HT_BALANCED)}},
#define FHD_CHILD(n) DT_FOREACH_PROP_ELEM(n, key_positions, FHD_POS)

static const struct fhd_entry fhd_entries[] = {DT_FOREACH_CHILD(FHD_NODE, FHD_CHILD)};
#endif

static struct flask_holdtap_timing sanitize(struct flask_holdtap_timing t) {
    t.term_ms = CLAMP(t.term_ms, FLASK_HT_TERM_MIN_MS, FLASK_HT_TERM_MAX_MS);
    t.quick_tap_ms = MIN(t.quick_tap_ms, FLASK_HT_IDLE_MAX_MS);
    t.prior_idle_ms = MIN(t.prior_idle_ms, FLASK_HT_IDLE_MAX_MS);
    return t;
}

/* PRE_KERNEL: defaults must be in place before main()'s settings_load
 * restores saved slots over them. */
static int flask_holdtap_table_init(void) {
    for (int i = 0; i < HT_SLOTS; i++) {
        defaults[i] = builtin_default;
    }
#if DT_HAS_COMPAT_STATUS_OKAY(flask_holdtap_defaults)
    for (int i = 0; i < (int)ARRAY_SIZE(fhd_entries); i++) {
        if (fhd_entries[i].pos < HT_SLOTS) {
            defaults[fhd_entries[i].pos] = sanitize(fhd_entries[i].t);
            names[fhd_entries[i].pos] = fhd_entries[i].name;
        }
    }
#endif
    memcpy(live, defaults, sizeof(live));
    return 0;
}

SYS_INIT(flask_holdtap_table_init, PRE_KERNEL_1, 0);

static bool timing_eq(const struct flask_holdtap_timing *a, const struct flask_holdtap_timing *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

uint8_t flask_holdtap_slot_count(void) { return HT_SLOTS; }

int flask_holdtap_get(uint8_t slot, struct flask_holdtap_timing *out) {
    if (slot >= HT_SLOTS || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = live[slot]; }
    return 0;
}

int flask_holdtap_default_get(uint8_t slot, struct flask_holdtap_timing *out) {
    if (slot >= HT_SLOTS || out == NULL) {
        return -EINVAL;
    }
    *out = defaults[slot]; /* written only at init */
    return 0;
}

int flask_holdtap_slot_info(uint8_t slot, uint8_t *key_pos, char *name, size_t name_len) {
    if (slot >= HT_SLOTS || key_pos == NULL || name == NULL || name_len == 0) {
        return -EINVAL;
    }
    *key_pos = slot < ZMK_KEYMAP_LEN ? slot : 0xFF;
    memset(name, 0, name_len);
    if (names[slot] != NULL) {
        strncpy(name, names[slot], name_len - 1);
    }
    return 0;
}

int flask_holdtap_set(uint8_t slot, const struct flask_holdtap_timing *in) {
    /* Flavor first: an invalid frame is rejected even when it asks for a
     * reset (contract: flavor > 3 → 0xFF, nothing applied). */
    if (slot >= HT_SLOTS || in == NULL || in->flavor > FLASK_HT_FLAVOR_MAX) {
        return -EINVAL;
    }
    struct flask_holdtap_timing t = in->term_ms == 0 ? defaults[slot] : sanitize(*in);

    K_SPINLOCK(&cfg_lock) {
        live[slot] = t;
        dirty[slot] = true;
    }
    return 0;
}

/* Timing for a press reading `position` (the node's `slot` when it has one);
 * anything past the table gets the built-in default. */
static struct flask_holdtap_timing timing_for(uint32_t position) {
    struct flask_holdtap_timing t = builtin_default;

    if (position < HT_SLOTS) {
        K_SPINLOCK(&cfg_lock) { t = live[position]; }
    }
    return t;
}

/* --- persistence (settings subtree "flask/holdtap") --- */

struct flask_holdtap_saved {
    uint8_t version;
    struct flask_holdtap_timing t;
} __packed;

#define HT_SETTINGS_VERSION 1

int flask_holdtap_save(void) {
    for (int i = 0; i < HT_SLOTS; i++) {
        struct flask_holdtap_saved saved = {.version = HT_SETTINGS_VERSION};
        bool pending, custom;

        K_SPINLOCK(&cfg_lock) {
            pending = dirty[i];
            dirty[i] = false;
            saved.t = live[i];
        }
        if (!pending) {
            continue;
        }
        custom = !timing_eq(&saved.t, &defaults[i]);
        if (!custom && !on_flash[i]) {
            continue;
        }

        char key[24];
        int err;

        snprintf(key, sizeof(key), "flask/holdtap/p%d", i);
        err = custom ? settings_save_one(key, &saved, sizeof(saved)) : settings_delete(key);
        if (err) {
            LOG_ERR("%s settings save failed: %d", key, err);
            K_SPINLOCK(&cfg_lock) { dirty[i] = true; }
            return err;
        }
        on_flash[i] = custom;
    }
    return 0;
}

int flask_holdtap_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                   void *cb_arg) {
    struct flask_holdtap_saved saved;

    if (sub == NULL || sub[0] != 'p') {
        return -ENOENT;
    }
    int idx = atoi(&sub[1]);

    if (idx < 0 || idx >= HT_SLOTS || len != sizeof(saved)) {
        LOG_WRN("flask/holdtap/%s ignored (index/len %d)", sub, (int)len);
        return 0;
    }
    if (read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
        return -EIO;
    }
    if (saved.version != HT_SETTINGS_VERSION || saved.t.flavor > FLASK_HT_FLAVOR_MAX) {
        LOG_WRN("flask/holdtap/%s ignored (version %d)", sub, saved.version);
        return 0;
    }
    K_SPINLOCK(&cfg_lock) {
        live[idx] = sanitize(saved.t);
        dirty[idx] = false;
    }
    on_flash[idx] = true;
    return 0;
}

/* ===================== engine (core hold-tap port) ===================== */

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ZMK_BHV_HOLD_TAP_MAX_HELD CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_HELD
#define ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_CAPTURED_EVENTS

#define ZMK_BHV_HOLD_TAP_POSITION_NOT_USED 9999

enum status {
    STATUS_UNDECIDED,
    STATUS_TAP,
    STATUS_HOLD_INTERRUPT,
    STATUS_HOLD_TIMER,
};

enum decision_moment {
    HT_KEY_DOWN,
    HT_KEY_UP,
    HT_OTHER_KEY_DOWN,
    HT_OTHER_KEY_UP,
    HT_TIMER_EVENT,
    HT_QUICK_TAP,
};

/* Core's config minus the four timing fields (now runtime, per slot). */
struct behavior_hold_tap_config {
    char *hold_behavior_dev;
    char *tap_behavior_dev;
    bool hold_while_undecided;
    bool hold_while_undecided_linger;
    bool retro_tap;
    bool hold_trigger_on_release;
    bool retro_shape; /* tap is &kp and hold is &kp / &mo: retro shift may apply */
    int16_t slot; /* timing slot; -1 = the pressed key position */
    int32_t hold_trigger_key_positions_len;
    int32_t hold_trigger_key_positions[];
};

struct behavior_hold_tap_data {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    struct behavior_parameter_metadata_set set;
#endif
};

struct active_hold_tap {
    int32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    uint32_t param_hold;
    uint32_t param_tap;
    int64_t timestamp;
    enum status status;
    const struct behavior_hold_tap_config *config;
    struct flask_holdtap_timing timing; /* snapshot at key-down */
    struct k_work_delayable work;
    bool work_is_cancelled;

    // initialized to -1, which is to be interpreted as "no other key has been pressed yet"
    int32_t position_of_first_other_key_pressed;

    /* Retro shift (snapshot at key-down; all zero = off). */
    bool as_retro;
    bool as_interrupted;   /* another key went down while undecided: never shifts */
    bool as_limit_armed;   /* the scheduled work is the retro limit, not the term */
    uint16_t as_timeout_ms;
    uint16_t as_limit_ms;  /* 0 = no limit */
};

static struct active_hold_tap *undecided_hold_tap = NULL;
static struct active_hold_tap active_hold_taps[ZMK_BHV_HOLD_TAP_MAX_HELD] = {};

enum captured_event_tag {
    ET_NONE,
    ET_POS_CHANGED,
    ET_CODE_CHANGED,
    ET_BINDING, /* a hold-tap press/release that bypassed our listener */
};

struct captured_binding {
    struct zmk_behavior_binding binding;
    struct zmk_behavior_binding_event event;
    bool pressed;
};

union captured_event_data {
    struct zmk_position_state_changed_event position;
    struct zmk_keycode_state_changed_event keycode;
    struct captured_binding binding;
};

struct captured_event {
    enum captured_event_tag tag;
    union captured_event_data data;
};

static struct captured_event captured_events[ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS] = {};

struct last_tapped {
    int32_t position;
    int64_t timestamp;
};

static struct last_tapped last_tapped = {INT32_MIN, INT32_MIN};

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_tapped.timestamp) {
        last_tapped.position = INT32_MIN;
        last_tapped.timestamp = timestamp;
    }
}

static void store_last_hold_tapped(struct active_hold_tap *hold_tap) {
    last_tapped.position = hold_tap->position;
    last_tapped.timestamp = hold_tap->timestamp;
}

/* Core treats a negative (unset) ms as "never"; 0 behaves the same here
 * since the comparisons are strict. */
static bool is_quick_tap(struct active_hold_tap *hold_tap) {
    if ((last_tapped.timestamp + hold_tap->timing.prior_idle_ms) > hold_tap->timestamp) {
        return true;
    } else {
        return (last_tapped.position == hold_tap->position) &&
               (last_tapped.timestamp + hold_tap->timing.quick_tap_ms) > hold_tap->timestamp;
    }
}

static int capture_event(struct captured_event *data) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS; i++) {
        if (captured_events[i].tag == ET_NONE) {
            captured_events[i] = *data;
            return 0;
        }
    }
    return -ENOMEM;
}

static bool have_captured_keydown_event(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS; i++) {
        struct captured_event *ev = &captured_events[i];
        if (ev->tag == ET_NONE) {
            return false;
        }

        if (ev->tag != ET_POS_CHANGED) {
            continue;
        }

        if (ev->data.position.data.position == position && ev->data.position.data.state) {
            return true;
        }
    }
    return false;
}

static bool have_captured_binding_press(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS; i++) {
        struct captured_event *ev = &captured_events[i];
        if (ev->tag == ET_NONE) {
            return false;
        }
        if (ev->tag == ET_BINDING && ev->data.binding.pressed &&
            ev->data.binding.event.position == position) {
            return true;
        }
    }
    return false;
}

static int capture_binding(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event, bool pressed) {
    struct captured_event capture = {
        .tag = ET_BINDING,
        .data = {.binding = {.binding = *binding, .event = event, .pressed = pressed}},
    };
    if (capture_event(&capture) != 0) {
        LOG_ERR("fht: capture buffer full, dropping hold-tap at %d", event.position);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

const struct zmk_listener zmk_listener_behavior_flask_hold_tap;

/* Replay order trick: see core release_captured_events(). Replays start AT
 * this listener, so a hold-tap that becomes undecided mid-replay captures
 * the rest, and core's hold-tap listener (linked after) still sees them. */
static void release_captured_events() {
    if (undecided_hold_tap != NULL) {
        return;
    }

    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS; i++) {
        struct captured_event *captured_event = &captured_events[i];
        enum captured_event_tag tag = captured_event->tag;

        if (tag == ET_NONE) {
            return;
        }

        captured_events[i].tag = ET_NONE;
        if (undecided_hold_tap != NULL) {
            k_msleep(10);
        }

        switch (tag) {
        case ET_CODE_CHANGED:
            ZMK_EVENT_RAISE_AT(captured_event->data.keycode, behavior_flask_hold_tap);
            break;
        case ET_POS_CHANGED:
            ZMK_EVENT_RAISE_AT(captured_event->data.position, behavior_flask_hold_tap);
            break;
        case ET_BINDING: {
            /* Copy out: the slot is free again and a hold-tap that turns
             * undecided inside this invoke may capture into it. */
            struct captured_binding b = captured_event->data.binding;
            zmk_behavior_invoke_binding(&b.binding, b.event, b.pressed);
            break;
        }
        default:
            LOG_ERR("Unhandled captured event type");
            break;
        }
    }
}

static struct active_hold_tap *find_hold_tap(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) {
        if (active_hold_taps[i].position == position) {
            return &active_hold_taps[i];
        }
    }
    return NULL;
}

static struct active_hold_tap *store_hold_tap(struct zmk_behavior_binding_event *event,
                                              uint32_t param_hold, uint32_t param_tap,
                                              const struct behavior_hold_tap_config *config) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) {
        if (active_hold_taps[i].position != ZMK_BHV_HOLD_TAP_POSITION_NOT_USED) {
            continue;
        }
        active_hold_taps[i].position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        active_hold_taps[i].source = event->source;
#endif
        active_hold_taps[i].status = STATUS_UNDECIDED;
        active_hold_taps[i].config = config;
        active_hold_taps[i].timing =
            timing_for(config->slot >= 0 ? (uint32_t)config->slot : event->position);
        active_hold_taps[i].param_hold = param_hold;
        active_hold_taps[i].param_tap = param_tap;
        active_hold_taps[i].timestamp = event->timestamp;
        active_hold_taps[i].position_of_first_other_key_pressed = -1;
        active_hold_taps[i].as_retro = false;
        active_hold_taps[i].as_interrupted = false;
        active_hold_taps[i].as_limit_armed = false;
        active_hold_taps[i].as_timeout_ms = 0;
        active_hold_taps[i].as_limit_ms = 0;
#if IS_ENABLED(CONFIG_ZMK_FLASK_AUTOSHIFT)
        active_hold_taps[i].as_retro =
            config->retro_shape && !config->hold_while_undecided &&
            flask_autoshift_retro_snapshot(param_tap, &active_hold_taps[i].as_timeout_ms,
                                           &active_hold_taps[i].as_limit_ms);
#endif
        return &active_hold_taps[i];
    }
    return NULL;
}

static void clear_hold_tap(struct active_hold_tap *hold_tap) {
    hold_tap->position = ZMK_BHV_HOLD_TAP_POSITION_NOT_USED;
    hold_tap->status = STATUS_UNDECIDED;
    hold_tap->work_is_cancelled = false;
    hold_tap->as_retro = false;
    hold_tap->as_limit_armed = false;
}

static void decide_balanced(struct active_hold_tap *hold_tap, enum decision_moment event) {
    switch (event) {
    case HT_KEY_UP:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_OTHER_KEY_UP:
        hold_tap->status = STATUS_HOLD_INTERRUPT;
        return;
    case HT_TIMER_EVENT:
        hold_tap->status = STATUS_HOLD_TIMER;
        return;
    case HT_QUICK_TAP:
        hold_tap->status = STATUS_TAP;
        return;
    default:
        return;
    }
}

static void decide_tap_preferred(struct active_hold_tap *hold_tap, enum decision_moment event) {
    switch (event) {
    case HT_KEY_UP:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_TIMER_EVENT:
        hold_tap->status = STATUS_HOLD_TIMER;
        return;
    case HT_QUICK_TAP:
        hold_tap->status = STATUS_TAP;
        return;
    default:
        return;
    }
}

static void decide_tap_unless_interrupted(struct active_hold_tap *hold_tap,
                                          enum decision_moment event) {
    switch (event) {
    case HT_KEY_UP:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_OTHER_KEY_DOWN:
        hold_tap->status = STATUS_HOLD_INTERRUPT;
        return;
    case HT_TIMER_EVENT:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_QUICK_TAP:
        hold_tap->status = STATUS_TAP;
        return;
    default:
        return;
    }
}

static void decide_hold_preferred(struct active_hold_tap *hold_tap, enum decision_moment event) {
    switch (event) {
    case HT_KEY_UP:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_OTHER_KEY_DOWN:
        hold_tap->status = STATUS_HOLD_INTERRUPT;
        return;
    case HT_TIMER_EVENT:
        hold_tap->status = STATUS_HOLD_TIMER;
        return;
    case HT_QUICK_TAP:
        hold_tap->status = STATUS_TAP;
        return;
    default:
        return;
    }
}

static int press_hold_binding(struct active_hold_tap *hold_tap) {
    struct zmk_behavior_binding_event event = {
        .position = hold_tap->position,
        .timestamp = hold_tap->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = hold_tap->source,
#endif
    };

    struct zmk_behavior_binding binding = {.behavior_dev = hold_tap->config->hold_behavior_dev,
                                           .param1 = hold_tap->param_hold};
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static int press_tap_binding(struct active_hold_tap *hold_tap) {
    struct zmk_behavior_binding_event event = {
        .position = hold_tap->position,
        .timestamp = hold_tap->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = hold_tap->source,
#endif
    };

    struct zmk_behavior_binding binding = {.behavior_dev = hold_tap->config->tap_behavior_dev,
                                           .param1 = hold_tap->param_tap};
    store_last_hold_tapped(hold_tap);
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static int release_hold_binding(struct active_hold_tap *hold_tap) {
    struct zmk_behavior_binding_event event = {
        .position = hold_tap->position,
        .timestamp = hold_tap->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = hold_tap->source,
#endif
    };

    struct zmk_behavior_binding binding = {.behavior_dev = hold_tap->config->hold_behavior_dev,
                                           .param1 = hold_tap->param_hold};
    return zmk_behavior_invoke_binding(&binding, event, false);
}

static int release_tap_binding(struct active_hold_tap *hold_tap) {
    struct zmk_behavior_binding_event event = {
        .position = hold_tap->position,
        .timestamp = hold_tap->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = hold_tap->source,
#endif
    };

    struct zmk_behavior_binding binding = {.behavior_dev = hold_tap->config->tap_behavior_dev,
                                           .param1 = hold_tap->param_tap};
    return zmk_behavior_invoke_binding(&binding, event, false);
}

/* Core retro-tap, or retro shift: past the term the hold is deferred. */
static bool is_retro(const struct active_hold_tap *hold_tap) {
    return hold_tap->config->retro_tap || hold_tap->as_retro;
}

static int press_binding(struct active_hold_tap *hold_tap) {
    if (is_retro(hold_tap) && hold_tap->status == STATUS_HOLD_TIMER) {
        return 0;
    }

    if (hold_tap->status == STATUS_HOLD_TIMER || hold_tap->status == STATUS_HOLD_INTERRUPT) {
        if (hold_tap->config->hold_while_undecided) {
            return 0;
        } else {
            return press_hold_binding(hold_tap);
        }
    } else {
        if (hold_tap->config->hold_while_undecided &&
            !hold_tap->config->hold_while_undecided_linger) {
            release_hold_binding(hold_tap);
        }
        return press_tap_binding(hold_tap);
    }
}

static int release_binding(struct active_hold_tap *hold_tap) {
    if (is_retro(hold_tap) && hold_tap->status == STATUS_HOLD_TIMER) {
        return 0;
    }

    if (hold_tap->status == STATUS_HOLD_TIMER || hold_tap->status == STATUS_HOLD_INTERRUPT) {
        return release_hold_binding(hold_tap);
    } else {
        return release_tap_binding(hold_tap);
    }
}

static bool is_first_other_key_pressed_trigger_key(struct active_hold_tap *hold_tap) {
    for (int i = 0; i < hold_tap->config->hold_trigger_key_positions_len; i++) {
        if (hold_tap->config->hold_trigger_key_positions[i] ==
            hold_tap->position_of_first_other_key_pressed) {
            return true;
        }
    }
    return false;
}

// Force a tap decision if the positional conditions for a hold decision are not met.
static void decide_positional_hold(struct active_hold_tap *hold_tap) {
    if (!(hold_tap->config->hold_trigger_key_positions_len > 0)) {
        return;
    }
    if (hold_tap->position_of_first_other_key_pressed == -1) {
        return;
    }
    if (is_first_other_key_pressed_trigger_key(hold_tap)) {
        return;
    }
    hold_tap->status = STATUS_TAP;
}

static void decide_hold_tap(struct active_hold_tap *hold_tap,
                            enum decision_moment decision_moment) {
    if (hold_tap->status != STATUS_UNDECIDED) {
        return;
    }

    if (hold_tap != undecided_hold_tap) {
        LOG_DBG("ERROR found undecided tap hold that is not the active tap hold");
        return;
    }

    if (hold_tap->config->hold_while_undecided && decision_moment == HT_KEY_DOWN) {
        press_hold_binding(hold_tap);
        return;
    }

    switch (hold_tap->timing.flavor) {
    case FLASK_HT_HOLD_PREFERRED:
        decide_hold_preferred(hold_tap, decision_moment);
        break;
    case FLASK_HT_BALANCED:
        decide_balanced(hold_tap, decision_moment);
        break;
    case FLASK_HT_TAP_PREFERRED:
        decide_tap_preferred(hold_tap, decision_moment);
        break;
    case FLASK_HT_TAP_UNLESS_INTERRUPTED:
        decide_tap_unless_interrupted(hold_tap, decision_moment);
        break;
    }

    if (hold_tap->status == STATUS_UNDECIDED) {
        return;
    }

    decide_positional_hold(hold_tap);

    LOG_DBG("fht %d decided %d (flavor %d, moment %d)", hold_tap->position, hold_tap->status,
            hold_tap->timing.flavor, decision_moment);
    undecided_hold_tap = NULL;
    press_binding(hold_tap);
    release_captured_events();
}

static void decide_retro_tap(struct active_hold_tap *hold_tap) {
    if (!is_retro(hold_tap)) {
        return;
    }
    if (hold_tap->status == STATUS_HOLD_TIMER) {
        release_binding(hold_tap);
        hold_tap->status = STATUS_TAP;
        press_binding(hold_tap);
        return;
    }
}

static void update_hold_status_for_retro_tap(uint32_t ignore_position) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) {
        struct active_hold_tap *hold_tap = &active_hold_taps[i];
        if (hold_tap->position == ignore_position ||
            hold_tap->position == ZMK_BHV_HOLD_TAP_POSITION_NOT_USED || !is_retro(hold_tap)) {
            continue;
        }
        if (hold_tap->status == STATUS_HOLD_TIMER) {
            hold_tap->status = STATUS_HOLD_INTERRUPT;
            press_binding(hold_tap);
        }
    }
}

static int on_hold_tap_binding_pressed(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_hold_tap_config *cfg = dev->config;

    if (undecided_hold_tap != NULL) {
        /* Reached us without passing our listener (combo output, macro
         * step): queue behind the undecided one, in order. Core drops it. */
        return capture_binding(binding, event, true);
    }

    struct active_hold_tap *hold_tap =
        store_hold_tap(&event, binding->param1, binding->param2, cfg);

    if (hold_tap == NULL) {
        LOG_ERR("unable to store hold-tap info, did you press more than %d hold-taps?",
                ZMK_BHV_HOLD_TAP_MAX_HELD);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    undecided_hold_tap = hold_tap;

    if (is_quick_tap(hold_tap)) {
        decide_hold_tap(hold_tap, HT_QUICK_TAP);
    }

    decide_hold_tap(hold_tap, HT_KEY_DOWN);

    // if this behavior was queued we have to adjust the timer to only
    // wait for the remaining time.
    int32_t tapping_term_ms_left =
        (hold_tap->timestamp + hold_tap->timing.term_ms) - k_uptime_get();
    k_work_schedule(&hold_tap->work, K_MSEC(tapping_term_ms_left));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_hold_tap_binding_released(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event) {
    struct active_hold_tap *hold_tap = find_hold_tap(event.position);
    if (hold_tap == NULL && have_captured_binding_press(event.position)) {
        return capture_binding(binding, event, false); /* its press is still queued */
    }
    if (hold_tap == NULL) {
        LOG_ERR("ACTIVE_HOLD_TAP_CLEANED_UP_TOO_EARLY");
        return ZMK_BEHAVIOR_OPAQUE;
    }

#if IS_ENABLED(CONFIG_ZMK_FLASK_AUTOSHIFT)
    /* Retro shift: released alone after the autoshift timeout -> the tap is
     * the shifted character. Press and release of the tap both use this
     * param. A tap already pressed (quick-tap) is never touched. */
    if (hold_tap->as_retro && !hold_tap->as_interrupted &&
        (hold_tap->status == STATUS_UNDECIDED || hold_tap->status == STATUS_HOLD_TIMER) &&
        event.timestamp - hold_tap->timestamp >= hold_tap->as_timeout_ms &&
        flask_autoshift_mods_allow()) {
        hold_tap->param_tap = flask_autoshift_shifted(hold_tap->param_tap);
    }
#endif

    int work_cancel_result = k_work_cancel_delayable(&hold_tap->work);
    if (event.timestamp > (hold_tap->timestamp + hold_tap->timing.term_ms)) {
        decide_hold_tap(hold_tap, HT_TIMER_EVENT);
    }
    if (hold_tap->as_retro && hold_tap->status == STATUS_HOLD_TIMER && hold_tap->as_limit_ms &&
        event.timestamp - hold_tap->timestamp >= hold_tap->as_limit_ms) {
        /* Released past the retro limit before its work item ran: the hold
         * was due at the limit, so it stands (no tap). */
        hold_tap->status = STATUS_HOLD_INTERRUPT;
        press_binding(hold_tap);
    }

    decide_hold_tap(hold_tap, HT_KEY_UP);
    decide_retro_tap(hold_tap);
    release_binding(hold_tap);

    if (hold_tap->config->hold_while_undecided && hold_tap->config->hold_while_undecided_linger) {
        release_hold_binding(hold_tap);
    }

    if (work_cancel_result == -EINPROGRESS) {
        hold_tap->work_is_cancelled = true;
    } else {
        clear_hold_tap(hold_tap);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static int hold_tap_parameter_metadata(const struct device *hold_tap,
                                       struct behavior_parameter_metadata *param_metadata) {
    const struct behavior_hold_tap_config *cfg = hold_tap->config;
    struct behavior_hold_tap_data *data = hold_tap->data;
    int err;
    struct behavior_parameter_metadata child_meta;

    err = behavior_get_parameter_metadata(zmk_behavior_get_binding(cfg->hold_behavior_dev),
                                          &child_meta);
    if (err < 0) {
        LOG_WRN("Failed to get the hold behavior parameter: %d", err);
        return err;
    }

    if (child_meta.sets_len > 0) {
        data->set.param1_values = child_meta.sets[0].param1_values;
        data->set.param1_values_len = child_meta.sets[0].param1_values_len;
    }

    err = behavior_get_parameter_metadata(zmk_behavior_get_binding(cfg->tap_behavior_dev),
                                          &child_meta);
    if (err < 0) {
        LOG_WRN("Failed to get the tap behavior parameter: %d", err);
        return err;
    }

    if (child_meta.sets_len > 0) {
        data->set.param2_values = child_meta.sets[0].param1_values;
        data->set.param2_values_len = child_meta.sets[0].param1_values_len;
    }

    param_metadata->sets = &data->set;
    param_metadata->sets_len = 1;

    return 0;
}
#endif

static const struct behavior_driver_api behavior_flask_hold_tap_driver_api = {
    .binding_pressed = on_hold_tap_binding_pressed,
    .binding_released = on_hold_tap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = hold_tap_parameter_metadata,
#endif
};

static int position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    update_hold_status_for_retro_tap(ev->position);

    if (undecided_hold_tap == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Retro shift: a roll never shifts. Own flag: position_of_first_other_
     * key_pressed is set on RELEASE for hold-trigger-on-release nodes. */
    if (ev->state && ev->position != undecided_hold_tap->position) {
        undecided_hold_tap->as_interrupted = true;
    }

    if ((undecided_hold_tap->config->hold_trigger_on_release != ev->state) &&
        (undecided_hold_tap->position_of_first_other_key_pressed == -1)) {
        undecided_hold_tap->position_of_first_other_key_pressed = ev->position;
    }

    if (undecided_hold_tap->position == ev->position) {
        if (ev->state) {
            LOG_ERR("hold-tap listener should be called before before most other listeners!");
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->timestamp > (undecided_hold_tap->timestamp + undecided_hold_tap->timing.term_ms)) {
        decide_hold_tap(undecided_hold_tap, HT_TIMER_EVENT);
    }

    if (undecided_hold_tap == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!ev->state && !have_captured_keydown_event(ev->position)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct captured_event capture = {
        .tag = ET_POS_CHANGED,
        .data = {.position = copy_raised_zmk_position_state_changed(ev)},
    };
    capture_event(&capture);
    decide_hold_tap(undecided_hold_tap, ev->state ? HT_OTHER_KEY_DOWN : HT_OTHER_KEY_UP);
    return ZMK_EV_EVENT_CAPTURED;
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }

    if (undecided_hold_tap == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!is_mod(ev->usage_page, ev->keycode)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (undecided_hold_tap->config->hold_while_undecided &&
        undecided_hold_tap->status == STATUS_UNDECIDED) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct captured_event capture = {
        .tag = ET_CODE_CHANGED, .data = {.keycode = copy_raised_zmk_keycode_state_changed(ev)}};
    capture_event(&capture);
    return ZMK_EV_EVENT_CAPTURED;
}

static int behavior_flask_hold_tap_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_flask_hold_tap, behavior_flask_hold_tap_listener);
ZMK_SUBSCRIPTION(behavior_flask_hold_tap, zmk_position_state_changed);
ZMK_SUBSCRIPTION(behavior_flask_hold_tap, zmk_keycode_state_changed);

static void behavior_flask_hold_tap_timer_work_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_hold_tap *hold_tap = CONTAINER_OF(d_work, struct active_hold_tap, work);

    if (hold_tap->work_is_cancelled) {
        clear_hold_tap(hold_tap);
    } else if (hold_tap->as_limit_armed) {
        /* Retro limit reached: the deferred hold stands. */
        hold_tap->as_limit_armed = false;
        if (hold_tap->status == STATUS_HOLD_TIMER) {
            hold_tap->status = STATUS_HOLD_INTERRUPT;
            press_binding(hold_tap);
        }
    } else {
        decide_hold_tap(hold_tap, HT_TIMER_EVENT);
        if (hold_tap->as_retro && hold_tap->status == STATUS_HOLD_TIMER && hold_tap->as_limit_ms) {
            /* The term passed and the hold is deferred; give it until the
             * limit (L <= term: none, the hold is due now). */
            int64_t left = hold_tap->timestamp + hold_tap->as_limit_ms - k_uptime_get();

            if (left <= 0) {
                hold_tap->status = STATUS_HOLD_INTERRUPT;
                press_binding(hold_tap);
            } else {
                hold_tap->as_limit_armed = true;
                k_work_schedule(&hold_tap->work, K_MSEC(left));
            }
        }
    }
}

static int behavior_flask_hold_tap_init(const struct device *dev) {
    static bool init_first_run = true;

    if (init_first_run) {
        for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) {
            k_work_init_delayable(&active_hold_taps[i].work,
                                  behavior_flask_hold_tap_timer_work_handler);
            active_hold_taps[i].position = ZMK_BHV_HOLD_TAP_POSITION_NOT_USED;
        }
    }
    init_first_run = false;
    return 0;
}

/* Retro shift applies to QMK's IS_RETRO shapes: tap `&kp`, hold `&kp` (MT) or
 * `&mo` (LT). Decided per node at compile time from the bindings' compats. */
#define FHT_RETRO_SHAPE(n)                                                                         \
    (DT_NODE_HAS_COMPAT(DT_INST_PHANDLE_BY_IDX(n, bindings, 1), zmk_behavior_key_press) &&         \
     (DT_NODE_HAS_COMPAT(DT_INST_PHANDLE_BY_IDX(n, bindings, 0), zmk_behavior_key_press) ||        \
      DT_NODE_HAS_COMPAT(DT_INST_PHANDLE_BY_IDX(n, bindings, 0), zmk_behavior_momentary_layer)))

#define FHT_INST(n)                                                                                \
    static const struct behavior_hold_tap_config behavior_flask_hold_tap_config_##n = {            \
        .hold_behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 0)),               \
        .tap_behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 1)),                \
        .hold_while_undecided = DT_INST_PROP(n, hold_while_undecided),                             \
        .hold_while_undecided_linger = DT_INST_PROP(n, hold_while_undecided_linger),               \
        .retro_tap = DT_INST_PROP(n, retro_tap),                                                   \
        .hold_trigger_on_release = DT_INST_PROP(n, hold_trigger_on_release),                       \
        .retro_shape = FHT_RETRO_SHAPE(n),                                                         \
        .slot = DT_INST_PROP_OR(n, slot, -1),                                                      \
        .hold_trigger_key_positions = DT_INST_PROP(n, hold_trigger_key_positions),                 \
        .hold_trigger_key_positions_len = DT_INST_PROP_LEN(n, hold_trigger_key_positions),         \
    };                                                                                             \
    static struct behavior_hold_tap_data behavior_flask_hold_tap_data_##n = {};                    \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_flask_hold_tap_init, NULL,                                 \
                            &behavior_flask_hold_tap_data_##n,                                     \
                            &behavior_flask_hold_tap_config_##n, POST_KERNEL,                      \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_flask_hold_tap_driver_api);

DT_INST_FOREACH_STATUS_OKAY(FHT_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
