/* libFuzzer harness for the Bramble wire-frame parsers in
 * components/packet/packet.c.
 *
 * Every function reached from here runs on attacker-controlled LoRa bytes
 * BEFORE any AEAD tag or HMAC is verified, which is what makes this the
 * highest-value fuzz target in the firmware (issue #76).
 *
 * Input encoding: byte 0 selects the parser, the remaining bytes are the
 * frame handed to it verbatim. The frame is copied into an exactly-sized
 * heap allocation so ASan redzones sit immediately past its last byte and
 * any one-byte over-read in a deserializer is a hard failure, which a
 * generously-sized stack buffer would silently absorb.
 *
 * Build and run: bash test/fuzz/run_fuzz.sh
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "packet.h"

/* Round-trip a parsed struct back onto the wire through an exactly-sized
 * heap buffer. This exercises the serializers with field values no honest
 * peer would ever produce, under the same redzone discipline as the parse
 * side. The re-serialized bytes are deliberately not compared against the
 * input: several frames carry reserved or clamped fields where a
 * non-identical round trip is correct behavior, and asserting equality
 * would test the harness's assumptions rather than the parser.
 */
#define ROUNDTRIP(fn, p, size)                                                                     \
    do {                                                                                           \
        size_t out_len = (size);                                                                   \
        uint8_t* out = (uint8_t*)malloc(out_len);                                                  \
        if (out) {                                                                                 \
            (void)fn((p), out, out_len);                                                           \
            free(out);                                                                             \
        }                                                                                          \
    } while (0)

static void fuzz_header(const uint8_t* buf, size_t len) {
    bramble_header_t h;
    if (bramble_header_deserialize(&h, buf, len) != ESP_OK)
        return;
    (void)bramble_header_is_supported_version(&h);

    uint8_t* aad = (uint8_t*)malloc(HEADER_SIZE);
    if (aad) {
        if (bramble_header_build_aad(&h, aad, HEADER_SIZE) == ESP_OK)
            assert(aad[3] == 0); /* hop_limit is relay-mutated, never authenticated */
        free(aad);
    }

    uint8_t* aead_aad = (uint8_t*)malloc(HEADER_SIZE + 4);
    if (aead_aad) {
        (void)bramble_build_aead_aad(&h, h.dest_addr, aead_aad, HEADER_SIZE + 4);
        free(aead_aad);
    }

    ROUNDTRIP(bramble_header_serialize, &h, HEADER_SIZE);
}

static void fuzz_ack(const uint8_t* buf, size_t len) {
    bramble_ack_t p;
    if (bramble_ack_deserialize(&p, buf, len) != ESP_OK)
        return;
    /* hop_count gates a relay_path[] walk in main/mesh_task.c's handle_ack,
     * so it must always come back inside the array bound. */
    assert(p.hop_count <= ACK_MAX_HOPS);
    ROUNDTRIP(bramble_ack_serialize, &p, bramble_ack_wire_size(&p));
}

static void fuzz_rreq(const uint8_t* buf, size_t len) {
    bramble_rreq_t p;
    if (bramble_rreq_deserialize(&p, buf, len) != ESP_OK)
        return;
    ROUNDTRIP(bramble_rreq_serialize, &p, RREQ_SIZE);
}

static void fuzz_rrep(const uint8_t* buf, size_t len) {
    bramble_rrep_t p;
    if (bramble_rrep_deserialize(&p, buf, len) != ESP_OK)
        return;
    ROUNDTRIP(bramble_rrep_serialize, &p, RREP_SIZE);
}

static void fuzz_rerr(const uint8_t* buf, size_t len) {
    bramble_rerr_t p;
    if (bramble_rerr_deserialize(&p, buf, len) != ESP_OK)
        return;
    ROUNDTRIP(bramble_rerr_serialize, &p, RERR_SIZE);
}

static void fuzz_beacon(const uint8_t* buf, size_t len) {
    bramble_beacon_t p;
    if (bramble_beacon_deserialize(&p, buf, len) != ESP_OK)
        return;
    /* name_len indexes name[BEACON_NAME_MAX + 1] for the NUL terminator and
     * bounds a copy into the neighbor table in main/mesh_task.c. */
    assert(p.name_len <= BEACON_NAME_MAX);
    assert(p.name[p.name_len] == '\0');
    ROUNDTRIP(bramble_beacon_serialize, &p, bramble_beacon_wire_size(&p));
}

static void fuzz_key_exchange(const uint8_t* buf, size_t len) {
    bramble_key_exchange_t p;
    if (bramble_key_exchange_deserialize(&p, buf, len) != ESP_OK)
        return;
    ROUNDTRIP(bramble_key_exchange_serialize, &p, KEY_EXCHANGE_SIZE);
}

static void fuzz_delivery_receipt(const uint8_t* buf, size_t len) {
    bramble_delivery_receipt_t p;
    if (bramble_delivery_receipt_deserialize(&p, buf, len) != ESP_OK)
        return;
    assert(p.hop_count <= DELIVERY_RECEIPT_MAX_HOPS);
    ROUNDTRIP(bramble_delivery_receipt_serialize, &p,
              DELIVERY_RECEIPT_MIN_SIZE + (size_t)p.hop_count * 4);
}

static void fuzz_identity_attestation(const uint8_t* buf, size_t len) {
    bramble_identity_attestation_t p;
    if (bramble_identity_attestation_deserialize(&p, buf, len) != ESP_OK)
        return;
    ROUNDTRIP(bramble_identity_attestation_serialize, &p, IDENTITY_ATTESTATION_SIZE);
    ROUNDTRIP(bramble_identity_attestation_signed_msg, &p, IDENTITY_ATTESTATION_MSG_SIZE);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1)
        return 0;

    uint8_t selector = data[0];
    size_t frame_len = size - 1;

    /* malloc(0) may return NULL or a unique pointer; either is fine to pass
     * along, every deserializer rejects a zero-length frame on size first. */
    uint8_t* frame = (uint8_t*)malloc(frame_len ? frame_len : 1);
    if (!frame)
        return 0;
    memcpy(frame, data + 1, frame_len);

    switch (selector % 9) {
    case 0:
        fuzz_header(frame, frame_len);
        break;
    case 1:
        fuzz_ack(frame, frame_len);
        break;
    case 2:
        fuzz_rreq(frame, frame_len);
        break;
    case 3:
        fuzz_rrep(frame, frame_len);
        break;
    case 4:
        fuzz_rerr(frame, frame_len);
        break;
    case 5:
        fuzz_beacon(frame, frame_len);
        break;
    case 6:
        fuzz_key_exchange(frame, frame_len);
        break;
    case 7:
        fuzz_delivery_receipt(frame, frame_len);
        break;
    default:
        fuzz_identity_attestation(frame, frame_len);
        break;
    }

    free(frame);
    return 0;
}
