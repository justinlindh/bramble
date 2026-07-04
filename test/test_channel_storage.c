/**
 * Unit tests for channel persistence (Phase 1)
 */

#include "unity.h"
#include <string.h>

/* Include implementation files for host testing */
#include "../components/channel/channel_storage.c"

void setUp(void) {}
void tearDown(void) {}

void test_init_succeeds(void) { TEST_ASSERT_EQUAL(0, channel_storage_init()); }

void test_save_and_load_empty(void) {
    bramble_channel_t channels[MAX_CHANNELS];
    bramble_channel_t loaded[MAX_CHANNELS];
    char names[MAX_CHANNELS][20];
    char loaded_names[MAX_CHANNELS][20];
    int loaded_count = -1;
    int loaded_default = -1;

    memset(names, 0, sizeof(names));
    memset(loaded_names, 0, sizeof(loaded_names));

    /* Clear first */
    channel_storage_clear();

    /* Save empty */
    TEST_ASSERT_EQUAL(0, channel_storage_save(channels, 0, names, 0));

    /* Load */
    int ret = channel_storage_load(loaded, &loaded_count, loaded_names, &loaded_default);
    TEST_ASSERT_EQUAL(-1, ret); /* No data */
    TEST_ASSERT_EQUAL(0, loaded_count);
}

void test_save_and_load_single_channel(void) {
    bramble_channel_t channels[MAX_CHANNELS];
    bramble_channel_t loaded[MAX_CHANNELS];
    char names[MAX_CHANNELS][20];
    char loaded_names[MAX_CHANNELS][20];
    int loaded_count = 0;
    int loaded_default = -1;

    memset(channels, 0, sizeof(channels));
    memset(names, 0, sizeof(names));
    memset(loaded_names, 0, sizeof(loaded_names));

    /* Create test channel */
    channels[0].channel_id = 0;
    channels[0].epoch = 100;
    memset(channels[0].key, 0xAA, sizeof(channels[0].key));
    strncpy(names[0], "TestChan", sizeof(names[0]) - 1);

    /* Save */
    channel_storage_clear();
    TEST_ASSERT_EQUAL(0, channel_storage_save(channels, 1, names, 0));

    /* Load */
    TEST_ASSERT_EQUAL(0,
                      channel_storage_load(loaded, &loaded_count, loaded_names, &loaded_default));
    TEST_ASSERT_EQUAL(1, loaded_count);
    TEST_ASSERT_EQUAL(0, loaded_default);

    /* Verify channel */
    TEST_ASSERT_EQUAL(0, loaded[0].channel_id);
    TEST_ASSERT_EQUAL(100, loaded[0].epoch);
    TEST_ASSERT_EQUAL_MEMORY(channels[0].key, loaded[0].key, sizeof(channels[0].key));
    TEST_ASSERT_EQUAL_STRING("TestChan", loaded_names[0]);
}

void test_save_and_load_multiple_channels(void) {
    bramble_channel_t channels[MAX_CHANNELS];
    bramble_channel_t loaded[MAX_CHANNELS];
    char names[MAX_CHANNELS][20];
    char loaded_names[MAX_CHANNELS][20];
    int loaded_count = 0;
    int loaded_default = -1;

    memset(channels, 0, sizeof(channels));
    memset(names, 0, sizeof(names));
    memset(loaded_names, 0, sizeof(loaded_names));

    /* Create test channels */
    for (int i = 0; i < 5; i++) {
        channels[i].channel_id = i;
        channels[i].epoch = 100 + i;
        memset(channels[i].key, i + 1, sizeof(channels[i].key));
        snprintf(names[i], sizeof(names[i]), "Channel%d", i);
    }

    /* Save */
    channel_storage_clear();
    TEST_ASSERT_EQUAL(0, channel_storage_save(channels, 5, names, 2));

    /* Load */
    TEST_ASSERT_EQUAL(0,
                      channel_storage_load(loaded, &loaded_count, loaded_names, &loaded_default));
    TEST_ASSERT_EQUAL(5, loaded_count);
    TEST_ASSERT_EQUAL(2, loaded_default);

    /* Verify channels */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(i, loaded[i].channel_id);
        TEST_ASSERT_EQUAL(100 + i, loaded[i].epoch);

        /* Verify key (all bytes should be i+1) */
        for (size_t j = 0; j < sizeof(loaded[i].key); j++) {
            TEST_ASSERT_EQUAL_UINT8(i + 1, loaded[i].key[j]);
        }

        char expected_name[20];
        snprintf(expected_name, sizeof(expected_name), "Channel%d", i);
        TEST_ASSERT_EQUAL_STRING(expected_name, loaded_names[i]);
    }
}

