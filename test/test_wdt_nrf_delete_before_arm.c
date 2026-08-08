/*
 * Companion to test_wdt_nrf.c, in its own process for a fresh set of
 * wdt_nrf.c's statics (see that file's header comment for why). Covers the
 * one sequence it does not: a task deleting itself BEFORE bramble_wdt_arm()
 * runs, which exercises the "first feed for anything already deleted
 * before arm ran" catch-up loop inside bramble_wdt_arm() itself, in
 * principle reachable if radio_init() fails during boot, before app_init.c
 * ever reaches bramble_wdt_arm().
 */
#include "esp_task_wdt.h"
#include "bramble_wdt.h"

#include <nrfx_wdt.h>

#include <stdbool.h>

#include "unity.h"

static int s_alloc_count;
static bool s_enabled;
static int s_feed_count[8];

nrfx_err_t nrfx_wdt_init(const nrfx_wdt_t* p_instance, const nrfx_wdt_config_t* p_config,
                         void* wdt_event_handler, void* p_context) {
    (void)p_instance;
    (void)p_config;
    (void)wdt_event_handler;
    (void)p_context;
    return NRFX_SUCCESS;
}

nrfx_err_t nrfx_wdt_channel_alloc(const nrfx_wdt_t* p_instance, nrfx_wdt_channel_id* p_channel_id) {
    (void)p_instance;
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

#define TASK_MESH ((TaskHandle_t)1)
#define TASK_RADIO ((TaskHandle_t)2)

static TaskHandle_t s_current_task = TASK_MESH;

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return s_current_task; }

const char* pcTaskGetName(TaskHandle_t task) {
    TaskHandle_t t = task ? task : s_current_task;
    if (t == TASK_MESH) {
        return "mesh";
    }
    if (t == TASK_RADIO) {
        return "radio";
    }
    return NULL;
}

static void as(TaskHandle_t task) { s_current_task = task; }

void setUp(void) {}
void tearDown(void) {}

/* Sets up both channels and deletes radio's before arm ever runs: channel 0
 * is mesh's, channel 1 is radio's, per nrfx_wdt_channel_alloc's allocation
 * order above. */
static void test_delete_before_arm_marks_the_bit_without_feeding(void) {
    bramble_wdt_init();
    as(TASK_MESH);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_add(NULL));
    as(TASK_RADIO);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_add(NULL));
    TEST_ASSERT_EQUAL(2, s_alloc_count);

    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_delete(NULL));
    /* Not armed yet: esp_task_wdt_delete's immediate-feed branch must not
     * fire early, since nrfx_wdt_channel_feed() before nrfx_wdt_enable()
     * would be feeding a channel that isn't counting down yet. */
    TEST_ASSERT_EQUAL(0, s_feed_count[1]);
}

/* This is the behavior the whole scenario exists to prove: bramble_wdt_arm()
 * must give radio's already-deleted channel its first feed as part of
 * arming, or it would sit unfed from the moment the countdown starts and
 * reset the board within one period even though nothing is actually
 * hung. */
static void test_arm_gives_the_already_deleted_channel_its_first_feed(void) {
    bramble_wdt_arm();
    TEST_ASSERT_TRUE(s_enabled);
    TEST_ASSERT_EQUAL(1, s_feed_count[1]);
    TEST_ASSERT_EQUAL(0, s_feed_count[0]); /* mesh's own feed has not run yet */
}

/* mesh keeps operating normally after arm, undisturbed by radio's deletion. */
static void test_mesh_still_feeds_normally_after_arm(void) {
    as(TASK_MESH);
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_reset());
    TEST_ASSERT_EQUAL(1, s_feed_count[0]);
    /* radio's deleted channel keeps getting fed by proxy on every live
     * reset() call, mesh's included. */
    TEST_ASSERT_EQUAL(2, s_feed_count[1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_delete_before_arm_marks_the_bit_without_feeding);
    RUN_TEST(test_arm_gives_the_already_deleted_channel_its_first_feed);
    RUN_TEST(test_mesh_still_feeds_normally_after_arm);
    return UNITY_END();
}
