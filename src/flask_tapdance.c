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
 *    immediately (fires the LAST output) unless that count has a hold —
 *    then release resolves it as the tap, term expiry as the hold.
 *  - Term expiry resolves at the deadline timestamp: key still down and a
 *    hold configured for this count = the hold output; otherwise the tap
 *    (held while the key is, released right away if the key was up).
 *  - Any OTHER key's press interrupts: undecided dances resolve first as
 *    the TAP of the current count (never the hold), so "td then a letter"
 *    keeps its order (core listener semantics).
 *  - Fast-typing bypass (prior_idle_ms > 0): a fresh dance pressed within
 *    prior_idle_ms of the last keycode press resolves at once as tap 1,
 *    so a dance on a letter key does not lag mid-word.
 *  - After-release window (release_ms > 0): releasing while more taps are
 *    possible pulls the deadline in to release + release_ms if that is
 *    earlier; the next press re-arms the full term as usual.
 *  - Eager slots (flags bit 0, Super Delete feel): every press fires its
 *    step at once — tap N for the Nth press of the chain, sticky at the
 *    last — held while the key is down. The chain stays open for the term
 *    after each press (or the release window); expiry or another key's
 *    press just closes it. Holds and the bypass do not apply.
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
#include <zmk/hid.h>
#include <zmk/keymap.h>

#include <flask_tapdance/flask_tapdance.h>
#include <flask_loop_guard.h>

#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
#include <flask_macros/flask_macros.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TD_TERM_MIN_MS 50
#define TD_TERM_MAX_MS 1000
#define TD_TERM_DEFAULT_MS 200
#define TD_PRIOR_IDLE_MAX_MS 1000
#define TD_RELEASE_MAX_MS 1000
/* An eager chain's entry outlives its deadline by this much (uptime), so a
 * press a hold-tap delivers late (decided at release, stamped with the
 * press time) still finds the chain; open/closed is decided by event
 * timestamps. ponytail: fixed, covers hold-tap terms up to ~1 s. */
#define TD_EAGER_GRACE_MS 1000

#define TD_MAX_HELD 4
#define TD_POSITION_FREE UINT32_MAX

/* --- config (spinlocked: raw-HID writes race the engine) --- */

/* Scalars live outside the table on purpose: one nonzero initializer would put the
 * whole table in .data (flash image + boot copy). The table itself is zero-init .bss. */
static bool cfg_enabled = true;
static struct {
    struct flask_tapdance_slot slots[FLASK_TAPDANCE_SLOTS];
} cfg;

static struct k_spinlock cfg_lock;

static uint64_t slots_saved;
static uint64_t slots_dirty;
/* Slots with a compiled default (flask,tapdance-defaults): emptying one
 * saves an all-zero tombstone instead of deleting the key, or the seed
 * would come back on the next boot. */
static uint64_t slots_defaulted;
static uint64_t __maybe_unused slots_seeded; /* defaults_commit has handled this slot */
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
    bool at_max; /* counter hit the length with a hold pending: release = tap */
    /* Eager chain (mode fixed when the dance starts). decided = chain
     * closed; the step output is down exactly while is_pressed. */
    bool eager;
    int64_t release_at;
    int64_t pressed_at; /* last PHYSICAL press: the bypass stamp for its outputs */
    /* Copied at resolve: the release must mirror the press even if the
     * table was rewritten mid-hold. */
    struct flask_tapdance_output fired;
    struct k_work_delayable release_timer;
};

static struct active_dance dances[TD_MAX_HELD];

/* Timestamp of the most recent keycode press (any source), for the
 * fast-typing bypass. -1 = none yet (timestamps are uptime, never < 0). */
static int64_t last_keycode_press = -1;

/* While a dance fires its own press output: that dance's last physical
 * press time, stamped instead of the output's (delayed) event time, so a
 * timer- or interrupt-resolved output does not make the NEXT dance a
 * bypass. -1 = not inside a dance output. */
static int64_t own_output_at = -1;

