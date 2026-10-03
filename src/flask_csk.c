/*
 * flask_csk — runtime custom shift keys (see include/flask_csk/flask_csk.h).
 *
 * The hook rides zmk_keycode_state_changed BEFORE core's HID listener
 * (module sources link before app sources), mutating the event in place:
 * when the held explicit mods (folded left|right) EQUAL a slot's trigger
 * set and the press is the slot's base usage, the event becomes the
 * replacement usage (its modifier bits land in implicit_modifiers) and
 * the trigger's mods (both sides) are masked out of the report via the
 * mod-morph mechanism (zmk_hid_masked_modifiers_set) unless the slot
 * keeps them. Explicit mods include hold-tap holds and non-lazy sticky
 * keys: both press a real modifier keycode before the morphed key's event
 * arrives. A `lazy` sticky key presses its mod only when the next key
 * arrives (its listener links after this one), so csk does not see it.
 * The RELEASE arrives carrying the ORIGINAL usage — whatever pressed it
 * releases the same code — so an active-override table maps it back to
 * the replacement, keeping HID press/release paired even when the mods
 * lift or change first (state at press time decides).
 *
 * OS-aware slots (flags bits 1-4, see flask_csk.h): an OS condition is
 * checked at press time only, so toggling the OS mid-hold cannot strand a
 * key (release goes through the active table). Specific slots are tried
 * before wildcards. Only trigger mods that are EXPLICITLY held are masked;
 * implicit trigger mods (bit4, e.g. &kp LG(V)) are dropped from the
 * event's own implicit set instead, so an implicit-only match never
 * touches the masked register.
 *
 * The masked-modifier register is a single global (mod-morph shares this
 * limit): it holds the union of the live overrides' masks, written only
 * when that union changes and cleared when it empties. A Shift-only table
 * therefore makes exactly the old calls (set on first press, clear on
 * last release).
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

/* OS mode comes from mctechnology17/zmk-switch-layout's public state API
 * (the index &sw_layout toggles and persists). Built without that module,
 * the mode reads FLASK_CSK_OS_NONE and OS-conditioned slots never fire. */
#if IS_ENABLED(CONFIG_ZMK_SWITCH_LAYOUT_STATE)
#include <zmk_switch_layout/state.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Side-agnostic 4-bit set (bit0 Ctrl, bit1 Shift, bit2 Alt, bit3 GUI) <->
 * ZMK mod flags (left = bits 0-3, right = bits 4-7, same order). */
#define CSK_FOLD(m) ((uint8_t)(((m) | ((m) >> 4)) & 0x0F))
#define CSK_SIDES(t) ((zmk_mod_flags_t)((t) | ((t) << 4)))

/* ZMK keymap encoding helpers (usage id 0-15, page 16-23, mods 24-31). */
#define ENC_ID(v) ((uint16_t)((v) & 0xFFFF))
#define ENC_PAGE(v) ((uint8_t)(((v) >> 16) & 0xFF))
#define ENC_MODS(v) ((uint8_t)(((v) >> 24) & 0xFF))

/* How many overrides can be held down at once. */
#define CSK_MAX_ACTIVE 4

/* Scalars live outside the table on purpose: one nonzero initializer would put the
 * whole table in .data (flash image + boot copy). The table itself is zero-init .bss. */
static bool cfg_enabled = true;
static struct {
    struct flask_csk_slot slots[FLASK_CSK_SLOTS];
} cfg;

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
    zmk_mod_flags_t mask;
    uint32_t repl;
} actives[CSK_MAX_ACTIVE];

static uint8_t slot_trigger(const struct flask_csk_slot *s) {
    return s->mods ? s->mods : FLASK_CSK_TRIGGER_SHIFT;
}

static bool slot_valid(const struct flask_csk_slot *s) {
    return !(s->mods & 0xF0) && !(s->flags & ~FLASK_CSK_FLAGS_VALID) &&
           (s->flags & FLASK_CSK_FLAG_OS_MASK) != FLASK_CSK_FLAG_OS_MASK;
}

/* A wildcard ignores both usages, so it is live (and saved) on its flag alone. */
static bool slot_empty(const struct flask_csk_slot *s) {
    return s->base == 0 && s->shifted == 0 && !(s->flags & FLASK_CSK_FLAG_WILD);
}

static bool slot_live(const struct flask_csk_slot *s) {
    return (s->flags & FLASK_CSK_FLAG_WILD) || (s->base != 0 && s->shifted != 0);
}

