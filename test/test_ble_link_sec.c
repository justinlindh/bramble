/*
 * BLE link security contract (issue #73).
 *
 * Two halves: the fail-closed payload predicate the GATT access callback and
 * the notify path both consult, and the sdkconfig contract that decides which
 * pairing algorithms the NimBLE build even contains. The second half matters
 * because the C code cannot detect a downgrade: if legacy pairing were
 * compiled back in, ble_link_payload_permitted would still return true on a
 * link an eavesdropper can decrypt.
 */
#include "unity.h"
#include "ble_link_sec.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SDKCONFIG_DEFAULTS_PATH
#error "SDKCONFIG_DEFAULTS_PATH must be defined"
#endif

static char* g_cfg;

void setUp(void) {
    FILE* f = fopen(SDKCONFIG_DEFAULTS_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open sdkconfig.defaults");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_cfg = malloc((size_t)n + 1);
    TEST_ASSERT_NOT_NULL(g_cfg);
    size_t got = fread(g_cfg, 1, (size_t)n, f);
    g_cfg[got] = '\0';
    fclose(f);
}

void tearDown(void) {
    free(g_cfg);
    g_cfg = NULL;
}

/* Matches an exact "KEY=VALUE" assignment at the start of a line, so a
 * mention inside a comment does not satisfy the assertion. */
static bool has_setting(const char* line) {
    size_t len = strlen(line);
    const char* p = g_cfg;
    while ((p = strstr(p, line)) != NULL) {
        bool at_line_start = (p == g_cfg) || (p[-1] == '\n');
        char after = p[len];
        if (at_line_start && (after == '\n' || after == '\r' || after == '\0')) {
            return true;
        }
        p += len;
    }
    return false;
}

void test_unencrypted_link_carries_no_payload(void) {
    /* The auth token is the first write on the TX characteristic. */
    TEST_ASSERT_FALSE(ble_link_payload_permitted(false));
}

void test_encrypted_link_carries_payload(void) {
    TEST_ASSERT_TRUE(ble_link_payload_permitted(true));
}

void test_secure_connections_enabled(void) {
    TEST_ASSERT_TRUE_MESSAGE(has_setting("CONFIG_BT_NIMBLE_SM_SC=y"),
                             "LE Secure Connections must be enabled");
}

void test_legacy_pairing_compiled_out(void) {
    /* Legacy Just Works derives its key from TK=0: a sniffed pairing is
     * decryptable. Allowing a downgrade would defeat the entire fix. */
    TEST_ASSERT_TRUE_MESSAGE(has_setting("CONFIG_BT_NIMBLE_SM_LEGACY=n"),
                             "legacy pairing must be compiled out");
}

void test_sc_debug_keys_disabled(void) {
    /* The SC debug key pair is published in the Bluetooth spec. */
    TEST_ASSERT_TRUE_MESSAGE(has_setting("CONFIG_BT_NIMBLE_SM_SC_DEBUG_KEYS=n"),
                             "Secure Connections debug keys must stay off");
}

void test_bonds_persist_across_reboot(void) {
    TEST_ASSERT_TRUE_MESSAGE(has_setting("CONFIG_BT_NIMBLE_NVS_PERSIST=y"),
                             "bonds must survive a reboot or every client re-pairs");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unencrypted_link_carries_no_payload);
    RUN_TEST(test_encrypted_link_carries_payload);
    RUN_TEST(test_secure_connections_enabled);
    RUN_TEST(test_legacy_pairing_compiled_out);
    RUN_TEST(test_sc_debug_keys_disabled);
    RUN_TEST(test_bonds_persist_across_reboot);
    return UNITY_END();
}
