/*
 * flask_gestures — runtime-editable mouse gestures input processor.
 *
 * Port of the Flask (QMK) pd_gestures module (drashna's
 * pointing_device_gestures), ratchet-only, engine semantics preserved:
 *
 *   - while a gesture is open, REL_X/REL_Y (and wheel) events are
 *     swallowed and x/y accumulate travel
 *   - one output fires per ratchet-step of euclidean travel, in the 8-way
 *     dominant direction; a fast pass can cover several steps in one
 *     sensor report — fire for each, keep the remainder (burst-capped)
 *   - empty diagonals fall back to the nearest cardinal by dominant axis
 *
 * Sits at the FRONT of both ball listener chains (raw counts, like the
 * kot149 module it replaces). The &fges behavior opens/closes gestures;
 * everything (table, ratchet step, active set) is live over channel 0x11.
 *
 * Threading: input events run in one context; the config table is
 * spinlocked against raw-HID writes (same rule as every flask module).
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_flask_gestures

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>

#include <flask_gestures/flask_gestures.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RATCHET_MIN 50
#define RATCHET_MAX 2000
#define RATCHET_DEFAULT 150 /* the benched kot149 stroke-size feel */

/* --- config (spinlocked) --- */

static struct {
    bool enabled;
    uint16_t ratchet_step;
    uint8_t active_set;
    struct flask_output table[FLASK_GESTURES_SETS][FLASK_GESTURES_DIRS];
} cfg = {
    .enabled = true,
    .ratchet_step = RATCHET_DEFAULT,
};

static struct k_spinlock cfg_lock;

/* --- gesture state (single input context) --- */

static bool active;
static uint8_t active_table; /* resolved set index */
static int32_t acc_x, acc_y;

/* Direction order 0-7: E SE S SW W NW N NE (East, then clockwise; mouse +y
 * points south). ZMK-encoded usages; kb page 0x07, consumer page 0x0C,
 * mods bits 24-31 (LCTL 0x01, LSFT 0x02). AJ's four Adept/Svalboard sets —
 * seeded when no settings exist, same as QMK's gesture_set_defaults. */
#define KB(id) ((0x07UL << 16) | (id))
#define CONS(id) ((0x0CUL << 16) | (id))
#define CTL(u) ((0x01UL << 24) | (u))
#define CTLSFT(u) ((0x03UL << 24) | (u))
#define U(usage) {.action = FLASK_OUTPUT_USAGE, .param = (usage)}
#define NONE {0}

static const struct flask_output gesture_set_defaults[4][FLASK_GESTURES_DIRS] = {
    /* set 0 "arrows": E SE S SW W NW N NE */
    {U(KB(0x4F)), NONE, U(KB(0x51)), NONE, U(KB(0x50)), NONE, U(KB(0x52)), NONE},
    /* set 1 "editing": E=DEL S=ENTER W=BSPC N=ESC */
    {U(KB(0x4C)), NONE, U(KB(0x28)), NONE, U(KB(0x2A)), NONE, U(KB(0x29)), NONE},
    /* set 2 "media": E=next S=vol- W=prev N=vol+ */
    {U(CONS(0xB5)), NONE, U(CONS(0xEA)), NONE, U(CONS(0xB6)), NONE, U(CONS(0xE9)), NONE},
    /* set 3 "tab-nav": E=^Tab S=Space W=^⇧Tab N=Tab */
    {U(CTL(KB(0x2B))), NONE, U(KB(0x2C)), NONE, U(CTLSFT(KB(0x2B))), NONE, U(KB(0x2B)), NONE},
};

static void seed_defaults(void) {
    memcpy(cfg.table, gesture_set_defaults,
           MIN(sizeof(gesture_set_defaults), sizeof(cfg.table)));
}

/* --- engine (pd_gestures.c semantics) --- */

static bool reached(int32_t x, int32_t y, uint16_t thresh) {
    int64_t d2 = (int64_t)x * x + (int64_t)y * y;
    int64_t t2 = (int64_t)thresh * thresh;

    return d2 >= t2;
}

/* 8-way bin, 45° sectors: 0=E 1=SE 2=S 3=SW 4=W 5=NW 6=N 7=NE. */
static uint8_t direction8(int32_t x, int32_t y) {
    float r = atan2f((float)y, (float)x);
    int16_t d = (int16_t)(180.0f * r / 3.14159265f);

    return ((d + 360 + 22) % 360 / 45) % 8;
}