static bool os_ok(uint8_t flags, uint16_t os) {
    switch (flags & FLASK_CSK_FLAG_OS_MASK) {
    case FLASK_CSK_FLAG_OS_MAC:
        return os == FLASK_CSK_OS_MAC;
    case FLASK_CSK_FLAG_OS_PC:
        return os == FLASK_CSK_OS_PC;
    default:
        return true;
    }
}

uint16_t flask_csk_os_mode(void) {
#if IS_ENABLED(CONFIG_ZMK_SWITCH_LAYOUT_STATE)
    return zmk_switch_layout_get();
#else
    return FLASK_CSK_OS_NONE;
#endif
}

static zmk_mod_flags_t active_mask(void) {
    zmk_mod_flags_t m = 0;

    for (int i = 0; i < CSK_MAX_ACTIVE; i++) {
        if (actives[i].live) {
            m |= actives[i].mask;
        }
    }
    return m;
}

static void update_mask(zmk_mod_flags_t before) {
    zmk_mod_flags_t now = active_mask();

    if (now == before) {
        return;
    }
    if (now) {
        zmk_hid_masked_modifiers_set(now);
    } else {
        zmk_hid_masked_modifiers_clear();
    }
}

/* expl/impl = folded explicit / event-implicit mods. Pass 0 = specific
 * slots (exact set), pass 1 = wildcards (trigger subset of held). */
