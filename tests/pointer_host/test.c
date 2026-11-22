/*
 * Host check for flask_scroll's vertical priority (h_bias_pct) and the
 * flask_smooth EMA, compiled unmodified against stub/. Run: make
 *
 * SPDX-License-Identifier: MIT
 */
#include "../../src/input_processor_flask_scroll.c"
#undef DT_DRV_COMPAT
#include "../../src/input_processor_flask_smooth.c"

#include <assert.h>
#include <stdio.h>

int64_t stub_now = 1000;

static struct flask_scroll_data sd;
static struct device sdev = {.data = &sd};

static void scroll_reset(uint16_t bias) {
    memset(&sd, 0, sizeof(sd));
    sd.live = (struct flask_scroll_params){.divisor_x = 32, .divisor_y = 16, .interval_ms = 16,
                                           .max_notches = 3, .h_bias_pct = bias};
}

/* One BLE report: X then Y, 8 ms apart from the last. */
static void report(int dx, int dy) {
    struct input_event ex = {.type = INPUT_EV_REL, .code = INPUT_REL_X, .value = dx};
    struct input_event ey = {.type = INPUT_EV_REL, .code = INPUT_REL_Y, .value = dy};

    stub_now += 8;
    if (dx) {
        flask_scroll_handle_event(&sdev, &ex, 0, 0, NULL);
    }
    if (dy) {
        flask_scroll_handle_event(&sdev, &ey, 0, 0, NULL);
    }
}

static struct flask_smooth_data md;
static struct device mdev = {.data = &md};

static int smooth(int v) {
    struct input_event e = {.type = INPUT_EV_REL, .code = INPUT_REL_X, .value = v};

    stub_now += 8;
    flask_smooth_handle_event(&mdev, &e, 0, 0, NULL);
    return e.value;
}

int main(void) {
    /* Off: both axes scroll together (pre-v24 behavior). */
    scroll_reset(0);
    for (int i = 0; i < 40; i++) {
        report(4, 8);
    }
    assert(sd.accum_h == 160 / 32 && sd.accum_v == 320 / 16);

    /* Vertical roll with ~27 degrees of sideways drift: no horizontal notch. */
    scroll_reset(300);
    for (int i = 0; i < 200; i++) {
        report(4, 8);
    }
    assert(sd.accum_h == 0 && sd.accum_v == 1600 / 16);

    /* Clearly sideways: horizontal only, and within a couple of notches. */
    scroll_reset(300);
    for (int i = 0; i < 20; i++) {
        report(10, 1);
    }
    assert(sd.accum_h >= 4 && sd.accum_v == 0);

    /* Turning sideways mid-gesture wins without a pause. */
    scroll_reset(300);
    for (int i = 0; i < 50; i++) {
        report(0, 8);
    }
    int32_t v = sd.accum_v;
    for (int i = 0; i < 40; i++) {
        report(10, 1);
    }
    assert(sd.accum_h >= 8 && sd.accum_v == v);

    /* Smoothing: factor 100 is raw. */
    md.live = (struct flask_smooth_params){.enabled = true, .factor_x100 = 100, .timeout_ms = 200};
    assert(smooth(7) == 7 && smooth(-3) == -3);

    /* Factor 40: a step ramps up, then settles at the input. */
    memset(md.axis, 0, sizeof(md.axis));
    md.live.factor_x100 = 40;
    stub_now += 1000;
    int first = smooth(10);
    assert(first == 4);
    int last = 0;
    for (int i = 0; i < 30; i++) {
        last = smooth(10);
    }
    assert(last == 10);

    /* Slow creep (1 count/event) survives: carry, no quantizing to zero. */
    stub_now += 1000;
    int sum = 0;
    for (int i = 0; i < 50; i++) {
        sum += smooth(1);
    }
    assert(sum >= 48 && sum <= 50);

    /* Disabled: untouched. */
    md.live.enabled = false;
    assert(smooth(5) == 5);

    printf("pointer_host: ok\n");
    return 0;
}
