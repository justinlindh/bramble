#ifndef BRAMBLE_PACKET_H
#define BRAMBLE_PACKET_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
#include "esp_stubs.h"
#endif

/* Protocol version.
 * was 3; Phase 1 delivery-core flag day: DATA/LOCATION now carry a
 * relay-mutated prev_hop for reverse-route learning (see
 * BRAMBLE_DATA_PREV_HOP_OFFSET below). */
#define BRAMBLE_VERSION 4

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
/* Self-signed identity attestation (per-node identity Phase 2): a node's
 * broadcast claim binding {address, X25519 pub, Ed25519 pub} under its own
 * Ed25519 key. Additive on wire v4: un-upgraded peers drop it at the RX
 * dispatch switch's default case. */
#define PKT_TYPE_IDENTITY_ATTESTATION 0x15

#define BEACON_FLAG_MAILBOX 0x01 /* Node willing to store messages */

/* Buffer sizes */
#define BRAMBLE_MAX_PACKET_SIZE 256

/* Flag bits (wire v2). Tier moved into the LOCATION ciphertext, freeing bits 6-7.
 * DES-9: FLAG_EMERGENCY no longer collides with FLAG_ENCRYPT (historical 0x04
 * collision is gone; there is no emergency feature, bit 6 is reserved-named). */
#define FLAG_RESERVED_HIGH 0x80 /* reserved (was FLAG_DEFERRED in RFC r1; not used) */
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
        * seq(6); was 31, +6 for seq (ws 1.3b control-plane freshness) */
#define ACK_MAX_HOPS 8
#define ACK_MAX_SIZE (ACK_BASE_SIZE + ACK_MAX_HOPS * 4) /* 37 + 32 = 69 */
#define ACK_SIZE ACK_BASE_SIZE                          /* backward compat for min size checks */
#define RREQ_SIZE 30
#define RREP_SIZE 40   /* was 34; +6 for seq (ws 1.3b control-plane freshness) */
#define RERR_SIZE 38   /* was 32; +6 for seq (ws 1.3b control-plane freshness) */
#define BEACON_SIZE 54 /* was 48; +6 for seq (ws 1.3b control-plane freshness) */
#define KEY_EXCHANGE_SIZE 101
#define DELIVERY_RECEIPT_MIN_SIZE 36 /* was 30; +6 for seq (ws 1.3b control-plane freshness) */
#define DELIVERY_RECEIPT_MAX_SIZE 68 /* was 62; +6 for seq (ws 1.3b control-plane freshness) */

#define DELIVERY_RECEIPT_MAX_HOPS 8

/* Identity attestation wire size: header(12) + src_addr(4) + x25519_pub(32)
 * + ed25519_pub(32) + sig(64) + auth_hmac(8) + seq(6). Fixed-size frame;
 * the deserializer rejects anything that is not EXACTLY this long.
 * Phase 3 grew the frame by the relay-gate MAC + seq (144 -> 158); the
 * canonical SIGNED message below is unchanged. */
#define IDENTITY_ATTESTATION_SIZE (HEADER_SIZE + 4 + 32 + 32 + 64 + 8 + 6) /* 158 */

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
    /* NEW-SEC-8 (Task 3.5, STAGED, not closed: see network_key.h), extended
     * by ws 1.3b. Covers src_addr||ack_packet_id||seq; excludes
     * relay_path/hop_count/hop_limit, which forward_ack mutates on every
     * relay hop. Placed BEFORE relay_path (a fixed, hop_count-independent
     * wire offset) so a verifier never has to trust the unauthenticated
     * hop_count to locate the tag. */
    uint8_t auth_hmac[8];
    /* ws 1.3b: 48-bit origin sequence, drawn once by the originating
     * destination (control_seq_next in mesh_task.c's send_ack) and
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
    /* ws 1.3b: 48-bit origin sequence, drawn once by the originator
     * (control_seq_next in mesh_task.c) and carried through rrep_forward
     * unchanged, exactly like auth_hmac. MAC-covered (rrep_build_auth_buf).
     */
    uint8_t seq[6];
} bramble_rrep_t;

typedef struct {
    bramble_header_t header;
    uint32_t reporter_addr;
    uint32_t broken_dest;
    uint32_t broken_next_hop;
    /* SEC-H1 (Task 3.3, STAGED, not closed: see network_key.h), extended by
     * ws 1.3b. Covers reporter_addr||broken_dest||broken_next_hop||seq;
     * excludes only header.packet_id, which every forwarder legitimately
     * rewrites on re-origination (send_rerr, mesh_task.c). reporter_addr
     * is MAC-covered because every forwarder re-signs with its OWN
     * reporter_addr on every re-origination (see routing_auth.h). */
    uint8_t auth_hmac[8];
    /* ws 1.3b: 48-bit origin sequence, freshly drawn by EACH hop on every
     * re-origination (control_seq_next in mesh_task.c's send_rerr), not
     * origin-stable like RREP's. MAC-covered; replay-keyed on
     * (reporter_addr, seq), both authenticated. */
    uint8_t seq[6];
} bramble_rerr_t;

