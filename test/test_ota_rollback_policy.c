#include "ota_rollback_policy.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Base input: no floors, no hardware enforcement. Individual tests override
 * only the fields they exercise. */
static ota_gate_input_t base(void) {
    ota_gate_input_t in = {
        .candidate_version = "1.4.0",
        .has_soft_floor = false,
        .soft_floor_version = NULL,
        .allow_downgrade = false,
        .secure_enforced = false,
        .candidate_clears_secure_floor = true,
    };
    return in;
}

/* ── Soft floor only (hardware anti-rollback not compiled in) ──────── */

void test_no_floor_accepts(void) {
    ota_gate_input_t in = base();
    TEST_ASSERT_EQUAL_INT(OTA_GATE_ACCEPT, ota_rollback_decide(&in));
}

void test_at_or_above_soft_floor_accepts(void) {
    ota_gate_input_t in = base();
    in.has_soft_floor = true;
    in.soft_floor_version = "1.4.0";
    in.candidate_version = "1.4.0"; /* equal */
    TEST_ASSERT_EQUAL_INT(OTA_GATE_ACCEPT, ota_rollback_decide(&in));
    in.candidate_version = "1.5.0"; /* above */
    TEST_ASSERT_EQUAL_INT(OTA_GATE_ACCEPT, ota_rollback_decide(&in));
}

void test_below_soft_floor_rejected_without_override(void) {
    ota_gate_input_t in = base();
    in.has_soft_floor = true;
    in.soft_floor_version = "1.4.0";
    in.candidate_version = "1.3.9";
    TEST_ASSERT_EQUAL_INT(OTA_GATE_REJECT_BELOW_SOFT_FLOOR, ota_rollback_decide(&in));
}

void test_below_soft_floor_allowed_with_override_lowers_floor(void) {
    ota_gate_input_t in = base();
    in.has_soft_floor = true;
    in.soft_floor_version = "1.4.0";
    in.candidate_version = "1.3.9";
    in.allow_downgrade = true;
    TEST_ASSERT_EQUAL_INT(OTA_GATE_ACCEPT_DOWNGRADE, ota_rollback_decide(&in));
}

void test_unparseable_version_fails_closed(void) {
    ota_gate_input_t in = base();
    in.candidate_version = "not-a-version";
    TEST_ASSERT_EQUAL_INT(OTA_GATE_REJECT_UNPARSEABLE, ota_rollback_decide(&in));
    in.candidate_version = NULL;
    TEST_ASSERT_EQUAL_INT(OTA_GATE_REJECT_UNPARSEABLE, ota_rollback_decide(&in));
}

void test_unparseable_version_accepted_with_override_no_floor_write(void) {
    ota_gate_input_t in = base();
    in.candidate_version = "garbage";
    in.allow_downgrade = true;
    /* ACCEPT, not ACCEPT_DOWNGRADE: there is no parseable version to store as
     * the new floor. */
    TEST_ASSERT_EQUAL_INT(OTA_GATE_ACCEPT, ota_rollback_decide(&in));
}

void test_malformed_stored_floor_treated_as_no_floor(void) {
    ota_gate_input_t in = base();
    in.has_soft_floor = true;
    in.soft_floor_version = "corrupt";
    in.candidate_version = "0.0.1";
    TEST_ASSERT_EQUAL_INT(OTA_GATE_ACCEPT, ota_rollback_decide(&in));
}

void test_null_input_rejected(void) {
    TEST_ASSERT_EQUAL_INT(OTA_GATE_REJECT_UNPARSEABLE, ota_rollback_decide(NULL));
}

/* ── Hardware eFuse floor reconciliation ──────────────────────────── */

void test_secure_floor_cleared_accepts(void) {
    ota_gate_input_t in = base();
    in.secure_enforced = true;
    in.candidate_clears_secure_floor = true;
    TEST_ASSERT_EQUAL_INT(OTA_GATE_ACCEPT, ota_rollback_decide(&in));
}

/* The central safety property: an image below the hardware floor is rejected
 * even when the caller explicitly asked for a downgrade, because the bootloader
 * would refuse to boot it and brick the device. */
void test_secure_floor_not_cleared_rejected_even_with_override(void) {
    ota_gate_input_t in = base();
    in.secure_enforced = true;
    in.candidate_clears_secure_floor = false;
    in.allow_downgrade = true;
    TEST_ASSERT_EQUAL_INT(OTA_GATE_REJECT_BELOW_SECURE_FLOOR, ota_rollback_decide(&in));
}

/* The hardware check does not depend on the semver string and takes precedence
 * over the unparseable-version path. */
void test_secure_floor_not_cleared_beats_unparseable(void) {
    ota_gate_input_t in = base();
    in.secure_enforced = true;
    in.candidate_clears_secure_floor = false;
    in.candidate_version = "garbage";
    in.allow_downgrade = true;
    TEST_ASSERT_EQUAL_INT(OTA_GATE_REJECT_BELOW_SECURE_FLOOR, ota_rollback_decide(&in));
}

/* After the hardware floor passes, the soft floor still applies. A semver
 * downgrade within the same secure epoch is rejected unless overridden. */
void test_secure_cleared_but_below_soft_floor(void) {
    ota_gate_input_t in = base();
    in.secure_enforced = true;
    in.candidate_clears_secure_floor = true;
    in.has_soft_floor = true;
    in.soft_floor_version = "1.4.0";
    in.candidate_version = "1.3.0";
    TEST_ASSERT_EQUAL_INT(OTA_GATE_REJECT_BELOW_SOFT_FLOOR, ota_rollback_decide(&in));
    in.allow_downgrade = true;
    TEST_ASSERT_EQUAL_INT(OTA_GATE_ACCEPT_DOWNGRADE, ota_rollback_decide(&in));
}

/* When hardware enforcement is not compiled in, candidate_clears_secure_floor
 * is ignored and behavior matches the soft-floor-only path. */
void test_secure_not_enforced_ignores_clear_flag(void) {
    ota_gate_input_t in = base();
    in.secure_enforced = false;
    in.candidate_clears_secure_floor = false; /* must be ignored */
    TEST_ASSERT_EQUAL_INT(OTA_GATE_ACCEPT, ota_rollback_decide(&in));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_floor_accepts);
    RUN_TEST(test_at_or_above_soft_floor_accepts);
    RUN_TEST(test_below_soft_floor_rejected_without_override);
    RUN_TEST(test_below_soft_floor_allowed_with_override_lowers_floor);
    RUN_TEST(test_unparseable_version_fails_closed);
    RUN_TEST(test_unparseable_version_accepted_with_override_no_floor_write);
    RUN_TEST(test_malformed_stored_floor_treated_as_no_floor);
    RUN_TEST(test_null_input_rejected);
    RUN_TEST(test_secure_floor_cleared_accepts);
    RUN_TEST(test_secure_floor_not_cleared_rejected_even_with_override);
    RUN_TEST(test_secure_floor_not_cleared_beats_unparseable);
    RUN_TEST(test_secure_cleared_but_below_soft_floor);
    RUN_TEST(test_secure_not_enforced_ignores_clear_flag);
    return UNITY_END();
}
