/*
 * Roll-call RPC surface: bramble.startRollCall and bramble.getRollCall.
 *
 * The primitive's own rules are proven in test_rollcall.c against the pure
 * core. What is under test HERE is the contract a client sees: the refusals
 * an operator has to act on (busy vs rate limited vs nothing transmitted,
 * each with the interval to wait), and the ledger projection, including the
 * honesty switch that makes `missing` empty on an un-anchored mesh no matter
 * how many members stayed silent.
 *
 * The ledger the handler reads is built through the REAL components/rollcall
 * API (via the driveable stubs in stubs/rollcall_stubs.c), so this test
 * cannot pass against a projection of a ledger shape the firmware would
 * never produce.
 */
#include "unity.h"

#include "cJSON.h"
#include "mesh_rollcall.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"

#include <stdio.h>
#include <string.h>

extern int g_rollcall_start_result;
extern uint32_t g_rollcall_next_id;
extern uint32_t g_rollcall_pending_dropped;
extern uint32_t g_rollcall_retry_after_ms;
extern bool g_rollcall_ledger_present;
extern rollcall_ledger_t g_rollcall_ledger;

#define INITIATOR_ADDR 0xAABBCCDDu
#define MEMBER_B 0x0000000Bu
#define MEMBER_C 0x0000000Cu
#define MEMBER_D 0x0000000Du

static bramble_identity_t s_id = {
    .address = INITIATOR_ADDR,
    .pubkey_hash = 0x11223344,
};

void setUp(void) {
    g_rollcall_start_result = MESH_ROLLCALL_OK;
    g_rollcall_next_id = 0x0000BEEF;
    g_rollcall_pending_dropped = 0;
    g_rollcall_retry_after_ms = 0;
    g_rollcall_ledger_present = false;
    rollcall_ledger_init(&g_rollcall_ledger);
    rpc_init();
    rpc_methods_init(&s_id);
}

void tearDown(void) {}

/* Dispatch one call and hand back the parsed envelope. The buffer is the
 * caller's so a test can also assert on the raw bytes. */
static cJSON* call(const char* method, const char* params_json, char* resp, size_t resp_len) {
    char req[512];
    snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
             method, params_json);
    int len = rpc_dispatch(req, resp, resp_len);
    TEST_ASSERT_GREATER_THAN(0, len);
    cJSON* j = cJSON_Parse(resp);
    TEST_ASSERT_NOT_NULL(j);
    return j;
}

static cJSON* result_of(cJSON* envelope) {
    cJSON* res = cJSON_GetObjectItem(envelope, "result");
    TEST_ASSERT_NOT_NULL(res);
    return res;
}

/* Open a ledger the way mesh_rollcall_start would, then make the stub hand
 * it to the handlers. */
static void ledger_open(bool anchored, const uint32_t* expected, uint8_t expected_count) {
    TEST_ASSERT_TRUE(rollcall_ledger_start(&g_rollcall_ledger, 0x0000BEEF, INITIATOR_ADDR, 1000,
                                           "sound off", 9, expected, expected_count, anchored));
    rollcall_ledger_note_round(&g_rollcall_ledger, 1, 1000);
    g_rollcall_ledger_present = true;
}

/* ── startRollCall ──────────────────────────────────────────────────── */

void test_start_reports_the_id_and_the_schedule(void) {
    const uint32_t expected[] = {MEMBER_B, MEMBER_C};
    ledger_open(true, expected, 2);

    char resp[1024];
    cJSON* j = call("bramble.startRollCall", "{\"text\":\"sound off\"}", resp, sizeof(resp));
    cJSON* res = result_of(j);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "ok")));
    TEST_ASSERT_EQUAL_STRING("0000BEEF", cJSON_GetObjectItem(res, "rollcall_id")->valuestring);
    TEST_ASSERT_EQUAL_UINT32(rollcall_window_ms(),
                             (uint32_t)cJSON_GetObjectItem(res, "window_ms")->valuedouble);
    TEST_ASSERT_EQUAL_INT(ROLLCALL_MAX_ROUNDS,
                          (int)cJSON_GetObjectItem(res, "rounds_total")->valuedouble);
    TEST_ASSERT_EQUAL_INT(2, (int)cJSON_GetObjectItem(res, "expected")->valuedouble);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "anchored")));
    cJSON_Delete(j);
}

