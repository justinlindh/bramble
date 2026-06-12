/*
 * DATA packet AAD vs relay mutation.
 *
 * Encrypted DATA packets bind the serialized 12-byte header as AES-GCM AAD.
 * Relays decrement hop_limit (header byte 3) in forward_data_packet before
 * retransmitting, so the destination must compute an AAD that is invariant
 * under that mutation while still binding every other header field
 * (version, type, flags, dest_addr, packet_id).
 *
 * These tests run the full originator -> relay -> destination pipeline using
 * the same serialization and channel crypto code the firmware uses:
 *   - originator encrypts as send_data_packet does (mesh_task.c)
 *   - relay mutates the header exactly as forward_data_packet does
 *   - destination decrypts as handle_data does
 */
#include "unity.h"
#include "esp_stubs.h"
#include "packet.h"
#include "crypto.h"
#include "channel_key.h"
#include "channel_msg.h"

/* Include implementations directly (host-test convention) */
#include "../components/packet/packet.c"
#include "../components/crypto/crypto_host.c"
#include "../components/channel/channel_key.c"
#include "../components/channel/channel_msg.c"

void setUp(void) {}
void tearDown(void) {}

#define TEST_SRC_ADDR 0xA1B2C3D4u
#define TEST_DEST_ADDR 0x11223344u
#define TEST_PACKET_ID 0xCAFEF00Du
#define TEST_APP_TYPE 0x01

static void make_channel(const char* psk, bramble_channel_t* ch) {
    TEST_ASSERT_EQUAL(0, channel_derive_key(psk, ch));
}

/*
 * Originate an encrypted DATA packet exactly as send_data_packet does.
 * Layout: header(12) + src_addr(4) + nonce(12) + ciphertext(N) + tag(16).
 * Returns total packet length.
 */
static size_t originate_data_packet(const bramble_channel_t* ch, const uint8_t* payload,
                                    size_t payload_len, uint8_t hop_limit, uint8_t* buf) {
    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_DATA,
        .flags = FLAG_ENCRYPT | FLAG_CHANNEL,
        .hop_limit = hop_limit,
        .dest_addr = TEST_DEST_ADDR,
        .packet_id = TEST_PACKET_ID,
    };
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_serialize(&header, buf, HEADER_SIZE));

    uint8_t aad[HEADER_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_build_aad(&header, aad, sizeof(aad)));

    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t ciphertext[CHANNEL_MSG_MAX_PLAINTEXT_SIZE];
    uint8_t tag[BRAMBLE_TAG_SIZE];
    size_t ct_len = CHANNEL_MSG_OVERHEAD + payload_len;

    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(ch, TEST_SRC_ADDR, TEST_APP_TYPE, payload, payload_len,
                                             aad, HEADER_SIZE, nonce, ciphertext, tag));

    uint32_t src = TEST_SRC_ADDR;
    memcpy(buf + HEADER_SIZE, &src, 4);
    memcpy(buf + HEADER_SIZE + 4, nonce, BRAMBLE_NONCE_SIZE);
    memcpy(buf + HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE, ciphertext, ct_len);
    memcpy(buf + HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE + ct_len, tag, BRAMBLE_TAG_SIZE);
    return HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE + ct_len + BRAMBLE_TAG_SIZE;
}

/* Relay the packet exactly as forward_data_packet does: deserialize the
 * header, decrement hop_limit, re-serialize in place. Payload untouched. */
static void relay_data_packet(uint8_t* buf, size_t len) {
    bramble_header_t hdr;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&hdr, buf, len));
    hdr.hop_limit--;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_serialize(&hdr, buf, HEADER_SIZE));
}

/* Decrypt at the destination exactly as handle_data does. Returns the
 * channel_msg_decrypt result (0 on success). */
static int destination_decrypt(bramble_channel_t* channels, int num_channels, const uint8_t* buf,
                               size_t len, channel_msg_info_t* info, uint8_t* plaintext) {
    const uint8_t* nonce = buf + HEADER_SIZE + 4;
    size_t ct_len = len - HEADER_SIZE - 4 - BRAMBLE_NONCE_SIZE - BRAMBLE_TAG_SIZE;
    const uint8_t* ciphertext = nonce + BRAMBLE_NONCE_SIZE;
    const uint8_t* tag = ciphertext + ct_len;

    bramble_header_t rx_hdr;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&rx_hdr, buf, len));
    uint8_t aad[HEADER_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_build_aad(&rx_hdr, aad, sizeof(aad)));

    return channel_msg_decrypt(channels, num_channels, nonce, ciphertext, ct_len, tag, aad,
                               HEADER_SIZE, plaintext, info);
}

