#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <zmk/event_manager.h>
struct zmk_position_state_changed {
    zmk_event_t header;
    uint32_t position;
    bool state;
    int64_t timestamp;
};
static inline struct zmk_position_state_changed *as_zmk_position_state_changed(const zmk_event_t *e) {
    return e->type == STUB_EV_POSITION ? (struct zmk_position_state_changed *)e : 0;
}
