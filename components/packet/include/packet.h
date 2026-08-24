#ifndef BRAMBLE_PACKET_H
#define BRAMBLE_PACKET_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Every build resolves esp_err.h by include path: the real header on
 * ESP-IDF, test/stubs/esp_err.h on host builds, nrf/shim/include/esp_err.h
 * on the nRF52840 target. */
#include "esp_err.h"

/* Protocol version. DM/LOCATION session payloads carry a 3-byte cleartext
 * ratchet header (epoch || msg_index, authenticated via the AEAD AAD) and are
 * keyed by a per-message ratchet (see DM_RATCHET_HEADER_SIZE below). The RX
 * version gate (bramble_header_is_supported_version) compares exactly, so a
 * session frame from any other version drops and the peers re-handshake. */
#define BRAMBLE_VERSION 5

/* The protocol release this build speaks, as reported to clients by
 * bramble.getStatus / bramble.getVersion / bramble.exportTopology. Distinct
 * from BRAMBLE_VERSION above, which is the single wire byte the RX version
 * gate compares: this is the human-facing release string, and it lives beside
 * the wire version so the two are revised together. It is here rather than in
 * main/ because the simulator (simulator/gosim) reports it too, from the same
 * protocol code. */
#define BRAMBLE_PROTOCOL_VERSION "0.5.0"

/* Packet types */
#define PKT_TYPE_ACK 0x01
#define PKT_TYPE_RREQ 0x02
#define PKT_TYPE_RREP 0x03
#define PKT_TYPE_RERR 0x04
#define PKT_TYPE_BEACON 0x05
/* 0x06 retired from the wire in v2: handshakes ride DATA envelopes
 * (app_type APP_TYPE_KE). Never RX-dispatched; a stray 0x06 falls through
 * to the RX switch's default case and is dropped. Kept defined only so
 * legacy references (e.g. traffic_debug's packet-type classifier) still
 * compile; do not add a case for it to the RX switch. */
#define PKT_TYPE_KEY_EXCHANGE 0x06
#define PKT_TYPE_DELIVERY_RECEIPT 0x07
#define PKT_TYPE_DATA 0x0A
#define PKT_TYPE_STORE_REQUEST 0x0B
#define PKT_TYPE_STORE_ACK 0x0C
#define PKT_TYPE_MAILBOX_DELIVERY 0x0D
#define PKT_TYPE_MAILBOX_QUERY 0x0E
#define PKT_TYPE_PROBE 0x12     /* Network reachability probe */
#define PKT_TYPE_PROBE_ACK 0x13 /* Probe acknowledgement */
#define PKT_TYPE_LOCATION 0x14  /* Location share */
/* Self-signed identity attestation (per-node identity): a node's
 * broadcast claim binding {address, X25519 pub, Ed25519 pub} under its own
 * Ed25519 key. Additive on wire v4: un-upgraded peers drop it at the RX
 * dispatch switch's default case. */
#define PKT_TYPE_IDENTITY_ATTESTATION 0x15

#define BEACON_FLAG_MAILBOX 0x01 /* Node willing to store messages */

/* Buffer sizes */
#define BRAMBLE_MAX_PACKET_SIZE 256

/* Flag bits (wire v2). Tier lives inside the LOCATION ciphertext, not in
 * these bits, so bits 6-7 carry nothing. There is no emergency feature; bit 6
 * holds the reserved FLAG_EMERGENCY name only. */
#define FLAG_RESERVED_HIGH 0x80 /* reserved; not used */
#define FLAG_EMERGENCY 0x40     /* origin-set, immutable, AAD-bound; reserved for future use */
#define FLAG_ACK_REQ (1 << 5)
#define FLAG_RECEIPT (1 << 4)
#define FLAG_CHANNEL (1 << 3)
#define FLAG_ENCRYPT (1 << 2)
#define FLAG_FRAG_MASK 0x03

