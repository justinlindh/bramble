#ifndef BRAMBLE_ROUTING_AUTH_H
#define BRAMBLE_ROUTING_AUTH_H
#include "packet.h"

/*
 * Task 3.3 (SEC-H1, STAGED, NOT closed: see network_key.h). Host-compilable
 * home for the routing/reliability control-plane sign+verify helpers
 * (RERR here; ACK/delivery-receipt land in Task 3.5). mesh_task.c is
 * 3800+ ESP-IDF-only lines and is never #include'd by a host test, so any
 * verify function defined there could never be host-tested; this component
 * exists purely so these can be. mesh_task.c includes this header and
 * calls these functions; it defines none of them.
 *
 * Every helper here is forgeable under network_key.h's unprovisioned
 * public-PSK fallback key: none of them close SEC-H1 on their own.
 */

/*
 * Authenticates reporter_addr||broken_dest||broken_next_hop||seq with
 * label "bramble-rerr-v2", excluding only header.packet_id: every
 * forwarder re-originates a RERR (mesh_task.c's send_rerr) with its own
 * reporter_addr and a freshly-drawn seq, passing broken_dest/
 * broken_next_hop through unchanged, and re-signs the WHOLE struct
 * (including its own reporter_addr and seq) on every call. reporter_addr
 * moved INTO the MAC in ws 1.3b (it used to be excluded alongside
 * packet_id): this is safe specifically because each hop signs its own
 * reporter_addr rather than carrying someone else's forward, and it is
 * what makes replay-keying RERR on (reporter_addr, seq) sound, since both
 * halves of that key are now authenticated and reporter_addr identifies
 * exactly one node drawing one monotonic seq counter (no cross-signer
 * interleaving to break the sliding window). rerr_sign fills r->auth_hmac;
 * call it once, right before serializing (both on first detection and on
 * every re-origination, since send_rerr builds a fresh struct each time
 * and must re-draw a fresh seq for it). rerr_verify recomputes the same
 * MAC and constant-time-compares; returns nonzero (true) iff it matches.
 */
void rerr_sign(bramble_rerr_t* r);
int rerr_verify(const bramble_rerr_t* r);

/*
 * Task 3.5 (NEW-SEC-8, STAGED, NOT closed: see network_key.h), extended by
 * ws 1.3b. Authenticates src_addr||ack_packet_id||seq with label
 * "bramble-ack-v2", excluding relay_path/hop_count/header.hop_limit:
 * mesh_task.c's forward_ack grows relay_path, increments hop_count, and
 * decrements hop_limit on every relay hop, so those three are the only
 * per-hop-mutated fields. seq (ws 1.3b) is origin-stable, drawn once by
 * send_ack and carried through forward_ack unchanged, so it sits in the
 * same coverage set as src_addr/ack_packet_id. Both auth_hmac and seq
 * live at a fixed, hop_count-independent wire offset (packet.h), so a
 * verifier never has to trust the unauthenticated hop_count to find
 * either before checking them. ack_sign fills a->auth_hmac; call it once,
 * in the ACK builder (send_ack), after the seq is drawn and written in,
 * before serializing. ack_verify recomputes and constant-time-compares.
 * Gates BOTH observable effects of a valid ACK, cancelling retransmission
 * (pending_ack_remove) and marking a message delivered
 * (msg_store_update_status), plus forwarding: callers must reject before
 * any of those, on both the for-us and forward branches of handle_ack.
 */
void ack_sign(bramble_ack_t* a);
int ack_verify(const bramble_ack_t* a);

/*
 * Task 3.5 (NEW-SEC-8, STAGED, NOT closed), extended by ws 1.3b.
 * Authenticates src_addr||orig_packet_id||seq with label
 * "bramble-receipt-v2", excluding relay_path/hop_count/header.hop_limit
 * for the same reason as ack_sign/verify above (forward_delivery_receipt
 * grows relay_path, increments hop_count, decrements hop_limit per hop).
 * seq (ws 1.3b) is origin-stable, drawn once by the receipt builder
 * (mesh_build_broadcast_delivery_receipt_packet) and carried through
 * forward_delivery_receipt unchanged, at the same fixed,
 * hop_count-independent offset as auth_hmac. Same fixed-offset,
 * constant-time-compare contract.
 */
void receipt_sign(bramble_delivery_receipt_t* r);
int receipt_verify(const bramble_delivery_receipt_t* r);

/*
 * Task 4-fix F1 (wire v4 DATA reverse-route poisoning). Authenticates a DATA
 * frame's ORIGIN-STABLE fields under the network key with label
 * "bramble-data-v1", so a relay -- which never decrypts DATA and therefore
 * never checks the AEAD tag -- can still confirm the frame came from a
 * network-key holder before learning a reverse route off it (dest=src_addr,
 * next_hop=prev_hop). The covered bytes are exactly bramble_build_aead_aad's
 * output: the masked header (hop_limit zeroed) followed by little-endian
 * src_addr (HEADER_SIZE + 4 bytes). This EXCLUDES prev_hop and hop_limit,
 * the two relay-mutable fields, so the MAC survives every forward hop
 * unchanged, exactly as the AEAD tag does. The originator calls
 * data_auth_sign once at TX (send_data_packet/send_dm_packet/
 * mesh_send_location_packet), writing the 8 bytes at
 * BRAMBLE_DATA_AUTH_HMAC_OFFSET; forwarders and the mailbox flusher copy
 * those bytes through verbatim. data_auth_verify recomputes the same MAC and
 * constant-time-compares; returns nonzero (true) iff it matches.
 *
 * Forgeable under network_key.h's unprovisioned public-PSK fallback, same as
 * every other helper here; this closes the keyless-poisoning attack, not the
 * keyed-insider residual.
 */
void data_auth_sign(const bramble_header_t* h, uint32_t src_addr, uint8_t out[8]);
int data_auth_verify(const bramble_header_t* h, uint32_t src_addr, const uint8_t hmac[8]);

#endif