void test_start_without_text_is_accepted(void) {
    char resp[1024];
    cJSON* j = call("bramble.startRollCall", "{}", resp, sizeof(resp));
    cJSON* res = result_of(j);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "ok")));
    /* No ledger to read yet: expected/anchored must still be present and
     * honest rather than absent, so a client never has to guess. */
    TEST_ASSERT_EQUAL_INT(0, (int)cJSON_GetObjectItem(res, "expected")->valuedouble);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "anchored")));
    cJSON_Delete(j);
}

void test_start_rejects_an_oversized_payload_as_a_client_defect(void) {
    /* One byte over ROLLCALL_TEXT_MAX. A payload the announce cannot carry
     * is a malformed request, not a state the mesh grows out of, so it is a
     * real JSON-RPC error rather than a soft refusal. */
    char params[128];
    char text[ROLLCALL_TEXT_MAX + 2];
    memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    snprintf(params, sizeof(params), "{\"text\":\"%s\"}", text);

    char resp[1024];
    cJSON* j = call("bramble.startRollCall", params, resp, sizeof(resp));
    cJSON* err = cJSON_GetObjectItem(j, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_INT(-32602, (int)cJSON_GetObjectItem(err, "code")->valuedouble);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(j, "result"));
    cJSON_Delete(j);
}

void test_start_accepts_a_payload_exactly_at_the_cap(void) {
    char params[128];
    char text[ROLLCALL_TEXT_MAX + 1];
    memset(text, 'x', ROLLCALL_TEXT_MAX);
    text[ROLLCALL_TEXT_MAX] = '\0';
    snprintf(params, sizeof(params), "{\"text\":\"%s\"}", text);

    char resp[1024];
    cJSON* j = call("bramble.startRollCall", params, resp, sizeof(resp));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(result_of(j), "ok")));
    cJSON_Delete(j);
}

void test_start_rate_limited_reports_how_long_to_wait(void) {
    g_rollcall_start_result = MESH_ROLLCALL_ERR_RATE_LIMITED;
    g_rollcall_retry_after_ms = 247000;

    char resp[1024];
    cJSON* j = call("bramble.startRollCall", "{}", resp, sizeof(resp));
    /* A refusal is ok:false in the RESULT, not a JSON-RPC error: the
     * dispatcher discards a handler's result whenever it returns nonzero,
     * so an error code could not carry retry_after_ms at all. */
    cJSON* res = result_of(j);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "ok")));
    TEST_ASSERT_EQUAL_STRING("rate_limited", cJSON_GetObjectItem(res, "reason")->valuestring);
    TEST_ASSERT_EQUAL_UINT32(247000,
                             (uint32_t)cJSON_GetObjectItem(res, "retry_after_ms")->valuedouble);
    TEST_ASSERT_EQUAL_UINT32(ROLLCALL_MIN_INTERVAL_MS,
                             (uint32_t)cJSON_GetObjectItem(res, "min_interval_ms")->valuedouble);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(res, "rollcall_id"));
    cJSON_Delete(j);
}

void test_start_busy_is_distinct_from_rate_limited(void) {
    g_rollcall_start_result = MESH_ROLLCALL_ERR_BUSY;
    g_rollcall_retry_after_ms = 90000;

    char resp[1024];
    cJSON* j = call("bramble.startRollCall", "{}", resp, sizeof(resp));
    cJSON* res = result_of(j);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "ok")));
    /* Both refusals mean "wait", but they are different operator problems
     * (one clears when this roll-call closes, the other when the interval
     * elapses), so the reason must distinguish them. */
    TEST_ASSERT_EQUAL_STRING("busy", cJSON_GetObjectItem(res, "reason")->valuestring);
    TEST_ASSERT_EQUAL_UINT32(90000,
                             (uint32_t)cJSON_GetObjectItem(res, "retry_after_ms")->valuedouble);
    cJSON_Delete(j);
}

void test_start_reports_a_refused_transmission(void) {
    g_rollcall_start_result = MESH_ROLLCALL_ERR_TX;

    char resp[1024];
    cJSON* j = call("bramble.startRollCall", "{}", resp, sizeof(resp));
    cJSON* res = result_of(j);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "ok")));
    TEST_ASSERT_EQUAL_STRING("not_transmitted", cJSON_GetObjectItem(res, "reason")->valuestring);
    /* Nothing reached the air, so the rate limiter was never charged and
     * the next attempt may go out immediately. */
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)cJSON_GetObjectItem(res, "retry_after_ms")->valuedouble);
    cJSON_Delete(j);
}

