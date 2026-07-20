#include "unity.h"

#include <stdbool.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "ota_rollback.h"
#include "ota_rollback_policy.h"

/* ── Controllable stubs for the advanced esp_https_ota API ─────────── */

static esp_err_t g_begin_result;
static esp_err_t g_img_desc_result;
static const char* g_img_desc_version;
static esp_err_t g_perform_result;
static bool g_complete_data;
static esp_err_t g_finish_result;
static int g_begin_calls;
static int g_finish_calls;
static int g_abort_calls;
static esp_http_client_config_t g_last_https_http_cfg;

static const esp_partition_t g_running = {.label = "factory"};
static const esp_app_desc_t g_app_desc = {.version = "1.2.3"};

void* esp_crt_bundle_attach = (void*)0xCAFE;

esp_err_t esp_https_ota(const esp_https_ota_config_t* ota_config) {
    (void)ota_config;
    return ESP_FAIL; /* legacy one-shot API must no longer be used */
}

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t* ota_config,
                              esp_https_ota_handle_t* handle) {
    g_begin_calls++;
    g_last_https_http_cfg = *ota_config->http_config;
    *handle = (esp_https_ota_handle_t)0x1;
    return g_begin_result;
}
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle, esp_app_desc_t* out) {
    (void)handle;
    out->version = g_img_desc_version;
    out->secure_version = 0;
    return g_img_desc_result;
}
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle) {
    (void)handle;
    return g_perform_result;
}
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle) {
    (void)handle;
    return g_complete_data;
}
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle) {
    (void)handle;
    g_finish_calls++;
    return g_finish_result;
}
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle) {
    (void)handle;
    g_abort_calls++;
    return ESP_OK;
}
int esp_https_ota_get_image_size(esp_https_ota_handle_t handle) {
    (void)handle;
    return 0; /* Task 1 progress reporting; not asserted by these tests */
}
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t handle) {
    (void)handle;
    return 0; /* Task 1 progress reporting; not asserted by these tests */
}

/* ── Anti-rollback gate stub ───────────────────────────────────────────
 *
 * ota_rollback.c itself is device-only (it reads and writes the NVS floor),
 * so ota.c's call into the gate is still exercised through this stub. The
 * decision logic the gate delegates to lives in ota_rollback_policy.c, which
 * is pure and is linked and tested for real below. */

static int g_gate_result;
static char g_gate_version[64];
static uint32_t g_gate_secure_version;
static bool g_gate_allow_downgrade;
static int g_gate_calls;

int ota_rollback_gate(const char* new_version, uint32_t candidate_secure_version,
                      bool allow_downgrade) {
    g_gate_calls++;
    snprintf(g_gate_version, sizeof(g_gate_version), "%s", new_version ? new_version : "");
    g_gate_secure_version = candidate_secure_version;
    g_gate_allow_downgrade = allow_downgrade;
    return g_gate_result;
}

/* HTTP path is compile-time disabled in test build; keep minimal stubs anyway */
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    (void)config;
    return NULL;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len) {
    (void)client;
    (void)write_len;
    return ESP_FAIL;
}
int esp_http_client_fetch_headers(esp_http_client_handle_t client) {
    (void)client;
    return -1;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) {
    (void)client;
    return 500;
}
int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len) {
    (void)client;
    (void)buffer;
    (void)len;
    return -1;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) {
    (void)client;
    return false;
}
void esp_http_client_close(esp_http_client_handle_t client) { (void)client; }
void esp_http_client_cleanup(esp_http_client_handle_t client) { (void)client; }

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* start_from) {
    (void)start_from;
    return NULL;
}
esp_err_t esp_ota_begin(const esp_partition_t* partition, int image_size,
                        esp_ota_handle_t* out_handle) {
    (void)partition;
    (void)image_size;
    (void)out_handle;
    return ESP_FAIL;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* data, int size) {
    (void)handle;
    (void)data;
    (void)size;
    return ESP_FAIL;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) {
    (void)handle;
    return ESP_FAIL;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) {
    (void)partition;
    return ESP_FAIL;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle) {
    (void)handle;
    return ESP_FAIL;
}
esp_err_t esp_ota_get_partition_description(const esp_partition_t* partition,
                                            esp_app_desc_t* app_desc) {
    (void)partition;
    (void)app_desc;
    return ESP_FAIL;
}
const esp_partition_t* esp_ota_get_running_partition(void) { return &g_running; }
const char* esp_err_to_name(esp_err_t err) {
    (void)err;
    return "ERR";
}
const esp_app_desc_t* esp_app_get_description(void) { return &g_app_desc; }

