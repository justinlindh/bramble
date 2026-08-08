/*
 * Host coverage for the SX1262 transmit-power path.
 *
 * Two defects motivate this suite. First, nothing clamped a requested power to
 * the chip's own -9..+22 dBm range: the only clamp applied was the regional
 * plan's regulatory ceiling, which is 30 dBm in US915 and AU915, so a value
 * the part does not define could reach SetTxParams. Second, SetPaConfig
 * ignored its power argument and always programmed the +22 dBm operating
 * point, biasing the PA for full output no matter what level was asked for.
 *
 * Both fixes are pure functions in sx1262.h so they can be exercised without
 * the SPI driver, the same way the bandwidth mapping is.
 */

#include "unity.h"

#include <string.h>

#include "radio.h"
#include "sx1262.h"

void setUp(void) {}
void tearDown(void) {}

/* ── Clamping to the chip's range ─────────────────────────────────── */

void test_clamp_passes_through_in_range(void) {
    TEST_ASSERT_EQUAL_INT(22, sx1262_clamp_tx_power(22));
    TEST_ASSERT_EQUAL_INT(14, sx1262_clamp_tx_power(14));
    TEST_ASSERT_EQUAL_INT(0, sx1262_clamp_tx_power(0));
    TEST_ASSERT_EQUAL_INT(-9, sx1262_clamp_tx_power(-9));
}

/* The defect itself: the US915 and AU915 plans clamp to 30 dBm, which is a
 * regulatory limit, not a hardware one. 23..30 must not reach the chip. */
void test_clamp_rejects_regulatory_ceiling_above_chip_max(void) {
    TEST_ASSERT_EQUAL_INT(22, sx1262_clamp_tx_power(30));
    TEST_ASSERT_EQUAL_INT(22, sx1262_clamp_tx_power(23));
    TEST_ASSERT_EQUAL_INT(22, sx1262_clamp_tx_power(127));
}

void test_clamp_rejects_below_chip_min(void) {
    TEST_ASSERT_EQUAL_INT(-9, sx1262_clamp_tx_power(-10));
    TEST_ASSERT_EQUAL_INT(-9, sx1262_clamp_tx_power(-128));
}

void test_clamp_is_idempotent(void) {
    for (int p = -128; p <= 127; p++) {
        int8_t once = sx1262_clamp_tx_power((int8_t)p);
        TEST_ASSERT_EQUAL_INT(once, sx1262_clamp_tx_power(once));
    }
}

/* Whatever the input, the result must be a level the part defines. */
void test_clamp_output_always_within_chip_range(void) {
    for (int p = -128; p <= 127; p++) {
        int8_t c = sx1262_clamp_tx_power((int8_t)p);
        TEST_ASSERT_TRUE(c >= SX1262_TX_POWER_MIN_DBM);
        TEST_ASSERT_TRUE(c <= SX1262_TX_POWER_MAX_DBM);
    }
}

/* ── Generic per-driver clamp ─────────────────────────────────────── */

/* Every backend clamps through this one function, including the bare-metal
 * LR1110 target that carries no host suite of its own, so its behaviour is
 * pinned here rather than three times over. Review of the original change
 * caught two backends storing an unclamped power, which made NVS, the RPC echo
 * and radio_health report a level the radio was never programmed with. */
void test_radio_clamp_respects_arbitrary_ranges(void) {
    /* LR1110: -17..+22 across both PA paths. */
    TEST_ASSERT_EQUAL_INT(22, radio_clamp_tx_power(30, -17, 22));
    TEST_ASSERT_EQUAL_INT(-17, radio_clamp_tx_power(-100, -17, 22));
    TEST_ASSERT_EQUAL_INT(14, radio_clamp_tx_power(14, -17, 22));
    /* SX1262 and the virtual radio: -9..+22. */
    TEST_ASSERT_EQUAL_INT(22, radio_clamp_tx_power(30, -9, 22));
    TEST_ASSERT_EQUAL_INT(-9, radio_clamp_tx_power(-10, -9, 22));
}

void test_radio_clamp_output_always_within_range(void) {
    for (int p = -128; p <= 127; p++) {
        int8_t c = radio_clamp_tx_power((int8_t)p, -17, 22);
        TEST_ASSERT_TRUE(c >= -17);
        TEST_ASSERT_TRUE(c <= 22);
    }
}

/* The chip-specific SX1262 clamp and the generic one must not disagree, or the
 * driver's own last-line-of-defence would contradict the layer above it. */
void test_radio_clamp_agrees_with_sx1262_clamp(void) {
    for (int p = -128; p <= 127; p++) {
        TEST_ASSERT_EQUAL_INT(
            sx1262_clamp_tx_power((int8_t)p),
            radio_clamp_tx_power((int8_t)p, SX1262_TX_POWER_MIN_DBM, SX1262_TX_POWER_MAX_DBM));
    }
}

/* ── PA operating point selection ─────────────────────────────────── */

