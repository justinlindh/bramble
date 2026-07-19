/*
 * Host tests for the SX1262 radio driver's state-reporting invariants.
 *
 * The core invariant: radio_get_state() must never claim a capability the
 * chip does not have. A radio_start_rx() whose SPI commands failed leaves the
 * node deaf, and reporting RADIO_STATE_RX in that situation hid the failure
 * until the stuck-BUSY counter tripped a hard reset.
 *
 * radio_esp.c is normally ESP_PLATFORM-only, so this suite whiteboxes it into
 * one TU behind a fake sx1262 layer that can inject per-command failures
 * (the same pattern test_radio_virt.c uses for radio_virt.c). The FreeRTOS,
 * GPIO and board shims below are only as complete as these tests need: the
 * radio task and the DIO1 ISR are never started, so the notification fakes
 * just have to be consistent, not concurrent.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "unity.h"

/* ------------------------------------------------------------------ */
/*  FreeRTOS / ESP shims not covered by test/stubs                     */
/* ------------------------------------------------------------------ */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define IRAM_ATTR
#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef pdFALSE
#define pdFALSE 0
#endif
#define portMAX_DELAY 0xFFFFFFFFu

/* Task notifications: no radio task runs here, so a give is recorded and a
 * take always finds the counter the give left behind. */
static uint32_t s_notify_count;

static void xTaskNotifyGive(TaskHandle_t task) {
    (void)task;
    s_notify_count++;
}

static uint32_t ulTaskNotifyTake(int clear_on_exit, uint32_t ticks_to_wait) {
    (void)ticks_to_wait;
    uint32_t taken = s_notify_count;
    if (clear_on_exit) {
        s_notify_count = 0;
    } else if (s_notify_count) {
        s_notify_count--;
    }
    return taken;
}

static void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t* woken) {
    (void)task;
    (void)woken;
    s_notify_count++;
}

static void portYIELD_FROM_ISR(void) {}

static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)0xABCD; }

/* GPIO bits radio_esp.c uses that the shared driver/gpio.h stub omits. */
#include "driver/gpio.h"

#define GPIO_INTR_POSEDGE 1
static void gpio_set_intr_type(int pin, int type) {
    (void)pin;
    (void)type;
}
static void gpio_install_isr_service(int flags) { (void)flags; }
static void gpio_isr_handler_add(int pin, void (*fn)(void*), void* arg) {
    (void)pin;
    (void)fn;
    (void)arg;
}
int gpio_get_level(gpio_num_t pin) {
    (void)pin;
    return 0;
}

#include "board_config.h"

static bramble_board_config_t s_board_cfg;
const bramble_board_config_t* board_get_config(void) { return &s_board_cfg; }

/* ------------------------------------------------------------------ */
/*  Fake SX1262 with per-command failure injection                     */
/* ------------------------------------------------------------------ */

#include "sx1262.h"

/* Exactly one command name is armed to fail per test. */
static const char* s_fail_call;
static int s_fail_code;

/* When set, the fake behaves like a radio that really transmits: the TxDone
 * IRQ fires once SetTx has been accepted, and radio_task turns that into a
 * notification. Modelling it inside sx1262_set_tx is what lets a test drive
 * radio_transmit_raw all the way through its ulTaskNotifyTake wait. */
static bool s_simulate_txdone;

/* Defined in radio_esp.c, which is included below. Declared here so the fake
 * SetTx can invoke exactly the path radio_task uses to wake the sender. */
static void wake_tx_waiter(void);

/* Call counters for the commands the assertions care about. */
static int s_calls_set_rx;
static int s_calls_set_tx;
static int s_calls_write_buffer;
static int s_calls_set_packet_params;

static void fake_reset(void) {
    s_fail_call = NULL;
    s_fail_code = 0;
    s_simulate_txdone = false;
    s_calls_set_rx = 0;
    s_calls_set_tx = 0;
    s_calls_write_buffer = 0;
    s_calls_set_packet_params = 0;
    s_notify_count = 0;
}