static void fire(uint8_t dir, int32_t x, int32_t y, int64_t ts) {
    struct flask_output out = {0};

    K_SPINLOCK(&cfg_lock) {
        out = cfg.table[active_table][dir];
        /* Empty diagonal → nearest cardinal by dominant axis (QMK). */
        if (out.action == FLASK_OUTPUT_NONE && (dir & 1)) {
            int64_t ax = x < 0 ? -(int64_t)x : x;
            int64_t ay = y < 0 ? -(int64_t)y : y;
            uint8_t cardinal = (ax >= ay) ? ((x >= 0) ? 0 : 4) : ((y >= 0) ? 2 : 6);

            out = cfg.table[active_table][cardinal];
        }
    }
    flask_output_fire(&out, ts);
}

void flask_gestures_begin(uint8_t set) {
    K_SPINLOCK(&cfg_lock) {
        if (!cfg.enabled) {
            K_SPINLOCK_BREAK;
        }
        active_table = (set == FLASK_GESTURES_FOLLOW_ACTIVE) ? cfg.active_set
                                                             : MIN(set, FLASK_GESTURES_SETS - 1);
        active = true;
        acc_x = 0;
        acc_y = 0;
    }
}

void flask_gestures_end(void) {
    active = false;
    acc_x = 0;
    acc_y = 0;
}

