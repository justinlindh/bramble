#include "unity.h"
#include "../main/ct_strcmp.h"

void setUp(void) {}
void tearDown(void) {}

void test_ct_strcmp_match(void) {
    TEST_ASSERT_EQUAL(0, ct_strcmp("abc", "abc"));
}

void test_ct_strcmp_mismatch(void) {
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("abc", "abd"));
}

void test_ct_strcmp_length_mismatch(void) {
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("abc", "ab"));
}

void test_ct_strcmp_both_empty(void) {
    TEST_ASSERT_EQUAL(0, ct_strcmp("", ""));
}

void test_ct_strcmp_one_empty_a(void) {
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("a", ""));
}

void test_ct_strcmp_one_empty_b(void) {
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("", "a"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ct_strcmp_match);
    RUN_TEST(test_ct_strcmp_mismatch);
    RUN_TEST(test_ct_strcmp_length_mismatch);
    RUN_TEST(test_ct_strcmp_both_empty);
    RUN_TEST(test_ct_strcmp_one_empty_a);
    RUN_TEST(test_ct_strcmp_one_empty_b);
    return UNITY_END();
}
