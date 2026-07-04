#include "unity.h"
#include "crypto.h"
#include "channel_key.h"
#include "channel_msg.h"

/* Include implementations directly */
#include "../components/crypto/crypto_host.c"
#include "../components/channel/channel_key.c"
#include "../components/channel/channel_msg.c"

void setUp(void) { channel_msg_catchup_reset(); }
void tearDown(void) {}

static void make_channel(const char* psk, bramble_channel_t* ch) {
    TEST_ASSERT_EQUAL(0, channel_derive_key(psk, ch));
}

/* Test 1: Encrypt + decrypt single channel */
void test_encrypt_decrypt_single(void) {
    bramble_channel_t ch;
    make_channel("test-psk-1", &ch);

    uint8_t data[] = "Hello Bramble!";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x01, sizeof(nonce));
    uint8_t aad[12] = {0};

    /* app_type 0x09: deliberately not APP_TYPE_CHAT (see test_sent_at_*
     * below for that), so ct_len stays at plain CHANNEL_MSG_OVERHEAD. */
    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch, 0xDEADBEEF, 0x09, 0, data, sizeof(data), aad,
                                             sizeof(aad), nonce, ct, tag));

    bramble_channel_t channels[1];
    make_channel("test-psk-1", &channels[0]);

    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 1, nonce, ct, ct_len, tag, aad, sizeof(aad),
                                             pt, &info, 0));

    TEST_ASSERT_EQUAL(ch.channel_id, info.channel_id);
    TEST_ASSERT_EQUAL(0, info.epoch);
    TEST_ASSERT_EQUAL(0x09, info.app_type);
    TEST_ASSERT_EQUAL(0xDEADBEEF, info.src_addr);
    TEST_ASSERT_EQUAL(sizeof(data), info.data_len);
    TEST_ASSERT_EQUAL(0, info.channel_index);
}

/* Test 2: Trial decryption - find channel A at index 2 in [B, C, A, D] */
void test_trial_decryption(void) {
    bramble_channel_t ch_a;
    make_channel("channel-A", &ch_a);

    uint8_t data[] = "secret";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x02, sizeof(nonce));
    uint8_t aad[12] = {0};
    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch_a, 0x12345678, 0x02, 0, data, sizeof(data), aad,
                                             sizeof(aad), nonce, ct, tag));

    bramble_channel_t channels[4];
    make_channel("channel-B", &channels[0]);
    make_channel("channel-C", &channels[1]);
    make_channel("channel-A", &channels[2]);
    make_channel("channel-D", &channels[3]);

    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 4, nonce, ct, ct_len, tag, aad, sizeof(aad),
                                             pt, &info, 0));
    TEST_ASSERT_EQUAL(2, info.channel_index);
    TEST_ASSERT_EQUAL(0x12345678, info.src_addr);
}

/* Test 3: Unknown channel → -1 */
void test_unknown_channel(void) {
    bramble_channel_t ch;
    make_channel("unknown", &ch);

    uint8_t data[] = "test";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x03, sizeof(nonce));
    uint8_t aad[12] = {0};
    channel_msg_encrypt(&ch, 1, 0, 0, data, sizeof(data), aad, sizeof(aad), nonce, ct, tag);

    bramble_channel_t channels[2];
    make_channel("other-1", &channels[0]);
    make_channel("other-2", &channels[1]);

    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(-1, channel_msg_decrypt(channels, 2, nonce, ct, ct_len, tag, aad, sizeof(aad),
                                              pt, &info, 0));
}

/* Test 4: 16 channels, encrypt with channel 12 */
void test_16_channels(void) {
    char psk[32];
    bramble_channel_t channels[16];
    for (int i = 0; i < 16; i++) {
        snprintf(psk, sizeof(psk), "channel-%d", i);
        make_channel(psk, &channels[i]);
    }

    /* Encrypt with channel 12's key */
    bramble_channel_t ch12;
    make_channel("channel-12", &ch12);

    uint8_t data[] = "from channel 12";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x04, sizeof(nonce));
    uint8_t aad[12] = {0};
    channel_msg_encrypt(&ch12, 0xAABBCCDD, 0x05, 0, data, sizeof(data), aad, sizeof(aad), nonce, ct,
                        tag);

    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 16, nonce, ct, ct_len, tag, aad, sizeof(aad),
                                             pt, &info, 0));
    TEST_ASSERT_EQUAL(12, info.channel_index);
    TEST_ASSERT_EQUAL(0xAABBCCDD, info.src_addr);
    TEST_ASSERT_EQUAL(0x05, info.app_type);
}

