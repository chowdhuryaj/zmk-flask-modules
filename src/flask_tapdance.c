/*
 * flask_tapdance — runtime tap dances (see include/flask_tapdance/).
 *
 * The dance engine is core behavior_tap_dance.c (pinned rev 484a0547)
 * with the const DT config swapped for the runtime slot table:
 *
 *  - An active dance is keyed by KEY POSITION (one per position; the
 *    same &ftd slot on two keys dances independently, like two DT
 *    instances would).
 *  - Each press stops the timer, bumps the counter, and re-arms the
 *    per-slot term; reaching the slot's configured length resolves
 *    immediately (fires the LAST output).
 *  - Term expiry resolves at the deadline timestamp; if the key was
 *    already up, the output releases right away (a completed tap).
 *  - Any OTHER key's press interrupts: undecided dances resolve first,
 *    so "td then a letter" keeps its order (core listener semantics).
 *  - Fire data is copied out of the slot at resolve time — the table can
 *    be rewritten mid-hold and the release must mirror the press.
 *
 * Config writes race the dance only at the table copy (spinlock); the
 * dance state itself is single-context like core.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>

#include <flask_tapdance/flask_tapdance.h>

#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
#include <flask_macros/flask_macros.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TD_TERM_MIN_MS 50
#define TD_TERM_MAX_MS 1000
#define TD_TERM_DEFAULT_MS 200

#define TD_MAX_HELD 4
#define TD_POSITION_FREE UINT32_MAX

/* --- config (spinlocked: raw-HID writes race the engine) --- */

static struct {
    bool enabled;
    struct flask_tapdance_slot slots[FLASK_TAPDANCE_SLOTS];
} cfg = {
    .enabled = true,
};

static struct k_spinlock cfg_lock;

static uint64_t slots_saved;
static uint64_t slots_dirty;
static bool cfg_saved;
static bool cfg_dirty;

BUILD_ASSERT(FLASK_TAPDANCE_SLOTS <= 64, "save bitmaps are u64");

/* Dance length = contiguous configured prefix. Caller holds cfg_lock. */
static uint8_t slot_len(const struct flask_tapdance_slot *s) {
    uint8_t n = 0;

    while (n < FLASK_TAPDANCE_TAPS && s->taps[n].action != FLASK_TD_OUT_NONE) {
        n++;
    }
    return n;
}

/* --- active dances (single-context, mirrors core active_tap_dances) --- */

struct active_dance {
    uint32_t position; /* TD_POSITION_FREE = free */
    uint8_t slot;
    int counter;
    bool is_pressed;
    bool timer_cancelled;
    bool decided;
    int64_t release_at;
    /* Copied at resolve: the release must mirror the press even if the
     * table was rewritten mid-hold. */
    struct flask_tapdance_output fired;
    struct k_work_delayable release_timer;
};

static struct active_dance dances[TD_MAX_HELD];

static void fire_output(const struct flask_tapdance_output *out, uint32_t position, bool pressed,
                        int64_t timestamp) {
    switch (out->action) {
    case FLASK_TD_OUT_USAGE:
        if (out->param1 != 0) {
            raise_zmk_keycode_state_changed_from_encoded(out->param1, pressed, timestamp);
        }
        break;
    case FLASK_TD_OUT_MACRO:
#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
        if (pressed) {
            flask_macros_play((uint8_t)out->param1);
        }
#endif
        break;
    case FLASK_TD_OUT_BEHAVIOR: {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
        const char *name = zmk_behavior_find_behavior_name_from_local_id(out->behavior_id);

        if (name == NULL) {
            LOG_WRN("flask_tapdance: behavior id %u not found", out->behavior_id);
            return;
        }
        struct zmk_behavior_binding binding = {
            .behavior_dev = name,
            .param1 = out->param1,
            .param2 = out->param2,
        };
        struct zmk_behavior_binding_event event = {
            .layer = zmk_keymap_highest_layer_active(),
            .position = position,
            .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
        };
        zmk_behavior_invoke_binding(&binding, event, pressed);
#else
        LOG_WRN("flask_tapdance: behavior outputs need CONFIG_ZMK_BEHAVIOR_LOCAL_IDS");
#endif
        break;
    }
    default:
        break;
    }
}

