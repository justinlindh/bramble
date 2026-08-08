/*
 * Attested roll-call core: wire codecs, the signed-message canonicalization,
 * the response stagger, the round schedule, the initiation rate limit, the
 * member-side answer-once table, and the initiator ledger.
 *
 * The Ed25519 half is exercised here too (sign with one identity, verify
 * with another's view of it) because the whole point of the primitive is
 * that a response cannot be minted by a network-key insider on someone
 * else's behalf: a test that only round-trips the bytes would not catch a
 * canonical message that both sides build the same WRONG way.
 */
#include "unity.h"

#include "rollcall.h"
#include "crypto.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Wire ───────────────────────────────────────────────────────────── */

void test_announce_round_trips(void) {
    rollcall_announce_t in;
    memset(&in, 0, sizeof(in));
    in.rollcall_id = 0xDEADBEEFu;
    in.round = 2;
    in.text_len = 5;
    memcpy(in.text, "check", 5);

    uint8_t buf[ROLLCALL_ANNOUNCE_MAX_SIZE];
    size_t n = rollcall_announce_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t((size_t)ROLLCALL_ANNOUNCE_HEADER_SIZE + 5u, n);

    /* Pin the byte order explicitly: a wire field that silently flips
     * endianness still round-trips through its own codec. */
    TEST_ASSERT_EQUAL_UINT8(0xDE, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAD, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xBE, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0xEF, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(2, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(5, buf[5]);

    rollcall_announce_t out;
    TEST_ASSERT_TRUE(rollcall_announce_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, out.rollcall_id);
    TEST_ASSERT_EQUAL_UINT8(2, out.round);
    TEST_ASSERT_EQUAL_UINT8(5, out.text_len);
    TEST_ASSERT_EQUAL_STRING("check", out.text);
}

void test_announce_empty_text_is_valid(void) {
    rollcall_announce_t in;
    memset(&in, 0, sizeof(in));
    in.rollcall_id = 7;
    in.round = 1;
    in.text_len = 0;

    uint8_t buf[ROLLCALL_ANNOUNCE_MAX_SIZE];
    size_t n = rollcall_announce_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t((size_t)ROLLCALL_ANNOUNCE_HEADER_SIZE, n);

    rollcall_announce_t out;
    TEST_ASSERT_TRUE(rollcall_announce_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(0, out.text_len);
    TEST_ASSERT_EQUAL_STRING("", out.text);
}

void test_announce_rejects_malformed(void) {
    uint8_t buf[ROLLCALL_ANNOUNCE_MAX_SIZE];
    rollcall_announce_t out;

    /* Too short for the fixed header. */
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_FALSE(rollcall_announce_decode(buf, ROLLCALL_ANNOUNCE_HEADER_SIZE - 1u, &out));

    /* id 0 is the reserved "no roll-call" value, so an all-zero buffer must
     * never decode into something plausible. */
    TEST_ASSERT_FALSE(rollcall_announce_decode(buf, ROLLCALL_ANNOUNCE_HEADER_SIZE, &out));

    /* Round out of range, both directions. */
    rollcall_announce_t in;
    memset(&in, 0, sizeof(in));
    in.rollcall_id = 1;
    in.round = 1;
    size_t n = rollcall_announce_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    buf[4] = 0;
    TEST_ASSERT_FALSE(rollcall_announce_decode(buf, n, &out));
    buf[4] = ROLLCALL_MAX_ROUNDS + 1;
    TEST_ASSERT_FALSE(rollcall_announce_decode(buf, n, &out));
    buf[4] = 1;

    /* Declared text_len that disagrees with the buffer length, both ways:
     * a trailing-byte smuggle and a truncation. */
    buf[5] = 4;
    TEST_ASSERT_FALSE(rollcall_announce_decode(buf, n, &out));
    buf[5] = 0;
    TEST_ASSERT_FALSE(rollcall_announce_decode(buf, n + 3u, &out));

    /* text_len over the cap. */
    buf[5] = ROLLCALL_TEXT_MAX + 1;
    TEST_ASSERT_FALSE(
        rollcall_announce_decode(buf, ROLLCALL_ANNOUNCE_HEADER_SIZE + ROLLCALL_TEXT_MAX + 1u, &out));
}

void test_announce_encode_rejects_bad_inputs(void) {
    uint8_t buf[ROLLCALL_ANNOUNCE_MAX_SIZE];
    rollcall_announce_t in;

    memset(&in, 0, sizeof(in));
    in.rollcall_id = 0; /* reserved */
    in.round = 1;
    TEST_ASSERT_EQUAL_size_t(0, rollcall_announce_encode(&in, buf, sizeof(buf)));

    in.rollcall_id = 1;
    in.round = ROLLCALL_MAX_ROUNDS + 1;
    TEST_ASSERT_EQUAL_size_t(0, rollcall_announce_encode(&in, buf, sizeof(buf)));

    in.round = 1;
    in.text_len = ROLLCALL_TEXT_MAX + 1;
    TEST_ASSERT_EQUAL_size_t(0, rollcall_announce_encode(&in, buf, sizeof(buf)));

    /* Output buffer one byte short of the frame. */
    in.text_len = 4;
    TEST_ASSERT_EQUAL_size_t(0, rollcall_announce_encode(&in, buf,
                                                         ROLLCALL_ANNOUNCE_HEADER_SIZE + 3u));
}

void test_response_round_trips_and_is_fixed_length(void) {
    rollcall_response_t in;
    memset(&in, 0, sizeof(in));
    in.rollcall_id = 0x01020304u;
    in.round = 3;
    in.responder_addr = 0xA1B2C3D4u;
    for (int i = 0; i < ROLLCALL_SIG_SIZE; i++)
        in.sig[i] = (uint8_t)(i * 3u + 1u);

    uint8_t buf[ROLLCALL_RESPONSE_SIZE + 4];
    TEST_ASSERT_EQUAL_size_t(ROLLCALL_RESPONSE_SIZE,
                             rollcall_response_encode(&in, buf, sizeof(buf)));

    rollcall_response_t out;
    TEST_ASSERT_TRUE(rollcall_response_decode(buf, ROLLCALL_RESPONSE_SIZE, &out));
    TEST_ASSERT_EQUAL_UINT32(0x01020304u, out.rollcall_id);
    TEST_ASSERT_EQUAL_UINT8(3, out.round);
    TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4u, out.responder_addr);
    TEST_ASSERT_EQUAL_MEMORY(in.sig, out.sig, ROLLCALL_SIG_SIZE);

    /* Exact-length only: a short frame and a long frame are both rejected,
     * so no caller can be handed a partially-populated signature. */
    TEST_ASSERT_FALSE(rollcall_response_decode(buf, ROLLCALL_RESPONSE_SIZE - 1u, &out));
    TEST_ASSERT_FALSE(rollcall_response_decode(buf, ROLLCALL_RESPONSE_SIZE + 1u, &out));
}

/* ── Signed message ─────────────────────────────────────────────────── */

void test_signed_msg_layout_is_domain_separated_and_bound(void) {
    uint8_t msg[ROLLCALL_MSG_SIZE];
    TEST_ASSERT_EQUAL_size_t(ROLLCALL_MSG_SIZE,
                             rollcall_signed_msg(0x11223344u, 0xAAAAAAAAu, 0xBBBBBBBBu, msg,
                                                 sizeof(msg)));
    TEST_ASSERT_EQUAL_MEMORY(ROLLCALL_MSG_CONTEXT, msg, ROLLCALL_MSG_CONTEXT_LEN);
    TEST_ASSERT_EQUAL_UINT8(0x11, msg[ROLLCALL_MSG_CONTEXT_LEN]);
    TEST_ASSERT_EQUAL_UINT8(0x44, msg[ROLLCALL_MSG_CONTEXT_LEN + 3]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, msg[ROLLCALL_MSG_CONTEXT_LEN + 4]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, msg[ROLLCALL_MSG_CONTEXT_LEN + 8]);

    /* Too small a buffer writes nothing. */
    uint8_t small[ROLLCALL_MSG_SIZE - 1];
    TEST_ASSERT_EQUAL_size_t(0, rollcall_signed_msg(1, 2, 3, small, sizeof(small)));
}

void test_signature_binds_rollcall_initiator_and_responder(void) {
    uint8_t pub[BRAMBLE_ED25519_PUBKEY_SIZE];
    uint8_t priv[BRAMBLE_ED25519_SECKEY_SIZE];
    uint8_t seed[32];
    for (int i = 0; i < 32; i++)
        seed[i] = (uint8_t)(0x40 + i);
    TEST_ASSERT_EQUAL_INT(0, crypto_ed25519_keypair_from_seed(seed, pub, priv));

    const uint32_t rc_id = 0x0BADC0DEu;
    const uint32_t initiator = 0x11111111u;
    const uint32_t responder = 0x22222222u;

    uint8_t msg[ROLLCALL_MSG_SIZE];
    TEST_ASSERT_EQUAL_size_t(ROLLCALL_MSG_SIZE,
                             rollcall_signed_msg(rc_id, initiator, responder, msg, sizeof(msg)));

    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];
    TEST_ASSERT_EQUAL_INT(0, crypto_ed25519_sign(priv, msg, sizeof(msg), sig));
    TEST_ASSERT_TRUE(crypto_ed25519_verify(pub, msg, sizeof(msg), sig));

    /* Every field the message binds must break the signature when changed.
     * The roll-call id stops a response being replayed into a later
     * roll-call; the initiator stops it being replayed into a DIFFERENT
     * operator's roll-call that drew the same id; the responder is what
     * stops a keyed insider answering on a victim's behalf. */
    uint8_t other[ROLLCALL_MSG_SIZE];
    rollcall_signed_msg(rc_id + 1u, initiator, responder, other, sizeof(other));
    TEST_ASSERT_FALSE(crypto_ed25519_verify(pub, other, sizeof(other), sig));

    rollcall_signed_msg(rc_id, initiator + 1u, responder, other, sizeof(other));
    TEST_ASSERT_FALSE(crypto_ed25519_verify(pub, other, sizeof(other), sig));

    rollcall_signed_msg(rc_id, initiator, responder + 1u, other, sizeof(other));
    TEST_ASSERT_FALSE(crypto_ed25519_verify(pub, other, sizeof(other), sig));
}

void test_another_members_key_cannot_forge_a_response(void) {
    /* The insider case in one test: a second, fully legitimate fleet member
     * (it holds the network key, so every transport-level check it faces
     * passes) signs a response naming the VICTIM as responder. The victim's
     * pinned key rejects it, which is the whole reason the response carries
     * an identity signature instead of relying on the network-key MAC. */
    uint8_t victim_pub[BRAMBLE_ED25519_PUBKEY_SIZE];
    uint8_t victim_priv[BRAMBLE_ED25519_SECKEY_SIZE];
    uint8_t insider_pub[BRAMBLE_ED25519_PUBKEY_SIZE];
    uint8_t insider_priv[BRAMBLE_ED25519_SECKEY_SIZE];
    uint8_t seed_a[32], seed_b[32];
    for (int i = 0; i < 32; i++) {
        seed_a[i] = (uint8_t)(0x10 + i);
        seed_b[i] = (uint8_t)(0x90 + i);
    }
    TEST_ASSERT_EQUAL_INT(0, crypto_ed25519_keypair_from_seed(seed_a, victim_pub, victim_priv));
    TEST_ASSERT_EQUAL_INT(0, crypto_ed25519_keypair_from_seed(seed_b, insider_pub, insider_priv));

    const uint32_t victim_addr = 0xB0B0B0B0u;
    uint8_t msg[ROLLCALL_MSG_SIZE];
    rollcall_signed_msg(0xFEEDFACEu, 0xAAAAAAAAu, victim_addr, msg, sizeof(msg));
    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];
    TEST_ASSERT_EQUAL_INT(0, crypto_ed25519_sign(insider_priv, msg, sizeof(msg), sig));

    /* The initiator verifies against the pin it holds for victim_addr, so
     * the insider's signature over the victim's canonical message fails. */
    TEST_ASSERT_FALSE(crypto_ed25519_verify(victim_pub, msg, sizeof(msg), sig));
    /* The same bytes DO verify under the insider's own key, which is what
     * makes this a forgery attempt rather than a corrupt signature. */
    TEST_ASSERT_TRUE(crypto_ed25519_verify(insider_pub, msg, sizeof(msg), sig));
    (void)victim_priv;
}

