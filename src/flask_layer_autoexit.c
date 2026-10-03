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

/* By name: Studio-set bindings carry the registry's name string, compiled
 * ones the DEVICE_DT_NAME literal. No &trans node compiled -> nothing can be
 * &trans (Studio only assigns compiled behaviors) -> feature inert. */
static bool is_trans(const struct zmk_behavior_binding *b) {
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_transparent)
    return b && b->behavior_dev &&
           strcmp(b->behavior_dev, DEVICE_DT_NAME(DT_INST(0, zmk_behavior_transparent))) == 0;
#else
    return false;
#endif
}

/* Called from flask_layer_autoexit_press.c on every key press that reaches
 * the keymap. Walks active layers top-down: each latched layer whose
 * binding here is &trans is marked for exit; the walk stops at the first
 * non-&trans binding, held or locked layer, or the default layer. */
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
    if (!exits) {
        return;
    }
    /* Toggle-back guard: the classic unlatch key (&tog L, or smart_layer L L,
     * under a &trans on L) would otherwise exit L here and then toggle it
     * straight back on. Skip the exit when the binding the press falls to
     * names an exiting layer in either param.
     * ponytail: param match, not behavior match; a small non-layer param
     * equal to an exiting layer id (&mkp MB1 vs layer 1, &bt BT_SEL n)
     * only costs a missed exit. Match behavior names if that bites. */
    if (resolver && ((resolver->param1 < 32 && (exits & BIT(resolver->param1))) ||
                     (resolver->param2 < 32 && (exits & BIT(resolver->param2))))) {
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