static void arm_failure(const char* call, int code) {
    s_fail_call = call;
    s_fail_code = code;
}

static int fake_rc(const char* call) {
    if (s_fail_call && strcmp(s_fail_call, call) == 0) {
        return s_fail_code;
    }
    return 0;
}

int sx1262_init(void) { return fake_rc("init"); }
bool sx1262_needs_reinit(void) { return false; }
void sx1262_clear_reinit(void) {}

int sx1262_write_register(uint16_t addr, const uint8_t* data, size_t len) {
    (void)addr;
    (void)data;
    (void)len;
    return fake_rc("write_register");
}

int sx1262_write_buffer(uint8_t offset, const uint8_t* data, size_t len) {
    (void)offset;
    (void)data;
    (void)len;
    s_calls_write_buffer++;
    return fake_rc("write_buffer");
}

int sx1262_read_buffer(uint8_t offset, uint8_t* data, size_t len) {
    (void)offset;
    (void)data;
    (void)len;
    return fake_rc("read_buffer");
}

int sx1262_set_standby(uint8_t mode) {
    (void)mode;
    return fake_rc("set_standby");
}

int sx1262_set_rf_frequency(float freq_mhz) {
    (void)freq_mhz;
    return fake_rc("set_rf_frequency");
}

int sx1262_set_pa_config(int8_t power_dbm) {
    (void)power_dbm;
    return fake_rc("set_pa_config");
}

int sx1262_set_tx_params(int8_t power_dbm, uint8_t ramp_time) {
    (void)power_dbm;
    (void)ramp_time;
    return fake_rc("set_tx_params");
}

int sx1262_set_modulation_params(uint8_t sf, uint8_t bw, uint8_t cr, uint8_t ldro) {
    (void)sf;
    (void)bw;
    (void)cr;
    (void)ldro;
    return fake_rc("set_modulation_params");
}

int sx1262_set_packet_params(uint16_t preamble, uint8_t header_type, uint8_t payload_len,
                             uint8_t crc_on, uint8_t invert_iq) {
    (void)preamble;
    (void)header_type;
    (void)payload_len;
    (void)crc_on;
    (void)invert_iq;
    s_calls_set_packet_params++;
    return fake_rc("set_packet_params");
}

int sx1262_set_dio_irq_params(uint16_t irq_mask, uint16_t dio1_mask, uint16_t dio2_mask,
                              uint16_t dio3_mask) {
    (void)irq_mask;
    (void)dio1_mask;
    (void)dio2_mask;
    (void)dio3_mask;
    return fake_rc("set_dio_irq_params");
}

int sx1262_clear_irq_status(uint16_t mask) {
    (void)mask;
    return fake_rc("clear_irq_status");
}

uint16_t sx1262_get_irq_status(void) { return 0; }

int sx1262_set_tx(uint32_t timeout_ms) {
    (void)timeout_ms;
    s_calls_set_tx++;
    int rc = fake_rc("set_tx");
    if (rc == 0 && s_simulate_txdone) {
        /* Stand in for radio_task handling the TX_DONE IRQ. */
        wake_tx_waiter();
    }
    return rc;
}

int sx1262_set_rx(uint32_t timeout_ms) {
    (void)timeout_ms;
    s_calls_set_rx++;
    return fake_rc("set_rx");
}

int sx1262_set_cad(void) { return fake_rc("set_cad"); }

int sx1262_set_cad_params(uint8_t symbol_num, uint8_t det_peak, uint8_t det_min, uint8_t exit_mode,
                          uint32_t timeout) {
    (void)symbol_num;
    (void)det_peak;
    (void)det_min;
    (void)exit_mode;
    (void)timeout;
    return fake_rc("set_cad_params");
}

int sx1262_get_rx_buffer_status(uint8_t* payload_len, uint8_t* rx_start_offset) {
    if (payload_len)
        *payload_len = 0;
    if (rx_start_offset)
        *rx_start_offset = 0;
    return 0;
}