/* Test 5: Epoch catch-up */
void test_epoch_catchup(void) {
    /* Sender at epoch 5 */
    bramble_channel_t sender;
    make_channel("epoch-test", &sender);
    for (int i = 0; i < 5; i++)
        channel_advance_epoch(&sender);
    TEST_ASSERT_EQUAL(5, sender.epoch);

    uint8_t data[] = "epoch5 msg";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x05, sizeof(nonce));
    uint8_t aad[12] = {0};
    channel_msg_encrypt(&sender, 0x11111111, 0x03, 0, data, sizeof(data), aad, sizeof(aad), nonce,
                        ct, tag);

    /* Receiver at epoch 3 */
    bramble_channel_t receiver;
    make_channel("epoch-test", &receiver);
    for (int i = 0; i < 3; i++)
        channel_advance_epoch(&receiver);
    TEST_ASSERT_EQUAL(3, receiver.epoch);

    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(&receiver, 1, nonce, ct, ct_len, tag, aad, sizeof(aad),
                                             pt, &info, 0));
    TEST_ASSERT_EQUAL(5, info.epoch);
    TEST_ASSERT_EQUAL(0x11111111, info.src_addr);
    /* Receiver should have advanced to epoch 5 */
    TEST_ASSERT_EQUAL(5, receiver.epoch);
}

/* Test 6: Constant-time trial — decryption succeeds at every position */
void test_constant_time_all_positions(void) {
    for (int target = 0; target < 16; target++) {
        char psk[32];
        bramble_channel_t channels[16];
        for (int i = 0; i < 16; i++) {
            snprintf(psk, sizeof(psk), "ct-channel-%d", i);
            make_channel(psk, &channels[i]);
        }

        /* Encrypt with target channel */
        bramble_channel_t enc_ch;
        snprintf(psk, sizeof(psk), "ct-channel-%d", target);
        make_channel(psk, &enc_ch);

        uint8_t data[] = "constant-time test";
        uint8_t nonce[12], ct[256], tag[16];
        memset(nonce, 0x06, sizeof(nonce));
        nonce[11] = (uint8_t)target;
        uint8_t aad[12] = {0};
        channel_msg_encrypt(&enc_ch, 0xBEEF0000 + target, 0x09, 0, data, sizeof(data), aad,
                            sizeof(aad), nonce, ct, tag);

        channel_msg_info_t info;
        uint8_t pt[256] = {0};
        size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
        TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 16, nonce, ct, ct_len, tag, aad,
                                                 sizeof(aad), pt, &info, 0));
        TEST_ASSERT_EQUAL(target, info.channel_index);
        TEST_ASSERT_EQUAL((uint32_t)(0xBEEF0000 + target), info.src_addr);
    }
}

/* ── Epoch catch-up rate limit (SEC-I1) ─────────────────────────────── */

/* Helper: produce a ciphertext the receiver cannot decrypt (unknown key),
 * which forces the full catch-up loop and drains the budget. */
static void make_garbage(uint8_t* nonce, uint8_t* ct, uint8_t* tag, size_t* ct_len) {
    bramble_channel_t attacker;
    make_channel("attacker-key-not-known-to-receiver", &attacker);
    uint8_t data[] = "junk";
    uint8_t aad[12] = {0};
    memset(nonce, 0x07, BRAMBLE_NONCE_SIZE);
    channel_msg_encrypt(&attacker, 0x66666666, 0x09, 0, data, sizeof(data), aad, sizeof(aad), nonce,
                        ct, tag);
    *ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
}

/* Deep recovery: a fresh budget covers a 200-epoch drift in one packet */
void test_epoch_catchup_deep_recovery(void) {
    bramble_channel_t sender;
    make_channel("deep-recovery", &sender);
    for (int i = 0; i < 200; i++)
        channel_advance_epoch(&sender);

    uint8_t data[] = "deep";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x08, sizeof(nonce));
    uint8_t aad[12] = {0};
    channel_msg_encrypt(&sender, 0x22222222, 0x09, 0, data, sizeof(data), aad, sizeof(aad), nonce,
                        ct, tag);

    bramble_channel_t receiver;
    make_channel("deep-recovery", &receiver);

    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(&receiver, 1, nonce, ct, ct_len, tag, aad, sizeof(aad),
                                             pt, &info, 0));
    TEST_ASSERT_EQUAL(200, receiver.epoch);
}

