/*
 * PHY passthrough gating matrix (DESIGN.md section 10).
 *
 * The gating IS the product, so every clause of section 10 has an assertion:
 *   - disabled by default                         -> test_default_off,
 *   - enable only via an authenticated RPC         -> test_auth_policy_rejects_unauth
 *                                                     (drives the real rpc_auth policy,
 *                                                     the exact gate the dispatcher uses),
 *   - auto-expires; TX refused after expiry        -> test_ttl_expiry_deactivates
 *                                                     (is_active gates handle_phy_tx),
 *   - refuse while holding a live identity          -> test_identity_refuses,
 *   - force override                                -> test_force_overrides_identity,
 *   - never persists across reboot                  -> structural: no NVS in the module
 *                                                     (default-off is test_default_off;
 *                                                     disable is immediate),
 *   - frame forwarded intact through the RX tap     -> test_forward_rx_* (the round trip:
 *                                                     raw bytes + rssi/snr/freq reach the
 *                                                     emit hook unchanged, only when active).
 *
 * The module's only platform seam is esp_timer; ESP_TIMER_CUSTOM_IMPL (set in
 * CMake) lets the test drive the clock for the TTL cases.
 */

#include <string.h>

#include "unity.h"
#include "rpc_auth.h"
#include "phy_passthrough.h"

/* ---- controllable clock (ESP_TIMER_CUSTOM_IMPL is set in CMake) ---- */
static int64_t g_now_us;
int64_t esp_timer_get_time(void) { return g_now_us; }
static void advance_sec(uint32_t s) { g_now_us += (int64_t)s * 1000000; }

/* ---- captured emit-hook (the RX forward tap's downstream) ---- */
static int g_emit_calls;
static uint8_t g_emit_buf[255];
static uint8_t g_emit_len;
static int16_t g_emit_rssi;
static int8_t g_emit_snr;
static uint32_t g_emit_freq;
static void capture_emit(const uint8_t* data, uint8_t len, const radio_rx_info_t* info,
                         uint32_t freq_hz) {
    g_emit_calls++;
    g_emit_len = len;
    memcpy(g_emit_buf, data, len);
    g_emit_rssi = info->rssi;
    g_emit_snr = info->snr;
    g_emit_freq = freq_hz;
}

void setUp(void) {
    g_now_us = 0;
    g_emit_calls = 0;
    g_emit_len = 0;
    g_emit_freq = 0;
    /* Every test starts from a disabled gate: the module is default-off, and
     * disable() is the deterministic reset (no init entry point, no NVS). */
    phy_passthrough_disable();
    phy_passthrough_set_emit(capture_emit);
}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/*  Clause: disabled by default                                       */
/* ------------------------------------------------------------------ */

void test_default_off(void) {
    phy_passthrough_status_t st;
    phy_passthrough_get_status(&st);
    TEST_ASSERT_FALSE(st.enabled);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_EQUAL_UINT32(0, st.remaining_s);
    TEST_ASSERT_FALSE(phy_passthrough_is_active());

    /* The RX tap is inert while off: no frame leaves the node. */
    radio_rx_info_t info = {.rssi = -70, .snr = 5, .len = 3};
    uint8_t frame[] = {1, 2, 3};
    phy_passthrough_forward_rx(frame, sizeof(frame), &info, 915000000u);
    TEST_ASSERT_EQUAL_INT(0, g_emit_calls);
}

/* ------------------------------------------------------------------ */
/*  Clause: enable (no identity) activates and sets the window        */
/* ------------------------------------------------------------------ */

void test_enable_activates_default_ttl(void) {
    TEST_ASSERT_EQUAL_INT(PHY_PT_OK, phy_passthrough_enable(0, false, false));
    TEST_ASSERT_TRUE(phy_passthrough_is_active());

    phy_passthrough_status_t st;
    phy_passthrough_get_status(&st);
    TEST_ASSERT_TRUE(st.enabled);
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_FALSE(st.forced);
    TEST_ASSERT_EQUAL_UINT32(PHY_PT_DEFAULT_TTL_S, st.ttl_s);
    TEST_ASSERT_EQUAL_UINT32(PHY_PT_DEFAULT_TTL_S, st.remaining_s);
}

/* ------------------------------------------------------------------ */
/*  Clause: auto-expiry (TX is gated on is_active in handle_phy_tx)   */
/* ------------------------------------------------------------------ */

void test_ttl_expiry_deactivates(void) {
    TEST_ASSERT_EQUAL_INT(PHY_PT_OK, phy_passthrough_enable(30, false, false));
    TEST_ASSERT_TRUE(phy_passthrough_is_active());

    advance_sec(29);
    TEST_ASSERT_TRUE(phy_passthrough_is_active()); /* still inside the window */

    advance_sec(2); /* 31 s: window elapsed */
    TEST_ASSERT_FALSE(phy_passthrough_is_active());

    phy_passthrough_status_t st;
    phy_passthrough_get_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_EQUAL_UINT32(0, st.remaining_s);

    /* The RX tap is inert once expired, too. */
    radio_rx_info_t info = {.rssi = -70, .snr = 5, .len = 1};
    uint8_t frame[] = {9};
    phy_passthrough_forward_rx(frame, 1, &info, 915000000u);
    TEST_ASSERT_EQUAL_INT(0, g_emit_calls);

    /* Re-enabling restores the window. */
    TEST_ASSERT_EQUAL_INT(PHY_PT_OK, phy_passthrough_enable(30, false, false));
    TEST_ASSERT_TRUE(phy_passthrough_is_active());
}

