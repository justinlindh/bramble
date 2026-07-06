/*
 * Trust-anchor RPC surface contract (P0): bramble.setAnchor,
 * bramble.getAnchorStatus, and the getIdentity ed25519_pub extension, driven
 * end to end through the real rpc_methods.c handlers + dispatcher (same
 * harness as test_rpc_set_auth_token_contract.c). The real identity.c backs
 * the anchor store, so these exercise genuine set/persist/fingerprint
 * behavior rather than a stub.
 */
#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include "rpc_auth.h"
#include "identity.h"
#include <stdio.h>
#include <string.h>

extern bool g_nvs_allow_open;

/* Known identity Ed25519 public key so the getIdentity hex is checkable. */
static bramble_identity_t s_id = {
    .address = 0xAABBCCDD,
    .pubkey_hash = 0x11223344,
};

void setUp(void) {
    g_nvs_allow_open = true;
    for (int i = 0; i < 32; i++)
        s_id.ed25519_public_key[i] = (uint8_t)(0xA0 + i);
    identity_host_store_reset();
    identity_anchor_clear();
    rpc_init();
    rpc_methods_init(&s_id);
}

void tearDown(void) {}

/* Dispatch a request and return the parsed response (caller frees). */
static cJSON* dispatch(const char* req) {
    char response[1024];
    int len = rpc_dispatch(req, response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);
    cJSON* j = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(j);
    return j;
}

static int error_code(cJSON* j) {
    cJSON* err = cJSON_GetObjectItem(j, "error");
    return err ? cJSON_GetObjectItem(err, "code")->valueint : 0;
}

static cJSON* result_of(cJSON* j) {
    cJSON* res = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_NOT_NULL(res);
    return res;
}

/* --- getIdentity now exposes the full identity Ed25519 public key -------- */

void test_get_identity_exposes_full_ed25519_pub(void) {
    cJSON* j = dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.getIdentity\"}");
    cJSON* res = result_of(j);
    cJSON* pub = cJSON_GetObjectItem(res, "ed25519_pub");
    TEST_ASSERT_NOT_NULL(pub);
    TEST_ASSERT_TRUE(cJSON_IsString(pub));

    char want[65];
    for (int i = 0; i < 32; i++)
        snprintf(want + i * 2, 3, "%02x", s_id.ed25519_public_key[i]);
    want[64] = '\0';
    TEST_ASSERT_EQUAL_STRING(want, pub->valuestring);

    /* The existing address/pubkey_hash fields remain. */
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(res, "address"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(res, "pubkey_hash"));
    cJSON_Delete(j);
}

/* --- getAnchorStatus: unanchored by default ------------------------------ */

void test_get_anchor_status_unanchored_by_default(void) {
    cJSON* j = dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.getAnchorStatus\"}");
    cJSON* res = result_of(j);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "anchored")));
    /* Fingerprint is present only when anchored. */
    TEST_ASSERT_NULL(cJSON_GetObjectItem(res, "anchor_fingerprint"));
    cJSON_Delete(j);
}

/* --- setAnchor: valid key provisions; status reflects it ----------------- */

void test_set_anchor_then_status_reports_fingerprint(void) {
    /* Anchor pubkey = 00..1f; SHA256(00..1f)[0:4] = 63 0d cd 29. */
    const char* req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.setAnchor\",\"params\":"
                      "{\"anchor_pubkey\":\"000102030405060708090a0b0c0d0e0f"
                      "101112131415161718191a1b1c1d1e1f\"}}";
    cJSON* j = dispatch(req);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(result_of(j), "ok")));
    cJSON_Delete(j);

    j = dispatch("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bramble.getAnchorStatus\"}");
    cJSON* res = result_of(j);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "anchored")));
    cJSON* fp = cJSON_GetObjectItem(res, "anchor_fingerprint");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_STRING("630dcd29", fp->valuestring);
    cJSON_Delete(j);

    /* And the anchor is really provisioned in the identity module. */
    TEST_ASSERT_TRUE(identity_anchor_is_set());
}

/* --- setAnchor param validation (mirrors setNetworkKey) ------------------- */

void test_set_anchor_rejects_short_key(void) {
    cJSON* j = dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.setAnchor\",\"params\":"
                        "{\"anchor_pubkey\":\"00\"}}");
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    TEST_ASSERT_FALSE(identity_anchor_is_set());
    cJSON_Delete(j);
}

void test_set_anchor_rejects_non_hex(void) {
    /* 64 chars but with non-hex nibbles ('g'). */
    const char* req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.setAnchor\",\"params\":"
                      "{\"anchor_pubkey\":\"gg0102030405060708090a0b0c0d0e0f"
                      "101112131415161718191a1b1c1d1e1f\"}}";
    cJSON* j = dispatch(req);
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    TEST_ASSERT_FALSE(identity_anchor_is_set());
    cJSON_Delete(j);
}

void test_set_anchor_rejects_missing_param(void) {
    cJSON* j = dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.setAnchor\","
                        "\"params\":{}}");
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    cJSON_Delete(j);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_get_identity_exposes_full_ed25519_pub);
    RUN_TEST(test_get_anchor_status_unanchored_by_default);
    RUN_TEST(test_set_anchor_then_status_reports_fingerprint);
    RUN_TEST(test_set_anchor_rejects_short_key);
    RUN_TEST(test_set_anchor_rejects_non_hex);
    RUN_TEST(test_set_anchor_rejects_missing_param);
    return UNITY_END();
}
