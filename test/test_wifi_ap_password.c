/*
 * Per-device SoftAP password derivation (issue #78).
 *
 * The properties under test are the ones the feature actually depends on:
 * a user can write the password down (stable), two devices are not each
 * other's key (distinct), and what comes out is a legal WPA2-PSK a human
 * can read off a small screen (length + alphabet).
 *
 * Fixtures here are synthetic byte patterns, never a real device's key, so
 * no password printed by this suite belongs to any real node.
 */

#include "unity.h"

#include "wifi_ap_password.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Deterministic synthetic "identity secret". Not a real key. */
static void fake_secret(uint8_t out[64], uint8_t seed) {
    for (int i = 0; i < 64; i++) {
        out[i] = (uint8_t)(seed * 31u + (unsigned)i * 7u + 11u);
    }
}

void test_derive_is_deterministic(void) {
    uint8_t secret[64];
    fake_secret(secret, 1);

    char a[WIFI_AP_PASSWORD_BUFSZ] = {0};
    char b[WIFI_AP_PASSWORD_BUFSZ] = {0};

    TEST_ASSERT_EQUAL_INT(0, wifi_ap_password_derive(secret, sizeof(secret), a, sizeof(a)));
    TEST_ASSERT_EQUAL_INT(0, wifi_ap_password_derive(secret, sizeof(secret), b, sizeof(b)));

    /* Same identity, same password: this is what makes it writable-down and
     * survivable across a reboot. */
    TEST_ASSERT_EQUAL_STRING(a, b);
}

void test_derive_differs_across_identities(void) {
    char prev[16][WIFI_AP_PASSWORD_BUFSZ];
    memset(prev, 0, sizeof(prev));

    for (int i = 0; i < 16; i++) {
        uint8_t secret[64];
        fake_secret(secret, (uint8_t)(i + 1));
        TEST_ASSERT_EQUAL_INT(
            0, wifi_ap_password_derive(secret, sizeof(secret), prev[i], sizeof(prev[i])));
        for (int j = 0; j < i; j++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(prev[i], prev[j]) != 0,
                                     "two distinct identities derived the same AP password");
        }
    }
}

void test_derive_is_sensitive_to_a_single_bit(void) {
    uint8_t a[64];
    uint8_t b[64];
    fake_secret(a, 9);
    memcpy(b, a, sizeof(a));
    b[63] ^= 0x01;

    char pa[WIFI_AP_PASSWORD_BUFSZ] = {0};
    char pb[WIFI_AP_PASSWORD_BUFSZ] = {0};
    TEST_ASSERT_EQUAL_INT(0, wifi_ap_password_derive(a, sizeof(a), pa, sizeof(pa)));
    TEST_ASSERT_EQUAL_INT(0, wifi_ap_password_derive(b, sizeof(b), pb, sizeof(pb)));
    TEST_ASSERT_TRUE(strcmp(pa, pb) != 0);
}

void test_derive_length_is_a_valid_wpa2_psk(void) {
    uint8_t secret[64];
    char pw[WIFI_AP_PASSWORD_BUFSZ] = {0};

    for (int i = 0; i < 64; i++) {
        fake_secret(secret, (uint8_t)i);
        TEST_ASSERT_EQUAL_INT(0, wifi_ap_password_derive(secret, sizeof(secret), pw, sizeof(pw)));

        size_t len = strlen(pw);
        TEST_ASSERT_EQUAL_UINT(WIFI_AP_PASSWORD_LEN, (unsigned)len);
        TEST_ASSERT_TRUE(len >= WIFI_AP_PASSWORD_MIN);
        TEST_ASSERT_TRUE(len <= WIFI_AP_PASSWORD_MAX);
    }
}