/* Sizes */
#define HEADER_SIZE 12
#define ACK_BASE_SIZE                                                                              \
    37 /* header(12) + src(4) + ack_pkt_id(4) + flags(1) + rssi(1) + hop_count(1) + auth_hmac(8) + \
        * seq(6) */
#define ACK_MAX_HOPS 8
#define ACK_MAX_SIZE (ACK_BASE_SIZE + ACK_MAX_HOPS * 4) /* 37 + 32 = 69 */
#define ACK_SIZE ACK_BASE_SIZE                          /* backward compat for min size checks */
#define RREQ_SIZE 30
#define RREP_SIZE 40   /* includes auth_hmac(8) + seq(6) */
#define RERR_SIZE 38   /* includes auth_hmac(8) + seq(6) */
#define BEACON_SIZE 54 /* fixed prefix: includes seq(6) + auth_hmac(16); name follows */
#define KEY_EXCHANGE_SIZE 101
#define DELIVERY_RECEIPT_MIN_SIZE 36 /* includes auth_hmac(8) + seq(6), zero relay_path hops */
#define DELIVERY_RECEIPT_MAX_SIZE 68 /* MIN + DELIVERY_RECEIPT_MAX_HOPS * 4 bytes of relay_path */

#define DELIVERY_RECEIPT_MAX_HOPS 8

/* Identity attestation wire size: header(12) + src_addr(4) + x25519_pub(32)
 * + ed25519_pub(32) + sig(64) + not_after(8) + endorsement_sig(64)
 * + auth_hmac(8) + seq(6). Fixed-size frame; the deserializer rejects
 * anything that is not EXACTLY this long, so a peer speaking a different
 * attestation layout is dropped rather than misparsed. The canonical SIGNED
 * message below covers only a subset of these fields and is sized
 * independently (IDENTITY_ATTESTATION_MSG_SIZE). */
#define IDENTITY_ATTESTATION_SIZE (HEADER_SIZE + 4 + 32 + 32 + 64 + 8 + 64 + 8 + 6) /* 230 */

/* Canonical signed message for the attestation (see
 * bramble_identity_attestation_signed_msg): context(16) + src_addr(4)
 * + x25519_pub(32) + ed25519_pub(32) = 84 bytes. */
#define IDENTITY_ATTESTATION_MSG_CONTEXT "bramble-ident-v1"
#define IDENTITY_ATTESTATION_MSG_CONTEXT_LEN 16
#define IDENTITY_ATTESTATION_MSG_SIZE (IDENTITY_ATTESTATION_MSG_CONTEXT_LEN + 4 + 32 + 32)

/* Common header (12 bytes) */
typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint8_t hop_limit;
    uint32_t dest_addr;
    uint32_t packet_id;
} bramble_header_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t ack_packet_id;
    uint8_t ack_flags;
    int8_t rssi_at_dest;
    uint8_t hop_count; /* number of addresses in relay_path */
    /* NEW-SEC-8 (STAGED, not closed: see network_key.h).
     * Covers src_addr||ack_packet_id||seq; excludes
     * relay_path/hop_count/hop_limit, which forward_ack mutates on every
     * relay hop. Placed BEFORE relay_path (a fixed, hop_count-independent
     * wire offset) so a verifier never has to trust the unauthenticated
     * hop_count to locate the tag. */
    uint8_t auth_hmac[8];
    /* 48-bit origin sequence, drawn once by the originating
     * destination (control_seq_next in mesh_reliability.c's send_ack) and
     * carried through forward_ack unchanged, exactly like auth_hmac. Sits
     * at the same fixed, hop_count-independent offset immediately after
     * auth_hmac and BEFORE relay_path, for the same reason auth_hmac does:
     * a verifier must never trust the unauthenticated hop_count to locate
     * either. MAC-covered (ack_build_auth_buf). */
    uint8_t seq[6];
    uint32_t relay_path[ACK_MAX_HOPS]; /* hop trail: [dest, relay1, relay2, ...] */
} bramble_ack_t;