int sx1262_get_packet_status(int16_t* rssi, int8_t* snr) {
    if (rssi)
        *rssi = -80;
    if (snr)
        *snr = 5;
    return 0;
}

int sx1262_set_sleep(uint8_t config) {
    (void)config;
    return fake_rc("set_sleep");
}

int sx1262_calibrate_image(float freq_mhz) {
    (void)freq_mhz;
    return fake_rc("calibrate_image");
}

int sx1262_set_dio2_as_rf_switch(bool enable) {
    (void)enable;
    return fake_rc("set_dio2_as_rf_switch");
}

/* ------------------------------------------------------------------ */
/*  Driver under test                                                  */
/* ------------------------------------------------------------------ */

#define ESP_PLATFORM 1
#include "radio_esp.c"

static radio_config_t test_config(void) {
    radio_config_t cfg = {0};
    cfg.frequency_mhz = 906.875f;
    cfg.sf = 9;
    cfg.bw_hz = 125000;
    cfg.coding_rate = 5;
    cfg.preamble = 8;
    cfg.tx_power = 14;
    cfg.explicit_header = true;
    cfg.crc = true;
    cfg.sync_word = 0x12;
    return cfg;
}

static int s_tx_done_cb_calls;
static void counting_tx_done_cb(void) { s_tx_done_cb_calls++; }

void setUp(void) {
    fake_reset();
    s_config = test_config();
    atomic_store(&s_state, RADIO_STATE_IDLE);
    atomic_store(&s_tx_waiter, (TaskHandle_t)NULL);
    s_tx_done_cb_calls = 0;
    s_tx_done_cb = NULL;
}

void tearDown(void) {}

/* ---------- radio_start_rx state honesty (issue #83) ---------- */

static void test_start_rx_reports_rx_when_every_command_succeeds(void) {
    radio_start_rx();
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
    TEST_ASSERT_EQUAL_INT(1, s_calls_set_rx);
}

static void test_start_rx_does_not_claim_rx_when_set_rx_fails(void) {
    arm_failure("set_rx", -1);
    radio_start_rx();
    /* The chip is not listening, so the state must not say RX. */
    TEST_ASSERT_NOT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_IDLE, radio_get_state());
}

static void test_start_rx_does_not_claim_rx_when_standby_fails(void) {
    arm_failure("set_standby", -1);
    radio_start_rx();
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_IDLE, radio_get_state());
    /* Bailing early means SetRx is never clocked into a chip that did not
     * make it to standby. */
    TEST_ASSERT_EQUAL_INT(0, s_calls_set_rx);
}

static void test_start_rx_does_not_claim_rx_when_clear_irq_fails(void) {
    arm_failure("clear_irq_status", -1);
    radio_start_rx();
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_IDLE, radio_get_state());
    TEST_ASSERT_EQUAL_INT(0, s_calls_set_rx);
}

static void test_start_rx_after_hard_reset_does_not_claim_rx(void) {
    arm_failure("set_standby", SX1262_ERR_RESET);
    radio_start_rx();
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_IDLE, radio_get_state());
    TEST_ASSERT_EQUAL_INT(0, s_calls_set_rx);
}

/* ---------- radio_transmit_raw abort paths (issue #83) ---------- */