/* Exhausted budget blocks catch-up, refill restores it; current-key
 * decryption is never affected */
void test_epoch_catchup_budget_exhaustion_and_refill(void) {
    bramble_channel_t receiver;
    make_channel("budget-test", &receiver);

    /* Drain: one undecryptable packet burns the full 256-attempt budget */
    uint8_t g_nonce[12], g_ct[256], g_tag[16];
    size_t g_len;
    make_garbage(g_nonce, g_ct, g_tag, &g_len);
    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    TEST_ASSERT_EQUAL(-1, channel_msg_decrypt(&receiver, 1, g_nonce, g_ct, g_len, g_tag,
                                              (uint8_t[12]){0}, 12, pt, &info, 1000));

    /* A 3-epoch-ahead legit message now fails: no budget at t=1000 */
    bramble_channel_t sender;
    make_channel("budget-test", &sender);
    for (int i = 0; i < 3; i++)
        channel_advance_epoch(&sender);
    uint8_t data[] = "late";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x09, sizeof(nonce));
    uint8_t aad[12] = {0};
    channel_msg_encrypt(&sender, 0x33333333, 0x09, 0, data, sizeof(data), aad, sizeof(aad), nonce,
                        ct, tag);
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(-1, channel_msg_decrypt(&receiver, 1, nonce, ct, ct_len, tag, aad,
                                              sizeof(aad), pt, &info, 1000));
    TEST_ASSERT_EQUAL(0, receiver.epoch);

    /* Current-epoch traffic still decrypts with an empty budget */
    bramble_channel_t sender_now;
    make_channel("budget-test", &sender_now);
    uint8_t data2[] = "now";
    uint8_t nonce2[12], ct2[256], tag2[16];
    memset(nonce2, 0x0A, sizeof(nonce2));
    channel_msg_encrypt(&sender_now, 0x44444444, 0x09, 0, data2, sizeof(data2), aad, sizeof(aad),
                        nonce2, ct2, tag2);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(&receiver, 1, nonce2, ct2,
                                             CHANNEL_MSG_OVERHEAD + sizeof(data2), tag2, aad,
                                             sizeof(aad), pt, &info, 1000));

    /* After the refill window the same delayed message recovers */
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(&receiver, 1, nonce, ct, ct_len, tag, aad, sizeof(aad),
                                             pt, &info, 1000 + CHANNEL_EPOCH_CATCHUP_REFILL_MS));
    TEST_ASSERT_EQUAL(3, receiver.epoch);
    TEST_ASSERT_EQUAL(0x33333333, info.src_addr);
}

/* Successful catch-up refunds its tokens: legit deep recovery does not
 * self-drain, so back-to-back deep drifts both recover at the same
 * timestamp (impossible without the refund: 200 + 200 > 256) */
void test_epoch_catchup_success_refunds_budget(void) {
    bramble_channel_t receiver;
    make_channel("refund-test", &receiver);

    bramble_channel_t sender;
    make_channel("refund-test", &sender);
    uint8_t aad[12] = {0};
    channel_msg_info_t info;
    uint8_t pt[256] = {0};

    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 200; i++)
            channel_advance_epoch(&sender);
        uint8_t data[] = "deep";
        uint8_t nonce[12], ct[256], tag[16];
        memset(nonce, 0x0B, sizeof(nonce));
        nonce[11] = (uint8_t)round;
        channel_msg_encrypt(&sender, 0x77777777, 0x09, 0, data, sizeof(data), aad, sizeof(aad),
                            nonce, ct, tag);
        TEST_ASSERT_EQUAL(0, channel_msg_decrypt(&receiver, 1, nonce, ct,
                                                 CHANNEL_MSG_OVERHEAD + sizeof(data), tag, aad,
                                                 sizeof(aad), pt, &info, 0));
    }
    TEST_ASSERT_EQUAL(400, receiver.epoch);
}