static int flask_gestures_handle_event(const struct device *dev, struct input_event *event,
                                       uint32_t param1, uint32_t param2,
                                       struct zmk_input_processor_state *state) {
    ARG_UNUSED(dev);

    if (!active || event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Swallow everything relative while a gesture is open (QMK zeroed
     * x/y/h/v); only X/Y feed the accumulator. */
    int32_t v = event->value;
    bool is_x = (event->code == INPUT_REL_X);
    bool is_y = (event->code == INPUT_REL_Y);

    event->value = 0;
    event->sync = false;

    if (is_x) {
        acc_x += v;
    } else if (is_y) {
        acc_y += v;
    } else {
        return ZMK_INPUT_PROC_STOP;
    }

    uint16_t step;

    K_SPINLOCK(&cfg_lock) { step = cfg.ratchet_step; }

    /* Fire one output per ratchet step; keep the remainder (pd_gestures
     * fast-roll fix), burst-capped. */
    uint8_t burst = 8;

    while (reached(acc_x, acc_y, step) && burst--) {
        fire(direction8(acc_x, acc_y), acc_x, acc_y, k_uptime_get());
        float dist = sqrtf((float)acc_x * acc_x + (float)acc_y * acc_y);

        if (dist <= (float)step) {
            acc_x = 0;
            acc_y = 0;
            break;
        }
        float keep = (dist - (float)step) / dist;

        acc_x = (int32_t)((float)acc_x * keep);
        acc_y = (int32_t)((float)acc_y * keep);
    }

    return ZMK_INPUT_PROC_STOP;
}

/* --- runtime API (proto channel 0x11) --- */

bool flask_gestures_enabled(void) {
    bool on;

    K_SPINLOCK(&cfg_lock) { on = cfg.enabled; }
    return on;
}

void flask_gestures_set_enabled(bool on) {
    K_SPINLOCK(&cfg_lock) { cfg.enabled = on; }
    if (!on) {
        flask_gestures_end();
    }
}

uint16_t flask_gestures_ratchet_step(void) {
    uint16_t s;

    K_SPINLOCK(&cfg_lock) { s = cfg.ratchet_step; }
    return s;
}

void flask_gestures_set_ratchet_step(uint16_t step) {
    step = CLAMP(step, RATCHET_MIN, RATCHET_MAX);
    K_SPINLOCK(&cfg_lock) { cfg.ratchet_step = step; }
}

uint8_t flask_gestures_active_set(void) {
    uint8_t s;

    K_SPINLOCK(&cfg_lock) { s = cfg.active_set; }
    return s;
}

void flask_gestures_set_active_set(uint8_t set) {
    K_SPINLOCK(&cfg_lock) { cfg.active_set = MIN(set, FLASK_GESTURES_SETS - 1); }
}

uint8_t flask_gestures_set_count(void) { return FLASK_GESTURES_SETS; }

int flask_gestures_output_get(uint8_t set, uint8_t dir, struct flask_output *out) {
    if (set >= FLASK_GESTURES_SETS || dir >= FLASK_GESTURES_DIRS || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = cfg.table[set][dir]; }
    return 0;
}

int flask_gestures_output_set(uint8_t set, uint8_t dir, const struct flask_output *in) {
    if (set >= FLASK_GESTURES_SETS || dir >= FLASK_GESTURES_DIRS || in == NULL) {
        return -EINVAL;
    }

    struct flask_output out = *in;

    flask_output_normalize(&out);
    K_SPINLOCK(&cfg_lock) { cfg.table[set][dir] = out; }
    return 0;
}

/* --- persistence (settings subtree "flask/gestures") --- */

struct flask_gestures_saved_cfg {
    uint8_t version;
    uint8_t enabled;
    uint16_t ratchet_step;
    uint8_t active_set;
} __packed;

#define GESTURES_SETTINGS_VERSION 1

int flask_gestures_save(void) {
    struct flask_gestures_saved_cfg saved = {.version = GESTURES_SETTINGS_VERSION};
    struct flask_output sets[FLASK_GESTURES_SETS][FLASK_GESTURES_DIRS];

    K_SPINLOCK(&cfg_lock) {
        saved.enabled = cfg.enabled ? 1 : 0;
        saved.ratchet_step = cfg.ratchet_step;
        saved.active_set = cfg.active_set;
        memcpy(sets, cfg.table, sizeof(sets));
    }

    int err = settings_save_one("flask/gestures/cfg", &saved, sizeof(saved));

    if (err) {
        LOG_ERR("flask/gestures/cfg settings save failed: %d", err);
        return err;
    }
    for (int i = 0; i < FLASK_GESTURES_SETS; i++) {
        char key[24];

        snprintf(key, sizeof(key), "flask/gestures/s%d", i);
        err = settings_save_one(key, sets[i], sizeof(sets[i]));
        if (err) {
            LOG_ERR("%s settings save failed: %d", key, err);
            return err;
        }
    }
    return 0;
}

int flask_gestures_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                    void *cb_arg) {
    if (sub == NULL) {
        return 0;
    }

    if (strcmp(sub, "cfg") == 0) {
        struct flask_gestures_saved_cfg saved;

        if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            LOG_WRN("flask/gestures/cfg unreadable (len %d)", (int)len);
            return 0;
        }
        if (saved.version != GESTURES_SETTINGS_VERSION) {
            LOG_WRN("flask/gestures/cfg version %d ignored", saved.version);
            return 0;
        }
        K_SPINLOCK(&cfg_lock) {
            cfg.enabled = saved.enabled != 0;
            cfg.ratchet_step = CLAMP(saved.ratchet_step, RATCHET_MIN, RATCHET_MAX);
            cfg.active_set = MIN(saved.active_set, FLASK_GESTURES_SETS - 1);
        }
        return 0;
    }

    if (sub[0] == 's') {
        int idx = atoi(&sub[1]);
        struct flask_output set[FLASK_GESTURES_DIRS];

        if (idx < 0 || idx >= FLASK_GESTURES_SETS || len != sizeof(set)) {
            LOG_WRN("flask/gestures/%s ignored (len %d)", sub, (int)len);
            return 0;
        }
        if (read_cb(cb_arg, set, sizeof(set)) < 0) {
            return -EIO;
        }
        K_SPINLOCK(&cfg_lock) {
            for (int d = 0; d < FLASK_GESTURES_DIRS; d++) {
                flask_output_normalize(&set[d]);
                cfg.table[idx][d] = set[d];
            }
        }
        return 0;
    }
    return -ENOENT;
}

/* --- driver plumbing --- */

static const struct zmk_input_processor_driver_api flask_gestures_api = {
    .handle_event = flask_gestures_handle_event,
};

static int flask_gestures_init(const struct device *dev) {
    ARG_UNUSED(dev);
    /* Defaults seed the QMK gesture_set_defaults; settings restore (which
     * runs later in boot) overwrites whatever the user saved. */
    seed_defaults();
    return 0;
}

#define FLASK_GESTURES_INST(n)                                                                     \
    DEVICE_DT_INST_DEFINE(n, flask_gestures_init, NULL, NULL, NULL, POST_KERNEL,                   \
                          CONFIG_INPUT_INIT_PRIORITY, &flask_gestures_api);

DT_INST_FOREACH_STATUS_OKAY(FLASK_GESTURES_INST)