/* ── getRollCall ────────────────────────────────────────────────────── */

void test_get_before_any_rollcall_reports_inactive_with_the_bounds(void) {
    char resp[1024];
    cJSON* j = call("bramble.getRollCall", "{}", resp, sizeof(resp));
    cJSON* res = result_of(j);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "active")));
    /* The bounds are reported even with no roll-call to describe: a client
     * needs the text cap and the rate limit to build its form before the
     * first roll-call exists. */
    TEST_ASSERT_EQUAL_INT(ROLLCALL_MAX_ROUNDS,
                          (int)cJSON_GetObjectItem(res, "rounds_total")->valuedouble);
    TEST_ASSERT_EQUAL_UINT32(ROLLCALL_MIN_INTERVAL_MS,
                             (uint32_t)cJSON_GetObjectItem(res, "min_interval_ms")->valuedouble);
    TEST_ASSERT_EQUAL_INT(ROLLCALL_TEXT_MAX,
                          (int)cJSON_GetObjectItem(res, "max_text_bytes")->valuedouble);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(res, "responders"));
    cJSON_Delete(j);
}

void test_get_reports_responders_and_the_anchored_missing_set(void) {
    const uint32_t expected[] = {MEMBER_B, MEMBER_C, MEMBER_D};
    ledger_open(true, expected, 3);
    TEST_ASSERT_TRUE(
        rollcall_ledger_note_response(&g_rollcall_ledger, 0x0000BEEF, MEMBER_B, 1, 4200));
    TEST_ASSERT_TRUE(
        rollcall_ledger_note_response(&g_rollcall_ledger, 0x0000BEEF, MEMBER_C, 2, 9000));
    rollcall_ledger_note_unattested(&g_rollcall_ledger);

    char resp[4096];
    cJSON* j = call("bramble.getRollCall", "{}", resp, sizeof(resp));
    cJSON* res = result_of(j);

    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "active")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "open")));
    TEST_ASSERT_EQUAL_STRING("0000BEEF", cJSON_GetObjectItem(res, "rollcall_id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("sound off", cJSON_GetObjectItem(res, "text")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "anchored")));
    TEST_ASSERT_EQUAL_INT(3, (int)cJSON_GetObjectItem(res, "expected")->valuedouble);
    TEST_ASSERT_EQUAL_INT(2, (int)cJSON_GetObjectItem(res, "responded")->valuedouble);
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItem(res, "unattested")->valuedouble);
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItem(res, "rounds_sent")->valuedouble);

    cJSON* responders = cJSON_GetObjectItem(res, "responders");
    TEST_ASSERT_TRUE(cJSON_IsArray(responders));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(responders));
    cJSON* first = cJSON_GetArrayItem(responders, 0);
    TEST_ASSERT_EQUAL_STRING("0000000B", cJSON_GetObjectItem(first, "address")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(first, "responded")));
    /* Time is reported INTO the roll-call, not as device uptime: the ledger
     * started at 1000 and this answer landed at 4200. */
    TEST_ASSERT_EQUAL_INT(3200, (int)cJSON_GetObjectItem(first, "at_ms")->valuedouble);
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItem(first, "round")->valuedouble);

    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItem(res, "missing_count")->valuedouble);
    cJSON* missing = cJSON_GetObjectItem(res, "missing");
    TEST_ASSERT_TRUE(cJSON_IsArray(missing));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(missing));
    TEST_ASSERT_EQUAL_STRING("0000000D", cJSON_GetArrayItem(missing, 0)->valuestring);
    cJSON_Delete(j);
}

