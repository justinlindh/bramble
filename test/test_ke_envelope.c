#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "channel_key.h"
#include "channel_msg.h"
#include "dm_session.h"
#include "packet.h"

#include "../components/crypto/crypto_host.c"
#include "../components/channel/channel_key.c"
#include "../components/channel/channel_msg.c"
#include "../components/dm_session/dm_session.c"
#include "../components/packet/packet.c"

/*
 * Task 1.4: the pure envelope encode/decode + session-establishment path,
 * exercised end to end without any FreeRTOS glue. An INIT built by A is
 * wrapped as an APP_TYPE_KE inner payload of a channel-key-encrypted DATA
 * envelope (the handshake TRANSPORT per B4/SEC-C2: it rides the channel key
 * because no session exists yet), decoded by B, verified, answered with a
 * RESP, verified back by A, and the resulting session keys are proven
 * identical by round-tripping a chat payload through dm_session_encrypt/
 * dm_session_decrypt (the DM PAYLOAD, which never uses the channel key).
 */

void setUp(void) { channel_msg_catchup_reset(); }
void tearDown(void) {}

static bramble_header_t make_data_header(uint32_t dest_addr, uint8_t flags) {
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_DATA;
    h.flags = flags;
    h.hop_limit = 8;
    h.dest_addr = dest_addr;
    h.packet_id = 0xABCD1234;
    return h;
}

void test_ke_envelope_round_trip_to_session(void) {
    bramble_channel_t ch_a, ch_b;
    TEST_ASSERT_EQUAL(0, channel_derive_key("shared-psk", &ch_a));
    TEST_ASSERT_EQUAL(0, channel_derive_key("shared-psk", &ch_b));

    bramble_identity_t a, b, a_eph, b_eph;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);

    /* A builds INIT (first contact: no cached peer id for b.address). */
    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(
        0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0, NULL, &init));

    uint8_t init_wire[KEY_EXCHANGE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_key_exchange_serialize(&init, init_wire, sizeof(init_wire)));

    /* Wrap as an APP_TYPE_KE DATA envelope under the shared channel key. */
    uint8_t nonce1[12];
    memset(nonce1, 0x11, sizeof(nonce1));
    uint8_t aad1[HEADER_SIZE + 4];
    bramble_header_t hdr1 = make_data_header(b.address, FLAG_ENCRYPT | FLAG_CHANNEL);
    TEST_ASSERT_EQUAL(ESP_OK, bramble_build_aead_aad(&hdr1, a.address, aad1, sizeof(aad1)));

    uint8_t ct1[CHANNEL_MSG_OVERHEAD + KEY_EXCHANGE_SIZE];
    uint8_t tag1[16];
    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch_a, a.address, APP_TYPE_KE, 0, init_wire,
                                             sizeof(init_wire), aad1, sizeof(aad1), nonce1, ct1,
                                             tag1));

    /* B decodes the envelope. */
    bramble_channel_t channels_b[1];
    channels_b[0] = ch_b;
    channel_msg_info_t info1;
    uint8_t pt1[CHANNEL_MSG_MAX_PLAINTEXT_SIZE] = {0};
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels_b, 1, nonce1, ct1, sizeof(ct1), tag1, aad1,
                                             sizeof(aad1), pt1, &info1, 0));
    TEST_ASSERT_EQUAL(APP_TYPE_KE, info1.app_type);
    TEST_ASSERT_EQUAL(KEY_EXCHANGE_SIZE, info1.data_len);

    bramble_key_exchange_t recv_init;
    TEST_ASSERT_EQUAL(ESP_OK,
                      bramble_key_exchange_deserialize(&recv_init, info1.data, info1.data_len));

    /* B verifies INIT (first contact: have_peer_id = 0). */
    TEST_ASSERT_EQUAL(0, dm_verify_init(&recv_init, &b, 0, NULL, NULL));

    /* B builds RESP. */
    bramble_key_exchange_t resp;
    uint8_t kb[32];
    TEST_ASSERT_EQUAL(
        0, dm_build_resp(&b, b_eph.public_key, b_eph.private_key, &recv_init, 0, &resp, kb));

    /* A verifies RESP; both sides now hold the same session key. */
    uint8_t ka[32];
    TEST_ASSERT_EQUAL(0,
                      dm_verify_resp(&resp, &a, a_eph.private_key, a_eph.public_key, 0, NULL, ka));
    TEST_ASSERT_EQUAL_MEMORY(ka, kb, 32);

    /* Establish both sides' session_t ratchet state from the handshake IKM and
     * round-trip a chat payload through the SAME ratchet wrappers mesh_task.c
     * uses (never the channel key: FLAG_CHANNEL absent). This proves the
     * envelope-to-ratchet path end to end: the quad-DH IKM both sides compute
     * from the handshake seeds RK_0 and the directional chains. */
    uint8_t ikm[128];
    TEST_ASSERT_EQUAL(
        0, dm_compute_ikm(a.private_key, a_eph.private_key, b.public_key, b_eph.public_key, ikm));

    dm_session_t sess_a = {0};
    sess_a.peer_addr = b.address;
    sess_a.state = DM_STATE_ACTIVE;
    dm_session_ratchet_init_state(&sess_a, ikm, a.address, b.address);

    dm_session_t sess_b = {0};
    sess_b.peer_addr = a.address;
    sess_b.state = DM_STATE_ACTIVE;
    dm_session_ratchet_init_state(&sess_b, ikm, b.address, a.address);

    /* RK_0 is bit-identical to the handshake session key (migration continuity). */
    TEST_ASSERT_EQUAL_MEMORY(ka, sess_a.ratchet.rk, 32);
    TEST_ASSERT_EQUAL_MEMORY(kb, sess_b.ratchet.rk, 32);

    bramble_header_t hdr2 = make_data_header(b.address, FLAG_ENCRYPT);
    const uint8_t chat_pt[] = "hello over the session";
    uint8_t nonce2[12];
    memset(nonce2, 0x22, sizeof(nonce2));
    uint8_t ct2[DM_RATCHET_HEADER_SIZE + sizeof(chat_pt)];
    uint8_t tag2[16];
    size_t flen2 = 0;
    TEST_ASSERT_EQUAL(0, dm_session_ratchet_encrypt(&sess_a, &hdr2, a.address, chat_pt,
                                                    sizeof(chat_pt), nonce2, ct2, tag2, &flen2));

    uint8_t pt2[sizeof(chat_pt)] = {0};
    size_t plen2 = 0;
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sess_b, &hdr2, a.address, nonce2,
                                                                ct2, flen2, tag2, pt2, &plen2));
    TEST_ASSERT_EQUAL(sizeof(chat_pt), plen2);
    TEST_ASSERT_EQUAL_MEMORY(chat_pt, pt2, sizeof(chat_pt));
}

