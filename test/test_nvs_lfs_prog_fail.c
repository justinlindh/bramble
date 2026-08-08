/**
 * The flash-backed NVS shim (nrf/shim/nvs_lfs.c) over a block device whose
 * program path fails, which is what nrf/src/lfs_nvmc_prog() reports
 * (LFS_ERR_IO) when its bounded NVMC write-completion wait expires. Before
 * that bound existed the wait could not expire, so this whole failure mode
 * was a permanent hang inside the shim's global lock rather than a return
 * value, and nothing downstream had ever been exercised against it.
 *
 * What matters here is that the failure is REPORTED. The chain that depends
 * on it is nonce_counter's durability callback: mesh_nonce_write()
 * (main/mesh_persist.c) tells nonce_counter whether a reserve-ceiling write
 * landed, and nonce_counter refuses to issue a nonce when it did not
 * (components/nonce_counter/nonce_counter.c, covered by test_nonce_counter).
 * On the nRF backend nvs_commit() is a no-op confirmation because every set
 * fsyncs its own file, so nvs_set_*()'s own result is the only signal a
 * failed flash program produces. If it could come back ESP_OK, issuance would
 * run past the ceiling storage holds and a reboot would reuse nonces under
 * the same key.
 */
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "unity.h"

/* test/stubs/lfs_nvmc_rambd.c: make the RAM disk's prog return LFS_ERR_IO. */
void lfs_nvmc_rambd_fail_prog(int fail);

#define NS "nonce"
#define KEY "ceiling"

void setUp(void) {
    lfs_nvmc_rambd_fail_prog(0);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_erase());
}

void tearDown(void) { lfs_nvmc_rambd_fail_prog(0); }

static nvs_handle_t open_rw(void) {
    nvs_handle_t h = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(NS, NVS_READWRITE, &h));
    return h;
}

/* The headline: a set whose program does not land must not report success. */
void test_set_blob_reports_failure_when_prog_fails(void) {
    nvs_handle_t h = open_rw();
    uint64_t ceiling = 4096;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, KEY, &ceiling, sizeof(ceiling)));

    lfs_nvmc_rambd_fail_prog(1);
    uint64_t next = 8192;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_set_blob(h, KEY, &next, sizeof(next)));
    nvs_close(h);
}

/* Scalars and strings go through the same set_value() body, so they must
 * report the same way; the settings and identity namespaces use them. */
void test_scalar_and_str_report_failure_when_prog_fails(void) {
    nvs_handle_t h = open_rw();
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(h, "u32", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(h, "str", "before"));

    lfs_nvmc_rambd_fail_prog(1);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_set_u32(h, "u32", 2));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_set_str(h, "str", "after"));
    nvs_close(h);
}

/* A failed write must not be readable as if it had landed: the last durable
 * value survives instead. That is the property the nonce ceiling depends on
 * across a reboot, where the stored ceiling is what issuance resumes above. */
void test_failed_write_leaves_previous_value(void) {
    nvs_handle_t h = open_rw();
    uint64_t ceiling = 4096;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, KEY, &ceiling, sizeof(ceiling)));

    lfs_nvmc_rambd_fail_prog(1);
    uint64_t next = 8192;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_set_blob(h, KEY, &next, sizeof(next)));
    lfs_nvmc_rambd_fail_prog(0);

    uint64_t read_back = 0;
    size_t len = sizeof(read_back);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(h, KEY, &read_back, &len));
    TEST_ASSERT_EQUAL_UINT64(ceiling, read_back);
    nvs_close(h);
}

/* Creating a namespace is an lfs_mkdir, which is also a program. It has to
 * fail closed rather than hand back a handle that writes into nothing. */
void test_namespace_create_reports_failure_when_prog_fails(void) {
    lfs_nvmc_rambd_fail_prog(1);
    nvs_handle_t h = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_open("freshns", NVS_READWRITE, &h));
}

/* The failure is per-operation, not a latch: a controller that completes the
 * next write must be usable again without a remount. */
void test_writes_succeed_again_after_prog_recovers(void) {
    nvs_handle_t h = open_rw();
    lfs_nvmc_rambd_fail_prog(1);
    uint64_t next = 8192;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_set_blob(h, KEY, &next, sizeof(next)));

    lfs_nvmc_rambd_fail_prog(0);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, KEY, &next, sizeof(next)));

    uint64_t read_back = 0;
    size_t len = sizeof(read_back);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(h, KEY, &read_back, &len));
    TEST_ASSERT_EQUAL_UINT64(next, read_back);
    nvs_close(h);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_set_blob_reports_failure_when_prog_fails);
    RUN_TEST(test_scalar_and_str_report_failure_when_prog_fails);
    RUN_TEST(test_failed_write_leaves_previous_value);
    RUN_TEST(test_namespace_create_reports_failure_when_prog_fails);
    RUN_TEST(test_writes_succeed_again_after_prog_recovers);
    return UNITY_END();
}
