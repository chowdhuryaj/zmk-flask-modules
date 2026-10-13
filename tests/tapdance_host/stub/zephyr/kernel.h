/* Host stub: the slice of Zephyr flask_tapdance.c uses. Delayable work is a
 * deadline the test fires by hand (td_advance in test.c). */
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#define __packed __attribute__((packed))
#define BUILD_ASSERT(c, m) _Static_assert(c, m)
#define ARG_UNUSED(x) (void)(x)
#define CONTAINER_OF(p, T, f) ((T *)((char *)(p) - offsetof(T, f)))
#define SYS_INIT(fn, ...) int sys_init_##fn(void) { return fn(); }

typedef struct { int64_t ms; } k_timeout_t;
#define K_MSEC(m) ((k_timeout_t){(m)})
#define K_NO_WAIT K_MSEC(0)

struct k_work;
typedef void (*k_work_handler_t)(struct k_work *);
struct k_work { k_work_handler_t handler; };
struct k_work_delayable { struct k_work work; bool pending; int64_t due; };

extern int64_t stub_now;
static inline int64_t k_uptime_get(void) { return stub_now; }
static inline void k_work_init_delayable(struct k_work_delayable *w, k_work_handler_t h) {
    w->work.handler = h;
    w->pending = false;
}
static inline int k_work_reschedule(struct k_work_delayable *w, k_timeout_t t) {
    w->pending = true;
    w->due = stub_now + t.ms;
    return 0;
}
static inline int k_work_cancel_delayable(struct k_work_delayable *w) {
    w->pending = false;
    return 0;
}
static inline struct k_work_delayable *k_work_delayable_from_work(struct k_work *w) {
    return CONTAINER_OF(w, struct k_work_delayable, work);
}
