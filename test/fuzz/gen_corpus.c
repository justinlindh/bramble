/* Regenerates the committed seed corpora under test/fuzz/corpus/.
 *
 * The seeds are the wire frames from the existing host suites, test_packet.c
 * and test_fragment.c, re-emitted in each harness's input encoding. Starting
 * libFuzzer from valid frames means the very first mutations land on real
 * field boundaries instead of burning the whole budget rediscovering that a
 * beacon is 54 bytes long.
 *
 * The corpus is committed, so this program only needs to run when a wire
 * format changes:
 *
 *   bash test/fuzz/run_fuzz.sh --regen-corpus
 *
 * Usage: gen_corpus <corpus_root>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fragment.h"
#include "packet.h"

/* Harness selectors, mirroring the switch in fuzz_packet.c. */
enum {
    SEL_HEADER = 0,
    SEL_ACK = 1,
    SEL_RREQ = 2,
    SEL_RREP = 3,
    SEL_RERR = 4,
    SEL_BEACON = 5,
    SEL_KEY_EXCHANGE = 6,
    SEL_DELIVERY_RECEIPT = 7,
    SEL_IDENTITY_ATTESTATION = 8,
};

static const char* g_root;
static int g_failed;

static void emit(const char* subdir, const char* name, const uint8_t* buf, size_t len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", g_root, subdir, name);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "gen_corpus: cannot write %s\n", path);
        g_failed = 1;
        return;
    }
    if (len && fwrite(buf, 1, len, f) != len) {
        fprintf(stderr, "gen_corpus: short write to %s\n", path);
        g_failed = 1;
    }
    fclose(f);
    printf("  %s/%s (%zu bytes)\n", subdir, name, len);
}

/* Prefix a serialized frame with its harness selector and write it out. */
static void emit_frame(const char* name, uint8_t selector, const uint8_t* frame, size_t len) {
    uint8_t buf[BRAMBLE_MAX_PACKET_SIZE + 1];
    if (len + 1 > sizeof(buf)) {
        fprintf(stderr, "gen_corpus: frame %s too large\n", name);
        g_failed = 1;
        return;
    }
    buf[0] = selector;
    memcpy(buf + 1, frame, len);
    emit("packet", name, buf, len + 1);
}

/* The header shape used throughout test_packet.c. */
static bramble_header_t make_header(uint8_t type) {
    bramble_header_t h = {
        .version = BRAMBLE_VERSION,
        .type = type,
        .flags = FLAG_ACK_REQ | FLAG_ENCRYPT,
        .hop_limit = 7,
        .dest_addr = 0xDEADBEEF,
        .packet_id = 0x12345678,
    };
    return h;
}

static void fill(uint8_t* p, size_t n, uint8_t start) {
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(start + i);
}

