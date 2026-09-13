#include "unity.h"
#include "tz_store.h"
#include "nvs_fake.h"
#include "nvs_keys.h"

#include <string.h>

/* A distinct valid spec so a successful read is never confused with the
 * BRAMBLE_TZ_DEFAULT_SPEC fallback. */
#define VALID_STD_SPEC "MST7"
#define VALID_DST_SPEC "PST8PDT,M3.2.0,M11.1.0"

void setUp(void) { nvs_fake_reset(); }
void tearDown(void) {}

void test_default_when_unset(void) {
    char out[BRAMBLE_TZ_SPEC_MAX];
    memset(out, 'x', sizeof(out));
    tz_store_get(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(BRAMBLE_TZ_DEFAULT_SPEC, out);
    TEST_ASSERT_FALSE(tz_store_is_configured());
}

void test_set_get_roundtrip_std(void) {
    TEST_ASSERT_EQUAL_INT(0, tz_store_set(VALID_STD_SPEC));
    char out[BRAMBLE_TZ_SPEC_MAX];
    tz_store_get(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(VALID_STD_SPEC, out);
    TEST_ASSERT_TRUE(tz_store_is_configured());
}

void test_set_get_roundtrip_dst(void) {
    TEST_ASSERT_EQUAL_INT(0, tz_store_set(VALID_DST_SPEC));
    char out[BRAMBLE_TZ_SPEC_MAX];
    tz_store_get(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(VALID_DST_SPEC, out);
    TEST_ASSERT_TRUE(tz_store_is_configured());
}

void test_invalid_spec_rejected_and_not_persisted(void) {
    /* Month 13 is out of range, so the rule never parses. */
    TEST_ASSERT_EQUAL_INT(-1, tz_store_set("PST8PDT,M13.2.0,M11.1.0"));
    TEST_ASSERT_FALSE(tz_store_is_configured());
    char out[BRAMBLE_TZ_SPEC_MAX];
    tz_store_get(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(BRAMBLE_TZ_DEFAULT_SPEC, out);
}

void test_corrupt_stored_spec_falls_back(void) {
    /* Simulate a value that was valid once but no longer parses (truncation,
     * flash corruption). It bypasses tz_store_set's validation by going
     * straight into NVS, which is the only way such a value can exist. */
    const char* corrupt = "12345";
    nvs_fake_put_blob(NVS_NS_BRAMBLE, NVS_KEY_TZ, corrupt, strlen(corrupt) + 1);
    char out[BRAMBLE_TZ_SPEC_MAX];
    tz_store_get(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(BRAMBLE_TZ_DEFAULT_SPEC, out);
    TEST_ASSERT_FALSE(tz_store_is_configured());
}

void test_empty_stored_spec_treated_as_unset(void) {
    nvs_fake_put_blob(NVS_NS_BRAMBLE, NVS_KEY_TZ, "", 1);
    char out[BRAMBLE_TZ_SPEC_MAX];
    tz_store_get(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(BRAMBLE_TZ_DEFAULT_SPEC, out);
    TEST_ASSERT_FALSE(tz_store_is_configured());
}

void test_nvs_unavailable(void) {
    nvs_fake_set_open_fails(true);
    TEST_ASSERT_EQUAL_INT(-2, tz_store_set(VALID_STD_SPEC));
    char out[BRAMBLE_TZ_SPEC_MAX];
    tz_store_get(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(BRAMBLE_TZ_DEFAULT_SPEC, out);
    TEST_ASSERT_FALSE(tz_store_is_configured());
}

void test_get_ignores_null_and_zero_len(void) {
    /* Guard clauses must not write or crash. */
    tz_store_get(NULL, 8);
    char out[4] = {'k', 'e', 'e', 'p'};
    tz_store_get(out, 0);
    TEST_ASSERT_EQUAL_MEMORY("keep", out, 4);
}

void test_overwrite_replaces_previous(void) {
    TEST_ASSERT_EQUAL_INT(0, tz_store_set(VALID_STD_SPEC));
    TEST_ASSERT_EQUAL_INT(0, tz_store_set(VALID_DST_SPEC));
    char out[BRAMBLE_TZ_SPEC_MAX];
    tz_store_get(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(VALID_DST_SPEC, out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_when_unset);
    RUN_TEST(test_set_get_roundtrip_std);
    RUN_TEST(test_set_get_roundtrip_dst);
    RUN_TEST(test_invalid_spec_rejected_and_not_persisted);
    RUN_TEST(test_corrupt_stored_spec_falls_back);
    RUN_TEST(test_empty_stored_spec_treated_as_unset);
    RUN_TEST(test_nvs_unavailable);
    RUN_TEST(test_get_ignores_null_and_zero_len);
    RUN_TEST(test_overwrite_replaces_previous);
    return UNITY_END();
}
