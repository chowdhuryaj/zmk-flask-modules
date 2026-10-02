/*
 * flask_adaptive — runtime adaptive keys (see include/flask_adaptive/).
 *
 * Matching semantics are urob/zmk-adaptive-key's behavior_adaptive_key.c
 * (main @ e987227) with the const DT config swapped for a runtime rule
 * table shared by all sets:
 *
 *  - One last-key tracker (zmk_keycode_state_changed, presses only). Modifier
 *    usages never count as the last key; everything else does, including
 *    &fak's own outputs and macro playback.
 *  - `&fak <set>` scans the rules of that set in pool order; first live rule
 *    whose trigger key, modifiers and idle window match wins. No match ->
 *    the set's fallback.
 *  - The output is fired like urob: one step = press now / release on key
 *    release; several steps = tap steps 0..n-2 through the behavior queue,
 *    press the last, queue its release on key release.
 *  - Press state is keyed by key position and copies the last binding at
 *    press, so a table edit mid-hold cannot unbalance the release.
 *
 * Persistence is the flask_combos model: "flask/adaptive/{cfg,r<idx>,f<set>}",
 * dirty entries only, compiled defaults (flask,adaptive-defaults) filled
 * after restore, deleted defaults stored as all-zero tombstones.
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
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/hid.h>
#include <zmk/keys.h>
#include <zmk/keymap.h>

#include <flask_adaptive/flask_adaptive.h>

#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS)
#include <flask_macros/flask_macros.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AK_FIRST_PAGE_KEYBOARD 7

BUILD_ASSERT(FLASK_ADAPTIVE_RULES <= 64, "rule save bitmaps are u64");
BUILD_ASSERT(FLASK_ADAPTIVE_SETS <= 32, "fallback save bitmaps are u32");
BUILD_ASSERT(sizeof(struct flask_adaptive_step) == 11, "step settings entries are the raw struct");
BUILD_ASSERT(sizeof(struct flask_adaptive_rule) == 8 + 11 * FLASK_ADAPTIVE_STEPS,
              "rule settings entries are the raw struct");

/* Names of the behaviors the engine fires through / refuses to recurse
 * into. Resolved from devicetree; NULL when the keymap lacks the node. */
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_key_press)
#define AK_KP_NAME_INIT DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_key_press))
#else
#define AK_KP_NAME_INIT NULL
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_flask_adaptive)
#define AK_SELF_NAME_INIT DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_flask_adaptive))
#else
#define AK_SELF_NAME_INIT NULL
#endif

#if IS_ENABLED(CONFIG_ZMK_FLASK_MACROS) && DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_flask_macros)
#define AK_FMAC_NAME_INIT DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_flask_macros))
#else
#define AK_FMAC_NAME_INIT NULL
#endif

static const char *const AK_KP_NAME = AK_KP_NAME_INIT;
static const char *const AK_SELF_NAME = AK_SELF_NAME_INIT;
static const char *const AK_FMAC_NAME = AK_FMAC_NAME_INIT;

/* --- config (spinlocked: raw-HID writes race the engine) --- */

static struct {
    bool enabled;
    struct flask_adaptive_rule rules[FLASK_ADAPTIVE_RULES];
    struct flask_adaptive_step fallback[FLASK_ADAPTIVE_SETS];
} cfg = {
    .enabled = true,
};

static struct k_spinlock cfg_lock;

/* Save bookkeeping (under cfg_lock): which entries exist in settings, which
 * changed since their last successful save. Rules and fallbacks are
 * separate bitmaps. */
static uint64_t rules_saved;
static uint64_t rules_dirty;
static uint64_t __maybe_unused rules_seeded; /* defaults_commit has handled this index */
static uint32_t fb_saved;
static uint32_t fb_dirty;
static uint32_t __maybe_unused fb_seeded;
static bool cfg_saved;
static bool cfg_dirty;

