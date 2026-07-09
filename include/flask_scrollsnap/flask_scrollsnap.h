/*
 * Runtime API for the flask_scrollsnap input processor.
 *
 * Runtime-tunable port of kot149/zmk-scroll-snap (MIT): watches a window of
 * recent wheel events, decides the dominant axis from a threshold ratio, and
 * zeroes the minority axis ("snap"). Once a direction wins it can be locked
 * for a duration and/or a number of events ("axis lock"), so a slightly
 * diagonal ball roll scrolls straight.
 *
 * Exposed over Flask protocol channel 0x26; every knob the DT config had is
 * live-tunable. The threshold ratio rides the wire as a percent p (50..99):
 * an axis wins when its share of recent motion exceeds p% — p=50 snaps
 * instantly to whichever axis leads, p=99 barely snaps at all.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct flask_scrollsnap_params {
    bool enabled;
    uint8_t threshold_pct;      /* axis dominance to snap; clamped 50..99 */
    uint8_t samples;            /* window size in events; clamped 1..32 */
    uint16_t immediate_thresh;  /* motion sum that snaps before the window fills */
    uint16_t lock_ms;           /* keep the winning axis this long (0 = off) */
    uint16_t lock_events;       /* ...or for this many events (0 = off) */
    uint16_t idle_reset_ms;     /* pause that clears window + lock (0 = off) */
};

/* Both return -ENODEV before the processor instance initializes. */
int flask_scrollsnap_params_get(struct flask_scrollsnap_params *out);
int flask_scrollsnap_params_set(const struct flask_scrollsnap_params *in);