/* ── Response stagger ───────────────────────────────────────────────── */

void test_response_delay_is_bounded_by_the_slot_window(void) {
    const uint32_t rc_id = 0xCAFEBABEu;
    /* Dense mesh: the full 32-bucket window. */
    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t d = rollcall_response_delay_ms(0x01000000u + i, rc_id, 40u, i * 7919u);
        TEST_ASSERT_TRUE(d >= ROLLCALL_RESPONSE_BASE_MS);
        TEST_ASSERT_TRUE(d < ROLLCALL_RESPONSE_BASE_MS +
                                  ROLLCALL_RESPONSE_SLOT_MS * ROLLCALL_SLOT_BUCKETS_MAX +
                                  ROLLCALL_RESPONSE_JITTER_MS);
    }
}

void test_response_slot_window_scales_with_peer_count(void) {
    /* A sparse mesh must not spread responses across the full window: the
     * bucket count floors at 4 and grows at two per peer, so a 2-peer mesh
     * uses 4 buckets and a 3-peer mesh uses 6. */
    bool saw_above_3 = false;
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t slot = rollcall_response_slot(i, 0u, 2u);
        TEST_ASSERT_TRUE(slot < ROLLCALL_SLOT_BUCKETS_MIN);
    }
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t slot = rollcall_response_slot(i, 0u, 3u);
        TEST_ASSERT_TRUE(slot < 6u);
        if (slot > 3u)
            saw_above_3 = true;
    }
    TEST_ASSERT_TRUE(saw_above_3);

    /* And it is capped: a huge mesh still uses at most 32 buckets. */
    for (uint32_t i = 0; i < 256u; i++) {
        TEST_ASSERT_TRUE(rollcall_response_slot(i, 0u, 200u) < ROLLCALL_SLOT_BUCKETS_MAX);
    }
}