/* ======================================================================
 * Compiled defaults: the keymap's `flask,adaptive-defaults` node.
 *
 * `sets = <&ak_rti &ak_alt>` points at urob adaptive-key nodes. Set i is
 * sets[i]: fallback = the node's bindings[0]; each child expands to one
 * rule per trigger-keys entry, packed into the pool in (set, child,
 * trigger) order. Nested DT_FOREACH_PROP_ELEM -> DT_FOREACH_CHILD_VARGS;
 * each child holds its triggers as an array and two sibling LISTIFYs
 * (triggers, bindings), expanded to rules in C at commit.
 * ====================================================================== */

#define LOW_BITS64(n) ((n) >= 64 ? UINT64_MAX : ((UINT64_C(1) << ((n) % 64)) - 1))

#if DT_HAS_COMPAT_STATUS_OKAY(flask_adaptive_defaults)

#define FAD_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(flask_adaptive_defaults)
#define FAD_MAX_TRIG 8
#define FAD_SETS_LEN DT_PROP_LEN(FAD_NODE, sets)

struct fad_def_child {
    uint8_t set;
    uint8_t n_trig;
    uint8_t n_bind;
    uint8_t flags;
    int32_t max_idle; /* ms, -1 = any time */
    uint32_t trig[FAD_MAX_TRIG];
    struct zmk_behavior_binding b[FLASK_ADAPTIVE_STEPS];
};

#define FAD_TRIG(i, child) DT_PROP_BY_IDX(child, trigger_keys, i)
#define FAD_BIND(i, child) ZMK_KEYMAP_EXTRACT_BINDING(i, child)

/* Per child (one `rti_a { ... }` node). set_idx is the index into `sets`. */
#define FAD_CHILD(child, set_idx)                                                                  \
    {                                                                                              \
        .set = set_idx,                                                                            \
        .n_trig = DT_PROP_LEN(child, trigger_keys),                                                \
        .n_bind = DT_PROP_LEN(child, bindings),                                                    \
        .flags = DT_PROP(child, strict_modifiers) ? FLASK_AK_FLAG_EXACT : 0,                       \
        .max_idle = DT_PROP(child, max_prior_idle_ms),                                             \
        .trig = {LISTIFY(DT_PROP_LEN(child, trigger_keys), FAD_TRIG, (, ), child)},                \
        .b = {LISTIFY(DT_PROP_LEN(child, bindings), FAD_BIND, (, ), child)},                       \
    },

/* Per element of `sets` (the ak node behind the phandle): all its children. */
#define FAD_SET_CHILDREN(node, prop, idx)                                                          \
    DT_FOREACH_CHILD_VARGS(DT_PHANDLE_BY_IDX(node, prop, idx), FAD_CHILD, idx)

static const struct fad_def_child fad_children[] = {
    DT_FOREACH_PROP_ELEM(FAD_NODE, sets, FAD_SET_CHILDREN)};

/* Per element of `sets`: that ak node's default binding (bindings[0]). */
#define FAD_SET_FALLBACK(node, prop, idx)                                                          \
    ZMK_KEYMAP_EXTRACT_BINDING(0, DT_PHANDLE_BY_IDX(node, prop, idx)),

static const struct zmk_behavior_binding fad_fallbacks[] = {
    DT_FOREACH_PROP_ELEM(FAD_NODE, sets, FAD_SET_FALLBACK)};

/* Total rules the table expands to (constant expression). */
#define FAD_CHILD_TRIGS(child) DT_PROP_LEN(child, trigger_keys) +
#define FAD_SET_TRIGS(node, prop, idx)                                                             \
    DT_FOREACH_CHILD(DT_PHANDLE_BY_IDX(node, prop, idx), FAD_CHILD_TRIGS)
#define FAD_TOTAL_RULES (DT_FOREACH_PROP_ELEM(FAD_NODE, sets, FAD_SET_TRIGS) 0)

BUILD_ASSERT(FAD_SETS_LEN <= FLASK_ADAPTIVE_SETS, "more default sets than ZMK_FLASK_ADAPTIVE_SETS");
BUILD_ASSERT(FAD_TOTAL_RULES <= FLASK_ADAPTIVE_RULES,
             "more default rules than ZMK_FLASK_ADAPTIVE_RULES");