static void gen_packet_corpus(void) {
    uint8_t frame[BRAMBLE_MAX_PACKET_SIZE];

    bramble_header_t h = make_header(PKT_TYPE_DATA);
    bramble_header_serialize(&h, frame, sizeof(frame));
    emit_frame("header", SEL_HEADER, frame, HEADER_SIZE);
    /* One byte short: the size-rejection path every parser shares. */
    emit_frame("header_truncated", SEL_HEADER, frame, HEADER_SIZE - 1);

    bramble_ack_t ack = {0};
    ack.header = make_header(PKT_TYPE_ACK);
    ack.src_addr = 0xCAFEBABE;
    ack.ack_packet_id = 0x11223344;
    ack.ack_flags = 0x01;
    ack.rssi_at_dest = -75;
    ack.hop_count = 0;
    fill(ack.auth_hmac, sizeof(ack.auth_hmac), 0xA0);
    fill(ack.seq, sizeof(ack.seq), 0xB0);
    bramble_ack_serialize(&ack, frame, sizeof(frame));
    emit_frame("ack_0hop", SEL_ACK, frame, bramble_ack_wire_size(&ack));
    /* A full relay path exercises the hop_count clamp and the trailing
     * variable-length region in one seed. */
    ack.hop_count = ACK_MAX_HOPS;
    for (int i = 0; i < ACK_MAX_HOPS; i++)
        ack.relay_path[i] = 0x1000 + (uint32_t)i;
    bramble_ack_serialize(&ack, frame, sizeof(frame));
    emit_frame("ack_maxhop", SEL_ACK, frame, bramble_ack_wire_size(&ack));

    bramble_rreq_t rreq = {0};
    rreq.header = make_header(PKT_TYPE_RREQ);
    rreq.query_id = 0xAABBCCDD;
    rreq.encrypted_source = 0x01020304;
    rreq.hop_count = 3;
    rreq.metric = 200;
    rreq.prev_hop = 0x05060708;
    rreq.rreq_salt = 0x090A0B0C;
    bramble_rreq_serialize(&rreq, frame, sizeof(frame));
    emit_frame("rreq", SEL_RREQ, frame, RREQ_SIZE);

    bramble_rrep_t rrep = {0};
    rrep.header = make_header(PKT_TYPE_RREP);
    rrep.query_id = 0xAABBCCDD;
    rrep.src_addr = 0x11112222;
    rrep.next_hop = 0x33334444;
    rrep.hop_count = 2;
    rrep.route_metric = 128;
    fill(rrep.auth_hmac, sizeof(rrep.auth_hmac), 0xC0);
    fill(rrep.seq, sizeof(rrep.seq), 0xD0);
    bramble_rrep_serialize(&rrep, frame, sizeof(frame));
    emit_frame("rrep", SEL_RREP, frame, RREP_SIZE);

    bramble_rerr_t rerr = {0};
    rerr.header = make_header(PKT_TYPE_RERR);
    rerr.reporter_addr = 0x55556666;
    rerr.broken_dest = 0x77778888;
    rerr.broken_next_hop = 0x9999AAAA;
    fill(rerr.auth_hmac, sizeof(rerr.auth_hmac), 0xE0);
    fill(rerr.seq, sizeof(rerr.seq), 0xF0);
    bramble_rerr_serialize(&rerr, frame, sizeof(frame));
    emit_frame("rerr", SEL_RERR, frame, RERR_SIZE);

    bramble_beacon_t beacon = {0};
    beacon.header = make_header(PKT_TYPE_BEACON);
    beacon.src_addr = 0x0BADF00D;
    beacon.pubkey_hash = 0xFEEDFACE;
    beacon.uptime_min = 1234;
    beacon.battery_pct = 88;
    beacon.tx_queue_depth = 3;
    beacon.neighbor_count = 5;
    beacon.flags = 0x02;
    beacon.network_time = 0x5F5E1000;
    beacon.time_confidence = 900;
    fill(beacon.seq, sizeof(beacon.seq), 0x10);
    fill(beacon.auth_hmac, sizeof(beacon.auth_hmac), 0x20);
    beacon.name_len = 0;
    bramble_beacon_serialize(&beacon, frame, sizeof(frame));
    emit_frame("beacon_noname", SEL_BEACON, frame, bramble_beacon_wire_size(&beacon));
    /* A maximum-length name puts the mutator right next to the name_len
     * bound check, the interesting edge in bramble_beacon_deserialize. */
    memcpy(beacon.name, "bramble-node-abc", BEACON_NAME_MAX);
    beacon.name[BEACON_NAME_MAX] = '\0';
    beacon.name_len = BEACON_NAME_MAX;
    bramble_beacon_serialize(&beacon, frame, sizeof(frame));
    emit_frame("beacon_maxname", SEL_BEACON, frame, bramble_beacon_wire_size(&beacon));
    /* name_len declaring more bytes than the frame carries: the state
     * inconsistency filed as issue #77, kept in the corpus so a future fix
     * has a seed that reaches it immediately. */
    frame[BEACON_SIZE] = BEACON_NAME_MAX;
    emit_frame("beacon_name_overrun", SEL_BEACON, frame, BEACON_SIZE + 2);

    bramble_key_exchange_t ke = {0};
    ke.header = make_header(PKT_TYPE_KEY_EXCHANGE);
    ke.src_addr = 0x0A0B0C0D;
    fill(ke.ephemeral_pubkey, sizeof(ke.ephemeral_pubkey), 0x30);
    fill(ke.long_term_pubkey, sizeof(ke.long_term_pubkey), 0x50);
    ke.key_id = 7;
    ke.ke_type = 1;
    fill(ke.auth_tag, sizeof(ke.auth_tag), 0x70);
    bramble_key_exchange_serialize(&ke, frame, sizeof(frame));
    emit_frame("key_exchange", SEL_KEY_EXCHANGE, frame, KEY_EXCHANGE_SIZE);

    bramble_delivery_receipt_t dr = {0};
    dr.header = make_header(PKT_TYPE_DELIVERY_RECEIPT);
    dr.src_addr = 0x0D0E0F10;
    dr.orig_packet_id = 0x12345678;
    dr.total_latency = 42;
    fill(dr.auth_hmac, sizeof(dr.auth_hmac), 0x80);
    fill(dr.seq, sizeof(dr.seq), 0x90);
    dr.hop_count = 0;
    bramble_delivery_receipt_serialize(&dr, frame, sizeof(frame));
    emit_frame("delivery_receipt_0hop", SEL_DELIVERY_RECEIPT, frame, DELIVERY_RECEIPT_MIN_SIZE);
    dr.hop_count = DELIVERY_RECEIPT_MAX_HOPS;
    for (int i = 0; i < DELIVERY_RECEIPT_MAX_HOPS; i++)
        dr.relay_path[i] = 0x2000 + (uint32_t)i;
    bramble_delivery_receipt_serialize(&dr, frame, sizeof(frame));
    emit_frame("delivery_receipt_maxhop", SEL_DELIVERY_RECEIPT, frame,
               DELIVERY_RECEIPT_MIN_SIZE + DELIVERY_RECEIPT_MAX_HOPS * 4);

    bramble_identity_attestation_t ia = {0};
    ia.header = make_header(PKT_TYPE_IDENTITY_ATTESTATION);
    ia.src_addr = 0x1A2B3C4D;
    fill(ia.x25519_pub, sizeof(ia.x25519_pub), 0x01);
    fill(ia.ed25519_pub, sizeof(ia.ed25519_pub), 0x21);
    fill(ia.sig, sizeof(ia.sig), 0x41);
    ia.not_after = 0x0000000160000000ULL;
    fill(ia.endorsement_sig, sizeof(ia.endorsement_sig), 0x81);
    fill(ia.auth_hmac, sizeof(ia.auth_hmac), 0xC1);
    fill(ia.seq, sizeof(ia.seq), 0xD1);
    bramble_identity_attestation_serialize(&ia, frame, sizeof(frame));
    emit_frame("identity_attestation", SEL_IDENTITY_ATTESTATION, frame, IDENTITY_ATTESTATION_SIZE);
}

