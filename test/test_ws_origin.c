#include "unity.h"
#include "../main/ws_origin.h"

void setUp(void) {}
void tearDown(void) {}

/* ── Absent Origin (non-browser clients) ─────────────────────────────── */

void test_no_origin_header_allowed(void) {
    TEST_ASSERT_TRUE(ws_origin_allowed(NULL, "192.168.4.1", ""));
    TEST_ASSERT_TRUE(ws_origin_allowed("", "192.168.4.1", ""));
}

/* ── Same-origin (device's own IP or hostname, any port/scheme) ──────── */

void test_same_origin_ip_allowed(void) {
    TEST_ASSERT_TRUE(ws_origin_allowed("http://192.168.4.1", "192.168.4.1", ""));
}

void test_same_origin_ip_with_ports_allowed(void) {
    TEST_ASSERT_TRUE(ws_origin_allowed("http://192.168.4.1:8080", "192.168.4.1:80", ""));
    TEST_ASSERT_TRUE(ws_origin_allowed("https://192.168.4.1", "192.168.4.1:80", ""));
}

void test_same_origin_hostname_case_insensitive(void) {
    TEST_ASSERT_TRUE(ws_origin_allowed("http://Bramble-1A2B.local", "bramble-1a2b.local", ""));
}

void test_same_origin_hostname_with_path_irrelevant(void) {
    /* Origin never carries a path, but be robust if one shows up */
    TEST_ASSERT_TRUE(ws_origin_allowed("http://bramble.local/", "bramble.local", ""));
}

void test_different_host_rejected(void) {
    TEST_ASSERT_FALSE(ws_origin_allowed("http://evil.example.com", "192.168.4.1", ""));
}

void test_host_prefix_not_enough(void) {
    TEST_ASSERT_FALSE(ws_origin_allowed("http://192.168.4.10", "192.168.4.1", ""));
    TEST_ASSERT_FALSE(ws_origin_allowed("http://192.168.4.1.evil.com", "192.168.4.1", ""));
}

void test_ipv6_same_origin(void) {
    TEST_ASSERT_TRUE(ws_origin_allowed("http://[fe80::1]", "[fe80::1]:80", ""));
    TEST_ASSERT_FALSE(ws_origin_allowed("http://[fe80::2]", "[fe80::1]:80", ""));
}

/* ── "null" and malformed origins ────────────────────────────────────── */

void test_null_origin_rejected_by_default(void) {
    TEST_ASSERT_FALSE(ws_origin_allowed("null", "192.168.4.1", ""));
}

void test_null_origin_allowed_when_configured(void) {
    TEST_ASSERT_TRUE(ws_origin_allowed("null", "192.168.4.1", "null"));
}

void test_malformed_origin_rejected(void) {
    TEST_ASSERT_FALSE(ws_origin_allowed("not-a-url", "192.168.4.1", ""));
    TEST_ASSERT_FALSE(ws_origin_allowed("http://", "192.168.4.1", ""));
}

void test_empty_host_header_rejects_same_origin(void) {
    TEST_ASSERT_FALSE(ws_origin_allowed("http://192.168.4.1", "", ""));
    TEST_ASSERT_FALSE(ws_origin_allowed("http://192.168.4.1", NULL, ""));
}

/* ── Configured extra origins ────────────────────────────────────────── */

void test_extra_origin_exact_match(void) {
    TEST_ASSERT_TRUE(
        ws_origin_allowed("https://app.example.com", "192.168.4.1", "https://app.example.com"));
}

void test_extra_origin_case_insensitive(void) {
    TEST_ASSERT_TRUE(
        ws_origin_allowed("https://App.Example.Com", "192.168.4.1", "https://app.example.com"));
}

void test_extra_origin_trailing_slash_tolerated(void) {
    TEST_ASSERT_TRUE(
        ws_origin_allowed("https://app.example.com/", "192.168.4.1", "https://app.example.com"));
    TEST_ASSERT_TRUE(
        ws_origin_allowed("https://app.example.com", "192.168.4.1", "https://app.example.com/"));
}

void test_extra_origin_list_scanning(void) {
    const char* extras = "https://a.example.com,https://b.example.com, https://c.example.com";
    TEST_ASSERT_TRUE(ws_origin_allowed("https://b.example.com", "192.168.4.1", extras));
    TEST_ASSERT_TRUE(ws_origin_allowed("https://c.example.com", "192.168.4.1", extras));
    TEST_ASSERT_FALSE(ws_origin_allowed("https://d.example.com", "192.168.4.1", extras));
}

void test_extra_origin_scheme_must_match(void) {
    TEST_ASSERT_FALSE(
        ws_origin_allowed("http://app.example.com", "192.168.4.1", "https://app.example.com"));
}

void test_extra_origin_port_must_match(void) {
    TEST_ASSERT_FALSE(ws_origin_allowed("https://app.example.com:8443", "192.168.4.1",
                                        "https://app.example.com"));
    TEST_ASSERT_TRUE(ws_origin_allowed("https://app.example.com:8443", "192.168.4.1",
                                       "https://app.example.com:8443"));
}

void test_extra_origin_substring_not_enough(void) {
    TEST_ASSERT_FALSE(ws_origin_allowed("https://app.example.com.evil.com", "192.168.4.1",
                                        "https://app.example.com"));
}

void test_null_extras_rejects_cross_origin(void) {
    TEST_ASSERT_FALSE(ws_origin_allowed("https://app.example.com", "192.168.4.1", NULL));
}