/* Per-child limits the runtime table cannot represent. */
#define FAD_CHECK_CHILD(child)                                                                     \
    BUILD_ASSERT(DT_PROP_LEN(child, bindings) <= FLASK_ADAPTIVE_STEPS,                             \
                 "adaptive default: more bindings than ZMK_FLASK_ADAPTIVE_STEPS");                 \
    BUILD_ASSERT(DT_PROP_LEN(child, trigger_keys) <= FAD_MAX_TRIG,                                 \
                 "adaptive default: more than 8 trigger-keys in one child");                       \
    BUILD_ASSERT(DT_PROP(child, min_prior_idle_ms) == -1,                                          \
                 "adaptive default: min-prior-idle-ms is not supported");                          \
    BUILD_ASSERT(!DT_PROP(child, delete_prior), "adaptive default: delete-prior is not supported");

#define FAD_CHECK_SET(node, prop, idx)                                                             \
    BUILD_ASSERT(DT_PROP_LEN_OR(DT_PHANDLE_BY_IDX(node, prop, idx), dead_keys, 0) == 0,            \
                 "adaptive default: dead-keys are not supported");                                 \
    DT_FOREACH_CHILD(DT_PHANDLE_BY_IDX(node, prop, idx), FAD_CHECK_CHILD)

DT_FOREACH_PROP_ELEM(FAD_NODE, sets, FAD_CHECK_SET)

/* Defaulted = the index has a compiled default, so deleting it must store
 * a tombstone rather than settings_delete (or the seed resurrects). */
#define FAD_RULES_DEFAULTED LOW_BITS64(FAD_TOTAL_RULES)
#define FAD_FB_DEFAULTED ((uint32_t)LOW_BITS64(FAD_SETS_LEN))

#else /* no defaults node in the keymap */

#define FAD_RULES_DEFAULTED 0
#define FAD_FB_DEFAULTED 0

#endif

static const uint64_t rules_defaulted = FAD_RULES_DEFAULTED;
static const uint32_t fb_defaulted = FAD_FB_DEFAULTED;

/* --- normalization --- */

static uint32_t norm_trigger(uint32_t t) {
    if (t != 0 && ((t >> 16) & 0xFF) == 0) {
        t |= (uint32_t)AK_FIRST_PAGE_KEYBOARD << 16;
    }
    return t;
}

static bool is_self_id(uint16_t id) {
    zmk_behavior_local_id_t self;

    if (AK_SELF_NAME == NULL) {
        return false;
    }
    self = zmk_behavior_get_local_id(AK_SELF_NAME);
    return self != 0 && self != UINT16_MAX && self == id;
}

/* flask_tapdance_output_set normalization, verbatim, plus no recursion. */
static void step_normalize(struct flask_adaptive_step *out) {
    if (out->action > FLASK_AK_OUT_MAX) {
        out->action = FLASK_AK_OUT_NONE;
    }
    if (out->action == FLASK_AK_OUT_USAGE && out->param1 == 0) {
        out->action = FLASK_AK_OUT_NONE;
    }
    if (out->action == FLASK_AK_OUT_BEHAVIOR && is_self_id(out->behavior_id)) {
        out->action = FLASK_AK_OUT_NONE;
    }
    if (out->action == FLASK_AK_OUT_NONE) {
        out->behavior_id = 0;
        out->param1 = 0;
        out->param2 = 0;
    }
    if (out->action != FLASK_AK_OUT_BEHAVIOR) {
        out->behavior_id = 0;
        if (out->action != FLASK_AK_OUT_NONE) {
            out->param2 = 0;
        }
    }
}

