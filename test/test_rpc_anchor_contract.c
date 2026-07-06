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
    identity_endorsement_clear_mem();
    rpc_init();
    rpc_methods_init(&s_id);
}

void tearDown(void) {}

/* Shared response helpers (defined below). */
static cJSON* dispatch(const char* req);
static int error_code(cJSON* j);
static cJSON* result_of(cJSON* j);

/* --- setEndorsement (P1) -------------------------------------------------- *
 * Provisions the node's OWN endorsement cert. The handler verifies the cert
 * against THIS node's ed25519 identity key + the provisioned anchor before
 * persisting, so these tests sign a genuine cert with a fixed anchor key.
 */

/* Fixed anchor seed (RFC 8032) -> deterministic anchor keypair. */
static const uint8_t ANCHOR_SEED[32] = {
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF};

static void to_hex(const uint8_t* in, size_t n, char* out) {
    for (size_t i = 0; i < n; i++)
        snprintf(out + i * 2, 3, "%02x", in[i]);
    out[n * 2] = '\0';
}

/* Provision the fixed anchor via setAnchor and return its private key so the
 * test can sign a cert. */
static void provision_anchor(uint8_t anchor_priv[64]) {
    uint8_t anchor_pub[32];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_priv));
    char pub_hex[65];
    to_hex(anchor_pub, 32, pub_hex);
    char req[256];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.setAnchor\","
             "\"params\":{\"anchor_pubkey\":\"%s\"}}",
             pub_hex);
    cJSON* j = dispatch(req);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(result_of(j), "ok")));
    cJSON_Delete(j);
}

/* Sign an endorsement over `node_pub` with the anchor key at `not_after`. */
static void sign_cert(const uint8_t anchor_priv[64], const uint8_t node_pub[32], uint64_t not_after,
                      uint8_t sig[64]) {
    uint8_t msg[IDENTITY_ENDORSEMENT_MSG_SIZE];
    TEST_ASSERT_EQUAL(IDENTITY_ENDORSEMENT_MSG_SIZE,
                      identity_endorsement_msg(node_pub, not_after, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(anchor_priv, msg, sizeof(msg), sig));
}

static cJSON* dispatch_set_endorsement(const char* na_hex, const char* sig_hex) {
    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.setEndorsement\","
             "\"params\":{\"not_after\":\"%s\",\"endorsement_sig\":\"%s\"}}",
             na_hex, sig_hex);
    return dispatch(req);
}

static bool status_endorsed(void) {
    cJSON* j = dispatch("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"bramble.getAnchorStatus\"}");
    cJSON* e = cJSON_GetObjectItem(result_of(j), "endorsed");
    TEST_ASSERT_NOT_NULL(e);
    bool v = cJSON_IsTrue(e);
    cJSON_Delete(j);
    return v;
}

/* Valid cert (permanent) provisions and flips getAnchorStatus.endorsed. */
void test_set_endorsement_valid_persists_and_status_flips(void) {
    TEST_ASSERT_FALSE(status_endorsed()); /* not endorsed before */
    uint8_t anchor_priv[64];
    provision_anchor(anchor_priv);
    uint8_t sig[64];
    sign_cert(anchor_priv, s_id.ed25519_public_key, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, sig);
    char sig_hex[129];
    to_hex(sig, 64, sig_hex);

    cJSON* j = dispatch_set_endorsement("ffffffffffffffff", sig_hex);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(result_of(j), "ok")));
    cJSON_Delete(j);

    TEST_ASSERT_TRUE(identity_endorsement_is_set());
    uint64_t na = 0;
    uint8_t got[64];
    TEST_ASSERT_EQUAL(0, identity_endorsement_get(&na, got));
    TEST_ASSERT_EQUAL_HEX64(IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, na);
    TEST_ASSERT_EQUAL_MEMORY(sig, got, 64);
    TEST_ASSERT_TRUE(status_endorsed()); /* endorsed after */
}

/* No anchor provisioned: reject even a syntactically valid cert. */
void test_set_endorsement_rejects_when_no_anchor(void) {
    char sig_hex[129];
    memset(sig_hex, '0', 128);
    sig_hex[128] = '\0';
    cJSON* j = dispatch_set_endorsement("ffffffffffffffff", sig_hex);
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    TEST_ASSERT_FALSE(identity_endorsement_is_set());
    cJSON_Delete(j);
}

