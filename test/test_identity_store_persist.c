/*
 * Persistence core for the verified TOFU pin store (DM forward-secrecy + SAS,
 * Task 6). These tests are NVS-FREE on purpose: they exercise the pure
 * serialize/deserialize round-trip that carries the pin table, the verified
 * bit, and the SAS-at-verification across a reboot. The actual nvs_* calls
 * live in mesh_task.c and are exercised by the board build + emulator.
 */
#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "packet.h"
#include "identity_store.h"

#include "../components/crypto/crypto_host.c"
#include "../components/packet/packet.c"
#include "../components/identity/identity_store.c"
#include "identity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void mkkeys(uint8_t ed[32], uint8_t x[32], uint8_t seed) {
    for (int i = 0; i < 32; i++) {
        ed[i] = (uint8_t)(seed + i);
        x[i] = (uint8_t)(seed * 2 + i);
    }
}

/* A verified bit + SAS survives a serialize/deserialize cycle (the NVS-free
 * core of persistence). */
void test_verified_bit_survives_roundtrip(void) {
    identity_store_t s;
    identity_store_init(&s, 1000);
    uint8_t ed[32], x[32];
    mkkeys(ed, x, 0x10);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s, 0xAABBCCDD, ed, x, 1000));
    TEST_ASSERT_TRUE(identity_store_set_verified(&s, 0xAABBCCDD, "1234567"));
    TEST_ASSERT_TRUE(identity_store_is_verified(&s, 0xAABBCCDD));

    uint8_t buf[IDENTITY_STORE_BLOB_MAX];
    int n = identity_store_serialize(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    identity_store_t s2 = {0};
    TEST_ASSERT_EQUAL(0, identity_store_deserialize(&s2, buf, (size_t)n, 2000));
    TEST_ASSERT_TRUE(identity_store_is_verified(&s2, 0xAABBCCDD));
    const identity_pin_t* e = identity_store_lookup(&s2, 0xAABBCCDD);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_MEMORY(ed, e->ed25519_pub, 32);
    TEST_ASSERT_EQUAL_MEMORY(x, e->x25519_pub, 32);
    TEST_ASSERT_EQUAL_STRING("1234567", e->verified_sas);
}

/* set_verified on an unknown address returns false; clear_verified toggles off. */
void test_set_and_clear_verified(void) {
    identity_store_t s;
    identity_store_init(&s, 0);
    TEST_ASSERT_FALSE(identity_store_set_verified(&s, 0xDEAD, "0000000"));
    uint8_t ed[32], x[32];
    mkkeys(ed, x, 0x20);
    identity_store_pin(&s, 0xBEEF, ed, x, 0);
    TEST_ASSERT_TRUE(identity_store_set_verified(&s, 0xBEEF, "7654321"));
    TEST_ASSERT_TRUE(identity_store_is_verified(&s, 0xBEEF));
    TEST_ASSERT_TRUE(identity_store_clear_verified(&s, 0xBEEF));
    TEST_ASSERT_FALSE(identity_store_is_verified(&s, 0xBEEF));
}

/* Several entries, mixed verified/unverified, all survive with keys, verified
 * bit, and SAS intact and looked up by address (not by slot order). */
void test_multi_entry_roundtrip(void) {
    identity_store_t s;
    identity_store_init(&s, 500);
    uint8_t ed[32], x[32];
    for (int i = 0; i < 5; i++) {
        mkkeys(ed, x, (uint8_t)(0x30 + i * 0x11));
        uint32_t addr = 0x1000u + (uint32_t)i;
        TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s, addr, ed, x, 500));
        if (i % 2 == 0) {
            char sas[8];
            snprintf(sas, sizeof(sas), "%07d", 1000000 + i);
            TEST_ASSERT_TRUE(identity_store_set_verified(&s, addr, sas));
        }
    }

    uint8_t buf[IDENTITY_STORE_BLOB_MAX];
    int n = identity_store_serialize(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    identity_store_t s2 = {0};
    TEST_ASSERT_EQUAL(0, identity_store_deserialize(&s2, buf, (size_t)n, 9999));
    TEST_ASSERT_EQUAL(5, identity_store_count(&s2));
    for (int i = 0; i < 5; i++) {
        uint32_t addr = 0x1000u + (uint32_t)i;
        mkkeys(ed, x, (uint8_t)(0x30 + i * 0x11));
        const identity_pin_t* e = identity_store_lookup(&s2, addr);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_MEMORY(ed, e->ed25519_pub, 32);
        TEST_ASSERT_EQUAL_MEMORY(x, e->x25519_pub, 32);
        if (i % 2 == 0) {
            char sas[8];
            snprintf(sas, sizeof(sas), "%07d", 1000000 + i);
            TEST_ASSERT_TRUE(identity_store_is_verified(&s2, addr));
            TEST_ASSERT_EQUAL_STRING(sas, e->verified_sas);
        } else {
            TEST_ASSERT_FALSE(identity_store_is_verified(&s2, addr));
        }
    }
}

/* A fresh device (no persisted blob) serializes to a header-only blob and
 * deserializes back to an empty store, cleanly. */
