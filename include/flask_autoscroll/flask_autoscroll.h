/*
 * Runtime API for the flask_autoscroll input processor.
 *
 * Port of Flask (QMK) autoscroll — Ben White's radiology autoscroller
 * (stepped speeds, 1000ms..25ms per wheel notch) plus a jog mode. Two
 * mutually exclusive modes:
 *
 *  - STEPPED: flask_autoscroll_step(+1/-1) moves a signed speed level
 *    (-9..9, 0 = stopped, stepping through zero stops). Each |level| picks
 *    a tick interval from the canonical table {1000,500,200,100,67,50,40,
 *    33,25} ms, scaled by speed_scale_x100. The ball keeps scrolling/moving
 *    normally in stepped mode.
 *  - JOG: flask_autoscroll_jog_toggle(). REL_Y winds a velocity (clamped to
 *    ±(deadzone+range)) that bleeds toward zero each notch, so scroll speed
 *    tracks how hard the ball is being rolled and a still ball coasts to a
 *    stop; direction follows the roll. (A trackball has no spring return —
 *    a held-position model would pin at the clamp and scroll one way at one
 *    speed.) Ball motion is swallowed while jogging; toggle again to exit.
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
    uint16_t jog_deadzone;     /* velocity below this = no scroll; 0..200 */
    uint16_t jog_range;        /* velocity past deadzone to full speed; 50..2000 */
    bool inverted;             /* flip scroll direction */
    bool stop_on_key;          /* any other key press stops autoscroll */
};

/* Jog velocity bled off per emitted notch (self-centering). Higher = the
 * ball must be rolled harder to sustain speed and coasts to a stop sooner.
 * Compile-time for now; the wire tunables are deadzone/range/scale. */
#ifndef AUTOSCROLL_JOG_DRAIN
#    define AUTOSCROLL_JOG_DRAIN 6
#endif

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
