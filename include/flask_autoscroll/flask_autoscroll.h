/*
 * Runtime API for the flask_autoscroll input processor.
 *
 * Port of Flask (QMK) autoscroll — Ben White's radiology autoscroller
 * (stepped speeds, 1000ms..25ms per wheel notch) plus the Contour Shuttle
 * jog model (ball deflection = speed). Two mutually exclusive modes:
 *
 *  - STEPPED: flask_autoscroll_step(+1/-1) moves a signed speed level
 *    (-9..9, 0 = stopped, stepping through zero stops). Each |level| picks
 *    a tick interval from the canonical table {1000,500,200,100,67,50,40,
 *    33,25} ms, scaled by speed_scale_x100. The ball keeps scrolling/moving
 *    normally in stepped mode.
 *  - JOG: flask_autoscroll_jog_toggle(). Ball vertical travel accumulates
 *    into a deflection (clamped to ±(deadzone+range)); scroll direction and
 *    speed follow it continuously with a deadzone around center. Ball
 *    motion is swallowed while jogging. Toggle again to exit and reset.
 *
 * Any other key press stops either mode when stop_on_key is set (same
 * auto-exit convention as the QMK firmwares; keycode events only — mouse
 * clicks don't raise keycode_state_changed and won't stop it).
 *
 * Wheel ticks are emitted as REL_WHEEL from the processor's own DT node —
 * attach a second zmk,input-listener to route them onward, same pattern as
 * flask_scroll.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct flask_autoscroll_params {
    uint16_t speed_scale_x100; /* 100 = table as-is; clamped 25..400 */
    uint16_t jog_deadzone;     /* counts ignored around center; 0..200 */
    uint16_t jog_range;        /* counts past deadzone to full speed; 50..2000 */
    bool inverted;             /* flip scroll direction */
    bool stop_on_key;          /* any other key press stops autoscroll */
};

/* All return -ENODEV before the processor instance initializes. */
int flask_autoscroll_params_get(struct flask_autoscroll_params *out);
int flask_autoscroll_params_set(const struct flask_autoscroll_params *in);

/* Control (behavior / protocol entry points). Safe to call when idle. */
void flask_autoscroll_step(int8_t direction); /* +1 = up, -1 = down */
void flask_autoscroll_jog_toggle(void);
void flask_autoscroll_stop(void);

/* Live state for the tuning protocol: signed stepped level (-9..9), or
 * 100 while jogging, 0 idle — same wire values the QMK families report. */
int16_t flask_autoscroll_live_state(void);