#include "../components/ota/ota.c"

void setUp(void) {
    g_begin_result = ESP_OK;
    g_img_desc_result = ESP_OK;
    g_img_desc_version = "1.3.0";
    g_perform_result = ESP_OK;
    g_complete_data = true;
    g_finish_result = ESP_OK;
    g_begin_calls = 0;
    g_finish_calls = 0;
    g_abort_calls = 0;
    g_gate_result = 0;
    g_gate_version[0] = '\0';
    g_gate_secure_version = 0;
    g_gate_allow_downgrade = false;
    g_gate_calls = 0;
    memset(&g_last_https_http_cfg, 0, sizeof(g_last_https_http_cfg));
    s_last_error[0] = '\0';
}

void tearDown(void) {}

void test_https_url_routes_to_https_ota_with_hardening_flags(void) {
    TEST_ASSERT_EQUAL_INT(0, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, g_finish_calls);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", g_last_https_http_cfg.url);
    TEST_ASSERT_FALSE(g_last_https_http_cfg.skip_cert_common_name_check);
    TEST_ASSERT_EQUAL_PTR(esp_crt_bundle_attach, g_last_https_http_cfg.crt_bundle_attach);
}

void test_empty_and_unknown_scheme_are_rejected(void) {
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("", false));
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("ftp://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(0, g_begin_calls);
}

void test_http_is_rejected_when_allow_http_not_defined(void) {
#ifdef CONFIG_BRAMBLE_OTA_ALLOW_HTTP
    TEST_IGNORE_MESSAGE("Test requires CONFIG_BRAMBLE_OTA_ALLOW_HTTP to be undefined");
#endif
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("http://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(0, g_begin_calls);
}

void test_rollback_gate_sees_image_version_and_downgrade_flag(void) {
    g_img_desc_version = "1.9.9";
    TEST_ASSERT_EQUAL_INT(0, ota_wifi_start("https://example.com/fw.bin", true));
    TEST_ASSERT_EQUAL_INT(1, g_gate_calls);
    TEST_ASSERT_EQUAL_STRING("1.9.9", g_gate_version);
    TEST_ASSERT_TRUE(g_gate_allow_downgrade);
}

void test_rollback_gate_rejection_aborts_before_download(void) {
    g_gate_result = -1;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_abort_calls);
    TEST_ASSERT_EQUAL_INT(0, g_finish_calls);
    TEST_ASSERT_NOT_NULL(ota_get_last_error());
    TEST_ASSERT_NOT_NULL(strstr(ota_get_last_error(), "anti-rollback"));
}

void test_signature_validation_failure_fails_closed_with_clear_error(void) {
    g_finish_result = ESP_ERR_OTA_VALIDATE_FAILED;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_NOT_NULL(ota_get_last_error());
    TEST_ASSERT_NOT_NULL(strstr(ota_get_last_error(), "signature"));
}

void test_incomplete_download_aborts(void) {
    g_complete_data = false;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_abort_calls);
    TEST_ASSERT_EQUAL_INT(0, g_finish_calls);
}

void test_unreadable_image_description_aborts(void) {
    g_img_desc_result = ESP_FAIL;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_abort_calls);
    TEST_ASSERT_EQUAL_INT(0, g_gate_calls);
}

void test_https_failure_propagates_error(void) {
    g_begin_result = ESP_FAIL;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, g_finish_calls);
}

void test_partition_and_version_helpers(void) {
    TEST_ASSERT_EQUAL_STRING("factory", ota_get_running_partition());
    TEST_ASSERT_EQUAL_STRING("1.2.3", ota_get_app_version());
}

/* ── Real anti-rollback policy (ota_rollback_policy.c, linked for real) ──
 *
 * These call the same ota_rollback_decide() the device gate calls. Everything
 * the gate adds on top is NVS I/O; the accept/reject decision is entirely
 * here, so an off-by-one in the floor comparison is caught here.
 *
 * The off-by-one shapes these are written to catch:
 *   - `>` instead of `>=`: a reinstall of the exact floor version would be
 *     rejected, bricking the repair path. Caught by the equal-version tests.
 *   - `>=` instead of `>` on the note_boot side: every boot would rewrite the
 *     NVS floor. Caught by should_raise_floor_is_false_for_equal_version.
 *   - swapped comparison arguments: equal still passes, so the strictly-newer
 *     and strictly-older pairs are what catch the inversion.
 *   - a one-unit step at the floor in each component (patch, minor, major)
 *     and across a rollover (1.9.9 vs 2.0.0), where a component-wise
 *     comparison that stops early goes wrong.
 *   - the prerelease boundary, where 1.4.0-rc.1 must rank BELOW 1.4.0. */

