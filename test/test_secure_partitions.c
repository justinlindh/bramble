#include "unity.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef SECURE_PARTITIONS_CSV
#error "SECURE_PARTITIONS_CSV path must be defined"
#endif

static char* g_buf;

void setUp(void) {
    FILE* f = fopen(SECURE_PARTITIONS_CSV, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open partitions.secure.csv");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_buf = malloc((size_t)n + 1);
    TEST_ASSERT_NOT_NULL(g_buf);
    size_t got = fread(g_buf, 1, (size_t)n, f);
    g_buf[got] = '\0';
    fclose(f);
}
void tearDown(void) { free(g_buf); g_buf = NULL; }

void test_has_encrypted_nvs_keys_partition(void) {
    /* A line naming subtype nvs_keys carrying the encrypted flag. */
    const char* line = strstr(g_buf, "nvs_keys");
    TEST_ASSERT_NOT_NULL_MESSAGE(line, "no nvs_keys partition");
    const char* eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    char row[256] = {0};
    memcpy(row, line, len < sizeof(row) - 1 ? len : sizeof(row) - 1);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(row, "encrypted"),
                                 "nvs_keys partition is not flagged encrypted");
}

void test_still_has_dual_ota_and_spiffs(void) {
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "ota_0"));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "ota_1"));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "spiffs"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_has_encrypted_nvs_keys_partition);
    RUN_TEST(test_still_has_dual_ota_and_spiffs);
    return UNITY_END();
}