#define BEACON_NAME_MAX 16

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
    /* ws 1.3b: 48-bit origin sequence. MUST stay inside the fixed prefix
     * beacon_compute_hmac hashes (i.e. BEFORE auth_hmac): the prefix
     * length there is BEACON_SIZE - sizeof(auth_hmac), so anything placed
     * before auth_hmac is covered automatically, and anything placed
     * after it (like name) needs its own explicit coverage, per Fix 4's
     * lesson (see beacon.c). Drawn once per periodic beacon
     * (control_seq_next in mesh_task.c's send_beacon); beacons are
     * single-hop and never forwarded, so there is no carry-through case
     * to preserve here (unlike RREP/ACK/receipt). */
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
 * (see handle_ke_envelope in mesh_task.c), it never appears standalone on
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
    /* NEW-SEC-8 (Task 3.5, STAGED, not closed: see network_key.h), extended
     * by ws 1.3b. Covers src_addr||orig_packet_id||seq; excludes
     * relay_path/hop_count/hop_limit, which forward_delivery_receipt
     * mutates on every relay hop. Placed BEFORE relay_path (a fixed,
     * hop_count-independent wire offset), same rationale as
     * bramble_ack_t's auth_hmac above. */
    uint8_t auth_hmac[8];
    /* ws 1.3b: 48-bit origin sequence, drawn once by the originating
     * builder (control_seq_next in mesh_task.c, via
     * mesh_build_broadcast_delivery_receipt_packet) and carried through
     * forward_delivery_receipt unchanged, exactly like auth_hmac. Same
     * fixed, hop_count-independent offset rule as auth_hmac and
     * bramble_ack_t's seq: immediately after auth_hmac, before
     * relay_path. MAC-covered (receipt_build_auth_buf). */
    uint8_t seq[6];
    uint32_t relay_path[DELIVERY_RECEIPT_MAX_HOPS];
} bramble_delivery_receipt_t;

/*
 * Identity attestation (per-node identity Phase 2, relay-gated in Phase 3):
 * a self-signed, flooded (low-cadence) binding of {src_addr, X25519 pub,
 * Ed25519 pub}.
 *
 * sig is Ed25519 over the domain-separated canonical message
 *
 *   "bramble-ident-v1" || src_addr(4, big-endian) || x25519_pub(32)
 *                      || ed25519_pub(32)
 *
 * (bramble_identity_attestation_signed_msg builds it; the Phase 3 pinning
 * verifier checks these same bytes). The header is deliberately NOT
 * covered: hop_limit is relay-mutable and packet_id is per-send, while the
 * identity claim itself is stable across re-sends and relays.
 *
 * TWO authenticators, two jobs (Phase 3):
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
    uint8_t auth_hmac[8]; /* network-key relay gate (Phase 3) */
    uint8_t seq[6];       /* 48-bit origin seq, big-endian (Phase 3) */
} bramble_identity_attestation_t;

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
 * (main/mesh_task.c's send_data_packet/send_dm_packet/mesh_send_location_packet
 * build this layout; handle_data/handle_location parse it; gosim's bridge.c
 * uses its own out-of-band src_addr/prev_hop tracking instead of these wire
 * bytes, since its DATA framing already diverges from firmware's -- see
 * task-4-report.md).
 *
 * prev_hop is RELAY-MUTABLE and MAC-EXCLUDED, mirroring RREP's relay-mutable
 * next_hop (#119): every node that transmits this frame -- the originator on
 * first TX, then each relay before it retransmits -- overwrites prev_hop
 * with its OWN address, so any receiver always knows the address of the
 * radio it just heard this specific frame from, regardless of how many hops
 * it has already travelled. Because every hop rewrites it, prev_hop cannot
 * live under the AEAD tag. It also is NOT fed into bramble_build_aead_aad:
 * that helper's AAD buffer is unchanged by this field's addition (still
 * HEADER_SIZE + 4 bytes: the masked header plus src_addr only), so prev_hop
 * simply sits at a wire offset the AAD build never reads, exactly the way
 * nonce/ciphertext/tag already sit outside it. There is nothing to "zero":
 * exclusion here is structural (prev_hop's offset is never copied into the
 * AAD buffer at all), not a masked-in-place byte like hop_limit.
 *
 * auth_hmac (Task 4-fix F1, wire v4) is an 8-byte NETWORK-KEY MAC the
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
 * Like all control-plane auth here it is forgeable under the unprovisioned
 * public-PSK fallback key (network_key.h) -- the accepted, documented
 * baseline, not closed by this field.
 *
 * src_addr stays AAD-bound, so the reverse route's TARGET (who a returning
 * confirmation is ultimately for) cannot be spoofed by an on-path relay;
 * only the NEXT-HOP hint is unauthenticated. Residual (see
 * docs/SECURITY-MODEL.md): a malicious relay can lie about prev_hop to
 * attract or blackhole reverse traffic -- the same insider-forwarding
 * residual already accepted for the RREP control plane.
 */
#define BRAMBLE_DATA_SRC_ADDR_OFFSET (HEADER_SIZE)
#define BRAMBLE_DATA_PREV_HOP_OFFSET (HEADER_SIZE + 4)
#define BRAMBLE_DATA_AUTH_HMAC_OFFSET (HEADER_SIZE + 8)
#define BRAMBLE_DATA_AUTH_HMAC_SIZE 8
#define BRAMBLE_DATA_NONCE_OFFSET (HEADER_SIZE + 16)
/* header + src_addr + prev_hop + auth_hmac, i.e. where the AEAD nonce begins */
#define BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE (HEADER_SIZE + 16)

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
