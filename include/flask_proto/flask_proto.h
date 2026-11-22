/*
 * Hook for channels a board owns (e.g. Totem-ZMK's mouse host channel 0x2E,
 * docs/mouse-host.md there). flask_proto calls it for GET/SET on any channel
 * it does not handle itself; return true to send the frame back as the reply
 * (payload rewritten in place), false to echo 0xFF (unhandled). Runs in the
 * transport's delivery context (BT RX thread): no blocking, no flash writes.
 * SAVE is never routed here.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool flask_proto_ext_handle(uint8_t cmd, uint8_t channel, uint8_t value_id, uint8_t *payload,
                            size_t payload_len);