typedef struct {
    bramble_header_t header;
    uint32_t query_id;
    uint32_t encrypted_source;
    uint8_t hop_count;
    uint8_t metric;
    uint32_t prev_hop;
    uint32_t rreq_salt;
} bramble_rreq_t;

typedef struct {
    bramble_header_t header;
    uint32_t query_id;
    uint32_t src_addr;
    uint32_t next_hop;
    uint8_t hop_count;
    uint8_t route_metric;
    uint8_t auth_hmac[8];
    /* 48-bit origin sequence, drawn once by the originator
     * (control_seq_next in mesh_beacon.c) and carried through rrep_forward
     * unchanged, exactly like auth_hmac. MAC-covered (rrep_build_auth_buf).
     */
    uint8_t seq[6];
} bramble_rrep_t;

typedef struct {
    bramble_header_t header;
    uint32_t reporter_addr;
    uint32_t broken_dest;
    uint32_t broken_next_hop;
    /* SEC-H1 (STAGED, not closed: see network_key.h).
     * Covers reporter_addr||broken_dest||broken_next_hop||seq;
     * excludes only header.packet_id, which every forwarder legitimately
     * rewrites on re-origination (send_rerr, mesh_routing.c). reporter_addr
     * is MAC-covered because every forwarder re-signs with its OWN
     * reporter_addr on every re-origination (see routing_auth.h). */
    uint8_t auth_hmac[8];
    /* 48-bit origin sequence, freshly drawn by EACH hop on every
     * re-origination (control_seq_next in mesh_routing.c's send_rerr), not
     * origin-stable like RREP's. MAC-covered; replay-keyed on
     * (reporter_addr, seq), both authenticated. */
    uint8_t seq[6];
} bramble_rerr_t;

#define BEACON_NAME_MAX 16

/* Largest prefix of the first `len` bytes of `s` that fits in `max_bytes`
 * without ending in the middle of a UTF-8 sequence.
 *
 * A node name may be up to BRAMBLE_NODE_NAME_MAX bytes while a beacon carries
 * at most BEACON_NAME_MAX, so a name gets cut for the air. Cutting on a raw
 * byte count splits a multi-byte character, and what goes out is then not
 * valid UTF-8: neighbours store the broken tail and render a replacement
 * character. Bytes that are not a valid lead byte are passed through one at a
 * time, so a name that is not UTF-8 at all still gets capped rather than
 * dropped. A trailing partial sequence is dropped even when `len` already
 * fits, because the input may itself have been cut on a byte count upstream.
 * Returns 0 for a NULL pointer. */
size_t bramble_utf8_trunc_len(const uint8_t* s, size_t len, size_t max_bytes);

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t pubkey_hash;
    uint16_t uptime_min;
    uint8_t battery_pct;
    uint8_t tx_queue_depth;
    uint8_t neighbor_count;
    uint8_t flags;
    uint32_t network_time;
    uint16_t time_confidence;
    /* 48-bit origin sequence. MUST stay inside the fixed prefix
     * beacon_compute_hmac hashes (i.e. BEFORE auth_hmac): the prefix
     * length there is BEACON_SIZE - sizeof(auth_hmac), so anything placed
     * before auth_hmac is covered automatically, and anything placed
     * after it (like name) needs its own explicit coverage (see
     * beacon.c). Drawn once per periodic beacon (control_seq_next in
     * mesh_beacon.c's send_beacon); beacons are single-hop and never
     * forwarded, so there is no carry-through case to preserve here
     * (unlike RREP/ACK/receipt). */
    uint8_t seq[6];
    uint8_t auth_hmac[16];
    /* Optional: node name (appended after fixed fields) */
    uint8_t name_len;
    char name[BEACON_NAME_MAX + 1];
} bramble_beacon_t;

