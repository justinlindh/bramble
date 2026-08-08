/*
 * State machine in nrf/shim/wdt_nrf.c: the real nrfx_wdt calls are stubbed
 * (test/stubs/wdt_nrf/nrfx_wdt.h declares the surface, this file supplies
 * recording bodies) and FreeRTOS task identity is faked (test/stubs/wdt_nrf/
 * task.h; fake TaskHandle_t values map to names via s_current_task /
 * pcTaskGetName below), so the module's bookkeeping runs for real, on the
 * host, without touching a peripheral.
 *
 * wdt_nrf.c has no reset hook (by design: a real boot calls
 * bramble_wdt_init() once, arms once, and never tears down), so its statics
 * persist for this whole process. The test cases below are therefore ONE
 * continuous scenario, run in the order RUN_TEST lists them (Unity does not
 * reorder), narrating a boot: driver init, both tasks subscribing, arming,
 * then post-arm operation, in the same sequence app_init.c and mesh_task.c
 * actually produce. test_wdt_nrf_delete_before_arm.c is a separate process
 * (fresh statics) for the one scenario that needs a channel deleted BEFORE
 * arm rather than after.
 */
#include "esp_task_wdt.h"
#include "bramble_wdt.h"

#include <nrfx_wdt.h>

#include <stdbool.h>

#include "unity.h"

/* ---- nrfx_wdt fakes: observable via the counters below ---- */
static int s_alloc_count;
static bool s_alloc_should_fail;
static bool s_init_called;
static bool s_enabled;
static int s_feed_count[8];

nrfx_err_t nrfx_wdt_init(const nrfx_wdt_t* p_instance, const nrfx_wdt_config_t* p_config,
                         void* wdt_event_handler, void* p_context) {
    (void)p_instance;
    (void)p_config;
    (void)wdt_event_handler;
    (void)p_context;
    s_init_called = true;
    return NRFX_SUCCESS;
}

nrfx_err_t nrfx_wdt_channel_alloc(const nrfx_wdt_t* p_instance, nrfx_wdt_channel_id* p_channel_id) {
    (void)p_instance;
    if (s_alloc_should_fail) {
        return NRFX_ERROR_NO_MEM;
    }
    *p_channel_id = s_alloc_count;
    s_alloc_count++;
    return NRFX_SUCCESS;
}

void nrfx_wdt_enable(const nrfx_wdt_t* p_instance) {
    (void)p_instance;
    s_enabled = true;
}

void nrfx_wdt_channel_feed(const nrfx_wdt_t* p_instance, nrfx_wdt_channel_id channel_id) {
    (void)p_instance;
    s_feed_count[channel_id]++;
}

/* ---- FreeRTOS task-identity fakes ---- */
#define TASK_MESH ((TaskHandle_t)1)
#define TASK_RADIO ((TaskHandle_t)2)
#define TASK_OTHER ((TaskHandle_t)3)

static TaskHandle_t s_current_task = TASK_OTHER;

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return s_current_task; }

const char* pcTaskGetName(TaskHandle_t task) {
    TaskHandle_t t = task ? task : s_current_task;
    if (t == TASK_MESH) {
        return "mesh";
    }
    if (t == TASK_RADIO) {
        return "radio";
    }
    if (t == TASK_OTHER) {
        return "other";
    }
    return NULL;
}

static void as(TaskHandle_t task) { s_current_task = task; }

void setUp(void) {}
void tearDown(void) {}

/* 1. Before bramble_wdt_init(): every call must behave exactly like the
 * no-op shim this file replaces, not touch nrfx state at all.
 * esp_task_wdt_reset() reports ESP_OK, not ESP_ERR_NOT_FOUND, before the
 * watchdog is armed: it short-circuits on WDT_STATE_ARMED before it ever
 * looks at whether the caller has a channel, matching "nothing is being
 * watched yet" rather than "you specifically are not being watched". */
static void test_add_before_init_is_a_noop(void) {
    as(TASK_MESH);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_add(NULL));
    TEST_ASSERT_EQUAL(0, s_alloc_count);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_reset());
}

/* 2. bramble_wdt_init() reaches the real nrfx_wdt_init() exactly once and
 * flips the driver-ready bit; a second call is a no-op (idempotent, since
 * main_nrf.c's boot sequence could in principle call it more than once). */
static void test_init_is_idempotent(void) {
    bramble_wdt_init();
    TEST_ASSERT_TRUE(s_init_called);
    s_init_called = false;
    bramble_wdt_init();
    TEST_ASSERT_FALSE(s_init_called); /* second call: already ready, short-circuits */
}

/* 3. task_id_for's "not one of this build's known subscribers" path: a
 * name-less or unrecognized task gets nothing to feed, not an error. */