static void test_transmit_aborts_when_write_buffer_fails(void) {
    arm_failure("write_buffer", -1);
    uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    TEST_ASSERT_EQUAL_INT(-1, radio_transmit_raw(frame, sizeof(frame)));
    /* Never key the PA: transmitting here would put the previous frame's
     * stale FIFO contents on air. */
    TEST_ASSERT_EQUAL_INT(0, s_calls_set_tx);
    /* Recovery path put the radio back into RX. */
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

static void test_transmit_aborts_when_packet_params_fail(void) {
    arm_failure("set_packet_params", -1);
    uint8_t frame[4] = {9, 9, 9, 9};

    TEST_ASSERT_EQUAL_INT(-1, radio_transmit_raw(frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_INT(1, s_calls_write_buffer);
    TEST_ASSERT_EQUAL_INT(1, s_calls_set_packet_params);
    TEST_ASSERT_EQUAL_INT(0, s_calls_set_tx);
}

static void test_transmit_after_hard_reset_defers_to_reinit(void) {
    arm_failure("write_buffer", SX1262_ERR_RESET);
    uint8_t frame[4] = {1, 2, 3, 4};

    TEST_ASSERT_EQUAL_INT(-1, radio_transmit_raw(frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_INT(0, s_calls_set_tx);
    /* No further commands are issued against a chip sitting in power-on
     * defaults, so RX is not restarted here: radio_check_and_clear_reinit()
     * reconfigures first. The state stays honest in the meantime. */
    TEST_ASSERT_EQUAL_INT(0, s_calls_set_rx);
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_IDLE, radio_get_state());
}

/* ---------- TX waiter lifecycle (issue #80) ---------- */

static void test_transmit_abort_clears_the_tx_waiter(void) {
    arm_failure("set_tx", -1);
    uint8_t frame[4] = {1, 2, 3, 4};

    TEST_ASSERT_EQUAL_INT(-1, radio_transmit_raw(frame, sizeof(frame)));
    /* A left-behind handle is what let radio_task notify a sender that had
     * already given up. */
    TEST_ASSERT_NULL(atomic_load(&s_tx_waiter));
}

static void test_stale_notification_does_not_survive_into_the_next_transmit(void) {
    /* Simulate a TxDone that landed after a previous transmit gave up: the
     * notification is pending but belongs to nobody. */
    s_notify_count = 1;

    arm_failure("set_tx", -1);
    uint8_t frame[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_INT(-1, radio_transmit_raw(frame, sizeof(frame)));

    /* The stale give must have been drained, otherwise the next transmit
     * would read it as its own TxDone and report a phantom success while
     * tx_gate debited airtime for a frame never confirmed on air. */
    TEST_ASSERT_EQUAL_UINT32(0, s_notify_count);
}

static void test_wake_tx_waiter_claims_the_handle_exactly_once(void) {
    atomic_store(&s_tx_waiter, xTaskGetCurrentTaskHandle());

    wake_tx_waiter();
    TEST_ASSERT_EQUAL_UINT32(1, s_notify_count);
    TEST_ASSERT_NULL(atomic_load(&s_tx_waiter));

    /* A second IRQ (TxDone followed by Timeout) finds the waiter already
     * claimed and must not give again, which is also what stops
     * xTaskNotifyGive(NULL) from tripping the FreeRTOS configASSERT. */
    wake_tx_waiter();
    TEST_ASSERT_EQUAL_UINT32(1, s_notify_count);
}

static void test_wake_tx_waiter_is_a_noop_when_the_sender_already_gave_up(void) {
    atomic_store(&s_tx_waiter, (TaskHandle_t)NULL);
    wake_tx_waiter();
    TEST_ASSERT_EQUAL_UINT32(0, s_notify_count);
}

/* ---------- the full TX wait, both outcomes (issue #80) ---------- */

/* These drive radio_transmit_raw all the way through its ulTaskNotifyTake,
 * which the abort-path tests above never reach. The notified == 0 branch is
 * where the phantom-success bug actually lived. */

static void test_transmit_completes_when_txdone_arrives(void) {
    radio_set_tx_done_callback(counting_tx_done_cb);
    s_simulate_txdone = true;
    uint8_t frame[6] = {1, 2, 3, 4, 5, 6};

    TEST_ASSERT_EQUAL_INT(0, radio_transmit_raw(frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_INT(1, s_calls_set_tx);
    TEST_ASSERT_EQUAL_INT(1, s_tx_done_cb_calls);
    /* Back to listening, and the waiter is released. */
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
    TEST_ASSERT_NULL(atomic_load(&s_tx_waiter));
    /* The notification was consumed, not left behind for the next frame. */
    TEST_ASSERT_EQUAL_UINT32(0, s_notify_count);
}

static void test_transmit_reports_failure_when_txdone_never_arrives(void) {
    radio_set_tx_done_callback(counting_tx_done_cb);
    s_simulate_txdone = false; /* SetTx succeeds but the IRQ never fires */
    uint8_t frame[6] = {1, 2, 3, 4, 5, 6};

    TEST_ASSERT_EQUAL_INT(-1, radio_transmit_raw(frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_INT(1, s_calls_set_tx);
    /* An unconfirmed frame must not run the TX-done callback: tx_gate keys
     * its airtime debit off this return value. */
    TEST_ASSERT_EQUAL_INT(0, s_tx_done_cb_calls);
    /* Recovery branch put the radio back into RX. */
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
    TEST_ASSERT_NULL(atomic_load(&s_tx_waiter));
}

static void test_late_txdone_is_not_read_as_the_next_transmits_success(void) {
    /* This is issue #80 exactly: a TxDone that landed just after the previous
     * transmit's 4 second expiry is sitting in the notification counter with
     * no owner. Before the fix, the next transmit consumed it and returned
     * success for a frame that was never confirmed on air, while tx_gate
     * debited its airtime. */
    s_notify_count = 1;
    radio_set_tx_done_callback(counting_tx_done_cb);
    s_simulate_txdone = false;
    uint8_t frame[6] = {1, 2, 3, 4, 5, 6};

    TEST_ASSERT_EQUAL_INT(-1, radio_transmit_raw(frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_INT(0, s_tx_done_cb_calls);
}

/* ---------- sync word and sleep (issue #83) ---------- */

static void test_reconfigure_fails_when_the_sync_word_write_fails(void) {
    /* A dropped sync word write leaves the node invisible to the mesh, so it
     * has to surface rather than being discarded. */
    arm_failure("write_register", -1);
    radio_config_t cfg = test_config();
    TEST_ASSERT_NOT_EQUAL_INT(0, radio_reconfigure(&cfg));
}

static void test_sleep_does_not_claim_sleep_when_the_command_fails(void) {
    atomic_store(&s_state, RADIO_STATE_RX);
    arm_failure("set_sleep", -1);
    radio_sleep();
    TEST_ASSERT_NOT_EQUAL_INT(RADIO_STATE_SLEEP, radio_get_state());
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_start_rx_reports_rx_when_every_command_succeeds);
    RUN_TEST(test_start_rx_does_not_claim_rx_when_set_rx_fails);
    RUN_TEST(test_start_rx_does_not_claim_rx_when_standby_fails);
    RUN_TEST(test_start_rx_does_not_claim_rx_when_clear_irq_fails);
    RUN_TEST(test_start_rx_after_hard_reset_does_not_claim_rx);

    RUN_TEST(test_transmit_aborts_when_write_buffer_fails);
    RUN_TEST(test_transmit_aborts_when_packet_params_fail);
    RUN_TEST(test_transmit_after_hard_reset_defers_to_reinit);

    RUN_TEST(test_transmit_abort_clears_the_tx_waiter);
    RUN_TEST(test_stale_notification_does_not_survive_into_the_next_transmit);
    RUN_TEST(test_wake_tx_waiter_claims_the_handle_exactly_once);
    RUN_TEST(test_wake_tx_waiter_is_a_noop_when_the_sender_already_gave_up);

    RUN_TEST(test_transmit_completes_when_txdone_arrives);
    RUN_TEST(test_transmit_reports_failure_when_txdone_never_arrives);
    RUN_TEST(test_late_txdone_is_not_read_as_the_next_transmits_success);

    RUN_TEST(test_reconfigure_fails_when_the_sync_word_write_fails);
    RUN_TEST(test_sleep_does_not_claim_sleep_when_the_command_fails);

    return UNITY_END();
}