/* not_after == 0 is the "no cert" sentinel and is rejected as a provisioning
 * value even under a valid anchor. */
void test_set_endorsement_rejects_not_after_zero(void) {
    uint8_t anchor_priv[64];
    provision_anchor(anchor_priv);
    uint8_t sig[64];
    sign_cert(anchor_priv, s_id.ed25519_public_key, 0, sig);
    char sig_hex[129];
    to_hex(sig, 64, sig_hex);
    cJSON* j = dispatch_set_endorsement("0000000000000000", sig_hex);
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    TEST_ASSERT_FALSE(identity_endorsement_is_set());
    cJSON_Delete(j);
}

/* A signature that does not verify (flipped bit) is rejected. */
void test_set_endorsement_rejects_bad_sig(void) {
    uint8_t anchor_priv[64];
    provision_anchor(anchor_priv);
    uint8_t sig[64];
    sign_cert(anchor_priv, s_id.ed25519_public_key, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, sig);
    sig[10] ^= 0x01; /* corrupt */
    char sig_hex[129];
    to_hex(sig, 64, sig_hex);
    cJSON* j = dispatch_set_endorsement("ffffffffffffffff", sig_hex);
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    TEST_ASSERT_FALSE(identity_endorsement_is_set());
    cJSON_Delete(j);
}

/* A cert signed over a DIFFERENT node key does not verify against this node. */
void test_set_endorsement_rejects_wrong_node_key(void) {
    uint8_t anchor_priv[64];
    provision_anchor(anchor_priv);
    uint8_t other_pub[32];
    for (int i = 0; i < 32; i++)
        other_pub[i] = (uint8_t)(0x11 + i); /* not this node's key */
    uint8_t sig[64];
    sign_cert(anchor_priv, other_pub, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, sig);
    char sig_hex[129];
    to_hex(sig, 64, sig_hex);
    cJSON* j = dispatch_set_endorsement("ffffffffffffffff", sig_hex);
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    TEST_ASSERT_FALSE(identity_endorsement_is_set());
    cJSON_Delete(j);
}

/* Malformed hex in either field is rejected (wrong length / non-hex). */
void test_set_endorsement_rejects_malformed_hex(void) {
    uint8_t anchor_priv[64];
    provision_anchor(anchor_priv);
    char good_sig[129];
    memset(good_sig, '0', 128);
    good_sig[128] = '\0';

    /* not_after not 16 hex chars */
    cJSON* j = dispatch_set_endorsement("ff", good_sig);
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    cJSON_Delete(j);

    /* not_after has a non-hex nibble */
    j = dispatch_set_endorsement("fffffffffffffffg", good_sig);
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    cJSON_Delete(j);

    /* sig not 128 hex chars */
    j = dispatch_set_endorsement("ffffffffffffffff", "abcd");
    TEST_ASSERT_EQUAL(-32602, error_code(j));
    cJSON_Delete(j);

    TEST_ASSERT_FALSE(identity_endorsement_is_set());
}

/* getAnchorStatus.endorsed is present and false when no cert is held. */
void test_get_anchor_status_reports_endorsed_false_by_default(void) {
    cJSON* j = dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.getAnchorStatus\"}");
    cJSON* e = cJSON_GetObjectItem(result_of(j), "endorsed");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(cJSON_IsFalse(e));
    cJSON_Delete(j);
}

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
    RUN_TEST(test_set_endorsement_valid_persists_and_status_flips);
    RUN_TEST(test_set_endorsement_rejects_when_no_anchor);
    RUN_TEST(test_set_endorsement_rejects_not_after_zero);
    RUN_TEST(test_set_endorsement_rejects_bad_sig);
    RUN_TEST(test_set_endorsement_rejects_wrong_node_key);
    RUN_TEST(test_set_endorsement_rejects_malformed_hex);
    RUN_TEST(test_get_anchor_status_reports_endorsed_false_by_default);
    return UNITY_END();
}
