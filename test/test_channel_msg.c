#include "unity.h"
#include "crypto.h"
#include "channel_key.h"
#include "channel_msg.h"

/* Include implementations directly */
#include "../components/crypto/crypto_host.c"
#include "../components/channel/channel_key.c"
#include "../components/channel/channel_msg.c"

void setUp(void) {}
void tearDown(void) {}

static void make_channel(const char *psk, bramble_channel_t *ch) {
    TEST_ASSERT_EQUAL(0, channel_derive_key(psk, ch));
}

/* Test 1: Encrypt + decrypt single channel */
void test_encrypt_decrypt_single(void) {
    bramble_channel_t ch;
    make_channel("test-psk-1", &ch);

    uint8_t data[] = "Hello Bramble!";
    uint8_t nonce[12], ct[256], tag[16];

    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch, 0xDEADBEEF, 0x01,
                                              data, sizeof(data),
                                              nonce, ct, tag));

    bramble_channel_t channels[1];
    make_channel("test-psk-1", &channels[0]);

    channel_msg_info_t info;
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 1, nonce, ct, ct_len, tag, &info));

    TEST_ASSERT_EQUAL(ch.channel_id, info.channel_id);
    TEST_ASSERT_EQUAL(0, info.epoch);
    TEST_ASSERT_EQUAL(0x01, info.app_type);
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
    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch_a, 0x12345678, 0x02,
                                              data, sizeof(data),
                                              nonce, ct, tag));

    bramble_channel_t channels[4];
    make_channel("channel-B", &channels[0]);
    make_channel("channel-C", &channels[1]);
    make_channel("channel-A", &channels[2]);
    make_channel("channel-D", &channels[3]);

    channel_msg_info_t info;
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 4, nonce, ct, ct_len, tag, &info));
    TEST_ASSERT_EQUAL(2, info.channel_index);
    TEST_ASSERT_EQUAL(0x12345678, info.src_addr);
}

/* Test 3: Unknown channel → -1 */
void test_unknown_channel(void) {
    bramble_channel_t ch;
    make_channel("unknown", &ch);

    uint8_t data[] = "test";
    uint8_t nonce[12], ct[256], tag[16];
    channel_msg_encrypt(&ch, 1, 0, data, sizeof(data), nonce, ct, tag);

    bramble_channel_t channels[2];
    make_channel("other-1", &channels[0]);
    make_channel("other-2", &channels[1]);

    channel_msg_info_t info;
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(-1, channel_msg_decrypt(channels, 2, nonce, ct, ct_len, tag, &info));
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
    channel_msg_encrypt(&ch12, 0xAABBCCDD, 0x05, data, sizeof(data), nonce, ct, tag);

    channel_msg_info_t info;
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 16, nonce, ct, ct_len, tag, &info));
    TEST_ASSERT_EQUAL(12, info.channel_index);
    TEST_ASSERT_EQUAL(0xAABBCCDD, info.src_addr);
    TEST_ASSERT_EQUAL(0x05, info.app_type);
}

/* Test 5: Epoch catch-up */
void test_epoch_catchup(void) {
    /* Sender at epoch 5 */
    bramble_channel_t sender;
    make_channel("epoch-test", &sender);
    for (int i = 0; i < 5; i++) channel_advance_epoch(&sender);
    TEST_ASSERT_EQUAL(5, sender.epoch);

    uint8_t data[] = "epoch5 msg";
    uint8_t nonce[12], ct[256], tag[16];
    channel_msg_encrypt(&sender, 0x11111111, 0x03, data, sizeof(data), nonce, ct, tag);

    /* Receiver at epoch 3 */
    bramble_channel_t receiver;
    make_channel("epoch-test", &receiver);
    for (int i = 0; i < 3; i++) channel_advance_epoch(&receiver);
    TEST_ASSERT_EQUAL(3, receiver.epoch);

    channel_msg_info_t info;
    size_t ct_len = CHANNEL_MSG_OVERHEAD + sizeof(data);
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(&receiver, 1, nonce, ct, ct_len, tag, &info));
    TEST_ASSERT_EQUAL(5, info.epoch);
    TEST_ASSERT_EQUAL(0x11111111, info.src_addr);
    /* Receiver should have advanced to epoch 5 */
    TEST_ASSERT_EQUAL(5, receiver.epoch);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_encrypt_decrypt_single);
    RUN_TEST(test_trial_decryption);
    RUN_TEST(test_unknown_channel);
    RUN_TEST(test_16_channels);
    RUN_TEST(test_epoch_catchup);
    return UNITY_END();
}
