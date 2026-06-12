#include <string.h>

#include "ota_version.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static int cmp_str(const char* a, const char* b) {
    int c = 0;
    TEST_ASSERT_TRUE_MESSAGE(ota_version_cmp_str(a, b, &c), "expected both versions to parse");
    return c;
}

/* ── Parsing ───────────────────────────────────────────────────────── */

void test_parse_plain_release(void) {
    ota_semver_t v;
    TEST_ASSERT_TRUE(ota_version_parse("1.4.0", &v));
    TEST_ASSERT_EQUAL_INT(1, v.major);
    TEST_ASSERT_EQUAL_INT(4, v.minor);
    TEST_ASSERT_EQUAL_INT(0, v.patch);
    TEST_ASSERT_EQUAL_STRING("", v.prerelease);
}

void test_parse_leading_v_and_prerelease(void) {
    ota_semver_t v;
    TEST_ASSERT_TRUE(ota_version_parse("v1.4.6-dev.3.gabc123", &v));
    TEST_ASSERT_EQUAL_INT(1, v.major);
    TEST_ASSERT_EQUAL_INT(4, v.minor);
    TEST_ASSERT_EQUAL_INT(6, v.patch);
    TEST_ASSERT_EQUAL_STRING("dev.3.gabc123", v.prerelease);
}

void test_parse_strips_build_metadata(void) {
    ota_semver_t v;
    TEST_ASSERT_TRUE(ota_version_parse("1.2.3+sha.abc", &v));
    TEST_ASSERT_EQUAL_STRING("", v.prerelease);
    TEST_ASSERT_TRUE(ota_version_parse("1.2.3-rc.1+sha.abc", &v));
    TEST_ASSERT_EQUAL_STRING("rc.1", v.prerelease);
}

void test_parse_rejects_malformed(void) {
    ota_semver_t v;
    TEST_ASSERT_FALSE(ota_version_parse(NULL, &v));
    TEST_ASSERT_FALSE(ota_version_parse("", &v));
    TEST_ASSERT_FALSE(ota_version_parse("1.4", &v));
    TEST_ASSERT_FALSE(ota_version_parse("1", &v));
    TEST_ASSERT_FALSE(ota_version_parse("a.b.c", &v));
    TEST_ASSERT_FALSE(ota_version_parse("1.4.x", &v));
    TEST_ASSERT_FALSE(ota_version_parse("1..4", &v));
    TEST_ASSERT_FALSE(ota_version_parse("1.4.0-", &v));
    TEST_ASSERT_FALSE(ota_version_parse("unknown", &v));
}

void test_parse_rejects_negative_and_garbage_suffix(void) {
    ota_semver_t v;
    TEST_ASSERT_FALSE(ota_version_parse("1.4.-1", &v));
    TEST_ASSERT_FALSE(ota_version_parse("1.4.0 extra", &v));
}

/* ── Comparison ────────────────────────────────────────────────────── */

void test_cmp_core_versions(void) {
    TEST_ASSERT_TRUE(cmp_str("1.4.0", "1.4.1") < 0);
    TEST_ASSERT_TRUE(cmp_str("1.5.0", "1.4.9") > 0);
    TEST_ASSERT_TRUE(cmp_str("2.0.0", "1.99.99") > 0);
    TEST_ASSERT_EQUAL_INT(0, cmp_str("1.4.0", "v1.4.0"));
}

void test_cmp_release_outranks_prerelease(void) {
    TEST_ASSERT_TRUE(cmp_str("1.4.0", "1.4.0-dev.9.gabc") > 0);
    TEST_ASSERT_TRUE(cmp_str("1.4.0-rc.1", "1.4.0") < 0);
}

void test_cmp_prerelease_numeric_fields(void) {
    TEST_ASSERT_TRUE(cmp_str("1.4.6-dev.3.gaaa", "1.4.6-dev.10.gbbb") < 0);
    TEST_ASSERT_TRUE(cmp_str("1.4.6-dev.11.gaaa", "1.4.6-dev.2.gbbb") > 0);
}

void test_cmp_prerelease_alpha_and_mixed_fields(void) {
    /* numeric identifiers rank below alphanumeric ones */
    TEST_ASSERT_TRUE(cmp_str("1.0.0-1", "1.0.0-alpha") < 0);
    TEST_ASSERT_TRUE(cmp_str("1.0.0-alpha", "1.0.0-beta") < 0);
    /* longer prerelease wins when fields are a strict prefix */
    TEST_ASSERT_TRUE(cmp_str("1.0.0-alpha", "1.0.0-alpha.1") < 0);
}

void test_cmp_higher_core_beats_any_prerelease(void) {
    TEST_ASSERT_TRUE(cmp_str("1.4.7-dev.0.gabc", "1.4.6") > 0);
}

void test_cmp_str_fails_on_unparseable_input(void) {
    int c = 0;
    TEST_ASSERT_FALSE(ota_version_cmp_str("garbage", "1.4.0", &c));
    TEST_ASSERT_FALSE(ota_version_cmp_str("1.4.0", "", &c));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_plain_release);
    RUN_TEST(test_parse_leading_v_and_prerelease);
    RUN_TEST(test_parse_strips_build_metadata);
    RUN_TEST(test_parse_rejects_malformed);
    RUN_TEST(test_parse_rejects_negative_and_garbage_suffix);
    RUN_TEST(test_cmp_core_versions);
    RUN_TEST(test_cmp_release_outranks_prerelease);
    RUN_TEST(test_cmp_prerelease_numeric_fields);
    RUN_TEST(test_cmp_prerelease_alpha_and_mixed_fields);
    RUN_TEST(test_cmp_higher_core_beats_any_prerelease);
    RUN_TEST(test_cmp_str_fails_on_unparseable_input);
    return UNITY_END();
}
