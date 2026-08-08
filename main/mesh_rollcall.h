#ifndef MESH_ROLLCALL_H
#define MESH_ROLLCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rollcall.h"

/*
 * Attested roll-call: the firmware entry points.
 *
 * The primitive itself is documented in components/rollcall/include/
 * rollcall.h and docs/rollcall.md; this header is only the surface the rest
 * of the firmware (the RX dispatch, the maintenance tick, the delivery
 * receipt path, and the RPC layer) reaches it through.
 */

/* mesh_rollcall_start return codes. Distinct rather than a single failure so
 * an operator can tell "wait for the current one" from "wait out the rate
 * limit" from "the radio refused it". */
#define MESH_ROLLCALL_OK 0
#define MESH_ROLLCALL_ERR_BUSY (-1)          /* a roll-call is still collecting */
#define MESH_ROLLCALL_ERR_RATE_LIMITED (-2)  /* inside ROLLCALL_MIN_INTERVAL_MS */
#define MESH_ROLLCALL_ERR_TEXT_TOO_LONG (-3) /* payload over ROLLCALL_TEXT_MAX */
#define MESH_ROLLCALL_ERR_TX (-4)            /* nothing reached the air */
#define MESH_ROLLCALL_ERR_INTERNAL (-5)      /* no identity, no channel, or no heap */

/*
 * Start a roll-call: open a ledger and put round 1 on the air. text may be
 * NULL when text_len is 0. On success *rollcall_id_out (when non-NULL)
 * receives the id the announce carries.
 *
 * Enforces the initiation rate limit and the one-at-a-time rule before
 * anything is transmitted. A failed round-1 send retires the ledger and does
 * NOT charge the rate limiter: a roll-call that never reached the air must
 * not block the next attempt.
 */
int mesh_rollcall_start(const char* text, size_t text_len, uint32_t* rollcall_id_out);

/* The ledger of the roll-call this node started, or NULL if it never started
 * one. Readable after the roll-call closes; `open` says whether it is still
 * collecting. */
const rollcall_ledger_t* mesh_rollcall_ledger(void);

/* Answers this node could not queue because its pending queue was full.
 * Surfaced so a node that failed to take part says so locally, rather than
 * only appearing as a hole in someone else's ledger. */
uint32_t mesh_rollcall_pending_dropped(void);

/* RX: a decrypted, authenticated APP_TYPE_ROLLCALL payload (broadcast
 * announce). Claims the answer-once slot and queues a staggered response. */
void mesh_rollcall_handle_announce(uint32_t src_addr, int channel_idx, const uint8_t* data,
                                   size_t data_len);

/* RX: a decrypted, authenticated APP_TYPE_ROLLCALL_REPLY payload (unicast
 * response). Verifies the Ed25519 signature against the responder's pinned
 * identity key before anything is recorded. */
void mesh_rollcall_handle_response(uint32_t src_addr, const uint8_t* data, size_t data_len);

/* A verified broadcast delivery receipt arrived. When orig_packet_id is one
 * of this roll-call's announce rounds, its relay path is attached to the
 * responder's ledger row. relay_path runs responder -> ... -> us, exactly as
 * the receipt carries it. */
void mesh_rollcall_note_receipt(uint32_t responder_addr, uint32_t orig_packet_id, uint8_t hop_count,
                                const uint32_t* relay_path);

/* Drive the re-announce rounds, the staggered responses, and the ledger
 * close. Called from mesh_periodic_maintenance's existing tick; returns
 * immediately (and touches no memory) on a node that has never taken part in
 * a roll-call. */
void mesh_rollcall_tick(uint32_t t);

#endif /* MESH_ROLLCALL_H */
