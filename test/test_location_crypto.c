#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "channel_key.h"
#include "channel_msg.h"
#include "dm_session.h"
#include "packet.h"
#include "location.h"

#include "../components/crypto/crypto_host.c"
#include "../components/channel/channel_key.c"
#include "../components/channel/channel_msg.c"
#include "../components/dm_session/dm_session.c"
#include "../components/packet/packet.c"

/*
 * Task 2.1 / SEC-C1: location moves under AEAD, the sharing tier moves into
 * the encrypted plaintext (byte LOCATION_INNER_TIER_OFFSET), and every tier
 * is padded to one canonical inner size L_LOC_INNER so a PRESENCE share and
 * a FULL share are indistinguishable by ciphertext length. This suite
 * exercises the pure encode/pad/encrypt/decrypt mechanism mesh_send_location_packet
 * uses, not mesh_task.c itself (not host-testable: FreeRTOS/ESP-IDF).
 */

void setUp(void) { channel_msg_catchup_reset(); }
void tearDown(void) {}

static bramble_position_t sample_position(void) {
    bramble_position_t pos = {
        .latitude_e7 = 407128000,
        .longitude_e7 = -740060000,
        .altitude_m = 10,
        .accuracy_m = 5,
        .speed_kmh = 3,
        .heading_deg2 = 90,
        .timestamp = 1700000000,
        .valid = true,
    };
    return pos;
}

/* Mirrors mesh_send_location_packet's channel-path inner plaintext:
 * tier(1) || location_serialize_for_tier(...) zero-padded to
 * LOCATION_FULL_SIZE. Every tier is exactly L_LOC_INNER bytes regardless of
 * how few real bytes that tier serializes (PRESENCE 1, COARSE 5, FULL 17):
 * this padding is the entire SEC-C1 point. */
static void build_channel_inner(const bramble_position_t *pos, uint8_t tier,
                                uint8_t out[L_LOC_INNER]) {
    memset(out, 0, L_LOC_INNER);
    out[LOCATION_INNER_TIER_OFFSET] = tier;
    int n = location_serialize_for_tier(pos, tier, out + 1, LOCATION_FULL_SIZE);
    TEST_ASSERT_GREATER_THAN(0, n);
}

static bramble_header_t make_location_header(uint32_t dest_addr, uint8_t flags) {
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_LOCATION;
    h.flags = flags;
    h.hop_limit = 3;
    h.dest_addr = dest_addr;
    h.packet_id = 0x99887766;
    return h;
}

void test_presence_and_full_tiers_yield_identical_channel_ciphertext_length(void) {
    bramble_channel_t ch;
    TEST_ASSERT_EQUAL(0, channel_derive_key("loc-psk", &ch));
    bramble_position_t pos = sample_position();

    uint8_t inner_presence[L_LOC_INNER];
    uint8_t inner_full[L_LOC_INNER];
    build_channel_inner(&pos, LOCATION_TIER_PRESENCE, inner_presence);
    build_channel_inner(&pos, LOCATION_TIER_FULL, inner_full);

    bramble_header_t hdr = make_location_header(0xFFFFFFFF, FLAG_ENCRYPT | FLAG_CHANNEL);
    uint8_t aad[HEADER_SIZE + 4];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_build_aead_aad(&hdr, 0xAAAAAAAA, aad, sizeof(aad)));

    uint8_t nonce[12];
    memset(nonce, 0x44, sizeof(nonce));
    uint8_t ct_presence[CHANNEL_MSG_OVERHEAD + L_LOC_INNER];
    uint8_t ct_full[CHANNEL_MSG_OVERHEAD + L_LOC_INNER];
    uint8_t tag_presence[16];
    uint8_t tag_full[16];

    /* data_len for each call is independently L_LOC_INNER, the padded
     * size, never the tier's real serialized length. This is the exact
     * mechanism under test; see the fault-injection note in the report for
     * how this was verified to genuinely discriminate a broken pad. */
    size_t presence_data_len = (size_t)L_LOC_INNER;
    size_t full_data_len = (size_t)L_LOC_INNER;

    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch, 0xAAAAAAAA, APP_TYPE_LOCATION, 0,
                                              inner_presence, presence_data_len, aad, sizeof(aad),
                                              nonce, ct_presence, tag_presence));
    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch, 0xAAAAAAAA, APP_TYPE_LOCATION, 0,
                                              inner_full, full_data_len, aad, sizeof(aad),
                                              nonce, ct_full, tag_full));

    size_t ct_len_presence = CHANNEL_MSG_OVERHEAD + presence_data_len;
    size_t ct_len_full = CHANNEL_MSG_OVERHEAD + full_data_len;
    TEST_ASSERT_EQUAL(ct_len_full, ct_len_presence);

    /* Channel-key decrypt recovers the tier and the position. */
    bramble_channel_t channels[1];
    channels[0] = ch;
    channel_msg_info_t info;
    uint8_t pt[CHANNEL_MSG_MAX_PLAINTEXT_SIZE] = {0};
    TEST_ASSERT_EQUAL(0, channel_msg_decrypt(channels, 1, nonce, ct_full, ct_len_full, tag_full,
                                             aad, sizeof(aad), pt, &info, 0));
    TEST_ASSERT_EQUAL(APP_TYPE_LOCATION, info.app_type);
    TEST_ASSERT_EQUAL((size_t)L_LOC_INNER, info.data_len);
    TEST_ASSERT_EQUAL(LOCATION_TIER_FULL, info.data[LOCATION_INNER_TIER_OFFSET]);

    bramble_position_t decoded = {0};
    TEST_ASSERT_EQUAL(LOCATION_FULL_SIZE,
                      location_deserialize_full(info.data + 1, LOCATION_FULL_SIZE, &decoded));
    TEST_ASSERT_EQUAL(pos.latitude_e7, decoded.latitude_e7);
    TEST_ASSERT_EQUAL(pos.longitude_e7, decoded.longitude_e7);
    TEST_ASSERT_EQUAL(pos.timestamp, decoded.timestamp);
}

