/*
 * Boot-trace page walking (nrf/src/boot_trace_scan.c).
 *
 * The device-side half of this (NVMC erase, word write, reboot into the UF2
 * bootloader) cannot run on the host, so the decision logic is split out and
 * tested here: where the next record goes, when the page is out of room, and
 * how many consecutive boots failed. That last number is what sends a board
 * in a reset loop back to the bootloader on its own, so getting it wrong
 * either strands the board dark or bounces a healthy one into DFU.
 */
#include <string.h>

#include "boot_trace.h"
#include "boot_trace_scan.h"
#include "unity.h"

static uint32_t page[BOOT_TRACE_PAGE_WORDS];
static uint32_t next_slot;

void setUp(void) {
    memset(page, 0xFF, sizeof(page)); /* erased flash */
    next_slot = 1;
}

void tearDown(void) {}

static void write_magic(void) { page[0] = BOOT_TRACE_MAGIC; }

static void put(uint32_t tag, uint32_t aux) {
    page[next_slot] = BOOT_TRACE_TAG_MARKER | tag;
    page[next_slot + 1] = aux;
    next_slot += 2;
}

/* One boot that got all the way through. */
static void put_good_boot(void) {
    put(BT_BOOT_BEGIN, 0);
    put(BT_MAIN_ENTRY, 0x27000);
    put(BT_BOOT_DONE, 40000);
}

/* One boot that died partway, leaving no failure record: the reset-loop
 * shape, not the hang shape. */
static void put_failed_boot(uint32_t resetreas) {
    put(BT_BOOT_BEGIN, resetreas);
    put(BT_MAIN_ENTRY, 0x27000);
    put(BT_MSG_STORE, 0);
}

static boot_trace_scan_t run_scan(void) {
    boot_trace_scan_t scan;
    boot_trace_scan(page, &scan);
    return scan;
}

static void test_virgin_page_is_invalid(void) {
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_FALSE(scan.valid);
    TEST_ASSERT_EQUAL_UINT32(1, scan.next);
    TEST_ASSERT_EQUAL_UINT32(0, scan.failed_boots);
}

static void test_page_without_magic_is_invalid(void) {
    page[0] = 0xDEADBEEF;
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_FALSE(scan.valid);
}

static void test_empty_page_after_erase(void) {
    write_magic();
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_TRUE(scan.valid);
    TEST_ASSERT_EQUAL_UINT32(1, scan.next);
    TEST_ASSERT_EQUAL_UINT32(0, scan.failed_boots);
}

static void test_next_slot_lands_after_the_last_record(void) {
    write_magic();
    put_good_boot();
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(next_slot, scan.next);
}

/* The bug this guards: an aux word may be any value at all, including the
 * erased pattern. nvs_flash_init returning -1 stamps aux=0xFFFFFFFF, and a
 * scan that stopped at any 0xFFFFFFFF word would take that for the end of
 * the trace, silently truncating every boot at the NVS stage and appending
 * the next boot's records on top of the old ones. Only tag words carry the
 * marker, so only tag words may terminate the walk. */
static void test_aux_of_all_ones_does_not_end_the_walk(void) {
    write_magic();
    put(BT_BOOT_BEGIN, 0);
    put(BT_NVS_INIT, 0xFFFFFFFFu);
    put(BT_BOOT_DONE, 40000);
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(next_slot, scan.next);
    TEST_ASSERT_EQUAL_UINT32(0, scan.failed_boots);
}

static void test_completed_boot_clears_the_failure_count(void) {
    write_magic();
    put_failed_boot(0);
    put_failed_boot(0);
    put_good_boot();
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(0, scan.failed_boots);
}

static void test_consecutive_failed_boots_accumulate(void) {
    write_magic();
    put_failed_boot(0);
    put_failed_boot(0);
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(2, scan.failed_boots);
    TEST_ASSERT_TRUE(scan.failed_boots < BT_BOOT_LOOP_LIMIT);

    put_failed_boot(0);
    scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(3, scan.failed_boots);
    TEST_ASSERT_TRUE(scan.failed_boots >= BT_BOOT_LOOP_LIMIT);
}

/* A boot that failed loudly still counts: the rescue is about consecutive
 * boots that never finished, whatever they stamped on the way down. */
static void test_boot_with_a_failure_record_still_counts(void) {
    write_magic();
    put(BT_BOOT_BEGIN, 0);
    put(BT_FAIL_HARDFAULT, 0x2A1C4);
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(1, scan.failed_boots);
}

/* Without this the device bounces straight back into DFU on the boot after
 * a rescue, and a freshly flashed working image never gets to run. */
static void test_rescue_record_clears_the_failure_count(void) {
    write_magic();
    put_failed_boot(0);
    put_failed_boot(0);
    put_failed_boot(0);
    put(BT_FAIL_BOOTLOOP, 3);
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(0, scan.failed_boots);

    /* And the next three failures are counted from scratch. */
    put_failed_boot(0);
    scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(1, scan.failed_boots);
}

/* The count outlives the page it was counted on. */
static void test_carry_record_seeds_the_failure_count(void) {
    write_magic();
    put(BT_BOOT_CARRY, 2);
    put_failed_boot(0);
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(3, scan.failed_boots);
}

static void test_carry_replaces_rather_than_adds(void) {
    write_magic();
    put(BT_BOOT_CARRY, 2);
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_EQUAL_UINT32(2, scan.failed_boots);
}

static void test_exhaustion_leaves_room_for_a_whole_boot(void) {
    TEST_ASSERT_FALSE(boot_trace_page_exhausted(1));
    TEST_ASSERT_TRUE(boot_trace_page_exhausted(BOOT_TRACE_PAGE_WORDS - 1));
    /* The bound must trip while a full boot's records still fit, not once
     * the page is already full, or the boot that matters gets cut in half. */
    TEST_ASSERT_TRUE(boot_trace_page_exhausted(BOOT_TRACE_PAGE_WORDS - 64));
}

/* A page filled to the brim must not walk past its own end. */
static void test_full_page_scan_stops_at_the_page_end(void) {
    write_magic();
    for (uint32_t i = 1; i + 1 < BOOT_TRACE_PAGE_WORDS; i += 2) {
        page[i] = BOOT_TRACE_TAG_MARKER | BT_MSG_STORE;
        page[i + 1] = 0;
    }
    boot_trace_scan_t scan = run_scan();
    TEST_ASSERT_TRUE(scan.valid);
    TEST_ASSERT_TRUE(scan.next <= BOOT_TRACE_PAGE_WORDS);
    TEST_ASSERT_TRUE(boot_trace_page_exhausted(scan.next));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_virgin_page_is_invalid);
    RUN_TEST(test_page_without_magic_is_invalid);
    RUN_TEST(test_empty_page_after_erase);
    RUN_TEST(test_next_slot_lands_after_the_last_record);
    RUN_TEST(test_aux_of_all_ones_does_not_end_the_walk);
    RUN_TEST(test_completed_boot_clears_the_failure_count);
    RUN_TEST(test_consecutive_failed_boots_accumulate);
    RUN_TEST(test_boot_with_a_failure_record_still_counts);
    RUN_TEST(test_rescue_record_clears_the_failure_count);
    RUN_TEST(test_carry_record_seeds_the_failure_count);
    RUN_TEST(test_carry_replaces_rather_than_adds);
    RUN_TEST(test_exhaustion_leaves_room_for_a_whole_boot);
    RUN_TEST(test_full_page_scan_stops_at_the_page_end);
    return UNITY_END();
}