void test_response_slot_separates_nodes_and_rerolls_per_rollcall(void) {
    /* Two different addresses must be able to land in different slots (the
     * anti-storm property), and the SAME pair must not be condemned to
     * collide on every future roll-call (the id is mixed in). */
    bool separated = false;
    for (uint32_t i = 1; i <= 32u; i++) {
        if (rollcall_response_slot(0x1000u, 0xAAAAu, 40u) !=
            rollcall_response_slot(0x1000u + i, 0xAAAAu, 40u)) {
            separated = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(separated);

    bool rerolled = false;
    for (uint32_t id = 1; id <= 64u; id++) {
        if (rollcall_response_slot(0x1000u, id, 40u) != rollcall_response_slot(0x1000u, 0u, 40u)) {
            rerolled = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(rerolled);
}

/* ── Round schedule ─────────────────────────────────────────────────── */

void test_round_schedule_backs_off_and_terminates(void) {
    TEST_ASSERT_EQUAL_UINT32(ROLLCALL_ROUND_BASE_MS, rollcall_round_delay_ms(1));
    TEST_ASSERT_EQUAL_UINT32(ROLLCALL_ROUND_BASE_MS * 2u, rollcall_round_delay_ms(2));
    /* The last round schedules nothing: the round count is a hard bound on
     * the primitive's airtime, not a starting point for a backoff that keeps
     * running. */
    TEST_ASSERT_EQUAL_UINT32(0, rollcall_round_delay_ms(ROLLCALL_MAX_ROUNDS));
    TEST_ASSERT_EQUAL_UINT32(0, rollcall_round_delay_ms(0));

    TEST_ASSERT_EQUAL_UINT32(ROLLCALL_ROUND_BASE_MS * 3u + ROLLCALL_COLLECT_TAIL_MS,
                             rollcall_window_ms());
}

/* ── Initiation rate limit ──────────────────────────────────────────── */

void test_rate_limit_bounds_initiation(void) {
    rollcall_rate_state_t rl;
    rollcall_rate_init(&rl);

    /* First ever start is allowed. */
    TEST_ASSERT_EQUAL_INT(ROLLCALL_START_OK, rollcall_rate_check(&rl, false, 1000u));
    rollcall_rate_note_start(&rl, 1000u);

    /* An open ledger reports BUSY, which is a distinct operator condition
     * from "too soon" and must not be collapsed into it. */
    TEST_ASSERT_EQUAL_INT(ROLLCALL_START_BUSY, rollcall_rate_check(&rl, true, 1000u));

    /* One millisecond short of the floor is still refused. */
    TEST_ASSERT_EQUAL_INT(ROLLCALL_START_RATE_LIMITED,
                          rollcall_rate_check(&rl, false, 1000u + ROLLCALL_MIN_INTERVAL_MS - 1u));
    TEST_ASSERT_EQUAL_INT(ROLLCALL_START_OK,
                          rollcall_rate_check(&rl, false, 1000u + ROLLCALL_MIN_INTERVAL_MS));
}

void test_rate_limit_survives_the_millisecond_rollover(void) {
    rollcall_rate_state_t rl;
    rollcall_rate_init(&rl);
    uint32_t before_wrap = 0xFFFFFF00u;
    rollcall_rate_note_start(&rl, before_wrap);

    /* 0x100 ms after the wrap: the unsigned difference is 0x200, well inside
     * the floor, so this must still be refused rather than reading as a
     * near-eternity of elapsed time. */
    TEST_ASSERT_EQUAL_INT(ROLLCALL_START_RATE_LIMITED, rollcall_rate_check(&rl, false, 0x100u));
    TEST_ASSERT_EQUAL_INT(ROLLCALL_START_OK,
                          rollcall_rate_check(&rl, false, before_wrap + ROLLCALL_MIN_INTERVAL_MS));
}

/* ── Member side ────────────────────────────────────────────────────── */

void test_member_answers_each_rollcall_once(void) {
    rollcall_seen_table_t t;
    rollcall_seen_init(&t);

    TEST_ASSERT_TRUE(rollcall_seen_claim(&t, 0x1234u, 0xAAAAu, 100u));
    /* Round 2 and round 3 of the same roll-call: already answered. This is
     * what makes re-announce rounds cost nothing for a member that heard
     * round 1. */
    TEST_ASSERT_FALSE(rollcall_seen_claim(&t, 0x1234u, 0xAAAAu, 200u));
    TEST_ASSERT_FALSE(rollcall_seen_claim(&t, 0x1234u, 0xAAAAu, 300u));
    TEST_ASSERT_TRUE(rollcall_seen_contains(&t, 0x1234u, 0xAAAAu));

    /* The same id from a DIFFERENT initiator is a different roll-call: two
     * operators can independently draw the same 32-bit id, and collapsing
     * them would silence a member for a roll-call it never answered. */
    TEST_ASSERT_TRUE(rollcall_seen_claim(&t, 0x1234u, 0xBBBBu, 400u));

    /* id 0 is reserved and never claimable. */
    TEST_ASSERT_FALSE(rollcall_seen_claim(&t, 0u, 0xAAAAu, 500u));
}

void test_member_seen_table_recycles_the_oldest(void) {
    rollcall_seen_table_t t;
    rollcall_seen_init(&t);
    for (uint32_t i = 0; i < ROLLCALL_SEEN_MAX; i++) {
        TEST_ASSERT_TRUE(rollcall_seen_claim(&t, 0x100u + i, 0xAAAAu, 1000u + i * 10u));
    }
    /* Full: one more claim evicts the oldest (0x100) and keeps the newest. */
    TEST_ASSERT_TRUE(rollcall_seen_claim(&t, 0x999u, 0xAAAAu, 5000u));
    TEST_ASSERT_FALSE(rollcall_seen_contains(&t, 0x100u, 0xAAAAu));
    TEST_ASSERT_TRUE(rollcall_seen_contains(&t, 0x999u, 0xAAAAu));
    TEST_ASSERT_TRUE(
        rollcall_seen_contains(&t, 0x100u + (ROLLCALL_SEEN_MAX - 1u), 0xAAAAu));
}

/* ── Ledger ─────────────────────────────────────────────────────────── */

static void start_anchored_ledger(rollcall_ledger_t* l, const uint32_t* expected, uint8_t n) {
    rollcall_ledger_init(l);
    TEST_ASSERT_TRUE(rollcall_ledger_start(l, 0xABCDu, 0x0001u, 1000u, "muster", 6, expected, n,
                                           true));
}

void test_ledger_start_records_the_operator_payload(void) {
    rollcall_ledger_t l;
    rollcall_ledger_init(&l);
    TEST_ASSERT_TRUE(rollcall_ledger_start(&l, 42u, 0x0001u, 1000u, "hello", 5, NULL, 0, false));
    TEST_ASSERT_TRUE(l.active);
    TEST_ASSERT_TRUE(l.open);
    TEST_ASSERT_EQUAL_UINT32(42u, l.rollcall_id);
    TEST_ASSERT_EQUAL_UINT32(1000u, l.started_ms);
    TEST_ASSERT_EQUAL_STRING("hello", l.text);
    TEST_ASSERT_FALSE(l.anchored);

    /* Reserved id and an over-long payload are both refused. */
    rollcall_ledger_t bad;
    rollcall_ledger_init(&bad);
    TEST_ASSERT_FALSE(rollcall_ledger_start(&bad, 0u, 1u, 0u, NULL, 0, NULL, 0, false));
    TEST_ASSERT_FALSE(
        rollcall_ledger_start(&bad, 1u, 1u, 0u, "x", ROLLCALL_TEXT_MAX + 1, NULL, 0, false));
    TEST_ASSERT_FALSE(bad.active);
}

void test_ledger_counts_responses_once_and_keeps_the_first_time(void) {
    rollcall_ledger_t l;
    rollcall_ledger_init(&l);
    TEST_ASSERT_TRUE(rollcall_ledger_start(&l, 7u, 0x0001u, 1000u, NULL, 0, NULL, 0, false));

    TEST_ASSERT_TRUE(rollcall_ledger_note_response(&l, 7u, 0x0002u, 1, 1500u));
    TEST_ASSERT_TRUE(rollcall_ledger_note_response(&l, 7u, 0x0003u, 1, 1700u));
    /* A retransmitted response must not move the recorded time. */
    TEST_ASSERT_TRUE(rollcall_ledger_note_response(&l, 7u, 0x0002u, 2, 9000u));

    TEST_ASSERT_EQUAL_UINT8(2, rollcall_ledger_responded_count(&l));
    const rollcall_entry_t* e = rollcall_ledger_find(&l, 0x0002u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->responded);
    TEST_ASSERT_EQUAL_UINT32(1500u, e->responded_at_ms);
    TEST_ASSERT_EQUAL_UINT8(1, e->round);

    /* Wrong roll-call id, the initiator's own address, and the null address
     * are all refused without touching the counts. */
    TEST_ASSERT_FALSE(rollcall_ledger_note_response(&l, 8u, 0x0004u, 1, 1800u));
    TEST_ASSERT_FALSE(rollcall_ledger_note_response(&l, 7u, 0x0001u, 1, 1800u));
    TEST_ASSERT_FALSE(rollcall_ledger_note_response(&l, 7u, 0u, 1, 1800u));
    TEST_ASSERT_EQUAL_UINT8(2, rollcall_ledger_responded_count(&l));
}

void test_ledger_reports_missing_only_on_an_anchored_mesh(void) {
    const uint32_t expected[4] = {0x0002u, 0x0003u, 0x0004u, 0x0005u};

    rollcall_ledger_t anchored;
    start_anchored_ledger(&anchored, expected, 4);
    TEST_ASSERT_TRUE(anchored.anchored);
    TEST_ASSERT_EQUAL_UINT8(4, anchored.expected_count);

    TEST_ASSERT_TRUE(rollcall_ledger_note_response(&anchored, 0xABCDu, 0x0002u, 1, 1500u));
    TEST_ASSERT_TRUE(rollcall_ledger_note_response(&anchored, 0xABCDu, 0x0004u, 1, 1600u));

    uint32_t missing[4] = {0};
    TEST_ASSERT_EQUAL_UINT8(2, rollcall_ledger_missing(&anchored, missing, 4));
    TEST_ASSERT_EQUAL_UINT32(0x0003u, missing[0]);
    TEST_ASSERT_EQUAL_UINT32(0x0005u, missing[1]);
    TEST_ASSERT_EQUAL_UINT8(2, rollcall_ledger_responded_count(&anchored));

    /* A truncating caller still learns the true total. */
    uint32_t one[1] = {0};
    TEST_ASSERT_EQUAL_UINT8(2, rollcall_ledger_missing(&anchored, one, 1));
    TEST_ASSERT_EQUAL_UINT32(0x0003u, one[0]);

    /* Un-anchored: the same responder set, but no authoritative expected
     * set exists, so nothing may be called missing. The ledger reports only
     * what it observed. */
    rollcall_ledger_t open_mesh;
    rollcall_ledger_init(&open_mesh);
    TEST_ASSERT_TRUE(
        rollcall_ledger_start(&open_mesh, 0xABCDu, 0x0001u, 1000u, NULL, 0, expected, 4, false));
    TEST_ASSERT_EQUAL_UINT8(0, open_mesh.expected_count);
    TEST_ASSERT_TRUE(rollcall_ledger_note_response(&open_mesh, 0xABCDu, 0x0002u, 1, 1500u));
    TEST_ASSERT_EQUAL_UINT8(0, rollcall_ledger_missing(&open_mesh, missing, 4));
    TEST_ASSERT_EQUAL_UINT8(1, rollcall_ledger_responded_count(&open_mesh));
}

void test_ledger_drops_self_and_null_from_the_expected_set(void) {
    const uint32_t expected[3] = {0x0002u, 0x0001u /* the initiator */, 0u};
    rollcall_ledger_t l;
    rollcall_ledger_init(&l);
    TEST_ASSERT_TRUE(
        rollcall_ledger_start(&l, 5u, 0x0001u, 1000u, NULL, 0, expected, 3, true));
    TEST_ASSERT_EQUAL_UINT8(1, l.expected_count);
    TEST_ASSERT_EQUAL_UINT32(0x0002u, l.expected[0]);
}

void test_ledger_attaches_the_receipt_relay_path(void) {
    rollcall_ledger_t l;
    rollcall_ledger_init(&l);
    TEST_ASSERT_TRUE(rollcall_ledger_start(&l, 9u, 0x0001u, 1000u, NULL, 0, NULL, 0, false));
    TEST_ASSERT_TRUE(rollcall_ledger_note_response(&l, 9u, 0x0002u, 1, 1500u));

    const uint32_t path[3] = {0x0001u, 0x00AAu, 0x0002u};
    TEST_ASSERT_TRUE(rollcall_ledger_note_path(&l, 9u, 0x0002u, 3, path));

    const rollcall_entry_t* e = rollcall_ledger_find(&l, 0x0002u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->have_path);
    TEST_ASSERT_EQUAL_UINT8(3, e->hop_count);
    TEST_ASSERT_EQUAL_UINT32(0x00AAu, e->relay_path[1]);

    /* A path with no response is a row that is NOT counted as responded:
     * the receipt proves the announce arrived, the missing signature proves
     * nothing about an answer. */
    TEST_ASSERT_TRUE(rollcall_ledger_note_path(&l, 9u, 0x0003u, 2, path));
    const rollcall_entry_t* silent = rollcall_ledger_find(&l, 0x0003u);
    TEST_ASSERT_NOT_NULL(silent);
    TEST_ASSERT_TRUE(silent->have_path);
    TEST_ASSERT_FALSE(silent->responded);
    TEST_ASSERT_EQUAL_UINT8(1, rollcall_ledger_responded_count(&l));

    /* An over-deep path is clamped rather than overrunning the row. */
    uint32_t deep[ROLLCALL_PATH_MAX + 4];
    for (int i = 0; i < ROLLCALL_PATH_MAX + 4; i++)
        deep[i] = (uint32_t)(0x8000u + i);
    TEST_ASSERT_TRUE(
        rollcall_ledger_note_path(&l, 9u, 0x0002u, ROLLCALL_PATH_MAX + 4, deep));
    e = rollcall_ledger_find(&l, 0x0002u);
    TEST_ASSERT_EQUAL_UINT8(ROLLCALL_PATH_MAX, e->hop_count);
}

void test_ledger_closes_once_and_counts_late_responses(void) {
    rollcall_ledger_t l;
    rollcall_ledger_init(&l);
    TEST_ASSERT_TRUE(rollcall_ledger_start(&l, 3u, 0x0001u, 1000u, NULL, 0, NULL, 0, false));

    uint32_t close_at = rollcall_ledger_close_at(&l);
    TEST_ASSERT_EQUAL_UINT32(1000u + rollcall_window_ms(), close_at);

    TEST_ASSERT_FALSE(rollcall_ledger_maybe_close(&l, close_at - 1u));
    TEST_ASSERT_TRUE(l.open);
    TEST_ASSERT_TRUE(rollcall_ledger_maybe_close(&l, close_at));
    TEST_ASSERT_FALSE(l.open);
    TEST_ASSERT_EQUAL_UINT32(close_at, l.closed_ms);
    /* Exactly one close, so a caller emits exactly one completion event. */
    TEST_ASSERT_FALSE(rollcall_ledger_maybe_close(&l, close_at + 10000u));

    /* A response after the close is counted, not recorded: "closed" has to
     * be a stable, reportable state. */
    TEST_ASSERT_FALSE(rollcall_ledger_note_response(&l, 3u, 0x0002u, 1, close_at + 1u));
    TEST_ASSERT_EQUAL_UINT32(1u, l.late);
    TEST_ASSERT_EQUAL_UINT8(0, rollcall_ledger_responded_count(&l));
    TEST_ASSERT_FALSE(rollcall_ledger_note_path(&l, 3u, 0x0002u, 0, NULL));
}

void test_ledger_round_schedule_walks_all_rounds_then_stops(void) {
    rollcall_ledger_t l;
    rollcall_ledger_init(&l);
    TEST_ASSERT_TRUE(rollcall_ledger_start(&l, 11u, 0x0001u, 1000u, NULL, 0, NULL, 0, false));

    /* Round 1 is due the instant the roll-call starts. */
    TEST_ASSERT_TRUE(rollcall_ledger_round_due(&l, 1000u));
    uint32_t due2 = rollcall_ledger_note_round(&l, 1, 1000u);
    TEST_ASSERT_EQUAL_UINT32(1000u + ROLLCALL_ROUND_BASE_MS, due2);
    TEST_ASSERT_FALSE(rollcall_ledger_round_due(&l, due2 - 1u));
    TEST_ASSERT_TRUE(rollcall_ledger_round_due(&l, due2));

    uint32_t due3 = rollcall_ledger_note_round(&l, 2, due2);
    TEST_ASSERT_EQUAL_UINT32(due2 + ROLLCALL_ROUND_BASE_MS * 2u, due3);
    TEST_ASSERT_TRUE(rollcall_ledger_round_due(&l, due3));

    /* After the last round nothing further is ever due, however long the
     * ledger stays open. */
    TEST_ASSERT_EQUAL_UINT32(0, rollcall_ledger_note_round(&l, ROLLCALL_MAX_ROUNDS, due3));
    TEST_ASSERT_FALSE(rollcall_ledger_round_due(&l, due3 + 1000000u));

    /* And a closed ledger never re-announces. */
    l.rounds_sent = 1;
    l.next_round_ms = due3;
    l.open = false;
    TEST_ASSERT_FALSE(rollcall_ledger_round_due(&l, due3 + 1u));
}

void test_ledger_counts_overflow_and_unattested(void) {
    rollcall_ledger_t l;
    rollcall_ledger_init(&l);
    TEST_ASSERT_TRUE(rollcall_ledger_start(&l, 13u, 0x0001u, 1000u, NULL, 0, NULL, 0, false));

    for (uint32_t i = 0; i < ROLLCALL_MAX_RESPONDERS; i++) {
        TEST_ASSERT_TRUE(rollcall_ledger_note_response(&l, 13u, 0x1000u + i, 1, 1500u));
    }
    TEST_ASSERT_EQUAL_UINT8(ROLLCALL_MAX_RESPONDERS, rollcall_ledger_responded_count(&l));

    /* Past capacity the response is COUNTED, never silently dropped: a
     * ledger that could not hold the whole mesh has to say so. */
    TEST_ASSERT_FALSE(rollcall_ledger_note_response(&l, 13u, 0x9999u, 1, 1600u));
    TEST_ASSERT_EQUAL_UINT32(1u, l.overflow);

    rollcall_ledger_note_unattested(&l);
    rollcall_ledger_note_unattested(&l);
    TEST_ASSERT_EQUAL_UINT32(2u, l.unattested);
    /* An unattested response never becomes a row: it is not evidence that
     * anybody answered. */
    TEST_ASSERT_EQUAL_UINT8(ROLLCALL_MAX_RESPONDERS, rollcall_ledger_responded_count(&l));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_announce_round_trips);
    RUN_TEST(test_announce_empty_text_is_valid);
    RUN_TEST(test_announce_rejects_malformed);
    RUN_TEST(test_announce_encode_rejects_bad_inputs);
    RUN_TEST(test_response_round_trips_and_is_fixed_length);
    RUN_TEST(test_signed_msg_layout_is_domain_separated_and_bound);
    RUN_TEST(test_signature_binds_rollcall_initiator_and_responder);
    RUN_TEST(test_another_members_key_cannot_forge_a_response);
    RUN_TEST(test_response_delay_is_bounded_by_the_slot_window);
    RUN_TEST(test_response_slot_window_scales_with_peer_count);
    RUN_TEST(test_response_slot_separates_nodes_and_rerolls_per_rollcall);
    RUN_TEST(test_round_schedule_backs_off_and_terminates);
    RUN_TEST(test_rate_limit_bounds_initiation);
    RUN_TEST(test_rate_limit_survives_the_millisecond_rollover);
    RUN_TEST(test_member_answers_each_rollcall_once);
    RUN_TEST(test_member_seen_table_recycles_the_oldest);
    RUN_TEST(test_ledger_start_records_the_operator_payload);
    RUN_TEST(test_ledger_counts_responses_once_and_keeps_the_first_time);
    RUN_TEST(test_ledger_reports_missing_only_on_an_anchored_mesh);
    RUN_TEST(test_ledger_drops_self_and_null_from_the_expected_set);
    RUN_TEST(test_ledger_attaches_the_receipt_relay_path);
    RUN_TEST(test_ledger_closes_once_and_counts_late_responses);
    RUN_TEST(test_ledger_round_schedule_walks_all_rounds_then_stops);
    RUN_TEST(test_ledger_counts_overflow_and_unattested);
    return UNITY_END();
}