void test_max_channels(void) {
    bramble_channel_t channels[MAX_CHANNELS];
    bramble_channel_t loaded[MAX_CHANNELS];
    char names[MAX_CHANNELS][20];
    char loaded_names[MAX_CHANNELS][20];
    int loaded_count = 0;
    int loaded_default = -1;

    memset(channels, 0, sizeof(channels));
    memset(names, 0, sizeof(names));
    memset(loaded_names, 0, sizeof(loaded_names));

    /* Create max channels */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        channels[i].channel_id = i;
        channels[i].epoch = i;
        memset(channels[i].key, i, sizeof(channels[i].key));
        snprintf(names[i], sizeof(names[i]), "Ch%d", i);
    }

    /* Save */
    channel_storage_clear();
    TEST_ASSERT_EQUAL(0, channel_storage_save(channels, MAX_CHANNELS, names, 0));

    /* Load */
    TEST_ASSERT_EQUAL(0,
                      channel_storage_load(loaded, &loaded_count, loaded_names, &loaded_default));
    TEST_ASSERT_EQUAL(MAX_CHANNELS, loaded_count);

    /* Verify all */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        TEST_ASSERT_EQUAL(i, loaded[i].channel_id);
    }
}

void test_clear_works(void) {
    bramble_channel_t channels[1];
    bramble_channel_t loaded[MAX_CHANNELS];
    char names[1][20];
    char loaded_names[MAX_CHANNELS][20];
    int loaded_count = 0;
    int loaded_default = -1;

    memset(channels, 0, sizeof(channels));
    memset(names, 0, sizeof(names));
    strncpy(names[0], "Test", sizeof(names[0]) - 1);

    /* Save one channel */
    channel_storage_clear();
    channel_storage_save(channels, 1, names, 0);

    /* Clear */
    channel_storage_clear();

    /* Load should fail */
    int ret = channel_storage_load(loaded, &loaded_count, loaded_names, &loaded_default);
    TEST_ASSERT_EQUAL(-1, ret);
    TEST_ASSERT_EQUAL(0, loaded_count);
}

void test_default_channel_index_persists(void) {
    bramble_channel_t channels[3];
    bramble_channel_t loaded[MAX_CHANNELS];
    char names[3][20];
    char loaded_names[MAX_CHANNELS][20];
    int loaded_count = 0;
    int loaded_default = -1;

    memset(channels, 0, sizeof(channels));
    memset(names, 0, sizeof(names));

    for (int i = 0; i < 3; i++) {
        channels[i].channel_id = i;
        channels[i].epoch = 0;
    }

    /* Save with default=2 */
    channel_storage_clear();
    TEST_ASSERT_EQUAL(0, channel_storage_save(channels, 3, names, 2));

    /* Load and verify default */
    TEST_ASSERT_EQUAL(0,
                      channel_storage_load(loaded, &loaded_count, loaded_names, &loaded_default));
    TEST_ASSERT_EQUAL(3, loaded_count);
    TEST_ASSERT_EQUAL(2, loaded_default);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_succeeds);
    RUN_TEST(test_save_and_load_empty);
    RUN_TEST(test_save_and_load_single_channel);
    RUN_TEST(test_save_and_load_multiple_channels);
    RUN_TEST(test_max_channels);
    RUN_TEST(test_clear_works);
    RUN_TEST(test_default_channel_index_persists);
    return UNITY_END();
}
