#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t zmk_behavior_local_id_t;
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1, param2; };
struct zmk_behavior_binding_event { int layer; uint32_t position; int64_t timestamp; };
zmk_behavior_local_id_t zmk_behavior_get_local_id(const char *name);
const char *zmk_behavior_find_behavior_name_from_local_id(zmk_behavior_local_id_t id);
int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *b,
                                struct zmk_behavior_binding_event ev, bool pressed);