/* Fragment harness commands: op, index, total, mid_lo, mid_hi, len, dt. */
enum {
    OP_ADD = 0,
    OP_COLLECT = 1,
    OP_PURGE = 2,
    OP_FIRST_ID = 3,
    OP_SPLIT = 4,
};

typedef struct {
    uint8_t buf[2048];
    size_t len;
} stream_t;

static void cmd(stream_t* s, uint8_t op, uint8_t index, uint8_t total, uint16_t mid,
                uint8_t payload_len, uint8_t dt, uint8_t payload_fill) {
    s->buf[s->len++] = op;
    s->buf[s->len++] = index;
    s->buf[s->len++] = total;
    s->buf[s->len++] = (uint8_t)(mid & 0xFF);
    s->buf[s->len++] = (uint8_t)(mid >> 8);
    s->buf[s->len++] = payload_len;
    s->buf[s->len++] = dt;
    for (uint8_t i = 0; i < payload_len; i++)
        s->buf[s->len++] = (uint8_t)(payload_fill + i);
}

static void gen_fragment_corpus(void) {
    stream_t s;

    /* A clean two-fragment message, added in order and collected: the
     * happy path from test_fragment.c's reassembly tests. */
    s.len = 0;
    cmd(&s, OP_ADD, 0, 2, 0x1234, FRAG_MAX_PLAINTEXT, 0, 0x00);
    cmd(&s, OP_ADD, 1, 2, 0x1234, 40, 10, 0x40);
    cmd(&s, OP_COLLECT, 0, 2, 0x1234, 0, 0, 0x00);
    emit("fragment", "reassemble_inorder", s.buf, s.len);

    /* Same message delivered back to front, plus a duplicate. */
    s.len = 0;
    cmd(&s, OP_ADD, 3, 4, 0x00AA, 20, 0, 0x10);
    cmd(&s, OP_ADD, 1, 4, 0x00AA, FRAG_MAX_PLAINTEXT, 5, 0x20);
    cmd(&s, OP_ADD, 1, 4, 0x00AA, FRAG_MAX_PLAINTEXT, 5, 0x20);
    cmd(&s, OP_ADD, 0, 4, 0x00AA, FRAG_MAX_PLAINTEXT, 5, 0x30);
    cmd(&s, OP_ADD, 2, 4, 0x00AA, FRAG_MAX_PLAINTEXT, 5, 0x40);
    cmd(&s, OP_COLLECT, 0, 4, 0x00AA, 0, 0, 0x00);
    emit("fragment", "reassemble_reordered", s.buf, s.len);

    /* Four concurrent messages fill every slot, then a fifth is refused:
     * the exhaustion path an insider can hold open for 30 seconds. */
    s.len = 0;
    for (uint8_t m = 0; m < 5; m++)
        cmd(&s, OP_ADD, 0, 2, (uint16_t)(0x100 + m), 16, 1, (uint8_t)(m * 16));
    cmd(&s, OP_PURGE, 0, 0, 0, 0, 200, 0x00);
    emit("fragment", "slot_exhaustion", s.buf, s.len);

    /* A second fragment declaring a different frag_total than the one that
     * opened the slot: issue #77 problem 2, seeded so the reproducer is on
     * hand when that recheck lands. */
    s.len = 0;
    cmd(&s, OP_ADD, 0, 1, 0x0BAD, 8, 0, 0x00);
    cmd(&s, OP_ADD, 3, 4, 0x0BAD, 8, 0, 0x11);
    cmd(&s, OP_COLLECT, 0, 4, 0x0BAD, 0, 0, 0x00);
    cmd(&s, OP_FIRST_ID, 0, 4, 0x0BAD, 0, 0, 0x00);
    emit("fragment", "total_mismatch", s.buf, s.len);

    /* Timeout expiry mid-message, then a purge sweep. */
    s.len = 0;
    cmd(&s, OP_ADD, 0, 2, 0x2222, 32, 0, 0x00);
    for (int i = 0; i < 130; i++)
        cmd(&s, OP_FIRST_ID, 0, 2, 0x2222, 0, 255, 0x00);
    cmd(&s, OP_ADD, 1, 2, 0x2222, 32, 255, 0x00);
    cmd(&s, OP_PURGE, 0, 0, 0, 0, 255, 0x00);
    emit("fragment", "timeout_expiry", s.buf, s.len);

    /* Out-of-range header fields: index past total, total past the maximum,
     * and a payload longer than FRAG_MAX_PLAINTEXT. */
    s.len = 0;
    cmd(&s, OP_ADD, 7, 2, 0x3333, 8, 0, 0x00);
    cmd(&s, OP_ADD, 0, 255, 0x3333, 8, 0, 0x00);
    cmd(&s, OP_ADD, 0, 2, 0x3333, 200, 0, 0x00);
    emit("fragment", "header_out_of_range", s.buf, s.len);

    /* The transmit side at both ends of its range. */
    s.len = 0;
    cmd(&s, OP_SPLIT, 0, FRAG_MAX_FRAGMENTS, 0x4444, 100, 0, 0x00);
    cmd(&s, OP_SPLIT, 0, FRAG_MAX_FRAGMENTS, 0x4444, 255, 0, 0x00);
    cmd(&s, OP_SPLIT, 0, 0, 0x4444, 10, 0, 0x00);
    emit("fragment", "split", s.buf, s.len);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <corpus_root>\n", argv[0]);
        return 2;
    }
    g_root = argv[1];
    printf("Regenerating seed corpora under %s\n", g_root);
    gen_packet_corpus();
    gen_fragment_corpus();
    return g_failed ? 1 : 0;
}
