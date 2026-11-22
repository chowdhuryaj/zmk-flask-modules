/* Host stub: input events, plus the k_work_schedule flask_scroll arms. */
#pragma once
#include <zephyr/kernel.h>
#define INPUT_EV_KEY 0x01
#define INPUT_EV_REL 0x02
#define INPUT_REL_X 0x00
#define INPUT_REL_Y 0x01
#define INPUT_REL_HWHEEL 0x06
#define INPUT_REL_WHEEL 0x08
struct input_event { const struct device *dev; uint8_t sync; uint8_t type; uint16_t code; int32_t value; };
static inline int k_work_schedule(struct k_work_delayable *w, k_timeout_t t) { return k_work_reschedule(w, t); }
static inline int input_report_rel(const struct device *d, uint16_t c, int32_t v, bool s, k_timeout_t t) { return 0; }
