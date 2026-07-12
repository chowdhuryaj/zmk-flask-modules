/*
 * flask_combos — runtime-editable combos (see include/flask_combos/flask_combos.h).
 *
 * The capture engine mirrors zmk app/src/combo.c (pinned rev 484a0547) with
 * the parts a runtime table doesn't need removed (per-combo timeouts, layer
 * masks, require-prior-idle, slow-release). The load-bearing semantics are
 * copied exactly:
 *
 *  - A key-down that could start/extend a combo is CAPTURED: the event is
 *    copied by value (copy_raised_zmk_position_state_changed) into a static
 *    array and the original stops propagating. No heap, no refcount — the
 *    copy carries the event header, including last_listener_index.
 *  - On non-match, the FIRST captured event is ZMK_EVENT_RELEASE'd (resumes
 *    the listener chain after this module — core combos and the keymap still
 *    see it) and the rest are ZMK_EVENT_RAISE'd from the top (they re-enter
 *    this listener as fresh first presses; pressed_count is zeroed before
 *    the loop so re-capture nests correctly — same re-entrancy dance as
 *    core combo.c's release_pressed_keys).
 *  - A key-up while captured downs were just re-raised is duplicated and
 *    re-raised too, so downstream processors see downs and ups in order
 *    (core combo.c position_state_up, verbatim).
 *  - A matched slot swallows its key events for the whole hold and raises
 *    the encoded output usage: press with the first key's timestamp,
 *    release when the FIRST combo key comes up (QMK/Vial release feel).
 *
 * Threading: position events and the timeout work item run on the same
 * context core combo.c assumes single-threaded (it has no locks) — the
 * capture state is left lock-free to match. The config table can be written
 * from the raw-HID path concurrently, so it is guarded by a spinlock.
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
#include <zmk/matrix.h>

#include <flask_combos/flask_combos.h>

#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
#include <flask_macros/flask_macros.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TIMEOUT_MIN_MS 10
#define TIMEOUT_MAX_MS 2000
#define TIMEOUT_DEFAULT_MS 50

/* How many runtime combos can be held down at once. */
#define MAX_ACTIVE 8

BUILD_ASSERT(FLASK_COMBOS_SLOTS <= 64, "candidate set is a single u64 bitmask");

static inline int popcount64(uint64_t v) { return __builtin_popcountll(v); }

/* --- config (spinlocked: raw-HID writes race the matcher) --- */

static struct {
    bool enabled;
    uint16_t timeout_ms;
    struct flask_combo_slot slots[FLASK_COMBOS_SLOTS];
} cfg = {
    .enabled = true,
    .timeout_ms = TIMEOUT_DEFAULT_MS,
};

/* Derived: per-position bitmask of live slots + per-slot key counts.
 * Rebuilt on every table edit; only LIVE slots (usage set, >= 2 keys) are
 * entered, so the matcher can trust any lookup hit. */
static uint64_t lookup[ZMK_KEYMAP_LEN];
static uint8_t slot_len[FLASK_COMBOS_SLOTS];

static struct k_spinlock cfg_lock;

/* Save bookkeeping (under cfg_lock): which slots exist in settings, which
 * changed since their last successful save. flask_combos_save() touches
 * flash only for dirty slots — the old save wrote or blind-deleted all 64
 * keys every time (~65 NVS ops), which is what the bench-4b combo save
 * died on. */
static uint64_t slots_saved;
static uint64_t slots_dirty;
static bool cfg_saved;
static bool cfg_dirty;

static uint8_t slot_key_count(const struct flask_combo_slot *s) {
    uint8_t n = 0;

    for (int k = 0; k < FLASK_COMBOS_KEYS; k++) {
        if (s->pos[k] != FLASK_COMBOS_POS_NONE) {
            n++;
        }
    }
    return n;
}

