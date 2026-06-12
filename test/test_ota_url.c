#include <string.h>

#include "ota_url.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ── Origin validation ─────────────────────────────────────────────── */

void test_origin_official_default_is_valid(void) {
    TEST_ASSERT_TRUE(ota_url_origin_valid("https://bramblemesh.org/ota/", false));
}

void test_origin_https_without_trailing_slash_is_valid(void) {
    TEST_ASSERT_TRUE(ota_url_origin_valid("https://bramblemesh.org/ota", false));
}

void test_origin_https_with_port_is_valid(void) {
    TEST_ASSERT_TRUE(ota_url_origin_valid("https://192.168.6.34:8443/fw/", false));
}

void test_origin_http_rejected_unless_allowed(void) {
    TEST_ASSERT_FALSE(ota_url_origin_valid("http://192.168.6.34:8088/", false));
    TEST_ASSERT_TRUE(ota_url_origin_valid("http://192.168.6.34:8088/", true));
}

void test_origin_scheme_is_case_insensitive(void) {
    TEST_ASSERT_TRUE(ota_url_origin_valid("HTTPS://bramblemesh.org/ota/", false));
    TEST_ASSERT_FALSE(ota_url_origin_valid("HTTP://bramblemesh.org/ota/", false));
}

void test_origin_rejects_other_schemes(void) {
    TEST_ASSERT_FALSE(ota_url_origin_valid("ftp://bramblemesh.org/", true));
    TEST_ASSERT_FALSE(ota_url_origin_valid("file:///etc/", true));
    TEST_ASSERT_FALSE(ota_url_origin_valid("bramblemesh.org/ota/", true));
    TEST_ASSERT_FALSE(ota_url_origin_valid("//bramblemesh.org/ota/", true));
}

void test_origin_rejects_empty_or_null(void) {
    TEST_ASSERT_FALSE(ota_url_origin_valid("", false));
    TEST_ASSERT_FALSE(ota_url_origin_valid(NULL, false));
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://", false));
    TEST_ASSERT_FALSE(ota_url_origin_valid("https:///ota/", false));
}

void test_origin_rejects_userinfo(void) {
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://user@evil.com/", false));
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://bramblemesh.org@evil.com/", false));
}

void test_origin_rejects_query_and_fragment(void) {
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://bramblemesh.org/ota/?x=1", false));
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://bramblemesh.org/ota/#frag", false));
}

void test_origin_rejects_whitespace_and_control(void) {
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://bramblemesh.org/ota /", false));
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://bramblemesh.org/ota/\n", false));
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://bramble\tmesh.org/", false));
}

void test_origin_rejects_backslash_and_percent(void) {
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://bramblemesh.org\\ota/", false));
    TEST_ASSERT_FALSE(ota_url_origin_valid("https://bramblemesh.org/%6fta/", false));
}

void test_origin_rejects_overlong(void) {
    char big[OTA_URL_MAX + 32];
    memset(big, 'a', sizeof(big));
    memcpy(big, "https://", 8);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_FALSE(ota_url_origin_valid(big, false));
}

/* ── Path validation ───────────────────────────────────────────────── */

void test_path_typical_artifact_is_valid(void) {
    TEST_ASSERT_TRUE(ota_url_path_valid("stable/v1.4.0/heltec-v3/bramble.bin"));
    TEST_ASSERT_TRUE(ota_url_path_valid("dev/v1.4.6-dev.3.gabc123/tdeck-plus/bramble.bin"));
    TEST_ASSERT_TRUE(ota_url_path_valid("bramble.bin"));
}

void test_path_rejects_empty_or_null(void) {
    TEST_ASSERT_FALSE(ota_url_path_valid(""));
    TEST_ASSERT_FALSE(ota_url_path_valid(NULL));
}

void test_path_rejects_absolute_and_protocol_relative(void) {
    TEST_ASSERT_FALSE(ota_url_path_valid("/etc/passwd"));
    TEST_ASSERT_FALSE(ota_url_path_valid("//evil.com/bramble.bin"));
}

void test_path_rejects_foreign_absolute_urls(void) {
    TEST_ASSERT_FALSE(ota_url_path_valid("https://evil.com/bramble.bin"));
    TEST_ASSERT_FALSE(ota_url_path_valid("http://evil.com/bramble.bin"));
    TEST_ASSERT_FALSE(ota_url_path_valid("HtTpS://evil.com/bramble.bin"));
}

void test_path_rejects_high_bit_and_utf8(void) {
    /* The path charset is isalnum + ._+-/ so any high-bit byte (and thus any
     * multi-byte UTF-8 sequence) must be rejected, not silently passed to
     * the HTTP client. */
    TEST_ASSERT_FALSE(ota_url_path_valid("stable/v1/brÃ¤mble.bin"));
    TEST_ASSERT_FALSE(ota_url_path_valid("stable/â®/bramble.bin"));
    TEST_ASSERT_FALSE(ota_url_path_valid("ÿ"));
    TEST_ASSERT_FALSE(ota_url_path_valid("stable/v1/bin"));
}

void test_path_rejects_traversal(void) {
    TEST_ASSERT_FALSE(ota_url_path_valid(".."));
    TEST_ASSERT_FALSE(ota_url_path_valid("../secrets"));
    TEST_ASSERT_FALSE(ota_url_path_valid("stable/../../etc/passwd"));
    TEST_ASSERT_FALSE(ota_url_path_valid("stable/.."));
    TEST_ASSERT_FALSE(ota_url_path_valid("./bramble.bin"));
    TEST_ASSERT_FALSE(ota_url_path_valid("stable/./bramble.bin"));
}

