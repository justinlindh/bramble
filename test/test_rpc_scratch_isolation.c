/**
 * test_rpc_scratch_isolation.c
 *
 * rpc_dispatch() has no serialization and is entered concurrently from the
 * WebSocket httpd task, the BLE RPC task and the CLI task. Handler scratch
 * that outlives the call frame is therefore shared mutable state: bramble.
 * getStatus, bramble.getNeighbors and bramble.getAirtime each used to snapshot
 * ~1.5 KB of mesh state into a function-local `static`, so two overlapping
 * calls stomped one buffer and a client could be served a torn neighbor table
 * (issue #85).
 *
 * The invariant these cases pin: a snapshot taken by one dispatch is still
 * that dispatch's snapshot when it is serialized, even if a second dispatch
 * takes its own snapshot in between. The interleaving is forced rather than
 * raced. Thread A parks inside mesh_get_state(), after its snapshot is
 * populated but before the handler reads it, until thread B has completed a
 * full mesh_get_state() of its own. With a shared buffer A necessarily
 * serializes B's neighbors; with per-call buffers A serializes its own. There
 * is no timing window, so the test fails deterministically against the old
 * code rather than flaking.
 */
#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include "mesh_task.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Stub hooks from stubs/rpc_methods_test_stubs.c */
extern void (*g_stub_mesh_state_fill)(mesh_shared_state_t* out);
extern void (*g_stub_mesh_state_after_fill)(void);

static bramble_identity_t s_id = {
    .address = 0xAABBCCDD,
    .pubkey_hash = 0x11223344,
};

/* The OTA path strdups its URL into a task that never runs on the host. */
const char* __asan_default_options(void) { return "detect_leaks=0"; }

/* ── Interleaving harness ─────────────────────────────────────────── */

#define NEIGHBORS_PER_THREAD 4

/* Marker addresses: thread A's snapshot is all 0xAAAA00xx, thread B's is all
 * 0xBBBB00xx, so a torn read is visible in the serialized JSON itself. */
#define ADDR_A_BASE 0xAAAA0000u
#define ADDR_B_BASE 0xBBBB0000u

typedef enum {
    PHASE_START = 0, /* nobody has snapshotted yet */
    PHASE_A_PARKED,  /* A has its snapshot and is waiting inside the hook */
    PHASE_B_DONE,    /* B has taken its own full snapshot; A may proceed */
} phase_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static phase_t s_phase;
static pthread_t s_thread_a;
static bool s_thread_a_valid;

static bool current_is_thread_a(void) {
    return s_thread_a_valid && pthread_equal(pthread_self(), s_thread_a);
}

static void fill_marked_neighbors(mesh_shared_state_t* out) {
    uint32_t base = current_is_thread_a() ? ADDR_A_BASE : ADDR_B_BASE;
    out->neighbors.count = NEIGHBORS_PER_THREAD;
    for (int i = 0; i < NEIGHBORS_PER_THREAD; i++) {
        out->neighbors.entries[i].addr = base + (uint32_t)i;
        out->neighbors.entries[i].rssi = -70;
        out->neighbors.entries[i].snr = 8;
    }
    out->radio_ok = true;
}

/* Runs with the caller's snapshot already populated. A waits here for B. */
static void park_a_until_b_has_snapshotted(void) {
    pthread_mutex_lock(&s_lock);
    if (current_is_thread_a()) {
        s_phase = PHASE_A_PARKED;
        pthread_cond_broadcast(&s_cond);
        while (s_phase != PHASE_B_DONE) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
    } else {
        /* B never blocks: it signals that its own snapshot is complete and
         * releases A. Only one side ever waits, so the pair cannot deadlock. */
        s_phase = PHASE_B_DONE;
        pthread_cond_broadcast(&s_cond);
    }
    pthread_mutex_unlock(&s_lock);
}

typedef struct {
    char response[4096];
    int len;
} dispatch_result_t;

static const char* k_get_neighbors_req =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.getNeighbors\"}";

static void* thread_a_main(void* arg) {
    dispatch_result_t* out = (dispatch_result_t*)arg;

    /* A registers itself rather than being registered by the spawner: the
     * hooks below branch on this identity, and A can reach them before
     * pthread_create() has even returned to main. */
    pthread_mutex_lock(&s_lock);
    s_thread_a = pthread_self();
    s_thread_a_valid = true;
    pthread_mutex_unlock(&s_lock);

    out->len = rpc_dispatch(k_get_neighbors_req, out->response, sizeof(out->response));
    return NULL;
}

