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

void setUp(void) { channel_msg_catchup_reset(); }

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
    TEST_ASSERT_EQUAL(BRAMBLE_PUBLIC_CHANNEL_INDEX, channels[0].channel_id);
    TEST_ASSERT_EQUAL(0, channels[0].epoch);
}

/* Test 4: Encrypt/decrypt on public channel */
void test_public_channel_encrypt_decrypt(void) {
    bramble_channel_t channels[MAX_CHANNELS];
    int num = 0;
    public_channel_init(channels, &num);

    uint8_t data[] = "Hello public channel!";
    uint8_t nonce[12], ct[256], tag[16];
    memset(nonce, 0x5A, sizeof(nonce)); /* fixed test pattern; caller-supplied since Task 0.4 */
    uint8_t aad[12] = {0};
    uint8_t pt[256] = {0};
    uint32_t src = 0x12345678;

    /* app_type 0x09: deliberately not APP_TYPE_CHAT, this test is unrelated
     * to the chat-only sent_at framing (Task 0.6). */
    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&channels[0], src, 0x09, 0,
                                              data, sizeof(data),
                                              aad, sizeof(aad),
                                              nonce, ct, tag));

    channel_msg_info_t info;
    /* Need a fresh channel set for decrypt (same key) */
    bramble_channel_t dec_channels[MAX_CHANNELS];
    int dec_num = 0;
    public_channel_init(dec_channels, &dec_num);

    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(dec_channels, dec_num,
                                              nonce, ct,
                                              CHANNEL_MSG_OVERHEAD + sizeof(data),
                                              tag, aad, sizeof(aad), pt, &info, 0));
    TEST_ASSERT_EQUAL(src, info.src_addr);
    TEST_ASSERT_EQUAL(0x09, info.app_type);
    TEST_ASSERT_EQUAL(sizeof(data), info.data_len);
    TEST_ASSERT_EQUAL_MEMORY(data, info.data, sizeof(data));
}

/* Fix 2 (red-team panel): channel_source_is_replay_trustworthy decides
 * whether a decrypt's src_addr is safe to feed into the shared per-sender
 * replay window. The public channel's key is known to everyone, so its
 * src_addr is a free-to-forge claim; a secret channel's src_addr costs at
 * least channel membership; a DM/session decrypt's src_addr costs a
 * negotiated session key. */
void test_replay_trustworthy_false_for_public_channel(void) {
    TEST_ASSERT_EQUAL(0, channel_source_is_replay_trustworthy(1, BRAMBLE_PUBLIC_CHANNEL_INDEX));
}
void test_replay_trustworthy_true_for_secret_channel(void) {
    TEST_ASSERT_EQUAL(1,
                       channel_source_is_replay_trustworthy(1, BRAMBLE_PUBLIC_CHANNEL_INDEX + 1));
}
/* DM/session decrypts leave channel_index at its zero default (the same
 * value as BRAMBLE_PUBLIC_CHANNEL_INDEX), so is_channel_message=0 must
 * still be trustworthy: relying on channel_index alone, without also
 * checking is_channel_message, would misclassify every DM as public. */
void test_replay_trustworthy_true_for_dm_session_despite_zero_channel_index(void) {
    TEST_ASSERT_EQUAL(1, channel_source_is_replay_trustworthy(0, BRAMBLE_PUBLIC_CHANNEL_INDEX));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_public_channel_init);
    RUN_TEST(test_public_channel_encrypt_decrypt);
    RUN_TEST(test_replay_trustworthy_false_for_public_channel);
    RUN_TEST(test_replay_trustworthy_true_for_secret_channel);
    RUN_TEST(test_replay_trustworthy_true_for_dm_session_despite_zero_channel_index);
    return UNITY_END();
}
