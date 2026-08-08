#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "traffic_debug.h"

/**
 * Format a 32-bit address as an 8-digit uppercase hex string.
 */
const char* addr_hex(uint32_t addr, char* buf, size_t len);

/**
 * Serialize a traffic event's fields into an existing JSON object.
 *
 * Single source of truth for the wire shape, because the same event is emitted
 * from two places: the bramble.onTrafficEvent notification and the
 * bramble.getTrafficEvents reply. They must not drift, since a consumer
 * backfilling over the reply and then following the stream has to see one
 * schema.
 *
 * src_addr is omitted rather than written as "00000000" when the frame carried
 * no origin address, so a consumer plotting RSSI per peer cannot mistake "not
 * carried by this packet type" for a real address.
 */
void traffic_event_add_json(cJSON* obj, const traffic_event_t* evt);
