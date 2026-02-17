#include "unity.h"
#include "crypto.h"
#include "channel_key.h"
#include "channel_msg.h"
#include "public_channel.h"

/* Include implementations directly */
#include "../components/crypto/crypto_host.c"
#include "../components/channel/channel_key.c"
#include "../components/channel/channel_msg.c"
#include "../components/channel/public_channel.c"

/* Reset helpers declared in public_channel.c */
extern void public_channel_reset_tx(void);
extern void public_channel_reset_rx(void);

void setUp(void) {
    public_channel_reset_tx();
    public_channel_reset_rx();
}

void tearDown(void) {}

/* Test 1: Channel 0 init with well-known PSK */
void test_public_channel_init(void) {
    bramble_channel_t channels[MAX_CHANNELS];
    int num = 0;
    TEST_ASSERT_EQUAL(0, public_channel_init(channels, &num));
    TEST_ASSERT_TRUE(num >= 1);

    /* Verify deterministic: derive separately and compare */
    bramble_channel_t expected;
    TEST_ASSERT_EQUAL(0, channel_derive_key(BRAMBLE_PUBLIC_CHANNEL_PSK, &expected));
    TEST_ASSERT_EQUAL_MEMORY(expected.key, channels[0].key, BRAMBLE_KEY_SIZE);
    TEST_ASSERT_EQUAL(expected.channel_id, channels[0].channel_id);
    TEST_ASSERT_EQUAL(0, channels[0].epoch);
}

/* Test 2: TX rate limiting — burst then throttle */
void test_public_channel_rate_limit(void) {
    uint32_t t = 1000;
    /* Should allow BURST (3) sends */
    TEST_ASSERT_TRUE(public_channel_can_send(t));
    TEST_ASSERT_TRUE(public_channel_can_send(t + 1));
    TEST_ASSERT_TRUE(public_channel_can_send(t + 2));
    /* 4th should be denied */
    TEST_ASSERT_FALSE(public_channel_can_send(t + 3));
    /* After one refill interval, one more allowed */
    t += BRAMBLE_PUBLIC_CHANNEL_RATE_LIMIT_MS;
    TEST_ASSERT_TRUE(public_channel_can_send(t));
    TEST_ASSERT_FALSE(public_channel_can_send(t + 1));
}

/* Test 3: RX per-source rate limiting */
void test_public_channel_rx_rate_limit(void) {
    uint32_t t = 5000;
    /* First message from source allowed */
    TEST_ASSERT_TRUE(public_channel_rx_check(0xAAAA, t));
    /* Second from same source too soon — denied */
    TEST_ASSERT_FALSE(public_channel_rx_check(0xAAAA, t + 5000));
    /* Different source — allowed */
    TEST_ASSERT_TRUE(public_channel_rx_check(0xBBBB, t + 5000));
    /* Same source after interval — allowed */
    TEST_ASSERT_TRUE(public_channel_rx_check(0xAAAA, t + 10000));
}

/* Test 4: Encrypt/decrypt on public channel */
void test_public_channel_encrypt_decrypt(void) {
    bramble_channel_t channels[MAX_CHANNELS];
    int num = 0;
    public_channel_init(channels, &num);

    uint8_t data[] = "Hello public channel!";
    uint8_t nonce[12], ct[256], tag[16];
    uint32_t src = 0x12345678;

    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&channels[0], src, 0x01,
                                              data, sizeof(data),
                                              nonce, ct, tag));

    channel_msg_info_t info;
    uint8_t pt[256];
    /* Need a fresh channel set for decrypt (same key) */
    bramble_channel_t dec_channels[MAX_CHANNELS];
    int dec_num = 0;
    public_channel_init(dec_channels, &dec_num);

    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(dec_channels, dec_num,
                                              nonce, ct,
                                              CHANNEL_MSG_OVERHEAD + sizeof(data),
                                              tag, &info));
    TEST_ASSERT_EQUAL(src, info.src_addr);
    TEST_ASSERT_EQUAL(0x01, info.app_type);
    TEST_ASSERT_EQUAL(sizeof(data), info.data_len);
    TEST_ASSERT_EQUAL_MEMORY(data, info.data, sizeof(data));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_public_channel_init);
    RUN_TEST(test_public_channel_rate_limit);
    RUN_TEST(test_public_channel_rx_rate_limit);
    RUN_TEST(test_public_channel_encrypt_decrypt);
    return UNITY_END();
}