/* The +22 dBm point must stay byte-for-byte what the driver already programmed,
 * because that is the level the fleet runs at: this change must not alter the
 * default configuration on air. */
void test_op_point_22dbm_matches_datasheet(void) {
    sx1262_pa_op_point_t op = sx1262_pa_op_point_for(22);
    TEST_ASSERT_EQUAL_HEX8(0x04, op.pa_duty_cycle);
    TEST_ASSERT_EQUAL_HEX8(0x07, op.hp_max);
    TEST_ASSERT_EQUAL_INT(22, op.rated_dbm);
}

void test_op_point_20dbm_matches_datasheet(void) {
    sx1262_pa_op_point_t op = sx1262_pa_op_point_for(20);
    TEST_ASSERT_EQUAL_HEX8(0x03, op.pa_duty_cycle);
    TEST_ASSERT_EQUAL_HEX8(0x05, op.hp_max);
    TEST_ASSERT_EQUAL_INT(20, op.rated_dbm);
}

void test_op_point_17dbm_matches_datasheet(void) {
    sx1262_pa_op_point_t op = sx1262_pa_op_point_for(17);
    TEST_ASSERT_EQUAL_HEX8(0x02, op.pa_duty_cycle);
    TEST_ASSERT_EQUAL_HEX8(0x03, op.hp_max);
    TEST_ASSERT_EQUAL_INT(17, op.rated_dbm);
}

void test_op_point_14dbm_matches_datasheet(void) {
    sx1262_pa_op_point_t op = sx1262_pa_op_point_for(14);
    TEST_ASSERT_EQUAL_HEX8(0x02, op.pa_duty_cycle);
    TEST_ASSERT_EQUAL_HEX8(0x02, op.hp_max);
    TEST_ASSERT_EQUAL_INT(14, op.rated_dbm);
}

/* The selected point must always be able to reach the level requested;
 * picking a point rated below the request would cap output silently. */
void test_op_point_never_rated_below_request(void) {
    for (int p = SX1262_TX_POWER_MIN_DBM; p <= SX1262_TX_POWER_MAX_DBM; p++) {
        sx1262_pa_op_point_t op = sx1262_pa_op_point_for((int8_t)p);
        if (p > 14) {
            TEST_ASSERT_TRUE_MESSAGE(op.rated_dbm >= p, "PA point cannot reach requested power");
        }
    }
}

/* And it must be the lowest such point, so the PA is not biased for +22 dBm
 * when a far lower level was asked for: that was the original defect. */
void test_op_point_picks_lowest_point_that_covers(void) {
    TEST_ASSERT_EQUAL_INT(22, sx1262_pa_op_point_for(21).rated_dbm);
    TEST_ASSERT_EQUAL_INT(20, sx1262_pa_op_point_for(18).rated_dbm);
    TEST_ASSERT_EQUAL_INT(17, sx1262_pa_op_point_for(15).rated_dbm);
    TEST_ASSERT_EQUAL_INT(14, sx1262_pa_op_point_for(14).rated_dbm);
}

/* Levels at or below the lowest characterized point share its bias, with
 * SetTxParams carrying the rest of the reduction. */
void test_op_point_low_levels_use_bottom_point(void) {
    for (int p = SX1262_TX_POWER_MIN_DBM; p <= 14; p++) {
        sx1262_pa_op_point_t op = sx1262_pa_op_point_for((int8_t)p);
        TEST_ASSERT_EQUAL_HEX8(0x02, op.pa_duty_cycle);
        TEST_ASSERT_EQUAL_HEX8(0x02, op.hp_max);
    }
}

/* Selection must be monotonic: raising the requested power can never move the
 * PA to a weaker bias. */
void test_op_point_monotonic_in_request(void) {
    uint8_t prev_duty = 0, prev_hp = 0;
    for (int p = SX1262_TX_POWER_MIN_DBM; p <= SX1262_TX_POWER_MAX_DBM; p++) {
        sx1262_pa_op_point_t op = sx1262_pa_op_point_for((int8_t)p);
        TEST_ASSERT_TRUE(op.pa_duty_cycle >= prev_duty);
        TEST_ASSERT_TRUE(op.hp_max >= prev_hp);
        prev_duty = op.pa_duty_cycle;
        prev_hp = op.hp_max;
    }
}

/* ── Device error decoding ────────────────────────────────────────── */

void test_device_errors_none(void) {
    char buf[96];
    TEST_ASSERT_EQUAL_STRING("none", sx1262_device_errors_str(0, buf, sizeof(buf)));
}

/* PA_RAMP is the flag that speaks to output power, so it must decode by name
 * and must lead: a log reader scanning for it should not have to parse a mask. */
void test_device_errors_pa_ramp_named_first(void) {
    char buf[96];
    TEST_ASSERT_EQUAL_STRING("PA_RAMP",
                             sx1262_device_errors_str(SX1262_DEVERR_PA_RAMP, buf, sizeof(buf)));
}