static bool match_slot(const struct zmk_keycode_state_changed *ev, uint8_t expl, uint8_t impl,
                       uint32_t *repl, zmk_mod_flags_t *mask) {
    uint8_t page = (uint8_t)ev->usage_page;
    uint16_t id = (uint16_t)ev->keycode;
    uint16_t os = flask_csk_os_mode();
    struct flask_csk_slot hit;
    bool found = false;

    K_SPINLOCK(&cfg_lock) {
        if (!cfg_enabled) {
            K_SPINLOCK_BREAK;
        }
        for (int pass = 0; pass < 2 && !found; pass++) {
            for (int i = 0; i < FLASK_CSK_SLOTS; i++) {
                const struct flask_csk_slot *s = &cfg.slots[i];
                bool wild = (s->flags & FLASK_CSK_FLAG_WILD) != 0;
                uint8_t trig = slot_trigger(s);
                uint8_t held = expl | ((s->flags & FLASK_CSK_FLAG_IMPLICIT) ? impl : 0);

                if (wild != pass || !slot_live(s) || !os_ok(s->flags, os)) {
                    continue;
                }
                if (wild ? (trig & held) == trig
                         : (trig == held && ENC_PAGE(s->base) == page && ENC_ID(s->base) == id)) {
                    hit = *s;
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        return false;
    }

    uint8_t trig = slot_trigger(&hit);
    bool keep = (hit.flags & FLASK_CSK_FLAG_KEEP) != 0;
    bool counts_impl = (hit.flags & FLASK_CSK_FLAG_IMPLICIT) != 0;

    /* Only explicitly held trigger mods sit in the register; for old slots
     * trig == expl, so this is the old CSK_SIDES(held). */
    *mask = keep ? 0 : CSK_SIDES(trig & expl);
    if (hit.flags & FLASK_CSK_FLAG_WILD) {
        /* Same key; implicit trigger mods dropped (unless keep), replacement mods added. */
        uint8_t strip = (counts_impl && !keep) ? CSK_SIDES(trig) : 0;
        uint8_t mods = (ev->implicit_modifiers & ~strip) | ENC_MODS(hit.shifted);

        *repl = ((uint32_t)mods << 24) | ((uint32_t)page << 16) | id;
    } else {
        /* Exact match: the event's implicit mods are all trigger mods
         * (bit4) or not counted (old behaviour: replaced). keep + bit4
         * keeps them, like explicit trigger mods under keep. */
        *repl = hit.shifted;
        if (counts_impl && keep) {
            *repl |= (uint32_t)ev->implicit_modifiers << 24;
        }
    }
    return true;
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
        /* Press: only when some modifier is held (explicitly, or implicitly
         * on the event for bit4 slots). */
        uint8_t expl = CSK_FOLD(zmk_hid_get_explicit_mods());
        uint8_t impl = CSK_FOLD(ev->implicit_modifiers);
        uint32_t repl;
        zmk_mod_flags_t mask;

        if (!(expl | impl) || is_mod(ev->usage_page, ev->keycode) ||
            !match_slot(ev, expl, impl, &repl, &mask)) {
            return ZMK_EV_EVENT_BUBBLE;
        }

        for (int i = 0; i < CSK_MAX_ACTIVE; i++) {
            if (actives[i].live) {
                continue;
            }
            zmk_mod_flags_t before = active_mask();

            actives[i].live = true;
            actives[i].orig_page = ev->usage_page;
            actives[i].orig_id = (uint16_t)ev->keycode;
            actives[i].mask = mask;
            actives[i].repl = repl;
            update_mask(before);
            apply_replacement(ev, repl);
            LOG_DBG("flask_csk: %02x/%04x -> %08x", actives[i].orig_page, actives[i].orig_id,
                    repl);
            return ZMK_EV_EVENT_BUBBLE;
        }
        return ZMK_EV_EVENT_BUBBLE; /* table full — let the plain key through */
    }

    /* Release: map an active override's ORIGINAL usage back to the
     * replacement so HID press/release stay paired (the mods may already
     * be up or changed — state at press time decides, like QMK CSK). */
    for (int i = 0; i < CSK_MAX_ACTIVE; i++) {
        if (!actives[i].live || actives[i].orig_page != ev->usage_page ||
            actives[i].orig_id != (uint16_t)ev->keycode) {
            continue;
        }
        zmk_mod_flags_t before = active_mask();

        apply_replacement(ev, actives[i].repl);
        actives[i].live = false;
        update_mask(before);
        break;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_csk, csk_listener);
ZMK_SUBSCRIPTION(flask_csk, zmk_keycode_state_changed);

/* --- runtime API (proto channel 0x16) --- */

bool flask_csk_enabled(void) {
    bool on;

    K_SPINLOCK(&cfg_lock) { on = cfg_enabled; }
    return on;
}

void flask_csk_set_enabled(bool on) {
    K_SPINLOCK(&cfg_lock) {
        cfg_enabled = on;
        cfg_dirty = true;
    }
}

uint8_t flask_csk_slot_count(void) { return FLASK_CSK_SLOTS; }

int flask_csk_slot_get(uint8_t idx, struct flask_csk_slot *out) {
    if (idx >= FLASK_CSK_SLOTS || out == NULL) {
        return -EINVAL;
    }
    K_SPINLOCK(&cfg_lock) { *out = cfg.slots[idx]; }
    out->mods = slot_trigger(out);
    return 0;
}

int flask_csk_slot_set(uint8_t idx, const struct flask_csk_slot *in) {
    if (idx >= FLASK_CSK_SLOTS || in == NULL) {
        return -EINVAL;
    }

    if (!slot_valid(in)) {
        return -EINVAL;
    }

    struct flask_csk_slot s = *in;

    /* A pair is live only when complete — half-filled drafts stay inert
     * but echo back as written (mods 0 echoes as Shift). */
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
    /* static: 32 slots = 320 B, too much for the 2 KB flask_save stack;
     * the save queue is single-flight (same as flask_combos). */
    static struct flask_csk_slot slots[FLASK_CSK_SLOTS];
    uint32_t pending, saved_bits;
    bool write_cfg;

    K_SPINLOCK(&cfg_lock) {
        saved.enabled = cfg_enabled ? 1 : 0;
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
        bool empty = slot_empty(&slots[i]);
        bool on_flash = (saved_bits & BIT(i)) != 0;
        char key[20];
        int err = 0;

        snprintf(key, sizeof(key), "flask/csk/s%d", i);
        if (empty && on_flash) {
            err = settings_delete(key);
        } else if (!empty) {
            /* Shift/no-flag slots keep the pre-mod-morph 8-byte blob, so a
             * downgraded image still loads them. */
            bool legacy = slot_trigger(&slots[i]) == FLASK_CSK_TRIGGER_SHIFT && !slots[i].flags;

            err = settings_save_one(key, &slots[i],
                                    legacy ? offsetof(struct flask_csk_slot, mods)
                                           : sizeof(slots[i]));
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
            cfg_enabled = saved.enabled != 0;
            cfg_saved = true;
            cfg_dirty = false;
        }
        return 0;
    }

    if (sub[0] == 's') {
        int idx = atoi(&sub[1]);
        struct flask_csk_slot s = {0};

        if (idx < 0 || idx >= FLASK_CSK_SLOTS) {
            LOG_WRN("flask/csk/%s ignored (bad index)", sub);
            return 0;
        }
        /* 8 bytes = pre-mod-morph entry: mods/flags stay 0 = Shift, masked. */
        if (len != sizeof(s) && len != offsetof(struct flask_csk_slot, mods)) {
            LOG_WRN("flask/csk/%s ignored (len %d)", sub, (int)len);
            return 0;
        }
        if (read_cb(cb_arg, &s, len) < 0) {
            return -EIO;
        }
        if (!slot_valid(&s)) {
            LOG_WRN("flask/csk/%s ignored (bad mods/flags)", sub);
            return 0;
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
