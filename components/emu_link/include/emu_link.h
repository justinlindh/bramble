#ifndef BRAMBLE_EMU_LINK_H
#define BRAMBLE_EMU_LINK_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * emu_link: host-only JSON-lines client for the emu-link broker protocol
 * (emulator/DESIGN.md section 8). It dials the broker named by the
 * EMU_BROKER env var ("unix:/path" or "tcp:host:port"), sends a hello on
 * connect, and dispatches inbound messages to per-type handlers from a
 * background reader thread. Sends are thread-safe.
 *
 * This component is host-only: it only ever runs on the IDF linux target
 * (the emulator node) or the plain-gcc test harness. On-device (esp32s3)
 * builds compile no implementation for it at all (see CMakeLists.txt);
 * virtual drivers (radio_virt, ssd1680_virt, gps_virt, ...) are its only
 * consumers.
 */

/* Handler for one inbound message type. msg is owned by emu_link and is
 * only valid for the duration of the call. */
typedef void (*emu_link_handler_t)(const cJSON *msg, void *ctx);

/* Connects to the broker named by the EMU_BROKER env var and sends the
 * hello message (node id, firmware version, caps, protocol version 1).
 * node_id must not be NULL; caps_csv may be NULL (treated as empty).
 * Returns 0 on success, negative on failure: EMU_BROKER unset or malformed,
 * a dial/connect failure, or emu_link is already connected. Never crashes
 * on a bad or missing EMU_BROKER. */
int emu_link_connect(const char *node_id, const char *caps_csv);

/* Registers h as the handler for inbound messages whose "t" field equals
 * type. One handler per type: a second registration for the same type
 * replaces the first. Returns 0 on success, negative if type or h is NULL,
 * type is too long, or the handler table is full. */
int emu_link_on(const char *type, emu_link_handler_t h, void *ctx);

/* Sends msg to the broker as one JSON line. Callers set msg's "t" field
 * before calling; emu_link_send takes ownership of msg (it is freed
 * internally whether or not the send succeeds) and is thread-safe: callers
 * on different threads may call it concurrently. Returns 0 on success,
 * negative if msg is NULL, msg has no "t" field, or emu_link is not
 * connected or the write failed. */
int emu_link_send(cJSON *msg);

/* Disconnects, stops the reader thread, and releases all connection state.
 * Safe to call when not connected. After this returns, emu_link_connect
 * may be called again. */
void emu_link_close(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAMBLE_EMU_LINK_H */