void test_derive_uses_only_the_unambiguous_alphabet(void) {
    uint8_t secret[64];
    char pw[WIFI_AP_PASSWORD_BUFSZ] = {0};

    for (int i = 0; i < 64; i++) {
        fake_secret(secret, (uint8_t)(i + 100));
        TEST_ASSERT_EQUAL_INT(0, wifi_ap_password_derive(secret, sizeof(secret), pw, sizeof(pw)));

        for (size_t c = 0; c < strlen(pw); c++) {
            TEST_ASSERT_NOT_NULL_MESSAGE(strchr(WIFI_AP_PASSWORD_ALPHABET, pw[c]),
                                         "derived password used an out-of-alphabet character");
            /* The glyphs deliberately excluded because they are misread on a
             * small mono font. Asserted explicitly so a future alphabet edit
             * that reintroduces them fails here. */
            TEST_ASSERT_TRUE(pw[c] != 'l' && pw[c] != 'i' && pw[c] != 'o');
            TEST_ASSERT_TRUE(pw[c] != '1');
            TEST_ASSERT_TRUE(!(pw[c] >= 'A' && pw[c] <= 'Z'));
        }
    }
}

void test_alphabet_is_exactly_32_unique_symbols(void) {
    const char* alpha = WIFI_AP_PASSWORD_ALPHABET;
    /* 32 symbols is what makes the 5-bit mapping unbiased. */
    TEST_ASSERT_EQUAL_UINT(32, (unsigned)strlen(alpha));
    for (size_t i = 0; alpha[i]; i++) {
        for (size_t j = i + 1; alpha[j]; j++) {
            TEST_ASSERT_TRUE_MESSAGE(alpha[i] != alpha[j], "duplicate symbol in the alphabet");
        }
    }
}

void test_derive_rejects_bad_arguments_without_leaking_a_partial(void) {
    uint8_t secret[64];
    fake_secret(secret, 3);
    char pw[WIFI_AP_PASSWORD_BUFSZ];

    memset(pw, 'x', sizeof(pw));
    TEST_ASSERT_EQUAL_INT(-1, wifi_ap_password_derive(NULL, 64, pw, sizeof(pw)));
    TEST_ASSERT_EQUAL_STRING("", pw);

    memset(pw, 'x', sizeof(pw));
    TEST_ASSERT_EQUAL_INT(-1, wifi_ap_password_derive(secret, 0, pw, sizeof(pw)));
    TEST_ASSERT_EQUAL_STRING("", pw);

    TEST_ASSERT_EQUAL_INT(-1, wifi_ap_password_derive(secret, sizeof(secret), NULL, 64));

    /* Too small an output buffer must fail rather than truncate to a short,
     * WPA2-illegal password. */
    char tiny[WIFI_AP_PASSWORD_BUFSZ - 1];
    TEST_ASSERT_EQUAL_INT(-1, wifi_ap_password_derive(secret, sizeof(secret), tiny, sizeof(tiny)));
}

void test_derive_accepts_short_secrets(void) {
    /* HKDF has no minimum IKM length; a caller passing a 32-byte key must
     * still get a full-length password. */
    uint8_t secret[64];
    fake_secret(secret, 5);
    char pw[WIFI_AP_PASSWORD_BUFSZ] = {0};
    TEST_ASSERT_EQUAL_INT(0, wifi_ap_password_derive(secret, 32, pw, sizeof(pw)));
    TEST_ASSERT_EQUAL_UINT(WIFI_AP_PASSWORD_LEN, (unsigned)strlen(pw));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_derive_is_deterministic);
    RUN_TEST(test_derive_differs_across_identities);
    RUN_TEST(test_derive_is_sensitive_to_a_single_bit);
    RUN_TEST(test_derive_length_is_a_valid_wpa2_psk);
    RUN_TEST(test_derive_uses_only_the_unambiguous_alphabet);
    RUN_TEST(test_alphabet_is_exactly_32_unique_symbols);
    RUN_TEST(test_derive_rejects_bad_arguments_without_leaking_a_partial);
    RUN_TEST(test_derive_accepts_short_secrets);
    return UNITY_END();
}
