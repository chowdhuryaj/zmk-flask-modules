/*
 * flask_rgb split-sync wire format — module-private.
 *
 * One custom GATT characteristic (write-without-response) hosted by the
 * PERIPHERAL half; the central discovers it on the existing split
 * connection and writes small frames. Same valdur/darknao insight: sync
 * layer STATE, not pixels — the map itself only crosses on edits and the
 * connect-time bulk resync.
 *
 * Frames (first byte = op):
 *   0x01 layers:  [op, bitmap LE u32]                  (5 B)
 *   0x02 led:     [op, layer, led LE u16, h, s, v]     (7 B)
 *   0x03 enabled: [op, 0|1]                            (2 B)
 *   0x04 fill:    [op, layer, h, s, v]                 (5 B)
 *   0x05 chunk:   [op, layer, start LE u16, n, n×hsv]  (5 + 3n B, n ≤ 4)
 *   0x06 effect:  [op, effect, speed, h, s, v, phase LE u16]  (8 B)
 *                 (phase re-anchors the peripheral's animation clock)
 *   0x07 overlay: [op, on, h, s, v, mask bytes]  (5 + ceil(total/8) B)
 *                 (reactive overlay — transient highlight above map+effect)
 *   0x08 bright:  [op, percent 0-100]                  (2 B)
 *                 (global brightness — scales every rendered pixel, v14)
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/bluetooth/uuid.h>
#include <zephyr/devicetree.h>

/* Whole-board LED count, visible to both split halves (same DT). Sizes the
 * overlay frame's bitmask. */
#define FRGB_SPLIT_TOTAL DT_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(flask_rgb), total_leds)
#define FRGB_OVERLAY_MASK_BYTES ((FRGB_SPLIT_TOTAL + 7) / 8)

#define FLASK_RGB_SVC_UUID                                                                         \
    BT_UUID_128_ENCODE(0xf1a5cb01, 0x52b6, 0x4e0b, 0x9fd2, 0x1e7e37c1a2f4)
#define FLASK_RGB_CHR_UUID                                                                         \
    BT_UUID_128_ENCODE(0xf1a5cb02, 0x52b6, 0x4e0b, 0x9fd2, 0x1e7e37c1a2f4)

#define FRGB_OP_LAYERS 0x01
#define FRGB_OP_LED 0x02
#define FRGB_OP_ENABLED 0x03
#define FRGB_OP_FILL 0x04
#define FRGB_OP_CHUNK 0x05
#define FRGB_OP_EFFECT 0x06
#define FRGB_OP_OVERLAY 0x07
#define FRGB_OP_BRIGHT 0x08
/* Idle blank timeout, seconds, LE16 at p[1..2] (v16). Both halves run their
 * OWN activity clock, so the peripheral needs the value — not a blank
 * command — or it would keep blanking at the compiled 30 s while the central
 * stayed lit. */
#define FRGB_OP_IDLE 0x09

#define FRGB_CHUNK_MAX 4