void test_ttl_clamped_to_max(void) {
    TEST_ASSERT_EQUAL_INT(PHY_PT_OK, phy_passthrough_enable(PHY_PT_MAX_TTL_S * 10u, false, false));
    phy_passthrough_status_t st;
    phy_passthrough_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(PHY_PT_MAX_TTL_S, st.ttl_s);
    TEST_ASSERT_EQUAL_UINT32(PHY_PT_MAX_TTL_S, st.remaining_s);
}

void test_remaining_counts_down(void) {
    phy_passthrough_enable(100, false, false);
    advance_sec(40);
    phy_passthrough_status_t st;
    phy_passthrough_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(60, st.remaining_s);
}

/* ------------------------------------------------------------------ */
/*  Clause: refuse while holding a live identity, unless forced       */
/* ------------------------------------------------------------------ */

void test_identity_refuses(void) {
    TEST_ASSERT_EQUAL_INT(PHY_PT_ERR_IDENTITY, phy_passthrough_enable(0, false, true));
    TEST_ASSERT_FALSE(phy_passthrough_is_active());

    /* Nothing was enabled, so the RX tap stays inert. */
    radio_rx_info_t info = {.rssi = -70, .snr = 5, .len = 1};
    uint8_t frame[] = {9};
    phy_passthrough_forward_rx(frame, 1, &info, 915000000u);
    TEST_ASSERT_EQUAL_INT(0, g_emit_calls);
}

void test_force_overrides_identity(void) {
    TEST_ASSERT_EQUAL_INT(PHY_PT_OK, phy_passthrough_enable(0, true, true));
    TEST_ASSERT_TRUE(phy_passthrough_is_active());
    phy_passthrough_status_t st;
    phy_passthrough_get_status(&st);
    TEST_ASSERT_TRUE(st.forced);
}

/* ------------------------------------------------------------------ */
/*  Clause: disable is immediate                                       */
/* ------------------------------------------------------------------ */

void test_disable_deactivates(void) {
    phy_passthrough_enable(600, false, false);
    TEST_ASSERT_TRUE(phy_passthrough_is_active());
    phy_passthrough_disable();
    TEST_ASSERT_FALSE(phy_passthrough_is_active());
    phy_passthrough_status_t st;
    phy_passthrough_get_status(&st);
    TEST_ASSERT_FALSE(st.enabled);
    TEST_ASSERT_EQUAL_UINT32(0, st.remaining_s);
}

/* ------------------------------------------------------------------ */
/*  Clause: enable ONLY via an authenticated RPC                      */
/*  The phy.* methods must not be on the unauthenticated allowlist,   */
/*  so an unauthenticated WS/BLE caller is refused by the same policy */
/*  the dispatcher enforces before any handler runs.                  */
/* ------------------------------------------------------------------ */

void test_auth_policy_rejects_unauth(void) {
    const char* privileged[] = {"phy.enable", "phy.disable", "phy.status", "phy.tx"};
    for (size_t i = 0; i < sizeof(privileged) / sizeof(privileged[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(rpc_auth_method_allowed(privileged[i], false), privileged[i]);
        TEST_ASSERT_TRUE_MESSAGE(rpc_auth_method_allowed(privileged[i], true), privileged[i]);
    }
    /* Sanity: the identification allowlist is unchanged and still open. */
    TEST_ASSERT_TRUE(rpc_auth_method_allowed("bramble.ping", false));
    TEST_ASSERT_TRUE(rpc_auth_method_allowed("bramble.getVersion", false));
}

/* ------------------------------------------------------------------ */
/*  RX tap: frame round-trips intact through the forward, only active  */
/* ------------------------------------------------------------------ */

void test_forward_rx_when_active(void) {
    phy_passthrough_enable(60, false, false);
    radio_rx_info_t info = {.rssi = -95, .snr = -3, .len = 6};
    uint8_t frame[] = {0x00, 0x01, 0x7f, 0x80, 0xff, 0x42};
    phy_passthrough_forward_rx(frame, sizeof(frame), &info, 914500000u);

    TEST_ASSERT_EQUAL_INT(1, g_emit_calls);
    TEST_ASSERT_EQUAL_UINT8(sizeof(frame), g_emit_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, g_emit_buf, sizeof(frame));
    TEST_ASSERT_EQUAL_INT16(-95, g_emit_rssi);
    TEST_ASSERT_EQUAL_INT8(-3, g_emit_snr);
    TEST_ASSERT_EQUAL_UINT32(914500000u, g_emit_freq);
}

void test_forward_rx_no_hook_is_safe(void) {
    phy_passthrough_set_emit(NULL); /* no transport registered */
    phy_passthrough_enable(60, false, false);
    radio_rx_info_t info = {.rssi = -70, .snr = 5, .len = 1};
    uint8_t frame[] = {7};
    phy_passthrough_forward_rx(frame, 1, &info, 915000000u); /* must not crash */
    TEST_ASSERT_EQUAL_INT(0, g_emit_calls);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_off);
    RUN_TEST(test_enable_activates_default_ttl);
    RUN_TEST(test_ttl_expiry_deactivates);
    RUN_TEST(test_ttl_clamped_to_max);
    RUN_TEST(test_remaining_counts_down);
    RUN_TEST(test_identity_refuses);
    RUN_TEST(test_force_overrides_identity);
    RUN_TEST(test_disable_deactivates);
    RUN_TEST(test_auth_policy_rejects_unauth);
    RUN_TEST(test_forward_rx_when_active);
    RUN_TEST(test_forward_rx_no_hook_is_safe);
    return UNITY_END();
}