static void assert_decides(const char* candidate, const char* floor,
                           ota_rollback_decision_t expected) {
    char msg[160];
    snprintf(msg, sizeof(msg), "candidate=%s floor=%s", candidate ? candidate : "(null)",
             floor ? floor : "(none)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, ota_rollback_decide(candidate, floor, false), msg);
}

void test_policy_equal_version_is_accepted(void) {
    /* Reinstalling the exact floor version is a legitimate repair path. A
     * `>` where the code needs `>=` rejects this. */
    assert_decides("1.4.0", "1.4.0", OTA_ROLLBACK_ACCEPT);
    assert_decides("0.0.0", "0.0.0", OTA_ROLLBACK_ACCEPT);
    assert_decides("2.10.7", "2.10.7", OTA_ROLLBACK_ACCEPT);
    /* Same version, different spellings that must still compare equal. */
    assert_decides("v1.4.0", "1.4.0", OTA_ROLLBACK_ACCEPT);
    assert_decides("1.4.0+build.9", "1.4.0", OTA_ROLLBACK_ACCEPT);
    assert_decides("1.4.0-rc.1", "1.4.0-rc.1", OTA_ROLLBACK_ACCEPT);
}

void test_policy_strictly_newer_is_accepted(void) {
    assert_decides("1.4.1", "1.4.0", OTA_ROLLBACK_ACCEPT);
    assert_decides("1.5.0", "1.4.0", OTA_ROLLBACK_ACCEPT);
    assert_decides("2.0.0", "1.4.0", OTA_ROLLBACK_ACCEPT);
    assert_decides("1.4.0", "1.3.99", OTA_ROLLBACK_ACCEPT);
}

void test_policy_strictly_older_is_rejected(void) {
    assert_decides("1.3.99", "1.4.0", OTA_ROLLBACK_REJECT_BELOW_FLOOR);
    assert_decides("1.4.0", "1.5.0", OTA_ROLLBACK_REJECT_BELOW_FLOOR);
    assert_decides("1.4.0", "2.0.0", OTA_ROLLBACK_REJECT_BELOW_FLOOR);
    assert_decides("0.9.9", "1.0.0", OTA_ROLLBACK_REJECT_BELOW_FLOOR);
}

void test_policy_one_step_each_side_of_the_floor(void) {
    /* One unit below, at, and one unit above the floor, in each component.
     * A comparison off by one in any component shows up as exactly one of
     * these three flipping. */
    assert_decides("1.4.3", "1.4.4", OTA_ROLLBACK_REJECT_BELOW_FLOOR); /* patch -1 */
    assert_decides("1.4.4", "1.4.4", OTA_ROLLBACK_ACCEPT);             /* patch  0 */
    assert_decides("1.4.5", "1.4.4", OTA_ROLLBACK_ACCEPT);             /* patch +1 */

    assert_decides("1.3.4", "1.4.4", OTA_ROLLBACK_REJECT_BELOW_FLOOR); /* minor -1 */
    assert_decides("1.5.4", "1.4.4", OTA_ROLLBACK_ACCEPT);             /* minor +1 */

    assert_decides("0.4.4", "1.4.4", OTA_ROLLBACK_REJECT_BELOW_FLOOR); /* major -1 */
    assert_decides("2.4.4", "1.4.4", OTA_ROLLBACK_ACCEPT);             /* major +1 */

    /* Across a rollover, where a higher patch number sits on a lower core. */
    assert_decides("1.9.9", "2.0.0", OTA_ROLLBACK_REJECT_BELOW_FLOOR);
    assert_decides("2.0.0", "1.9.9", OTA_ROLLBACK_ACCEPT);
    assert_decides("1.4.99", "1.5.0", OTA_ROLLBACK_REJECT_BELOW_FLOOR);
    assert_decides("1.5.0", "1.4.99", OTA_ROLLBACK_ACCEPT);
}

void test_policy_prerelease_ranks_below_its_release(void) {
    /* 1.4.0-rc.1 precedes 1.4.0: shipping the release candidate over the
     * release is a downgrade and must be rejected. */
    assert_decides("1.4.0-rc.1", "1.4.0", OTA_ROLLBACK_REJECT_BELOW_FLOOR);
    assert_decides("1.4.0", "1.4.0-rc.1", OTA_ROLLBACK_ACCEPT);
    assert_decides("1.4.0-rc.1", "1.4.0-rc.2", OTA_ROLLBACK_REJECT_BELOW_FLOOR);
    assert_decides("1.4.0-rc.2", "1.4.0-rc.1", OTA_ROLLBACK_ACCEPT);
}

void test_policy_accepts_when_no_usable_floor_is_stored(void) {
    assert_decides("1.4.0", NULL, OTA_ROLLBACK_ACCEPT);
    assert_decides("0.0.1", NULL, OTA_ROLLBACK_ACCEPT);
    /* A floor that does not parse means "not set yet", not "reject all". */
    assert_decides("1.4.0", "", OTA_ROLLBACK_ACCEPT);
    assert_decides("1.4.0", "garbage", OTA_ROLLBACK_ACCEPT);
}

void test_policy_unparseable_candidate_fails_closed(void) {
    assert_decides(NULL, "1.4.0", OTA_ROLLBACK_REJECT_UNPARSEABLE);
    assert_decides("", "1.4.0", OTA_ROLLBACK_REJECT_UNPARSEABLE);
    assert_decides("not-a-version", "1.4.0", OTA_ROLLBACK_REJECT_UNPARSEABLE);
    assert_decides("1.4", "1.4.0", OTA_ROLLBACK_REJECT_UNPARSEABLE);
    /* Fails closed even with no floor stored at all. */
    assert_decides("nonsense", NULL, OTA_ROLLBACK_REJECT_UNPARSEABLE);
}

void test_policy_allow_downgrade_lowers_the_floor_only_when_below_it(void) {
    /* Below the floor: accepted, and the floor moves down with it. */
    TEST_ASSERT_EQUAL_INT(OTA_ROLLBACK_ACCEPT_LOWER_FLOOR,
                          ota_rollback_decide("1.3.9", "1.4.0", true));
    /* At or above the floor: accepted with the floor left alone. Getting
     * this wrong rewrites NVS on every ordinary update. */
    TEST_ASSERT_EQUAL_INT(OTA_ROLLBACK_ACCEPT, ota_rollback_decide("1.4.0", "1.4.0", true));
    TEST_ASSERT_EQUAL_INT(OTA_ROLLBACK_ACCEPT, ota_rollback_decide("1.4.1", "1.4.0", true));
    /* Unparseable candidate is accepted under allow_downgrade, but there is
     * no version to lower the floor to. */
    TEST_ASSERT_EQUAL_INT(OTA_ROLLBACK_ACCEPT_UNPARSEABLE,
                          ota_rollback_decide("garbage", "1.4.0", true));
    TEST_ASSERT_EQUAL_INT(OTA_ROLLBACK_ACCEPT_UNPARSEABLE,
                          ota_rollback_decide(NULL, "1.4.0", true));
}

void test_policy_accept_predicate_matches_the_decisions(void) {
    TEST_ASSERT_TRUE(ota_rollback_decision_accepts(OTA_ROLLBACK_ACCEPT));
    TEST_ASSERT_TRUE(ota_rollback_decision_accepts(OTA_ROLLBACK_ACCEPT_UNPARSEABLE));
    TEST_ASSERT_TRUE(ota_rollback_decision_accepts(OTA_ROLLBACK_ACCEPT_LOWER_FLOOR));
    TEST_ASSERT_FALSE(ota_rollback_decision_accepts(OTA_ROLLBACK_REJECT_UNPARSEABLE));
    TEST_ASSERT_FALSE(ota_rollback_decision_accepts(OTA_ROLLBACK_REJECT_BELOW_FLOOR));
}

void test_should_raise_floor_only_for_a_strictly_higher_running_version(void) {
    /* Equal must NOT raise: a `>=` here rewrites NVS on every single boot. */
    TEST_ASSERT_FALSE(ota_rollback_should_raise_floor("1.4.0", "1.4.0"));
    TEST_ASSERT_FALSE(ota_rollback_should_raise_floor("1.4.0", "1.4.1"));
    TEST_ASSERT_FALSE(ota_rollback_should_raise_floor("1.4.0", "2.0.0"));
    TEST_ASSERT_TRUE(ota_rollback_should_raise_floor("1.4.1", "1.4.0"));
    TEST_ASSERT_TRUE(ota_rollback_should_raise_floor("2.0.0", "1.9.9"));
    /* Release outranks its own prerelease, so booting it raises the floor. */
    TEST_ASSERT_TRUE(ota_rollback_should_raise_floor("1.4.0", "1.4.0-rc.1"));
    TEST_ASSERT_FALSE(ota_rollback_should_raise_floor("1.4.0-rc.1", "1.4.0"));
    /* No usable floor stored: record the running version. */
    TEST_ASSERT_TRUE(ota_rollback_should_raise_floor("1.4.0", NULL));
    TEST_ASSERT_TRUE(ota_rollback_should_raise_floor("1.4.0", "garbage"));
    /* An unparseable running version never touches the floor. */
    TEST_ASSERT_FALSE(ota_rollback_should_raise_floor("dev-build", "1.4.0"));
    TEST_ASSERT_FALSE(ota_rollback_should_raise_floor(NULL, "1.4.0"));
    TEST_ASSERT_FALSE(ota_rollback_should_raise_floor("dev-build", NULL));
}

/* ── Hardware (eFuse) anti-rollback floor (ota_rollback_secure_floor_blocks) ──
 *
 * This pure helper is the reconciliation point between the two floors: it says
 * whether the hardware floor alone forces a rejection. The device gate calls
 * it BEFORE the soft-floor decision and, unlike the soft floor, its rejection
 * is never overridable by allow_downgrade, because a sub-floor image would be
 * refused by the bootloader and brick the device. */

void test_secure_floor_ignored_when_enforcement_not_compiled_in(void) {
    /* Not enforced: the clear flag is irrelevant, nothing is blocked, so the
     * decision is exactly the historical soft-floor-only behavior. */
    TEST_ASSERT_FALSE(ota_rollback_secure_floor_blocks(false, false));
    TEST_ASSERT_FALSE(ota_rollback_secure_floor_blocks(false, true));
}

void test_secure_floor_blocks_only_a_sub_floor_image_when_enforced(void) {
    /* Enforced and the image clears the eFuse floor: allowed through to the
     * soft-floor check. */
    TEST_ASSERT_FALSE(ota_rollback_secure_floor_blocks(true, true));
    /* Enforced and the image is below the eFuse floor: blocked, absolutely.
     * There is no allow_downgrade parameter here by design: the caller cannot
     * override this, matching the bootloader that would refuse to boot it. */
    TEST_ASSERT_TRUE(ota_rollback_secure_floor_blocks(true, false));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_https_url_routes_to_https_ota_with_hardening_flags);
    RUN_TEST(test_empty_and_unknown_scheme_are_rejected);
    RUN_TEST(test_http_is_rejected_when_allow_http_not_defined);
    RUN_TEST(test_rollback_gate_sees_image_version_and_downgrade_flag);
    RUN_TEST(test_rollback_gate_rejection_aborts_before_download);
    RUN_TEST(test_signature_validation_failure_fails_closed_with_clear_error);
    RUN_TEST(test_incomplete_download_aborts);
    RUN_TEST(test_unreadable_image_description_aborts);
    RUN_TEST(test_https_failure_propagates_error);
    RUN_TEST(test_partition_and_version_helpers);
    RUN_TEST(test_policy_equal_version_is_accepted);
    RUN_TEST(test_policy_strictly_newer_is_accepted);
    RUN_TEST(test_policy_strictly_older_is_rejected);
    RUN_TEST(test_policy_one_step_each_side_of_the_floor);
    RUN_TEST(test_policy_prerelease_ranks_below_its_release);
    RUN_TEST(test_policy_accepts_when_no_usable_floor_is_stored);
    RUN_TEST(test_policy_unparseable_candidate_fails_closed);
    RUN_TEST(test_policy_allow_downgrade_lowers_the_floor_only_when_below_it);
    RUN_TEST(test_policy_accept_predicate_matches_the_decisions);
    RUN_TEST(test_should_raise_floor_only_for_a_strictly_higher_running_version);
    RUN_TEST(test_secure_floor_ignored_when_enforcement_not_compiled_in);
    RUN_TEST(test_secure_floor_blocks_only_a_sub_floor_image_when_enforced);
    return UNITY_END();
}