static void* thread_b_main(void* arg) {
    dispatch_result_t* out = (dispatch_result_t*)arg;

    /* Do not start until A is parked mid-snapshot, so B's snapshot lands
     * strictly between A's snapshot and A's serialization. */
    pthread_mutex_lock(&s_lock);
    while (s_phase != PHASE_A_PARKED) {
        pthread_cond_wait(&s_cond, &s_lock);
    }
    pthread_mutex_unlock(&s_lock);

    out->len = rpc_dispatch(k_get_neighbors_req, out->response, sizeof(out->response));
    return NULL;
}

/* Asserts the response carries exactly this thread's marker addresses. */
static void assert_neighbors_all_from(const dispatch_result_t* res, uint32_t expected_base) {
    TEST_ASSERT_GREATER_THAN(0, res->len);

    cJSON* resp = cJSON_Parse(res->response);
    TEST_ASSERT_NOT_NULL(resp);

    cJSON* result = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_NOT_NULL_MESSAGE(result, "getNeighbors returned an error, not a result");

    cJSON* arr = cJSON_GetObjectItem(result, "neighbors");
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_EQUAL_INT(NEIGHBORS_PER_THREAD, cJSON_GetArraySize(arr));

    for (int i = 0; i < NEIGHBORS_PER_THREAD; i++) {
        cJSON* entry = cJSON_GetArrayItem(arr, i);
        TEST_ASSERT_NOT_NULL(entry);
        const char* addr = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "address"));
        TEST_ASSERT_NOT_NULL(addr);

        char expected[12];
        snprintf(expected, sizeof(expected), "%08X", expected_base + (uint32_t)i);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(
            expected, addr,
            "neighbor table was torn: this dispatch serialized another dispatch's snapshot");
    }

    cJSON_Delete(resp);
}

void setUp(void) {
    rpc_init();
    rpc_methods_init(&s_id);
    s_phase = PHASE_START;
    s_thread_a_valid = false;
    g_stub_mesh_state_fill = NULL;
    g_stub_mesh_state_after_fill = NULL;
}

void tearDown(void) {
    g_stub_mesh_state_fill = NULL;
    g_stub_mesh_state_after_fill = NULL;
}

/* ── Cases ─────────────────────────────────────────────────────────── */

/* The core regression: two getNeighbors dispatches deliberately interleaved
 * between snapshot and serialization must each answer from their own data. */
void test_interleaved_get_neighbors_do_not_share_scratch(void) {
    dispatch_result_t res_a = {0};
    dispatch_result_t res_b = {0};

    g_stub_mesh_state_fill = fill_marked_neighbors;
    g_stub_mesh_state_after_fill = park_a_until_b_has_snapshotted;

    pthread_t a, b;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&a, NULL, thread_a_main, &res_a));
    /* B is gated on PHASE_A_PARKED, which only A can publish and only after
     * A has taken its own snapshot, so the ordering holds however the two
     * threads are scheduled. */
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&b, NULL, thread_b_main, &res_b));

    TEST_ASSERT_EQUAL_INT(0, pthread_join(a, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(b, NULL));

    /* A is the assertion that mattered: it snapshotted first and serialized
     * last, so a shared buffer hands it B's table. */
    assert_neighbors_all_from(&res_a, ADDR_A_BASE);
    assert_neighbors_all_from(&res_b, ADDR_B_BASE);
}

/* A single dispatch is unaffected by the change: the snapshot still round
 * trips, so the isolation above is not achieved by dropping data. */
void test_single_get_neighbors_still_serializes_its_snapshot(void) {
    dispatch_result_t res = {0};

    g_stub_mesh_state_fill = fill_marked_neighbors;
    /* No parking hook: nothing else is in flight. Thread A is never set, so
     * fill_marked_neighbors uses the B marker on this (main) thread. */
    res.len = rpc_dispatch(k_get_neighbors_req, res.response, sizeof(res.response));

    assert_neighbors_all_from(&res, ADDR_B_BASE);
}

/* getStatus read the same shared snapshot buffer and reports neighbor count
 * from it, so it gets the same treatment and the same coverage. */
void test_get_status_reports_its_own_snapshot_peer_count(void) {
    char response[2048];

    g_stub_mesh_state_fill = fill_marked_neighbors;
    int len = rpc_dispatch("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bramble.getStatus\"}",
                           response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON* resp = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(resp);
    cJSON* result = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(NEIGHBORS_PER_THREAD, cJSON_GetObjectItem(result, "peers")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(result, "radio_ok")));
    cJSON_Delete(resp);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_interleaved_get_neighbors_do_not_share_scratch);
    RUN_TEST(test_single_get_neighbors_still_serializes_its_snapshot);
    RUN_TEST(test_get_status_reports_its_own_snapshot_peer_count);
    return UNITY_END();
}
