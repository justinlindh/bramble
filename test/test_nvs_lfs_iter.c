/**
 * Iterator regression tests for the flash-backed NVS shim
 * (nrf/shim/nvs_lfs.c) over littlefs's RAM block device.
 *
 * These exist because the iterator family took no lock at all and walked a
 * single global iterator plus a shared file buffer. Two 1Hz callers (the mesh
 * task's location-policy tick and, on the GNSS branch, the gnss task's
 * duty-cycle tick) could therefore both be inside lfs_dir_open() on the same
 * lfs_dir_t; littlefs threads open dirs onto lfs->mlist, and the concurrent
 * open self-cycles that list, after which the next write walks it forever
 * while holding the shim lock and every NVS consumer wedges behind it.
 *
 * The host suite is single-threaded, so it cannot reproduce that interleaving
 * directly. What it CAN pin down is the enforced invariant that makes the
 * interleaving impossible on the device, and the lock-lifetime bookkeeping
 * that goes with it:
 *
 *  - a second find() while one iteration is live fails cleanly instead of
 *    closing the live iteration's directory out from under it,
 *  - every way an iteration can end (release, or next() running out) really
 *    does end it, so the lifetime lock is never stranded. On the device a
 *    stranded lock hangs the next caller forever; here the no-op host lock
 *    would hide that, but the same bookkeeping flag drives both, so a leak
 *    shows up as a later find() wrongly reporting ESP_ERR_INVALID_STATE,
 *  - nested locked nvs_* calls inside an iteration do not self-deadlock,
 *    which is what forced the shim's mutex to become recursive.
 */
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "unity.h"

#define NS "location"

void setUp(void) { TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_erase()); }

void tearDown(void) {}

/* Three contact-rule style keys, the shape the location policy actually
 * stores and iterates. */
static void seed_entries(void) {
    nvs_handle_t h = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(NS, NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(h, "lcr_aaaa", "1|full|60"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(h, "lcr_bbbb", "1|coarse|120"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(h, "def_tier", "coarse"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(h));
    nvs_close(h);
}

static void test_iterates_every_entry_then_releases(void) {
    seed_entries();

    nvs_iterator_t it = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &it));

    int seen = 0;
    while (it != NULL) {
        nvs_entry_info_t info;
        TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_info(it, &info));
        TEST_ASSERT_EQUAL_STRING(NS, info.namespace_name);
        seen++;
        if (nvs_entry_next(&it) != ESP_OK) {
            break;
        }
    }
    nvs_release_iterator(it);
    TEST_ASSERT_EQUAL_INT(3, seen);

    /* The iteration really ended: a fresh find must succeed rather than trip
     * the single-live-iterator guard. */
    nvs_iterator_t again = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &again));
    nvs_release_iterator(again);
}

/* The invariant that makes the device-side race unrepresentable: while one
 * iteration is live, a second find is refused. The old code silently closed
 * the live directory and reused the global iterator, which is precisely the
 * concurrent-open corruption. */
static void test_second_find_while_open_is_refused(void) {
    seed_entries();

    nvs_iterator_t first = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &first));
    TEST_ASSERT_NOT_NULL(first);

    nvs_iterator_t second = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &second));
    TEST_ASSERT_NULL(second);

    /* The refusal must not have disturbed the live iteration: it still walks
     * to completion. */
    int seen = 1;
    while (nvs_entry_next(&first) == ESP_OK) {
        nvs_entry_info_t info;
        TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_info(first, &info));
        seen++;
    }
    TEST_ASSERT_EQUAL_INT(3, seen);
    nvs_release_iterator(first);

    nvs_iterator_t after = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &after));
    nvs_release_iterator(after);
}

/* Exhausting the iteration via nvs_entry_next must end it by itself. Callers
 * all use the "break out of the loop, then release the now-NULL handle"
 * shape, so nvs_release_iterator(NULL) has to be the no-op that leaves the
 * shim reusable rather than a path that strands the lifetime lock. */
static void test_exhausted_iteration_ends_without_release(void) {
    seed_entries();

    nvs_iterator_t it = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &it));
    while (nvs_entry_next(&it) == ESP_OK) {
    }
    TEST_ASSERT_NULL(it);

    /* Deliberately NOT calling nvs_release_iterator here. */
    nvs_iterator_t again = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &again));
    nvs_release_iterator(again);

    /* And the redundant release callers do make is harmless. */
    nvs_release_iterator(NULL);
    nvs_iterator_t third = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &third));
    nvs_release_iterator(third);
}

/* The reason the shim's mutex had to become recursive. Every real caller
 * reads each entry's value with a locked nvs_get_* from inside the loop
 * (main/mesh_location.c and main/rpc_methods.c both do), and the iterator now
 * holds that same lock for the whole iteration. A non-recursive mutex would
 * deadlock the caller against itself on the first such call. */
static void test_nested_get_inside_iteration(void) {
    seed_entries();

    nvs_handle_t h = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(NS, NVS_READONLY, &h));

    nvs_iterator_t it = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &it));

    int rules = 0;
    while (it != NULL) {
        nvs_entry_info_t info;
        TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_info(it, &info));
        if (strncmp(info.key, "lcr_", 4) == 0) {
            char raw[48] = {0};
            size_t raw_len = sizeof(raw);
            TEST_ASSERT_EQUAL(ESP_OK, nvs_get_str(h, info.key, raw, &raw_len));
            TEST_ASSERT_TRUE(raw[0] == '1');
            rules++;
        }
        if (nvs_entry_next(&it) != ESP_OK) {
            break;
        }
    }
    nvs_release_iterator(it);
    nvs_close(h);
    TEST_ASSERT_EQUAL_INT(2, rules);
}

/* A find that matches nothing must not leave the iteration open, or the very
 * next caller is refused forever. */
static void test_empty_namespace_find_leaves_shim_reusable(void) {
    nvs_handle_t h = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(NS, NVS_READWRITE, &h));
    nvs_close(h);

    nvs_iterator_t it = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &it));
    TEST_ASSERT_NULL(it);

    seed_entries();
    nvs_iterator_t again = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &again));
    nvs_release_iterator(again);
}

/* An unknown namespace takes a different early-return path in find(); it must
 * drop the lock too. */
static void test_unknown_namespace_find_leaves_shim_reusable(void) {
    seed_entries();

    nvs_iterator_t missing = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND,
                      nvs_entry_find("nvs", "no_such_ns", NVS_TYPE_ANY, &missing));
    TEST_ASSERT_NULL(missing);

    nvs_iterator_t it = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_find("nvs", NS, NVS_TYPE_ANY, &it));
    nvs_release_iterator(it);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iterates_every_entry_then_releases);
    RUN_TEST(test_second_find_while_open_is_refused);
    RUN_TEST(test_exhausted_iteration_ends_without_release);
    RUN_TEST(test_nested_get_inside_iteration);
    RUN_TEST(test_empty_namespace_find_leaves_shim_reusable);
    RUN_TEST(test_unknown_namespace_find_leaves_shim_reusable);
    return UNITY_END();
}