static void fire_output_raw(const struct flask_tapdance_output *out, uint32_t position,
                            bool pressed, int64_t timestamp) {
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
        /* ZMK's event.layer is a layer ID; highest_layer_active is an INDEX. */
        struct zmk_behavior_binding_event event = {
            .layer = zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active()),
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

/* Depth cap: &ftd output -> &ftd/&fak -> ... must end, not overflow the stack. */

static void fire_output(const struct flask_tapdance_output *out, uint32_t position, bool pressed,
                        int64_t timestamp) {
    if (!pressed) { /* releases terminate (the dance is already cleared) */
        fire_output_raw(out, position, pressed, timestamp);
        return;
    }
    if (!FLASK_GUARD_ENTER()) {
        LOG_WRN("flask_tapdance: output recursion past depth %d refused", FLASK_GUARD_MAX_DEPTH);
        return;
    }
    fire_output_raw(out, position, pressed, timestamp);
    FLASK_GUARD_LEAVE();
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

/* hold = fire the count's hold output if it has one (term expiry with the
 * key down); otherwise, and always for interrupts, the tap output. */
static void press_dance(struct active_dance *d, bool hold, int64_t timestamp);

/* Eager chain over (term expiry or interrupt): nothing to fire. A held
 * step keeps its output until the key's release (released() then clears). */
static void eager_close(struct active_dance *d) {
    if (d->is_pressed) {
        d->decided = true;
    } else {
        clear_dance(d);
    }
}

/* After-release window: pull the deadline in to release + release_ms, never
 * later than it already is. */
/* Timer at the deadline (non-eager: resolve) or the deadline + grace
 * (eager: cleanup only). reschedule, not schedule — a stale one is replaced. */
static void arm_timer(struct active_dance *d) {
    int32_t ms_left = d->release_at + (d->eager ? TD_EAGER_GRACE_MS : 0) - k_uptime_get();

    k_work_reschedule(&d->release_timer, ms_left > 0 ? K_MSEC(ms_left) : K_NO_WAIT);
}

/* Anchored on the later of the event time and now: a hold-tap delivers the
 * release stamped with its PRESS time, which would end the window early. */
static void apply_release_window(struct active_dance *d, int64_t timestamp) {
    int64_t anchor = MAX(timestamp, k_uptime_get());
    uint16_t window;

    K_SPINLOCK(&cfg_lock) { window = cfg.slots[d->slot].release_ms; }
    if (window && anchor + window < d->release_at) {
        d->release_at = anchor + window;
        arm_timer(d);
    }
}

static void press_dance(struct active_dance *d, bool hold, int64_t timestamp) {
    d->decided = true;
    K_SPINLOCK(&cfg_lock) {
        const struct flask_tapdance_slot *s = &cfg.slots[d->slot];
        uint8_t len = slot_len(s);

        /* Slot emptied mid-dance: len 0 would index taps[255]. NONE is a no-op. */
        if (len == 0) {
            memset(&d->fired, 0, sizeof(d->fired));
        } else {
            uint8_t i = MIN((uint8_t)d->counter, len) - 1;

            d->fired = (hold && s->holds[i].action != FLASK_TD_OUT_NONE) ? s->holds[i]
                                                                       : s->taps[i];
        }
    }
    own_output_at = d->pressed_at;
    fire_output(&d->fired, d->position, true, timestamp);
    own_output_at = -1;
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
    if (d->eager) {
        eager_close(d);
        return;
    }
    LOG_DBG("flask_tapdance: slot %d decided by timer at %d taps", d->slot, d->counter);
    press_dance(d, d->is_pressed, d->release_at);
    if (!d->is_pressed) {
        release_dance(d, d->release_at);
    }
}

/* --- behavior entry points (&ftd) --- */

int flask_tapdance_pressed(uint8_t slot, uint32_t position, int64_t timestamp) {
    uint8_t len = 0;
    uint16_t term = TD_TERM_DEFAULT_MS;
    uint16_t prior_idle;
    bool enabled;
    bool last_has_hold;
    bool eager;

    if (slot >= FLASK_TAPDANCE_SLOTS) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) {
        enabled = cfg_enabled;
        len = slot_len(&cfg.slots[slot]);
        if (cfg.slots[slot].term_ms) {
            term = cfg.slots[slot].term_ms;
        }
        prior_idle = cfg.slots[slot].prior_idle_ms;
        eager = (cfg.slots[slot].flags & FLASK_TD_FLAG_EAGER) != 0;
        last_has_hold = len > 0 && cfg.slots[slot].holds[len - 1].action != FLASK_TD_OUT_NONE;
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
                d->at_max = false;
                d->eager = eager;
                d->is_pressed = false;
                d->release_at = 0;
                break;
            }
        }
        if (d == NULL) {
            LOG_ERR("flask_tapdance: no free active slot");
            return -ENOMEM;
        }
    }

    if (d->eager) {
        if (d->is_pressed) {
            /* One key cannot re-press while down; never leave a step stuck. */
            fire_output(&d->fired, d->position, false, timestamp);
        }
        if (d->decided || timestamp >= d->release_at) {
            /* Chain closed while held, or this press (by its own event
             * time) came after the deadline: start over. */
            d->counter = 0;
        }
        d->is_pressed = true;
        d->pressed_at = timestamp;
        stop_timer(d);
        if (d->counter < len) {
            d->counter++;
        }
        press_dance(d, false, timestamp);
        d->decided = false; /* the chain stays open */
        d->release_at = timestamp + term;
        arm_timer(d);
        return 0;
    }

    d->is_pressed = true;
    d->pressed_at = timestamp;
    stop_timer(d);
    if (d->counter == 0 && prior_idle > 0 && last_keycode_press >= 0 &&
        timestamp - last_keycode_press < prior_idle) {
        /* Fast typing: tap 1 now, held until release. */
        d->counter = 1;
        press_dance(d, false, timestamp);
        return 0;
    }
    if (d->counter < len) {
        d->counter++;
    }
    if (d->counter == len) {
        if (!last_has_hold) {
            /* Max taps — decided immediately (core semantics). */
            press_dance(d, false, timestamp);
            return 0;
        }
        d->at_max = true; /* release = tap, term expiry while down = hold */
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
    if (d->eager && !d->decided) {
        /* Step released; the chain stays open for the next press. */
        fire_output(&d->fired, d->position, false, timestamp);
        apply_release_window(d, timestamp);
        return 0;
    }
    if (!d->decided && d->at_max) {
        /* Last count with a hold, released before the term: a tap. */
        stop_timer(d);
        press_dance(d, false, timestamp);
    } else if (!d->decided) {
        apply_release_window(d, timestamp);
    }
    if (d->decided) {
        release_dance(d, timestamp);
    }
    return 0;
}