static void test_unknown_task_add_is_ignored(void) {
    as(TASK_OTHER);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_add(NULL));
    TEST_ASSERT_EQUAL(0, s_alloc_count);
}

/* 4. mesh registers: task_id_for maps "mesh" to a channel, and it is the
 * first one nrfx hands out. */
static void test_mesh_add_allocates_channel_zero(void) {
    as(TASK_MESH);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_add(NULL));
    TEST_ASSERT_EQUAL(1, s_alloc_count);
}

/* 5. Re-adding the same task is idempotent: no second channel. */
static void test_duplicate_add_does_not_allocate_again(void) {
    as(TASK_MESH);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_add(NULL));
    TEST_ASSERT_EQUAL(1, s_alloc_count);
}

/* 6. Before bramble_wdt_arm(), esp_task_wdt_reset() is a no-op for anyone,
 * registered or not: nothing should feed. */
static void test_reset_before_arm_feeds_nothing(void) {
    as(TASK_MESH);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_reset());
    TEST_ASSERT_EQUAL(0, s_feed_count[0]);
}

/* 7. Arming: nrfx_wdt_enable() runs, and since nothing is deleted yet, the
 * arm-time catch-up loop feeds nothing (see
 * test_wdt_nrf_delete_before_arm.c for the case where it does). radio has
 * deliberately not been added yet, so this exercises arming with exactly
 * one of the two possible channels registered. */
static void test_arm_enables_and_feeds_nothing_yet(void) {
    bramble_wdt_arm();
    TEST_ASSERT_TRUE(s_enabled);
    TEST_ASSERT_EQUAL(0, s_feed_count[0]);
}

/* 8. The post-arm refusal this whole file exists to make real: radio is a
 * known task (task_id_for recognizes "radio") that never registered before
 * arm. nrfx_wdt_channel_alloc() cannot run anymore, so this must be
 * refused, not silently drop the channel or crash into an NRFX_ASSERT. */
static void test_late_add_after_arm_is_refused(void) {
    as(TASK_RADIO);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_task_wdt_add(NULL));
    TEST_ASSERT_EQUAL(1, s_alloc_count); /* still just mesh's */
}

/* 9. Post-arm esp_task_wdt_reset() feeds only the calling task's own
 * channel radio has no channel at all (refused above), so its reset()
 * call finds nothing of its own to feed and returns NOT_FOUND, while
 * mesh's own reset() feeds channel 0 exactly once. */
static void test_reset_feeds_only_the_calling_tasks_channel(void) {
    as(TASK_RADIO);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, esp_task_wdt_reset());
    TEST_ASSERT_EQUAL(0, s_feed_count[0]);

    as(TASK_MESH);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_reset());
    TEST_ASSERT_EQUAL(1, s_feed_count[0]);
}

/* 10. esp_task_wdt_delete() after arm: marks mesh's channel opted out and
 * feeds it immediately (nothing else will touch it until some other live
 * task's reset() runs, per the proxy-feed design). */
static void test_delete_after_arm_feeds_immediately(void) {
    as(TASK_MESH);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_delete(NULL));
    TEST_ASSERT_EQUAL(2, s_feed_count[0]); /* the reset() feed, plus this one */
}

/* 11. The deleted-bit proxy-feed path: mesh is now deleted, has no live
 * owner left to feed it, yet radio (which has no channel of its own, so
 * its own outcome is still NOT_FOUND) must feed it anyway as a side effect
 * of calling esp_task_wdt_reset() at all. This is what keeps an
 * orphaned channel from starving the board on its own. */
static void test_reset_from_any_live_task_proxy_feeds_deleted_channels(void) {
    as(TASK_RADIO);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, esp_task_wdt_reset());
    TEST_ASSERT_EQUAL(3, s_feed_count[0]); /* fed by proxy despite radio's own miss */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_before_init_is_a_noop);
    RUN_TEST(test_init_is_idempotent);
    RUN_TEST(test_unknown_task_add_is_ignored);
    RUN_TEST(test_mesh_add_allocates_channel_zero);
    RUN_TEST(test_duplicate_add_does_not_allocate_again);
    RUN_TEST(test_reset_before_arm_feeds_nothing);
    RUN_TEST(test_arm_enables_and_feeds_nothing_yet);
    RUN_TEST(test_late_add_after_arm_is_refused);
    RUN_TEST(test_reset_feeds_only_the_calling_tasks_channel);
    RUN_TEST(test_delete_after_arm_feeds_immediately);
    RUN_TEST(test_reset_from_any_live_task_proxy_feeds_deleted_channels);
    return UNITY_END();
}
