/**
 * Host tests for the nRF target's RAM-backed NVS shim (nrf/shim/nvs_ram.c).
 *
 * The shim must hold the real blob sizes the firmware persists: the largest
 * today is the serialized identity pin store (IDENTITY_STORE_BLOB_MAX = 2466
 * bytes), so the pool-capacity tests exercise that size, not a toy bound.
 */
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "unity.h"

#define PIN_STORE_BLOB_SIZE 2466

void setUp(void) { TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_erase()); }

void tearDown(void) {}

static nvs_handle_t open_rw(const char* ns) {
    nvs_handle_t h = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(ns, NVS_READWRITE, &h));
    return h;
}

static void test_get_before_set_returns_not_found(void) {
    nvs_handle_t h = open_rw("bramble");
    uint32_t v = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_get_u32(h, "absent", &v));
    nvs_close(h);
}

static void test_scalar_roundtrips(void) {
    nvs_handle_t h = open_rw("bramble");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(h, "k8", 0xAB));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u16(h, "k16", 0xBEEF));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(h, "k32", 0xDEADBEEF));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_i32(h, "ki32", -1234567));
    uint8_t v8;
    uint16_t v16;
    uint32_t v32;
    int32_t vi32;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(h, "k8", &v8));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u16(h, "k16", &v16));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(h, "k32", &v32));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_i32(h, "ki32", &vi32));
    TEST_ASSERT_EQUAL_HEX8(0xAB, v8);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, v16);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, v32);
    TEST_ASSERT_EQUAL_INT32(-1234567, vi32);
    nvs_close(h);
}

static void test_blob_roundtrip_small(void) {
    nvs_handle_t h = open_rw("bramble_id");
    uint8_t in[64];
    for (size_t i = 0; i < sizeof(in); i++) {
        in[i] = (uint8_t)(i * 7);
    }
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "anchor_pub", in, sizeof(in)));
    uint8_t out[64];
    size_t len = sizeof(out);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(h, "anchor_pub", out, &len));
    TEST_ASSERT_EQUAL_size_t(sizeof(in), len);
    TEST_ASSERT_EQUAL_MEMORY(in, out, sizeof(in));
    nvs_close(h);
}

static void test_blob_roundtrip_pin_store_size(void) {
    nvs_handle_t h = open_rw("bramble_id");
    static uint8_t in[PIN_STORE_BLOB_SIZE];
    for (size_t i = 0; i < sizeof(in); i++) {
        in[i] = (uint8_t)(i ^ (i >> 8));
    }
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "pin_store", in, sizeof(in)));
    static uint8_t out[PIN_STORE_BLOB_SIZE];
    size_t len = sizeof(out);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(h, "pin_store", out, &len));
    TEST_ASSERT_EQUAL_size_t(sizeof(in), len);
    TEST_ASSERT_EQUAL_MEMORY(in, out, sizeof(in));
    nvs_close(h);
}

static void test_blob_null_buffer_queries_length(void) {
    nvs_handle_t h = open_rw("bramble_rp");
    uint8_t in[100] = {1, 2, 3};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "data_win", in, sizeof(in)));
    size_t len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(h, "data_win", NULL, &len));
    TEST_ASSERT_EQUAL_size_t(100, len);
    nvs_close(h);
}

static void test_blob_buffer_too_small(void) {
    nvs_handle_t h = open_rw("bramble_rp");
    uint8_t in[100] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "data_win", in, sizeof(in)));
    uint8_t out[10];
    size_t len = sizeof(out);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_INVALID_LENGTH, nvs_get_blob(h, "data_win", out, &len));
    nvs_close(h);
}

static void test_str_roundtrip_and_length_semantics(void) {
    nvs_handle_t h = open_rw("bramble");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(h, "node_name", "wm1110-bench"));
    size_t len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_str(h, "node_name", NULL, &len));
    TEST_ASSERT_EQUAL_size_t(strlen("wm1110-bench") + 1, len);
    char out[32];
    len = sizeof(out);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_str(h, "node_name", out, &len));
    TEST_ASSERT_EQUAL_STRING("wm1110-bench", out);
    nvs_close(h);
}

static void test_namespace_isolation(void) {
    nvs_handle_t ha = open_rw("bramble");
    nvs_handle_t hb = open_rw("bramble_radio");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(ha, "same_key", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(hb, "same_key", 2));
    uint8_t va, vb;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(ha, "same_key", &va));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(hb, "same_key", &vb));
    TEST_ASSERT_EQUAL_HEX8(1, va);
    TEST_ASSERT_EQUAL_HEX8(2, vb);
    nvs_close(ha);
    nvs_close(hb);
}

