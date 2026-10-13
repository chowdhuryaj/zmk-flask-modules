#pragma once
/* One tagged struct per event type; the test calls the listener directly. */
typedef struct { int type; } zmk_event_t;
enum { STUB_EV_POSITION = 1, STUB_EV_KEYCODE = 2 };
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_LISTENER(n, cb) int (*const stub_listener_##n)(const zmk_event_t *) = cb;
#define ZMK_SUBSCRIPTION(n, ev)
