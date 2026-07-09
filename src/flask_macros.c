/*
 * flask_macros — runtime-editable macros (see include/flask_macros/flask_macros.h).
 *
 * Playback is a k_work_delayable state machine on the system workqueue: one
 * step (or tap-release phase) per invocation, the next scheduled after the
 * step's delay. All raises go through
 * raise_zmk_keycode_state_changed_from_encoded — the same exit flask_combos
 * outputs take, so anything a combo output can type, a macro step can.
 *
 * Threading: the step table and pacing knobs can be written from the raw-HID
 * path while playback reads them, so they sit behind a spinlock (same rule
 * as flask_combos' config). Playback state transitions (play/stop/step) are
 * guarded by the same lock; event raises happen outside it on copied values.
 *
 * Stuck-key safety: press steps push onto a held-usage stack; release steps
 * pop their match; stop and end-of-macro release whatever is left, newest
 * first. A macro that presses Shift and never releases it un-wedges the
 * moment it ends.
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
#include <zmk/events/keycode_state_changed.h>

#include <flask_macros/flask_macros.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TAP_MIN_MS 1
#define TAP_MAX_MS 500
#define TAP_DEFAULT_MS 30
#define WAIT_MIN_MS 0
#define WAIT_MAX_MS 2000
#define WAIT_DEFAULT_MS 15
#define WAIT_STEP_MAX_MS 10000

/* Press steps that can be held at once per macro. */
#define MAX_HELD 8

/* --- config (spinlocked: raw-HID writes race playback reads) --- */

static struct {
    bool enabled;
    uint16_t tap_ms;
    uint16_t wait_ms;
    struct flask_macro_step steps[FLASK_MACROS_SLOTS][FLASK_MACROS_STEPS];
} cfg = {
    .enabled = true,
    .tap_ms = TAP_DEFAULT_MS,
    .wait_ms = WAIT_DEFAULT_MS,
};

static struct k_spinlock cfg_lock;

/* --- playback state (guarded by cfg_lock; raises outside it) --- */

static int8_t play_slot = -1;
static uint8_t play_step;
static bool tap_up_pending;
static uint32_t tap_up_usage;
static uint32_t held[MAX_HELD];
static uint8_t held_count;

static struct k_work_delayable play_task;

static void raise_usage(uint32_t usage, bool pressed) {
    if (usage != 0) {
        raise_zmk_keycode_state_changed_from_encoded(usage, pressed, k_uptime_get());
    }
}

/* Callers hold cfg_lock. Copies out the usages so the raises can happen
 * after the lock drops. */
static uint8_t drain_held(uint32_t *out) {
    uint8_t n = held_count;

    for (int i = 0; i < n; i++) {
        out[i] = held[n - 1 - i]; /* newest first */
    }
    held_count = 0;
    return n;
}

static void finish_playback(void) {
    uint32_t release[MAX_HELD];
    uint8_t n;

    K_SPINLOCK(&cfg_lock) {
        n = drain_held(release);
        play_slot = -1;
        play_step = 0;
        tap_up_pending = false;
    }
    for (int i = 0; i < n; i++) {
        raise_usage(release[i], false);
    }
}

static void play_task_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint32_t raise_param = 0;
    bool raise_pressed = false;
    bool do_raise = false;
    bool done = false;
    int32_t next_delay = -1;

    K_SPINLOCK(&cfg_lock) {
        if (play_slot < 0) {
            /* Stopped between schedule and run. */
        } else if (tap_up_pending) {
            tap_up_pending = false;
            raise_param = tap_up_usage;
            raise_pressed = false;
            do_raise = true;
            /* Pop the tap's own down — only if it actually got pushed (a
             * full stack skips the push; popping blind would orphan a
             * press-step usage into a stuck key). */
            if (held_count > 0 && held[held_count - 1] == tap_up_usage) {
                held_count--;
            }
            next_delay = cfg.wait_ms;
        } else if (play_step >= FLASK_MACROS_STEPS) {
            done = true;
        } else {
            struct flask_macro_step s = cfg.steps[play_slot][play_step];

            switch (s.action) {
            case FLASK_MACRO_ACTION_TAP:
                play_step++;
                raise_param = s.param;
                raise_pressed = true;
                do_raise = true;
                if (held_count < MAX_HELD) {
                    held[held_count++] = s.param;
                }
                tap_up_pending = true;
                tap_up_usage = s.param;
                next_delay = cfg.tap_ms;
                break;
            case FLASK_MACRO_ACTION_PRESS:
                play_step++;
                raise_param = s.param;
                raise_pressed = true;
                do_raise = true;
                if (held_count < MAX_HELD) {
                    held[held_count++] = s.param;
                }
                next_delay = cfg.wait_ms;
                break;
            case FLASK_MACRO_ACTION_RELEASE:
                play_step++;
                raise_param = s.param;
                raise_pressed = false;
                do_raise = true;
                for (int i = held_count - 1; i >= 0; i--) {
                    if (held[i] == s.param) {
                        held[i] = held[--held_count];
                        break;
                    }
                }
                next_delay = cfg.wait_ms;
                break;
            case FLASK_MACRO_ACTION_WAIT:
                play_step++;
                next_delay = MIN(s.param, WAIT_STEP_MAX_MS);
                break;
            case FLASK_MACRO_ACTION_EMPTY:
            default:
                done = true;
                break;
            }
        }
    }

    if (do_raise) {
        raise_usage(raise_param, raise_pressed);
    }
    if (done) {
        finish_playback();
        return;
    }
    if (next_delay >= 0) {
        k_work_reschedule(&play_task, K_MSEC(next_delay));
    }
}

