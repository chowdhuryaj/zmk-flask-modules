/*
 * flask_leader — runtime-editable leader sequences (see
 * include/flask_leader/flask_leader.h).
 *
 * Capture model (simpler than flask_combos — no release/re-raise dance):
 * while a capture is open every position DOWN is swallowed outright
 * (ZMK_EV_EVENT_HANDLED) and appended to the buffer; the matching UP is
 * swallowed too (tracked in a pressed-set) so downstream never sees half a
 * key. Matching after each key:
 *
 *   - no slot has the buffer as a prefix        → capture ends, keys eaten
 *   - exactly one slot matches the FULL buffer
 *     and no other slot extends it              → fire, capture ends
 *   - a full match exists but longer candidates
 *     remain                                    → wait; the per-key timeout
 *                                                 fires the exact match
 *     (QMK leader semantics: timeout confirms the shorter sequence)
 *
 * Threading: position events and the timeout work item share ZMK's
 * single-threaded event context (same assumption core combo.c makes); the
 * slot table is spinlocked against raw-HID writes.
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
#include <zmk/matrix.h>

#include <flask_leader/flask_leader.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TIMEOUT_MIN_MS 100
#define TIMEOUT_MAX_MS 5000
#define TIMEOUT_DEFAULT_MS 1000

/* --- config (spinlocked: raw-HID writes race the matcher) --- */

static struct {
    bool enabled;
    uint16_t timeout_ms;
    struct flask_leader_slot slots[FLASK_LEADER_SLOTS];
} cfg = {
    .enabled = true,
    .timeout_ms = TIMEOUT_DEFAULT_MS,
};

static struct k_spinlock cfg_lock;

static uint8_t slot_seq_len(const struct flask_leader_slot *s) {
    uint8_t n = 0;

    while (n < FLASK_LEADER_KEYS && s->pos[n] != FLASK_LEADER_POS_NONE) {
        n++;
    }
    return n;
}

static bool slot_live(const struct flask_leader_slot *s) {
    return s->out.action != FLASK_OUTPUT_NONE && slot_seq_len(s) >= 1;
}

/* --- capture state (single event context, lock-free) --- */

static bool capturing;
static uint8_t buf[FLASK_LEADER_KEYS];
static uint8_t buf_len;
static uint8_t held[FLASK_LEADER_KEYS]; /* downs we swallowed, ups pending */
static uint8_t held_count;

static struct k_work_delayable timeout_task;

static void capture_end(void) {
    capturing = false;
    buf_len = 0;
    k_work_cancel_delayable(&timeout_task);
    /* held[] persists — swallowed downs still owe swallowed ups. */
}

/* Match the buffer against the table. Returns:
 *   -1 nothing matches (not even as a prefix)
 *   -2 prefix of something, no exact match yet
 *   -3 exact match exists but longer candidates remain (idx via *exact)
 *   >=0 unambiguous exact match — fire this slot                        */
static int match_buffer(int *exact_out) {
    int exact = -1;
    bool extendable = false;

    K_SPINLOCK(&cfg_lock) {
        for (int i = 0; i < FLASK_LEADER_SLOTS; i++) {
            const struct flask_leader_slot *s = &cfg.slots[i];

            if (!slot_live(s)) {
                continue;
            }
            uint8_t len = slot_seq_len(s);

            if (len < buf_len || memcmp(s->pos, buf, buf_len) != 0) {
                continue;
            }
            if (len == buf_len) {
                if (exact < 0) {
                    exact = i;
                }
            } else {
                extendable = true;
            }
        }
    }

    if (exact_out) {
        *exact_out = exact;
    }
    if (exact >= 0 && !extendable) {
        return exact;
    }
    if (exact >= 0) {
        return -3;
    }
    return extendable ? -2 : -1;
}

static void fire_slot(int idx, int64_t timestamp) {
    struct flask_output out = {0};

    K_SPINLOCK(&cfg_lock) { out = cfg.slots[idx].out; }
    LOG_DBG("flask_leader: slot %d fires (action %d)", idx, out.action);
    flask_output_fire(&out, timestamp);
}

static void timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!capturing) {
        return;
    }
    int exact = -1;

    match_buffer(&exact);
    if (exact >= 0) {
        fire_slot(exact, k_uptime_get());
    }
    capture_end();
}

void flask_leader_begin(void) {
    if (!flask_leader_enabled()) {
        return;
    }
    capturing = true;
    buf_len = 0;
    k_work_reschedule(&timeout_task, K_MSEC(flask_leader_timeout_ms()));
}