/* Budgets are per channel: draining channel 0 leaves channel 1 intact */
void test_epoch_catchup_budget_is_per_channel(void) {
    bramble_channel_t channels[2];
    make_channel("per-chan-A", &channels[0]);
    make_channel("per-chan-B", &channels[1]);

    /* Drain both buckets with garbage (each channel runs its own loop) */
    uint8_t g_nonce[12], g_ct[256], g_tag[16];
    size_t g_len;
    make_garbage(g_nonce, g_ct, g_tag, &g_len);
    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    TEST_ASSERT_EQUAL(-1, channel_msg_decrypt(channels, 1, g_nonce, g_ct, g_len, g_tag,
                                              (uint8_t[12]){0}, 12, pt, &info, 0));

    /* Channel 0 drained, channel 1 untouched: a 2-epoch catch-up on
     * channel 1 still succeeds at the same timestamp */
    bramble_channel_t sender;
    make_channel("per-chan-B", &sender);
    channel_advance_epoch(&sender);
    channel_advance_epoch(&sender);
    uint8_t data[] = "chB";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x0C, sizeof(nonce));
    uint8_t aad[12] = {0};
    channel_msg_encrypt(&sender, 0x55555555, 0x09, 0, data, sizeof(data), aad, sizeof(aad), nonce,
                        ct, tag);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 2, nonce, ct,
                                             CHANNEL_MSG_OVERHEAD + sizeof(data), tag, aad,
                                             sizeof(aad), pt, &info, 0));
    TEST_ASSERT_EQUAL(1, info.channel_index);
}

/* ── sent_at (Task 0.6) ─────────────────────────────────────────────── */

/* APP_TYPE_CHAT carries an authenticated sent_at inside the ciphertext:
 * confirm it survives the round trip and the extra 4 bytes are correctly
 * excluded from the decoded data. */
void test_sent_at_round_trips_for_chat(void) {
    bramble_channel_t ch;
    make_channel("sent-at-psk", &ch);

    uint8_t data[] = "hello at a known time";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x0D, sizeof(nonce));
    uint8_t aad[12] = {0};
    uint32_t sent_at = 1700000000u;

    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch, 0x99999999, APP_TYPE_CHAT, sent_at, data,
                                             sizeof(data), aad, sizeof(aad), nonce, ct, tag));

    bramble_channel_t channels[1];
    make_channel("sent-at-psk", &channels[0]);
    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    size_t ct_len = CHANNEL_MSG_OVERHEAD + CHANNEL_MSG_SENT_AT_SIZE + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 1, nonce, ct, ct_len, tag, aad, sizeof(aad),
                                             pt, &info, 0));

    TEST_ASSERT_EQUAL(APP_TYPE_CHAT, info.app_type);
    TEST_ASSERT_EQUAL_UINT32(sent_at, info.sent_at);
    TEST_ASSERT_EQUAL(sizeof(data), info.data_len);
    TEST_ASSERT_EQUAL_MEMORY(data, info.data, sizeof(data));
}

/* A non-chat app_type carries no sent_at: the caller's value is ignored on
 * encrypt, and decrypt reports it absent (0), not whatever was passed in. */
void test_sent_at_absent_for_non_chat(void) {
    bramble_channel_t ch;
    make_channel("sent-at-psk-2", &ch);

    uint8_t data[] = "not chat";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x0E, sizeof(nonce));
    uint8_t aad[12] = {0};

    /* Deliberately pass a nonzero sent_at to confirm it is ignored for a
     * non-chat app_type. */
    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch, 0x88888888, 0x09, 1700000000u, data, sizeof(data),
                                             aad, sizeof(aad), nonce, ct, tag));

    bramble_channel_t channels[1];
    make_channel("sent-at-psk-2", &channels[0]);
    channel_msg_info_t info;
    uint8_t pt[256] = {0};
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 1, nonce, ct, ct_len, tag, aad, sizeof(aad),
                                             pt, &info, 0));

    TEST_ASSERT_EQUAL(0x09, info.app_type);
    TEST_ASSERT_EQUAL_UINT32(0, info.sent_at);
    TEST_ASSERT_EQUAL(sizeof(data), info.data_len);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_encrypt_decrypt_single);
    RUN_TEST(test_trial_decryption);
    RUN_TEST(test_unknown_channel);
    RUN_TEST(test_16_channels);
    RUN_TEST(test_epoch_catchup);
    RUN_TEST(test_constant_time_all_positions);
    RUN_TEST(test_epoch_catchup_deep_recovery);
    RUN_TEST(test_epoch_catchup_budget_exhaustion_and_refill);
    RUN_TEST(test_epoch_catchup_success_refunds_budget);
    RUN_TEST(test_epoch_catchup_budget_is_per_channel);
    RUN_TEST(test_sent_at_round_trips_for_chat);
    RUN_TEST(test_sent_at_absent_for_non_chat);
    return UNITY_END();
}
