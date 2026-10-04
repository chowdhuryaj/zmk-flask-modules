/*
 * Runtime API for the flask_mousekeys input processor (Flask protocol
 * channel 0x2D, proto v20; start floor v21).
 *
 * ZMK's &mmv / &msc ramp (time-to-max-speed-ms, acceleration-exponent) is
 * const devicetree config, so mouse-key speed and acceleration could only
 * change by reflashing. The keymap sets the behaviors to emit their compiled
 * max speed constantly (time-to-max-speed-ms = <0>) and puts one of these
 * processors on each listener's BASE chain; the processor re-creates the
 * ramp and scales by a live speed percent:
 *
 *   frac = floor_pct/100 + (1 - floor_pct/100) * min(1, (t / ramp_ms)^exponent)
 *   out  = in * speed_pct/100 * frac
 *
 * floor_pct is the start speed: the share of full speed a movement begins
 * at, so a held key is not dead for the first few hundred ms. 0 reproduces
 * the plain ramp exactly; 100 disables the ramp.
 *
 * t = ms since the current movement started (first event after a quiet gap
 * on that instance). ramp_ms 0 or exponent 0 = no ramp (full speed at once).
 *
 * Two parameter sets: FLASK_MOUSEKEYS_POINTER (REL_X/REL_Y on the &mmv
 * listener) and FLASK_MOUSEKEYS_SCROLL (REL_WHEEL/REL_HWHEEL on &msc).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#define FLASK_MOUSEKEYS_POINTER 0
#define FLASK_MOUSEKEYS_SCROLL 1
#define FLASK_MOUSEKEYS_KINDS 2

#define FLASK_MOUSEKEYS_SPEED_MIN 10
#define FLASK_MOUSEKEYS_SPEED_MAX 400
#define FLASK_MOUSEKEYS_RAMP_MAX 3000
#define FLASK_MOUSEKEYS_EXP_MAX 3
#define FLASK_MOUSEKEYS_FLOOR_MAX 100

struct flask_mousekeys_params {
    uint16_t speed_pct; /* 10..400, percent of the compiled max speed */
    uint16_t ramp_ms;   /* 0..3000, time to full speed */
    uint16_t exponent;  /* 0 uniform, 1 linear, 2 quadratic, 3 cubic */
    uint16_t floor_pct; /* 0..100, start speed as percent of full speed (0 = none) */
};

/* kind = FLASK_MOUSEKEYS_POINTER / _SCROLL. -EINVAL on a bad kind or NULL.
 * set clamps every field. */
int flask_mousekeys_params_get(uint8_t kind, struct flask_mousekeys_params *out);
int flask_mousekeys_params_set(uint8_t kind, const struct flask_mousekeys_params *in);