void test_empty_store_roundtrip(void) {
    identity_store_t s;
    identity_store_init(&s, 42);
    uint8_t buf[IDENTITY_STORE_BLOB_MAX];
    int n = identity_store_serialize(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n); /* still writes the version + count header */

    identity_store_t s2 = {0};
    TEST_ASSERT_EQUAL(0, identity_store_deserialize(&s2, buf, (size_t)n, 7));
    TEST_ASSERT_EQUAL(0, identity_store_count(&s2));
    TEST_ASSERT_NULL(identity_store_lookup(&s2, 0x1234));
}

/* A wrong format-version byte is rejected (clean flag day, no garbage pins). */
void test_bad_version_rejected(void) {
    identity_store_t s;
    identity_store_init(&s, 0);
    uint8_t ed[32], x[32];
    mkkeys(ed, x, 0x55);
    identity_store_pin(&s, 0x9999, ed, x, 0);
    uint8_t buf[IDENTITY_STORE_BLOB_MAX];
    int n = identity_store_serialize(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    buf[0] = 0xFE; /* corrupt the version byte */

    identity_store_t s2 = {0};
    TEST_ASSERT_EQUAL(-1, identity_store_deserialize(&s2, buf, (size_t)n, 0));
    /* Even on rejection the store is left initialized and empty, never garbage. */
    TEST_ASSERT_EQUAL(0, identity_store_count(&s2));
}

/* A truncated buffer is rejected without over-reading. */
void test_truncated_buffer_rejected(void) {
    identity_store_t s;
    identity_store_init(&s, 0);
    uint8_t ed[32], x[32];
    mkkeys(ed, x, 0x66);
    identity_store_pin(&s, 0x4242, ed, x, 0);
    uint8_t buf[IDENTITY_STORE_BLOB_MAX];
    int n = identity_store_serialize(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    identity_store_t s2 = {0};
    /* Chop the last byte off a one-record blob: the record no longer fits. */
    TEST_ASSERT_EQUAL(-1, identity_store_deserialize(&s2, buf, (size_t)n - 1, 0));
}

/* identity_store_mark_key_changed sets the flag on a pinned addr and
 * identity_store_key_changed reflects it; an unknown addr is refused. */
void test_mark_key_changed(void) {
    identity_store_t s;
    identity_store_init(&s, 0);
    TEST_ASSERT_FALSE(identity_store_mark_key_changed(&s, 0xDEAD));

    uint8_t ed[32], x[32];
    mkkeys(ed, x, 0x70);
    identity_store_pin(&s, 0xC0FFEE, ed, x, 0);
    TEST_ASSERT_FALSE(identity_store_key_changed(&s, 0xC0FFEE));
    TEST_ASSERT_TRUE(identity_store_mark_key_changed(&s, 0xC0FFEE));
    TEST_ASSERT_TRUE(identity_store_key_changed(&s, 0xC0FFEE));
}

/* Re-verifying (identity_store_set_verified) dismisses a pending key-change
 * warning: mark, then verify, then the flag reads clear. */
void test_set_verified_clears_key_changed(void) {
    identity_store_t s;
    identity_store_init(&s, 0);
    uint8_t ed[32], x[32];
    mkkeys(ed, x, 0x71);
    identity_store_pin(&s, 0xFACE, ed, x, 0);
    TEST_ASSERT_TRUE(identity_store_mark_key_changed(&s, 0xFACE));
    TEST_ASSERT_TRUE(identity_store_key_changed(&s, 0xFACE));

    TEST_ASSERT_TRUE(identity_store_set_verified(&s, 0xFACE, "1112223"));
    TEST_ASSERT_FALSE(identity_store_key_changed(&s, 0xFACE));
}

/* key_changed is RAM-only: it must NOT survive a serialize/deserialize
 * round-trip, while the persisted verified bit still does. This locks the
 * contract that the NVS blob layout is unchanged by this flag. */
void test_key_changed_not_persisted(void) {
    identity_store_t s;
    identity_store_init(&s, 1000);
    uint8_t ed[32], x[32];
    mkkeys(ed, x, 0x72);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s, 0x13579, ed, x, 1000));
    TEST_ASSERT_TRUE(identity_store_set_verified(&s, 0x13579, "9998887"));
    TEST_ASSERT_TRUE(identity_store_mark_key_changed(&s, 0x13579));
    TEST_ASSERT_TRUE(identity_store_key_changed(&s, 0x13579));

    uint8_t buf[IDENTITY_STORE_BLOB_MAX];
    int n = identity_store_serialize(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    identity_store_t s2 = {0};
    TEST_ASSERT_EQUAL(0, identity_store_deserialize(&s2, buf, (size_t)n, 2000));
    TEST_ASSERT_TRUE(identity_store_is_verified(&s2, 0x13579));
    TEST_ASSERT_FALSE(identity_store_key_changed(&s2, 0x13579));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_verified_bit_survives_roundtrip);
    RUN_TEST(test_set_and_clear_verified);
    RUN_TEST(test_multi_entry_roundtrip);
    RUN_TEST(test_empty_store_roundtrip);
    RUN_TEST(test_bad_version_rejected);
    RUN_TEST(test_truncated_buffer_rejected);
    RUN_TEST(test_mark_key_changed);
    RUN_TEST(test_set_verified_clears_key_changed);
    RUN_TEST(test_key_changed_not_persisted);
    return UNITY_END();
}