/* --- runtime API (proto channel 0x25 + &fmac behavior) --- */

bool flask_macros_enabled(void) {
    bool on;

    K_SPINLOCK(&cfg_lock) { on = cfg.enabled; }
    return on;
}

void flask_macros_set_enabled(bool on) {
    K_SPINLOCK(&cfg_lock) { cfg.enabled = on; }
    if (!on) {
        flask_macros_stop();
    }
}

uint16_t flask_macros_tap_ms(void) {
    uint16_t ms;

    K_SPINLOCK(&cfg_lock) { ms = cfg.tap_ms; }
    return ms;
}

void flask_macros_set_tap_ms(uint16_t ms) {
    ms = CLAMP(ms, TAP_MIN_MS, TAP_MAX_MS);
    K_SPINLOCK(&cfg_lock) { cfg.tap_ms = ms; }
}

uint16_t flask_macros_wait_ms(void) {
    uint16_t ms;

    K_SPINLOCK(&cfg_lock) { ms = cfg.wait_ms; }
    return ms;
}

void flask_macros_set_wait_ms(uint16_t ms) {
    ms = CLAMP(ms, WAIT_MIN_MS, WAIT_MAX_MS);
    K_SPINLOCK(&cfg_lock) { cfg.wait_ms = ms; }
}

uint8_t flask_macros_slot_count(void) { return FLASK_MACROS_SLOTS; }
uint8_t flask_macros_step_count(void) { return FLASK_MACROS_STEPS; }

int flask_macros_step_get(uint8_t slot, uint8_t step, struct flask_macro_step *out) {
    if (slot >= FLASK_MACROS_SLOTS || step >= FLASK_MACROS_STEPS || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = cfg.steps[slot][step]; }
    return 0;
}

int flask_macros_step_set(uint8_t slot, uint8_t step, const struct flask_macro_step *in) {
    if (slot >= FLASK_MACROS_SLOTS || step >= FLASK_MACROS_STEPS || in == NULL) {
        return -EINVAL;
    }

    struct flask_macro_step s = *in;

    if (s.action > FLASK_MACRO_ACTION_WAIT) {
        s.action = FLASK_MACRO_ACTION_EMPTY;
    }
    if (s.action == FLASK_MACRO_ACTION_WAIT) {
        s.param = MIN(s.param, WAIT_STEP_MAX_MS);
    }
    if (s.action == FLASK_MACRO_ACTION_EMPTY) {
        s.param = 0;
    }

    K_SPINLOCK(&cfg_lock) { cfg.steps[slot][step] = s; }
    return 0;
}

int flask_macros_play(uint8_t slot) {
    bool start = false;

    if (slot >= FLASK_MACROS_SLOTS) {
        return -EINVAL;
    }

    K_SPINLOCK(&cfg_lock) {
        if (!cfg.enabled) {
            /* -EACCES below */
        } else if (play_slot >= 0) {
            /* -EBUSY below */
        } else {
            play_slot = slot;
            play_step = 0;
            tap_up_pending = false;
            held_count = 0;
            start = true;
        }
    }

    if (!start) {
        bool on = flask_macros_enabled();

        LOG_WRN("flask_macros: play %d ignored (%s)", slot, on ? "busy" : "disabled");
        return on ? -EBUSY : -EACCES;
    }
    k_work_reschedule(&play_task, K_NO_WAIT);
    return 0;
}

void flask_macros_stop(void) {
    k_work_cancel_delayable(&play_task);
    finish_playback();
}

int flask_macros_playing_slot(void) {
    int slot;

    K_SPINLOCK(&cfg_lock) { slot = play_slot; }
    return slot;
}