static void test_erase_key(void) {
    nvs_handle_t h = open_rw("bramble");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(h, "gps_en", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_erase_key(h, "gps_en"));
    uint8_t v;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_get_u8(h, "gps_en", &v));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_erase_key(h, "gps_en"));
    nvs_close(h);
}

static void test_overwrite_with_larger_value(void) {
    nvs_handle_t h = open_rw("bramble_ch");
    uint8_t small[16] = {1};
    uint8_t big[512] = {2};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "ch0", small, sizeof(small)));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "ch0", big, sizeof(big)));
    uint8_t out[512];
    size_t len = sizeof(out);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(h, "ch0", out, &len));
    TEST_ASSERT_EQUAL_size_t(sizeof(big), len);
    TEST_ASSERT_EQUAL_MEMORY(big, out, sizeof(big));
    nvs_close(h);
}

static void test_type_mismatch(void) {
    nvs_handle_t h = open_rw("bramble");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(h, "conn_mode", 3));
    char out[8];
    size_t len = sizeof(out);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_TYPE_MISMATCH, nvs_get_str(h, "conn_mode", out, &len));
    nvs_close(h);
}

static void test_pool_exhaustion_returns_not_enough_space(void) {
    nvs_handle_t h = open_rw("bramble_id");
    static uint8_t blob[PIN_STORE_BLOB_SIZE];
    memset(blob, 0x5A, sizeof(blob));
    char key[NVS_RAM_NAME_MAX];
    esp_err_t err = ESP_OK;
    int wrote = 0;
    for (int i = 0; i < 8; i++) {
        snprintf(key, sizeof(key), "big%d", i);
        err = nvs_set_blob(h, key, blob, sizeof(blob));
        if (err != ESP_OK) {
            break;
        }
        wrote++;
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_ENOUGH_SPACE, err);
    // The pool must hold at least three pin-store-sized blobs (pin store +
    // both replay windows head-room).
    TEST_ASSERT_GREATER_OR_EQUAL_INT(3, wrote);
    nvs_close(h);
}

static void test_iterator_enumerates_namespace(void) {
    nvs_handle_t h = open_rw("bramble_ch");
    uint8_t b[4] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "ch0", b, sizeof(b)));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "ch1", b, sizeof(b)));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(h, "count", 2));
    // A different namespace must not appear.
    nvs_handle_t other = open_rw("bramble");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(other, "noise", 1));

    nvs_iterator_t it = NULL;
    int blobs = 0, scalars = 0;
    esp_err_t err = nvs_entry_find("nvs", "bramble_ch", NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_info(it, &info));
        TEST_ASSERT_EQUAL_STRING("bramble_ch", info.namespace_name);
        if (info.type == NVS_TYPE_BLOB) {
            blobs++;
        } else {
            scalars++;
        }
        err = nvs_entry_next(&it);
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, err);
    TEST_ASSERT_EQUAL_INT(2, blobs);
    TEST_ASSERT_EQUAL_INT(1, scalars);
    nvs_release_iterator(it);
    nvs_close(h);
    nvs_close(other);
}

static void test_flash_erase_resets_everything(void) {
    nvs_handle_t h = open_rw("bramble");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(h, "gps_en", 1));
    nvs_close(h);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_erase());
    h = open_rw("bramble");
    uint8_t v;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_get_u8(h, "gps_en", &v));
    nvs_close(h);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_get_before_set_returns_not_found);
    RUN_TEST(test_scalar_roundtrips);
    RUN_TEST(test_blob_roundtrip_small);
    RUN_TEST(test_blob_roundtrip_pin_store_size);
    RUN_TEST(test_blob_null_buffer_queries_length);
    RUN_TEST(test_blob_buffer_too_small);
    RUN_TEST(test_str_roundtrip_and_length_semantics);
    RUN_TEST(test_namespace_isolation);
    RUN_TEST(test_erase_key);
    RUN_TEST(test_overwrite_with_larger_value);
    RUN_TEST(test_type_mismatch);
    RUN_TEST(test_pool_exhaustion_returns_not_enough_space);
    RUN_TEST(test_iterator_enumerates_namespace);
    RUN_TEST(test_flash_erase_resets_everything);
    return UNITY_END();
}
