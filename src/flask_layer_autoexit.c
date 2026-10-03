/*
 * flask_layer_autoexit — a LATCHED layer turns off when a key that is
 * &trans on it is pressed; the press then falls through exactly like
 * &trans (same output, but the layer is gone).
 *
 * Latched vs held: when a layer turns on, the physical keys down at that
 * moment are recorded. The layer counts as HELD while any of them stays
 * down, LATCHED once all are up. So &mo / layer-tap holds / combo holds /
 * conditional layers (their source keys are down) never exit, and a &tog /
 * &to / smart_layer tap (its key is up by the time the user presses the
 * next key) does. Locked layers (zmk-auto-layer's &num_word activates
 * with locking=true) are left to their owner.
 *
 * Exempt presses: only a press that falls through to something that types
 * or acts exits. If the binding it falls to is a modifier (&kp / sticky of
 * a pure modifier usage) or a layer / hold-tap key (&mo &tog &to &sl &skl,
 * any core or flask hold-tap incl. &lt / mod-taps / smart_layer / &flt_*,
 * &num_word), nothing exits.
 *
 * Re-held latched layer: a latched layer L whose own layer key (&mo L,
 * &lt L, smart_layer L, a layer combo for L) is pressed while L is already
 * on gets no layer event, so that key is added to L's holders here and L
 * counts as HELD until it comes up.
 *
 * Two listeners, two link positions (event subscriptions run in link
 * order, see CMakeLists.txt):
 *  - this file links BEFORE flask_combos: it must see raw key positions,
 *    including combo keys the combo engine swallows for the whole hold
 *    (thumb combo 32+33 -> Control would otherwise look latched).
 *  - flask_layer_autoexit_press.c links AFTER flask_holdtap: the exit
 *    decision runs on presses that actually reach the keymap (not combo
 *    keys, and hold-tap captured keys only once replayed after the
 *    decision), before the core keymap listener records the press layer
 *    state. Press and release therefore both resolve on the post-exit
 *    state, so nothing sticks.
 *
 * Bindings are read live (zmk_keymap_get_layer_binding_at_idx), so Studio
 * edits apply at once.
 *
 * Threading: position and layer events run on the system workqueue, the
 * same single context core combo.c / hold-tap assume; state is lock-free.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/keys.h>
#include <zmk/matrix.h>

#define WORDS DIV_ROUND_UP(ZMK_KEYMAP_LEN, 32)

static uint32_t down[WORDS];                             /* raw keys down now */
static uint32_t holders[ZMK_KEYMAP_LAYERS_LEN][WORDS]; /* keys down at layer-on, still down */

static bool held(zmk_keymap_layer_id_t layer) {
    for (size_t w = 0; w < WORDS; w++) {
        if (holders[layer][w]) {
            return true;
        }
    }
    return false;
}

/* Behaviors are matched by device name: Studio-set bindings carry the
 * registry's name string, compiled ones the DEVICE_DT_NAME literal, and
 * both are the same device->name. No &trans node compiled -> nothing can be
 * &trans (Studio only assigns compiled behaviors) -> feature inert. */
#define AE_NAME_IS(node) || strcmp(n, DEVICE_DT_NAME(node)) == 0

static bool is_trans(const struct zmk_behavior_binding *b) {
    const char *n = b ? b->behavior_dev : NULL;

    return n && (false DT_FOREACH_STATUS_OKAY(zmk_behavior_transparent, AE_NAME_IS));
}

static bool is_key_press(const char *n) {
    return false DT_FOREACH_STATUS_OKAY(zmk_behavior_key_press, AE_NAME_IS);
}

/* &sk/&skm wrap &kp (param1 = usage), &sl/&skl wrap &mo (param1 = layer). */
static bool is_sticky(const char *n) {
    return false DT_FOREACH_STATUS_OKAY(zmk_behavior_sticky_key, AE_NAME_IS);
}

/* Layer keys and every hold-tap (core and flask: &lt, mod-taps,
 * smart_layer, &fht_*, &flt_*). */
static bool is_layer_or_holdtap(const char *n) {
    /* clang-format off */
    return false
        DT_FOREACH_STATUS_OKAY(zmk_behavior_momentary_layer, AE_NAME_IS)
        DT_FOREACH_STATUS_OKAY(zmk_behavior_toggle_layer, AE_NAME_IS)
        DT_FOREACH_STATUS_OKAY(zmk_behavior_to_layer, AE_NAME_IS)
        DT_FOREACH_STATUS_OKAY(zmk_behavior_auto_layer, AE_NAME_IS)
        DT_FOREACH_STATUS_OKAY(zmk_behavior_hold_tap, AE_NAME_IS)
        DT_FOREACH_STATUS_OKAY(zmk_behavior_flask_hold_tap, AE_NAME_IS);
    /* clang-format on */
}

