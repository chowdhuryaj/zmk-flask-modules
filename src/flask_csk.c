/*
 * flask_csk — runtime custom shift keys (see include/flask_csk/flask_csk.h).
 *
 * The hook rides zmk_keycode_state_changed BEFORE core's HID listener
 * (module sources link before app sources), mutating the event in place:
 * on a shifted press of a slot's base usage, the event becomes the
 * shifted usage (its modifier bits land in implicit_modifiers) and the
 * physical Shift is masked out of the report via the mod-morph mechanism
 * (zmk_hid_masked_modifiers_set). The RELEASE arrives carrying the
 * ORIGINAL usage — whatever pressed it releases the same code — so an
 * active-override table maps it back to the replacement, keeping HID
 * press/release paired even when Shift lifts first.
 *
 * The masked-modifier register is a single global (mod-morph shares this
 * limit): it is set while at least one override is live and cleared when
 * the last one releases.
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

#include <dt-bindings/zmk/modifiers.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

#include <flask_csk/flask_csk.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define CSK_SHIFT_MASK (MOD_LSFT | MOD_RSFT)

/* ZMK keymap encoding helpers (usage id 0-15, page 16-23, mods 24-31). */
#define ENC_ID(v) ((uint16_t)((v) & 0xFFFF))
#define ENC_PAGE(v) ((uint8_t)(((v) >> 16) & 0xFF))
#define ENC_MODS(v) ((uint8_t)(((v) >> 24) & 0xFF))

/* How many overrides can be held down at once. */
#define CSK_MAX_ACTIVE 4

static struct {
    bool enabled;
    struct flask_csk_slot slots[FLASK_CSK_SLOTS];
} cfg = {
    .enabled = true,
};

static struct k_spinlock cfg_lock;

/* Save bookkeeping (under cfg_lock) — dirty-slot discipline like combos:
 * SAVE touches flash only for slots that changed. */
static uint32_t slots_saved;
static uint32_t slots_dirty;
static bool cfg_saved;
static bool cfg_dirty;

BUILD_ASSERT(FLASK_CSK_SLOTS <= 32, "save bitmaps are u32");

/* Active overrides (event-delivery context only — single-threaded like the
 * combos capture state). */
static struct {
    bool live;
    uint16_t orig_id;
    uint8_t orig_page;
    uint32_t repl;
} actives[CSK_MAX_ACTIVE];

static int active_count;

static bool match_slot(uint8_t page, uint16_t id, uint32_t *repl) {
    bool hit = false;

    K_SPINLOCK(&cfg_lock) {
        if (!cfg.enabled) {
            K_SPINLOCK_BREAK;
        }
        for (int i = 0; i < FLASK_CSK_SLOTS; i++) {
            const struct flask_csk_slot *s = &cfg.slots[i];

            if (s->base == 0 || s->shifted == 0) {
                continue;
            }
            if (ENC_PAGE(s->base) == page && ENC_ID(s->base) == id) {
                *repl = s->shifted;
                hit = true;
                break;
            }
        }
    }
    return hit;
}

static void apply_replacement(struct zmk_keycode_state_changed *ev, uint32_t repl) {
    ev->usage_page = ENC_PAGE(repl);
    ev->keycode = ENC_ID(repl);
    ev->implicit_modifiers = ENC_MODS(repl);
}

static int csk_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        /* Press: only when a physical Shift is explicitly held. */
        uint32_t repl;

        if (!(zmk_hid_get_explicit_mods() & CSK_SHIFT_MASK) ||
            is_mod(ev->usage_page, ev->keycode) ||
            !match_slot(ev->usage_page, (uint16_t)ev->keycode, &repl)) {
            return ZMK_EV_EVENT_BUBBLE;
        }

        for (int i = 0; i < CSK_MAX_ACTIVE; i++) {
            if (actives[i].live) {
                continue;
            }
            actives[i].live = true;
            actives[i].orig_page = ev->usage_page;
            actives[i].orig_id = (uint16_t)ev->keycode;
            actives[i].repl = repl;
            if (active_count++ == 0) {
                zmk_hid_masked_modifiers_set(CSK_SHIFT_MASK);
            }
            apply_replacement(ev, repl);
            LOG_DBG("flask_csk: %02x/%04x -> %08x", actives[i].orig_page, actives[i].orig_id,
                    repl);
            return ZMK_EV_EVENT_BUBBLE;
        }
        return ZMK_EV_EVENT_BUBBLE; /* table full — let the plain key through */
    }

    /* Release: map an active override's ORIGINAL usage back to the
     * replacement so HID press/release stay paired (Shift may already be
     * up — state at press time decides, like QMK CSK). */
    for (int i = 0; i < CSK_MAX_ACTIVE; i++) {
        if (!actives[i].live || actives[i].orig_page != ev->usage_page ||
            actives[i].orig_id != (uint16_t)ev->keycode) {
            continue;
        }
        apply_replacement(ev, actives[i].repl);
        actives[i].live = false;
        if (--active_count == 0) {
            zmk_hid_masked_modifiers_clear();
        }
        break;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_csk, csk_listener);