/* Callers hold cfg_lock. */
static void rebuild_lookup(void) {
    memset(lookup, 0, sizeof(lookup));
    memset(slot_len, 0, sizeof(slot_len));

    for (int i = 0; i < FLASK_COMBOS_SLOTS; i++) {
        const struct flask_combo_slot *s = &cfg.slots[i];
        uint8_t len = slot_key_count(s);

        if (s->action == FLASK_COMBO_OUT_NONE || len < 2) {
            continue;
        }
        slot_len[i] = len;
        for (int k = 0; k < FLASK_COMBOS_KEYS; k++) {
            if (s->pos[k] != FLASK_COMBOS_POS_NONE) {
                lookup[s->pos[k]] |= BIT64(i);
            }
        }
    }
}

/* --- capture state (single-context, lock-free like core combo.c) --- */

static struct zmk_position_state_changed_event pressed[FLASK_COMBOS_KEYS];
static uint8_t pressed_count;
static uint64_t candidates;
static int fully_pressed = -1;

struct active_rt {
    uint8_t slot; /* 0xFF = free */
    uint8_t remaining[FLASK_COMBOS_KEYS];
    uint8_t remaining_count;
    bool output_down;
    /* Fire data copied out of the slot at activation — the config table
     * can be rewritten mid-hold and the release must mirror the press. */
    uint8_t action;
    uint16_t behavior_id;
    uint32_t param1;
    uint32_t param2;
    uint32_t position; /* first combo position = the behavior's event position */
};

/* Press/release the typed output. BEHAVIOR outputs go through the same
 * invoke path the keymap uses, with the first combo key's position — so
 * hold-taps, layer-taps and layer keys behave like a real key at that
 * position (AJ's bench-5 ask: combo outputs beyond plain keycodes). */
static void fire_output(const struct active_rt *a, bool pressed, int64_t timestamp) {
    switch (a->action) {
    case FLASK_COMBO_OUT_USAGE:
        if (a->param1 != 0) {
            raise_zmk_keycode_state_changed_from_encoded(a->param1, pressed, timestamp);
        }
        break;
    case FLASK_COMBO_OUT_MACRO:
#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
        if (pressed) {
            flask_macros_play((uint8_t)a->param1);
        }
#endif
        break;
    case FLASK_COMBO_OUT_BEHAVIOR: {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
        /* Local-id lookup rides Studio's behavior registry — absent on the
         * diagnostic (no-Studio) central image, where behavior outputs are
         * stored but inert. */
        const char *name = zmk_behavior_find_behavior_name_from_local_id(a->behavior_id);

        if (name == NULL) {
            LOG_WRN("flask_combos: behavior id %u not found", a->behavior_id);
            return;
        }
        struct zmk_behavior_binding binding = {
            .behavior_dev = name,
            .param1 = a->param1,
            .param2 = a->param2,
        };
        struct zmk_behavior_binding_event event = {
            .layer = zmk_keymap_highest_layer_active(),
            .position = a->position,
            .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
        };
        zmk_behavior_invoke_binding(&binding, event, pressed);
#else
        LOG_WRN("flask_combos: behavior outputs need CONFIG_ZMK_BEHAVIOR_LOCAL_IDS");
#endif
        break;
    }
    default:
        break;
    }
}

static struct active_rt actives[MAX_ACTIVE];

static struct k_work_delayable timeout_task;
static int64_t timeout_at;

static int setup_candidates(uint32_t position, uint64_t *out) {
    uint64_t set = 0;

    K_SPINLOCK(&cfg_lock) {
        if (cfg.enabled && position < ZMK_KEYMAP_LEN) {
            set = lookup[position];
        }
    }
    *out = set;
    return popcount64(set);
}

static int filter_candidates(uint32_t position) {
    uint64_t mask = 0;

    K_SPINLOCK(&cfg_lock) {
        /* Gate on enabled here too — a capture opened while enabled must
         * die the moment the master switch goes off (bench 5: "combos
         * kept firing when off" — every window counts). */
        if (cfg.enabled && position < ZMK_KEYMAP_LEN) {
            mask = lookup[position];
        }
    }
    candidates &= mask;
    return popcount64(candidates);
}