static bool rule_is_empty(const struct flask_adaptive_rule *r) {
    const uint8_t *p = (const uint8_t *)r;

    for (size_t i = 0; i < sizeof(*r); i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

/* --- last-key tracker (event thread only, like urob's globals) --- */

static struct {
    bool valid;
    uint16_t page;
    uint32_t id;
    uint8_t mods;
    int64_t ts;
} last;

static int ak_keycode_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    /* Modifier usages never count: "b, hold Shift, fak" must still see b. */
    if (is_mod(ev->usage_page, ev->keycode)) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    last.valid = true;
    last.page = ev->usage_page;
    last.id = ev->keycode;
    last.mods = ev->implicit_modifiers | zmk_hid_get_explicit_mods();
    last.ts = ev->timestamp;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_adaptive, ak_keycode_listener);
ZMK_SUBSCRIPTION(flask_adaptive, zmk_keycode_state_changed);

/* Right mods fold onto left; exact = equal, else trigger mods subset of
 * the last key's. */
static bool mods_ok(uint8_t trig, uint8_t seen, bool exact) {
    trig = (trig | (trig >> 4)) & 0x0F;
    seen = (seen | (seen >> 4)) & 0x0F;
    return exact ? trig == seen : (trig & seen) == trig;
}

/* Fills seq with the output for `set`, returns its length. Caller holds
 * cfg_lock; reads the tracker, which only the event thread writes. */
static uint8_t pick_locked(uint8_t set, int64_t t, struct flask_adaptive_step *seq) {
    uint8_t n = 0;
    bool matched = false;

    if (cfg.enabled && last.valid) {
        for (int i = 0; i < FLASK_ADAPTIVE_RULES && !matched; i++) {
            const struct flask_adaptive_rule *r = &cfg.rules[i];
            uint32_t trig = r->trigger;
            uint16_t idle = r->max_idle_ms;

            if (r->set != set || trig == 0 || r->steps[0].action == FLASK_AK_OUT_NONE) {
                continue;
            }
            if (((trig >> 16) & 0xFF) != last.page || (trig & 0xFFFF) != last.id) {
                continue;
            }
            if (!mods_ok(trig >> 24, last.mods, (r->flags & FLASK_AK_FLAG_EXACT) != 0)) {
                continue;
            }
            if (idle != 0 && (t - last.ts) > (int64_t)idle) {
                continue;
            }
            matched = true;
            while (n < FLASK_ADAPTIVE_STEPS && r->steps[n].action != FLASK_AK_OUT_NONE) {
                seq[n] = r->steps[n];
                n++;
            }
        }
    }
    if (!matched && cfg.fallback[set].action != FLASK_AK_OUT_NONE) {
        seq[0] = cfg.fallback[set];
        n = 1;
    }
    return n;
}

/* Typed step -> behavior binding. 0 ok, -1 skip the step. */
static int build_binding(const struct flask_adaptive_step *s, struct zmk_behavior_binding *b) {
    memset(b, 0, sizeof(*b));

    switch (s->action) {
    case FLASK_AK_OUT_USAGE:
        if (AK_KP_NAME == NULL || s->param1 == 0) {
            return -1;
        }
        b->behavior_dev = AK_KP_NAME;
        b->param1 = s->param1;
        return 0;
    case FLASK_AK_OUT_MACRO:
        if (AK_FMAC_NAME == NULL) {
            LOG_WRN("flask_adaptive: macro step needs flask_macros and an &fmac node");
            return -1;
        }
        b->behavior_dev = AK_FMAC_NAME;
        b->param1 = s->param1;
        return 0;
    case FLASK_AK_OUT_BEHAVIOR: {
        const char *name = zmk_behavior_find_behavior_name_from_local_id(s->behavior_id);

        if (name == NULL) {
            LOG_WRN("flask_adaptive: behavior id %u not found", s->behavior_id);
            return -1;
        }
        if (AK_SELF_NAME != NULL && strcmp(name, AK_SELF_NAME) == 0) {
            return -1;
        }
        b->behavior_dev = name;
        b->param1 = s->param1;
        b->param2 = s->param2;
        return 0;
    }
    default:
        return -1;
    }
}

/* --- press state, keyed by key position (single-context like tapdance) --- */

#define AK_MAX_HELD 4

struct ak_press {
    bool used;
    uint32_t position;
    uint8_t n;
    struct zmk_behavior_binding last; /* copied at press */
};

static struct ak_press presses[AK_MAX_HELD];

int flask_adaptive_pressed(uint8_t set, struct zmk_behavior_binding_event event) {
    struct flask_adaptive_step seq[FLASK_ADAPTIVE_STEPS];
    struct zmk_behavior_binding bs[FLASK_ADAPTIVE_STEPS];
    uint8_t n = 0;
    uint8_t m = 0;

    if (set >= FLASK_ADAPTIVE_SETS) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { n = pick_locked(set, event.timestamp, seq); }

    for (uint8_t i = 0; i < n; i++) {
        if (build_binding(&seq[i], &bs[m]) == 0) {
            m++;
        }
    }
    if (m == 0) {
        return 0;
    }

    struct ak_press *p = NULL;

    for (int i = 0; i < AK_MAX_HELD; i++) {
        if (presses[i].used && presses[i].position == event.position) {
            p = &presses[i]; /* press without a release: reuse */
            break;
        }
    }
    for (int i = 0; p == NULL && i < AK_MAX_HELD; i++) {
        if (!presses[i].used) {
            p = &presses[i];
        }
    }
    if (p == NULL) {
        LOG_ERR("flask_adaptive: no free press slot");
        return -ENOMEM;
    }
    p->used = true;
    p->position = event.position;
    p->n = m;
    p->last = bs[m - 1];

    if (m == 1) {
        return zmk_behavior_invoke_binding(&bs[0], event, true);
    }
    for (uint8_t i = 0; i < m - 1; i++) {
        zmk_behavior_queue_add(&event, bs[i], true, CONFIG_ZMK_FLASK_ADAPTIVE_TAP_MS);
        zmk_behavior_queue_add(&event, bs[i], false, CONFIG_ZMK_FLASK_ADAPTIVE_WAIT_MS);
    }
    return zmk_behavior_queue_add(&event, bs[m - 1], true, CONFIG_ZMK_FLASK_ADAPTIVE_TAP_MS);
}

int flask_adaptive_released(uint8_t set, struct zmk_behavior_binding_event event) {
    ARG_UNUSED(set);

    for (int i = 0; i < AK_MAX_HELD; i++) {
        if (!presses[i].used || presses[i].position != event.position) {
            continue;
        }
        struct zmk_behavior_binding b = presses[i].last;
        uint8_t n = presses[i].n;

        presses[i].used = false;
        if (n > 1) {
            return zmk_behavior_queue_add(&event, b, false, CONFIG_ZMK_FLASK_ADAPTIVE_WAIT_MS);
        }
        return zmk_behavior_invoke_binding(&b, event, false);
    }
    return 0;
}

/* --- runtime API (proto channel 0x2B) --- */

bool flask_adaptive_enabled(void) {
    bool on;

    K_SPINLOCK(&cfg_lock) { on = cfg.enabled; }
    return on;
}

void flask_adaptive_set_enabled(bool on) {
    K_SPINLOCK(&cfg_lock) {
        cfg.enabled = on;
        cfg_dirty = true;
    }
}

uint8_t flask_adaptive_set_count(void) { return FLASK_ADAPTIVE_SETS; }
uint8_t flask_adaptive_rule_count(void) { return FLASK_ADAPTIVE_RULES; }
uint8_t flask_adaptive_step_count(void) { return FLASK_ADAPTIVE_STEPS; }

int flask_adaptive_rule_get(uint8_t idx, struct flask_adaptive_rule *out) {
    if (idx >= FLASK_ADAPTIVE_RULES || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = cfg.rules[idx]; }
    return 0;
}

int flask_adaptive_rule_set(uint8_t idx, uint8_t set, uint32_t trigger, uint16_t max_idle_ms,
                            uint8_t flags) {
    if (idx >= FLASK_ADAPTIVE_RULES) {
        return -EINVAL;
    }
    if (trigger != 0 && set >= FLASK_ADAPTIVE_SETS) {
        return -EINVAL;
    }
    trigger = norm_trigger(trigger);
    max_idle_ms = MIN(max_idle_ms, FLASK_AK_IDLE_MAX_MS);
    flags &= FLASK_AK_FLAG_EXACT;

    K_SPINLOCK(&cfg_lock) {
        struct flask_adaptive_rule *r = &cfg.rules[idx];

        if (trigger == 0) {
            memset(r, 0, sizeof(*r)); /* delete: header and every step */
        } else {
            r->set = set;
            r->trigger = trigger;
            r->max_idle_ms = max_idle_ms;
            r->flags = flags;
        }
        rules_dirty |= BIT64(idx);
    }
    return 0;
}

int flask_adaptive_step_set(uint8_t rule, uint8_t step, const struct flask_adaptive_step *in) {
    if (rule >= FLASK_ADAPTIVE_RULES || step >= FLASK_ADAPTIVE_STEPS || in == NULL) {
        return -EINVAL;
    }

    struct flask_adaptive_step out = *in;

    step_normalize(&out);
    K_SPINLOCK(&cfg_lock) {
        cfg.rules[rule].steps[step] = out;
        rules_dirty |= BIT64(rule);
    }
    return 0;
}

int flask_adaptive_fallback_get(uint8_t set, struct flask_adaptive_step *out) {
    if (set >= FLASK_ADAPTIVE_SETS || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = cfg.fallback[set]; }
    return 0;
}

int flask_adaptive_fallback_set(uint8_t set, const struct flask_adaptive_step *in) {
    if (set >= FLASK_ADAPTIVE_SETS || in == NULL) {
        return -EINVAL;
    }

    struct flask_adaptive_step out = *in;

    step_normalize(&out);
    K_SPINLOCK(&cfg_lock) {
        cfg.fallback[set] = out;
        fb_dirty |= BIT(set);
    }
    return 0;
}

/* --- persistence (settings subtree "flask/adaptive") --- */

struct flask_adaptive_saved_cfg {
    uint8_t version;
    uint8_t enabled;
} __packed;

#define AK_SETTINGS_VERSION 1

int flask_adaptive_save(void) {
    struct flask_adaptive_saved_cfg saved = {.version = AK_SETTINGS_VERSION};
    uint64_t pending_r, saved_r;
    uint32_t pending_f, saved_f;
    bool write_cfg;

    K_SPINLOCK(&cfg_lock) {
        saved.enabled = cfg.enabled ? 1 : 0;
        pending_r = rules_dirty;
        saved_r = rules_saved;
        pending_f = fb_dirty;
        saved_f = fb_saved;
        write_cfg = cfg_dirty || !cfg_saved;
        /* Claimed: re-marked on failure below; an edit landing mid-save
         * re-sets its bit and rides the next save. */
        rules_dirty = 0;
        fb_dirty = 0;
        cfg_dirty = false;
    }

    if (write_cfg) {
        int err = settings_save_one("flask/adaptive/cfg", &saved, sizeof(saved));

        if (err) {
            LOG_ERR("flask/adaptive/cfg settings save failed: %d", err);
            K_SPINLOCK(&cfg_lock) {
                cfg_dirty = true;
                rules_dirty |= pending_r;
                fb_dirty |= pending_f;
            }
            return err;
        }
        K_SPINLOCK(&cfg_lock) { cfg_saved = true; }
    }

    while (pending_r) {
        int i = __builtin_ctzll(pending_r);
        struct flask_adaptive_rule r;
        char key[28];
        int err = 0;

        /* One rule at a time: a whole-table copy would double the RAM. */
        K_SPINLOCK(&cfg_lock) { r = cfg.rules[i]; }

        bool empty = rule_is_empty(&r);
        bool on_flash = (saved_r & BIT64(i)) != 0;
        /* A deleted DEFAULT persists as an explicit all-zero entry: plain
         * settings_delete would resurrect the compiled seed next boot. */
        bool tombstone = empty && (rules_defaulted & BIT64(i)) != 0;

        snprintf(key, sizeof(key), "flask/adaptive/r%d", i);
        if (tombstone || !empty) {
            err = settings_save_one(key, &r, sizeof(r));
        } else if (on_flash) {
            err = settings_delete(key);
        } /* empty + never saved + not defaulted: nothing stored, nothing to delete */
        if (err) {
            LOG_ERR("%s settings save failed: %d", key, err);
            K_SPINLOCK(&cfg_lock) {
                rules_dirty |= pending_r;
                fb_dirty |= pending_f;
            }
            return err;
        }
        K_SPINLOCK(&cfg_lock) {
            if (empty && !tombstone) {
                rules_saved &= ~BIT64(i);
            } else {
                rules_saved |= BIT64(i);
            }
        }
        pending_r &= ~BIT64(i);
    }

    while (pending_f) {
        int i = __builtin_ctz(pending_f);
        struct flask_adaptive_step f;
        char key[28];
        int err = 0;

        K_SPINLOCK(&cfg_lock) { f = cfg.fallback[i]; }

        bool empty = f.action == FLASK_AK_OUT_NONE;
        bool on_flash = (saved_f & BIT(i)) != 0;
        bool tombstone = empty && (fb_defaulted & BIT(i)) != 0;

        snprintf(key, sizeof(key), "flask/adaptive/f%d", i);
        if (tombstone || !empty) {
            err = settings_save_one(key, &f, sizeof(f));
        } else if (on_flash) {
            err = settings_delete(key);
        }
        if (err) {
            LOG_ERR("%s settings save failed: %d", key, err);
            K_SPINLOCK(&cfg_lock) { fb_dirty |= pending_f; }
            return err;
        }
        K_SPINLOCK(&cfg_lock) {
            if (empty && !tombstone) {
                fb_saved &= ~BIT(i);
            } else {
                fb_saved |= BIT(i);
            }
        }
        pending_f &= ~BIT(i);
    }
    return 0;
}

int flask_adaptive_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                                    void *cb_arg) {
    if (sub == NULL) {
        return 0;
    }

    if (strcmp(sub, "cfg") == 0) {
        struct flask_adaptive_saved_cfg saved;

        if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            LOG_WRN("flask/adaptive/cfg unreadable (len %d)", (int)len);
            return 0;
        }
        if (saved.version != AK_SETTINGS_VERSION) {
            LOG_WRN("flask/adaptive/cfg version %d ignored", saved.version);
            return 0;
        }
        K_SPINLOCK(&cfg_lock) {
            cfg.enabled = saved.enabled != 0;
            cfg_saved = true;
            cfg_dirty = false;
        }
        return 0;
    }

    if ((sub[0] == 'r' || sub[0] == 'f') && sub[1] >= '0' && sub[1] <= '9') {
        bool is_rule = sub[0] == 'r';
        int idx = atoi(&sub[1]);

        if (idx < 0 || idx >= (is_rule ? FLASK_ADAPTIVE_RULES : FLASK_ADAPTIVE_SETS)) {
            LOG_WRN("flask/adaptive/%s ignored (bad index)", sub);
            return 0;
        }
        if (is_rule) {
            struct flask_adaptive_rule r;

            if (len != sizeof(r)) {
                /* STEPS changed across a reflash: dropped, defaults reseed. */
                LOG_WRN("flask/adaptive/%s ignored (len %d)", sub, (int)len);
                return 0;
            }
            if (read_cb(cb_arg, &r, sizeof(r)) < 0) {
                return -EIO;
            }
            if (r.set >= FLASK_ADAPTIVE_SETS) {
                LOG_WRN("flask/adaptive/%s ignored (set %d)", sub, r.set);
                return 0;
            }
            r.trigger = norm_trigger(r.trigger);
            r.max_idle_ms = MIN(r.max_idle_ms, FLASK_AK_IDLE_MAX_MS);
            r.flags &= FLASK_AK_FLAG_EXACT;
            for (int s = 0; s < FLASK_ADAPTIVE_STEPS; s++) {
                if (r.steps[s].action > FLASK_AK_OUT_MAX) {
                    r.steps[s].action = FLASK_AK_OUT_NONE;
                }
            }
            K_SPINLOCK(&cfg_lock) {
                cfg.rules[idx] = r;
                /* Restored = saved, INCLUDING all-zero tombstones: that is
                 * what keeps defaults_commit off a deleted default. */
                rules_saved |= BIT64(idx);
                rules_dirty &= ~BIT64(idx);
            }
        } else {
            struct flask_adaptive_step f;

            if (len != sizeof(f)) {
                LOG_WRN("flask/adaptive/%s ignored (len %d)", sub, (int)len);
                return 0;
            }
            if (read_cb(cb_arg, &f, sizeof(f)) < 0) {
                return -EIO;
            }
            if (f.action > FLASK_AK_OUT_MAX) {
                f.action = FLASK_AK_OUT_NONE;
            }
            K_SPINLOCK(&cfg_lock) {
                cfg.fallback[idx] = f;
                fb_saved |= BIT(idx);
                fb_dirty &= ~BIT(idx);
            }
        }
        return 0;
    }
    return -ENOENT;
}