void test_get_on_an_unanchored_mesh_names_nobody_missing(void) {
    /* Same silence as the anchored case above (B answered, C and D did
     * not), but this node pins trust-on-first-use identities, so there is
     * no authoritative expected set and nothing can be called missing. */
    const uint32_t expected[] = {MEMBER_B, MEMBER_C, MEMBER_D};
    ledger_open(false, expected, 3);
    TEST_ASSERT_TRUE(
        rollcall_ledger_note_response(&g_rollcall_ledger, 0x0000BEEF, MEMBER_B, 1, 4200));

    char resp[4096];
    cJSON* j = call("bramble.getRollCall", "{}", resp, sizeof(resp));
    cJSON* res = result_of(j);

    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "anchored")));
    TEST_ASSERT_EQUAL_INT(0, (int)cJSON_GetObjectItem(res, "expected")->valuedouble);
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItem(res, "responded")->valuedouble);
    TEST_ASSERT_EQUAL_INT(0, (int)cJSON_GetObjectItem(res, "missing_count")->valuedouble);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(cJSON_GetObjectItem(res, "missing")));
    cJSON_Delete(j);
}

void test_get_reports_the_relay_path_when_a_receipt_supplied_one(void) {
    const uint32_t expected[] = {MEMBER_B};
    ledger_open(true, expected, 1);
    TEST_ASSERT_TRUE(
        rollcall_ledger_note_response(&g_rollcall_ledger, 0x0000BEEF, MEMBER_B, 1, 2000));
    const uint32_t path[] = {INITIATOR_ADDR, MEMBER_C, MEMBER_B};
    TEST_ASSERT_TRUE(rollcall_ledger_note_path(&g_rollcall_ledger, 0x0000BEEF, MEMBER_B, 3, path));

    char resp[4096];
    cJSON* j = call("bramble.getRollCall", "{}", resp, sizeof(resp));
    cJSON* res = result_of(j);
    cJSON* row = cJSON_GetArrayItem(cJSON_GetObjectItem(res, "responders"), 0);
    TEST_ASSERT_EQUAL_INT(3, (int)cJSON_GetObjectItem(row, "hops")->valuedouble);
    cJSON* p = cJSON_GetObjectItem(row, "path");
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(p));
    TEST_ASSERT_EQUAL_STRING("AABBCCDD", cJSON_GetArrayItem(p, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("0000000C", cJSON_GetArrayItem(p, 1)->valuestring);
    TEST_ASSERT_EQUAL_STRING("0000000B", cJSON_GetArrayItem(p, 2)->valuestring);
    cJSON_Delete(j);
}

void test_get_reports_answers_this_node_could_not_queue(void) {
    /* A node whose own pending-answer queue overflowed is a gap in the
     * fleet's coverage that no other node's ledger can show, so the count
     * has to surface locally. */
    g_rollcall_pending_dropped = 4;

    char resp[1024];
    cJSON* j = call("bramble.getRollCall", "{}", resp, sizeof(resp));
    cJSON* res = result_of(j);
    TEST_ASSERT_EQUAL_INT(4, (int)cJSON_GetObjectItem(res, "pending_dropped")->valuedouble);
    cJSON_Delete(j);
}

void test_get_reports_a_closed_ledger_as_still_readable(void) {
    const uint32_t expected[] = {MEMBER_B};
    ledger_open(true, expected, 1);
    TEST_ASSERT_TRUE(
        rollcall_ledger_maybe_close(&g_rollcall_ledger, 1000 + rollcall_window_ms() + 1));

    char resp[2048];
    cJSON* j = call("bramble.getRollCall", "{}", resp, sizeof(resp));
    cJSON* res = result_of(j);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "active")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "open")));
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItem(res, "missing_count")->valuedouble);
    cJSON_Delete(j);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_start_reports_the_id_and_the_schedule);
    RUN_TEST(test_start_without_text_is_accepted);
    RUN_TEST(test_start_rejects_an_oversized_payload_as_a_client_defect);
    RUN_TEST(test_start_accepts_a_payload_exactly_at_the_cap);
    RUN_TEST(test_start_rate_limited_reports_how_long_to_wait);
    RUN_TEST(test_start_busy_is_distinct_from_rate_limited);
    RUN_TEST(test_start_reports_a_refused_transmission);
    RUN_TEST(test_get_before_any_rollcall_reports_inactive_with_the_bounds);
    RUN_TEST(test_get_reports_responders_and_the_anchored_missing_set);
    RUN_TEST(test_get_on_an_unanchored_mesh_names_nobody_missing);
    RUN_TEST(test_get_reports_the_relay_path_when_a_receipt_supplied_one);
    RUN_TEST(test_get_reports_answers_this_node_could_not_queue);
    RUN_TEST(test_get_reports_a_closed_ledger_as_still_readable);
    return UNITY_END();
}