void test_device_errors_multiple_flags(void) {
    char buf[96];
    TEST_ASSERT_EQUAL_STRING("PA_RAMP PLL_LOCK XOSC_START",
                             sx1262_device_errors_str(SX1262_DEVERR_PA_RAMP |
                                                          SX1262_DEVERR_PLL_LOCK |
                                                          SX1262_DEVERR_XOSC_START,
                                                      buf, sizeof(buf)));
}

void test_device_errors_all_flags_decode(void) {
    char buf[128];
    sx1262_device_errors_str(SX1262_DEVERR_ALL, buf, sizeof(buf));
    /* Every flag name must appear; a silently dropped one hides a fault. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "PA_RAMP"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "PLL_LOCK"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "XOSC_START"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "IMG_CALIB"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "ADC_CALIB"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "PLL_CALIB"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "RC13M_CALIB"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "RC64K_CALIB"));
}

/* A short buffer must truncate cleanly rather than overrun it. */
void test_device_errors_truncates_safely(void) {
    char buf[8];
    sx1262_device_errors_str(SX1262_DEVERR_ALL, buf, sizeof(buf));
    TEST_ASSERT_TRUE(strlen(buf) < sizeof(buf));
}

void test_device_errors_zero_length_buffer_is_safe(void) {
    char buf[4] = {'x', 'x', 'x', 'x'};
    sx1262_device_errors_str(SX1262_DEVERR_ALL, buf, 0);
    TEST_ASSERT_EQUAL_CHAR('x', buf[0]); /* untouched, no write past the end */
}

/* ── Status byte decoding ─────────────────────────────────────────── */

void test_status_decodes_chip_mode(void) {
    TEST_ASSERT_EQUAL_STRING("TX", sx1262_chip_mode_str(SX1262_MODE_TX << 4));
    TEST_ASSERT_EQUAL_STRING("RX", sx1262_chip_mode_str(SX1262_MODE_RX << 4));
    TEST_ASSERT_EQUAL_STRING("STBY_RC", sx1262_chip_mode_str(SX1262_MODE_STBY_RC << 4));
}

/* A rejected command is how an out-of-range parameter would surface, so these
 * two statuses must be distinguishable by name. */
void test_status_decodes_command_failures(void) {
    TEST_ASSERT_EQUAL_STRING("exec-failed",
                             sx1262_cmd_status_str(SX1262_CMD_STATUS_EXEC_FAIL << 1));
    TEST_ASSERT_EQUAL_STRING("processing-error",
                             sx1262_cmd_status_str(SX1262_CMD_STATUS_PROCESSING_ERR << 1));
    TEST_ASSERT_EQUAL_STRING("tx-done", sx1262_cmd_status_str(SX1262_CMD_STATUS_TX_DONE << 1));
}

/* Mode and command status share one byte and must not bleed into each other. */
void test_status_fields_are_independent(void) {
    uint8_t st = (uint8_t)((SX1262_MODE_TX << 4) | (SX1262_CMD_STATUS_TX_DONE << 1));
    TEST_ASSERT_EQUAL_STRING("TX", sx1262_chip_mode_str(st));
    TEST_ASSERT_EQUAL_STRING("tx-done", sx1262_cmd_status_str(st));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_clamp_passes_through_in_range);
    RUN_TEST(test_clamp_rejects_regulatory_ceiling_above_chip_max);
    RUN_TEST(test_clamp_rejects_below_chip_min);
    RUN_TEST(test_clamp_is_idempotent);
    RUN_TEST(test_clamp_output_always_within_chip_range);

    RUN_TEST(test_radio_clamp_respects_arbitrary_ranges);
    RUN_TEST(test_radio_clamp_output_always_within_range);
    RUN_TEST(test_radio_clamp_agrees_with_sx1262_clamp);

    RUN_TEST(test_op_point_22dbm_matches_datasheet);
    RUN_TEST(test_op_point_20dbm_matches_datasheet);
    RUN_TEST(test_op_point_17dbm_matches_datasheet);
    RUN_TEST(test_op_point_14dbm_matches_datasheet);
    RUN_TEST(test_op_point_never_rated_below_request);
    RUN_TEST(test_op_point_picks_lowest_point_that_covers);
    RUN_TEST(test_op_point_low_levels_use_bottom_point);
    RUN_TEST(test_op_point_monotonic_in_request);

    RUN_TEST(test_device_errors_none);
    RUN_TEST(test_device_errors_pa_ramp_named_first);
    RUN_TEST(test_device_errors_multiple_flags);
    RUN_TEST(test_device_errors_all_flags_decode);
    RUN_TEST(test_device_errors_truncates_safely);
    RUN_TEST(test_device_errors_zero_length_buffer_is_safe);

    RUN_TEST(test_status_decodes_chip_mode);
    RUN_TEST(test_status_decodes_command_failures);
    RUN_TEST(test_status_fields_are_independent);

    return UNITY_END();
}