static struct active_dance *find_dance(uint32_t position) {
    for (int i = 0; i < TD_MAX_HELD; i++) {
        if (dances[i].position == position && !dances[i].timer_cancelled) {
            return &dances[i];
        }
    }
    return NULL;
}

static void clear_dance(struct active_dance *d) { d->position = TD_POSITION_FREE; }

static void stop_timer(struct active_dance *d) {
    if (k_work_cancel_delayable(&d->release_timer) == -EINPROGRESS) {
        d->timer_cancelled = true; /* too late — the handler cleans up */
    }
}

static void press_dance(struct active_dance *d, int64_t timestamp) {
    uint8_t idx;

    d->decided = true;
    K_SPINLOCK(&cfg_lock) {
        const struct flask_tapdance_slot *s = &cfg.slots[d->slot];
        uint8_t len = slot_len(s);

        idx = MIN((uint8_t)d->counter, len) - 1;
        d->fired = s->taps[idx];
    }
    fire_output(&d->fired, d->position, true, timestamp);
}

static void release_dance(struct active_dance *d, int64_t timestamp) {
    struct flask_tapdance_output out = d->fired;
    uint32_t position = d->position;

    clear_dance(d);
    fire_output(&out, position, false, timestamp);
}

static void timer_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_dance *d = CONTAINER_OF(d_work, struct active_dance, release_timer);

    if (d->position == TD_POSITION_FREE || d->timer_cancelled) {
        return;
    }
    LOG_DBG("flask_tapdance: slot %d decided by timer at %d taps", d->slot, d->counter);
    press_dance(d, d->release_at);
    if (!d->is_pressed) {
        release_dance(d, d->release_at);
    }
}

/* --- behavior entry points (&ftd) --- */

int flask_tapdance_pressed(uint8_t slot, uint32_t position, int64_t timestamp) {
    uint8_t len = 0;
    uint16_t term = TD_TERM_DEFAULT_MS;
    bool enabled;

    if (slot >= FLASK_TAPDANCE_SLOTS) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) {
        enabled = cfg.enabled;
        len = slot_len(&cfg.slots[slot]);
        if (cfg.slots[slot].term_ms) {
            term = cfg.slots[slot].term_ms;
        }
    }
    if (!enabled || len == 0) {
        LOG_DBG("flask_tapdance: slot %d empty/disabled — key does nothing", slot);
        return 0;
    }

    struct active_dance *d = find_dance(position);

    if (d == NULL) {
        for (int i = 0; i < TD_MAX_HELD; i++) {
            if (dances[i].position == TD_POSITION_FREE) {
                d = &dances[i];
                d->position = position;
                d->slot = slot;
                d->counter = 0;
                d->timer_cancelled = false;
                d->decided = false;
                d->release_at = 0;
                break;
            }
        }
        if (d == NULL) {
            LOG_ERR("flask_tapdance: no free active slot");
            return -ENOMEM;
        }
    }

    d->is_pressed = true;
    stop_timer(d);
    if (d->counter < len) {
        d->counter++;
    }
    if (d->counter == len) {
        /* Max taps — decided immediately (core semantics). */
        press_dance(d, timestamp);
        return 0;
    }
    d->release_at = timestamp + term;

    int32_t ms_left = d->release_at - k_uptime_get();

    /* reschedule, not schedule — a stale deadline must be replaced. */
    k_work_reschedule(&d->release_timer, ms_left > 0 ? K_MSEC(ms_left) : K_NO_WAIT);
    return 0;
}

int flask_tapdance_released(uint8_t slot, uint32_t position, int64_t timestamp) {
    struct active_dance *d = find_dance(position);

    ARG_UNUSED(slot);
    if (d == NULL) {
        return 0; /* dance already resolved and released (or slot empty) */
    }
    d->is_pressed = false;
    if (d->decided) {
        release_dance(d, timestamp);
    }
    return 0;
}