/* A session-decrypt must fail if the wire src_addr is tampered: src_addr is
 * bound into the AAD (SEC-M2 residual, same as the channel path), so a
 * mismatched claim breaks the GCM tag rather than silently decrypting under
 * the wrong sender's identity. */
void test_session_decrypt_rejects_spoofed_src_addr(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);

    dm_session_t sess = {0};
    crypto_random(sess.session_key, 32);
    sess.peer_addr = a.address;
    sess.state = DM_STATE_ACTIVE;

    bramble_header_t hdr = make_data_header(b.address, FLAG_ENCRYPT);
    const uint8_t pt[] = "spoof me not";
    uint8_t nonce[12];
    memset(nonce, 0x33, sizeof(nonce));
    uint8_t ct[sizeof(pt)];
    uint8_t tag[16];
    TEST_ASSERT_EQUAL(0,
                      dm_session_encrypt(&sess, &hdr, a.address, pt, sizeof(pt), nonce, ct, tag));

    uint8_t out[sizeof(pt)] = {0};
    TEST_ASSERT_NOT_EQUAL(
        0, dm_session_decrypt(&sess, &hdr, a.address + 1, nonce, ct, sizeof(ct), tag, out));
}

/* The serializer writes 98 fixed-field bytes but the wire frame is
 * KEY_EXCHANGE_SIZE (101), and send_ke_envelope transmits all of it. The
 * reserved tail must be zero-filled, not left as uninitialized stack, or
 * every handshake leaks stack bytes to any channel-key holder. Pre-poison
 * the buffer so a regression (dropping the zero-fill) leaves the poison
 * behind and fails here. */
void test_ke_serialize_zeroes_reserved_tail(void) {
    bramble_key_exchange_t ke;
    memset(&ke, 0x5A, sizeof(ke));
    ke.header = make_data_header(0x1234, 0);
    ke.header.type = PKT_TYPE_KEY_EXCHANGE;

    uint8_t wire[KEY_EXCHANGE_SIZE];
    memset(wire, 0xEE, sizeof(wire));
    TEST_ASSERT_EQUAL(ESP_OK, bramble_key_exchange_serialize(&ke, wire, sizeof(wire)));

    /* Fixed fields end at offset 98 (HEADER_SIZE + 4 + 32 + 32 + 1 + 1 + 16);
     * bytes [98, KEY_EXCHANGE_SIZE) are the reserved tail. */
    for (size_t i = HEADER_SIZE + 86; i < KEY_EXCHANGE_SIZE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, wire[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ke_envelope_round_trip_to_session);
    RUN_TEST(test_session_decrypt_rejects_spoofed_src_addr);
    RUN_TEST(test_ke_serialize_zeroes_reserved_tail);
    return UNITY_END();
}
