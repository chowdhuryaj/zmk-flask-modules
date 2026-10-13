#pragma once
#include <stdbool.h>
#include <stdint.h>
static inline bool is_mod(uint16_t page, uint32_t kc) { return page == 0x07 && kc >= 0xE0 && kc <= 0xE7; }