/* Interrupts: any OTHER key's press resolves undecided dances first, so
 * "dance key then letter" keeps its order (core listener, verbatim). */
static int td_position_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < TD_MAX_HELD; i++) {
        struct active_dance *d = &dances[i];

        if (d->position == TD_POSITION_FREE || d->position == ev->position) {
            continue;
        }
        stop_timer(d);
        if (!d->decided) {
            press_dance(d, ev->timestamp);
            if (!d->is_pressed) {
                release_dance(d, ev->timestamp);
            }
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_tapdance, td_position_listener);
ZMK_SUBSCRIPTION(flask_tapdance, zmk_position_state_changed);

/* --- runtime API (proto channel 0x28) --- */

bool flask_tapdance_enabled(void) {
    bool on;

    K_SPINLOCK(&cfg_lock) { on = cfg.enabled; }
    return on;
}

void flask_tapdance_set_enabled(bool on) {
    K_SPINLOCK(&cfg_lock) {
        cfg.enabled = on;
        cfg_dirty = true;
    }
}

uint8_t flask_tapdance_slot_count(void) { return FLASK_TAPDANCE_SLOTS; }
uint8_t flask_tapdance_tap_count(void) { return FLASK_TAPDANCE_TAPS; }

int flask_tapdance_slot_get(uint8_t idx, struct flask_tapdance_slot *out) {
    if (idx >= FLASK_TAPDANCE_SLOTS || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = cfg.slots[idx]; }
    return 0;
}

int flask_tapdance_term_set(uint8_t idx, uint16_t term_ms) {
    if (idx >= FLASK_TAPDANCE_SLOTS) {
        return -EINVAL;
    }
    if (term_ms) {
        term_ms = CLAMP(term_ms, TD_TERM_MIN_MS, TD_TERM_MAX_MS);
    }
    K_SPINLOCK(&cfg_lock) {
        cfg.slots[idx].term_ms = term_ms;
        slots_dirty |= BIT64(idx);
    }
    return 0;
}

int flask_tapdance_output_set(uint8_t idx, uint8_t tap, const struct flask_tapdance_output *in) {
    if (idx >= FLASK_TAPDANCE_SLOTS || tap >= FLASK_TAPDANCE_TAPS || in == NULL) {
        return -EINVAL;
    }

    struct flask_tapdance_output out = *in;

    if (out.action > FLASK_TD_OUT_MAX) {
        out.action = FLASK_TD_OUT_NONE;
    }
    if (out.action == FLASK_TD_OUT_USAGE && out.param1 == 0) {
        out.action = FLASK_TD_OUT_NONE;
    }
    if (out.action == FLASK_TD_OUT_NONE) {
        out.behavior_id = 0;
        out.param1 = 0;
        out.param2 = 0;
    }
    if (out.action != FLASK_TD_OUT_BEHAVIOR) {
        out.behavior_id = 0;
        if (out.action != FLASK_TD_OUT_NONE) {
            out.param2 = 0;
        }
    }
    K_SPINLOCK(&cfg_lock) {
        cfg.slots[idx].taps[tap] = out;
        slots_dirty |= BIT64(idx);
    }
    return 0;
}

/* --- persistence (settings subtree "flask/tapdance") --- */

struct flask_tapdance_saved_cfg {
    uint8_t version;
    uint8_t enabled;
} __packed;

#define TD_SETTINGS_VERSION 1

static bool slot_is_empty(const struct flask_tapdance_slot *s) {
    if (s->term_ms != 0) {
        return false;
    }
    for (int t = 0; t < FLASK_TAPDANCE_TAPS; t++) {
        if (s->taps[t].action != FLASK_TD_OUT_NONE) {
            return false;
        }
    }
    return true;
}

int flask_tapdance_save(void) {
    struct flask_tapdance_saved_cfg saved = {.version = TD_SETTINGS_VERSION};
    static struct flask_tapdance_slot slots[FLASK_TAPDANCE_SLOTS];
    uint64_t pending, saved_bits;
    bool write_cfg;

    K_SPINLOCK(&cfg_lock) {
        saved.enabled = cfg.enabled ? 1 : 0;
        memcpy(slots, cfg.slots, sizeof(slots));
        pending = slots_dirty;
        saved_bits = slots_saved;
        write_cfg = cfg_dirty || !cfg_saved;
        slots_dirty = 0;
        cfg_dirty = false;
    }

    if (write_cfg) {
        int err = settings_save_one("flask/tapdance/cfg", &saved, sizeof(saved));

        if (err) {
            LOG_ERR("flask/tapdance/cfg settings save failed: %d", err);
            K_SPINLOCK(&cfg_lock) {
                cfg_dirty = true;
                slots_dirty |= pending;
            }
            return err;
        }
        K_SPINLOCK(&cfg_lock) { cfg_saved = true; }
    }

    while (pending) {
        int i = __builtin_ctzll(pending);
        bool empty = slot_is_empty(&slots[i]);
        bool on_flash = (saved_bits & BIT64(i)) != 0;
        char key[26];
        int err = 0;

        snprintf(key, sizeof(key), "flask/tapdance/s%d", i);
        if (empty && on_flash) {
            err = settings_delete(key);
        } else if (!empty) {
            err = settings_save_one(key, &slots[i], sizeof(slots[i]));
        }
        if (err) {
            LOG_ERR("%s settings save failed: %d", key, err);
            K_SPINLOCK(&cfg_lock) { slots_dirty |= pending; }
            return err;
        }
        K_SPINLOCK(&cfg_lock) {
            if (empty) {
                slots_saved &= ~BIT64(i);
            } else {
                slots_saved |= BIT64(i);
            }
        }
        pending &= ~BIT64(i);
    }
    return 0;
}

int flask_tapdance_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                    void *cb_arg) {
    if (sub == NULL) {
        return 0;
    }

    if (strcmp(sub, "cfg") == 0) {
        struct flask_tapdance_saved_cfg saved;

        if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            LOG_WRN("flask/tapdance/cfg unreadable (len %d)", (int)len);
            return 0;
        }
        if (saved.version != TD_SETTINGS_VERSION) {
            LOG_WRN("flask/tapdance/cfg version %d ignored", saved.version);
            return 0;
        }
        K_SPINLOCK(&cfg_lock) {
            cfg.enabled = saved.enabled != 0;
            cfg_saved = true;
            cfg_dirty = false;
        }
        return 0;
    }

    if (sub[0] == 's') {
        int idx = atoi(&sub[1]);
        struct flask_tapdance_slot s;

        if (idx < 0 || idx >= FLASK_TAPDANCE_SLOTS) {
            LOG_WRN("flask/tapdance/%s ignored (bad index)", sub);
            return 0;
        }
        if (len != sizeof(s)) {
            LOG_WRN("flask/tapdance/%s ignored (len %d)", sub, (int)len);
            return 0;
        }
        if (read_cb(cb_arg, &s, sizeof(s)) < 0) {
            return -EIO;
        }
        for (int t = 0; t < FLASK_TAPDANCE_TAPS; t++) {
            if (s.taps[t].action > FLASK_TD_OUT_MAX) {
                s.taps[t].action = FLASK_TD_OUT_NONE;
            }
        }
        K_SPINLOCK(&cfg_lock) {
            cfg.slots[idx] = s;
            slots_saved |= BIT64(idx);
            slots_dirty &= ~BIT64(idx);
        }
        return 0;
    }
    return -ENOENT;
}

static int flask_tapdance_init(void) {
    for (int i = 0; i < TD_MAX_HELD; i++) {
        k_work_init_delayable(&dances[i].release_timer, timer_handler);
        dances[i].position = TD_POSITION_FREE;
    }
    /* Empty slots are all-zero on the wire BY DESIGN here (term 0 = the
     * default, action 0 = none) — no 0xFF sentinel needed, zero IS the
     * sentinel for this table. */
    return 0;
}

SYS_INIT(flask_tapdance_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