/* --- persistence (settings subtree "flask/macros") ---
 *
 * v2 layout (2026-07-09, capacities round): one entry per USED slot —
 * "flask/macros/s<idx>" holds the used-steps PREFIX of the slot (length =
 * steps up to the last non-empty, x step size), "flask/macros/cfg" the
 * globals. Empty slots are deleted. NVS caps a single entry near its
 * sector size, so the old whole-table blob stopped scaling past ~4 KB;
 * per-slot prefix entries cost what the macro actually uses. The v1 blob
 * (exact "flask/macros" leaf) is ignored on load — it never reached
 * hardware. */

struct flask_macros_saved_cfg {
    uint8_t version;
    uint8_t enabled;
    uint16_t tap_ms;
    uint16_t wait_ms;
} __packed;

#define MACROS_SETTINGS_VERSION 2

int flask_macros_save(void) {
    struct flask_macros_saved_cfg saved = {.version = MACROS_SETTINGS_VERSION};
    /* One slot's steps — copied under the lock, written outside it. */
    static struct flask_macro_step slot[FLASK_MACROS_STEPS];

    K_SPINLOCK(&cfg_lock) {
        saved.enabled = cfg.enabled ? 1 : 0;
        saved.tap_ms = cfg.tap_ms;
        saved.wait_ms = cfg.wait_ms;
    }

    int err = settings_save_one("flask/macros/cfg", &saved, sizeof(saved));

    if (err) {
        LOG_ERR("flask/macros/cfg settings save failed: %d", err);
        return err;
    }

    for (int m = 0; m < FLASK_MACROS_SLOTS; m++) {
        int used = 0;
        char key[24];

        K_SPINLOCK(&cfg_lock) {
            memcpy(slot, cfg.steps[m], sizeof(slot));
        }
        for (int s = 0; s < FLASK_MACROS_STEPS; s++) {
            if (slot[s].action != FLASK_MACRO_ACTION_EMPTY) {
                used = s + 1;
            }
        }

        snprintf(key, sizeof(key), "flask/macros/s%d", m);
        err = (used == 0)
                  ? settings_delete(key)
                  : settings_save_one(key, slot, used * sizeof(struct flask_macro_step));
        if (err) {
            LOG_ERR("%s settings save failed: %d", key, err);
            return err;
        }
    }
    return 0;
}

/* Restore one entry of the "flask/macros" subtree; sub is the name past
 * the subtree ("cfg", "s<idx>", or NULL for the retired v1 blob leaf). */
int flask_macros_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    if (sub == NULL) {
        LOG_WRN("flask/macros v1 blob ignored (per-slot layout since v2)");
        return 0;
    }

    if (strcmp(sub, "cfg") == 0) {
        struct flask_macros_saved_cfg saved;

        if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            LOG_WRN("flask/macros/cfg unreadable (len %d)", (int)len);
            return 0;
        }
        if (saved.version != MACROS_SETTINGS_VERSION) {
            LOG_WRN("flask/macros/cfg version %d ignored", saved.version);
            return 0;
        }
        K_SPINLOCK(&cfg_lock) {
            cfg.enabled = saved.enabled != 0;
            cfg.tap_ms = CLAMP(saved.tap_ms, TAP_MIN_MS, TAP_MAX_MS);
            cfg.wait_ms = CLAMP(saved.wait_ms, WAIT_MIN_MS, WAIT_MAX_MS);
        }
        return 0;
    }

    if (sub[0] == 's') {
        int idx = atoi(&sub[1]);
        static struct flask_macro_step steps[FLASK_MACROS_STEPS];
        size_t n = len / sizeof(struct flask_macro_step);

        if (idx < 0 || idx >= FLASK_MACROS_SLOTS || n == 0 || n > FLASK_MACROS_STEPS ||
            len != n * sizeof(struct flask_macro_step)) {
            LOG_WRN("flask/macros/%s ignored (len %d)", sub, (int)len);
            return 0;
        }
        if (read_cb(cb_arg, steps, len) < 0) {
            return -EIO;
        }
        for (size_t s = 0; s < n; s++) {
            if (steps[s].action > FLASK_MACRO_ACTION_WAIT) {
                steps[s].action = FLASK_MACRO_ACTION_EMPTY;
            }
        }
        K_SPINLOCK(&cfg_lock) {
            memcpy(cfg.steps[idx], steps, len);
            memset(&cfg.steps[idx][n], 0,
                   (FLASK_MACROS_STEPS - n) * sizeof(struct flask_macro_step));
        }
        return 0;
    }
    return -ENOENT;
}

static int flask_macros_init(void) {
    k_work_init_delayable(&play_task, play_task_handler);
    return 0;
}

SYS_INIT(flask_macros_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
