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
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/bluetooth/uuid.h>

#define FLASK_RGB_SVC_UUID                                                                         \
    BT_UUID_128_ENCODE(0xf1a5cb01, 0x52b6, 0x4e0b, 0x9fd2, 0x1e7e37c1a2f4)
#define FLASK_RGB_CHR_UUID                                                                         \
    BT_UUID_128_ENCODE(0xf1a5cb02, 0x52b6, 0x4e0b, 0x9fd2, 0x1e7e37c1a2f4)

#define FRGB_OP_LAYERS 0x01
#define FRGB_OP_LED 0x02
#define FRGB_OP_ENABLED 0x03
#define FRGB_OP_FILL 0x04
#define FRGB_OP_CHUNK 0x05

#define FRGB_CHUNK_MAX 4