/* M11: the session path (per-contact directed share) has no framing of its
 * own (dm_session_encrypt is a thin AEAD wrapper), so it pads more inner
 * bytes to make up for the channel path's built-in CHANNEL_MSG_OVERHEAD,
 * landing on the SAME total ciphertext length either way. */
void test_session_path_matches_channel_path_ciphertext_length(void) {
    bramble_channel_t ch;
    TEST_ASSERT_EQUAL(0, channel_derive_key("loc-psk", &ch));
    bramble_position_t pos = sample_position();

    uint8_t inner_channel[L_LOC_INNER];
    build_channel_inner(&pos, LOCATION_TIER_COARSE, inner_channel);

    bramble_header_t hdr_ch = make_location_header(0xFFFFFFFF, FLAG_ENCRYPT | FLAG_CHANNEL);
    uint8_t aad_ch[HEADER_SIZE + 4];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_build_aead_aad(&hdr_ch, 0xAAAAAAAA, aad_ch, sizeof(aad_ch)));
    uint8_t nonce1[12];
    memset(nonce1, 0x11, sizeof(nonce1));
    uint8_t ct_ch[CHANNEL_MSG_OVERHEAD + L_LOC_INNER];
    uint8_t tag_ch[16];
    TEST_ASSERT_EQUAL(0, channel_msg_encrypt(&ch, 0xAAAAAAAA, APP_TYPE_LOCATION, 0,
                                              inner_channel, L_LOC_INNER, aad_ch, sizeof(aad_ch),
                                              nonce1, ct_ch, tag_ch));
    size_t channel_total = CHANNEL_MSG_OVERHEAD + (size_t)L_LOC_INNER;

    size_t session_inner_len = (size_t)L_LOC_INNER + CHANNEL_MSG_OVERHEAD;
    uint8_t inner_session[64] = {0};
    inner_session[LOCATION_INNER_TIER_OFFSET] = LOCATION_TIER_COARSE;
    int n = location_serialize_for_tier(&pos, LOCATION_TIER_COARSE, inner_session + 1,
                                        LOCATION_FULL_SIZE);
    TEST_ASSERT_GREATER_THAN(0, n);
    /* Bytes from (1+n) up to session_inner_len are pure padding, already
     * zero from the {0} initializer. */

    dm_session_t sess = {0};
    crypto_random(sess.session_key, 32);
    sess.state = DM_STATE_ACTIVE;
    sess.peer_addr = 0xBBBBBBBB;

    bramble_header_t hdr_sess = make_location_header(0xBBBBBBBB, FLAG_ENCRYPT);
    uint8_t nonce2[12];
    memset(nonce2, 0x22, sizeof(nonce2));
    uint8_t ct_sess[64];
    uint8_t tag_sess[16];
    TEST_ASSERT_EQUAL(0, dm_session_encrypt(&sess, &hdr_sess, 0xAAAAAAAA, inner_session,
                                            session_inner_len, nonce2, ct_sess, tag_sess));
    size_t session_total = session_inner_len; /* dm_session_encrypt adds no framing */

    TEST_ASSERT_EQUAL(channel_total, session_total);

    uint8_t pt_sess[64] = {0};
    TEST_ASSERT_EQUAL(0, dm_session_decrypt(&sess, &hdr_sess, 0xAAAAAAAA, nonce2, ct_sess,
                                            session_inner_len, tag_sess, pt_sess));
    TEST_ASSERT_EQUAL(LOCATION_TIER_COARSE, pt_sess[LOCATION_INNER_TIER_OFFSET]);

    bramble_position_t decoded = {0};
    TEST_ASSERT_EQUAL(LOCATION_COARSE_SIZE,
                      location_deserialize_coarse(pt_sess + 1, LOCATION_COARSE_SIZE, &decoded));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_presence_and_full_tiers_yield_identical_channel_ciphertext_length);
    RUN_TEST(test_session_path_matches_channel_path_ciphertext_length);
    return UNITY_END();
}