void flask_tapdance_interrupt(uint32_t position, int64_t timestamp) {
    for (int i = 0; i < TD_MAX_HELD; i++) {
        struct active_dance *d = &dances[i];

        if (d->position == TD_POSITION_FREE || d->position == position) {
            continue;
        }
        stop_timer(d);
        if (d->eager) {
            if (!d->decided) {
                eager_close(d); /* chain over, nothing fires */
            }
            continue;
        }
        if (!d->decided) {
            press_dance(d, false, timestamp);
            if (!d->is_pressed) {
                release_dance(d, timestamp);
            }
        }
    }
}

/* Interrupts: any OTHER key's press resolves undecided dances first (as
 * the tap), so "dance key then letter" keeps its order (core listener).
 * Keycode presses only stamp the fast-typing clock. */
static int td_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *kc = as_zmk_keycode_state_changed(eh);

    if (kc != NULL) {
        /* Modifiers never count (as flask_adaptive): a held Ctrl or a
         * hold-tap's mod must not turn the next dance into a bypass. */
        if (kc->state && !is_mod(kc->usage_page, kc->keycode)) {
            last_keycode_press =
                MAX(last_keycode_press, own_output_at >= 0 ? own_output_at : kc->timestamp);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev != NULL && ev->state) {
        flask_tapdance_interrupt(ev->position, ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_tapdance, td_listener);
ZMK_SUBSCRIPTION(flask_tapdance, zmk_position_state_changed);
ZMK_SUBSCRIPTION(flask_tapdance, zmk_keycode_state_changed);

/* --- runtime API (proto channel 0x28) --- */

bool flask_tapdance_enabled(void) {
    bool on;

    K_SPINLOCK(&cfg_lock) { on = cfg_enabled; }
    return on;
}

void flask_tapdance_set_enabled(bool on) {
    K_SPINLOCK(&cfg_lock) {
        cfg_enabled = on;
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

int flask_tapdance_prior_idle_set(uint8_t idx, uint16_t prior_idle_ms) {
    if (idx >= FLASK_TAPDANCE_SLOTS) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) {
        cfg.slots[idx].prior_idle_ms = MIN(prior_idle_ms, TD_PRIOR_IDLE_MAX_MS);
        slots_dirty |= BIT64(idx);
    }
    return 0;
}

int flask_tapdance_release_set(uint8_t idx, uint16_t release_ms) {
    if (idx >= FLASK_TAPDANCE_SLOTS) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) {
        cfg.slots[idx].release_ms = MIN(release_ms, TD_RELEASE_MAX_MS);
        slots_dirty |= BIT64(idx);
    }
    return 0;
}

int flask_tapdance_flags_set(uint8_t idx, uint8_t flags) {
    if (idx >= FLASK_TAPDANCE_SLOTS) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) {
        cfg.slots[idx].flags = flags & FLASK_TD_FLAGS_KNOWN;
        slots_dirty |= BIT64(idx);
    }
    return 0;
}

static void normalize_output(struct flask_tapdance_output *out) {
    if (out->action > FLASK_TD_OUT_MAX) {
        out->action = FLASK_TD_OUT_NONE;
    }
    if (out->action == FLASK_TD_OUT_USAGE && out->param1 == 0) {
        out->action = FLASK_TD_OUT_NONE;
    }
    if (out->action == FLASK_TD_OUT_BEHAVIOR && flask_behavior_id_is_tapdance(out->behavior_id)) {
        out->action = FLASK_TD_OUT_NONE; /* a dance output would re-enter at this position */
    }
    if (out->action == FLASK_TD_OUT_NONE) {
        out->behavior_id = 0;
        out->param1 = 0;
        out->param2 = 0;
    }
    if (out->action != FLASK_TD_OUT_BEHAVIOR) {
        out->behavior_id = 0;
        if (out->action != FLASK_TD_OUT_NONE) {
            out->param2 = 0;
        }
    }
}

int flask_tapdance_output_set(uint8_t idx, uint8_t tap, bool hold,
                              const struct flask_tapdance_output *in) {
    if (idx >= FLASK_TAPDANCE_SLOTS || tap >= FLASK_TAPDANCE_TAPS || in == NULL) {
        return -EINVAL;
    }

    struct flask_tapdance_output out = *in;

    normalize_output(&out);
    K_SPINLOCK(&cfg_lock) {
        if (hold) {
            cfg.slots[idx].holds[tap] = out;
        } else {
            cfg.slots[idx].taps[tap] = out;
        }
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

/* Pre-v22 slot blob: term + taps only. Restored with no holds, bypass off,
 * and marked dirty so the next SAVE rewrites it in the current layout. */
struct flask_tapdance_slot_v1 {
    uint16_t term_ms;
    struct flask_tapdance_output taps[FLASK_TAPDANCE_TAPS];
} __packed;

/* v22 slot blob: no release_ms (window off). Same upgrade path. */
struct flask_tapdance_slot_v2 {
    uint16_t term_ms;
    uint16_t prior_idle_ms;
    struct flask_tapdance_output taps[FLASK_TAPDANCE_TAPS];
    struct flask_tapdance_output holds[FLASK_TAPDANCE_TAPS];
} __packed;

/* Sizes 2+11N, 4+22N, 7+22N never coincide (N >= 1), so len picks the layout. */
BUILD_ASSERT(sizeof(struct flask_tapdance_slot_v1) != sizeof(struct flask_tapdance_slot) &&
                 sizeof(struct flask_tapdance_slot_v2) != sizeof(struct flask_tapdance_slot) &&
                 sizeof(struct flask_tapdance_slot_v1) != sizeof(struct flask_tapdance_slot_v2),
             "tap dance slot blob sizes must differ");

static bool slot_is_empty(const struct flask_tapdance_slot *s) {
    if (s->term_ms != 0 || s->prior_idle_ms != 0 || s->release_ms != 0 || s->flags != 0) {
        return false;
    }
    for (int t = 0; t < FLASK_TAPDANCE_TAPS; t++) {
        if (s->taps[t].action != FLASK_TD_OUT_NONE || s->holds[t].action != FLASK_TD_OUT_NONE) {
            return false;
        }
    }
    return true;
}

int flask_tapdance_save(void) {
    struct flask_tapdance_saved_cfg saved = {.version = TD_SETTINGS_VERSION};
    static struct flask_tapdance_slot slots[FLASK_TAPDANCE_SLOTS];
    uint64_t pending, saved_bits, defaulted;
    bool write_cfg;

    K_SPINLOCK(&cfg_lock) {
        saved.enabled = cfg_enabled ? 1 : 0;
        memcpy(slots, cfg.slots, sizeof(slots));
        pending = slots_dirty;
        saved_bits = slots_saved;
        defaulted = slots_defaulted;
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

        /* A defaulted slot always keeps a key (all-zero = tombstone). */
        empty = empty && !(defaulted & BIT64(i));
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
            cfg_enabled = saved.enabled != 0;
            cfg_saved = true;
            cfg_dirty = false;
        }
        return 0;
    }

    if (sub[0] == 's') {
        int idx = atoi(&sub[1]);
        struct flask_tapdance_slot s = {0};
        bool migrated = false;

        if (idx < 0 || idx >= FLASK_TAPDANCE_SLOTS) {
            LOG_WRN("flask/tapdance/%s ignored (bad index)", sub);
            return 0;
        }
        if (len == sizeof(struct flask_tapdance_slot_v1)) {
            struct flask_tapdance_slot_v1 old;

            if (read_cb(cb_arg, &old, sizeof(old)) < 0) {
                return -EIO;
            }
            s.term_ms = old.term_ms;
            memcpy(s.taps, old.taps, sizeof(s.taps));
            migrated = true;
        } else if (len == sizeof(struct flask_tapdance_slot_v2)) {
            struct flask_tapdance_slot_v2 old;

            if (read_cb(cb_arg, &old, sizeof(old)) < 0) {
                return -EIO;
            }
            s.term_ms = old.term_ms;
            s.prior_idle_ms = old.prior_idle_ms;
            memcpy(s.taps, old.taps, sizeof(s.taps));
            memcpy(s.holds, old.holds, sizeof(s.holds));
            migrated = true;
        } else if (len == sizeof(s)) {
            if (read_cb(cb_arg, &s, sizeof(s)) < 0) {
                return -EIO;
            }
        } else {
            LOG_WRN("flask/tapdance/%s ignored (len %d)", sub, (int)len);
            return 0;
        }
        for (int t = 0; t < FLASK_TAPDANCE_TAPS; t++) {
            if (s.taps[t].action > FLASK_TD_OUT_MAX) {
                s.taps[t].action = FLASK_TD_OUT_NONE;
            }
            if (s.holds[t].action > FLASK_TD_OUT_MAX) {
                s.holds[t].action = FLASK_TD_OUT_NONE;
            }
        }
        if (s.prior_idle_ms > TD_PRIOR_IDLE_MAX_MS) {
            s.prior_idle_ms = TD_PRIOR_IDLE_MAX_MS;
        }
        if (s.release_ms > TD_RELEASE_MAX_MS) {
            s.release_ms = TD_RELEASE_MAX_MS;
        }
        s.flags &= FLASK_TD_FLAGS_KNOWN;
        K_SPINLOCK(&cfg_lock) {
            cfg.slots[idx] = s;
            slots_saved |= BIT64(idx);
            if (migrated) {
                slots_dirty |= BIT64(idx);
            } else {
                slots_dirty &= ~BIT64(idx);
            }
        }
        return 0;
    }
    return -ENOENT;
}

/* --- compiled defaults (flask,tapdance-defaults) -> live table --- */

struct td_default {
    uint8_t slot;
    uint16_t term_ms;
    uint16_t release_ms;
    uint8_t flags;
    uint8_t n_bind;
    struct zmk_behavior_binding b[FLASK_TAPDANCE_TAPS];
};

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_key_press)
#define TD_KP_NAME_INIT DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_key_press))
#else
#define TD_KP_NAME_INIT NULL
#endif

/* 0 = ok, 1 = the behavior has no local id yet (zmk assigns them in its own
 * settings commit, which can run after ours on a fresh device). */
static int __maybe_unused binding_to_output(const struct zmk_behavior_binding *b,
                                            struct flask_tapdance_output *o) {
    static const char *const kp_name = TD_KP_NAME_INIT;
    zmk_behavior_local_id_t id;

    memset(o, 0, sizeof(*o));
    if (kp_name != NULL && strcmp(b->behavior_dev, kp_name) == 0) {
        o->action = FLASK_TD_OUT_USAGE;
        o->param1 = b->param1;
        return 0;
    }
    id = zmk_behavior_get_local_id(b->behavior_dev);
    if (id == 0 || id == UINT16_MAX) {
        return 1;
    }
    o->action = FLASK_TD_OUT_BEHAVIOR;
    o->behavior_id = id;
    o->param1 = b->param1;
    o->param2 = b->param2;
    normalize_output(o);
    return 0;
}

/* Seed each default into its slot only if settings never held that slot
 * (no flask/tapdance/s<idx>, tombstones included) and it is still empty, so
 * app edits win forever after. Not dirty: the seed is not written to flash
 * until the app changes the slot. Returns bindings still unresolved
 * (flask_proto retries). Idempotent. */
static int __maybe_unused seed_defaults(const struct td_default *defs, int n) {
    int unresolved = 0;

    for (int i = 0; i < n; i++) {
        const struct td_default *d = &defs[i];
        struct flask_tapdance_slot s = {0};
        int bad = 0;

        if (d->slot >= FLASK_TAPDANCE_SLOTS) {
            continue;
        }
        K_SPINLOCK(&cfg_lock) { slots_defaulted |= BIT64(d->slot); }
        for (int t = 0; t < d->n_bind && t < FLASK_TAPDANCE_TAPS; t++) {
            bad += binding_to_output(&d->b[t], &s.taps[t]);
        }
        if (bad) {
            unresolved += bad;
            continue;
        }
        s.term_ms = d->term_ms ? CLAMP(d->term_ms, TD_TERM_MIN_MS, TD_TERM_MAX_MS) : 0;
        s.release_ms = MIN(d->release_ms, TD_RELEASE_MAX_MS);
        s.flags = d->flags & FLASK_TD_FLAGS_KNOWN;
        K_SPINLOCK(&cfg_lock) {
            if (((slots_saved | slots_seeded) & BIT64(d->slot)) == 0 &&
                slot_is_empty(&cfg.slots[d->slot])) {
                cfg.slots[d->slot] = s;
            }
            slots_seeded |= BIT64(d->slot);
        }
    }
    return unresolved;
}

#if DT_HAS_COMPAT_STATUS_OKAY(flask_tapdance_defaults)

#define FTDD_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(flask_tapdance_defaults)
#define FTDD_BIND(i, child) ZMK_KEYMAP_EXTRACT_BINDING(i, child)
#define FTDD_CHILD(child)                                                                          \
    {                                                                                              \
        .slot = DT_PROP(child, slot),                                                              \
        .term_ms = DT_PROP_OR(child, term_ms, 0),                                                  \
        .release_ms = DT_PROP_OR(child, release_ms, 0),                                            \
        .flags = DT_PROP(child, eager) ? FLASK_TD_FLAG_EAGER : 0,                                  \
        .n_bind = DT_PROP_LEN(child, bindings),                                                    \
        .b = {LISTIFY(DT_PROP_LEN(child, bindings), FTDD_BIND, (, ), child)},                      \
    },
#define FTDD_CHECK(child)                                                                          \
    BUILD_ASSERT(DT_PROP(child, slot) < FLASK_TAPDANCE_SLOTS,                                      \
                 "tap dance default: slot >= ZMK_FLASK_TAPDANCE_SLOTS");                           \
    BUILD_ASSERT(DT_PROP_LEN(child, bindings) <= FLASK_TAPDANCE_TAPS,                              \
                 "tap dance default: more bindings than ZMK_FLASK_TAPDANCE_TAPS");

DT_FOREACH_CHILD(FTDD_NODE, FTDD_CHECK)

static const struct td_default td_defaults[] = {DT_FOREACH_CHILD(FTDD_NODE, FTDD_CHILD)};

int flask_tapdance_defaults_commit(void) {
    return seed_defaults(td_defaults, ARRAY_SIZE(td_defaults));
}

#else /* no defaults node in the keymap */

int flask_tapdance_defaults_commit(void) { return 0; }

#endif

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