/* ── Authority parsing hardening (userinfo, trailing dot, IPv6 case) ── */

void test_userinfo_origin_never_accepted(void) {
    /* '@' in the authority is malformed for an Origin; must not be
       parsed as "userinfo before the real host" or vice versa */
    TEST_ASSERT_FALSE(ws_origin_allowed("http://192.168.4.1@evil.com", "192.168.4.1", ""));
    TEST_ASSERT_FALSE(ws_origin_allowed("http://evil.com@192.168.4.1", "192.168.4.1", ""));
    TEST_ASSERT_FALSE(ws_origin_allowed("http://device@evil.com", "device", ""));
}

void test_trailing_dot_fqdn_equivalence(void) {
    TEST_ASSERT_TRUE(ws_origin_allowed("http://bramble.local.", "bramble.local", ""));
    TEST_ASSERT_TRUE(ws_origin_allowed("http://bramble.local", "bramble.local.", ""));
    TEST_ASSERT_FALSE(ws_origin_allowed("http://bramble.local.evil.com", "bramble.local", ""));
}

void test_ipv6_case_insensitive(void) {
    TEST_ASSERT_TRUE(ws_origin_allowed("http://[FE80::1]", "[fe80::1]:80", ""));
}

/* ── /config POST CSRF decision ──────────────────────────────────────── */

void test_config_post_no_browser_headers_allowed(void) {
    TEST_ASSERT_TRUE(ws_config_post_allowed(NULL, NULL, "192.168.4.1", ""));
    TEST_ASSERT_TRUE(ws_config_post_allowed("", "", "192.168.4.1", ""));
}

void test_config_post_same_origin_allowed(void) {
    TEST_ASSERT_TRUE(ws_config_post_allowed("http://192.168.4.1", NULL, "192.168.4.1", ""));
}

void test_config_post_foreign_origin_rejected(void) {
    TEST_ASSERT_FALSE(ws_config_post_allowed("http://evil.example.com", NULL, "192.168.4.1", ""));
    TEST_ASSERT_FALSE(ws_config_post_allowed("null", NULL, "192.168.4.1", ""));
}

void test_config_post_allowlisted_origin_allowed(void) {
    TEST_ASSERT_TRUE(ws_config_post_allowed("https://app.example.com", NULL, "192.168.4.1",
                                            "https://app.example.com"));
}

void test_config_post_same_origin_referer_allowed(void) {
    /* Older browsers may omit Origin on same-origin POSTs but send Referer */
    TEST_ASSERT_TRUE(ws_config_post_allowed(NULL, "http://192.168.4.1/", "192.168.4.1", ""));
    TEST_ASSERT_TRUE(
        ws_config_post_allowed(NULL, "http://192.168.4.1/index.html", "192.168.4.1:80", ""));
}

void test_config_post_foreign_referer_rejected(void) {
    TEST_ASSERT_FALSE(
        ws_config_post_allowed(NULL, "http://evil.example.com/csrf.html", "192.168.4.1", ""));
}

void test_config_post_malformed_referer_rejected(void) {
    TEST_ASSERT_FALSE(ws_config_post_allowed(NULL, "not-a-url", "192.168.4.1", ""));
}

void test_config_post_origin_takes_precedence_over_referer(void) {
    /* A foreign Origin with a spoofed same-origin Referer must lose */
    TEST_ASSERT_FALSE(ws_config_post_allowed("http://evil.example.com", "http://192.168.4.1/",
                                             "192.168.4.1", ""));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_origin_header_allowed);
    RUN_TEST(test_same_origin_ip_allowed);
    RUN_TEST(test_same_origin_ip_with_ports_allowed);
    RUN_TEST(test_same_origin_hostname_case_insensitive);
    RUN_TEST(test_same_origin_hostname_with_path_irrelevant);
    RUN_TEST(test_different_host_rejected);
    RUN_TEST(test_host_prefix_not_enough);
    RUN_TEST(test_ipv6_same_origin);
    RUN_TEST(test_null_origin_rejected_by_default);
    RUN_TEST(test_null_origin_allowed_when_configured);
    RUN_TEST(test_malformed_origin_rejected);
    RUN_TEST(test_empty_host_header_rejects_same_origin);
    RUN_TEST(test_extra_origin_exact_match);
    RUN_TEST(test_extra_origin_case_insensitive);
    RUN_TEST(test_extra_origin_trailing_slash_tolerated);
    RUN_TEST(test_extra_origin_list_scanning);
    RUN_TEST(test_extra_origin_scheme_must_match);
    RUN_TEST(test_extra_origin_port_must_match);
    RUN_TEST(test_extra_origin_substring_not_enough);
    RUN_TEST(test_null_extras_rejects_cross_origin);
    RUN_TEST(test_userinfo_origin_never_accepted);
    RUN_TEST(test_trailing_dot_fqdn_equivalence);
    RUN_TEST(test_ipv6_case_insensitive);
    RUN_TEST(test_config_post_no_browser_headers_allowed);
    RUN_TEST(test_config_post_same_origin_allowed);
    RUN_TEST(test_config_post_foreign_origin_rejected);
    RUN_TEST(test_config_post_allowlisted_origin_allowed);
    RUN_TEST(test_config_post_same_origin_referer_allowed);
    RUN_TEST(test_config_post_foreign_referer_rejected);
    RUN_TEST(test_config_post_malformed_referer_rejected);
    RUN_TEST(test_config_post_origin_takes_precedence_over_referer);
    return UNITY_END();
}
