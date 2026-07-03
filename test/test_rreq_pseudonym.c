#include "unity.h"
#include <string.h>
#include "crypto.h"
#include "rreq_pseudonym.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t KEY[BRAMBLE_KEY_SIZE] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20};

/* Reference: reproduce the exact pre-extraction construction. */
static uint32_t reference_pseudonym(const uint8_t *k, uint32_t addr, uint32_t qid) {
    uint8_t input[8];
    memcpy(input, &addr, 4);
    memcpy(input + 4, &qid, 4);
    uint8_t mac[32];
    crypto_hmac_sha256(k, BRAMBLE_KEY_SIZE, input, sizeof(input), mac);
    uint32_t out;
    memcpy(&out, mac, 4);
    return out;
}

void test_matches_original_construction(void) {
    TEST_ASSERT_EQUAL_UINT32(reference_pseudonym(KEY, 0xA1B2C3D4u, 0x11223344u),
                             rreq_pseudonym_generate(KEY, 0xA1B2C3D4u, 0x11223344u));
}

void test_is_deterministic(void) {
    uint32_t a = rreq_pseudonym_generate(KEY, 0xDEADBEEFu, 7u);
    uint32_t b = rreq_pseudonym_generate(KEY, 0xDEADBEEFu, 7u);
    TEST_ASSERT_EQUAL_UINT32(a, b);
}

void test_query_id_changes_output(void) {
    uint32_t a = rreq_pseudonym_generate(KEY, 0xDEADBEEFu, 7u);
    uint32_t b = rreq_pseudonym_generate(KEY, 0xDEADBEEFu, 8u);
    TEST_ASSERT_NOT_EQUAL(a, b);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_matches_original_construction);
    RUN_TEST(test_is_deterministic);
    RUN_TEST(test_query_id_changes_output);
    return UNITY_END();
}