/* 0x06 retired from the wire in v2: handshakes ride DATA envelopes (app_type
 * APP_TYPE_KE). Struct kept as the inner-payload layout: dm_build_init/
 * dm_build_resp fill it, bramble_key_exchange_serialize/deserialize frame
 * it as the plaintext carried inside a channel-key-encrypted DATA packet
 * (see handle_ke_envelope in mesh_dm.c), it never appears standalone on
 * the wire under PKT_TYPE_KEY_EXCHANGE again. */
typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint8_t ephemeral_pubkey[32];
    uint8_t long_term_pubkey[32];
    uint8_t key_id;
    uint8_t ke_type;
    uint8_t auth_tag[16];
} bramble_key_exchange_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t orig_packet_id;
    uint8_t hop_count;
    uint8_t total_latency;
    /* NEW-SEC-8 (STAGED, not closed: see network_key.h).
     * Covers src_addr||orig_packet_id||seq; excludes
     * relay_path/hop_count/hop_limit, which forward_delivery_receipt
     * mutates on every relay hop. Placed BEFORE relay_path (a fixed,
     * hop_count-independent wire offset), same rationale as
     * bramble_ack_t's auth_hmac above. */
    uint8_t auth_hmac[8];
    /* 48-bit origin sequence, drawn once by the originating
     * builder (control_seq_next in mesh_reliability.c, via
     * mesh_build_broadcast_delivery_receipt_packet) and carried through
     * forward_delivery_receipt unchanged, exactly like auth_hmac. Same
     * fixed, hop_count-independent offset rule as auth_hmac and
     * bramble_ack_t's seq: immediately after auth_hmac, before
     * relay_path. MAC-covered (receipt_build_auth_buf). */
    uint8_t seq[6];
    uint32_t relay_path[DELIVERY_RECEIPT_MAX_HOPS];
} bramble_delivery_receipt_t;

/*
 * Identity attestation (per-node identity, relay-gated):
 * a self-signed, flooded (low-cadence) binding of {src_addr, X25519 pub,
 * Ed25519 pub}.
 *
 * sig is Ed25519 over the domain-separated canonical message
 *
 *   "bramble-ident-v1" || src_addr(4, big-endian) || x25519_pub(32)
 *                      || ed25519_pub(32)
 *
 * (bramble_identity_attestation_signed_msg builds it; the pinning verifier
 * checks these same bytes). The header is deliberately NOT
 * covered: hop_limit is relay-mutable and packet_id is per-send, while the
 * identity claim itself is stable across re-sends and relays.
 *
 * TWO authenticators, two jobs:
 *   - sig (Ed25519) carries the identity claim's TRUTH: it is
 *     self-authenticating, checkable by ANY member against the embedded
 *     ed25519_pub with no shared secret needed. A network-key MAC adds
 *     nothing to the claim's truth.
 *   - auth_hmac (network-key MAC, label "bramble-ident-relay-v1",
 *     routing_auth.h's ident_relay_sign/verify) gates RELAY PRIVILEGE,
 *     preserving the branch invariant that keyless traffic never
 *     propagates: an outsider without the network key can neither get its
 *     spam flooded through the mesh nor grind relays with Ed25519
 *     verifies, because relays check this CHEAP MAC first and never run
 *     the Ed25519 verify at all (only pinning receivers do).
 *   It covers src_addr || x25519_pub || ed25519_pub || sig || seq and NOT
 *   the header (relay-mutable): a relay decrements hop_limit and passes
 *   the frame through otherwise unmodified.
 *
 * seq is a fresh control-plane sequence (control_seq_next) drawn once at
 * ORIGINATION and never re-drawn by relays; receivers replay-check it
 * (src_addr-scoped) after the MAC verifies, so a captured attestation
 * cannot be re-injected to burn relay airtime.
 */
typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint8_t x25519_pub[32];
    uint8_t ed25519_pub[32];
    uint8_t sig[64];
    /* Endorsement certificate: the anchor's signature vouching for
     * ed25519_pub, carried inline so a receiver can verify fleet membership
     * from the frame alone (identity_store.c's pin check). not_after is
     * big-endian ms epoch; UINT64_MAX = permanent, 0 = "no cert present"
     * (endorsement_sig then all-zero). The node NEVER signs this; it is the
     * anchor's signature, provisioned via setEndorsement. Not part of the
     * self-signed canonical message (bramble_identity_attestation_signed_msg
     * does not cover it); it IS covered by the relay-gate MAC
     * (ident_relay_sign) so an outsider cannot flip cert bits in flight. */
    uint64_t not_after;
    uint8_t endorsement_sig[64];
    uint8_t auth_hmac[8]; /* network-key relay gate */
    uint8_t seq[6];       /* 48-bit origin seq, big-endian */
} bramble_identity_attestation_t;

/*
 * 48-bit control-plane sequence pack/unpack. The seq field on the
 * ACK/RREP/RERR/BEACON/delivery-receipt/attestation frames above is a
 * big-endian 6-byte array; these two helpers are the single definition of that
 * byte order, so no build or parse site can transcribe the shifts differently
 * (a drift here is exactly the freshness/replay bug the seq field defends
 * against).
 */
static inline void bramble_seq48_pack(uint8_t out[6], uint64_t seq) {
    out[0] = (uint8_t)(seq >> 40);
    out[1] = (uint8_t)(seq >> 32);
    out[2] = (uint8_t)(seq >> 24);
    out[3] = (uint8_t)(seq >> 16);
    out[4] = (uint8_t)(seq >> 8);
    out[5] = (uint8_t)seq;
}

static inline uint64_t bramble_seq48_unpack(const uint8_t in[6]) {
    return ((uint64_t)in[0] << 40) | ((uint64_t)in[1] << 32) | ((uint64_t)in[2] << 24) |
           ((uint64_t)in[3] << 16) | ((uint64_t)in[4] << 8) | (uint64_t)in[5];
}

/* Serialize/deserialize functions. Return ESP_OK or ESP_ERR_INVALID_SIZE. */
esp_err_t bramble_header_serialize(const bramble_header_t* h, uint8_t* buf, size_t len);
esp_err_t bramble_header_deserialize(bramble_header_t* h, const uint8_t* buf, size_t len);
bool bramble_header_is_supported_version(const bramble_header_t* h);

/*
 * Build the AES-GCM AAD for an encrypted DATA packet: the serialized header
 * with the hop_limit byte zeroed.
 *
 * Invariant: hop_limit is the only header field a relay mutates in flight
 * (forward_data_packet decrements it before retransmitting), so it must be
 * excluded from authentication or every forwarded packet fails the
 * destination's tag check. All other fields (version, type, flags,
 * dest_addr, packet_id) stay bound. If a relay ever needs to mutate another
 * header field, mask it here so the encrypt and decrypt paths cannot diverge.
 *
 * Both endpoints MUST use this helper: the originator when encrypting
 * (send_data_packet) and the destination when decrypting (handle_data).
 */
esp_err_t bramble_header_build_aad(const bramble_header_t* h, uint8_t* buf, size_t len);

/*
 * Build the full AEAD AAD for an encrypted DATA packet: the masked header
 * (see bramble_header_build_aad) followed by the 4-byte little-endian
 * src_addr (SEC-M2 residual). Binds the body's src_addr field into the GCM
 * tag so tampering it after origination fails authentication instead of
 * silently misattributing the message. Writes HEADER_SIZE + 4 bytes.
 *
 * Both endpoints MUST pass the same src_addr: the originator its own
 * identity address (send_data_packet), the destination the src_addr it just
 * read off the wire (handle_data), before hop_limit is masked or mutated.
 */
esp_err_t bramble_build_aead_aad(const bramble_header_t* h, uint32_t src_addr, uint8_t* buf,
                                 size_t len);