ZMK_SUBSCRIPTION(flask_csk, zmk_keycode_state_changed);

/* --- runtime API (proto channel 0x16) --- */

bool flask_csk_enabled(void) {
    bool on;

    K_SPINLOCK(&cfg_lock) { on = cfg.enabled; }
    return on;
}

void flask_csk_set_enabled(bool on) {
    K_SPINLOCK(&cfg_lock) {
        cfg.enabled = on;
        cfg_dirty = true;
    }
}

uint8_t flask_csk_slot_count(void) { return FLASK_CSK_SLOTS; }

int flask_csk_slot_get(uint8_t idx, struct flask_csk_slot *out) {
    if (idx >= FLASK_CSK_SLOTS || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = cfg.slots[idx]; }
    return 0;
}

int flask_csk_slot_set(uint8_t idx, const struct flask_csk_slot *in) {
    if (idx >= FLASK_CSK_SLOTS || in == NULL) {
        return -EINVAL;
    }

    struct flask_csk_slot s = *in;

    /* A pair is live only when complete — half-filled drafts stay inert
     * but echo back as written. */
    K_SPINLOCK(&cfg_lock) {
        cfg.slots[idx] = s;
        slots_dirty |= BIT(idx);
    }
    return 0;
}

/* --- persistence (settings subtree "flask/csk") --- */

struct flask_csk_saved_cfg {
    uint8_t version;
    uint8_t enabled;
} __packed;

#define CSK_SETTINGS_VERSION 1

int flask_csk_save(void) {
    struct flask_csk_saved_cfg saved = {.version = CSK_SETTINGS_VERSION};
    struct flask_csk_slot slots[FLASK_CSK_SLOTS];
    uint32_t pending, saved_bits;
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
        int err = settings_save_one("flask/csk/cfg", &saved, sizeof(saved));

        if (err) {
            LOG_ERR("flask/csk/cfg settings save failed: %d", err);
            K_SPINLOCK(&cfg_lock) {
                cfg_dirty = true;
                slots_dirty |= pending;
            }
            return err;
        }
        K_SPINLOCK(&cfg_lock) { cfg_saved = true; }
    }

    while (pending) {
        int i = __builtin_ctz(pending);
        bool empty = slots[i].base == 0 && slots[i].shifted == 0;
        bool on_flash = (saved_bits & BIT(i)) != 0;
        char key[20];
        int err = 0;

        snprintf(key, sizeof(key), "flask/csk/s%d", i);
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
                slots_saved &= ~BIT(i);
            } else {
                slots_saved |= BIT(i);
            }
        }
        pending &= ~BIT(i);
    }
    return 0;
}

int flask_csk_settings_restore(const char *sub, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    if (sub == NULL) {
        return 0;
    }

    if (strcmp(sub, "cfg") == 0) {
        struct flask_csk_saved_cfg saved;

        if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) < 0) {
            LOG_WRN("flask/csk/cfg unreadable (len %d)", (int)len);
            return 0;
        }
        if (saved.version != CSK_SETTINGS_VERSION) {
            LOG_WRN("flask/csk/cfg version %d ignored", saved.version);
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
        struct flask_csk_slot s;

        if (idx < 0 || idx >= FLASK_CSK_SLOTS) {
            LOG_WRN("flask/csk/%s ignored (bad index)", sub);
            return 0;
        }
        if (len != sizeof(s)) {
            LOG_WRN("flask/csk/%s ignored (len %d)", sub, (int)len);
            return 0;
        }
        if (read_cb(cb_arg, &s, sizeof(s)) < 0) {
            return -EIO;
        }
        K_SPINLOCK(&cfg_lock) {
            cfg.slots[idx] = s;
            slots_saved |= BIT(idx);
            slots_dirty &= ~BIT(idx);
        }
        return 0;
    }
    return -ENOENT;
}