void test_path_allows_dotted_filenames_but_not_dot_segments(void) {
    TEST_ASSERT_TRUE(ota_url_path_valid("stable/v1.4.0/bramble.bin"));
    TEST_ASSERT_TRUE(ota_url_path_valid("stable/..hidden/bramble.bin"));
}

void test_path_rejects_percent_encoding(void) {
    TEST_ASSERT_FALSE(ota_url_path_valid("stable/%2e%2e/secrets"));
    TEST_ASSERT_FALSE(ota_url_path_valid("bramble%00.bin"));
}

void test_path_rejects_query_fragment_userinfo_backslash(void) {
    TEST_ASSERT_FALSE(ota_url_path_valid("bramble.bin?x=1"));
    TEST_ASSERT_FALSE(ota_url_path_valid("bramble.bin#frag"));
    TEST_ASSERT_FALSE(ota_url_path_valid("a@b/bramble.bin"));
    TEST_ASSERT_FALSE(ota_url_path_valid("stable\\bramble.bin"));
}

void test_path_rejects_colon_anywhere(void) {
    TEST_ASSERT_FALSE(ota_url_path_valid("stable:8080/bramble.bin"));
}

void test_path_rejects_whitespace_control_and_empty_segments(void) {
    TEST_ASSERT_FALSE(ota_url_path_valid("stable /bramble.bin"));
    TEST_ASSERT_FALSE(ota_url_path_valid("stable\n/bramble.bin"));
    TEST_ASSERT_FALSE(ota_url_path_valid("stable//bramble.bin"));
    TEST_ASSERT_FALSE(ota_url_path_valid("stable/bramble.bin/"));
}

/* ── Resolution ────────────────────────────────────────────────────── */

void test_resolve_joins_with_trailing_slash_origin(void) {
    char out[OTA_URL_MAX];
    TEST_ASSERT_EQUAL_INT(OTA_URL_OK, ota_url_resolve("https://bramblemesh.org/ota/",
                                                      "stable/v1.4.0/heltec-v3/bramble.bin", false,
                                                      out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://bramblemesh.org/ota/stable/v1.4.0/heltec-v3/bramble.bin",
                             out);
}

void test_resolve_inserts_slash_when_origin_lacks_one(void) {
    char out[OTA_URL_MAX];
    TEST_ASSERT_EQUAL_INT(OTA_URL_OK, ota_url_resolve("https://bramblemesh.org/ota", "bramble.bin",
                                                      false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://bramblemesh.org/ota/bramble.bin", out);
}

void test_resolve_rejects_bad_origin(void) {
    char out[OTA_URL_MAX] = "sentinel";
    TEST_ASSERT_EQUAL_INT(
        OTA_URL_ERR_ORIGIN,
        ota_url_resolve("http://192.168.6.34:8088/", "bramble.bin", false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_resolve_allows_http_origin_in_dev_mode(void) {
    char out[OTA_URL_MAX];
    TEST_ASSERT_EQUAL_INT(OTA_URL_OK, ota_url_resolve("http://192.168.6.34:8088", "bramble.bin",
                                                      true, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.6.34:8088/bramble.bin", out);
}

void test_resolve_rejects_bad_path(void) {
    char out[OTA_URL_MAX] = "sentinel";
    TEST_ASSERT_EQUAL_INT(OTA_URL_ERR_PATH,
                          ota_url_resolve("https://bramblemesh.org/ota/",
                                          "https://evil.com/bramble.bin", false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_resolve_rejects_overflow(void) {
    char out[32] = "sentinel";
    TEST_ASSERT_EQUAL_INT(OTA_URL_ERR_TOOLONG,
                          ota_url_resolve("https://bramblemesh.org/ota/",
                                          "stable/v1.4.0/heltec-v3/bramble.bin", false, out,
                                          sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_origin_official_default_is_valid);
    RUN_TEST(test_origin_https_without_trailing_slash_is_valid);
    RUN_TEST(test_origin_https_with_port_is_valid);
    RUN_TEST(test_origin_http_rejected_unless_allowed);
    RUN_TEST(test_origin_scheme_is_case_insensitive);
    RUN_TEST(test_origin_rejects_other_schemes);
    RUN_TEST(test_origin_rejects_empty_or_null);
    RUN_TEST(test_origin_rejects_userinfo);
    RUN_TEST(test_origin_rejects_query_and_fragment);
    RUN_TEST(test_origin_rejects_whitespace_and_control);
    RUN_TEST(test_origin_rejects_backslash_and_percent);
    RUN_TEST(test_origin_rejects_overlong);
    RUN_TEST(test_path_typical_artifact_is_valid);
    RUN_TEST(test_path_rejects_empty_or_null);
    RUN_TEST(test_path_rejects_absolute_and_protocol_relative);
    RUN_TEST(test_path_rejects_foreign_absolute_urls);
    RUN_TEST(test_path_rejects_traversal);
    RUN_TEST(test_path_rejects_high_bit_and_utf8);
    RUN_TEST(test_path_allows_dotted_filenames_but_not_dot_segments);
    RUN_TEST(test_path_rejects_percent_encoding);
    RUN_TEST(test_path_rejects_query_fragment_userinfo_backslash);
    RUN_TEST(test_path_rejects_colon_anywhere);
    RUN_TEST(test_path_rejects_whitespace_control_and_empty_segments);
    RUN_TEST(test_resolve_joins_with_trailing_slash_origin);
    RUN_TEST(test_resolve_inserts_slash_when_origin_lacks_one);
    RUN_TEST(test_resolve_rejects_bad_origin);
    RUN_TEST(test_resolve_allows_http_origin_in_dev_mode);
    RUN_TEST(test_resolve_rejects_bad_path);
    RUN_TEST(test_resolve_rejects_overflow);
    return UNITY_END();
}