/*
 * DATA/LOCATION envelope layout (wire v4):
 *   header(HEADER_SIZE) + src_addr(4) + prev_hop(4) + auth_hmac(8)
 *   + nonce(BRAMBLE_NONCE_SIZE) + ciphertext(N) + tag(BRAMBLE_TAG_SIZE)
 * (send_data_packet (main/mesh_task.c), send_dm_packet (main/mesh_dm.c), and
 * mesh_send_location_packet (main/mesh_location.c) build this layout;
 * handle_data (main/mesh_task.c) and handle_location (main/mesh_location.c)
 * parse it; gosim's bridge.c
 * uses its own out-of-band src_addr/prev_hop tracking instead of these wire
 * bytes, since its DATA framing already diverges from firmware's).
 *
 * prev_hop is RELAY-MUTABLE and MAC-EXCLUDED, mirroring RREP's relay-mutable
 * next_hop: every node that transmits this frame -- the originator on
 * first TX, then each relay before it retransmits -- overwrites prev_hop
 * with its OWN address, so any receiver always knows the address of the
 * radio it just heard this specific frame from, regardless of how many hops
 * it has already travelled. Because every hop rewrites it, prev_hop cannot
 * live under the AEAD tag. It also is NOT fed into bramble_build_aead_aad:
 * that helper's AAD buffer covers no part of it (HEADER_SIZE + 4 bytes: the
 * masked header plus src_addr only), so prev_hop
 * simply sits at a wire offset the AAD build never reads, exactly the way
 * nonce/ciphertext/tag already sit outside it. There is nothing to "zero":
 * exclusion here is structural (prev_hop's offset is never copied into the
 * AAD buffer at all), not a masked-in-place byte like hop_limit.
 *
 * auth_hmac (wire v4) is an 8-byte NETWORK-KEY MAC the
 * ORIGINATOR writes and never mutates in flight (relays and the mailbox
 * flusher copy it through verbatim, exactly like the AEAD tag). It covers
 * the ORIGIN-STABLE authenticated fields -- the masked header (hop_limit
 * zeroed) plus src_addr, i.e. the same HEADER_SIZE + 4 byte buffer
 * bramble_build_aead_aad produces -- and EXCLUDES prev_hop and hop_limit
 * (both relay-mutable). It is the DATA analogue of ack/rrep/rerr's
 * auth_hmac[8] (routing_auth.h): a relay never decrypts a DATA frame, so the
 * AEAD tag (checked only at the destination) cannot gate reverse-route
 * learning. Without this MAC a keyless attacker could inject a DATA frame
 * with a spoofed src_addr and poison every node's route toward that victim.
 * data_auth_sign/data_auth_verify (routing_auth.h) build and check it;
 * mesh_process_rx_packet verifies it BEFORE learning a breadcrumb or
 * forwarding, so only network-key holders can lay breadcrumbs (RREP parity).
 * Like all control-plane auth here, when unprovisioned there is no key
 * (fail-closed inert: network_key_mac is the all-zero sentinel and verify
 * rejects before the compare, no public-PSK fallback), so only provisioned
 * network-key holders can lay breadcrumbs. The keyed-insider residual
 * remains.
 *
 * src_addr stays AAD-bound, so the reverse route's TARGET (who a returning
 * confirmation is ultimately for) cannot be spoofed by an on-path relay;
 * only the NEXT-HOP hint is unauthenticated. Residual (see
 * docs/SECURITY-MODEL.md): a malicious relay can lie about prev_hop to
 * attract or blackhole reverse traffic -- the same insider-forwarding
 * residual already accepted for the RREP control plane.
 */
#define BRAMBLE_DATA_SRC_ADDR_OFFSET (HEADER_SIZE)