static int find_fully_pressed(void) {
    int found = -1;

    K_SPINLOCK(&cfg_lock) {
        if (!cfg.enabled) {
            K_SPINLOCK_BREAK;
        }
        for (int i = 0; i < FLASK_COMBOS_SLOTS; i++) {
            if ((candidates & BIT64(i)) && slot_len[i] == pressed_count) {
                found = i;
                break;
            }
        }
    }
    return found;
}

const struct zmk_listener zmk_listener_flask_combos;

static int release_pressed_keys(void) {
    uint8_t count = pressed_count;

    pressed_count = 0;
    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed[i];

        if (i == 0) {
            LOG_DBG("flask_combos: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            /* Re-raised events re-enter this listener as fresh first
             * presses — pressed[] is already reset, so they may nest a new
             * capture (core combo.c fully-overlapping semantics). */
            LOG_DBG("flask_combos: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static void activate_combo(int slot_idx) {
    struct active_rt *active = NULL;
    struct active_rt fire = { .slot = 0xFF };
    uint8_t len = 0;
    bool enabled = false;

    for (int i = 0; i < MAX_ACTIVE; i++) {
        if (actives[i].slot == 0xFF) {
            active = &actives[i];
            break;
        }
    }

    K_SPINLOCK(&cfg_lock) {
        const struct flask_combo_slot *s = &cfg.slots[slot_idx];

        enabled = cfg.enabled;
        fire.action = s->action;
        fire.behavior_id = s->behavior_id;
        fire.param1 = s->param1;
        fire.param2 = s->param2;
        len = slot_len[slot_idx];
    }

    /* Config changed mid-capture, master switch flipped off, or no active
     * slot free: give the keys back to the pipeline instead of eating them. */
    if (active == NULL || !enabled || fire.action == FLASK_COMBO_OUT_NONE || len == 0) {
        LOG_WRN("flask_combos: cannot activate slot %d, releasing keys", slot_idx);
        release_pressed_keys();
        return;
    }

    uint8_t take = MIN(pressed_count, len);
    int64_t timestamp = pressed[0].data.timestamp;

    *active = fire;
    active->slot = slot_idx;
    active->remaining_count = take;
    active->output_down = true;
    active->position = pressed[0].data.position;
    for (int i = 0; i < take; i++) {
        active->remaining[i] = (uint8_t)pressed[i].data.position;
    }

    /* Shift any captured keys beyond the combo up front (mirrors core
     * move_pressed_keys_to_active_combo). */
    for (int i = 0; i + take < pressed_count; i++) {
        pressed[i] = pressed[i + take];
    }
    pressed_count -= take;

    LOG_DBG("flask_combos: slot %d fires action %d", slot_idx, active->action);
    fire_output(active, true, timestamp);
}

/* Resolve the current capture: activate the fully-pressed slot if there is
 * one, then release whatever is left. Returns how many captured events were
 * released back into the pipeline. */
static int cleanup(void) {
    k_work_cancel_delayable(&timeout_task);
    timeout_at = 0;
    candidates = 0;
    if (fully_pressed >= 0) {
        activate_combo(fully_pressed);
        fully_pressed = -1;
    }
    return release_pressed_keys();
}

/* Returns true when the key belonged to an active runtime combo (the up
 * event is swallowed). Output releases on the FIRST key up. */
static bool release_combo_key(uint32_t position, int64_t timestamp) {
    for (int i = 0; i < MAX_ACTIVE; i++) {
        struct active_rt *active = &actives[i];

        if (active->slot == 0xFF) {
            continue;
        }
        for (int k = 0; k < active->remaining_count; k++) {
            if (active->remaining[k] != position) {
                continue;
            }
            active->remaining[k] = active->remaining[--active->remaining_count];
            if (active->output_down) {
                /* Release mirrors the press from the activation-time copy —
                 * the slot table may have been rewritten mid-hold. */
                active->output_down = false;
                fire_output(active, false, timestamp);
            }
            if (active->remaining_count == 0) {
                active->slot = 0xFF;
            }
            return true;
        }
    }
    return false;
}

static void update_timeout(void) {
    if (pressed_count == 0) {
        timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }

    uint16_t window;

    K_SPINLOCK(&cfg_lock) { window = cfg.timeout_ms; }

    int64_t deadline = pressed[0].data.timestamp + window;
    int64_t delay = deadline - k_uptime_get();

    timeout_at = deadline;
    /* k_work_schedule keeps an existing deadline — reschedule replaces it
     * (hard-won: see the autoscroll stale-deadline bug). */
    k_work_reschedule(&timeout_task, delay > 0 ? K_MSEC(delay) : K_NO_WAIT);
}

static void timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (timeout_at == 0 || k_uptime_get() < timeout_at) {
        return; /* cancelled or rescheduled */
    }
    timeout_at = 0;
    cleanup();
}

static int capture_key(const struct zmk_position_state_changed *ev) {
    if (pressed_count == FLASK_COMBOS_KEYS) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    pressed[pressed_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}

static int position_down(struct zmk_position_state_changed *data) {
    int num_candidates;

    if (pressed_count == 0) {
        num_candidates = setup_candidates(data->position, &candidates);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        if (data->timestamp - pressed[0].data.timestamp >=
            (int64_t)flask_combos_timeout_ms()) {
            /* Window closed before the work item fired: no candidate
             * survives (global timeout — they all share the deadline).
             * The whole group resolves below via cleanup(), and this key
             * gets re-raised as a fresh first press — do NOT cleanup()
             * here mid-down; its re-raise recursion would clobber the
             * candidates we are about to compute (core combo.c's
             * filter_timed_out_candidates semantics). */
            candidates = 0;
        }
        num_candidates = filter_candidates(data->position);
    }

    int ret = capture_key(data);

    update_timeout();

    if (num_candidates == 0) {
        cleanup();
        return ret;
    }

    int complete = find_fully_pressed();

    if (complete >= 0) {
        fully_pressed = complete;
        if (num_candidates == 1) {
            cleanup();
        }
    }
    return ret;
}

static int position_up(struct zmk_position_state_changed *data) {
    int released_keys = cleanup();

    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        /* Downs 2..n were just re-raised through the full chain; re-raise
         * the up too so downstream sees them in order (core combo.c). */
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int flask_combos_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(eh);

    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    return data->state ? position_down(data) : position_up(data);
}

ZMK_LISTENER(flask_combos, flask_combos_listener);
ZMK_SUBSCRIPTION(flask_combos, zmk_position_state_changed);

/* --- runtime API (proto channel 0x24) --- */

bool flask_combos_enabled(void) {
    bool on;

    K_SPINLOCK(&cfg_lock) { on = cfg.enabled; }
    return on;
}

void flask_combos_set_enabled(bool on) {
    K_SPINLOCK(&cfg_lock) {
        cfg.enabled = on;
        cfg_dirty = true;
    }
}

uint16_t flask_combos_timeout_ms(void) {
    uint16_t ms;

    K_SPINLOCK(&cfg_lock) { ms = cfg.timeout_ms; }
    return ms;
}

void flask_combos_set_timeout_ms(uint16_t ms) {
    ms = CLAMP(ms, TIMEOUT_MIN_MS, TIMEOUT_MAX_MS);
    K_SPINLOCK(&cfg_lock) {
        cfg.timeout_ms = ms;
        cfg_dirty = true;
    }
}

uint8_t flask_combos_slot_count(void) { return FLASK_COMBOS_SLOTS; }

int flask_combos_slot_get(uint8_t idx, struct flask_combo_slot *out) {
    if (idx >= FLASK_COMBOS_SLOTS || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = cfg.slots[idx]; }
    return 0;
}

int flask_combos_slot_set(uint8_t idx, const struct flask_combo_slot *in) {
    if (idx >= FLASK_COMBOS_SLOTS || in == NULL) {
        return -EINVAL;
    }

    struct flask_combo_slot s = *in;

    for (int k = 0; k < FLASK_COMBOS_KEYS; k++) {
        if (s.pos[k] >= ZMK_KEYMAP_LEN) {
            s.pos[k] = FLASK_COMBOS_POS_NONE;
        }
    }
    if (s.action > FLASK_COMBO_OUT_MAX) {
        s.action = FLASK_COMBO_OUT_NONE;
    }
    if (s.action == FLASK_COMBO_OUT_USAGE && s.param1 == 0) {
        s.action = FLASK_COMBO_OUT_NONE;
    }
    if (s.action == FLASK_COMBO_OUT_NONE) {
        s.behavior_id = 0;
        s.param1 = 0;
        s.param2 = 0;
    }
    if (s.action != FLASK_COMBO_OUT_BEHAVIOR) {
        s.behavior_id = 0;
        if (s.action != FLASK_COMBO_OUT_NONE) {
            s.param2 = 0;
        }
    }

    K_SPINLOCK(&cfg_lock) {
        cfg.slots[idx] = s;
        slots_dirty |= BIT64(idx);
        rebuild_lookup();
    }
    return 0;
}

/* --- persistence (settings subtree "flask/combos") ---
 *
 * v2 layout (2026-07-09, capacities round): one entry per USED slot —
 * "flask/combos/s<idx>" holds the raw slot struct, "flask/combos/cfg" the
 * globals. Empty slots are deleted. NVS caps a single entry near its
 * sector size, so the old whole-table blob stopped scaling the moment the
 * table grew past ~4 KB; per-slot entries scale to whatever the partition
 * holds. The v1 blob (exact "flask/combos" leaf) is ignored on load — it
 * never reached hardware. */

struct flask_combos_saved_cfg {
    uint8_t version;
    uint8_t enabled;
    uint16_t timeout_ms;
} __packed;

/* v3 (2026-07-12, typed outputs): slot entries carry the whole typed
 * struct. v2 slot entries (pos[] + u32 usage) migrate on restore — a
 * usage becomes a USAGE-action output, so bench-saved combos survive the
 * upgrade. */
#define COMBOS_SETTINGS_VERSION 3

struct flask_combo_slot_v2 {
    uint8_t pos[FLASK_COMBOS_KEYS];
    uint32_t usage;
} __packed;

static bool slot_is_empty(const struct flask_combo_slot *s) {
    if (s->action != FLASK_COMBO_OUT_NONE) {
        return false;
    }
    for (int k = 0; k < FLASK_COMBOS_KEYS; k++) {
        if (s->pos[k] != FLASK_COMBOS_POS_NONE) {
            return false;
        }
    }
    return true;
}

int flask_combos_save(void) {
    struct flask_combos_saved_cfg saved = {.version = COMBOS_SETTINGS_VERSION};
    /* Static keeps the table copy off the save-queue stack; safe because
     * flask_proto's save queue runs at most one save at a time. */
    static struct flask_combo_slot slots[FLASK_COMBOS_SLOTS];
    uint64_t pending, saved_bits;
    bool write_cfg;

    K_SPINLOCK(&cfg_lock) {
        saved.enabled = cfg.enabled ? 1 : 0;
        saved.timeout_ms = cfg.timeout_ms;
        memcpy(slots, cfg.slots, sizeof(slots));
        pending = slots_dirty;
        saved_bits = slots_saved;
        write_cfg = cfg_dirty || !cfg_saved;
        /* Claimed: re-marked on failure below; an edit landing mid-save
         * re-sets its bit and rides the next save. */
        slots_dirty = 0;
        cfg_dirty = false;
    }

    if (write_cfg) {
        int err = settings_save_one("flask/combos/cfg", &saved, sizeof(saved));

        if (err) {
            LOG_ERR("flask/combos/cfg settings save failed: %d", err);
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
        char key[24];
        int err = 0;

        snprintf(key, sizeof(key), "flask/combos/s%d", i);
        if (empty && on_flash) {
            err = settings_delete(key);
        } else if (!empty) {
            err = settings_save_one(key, &slots[i], sizeof(slots[i]));
        } /* empty + never saved: nothing stored, nothing to delete */
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

/* Restore one entry of the "flask/combos" subtree; sub is the name past
 * the subtree ("cfg", "s<idx>", or NULL for the retired v1 blob leaf). */
int flask_combos_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    if (sub == NULL) {
        LOG_WRN("flask/combos v1 blob ignored (per-slot layout since v2)");
        return 0;
    }

    if (strcmp(sub, "cfg") == 0) {
        struct flask_combos_saved_cfg saved;

        if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            LOG_WRN("flask/combos/cfg unreadable (len %d)", (int)len);
            return 0;
        }
        /* v2 cfg is shape-identical — accept both versions. */
        if (saved.version != COMBOS_SETTINGS_VERSION && saved.version != 2) {
            LOG_WRN("flask/combos/cfg version %d ignored", saved.version);
            return 0;
        }
        K_SPINLOCK(&cfg_lock) {
            cfg.enabled = saved.enabled != 0;
            cfg.timeout_ms = CLAMP(saved.timeout_ms, TIMEOUT_MIN_MS, TIMEOUT_MAX_MS);
            cfg_saved = true;
            cfg_dirty = false;
        }
        return 0;
    }

    if (sub[0] == 's') {
        int idx = atoi(&sub[1]);
        struct flask_combo_slot s;

        if (idx < 0 || idx >= FLASK_COMBOS_SLOTS) {
            LOG_WRN("flask/combos/%s ignored (bad index)", sub);
            return 0;
        }
        if (len == sizeof(s)) {
            if (read_cb(cb_arg, &s, sizeof(s)) < 0) {
                return -EIO;
            }
        } else if (len == sizeof(struct flask_combo_slot_v2)) {
            /* v2 slot (pos + usage) — migrate to a USAGE-action output. */
            struct flask_combo_slot_v2 old;

            if (read_cb(cb_arg, &old, sizeof(old)) < 0) {
                return -EIO;
            }
            s = (struct flask_combo_slot){
                .action = old.usage ? FLASK_COMBO_OUT_USAGE : FLASK_COMBO_OUT_NONE,
                .param1 = old.usage,
            };
            memcpy(s.pos, old.pos, FLASK_COMBOS_KEYS);
        } else {
            LOG_WRN("flask/combos/%s ignored (len %d)", sub, (int)len);
            return 0;
        }
        for (int k = 0; k < FLASK_COMBOS_KEYS; k++) {
            if (s.pos[k] >= ZMK_KEYMAP_LEN) {
                s.pos[k] = FLASK_COMBOS_POS_NONE;
            }
        }
        if (s.action > FLASK_COMBO_OUT_MAX) {
            s.action = FLASK_COMBO_OUT_NONE;
        }
        K_SPINLOCK(&cfg_lock) {
            cfg.slots[idx] = s;
            slots_saved |= BIT64(idx);
            /* Migrated v2 slots re-save in the new shape on the next save. */
            if (len != sizeof(s)) {
                slots_dirty |= BIT64(idx);
            } else {
                slots_dirty &= ~BIT64(idx);
            }
            rebuild_lookup();
        }
        return 0;
    }
    return -ENOENT;
}

static int flask_combos_init(void) {
    for (int i = 0; i < MAX_ACTIVE; i++) {
        actives[i].slot = 0xFF;
    }
    /* Empty slots must read back as pos[]=NONE, not 0 — zero-filled BSS
     * means "position 0, eight times", which the app faithfully rendered
     * as phantom entries on every draft (bench 2026-07-12; flask_leader
     * shipped with the same init from day one). The matcher never cared
     * (usage==0 keeps a slot out of the lookup), the wire echo did. */
    for (int i = 0; i < FLASK_COMBOS_SLOTS; i++) {
        memset(cfg.slots[i].pos, FLASK_COMBOS_POS_NONE, FLASK_COMBOS_KEYS);
    }
    k_work_init_delayable(&timeout_task, timeout_handler);
    /* Lookup is already all-zero. Settings restore (via flask_proto's
     * handler) fills the table in before the first key event matters. */
    return 0;
}

SYS_INIT(flask_combos_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