static int flask_leader_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Swallow the UP of any down we ate, capture over or not. */
    if (!ev->state) {
        for (int i = 0; i < held_count; i++) {
            if (held[i] == ev->position) {
                held[i] = held[--held_count];
                return ZMK_EV_EVENT_HANDLED;
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!capturing) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->position > 0xFE) {
        return ZMK_EV_EVENT_BUBBLE; /* table positions are bytes */
    }

    /* Eat the down; remember it so its up is eaten too. */
    if (held_count < FLASK_LEADER_KEYS) {
        held[held_count++] = (uint8_t)ev->position;
    }
    if (buf_len < FLASK_LEADER_KEYS) {
        buf[buf_len++] = (uint8_t)ev->position;
    }

    int res = match_buffer(NULL);

    if (res >= 0) {
        fire_slot(res, ev->timestamp);
        capture_end();
    } else if (res == -1 || buf_len >= FLASK_LEADER_KEYS) {
        capture_end();
    } else {
        /* -2 / -3: keep listening; every key re-arms the timeout. */
        k_work_reschedule(&timeout_task, K_MSEC(flask_leader_timeout_ms()));
    }
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(flask_leader, flask_leader_listener);
ZMK_SUBSCRIPTION(flask_leader, zmk_position_state_changed);

/* --- runtime API (proto channel 0x19) --- */

bool flask_leader_enabled(void) {
    bool on;

    K_SPINLOCK(&cfg_lock) { on = cfg.enabled; }
    return on;
}

void flask_leader_set_enabled(bool on) {
    K_SPINLOCK(&cfg_lock) { cfg.enabled = on; }
    if (!on && capturing) {
        capture_end();
    }
}

uint16_t flask_leader_timeout_ms(void) {
    uint16_t ms;

    K_SPINLOCK(&cfg_lock) { ms = cfg.timeout_ms; }
    return ms;
}

void flask_leader_set_timeout_ms(uint16_t ms) {
    ms = CLAMP(ms, TIMEOUT_MIN_MS, TIMEOUT_MAX_MS);
    K_SPINLOCK(&cfg_lock) { cfg.timeout_ms = ms; }
}

uint8_t flask_leader_slot_count(void) { return FLASK_LEADER_SLOTS; }

int flask_leader_slot_get(uint8_t idx, struct flask_leader_slot *out) {
    if (idx >= FLASK_LEADER_SLOTS || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = cfg.slots[idx]; }
    return 0;
}

int flask_leader_slot_set(uint8_t idx, const struct flask_leader_slot *in) {
    if (idx >= FLASK_LEADER_SLOTS || in == NULL) {
        return -EINVAL;
    }

    struct flask_leader_slot s = *in;
    bool gap = false;

    /* Normalize: positions must exist on the board; the sequence is the
     * leading prefix (first NONE ends it). */
    for (int k = 0; k < FLASK_LEADER_KEYS; k++) {
        if (s.pos[k] >= ZMK_KEYMAP_LEN) {
            s.pos[k] = FLASK_LEADER_POS_NONE;
        }
        if (s.pos[k] == FLASK_LEADER_POS_NONE) {
            gap = true;
        } else if (gap) {
            s.pos[k] = FLASK_LEADER_POS_NONE;
        }
    }
    flask_output_normalize(&s.out);

    K_SPINLOCK(&cfg_lock) { cfg.slots[idx] = s; }
    return 0;
}

/* --- persistence (settings subtree "flask/leader") --- */

struct flask_leader_saved_cfg {
    uint8_t version;
    uint8_t enabled;
    uint16_t timeout_ms;
} __packed;

#define LEADER_SETTINGS_VERSION 1

static bool slot_is_empty(const struct flask_leader_slot *s) {
    return s->out.action == FLASK_OUTPUT_NONE && s->pos[0] == FLASK_LEADER_POS_NONE;
}

int flask_leader_save(void) {
    struct flask_leader_saved_cfg saved = {.version = LEADER_SETTINGS_VERSION};
    struct flask_leader_slot slots[FLASK_LEADER_SLOTS];

    K_SPINLOCK(&cfg_lock) {
        saved.enabled = cfg.enabled ? 1 : 0;
        saved.timeout_ms = cfg.timeout_ms;
        memcpy(slots, cfg.slots, sizeof(slots));
    }

    int err = settings_save_one("flask/leader/cfg", &saved, sizeof(saved));

    if (err) {
        LOG_ERR("flask/leader/cfg settings save failed: %d", err);
        return err;
    }
    for (int i = 0; i < FLASK_LEADER_SLOTS; i++) {
        char key[24];

        snprintf(key, sizeof(key), "flask/leader/s%d", i);
        err = slot_is_empty(&slots[i]) ? settings_delete(key)
                                       : settings_save_one(key, &slots[i], sizeof(slots[i]));
        if (err) {
            LOG_ERR("%s settings save failed: %d", key, err);
            return err;
        }
    }
    return 0;
}

int flask_leader_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    if (sub == NULL) {
        return 0;
    }

    if (strcmp(sub, "cfg") == 0) {
        struct flask_leader_saved_cfg saved;

        if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            LOG_WRN("flask/leader/cfg unreadable (len %d)", (int)len);
            return 0;
        }
        if (saved.version != LEADER_SETTINGS_VERSION) {
            LOG_WRN("flask/leader/cfg version %d ignored", saved.version);
            return 0;
        }
        K_SPINLOCK(&cfg_lock) {
            cfg.enabled = saved.enabled != 0;
            cfg.timeout_ms = CLAMP(saved.timeout_ms, TIMEOUT_MIN_MS, TIMEOUT_MAX_MS);
        }
        return 0;
    }

    if (sub[0] == 's') {
        int idx = atoi(&sub[1]);
        struct flask_leader_slot s;

        if (idx < 0 || idx >= FLASK_LEADER_SLOTS || len != sizeof(s)) {
            LOG_WRN("flask/leader/%s ignored (len %d)", sub, (int)len);
            return 0;
        }
        if (read_cb(cb_arg, &s, sizeof(s)) < 0) {
            return -EIO;
        }
        /* Same normalization as the runtime setter. */
        flask_leader_slot_set((uint8_t)idx, &s);
        return 0;
    }
    return -ENOENT;
}

static int flask_leader_init(void) {
    /* Table starts empty: slots must read as pos[]=NONE, not 0. */
    for (int i = 0; i < FLASK_LEADER_SLOTS; i++) {
        memset(cfg.slots[i].pos, FLASK_LEADER_POS_NONE, FLASK_LEADER_KEYS);
    }
    k_work_init_delayable(&timeout_task, timeout_handler);
    return 0;
}

SYS_INIT(flask_leader_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