/* --- compiled defaults -> live table --- */

#if DT_HAS_COMPAT_STATUS_OKAY(flask_adaptive_defaults)

/* 0 = ok (s filled), 1 = the behavior has no local id yet. */
static int binding_to_step(const struct zmk_behavior_binding *b, struct flask_adaptive_step *s) {
    zmk_behavior_local_id_t id;

    memset(s, 0, sizeof(*s));
    if (AK_KP_NAME != NULL && strcmp(b->behavior_dev, AK_KP_NAME) == 0) {
        s->action = FLASK_AK_OUT_USAGE;
        s->param1 = b->param1;
        return 0;
    }
    id = zmk_behavior_get_local_id(b->behavior_dev);
    if (id == 0 || id == UINT16_MAX) {
        /* SETTINGS_TABLE local ids are assigned in zmk's own settings
         * commit, which can run AFTER ours on a fresh device. */
        return 1;
    }
    s->action = FLASK_AK_OUT_BEHAVIOR;
    s->behavior_id = id;
    s->param1 = b->param1;
    s->param2 = b->param2;
    return 0;
}

int flask_adaptive_defaults_commit(void) {
    int applied = 0;
    int unresolved = 0;
    int idx = 0;

    for (int c = 0; c < (int)ARRAY_SIZE(fad_children); c++) {
        const struct fad_def_child *d = &fad_children[c];
        struct flask_adaptive_step steps[FLASK_ADAPTIVE_STEPS];
        int bad = 0;

        memset(steps, 0, sizeof(steps));
        for (int s = 0; s < d->n_bind && s < FLASK_ADAPTIVE_STEPS; s++) {
            bad += binding_to_step(&d->b[s], &steps[s]);
        }
        if (bad) {
            unresolved += bad;
            idx += d->n_trig; /* keep later children on their own indexes */
            continue;
        }

        for (int t = 0; t < d->n_trig; t++, idx++) {
            struct flask_adaptive_rule r;
            bool fill = false;

            if (idx >= FLASK_ADAPTIVE_RULES) {
                break;
            }
            memset(&r, 0, sizeof(r));
            r.set = d->set;
            r.trigger = norm_trigger(d->trig[t]);
            r.max_idle_ms = d->max_idle < 0 ? 0 : MIN((uint32_t)d->max_idle, FLASK_AK_IDLE_MAX_MS);
            r.flags = d->flags;
            memcpy(r.steps, steps, sizeof(steps));

            K_SPINLOCK(&cfg_lock) {
                /* Restored entries, tombstones included, and indexes this
                 * pass already seeded win over the seed. */
                if (((rules_saved | rules_seeded) & BIT64(idx)) == 0) {
                    fill = rule_is_empty(&cfg.rules[idx]);
                    if (fill) {
                        cfg.rules[idx] = r;
                    }
                }
                rules_seeded |= BIT64(idx);
            }
            if (fill) {
                applied++;
            }
        }
    }

    for (int s = 0; s < (int)ARRAY_SIZE(fad_fallbacks) && s < FLASK_ADAPTIVE_SETS; s++) {
        struct flask_adaptive_step f;
        bool fill = false;

        if (binding_to_step(&fad_fallbacks[s], &f) != 0) {
            unresolved++;
            continue;
        }
        K_SPINLOCK(&cfg_lock) {
            if (((fb_saved | fb_seeded) & BIT(s)) == 0) {
                fill = cfg.fallback[s].action == FLASK_AK_OUT_NONE;
                if (fill) {
                    cfg.fallback[s] = f;
                }
            }
            fb_seeded |= BIT(s);
        }
        if (fill) {
            applied++;
        }
    }
    LOG_INF("flask_adaptive: %d compiled defaults applied (%d pending ids)", applied, unresolved);
    return unresolved;
}

#else /* no defaults node in the keymap */

int flask_adaptive_defaults_commit(void) { return 0; }

#endif