/* Single hop, header untouched: decrypts (baseline sanity). */
void test_direct_delivery_decrypts(void) {
    bramble_channel_t tx_ch;
    make_channel("relay-aad-psk", &tx_ch);
    bramble_channel_t rx_channels[1];
    make_channel("relay-aad-psk", &rx_channels[0]);

    uint8_t payload[] = "direct hello";
    uint8_t buf[BRAMBLE_MAX_PACKET_SIZE];
    size_t len = originate_data_packet(&tx_ch, payload, sizeof(payload), 3, buf);

    channel_msg_info_t info;
    uint8_t pt[CHANNEL_MSG_MAX_PLAINTEXT_SIZE] = {0};
    TEST_ASSERT_EQUAL(0, destination_decrypt(rx_channels, 1, buf, len, &info, pt));
    TEST_ASSERT_EQUAL(TEST_SRC_ADDR, info.src_addr);
    TEST_ASSERT_EQUAL_MEMORY(payload, pt, sizeof(payload));
}

/* THE BUG: one relay hop decrements hop_limit; the destination must still
 * authenticate and decrypt the packet. */
void test_one_relay_hop_decrypts(void) {
    bramble_channel_t tx_ch;
    make_channel("relay-aad-psk", &tx_ch);
    bramble_channel_t rx_channels[1];
    make_channel("relay-aad-psk", &rx_channels[0]);

    uint8_t payload[] = "multi-hop hello";
    uint8_t buf[BRAMBLE_MAX_PACKET_SIZE];
    size_t len = originate_data_packet(&tx_ch, payload, sizeof(payload), 3, buf);

    relay_data_packet(buf, len);

    channel_msg_info_t info;
    uint8_t pt[CHANNEL_MSG_MAX_PLAINTEXT_SIZE] = {0};
    TEST_ASSERT_EQUAL_MESSAGE(0, destination_decrypt(rx_channels, 1, buf, len, &info, pt),
                              "forwarded DATA packet failed GCM tag check after relay "
                              "decremented hop_limit");
    TEST_ASSERT_EQUAL(TEST_SRC_ADDR, info.src_addr);
    TEST_ASSERT_EQUAL_MEMORY(payload, pt, sizeof(payload));
}

/* Two relay hops (hop_limit 3 -> 1): still decrypts. */
void test_two_relay_hops_decrypt(void) {
    bramble_channel_t tx_ch;
    make_channel("relay-aad-psk", &tx_ch);
    bramble_channel_t rx_channels[1];
    make_channel("relay-aad-psk", &rx_channels[0]);

    uint8_t payload[] = "two hops";
    uint8_t buf[BRAMBLE_MAX_PACKET_SIZE];
    size_t len = originate_data_packet(&tx_ch, payload, sizeof(payload), 3, buf);

    relay_data_packet(buf, len);
    relay_data_packet(buf, len);

    channel_msg_info_t info;
    uint8_t pt[CHANNEL_MSG_MAX_PLAINTEXT_SIZE] = {0};
    TEST_ASSERT_EQUAL(0, destination_decrypt(rx_channels, 1, buf, len, &info, pt));
    TEST_ASSERT_EQUAL_MEMORY(payload, pt, sizeof(payload));
}

/* Tamper guards: hop_limit is the ONLY header byte a relay may change.
 * Mutating anything else must still fail authentication. */

static void tamper_and_expect_failure(size_t byte_offset, uint8_t xor_mask) {
    bramble_channel_t tx_ch;
    make_channel("relay-aad-psk", &tx_ch);
    bramble_channel_t rx_channels[1];
    make_channel("relay-aad-psk", &rx_channels[0]);

    uint8_t payload[] = "tamper target";
    uint8_t buf[BRAMBLE_MAX_PACKET_SIZE];
    size_t len = originate_data_packet(&tx_ch, payload, sizeof(payload), 3, buf);

    buf[byte_offset] ^= xor_mask;

    channel_msg_info_t info;
    uint8_t pt[CHANNEL_MSG_MAX_PLAINTEXT_SIZE] = {0};
    TEST_ASSERT_NOT_EQUAL(0, destination_decrypt(rx_channels, 1, buf, len, &info, pt));
}

void test_tampered_type_fails(void) {
    tamper_and_expect_failure(1, 0x01); /* header byte 1: type */
}

void test_tampered_flags_fails(void) {
    tamper_and_expect_failure(2, FLAG_ACK_REQ); /* header byte 2: flags */
}

void test_tampered_dest_addr_fails(void) {
    tamper_and_expect_failure(4, 0x01); /* header bytes 4-7: dest_addr */
}

void test_tampered_packet_id_fails(void) {
    tamper_and_expect_failure(11, 0x80); /* header bytes 8-11: packet_id */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_direct_delivery_decrypts);
    RUN_TEST(test_one_relay_hop_decrypts);
    RUN_TEST(test_two_relay_hops_decrypt);
    RUN_TEST(test_tampered_type_fails);
    RUN_TEST(test_tampered_flags_fails);
    RUN_TEST(test_tampered_dest_addr_fails);
    RUN_TEST(test_tampered_packet_id_fails);
    return UNITY_END();
}
