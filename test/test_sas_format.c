/*
 * SAS grouping helper (pager SAS verification UX, Task 8). The pure logic
 * behind the read-aloud grouping is the host-testable slice of this task; the
 * LVGL screen that renders it (scr_sas_verify.c) is not host-linkable and is
 * gated by the board build and the Task 10 emulator E2E instead.
 */
#include "unity.h"
#include "sas_format.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_groups_seven_digits_as_three_plus_four(void) {
    char out[9];
    sas_format_grouped("1234567", out);
    TEST_ASSERT_EQUAL_STRING("123 4567", out);
}

void test_groups_leading_and_trailing_zeros(void) {
    char out[9];
    sas_format_grouped("0000000", out);
    TEST_ASSERT_EQUAL_STRING("000 0000", out);
}

void test_null_input_yields_empty_string(void) {
    char out[9];
    sas_format_grouped(NULL, out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_short_input_yields_empty_string(void) {
    /* Zero-padded to 8 bytes: sas7 is documented as a fixed 8-byte array, and
     * a bare short literal trips -Wstringop-overread on the declared bound. */
    char short_sas[8] = "123";
    char out[9];
    sas_format_grouped(short_sas, out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_non_digit_input_yields_empty_string(void) {
    char out[9];
    sas_format_grouped("12a4567", out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_null_out_does_not_crash(void) { sas_format_grouped("1234567", NULL); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_groups_seven_digits_as_three_plus_four);
    RUN_TEST(test_groups_leading_and_trailing_zeros);
    RUN_TEST(test_null_input_yields_empty_string);
    RUN_TEST(test_short_input_yields_empty_string);
    RUN_TEST(test_non_digit_input_yields_empty_string);
    RUN_TEST(test_null_out_does_not_crash);
    return UNITY_END();
}