/**
 * Read the origin address a received frame carries at wire offset HEADER_SIZE.
 *
 * Two encodings share that offset, which is why this cannot be one
 * unconditional read: the types framed by bramble_*_serialize write src_addr
 * big-endian via put_be32, while the hand-built envelope types (DATA, its
 * LOCATION twin, and the PROBE pair) memcpy the field in host order.
 *
 * Returns false, leaving *out untouched, when the frame is too short or its
 * type carries no origin address there. RREQ, RREP and RERR lead with
 * query_id or reporter_addr, so they are deliberately excluded rather than
 * misread into a plausible-looking address. Callers must treat false as
 * "unknown origin", never as address zero.
 *
 * The address is read off the still-unauthenticated wire prefix, so it is a
 * claim, not a verified identity. It is fine for telemetry and attribution;
 * it must not be used for any trust decision.
 */
bool bramble_packet_origin_addr(uint8_t type, const uint8_t* buf, size_t len, uint32_t* out);
#define BRAMBLE_DATA_PREV_HOP_OFFSET (HEADER_SIZE + 4)
#define BRAMBLE_DATA_AUTH_HMAC_OFFSET (HEADER_SIZE + 8)
#define BRAMBLE_DATA_NONCE_OFFSET (HEADER_SIZE + 16)
/* header + src_addr + prev_hop + auth_hmac, i.e. where the AEAD nonce begins */
#define BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE (HEADER_SIZE + 16)

/* Ratchet header carried in the CLEARTEXT of DM/LOCATION session frames, on the
 * wire between the nonce and the ciphertext, and authenticated via the AEAD AAD:
 * epoch(1) || msg_index(2, big-endian). Read before decrypt. */
#define DM_RATCHET_HEADER_SIZE 3

esp_err_t bramble_ack_serialize(const bramble_ack_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_ack_deserialize(bramble_ack_t* p, const uint8_t* buf, size_t len);
size_t bramble_ack_wire_size(const bramble_ack_t* p);

esp_err_t bramble_rreq_serialize(const bramble_rreq_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_rreq_deserialize(bramble_rreq_t* p, const uint8_t* buf, size_t len);

esp_err_t bramble_rrep_serialize(const bramble_rrep_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_rrep_deserialize(bramble_rrep_t* p, const uint8_t* buf, size_t len);

esp_err_t bramble_rerr_serialize(const bramble_rerr_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_rerr_deserialize(bramble_rerr_t* p, const uint8_t* buf, size_t len);

esp_err_t bramble_beacon_serialize(const bramble_beacon_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_beacon_deserialize(bramble_beacon_t* p, const uint8_t* buf, size_t len);
size_t bramble_beacon_wire_size(const bramble_beacon_t* p);

esp_err_t bramble_key_exchange_serialize(const bramble_key_exchange_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_key_exchange_deserialize(bramble_key_exchange_t* p, const uint8_t* buf,
                                           size_t len);

esp_err_t bramble_delivery_receipt_serialize(const bramble_delivery_receipt_t* p, uint8_t* buf,
                                             size_t len);
esp_err_t bramble_delivery_receipt_deserialize(bramble_delivery_receipt_t* p, const uint8_t* buf,
                                               size_t len);

esp_err_t bramble_identity_attestation_serialize(const bramble_identity_attestation_t* p,
                                                 uint8_t* buf, size_t len);
/* Exact-length check: len must be IDENTITY_ATTESTATION_SIZE, not merely >=. */
esp_err_t bramble_identity_attestation_deserialize(bramble_identity_attestation_t* p,
                                                   const uint8_t* buf, size_t len);
/* Write the IDENTITY_ATTESTATION_MSG_SIZE-byte canonical message the sig
 * covers (see the struct comment). Signer and verifier both use this, so
 * the signed bytes can never diverge between them. */
esp_err_t bramble_identity_attestation_signed_msg(const bramble_identity_attestation_t* p,
                                                  uint8_t* buf, size_t len);

#endif /* BRAMBLE_PACKET_H */
