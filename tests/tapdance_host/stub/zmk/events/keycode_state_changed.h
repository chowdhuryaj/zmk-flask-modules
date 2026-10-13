#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <zmk/event_manager.h>
struct zmk_keycode_state_changed {
    zmk_event_t header;
    uint32_t keycode;
    bool state;
    int64_t timestamp;
};
static inline struct zmk_keycode_state_changed *as_zmk_keycode_state_changed(const zmk_event_t *e) {
    return e->type == STUB_EV_KEYCODE ? (struct zmk_keycode_state_changed *)e : 0;
}
void raise_zmk_keycode_state_changed_from_encoded(uint32_t encoded, bool pressed, int64_t ts);
