/*
 * Runtime API for flask_ballswap — live trackball ROLE swap (proto channel
 * 0x1B). The Imprint's balls are role-fixed by their listener chains
 * (left = scroll, right = cursor); this module swaps the roles at runtime
 * without rebuilding: an input processor at the front of each physical
 * chain swallows motion while a swap is in effect and re-emits it from its
 * own node, where a second listener carries the OTHER role's chain.
 *
 * Two swap sources compose by XOR:
 *   - the persisted BASE state (toggle key / app toggle; survives power
 *     cycle AND reflashing — Zephyr settings live outside the app image)
 *   - a MOMENTARY hold (held-key count; never persisted)
 *
 * Central-only on splits (both listeners run on the central).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>

/** Persisted base state. */
bool flask_ballswap_swapped(void);
void flask_ballswap_set_swapped(bool on);

/** Effective state = base XOR (momentary holds > 0) — what the processors
 * actually apply. */
bool flask_ballswap_effective(void);

/** Momentary hold (the &bswap 1 key): press/release pairs nest. */
void flask_ballswap_momentary_press(void);
void flask_ballswap_momentary_release(void);

/** Persist ("flask/ballswap"). The toggle key saves on every flip; the
 * protocol SAVE command lands here too. */
int flask_ballswap_save(void);
int flask_ballswap_settings_restore(size_t len, settings_read_cb read_cb, void *cb_arg);