static bool is_mod_usage(uint32_t usage) {
    uint8_t page = ZMK_HID_USAGE_PAGE(usage);

    return is_mod(page ? page : HID_USAGE_KEY, ZMK_HID_USAGE_ID(usage));
}

/* A press falling to this binding does not exit latched layers. */
static bool is_exempt(const struct zmk_behavior_binding *b) {
    const char *n = b ? b->behavior_dev : NULL;

    if (!n) {
        return false;
    }
    if (is_layer_or_holdtap(n)) {
        return true;
    }
    if (is_sticky(n)) {
        return b->param1 < ZMK_KEYMAP_LAYERS_LEN || is_mod_usage(b->param1);
    }
    return is_key_press(n) && is_mod_usage(b->param1);
}

/* Press at `position` resolved to `b`: if b is a layer / hold-tap / sticky
 * key whose param1 names a layer that is already on, that layer counts as
 * held while the key stays down (no layer event fires for an already-on
 * layer, so the holders snapshot never saw this key). Only for keys still
 * physically down: a hold-tap replay can arrive after the release, and a
 * bit set then would never clear. Also called by flask_combos for a combo's
 * behavior output (position = first combo key).
 * ponytail: param1 as the layer; the hold half of a mod-tap carries a usage
 * (>= 0x70000) so it never matches. */
void flask_layer_autoexit_hold(const struct zmk_behavior_binding *b, uint32_t position) {
    if (!b || !b->behavior_dev || position >= ZMK_KEYMAP_LEN ||
        b->param1 >= ZMK_KEYMAP_LAYERS_LEN ||
        !(is_layer_or_holdtap(b->behavior_dev) || is_sticky(b->behavior_dev))) {
        return;
    }
    const zmk_keymap_layer_id_t l = b->param1;
    const uint32_t bit = BIT(position % 32);
    const int w = position / 32;

    if (l != zmk_keymap_layer_default() && zmk_keymap_layer_active(l) && (down[w] & bit)) {
        holders[l][w] |= bit;
    }
}

/* Called from flask_layer_autoexit_press.c on every key press that reaches
 * the keymap. Walks active layers top-down: each latched layer whose
 * binding here is &trans is marked for exit; the walk stops at the first
 * non-&trans binding, held or locked layer, or the default layer. The
 * marked layers exit unless the binding the press falls to is exempt. */
void flask_layer_autoexit_press(uint32_t position) {
    if (position >= ZMK_KEYMAP_LEN) {
        return;
    }
    const zmk_keymap_layer_id_t dflt = zmk_keymap_layer_default();
    const struct zmk_behavior_binding *resolver = NULL;
    uint32_t exits = 0;
    bool walking = true;

    /* Priority = INDEX (Studio layer reordering); everything else (state,
     * bindings, events, holders, &tog params) is by layer ID. Same walk and
     * stop-at-default as core zmk_keymap_position_state_changed. */
    for (int idx = ZMK_KEYMAP_LAYERS_LEN - 1; idx >= 0; idx--) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(idx);
        if (id >= ZMK_KEYMAP_LAYERS_LEN || !zmk_keymap_layer_active(id)) {
            continue;
        }
        const struct zmk_behavior_binding *b = zmk_keymap_get_layer_binding_at_idx(id, position);
        bool tr = is_trans(b);
        if (walking && tr && id != dflt && !zmk_keymap_layer_locked(id) && !held(id)) {
            exits |= BIT(id);
            continue;
        }
        walking = false;
        if (!tr) {
            resolver = b;
            break;
        }
        if (id == dflt) {
            break;
        }
    }
    /* Exempt also covers the unlatch key (&tog L / smart_layer L L under a
     * &trans on L): it must not exit L and then toggle it straight back on. */
    if (is_exempt(resolver)) {
        flask_layer_autoexit_hold(resolver, position);
        return;
    }
    for (int id = 0; id < ZMK_KEYMAP_LAYERS_LEN; id++) {
        if (exits & BIT(id)) {
            zmk_keymap_layer_deactivate(id, false);
        }
    }
}

static int autoexit_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        if (pos->position < ZMK_KEYMAP_LEN) {
            uint32_t bit = BIT(pos->position % 32);
            int w = pos->position / 32;
            if (pos->state) {
                down[w] |= bit;
            } else {
                down[w] &= ~bit;
                for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
                    holders[l][w] &= ~bit;
                }
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_layer_state_changed *ls = as_zmk_layer_state_changed(eh);
    if (ls && ls->layer < ZMK_KEYMAP_LAYERS_LEN) {
        if (ls->state) {
            memcpy(holders[ls->layer], down, sizeof(down));
        } else {
            memset(holders[ls->layer], 0, sizeof(down));
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(flask_layer_autoexit, autoexit_listener);
ZMK_SUBSCRIPTION(flask_layer_autoexit, zmk_position_state_changed);
ZMK_SUBSCRIPTION(flask_layer_autoexit, zmk_layer_state_changed);
