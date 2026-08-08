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

/* A clock the reinit-retry cases can advance by hand. Defined before radio_esp.c
 * pulls in esp_timer.h so the stub's extern branch is what the driver sees. */
#define ESP_TIMER_CUSTOM_IMPL
#include "esp_timer.h"

static int64_t s_fake_now_us;
int64_t esp_timer_get_time(void) { return s_fake_now_us; }
static void advance_ms(uint32_t ms) { s_fake_now_us += (int64_t)ms * 1000; }

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

/* Wall-clock a failing command burns before returning, so a test can model a
 * wedged chip: the real driver spends BUSY_STUCK_THRESHOLD command waits of
 * 2000ms (5000ms for calibration) before a reconfigure gives up, which can
 * exceed the reinit backoff itself. */
static uint32_t s_fail_delay_ms;

static int fake_rc(const char* call) {
    if (s_fail_call && strcmp(s_fail_call, call) == 0) {
        advance_ms(s_fail_delay_ms);
        return s_fail_code;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Instrumented semaphore layer (issue #225)                          */
/* ------------------------------------------------------------------ */

/* The shared stub semaphore is a no-op, but these tests need to observe that
 * radio_task's RX-done read and radio_reconfigure take and release the SAME
 * rx-sequence mutex, and that a reconfigure landing mid-read contends on it
 * (i.e. would block, so it cannot splice into the read). Defining the stub's
 * include guard here makes radio_esp.c pick up these instrumented versions
 * instead. Only the handle the test tracks (s_tracked_mutex) is counted; every
 * other semaphore behaves like the stub. */
#define FREERTOS_SEMPHR_H_STUB
typedef void* SemaphoreHandle_t;

static int s_sem_next_handle;
static void* s_tracked_mutex; /* set by setUp to radio_esp.c's s_rx_seq_mutex */
static int s_rxseq_depth;     /* current held depth of the tracked mutex */
static int s_rxseq_max_depth; /* max simultaneous depth (2 == a mid-read take) */
static int s_rxseq_takes;     /* total takes of the tracked mutex */

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)(intptr_t)(++s_sem_next_handle);
}
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    return (SemaphoreHandle_t)(intptr_t)(++s_sem_next_handle);
}
static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned long ticks) {
    (void)ticks;
    if (s && s == s_tracked_mutex) {
        s_rxseq_takes++;
        s_rxseq_depth++;
        if (s_rxseq_depth > s_rxseq_max_depth) {
            s_rxseq_max_depth = s_rxseq_depth;
        }
    }
    return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t s) {
    if (s && s == s_tracked_mutex) {
        s_rxseq_depth--;
    }
    return 1;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }

/* RX-read fake controls: length GetRxBufferStatus reports, the lock depth seen
 * inside GetPacketStatus, and an optional hook fired inside ReadBuffer to
 * simulate a concurrent reconfigure arriving mid-read. */
static int s_fake_rx_len;
static int s_depth_in_get_pkt_status;
static void (*s_rx_read_hook)(void);

int sx1262_init(void) { return fake_rc("init"); }

/* The reinit latch is stateful here, not a stub: whether a failed recovery
 * leaves the request standing is exactly what the reinit cases assert, and a
 * latch that always reads false could not tell the two behaviors apart. Also
 * reached from radio_cad_check's fail-closed path (issue #118), which these
 * tests do not exercise (the semaphore stub always succeeds, so the CAD timeout
 * branch is never taken). */
static bool s_fake_needs_reinit;
static int s_reinit_requests;

bool sx1262_needs_reinit(void) { return s_fake_needs_reinit; }
void sx1262_clear_reinit(void) { s_fake_needs_reinit = false; }
void sx1262_request_reinit(void) {
    s_fake_needs_reinit = true;
    s_reinit_requests++;
}

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
    /* Simulate a concurrent reconfigure landing between ReadBuffer and
     * GetPacketStatus, i.e. right in the middle of the RX read sequence. */
    if (s_rx_read_hook) {
        s_rx_read_hook();
    }
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

int sx1262_set_modulation_params(uint8_t sf, uint32_t bw_hz, uint8_t cr, uint8_t ldro) {
    (void)sf;
    (void)bw_hz;
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

/* Transmit-path readback fakes. Defaults describe a healthy chip: no latched
 * device errors, standby with the last command done, and the OCP value the
 * driver programs, so radio_get_health's "everything is fine" path is what the
 * existing state tests see unless a case overrides these. */
uint16_t g_fake_device_errors = 0;
uint8_t g_fake_status = (uint8_t)((SX1262_MODE_STBY_RC << 4) | (SX1262_CMD_STATUS_TX_DONE << 1));
uint8_t g_fake_ocp = SX1262_OCP_140MA;

int sx1262_get_status(uint8_t* status) {
    if (status)
        *status = g_fake_status;
    return fake_rc("get_status");
}

int sx1262_get_device_errors(uint16_t* errors) {
    if (errors)
        *errors = g_fake_device_errors;
    return fake_rc("get_device_errors");
}

int sx1262_clear_device_errors(void) { return fake_rc("clear_device_errors"); }

int sx1262_read_register(uint16_t addr, uint8_t* data, size_t len) {
    if (data && len >= 1 && addr == SX1262_REG_OCP)
        data[0] = g_fake_ocp;
    return fake_rc("read_register");
}

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
        *payload_len = (uint8_t)s_fake_rx_len;
    if (rx_start_offset)
        *rx_start_offset = 0;
    return 0;
}

int sx1262_get_packet_status(int16_t* rssi, int8_t* snr) {
    /* Record whether the RX-sequence lock is held at the innermost read. */
    s_depth_in_get_pkt_status = s_rxseq_depth;
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
/*  Fake tx_gate serialization hooks (issue #82)                       */
/* ------------------------------------------------------------------ */

/* radio_reconfigure now brackets its whole command sequence in the transmit
 * serialization lock so it cannot splice into an in-flight radio_transmit_raw.
 * No real gate runs here, so the fakes just track the balance: the tests assert
 * the lock is taken and released on every path, including the failure return. */
static int s_gate_lock_depth;
static int s_gate_lock_max_depth;
static int s_gate_lock_calls;

void tx_gate_radio_lock(void) {
    s_gate_lock_depth++;
    s_gate_lock_calls++;
    if (s_gate_lock_depth > s_gate_lock_max_depth)
        s_gate_lock_max_depth = s_gate_lock_depth;
}

void tx_gate_radio_unlock(void) { s_gate_lock_depth--; }

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
    s_rx_cb = NULL;
    s_gate_lock_depth = 0;
    s_gate_lock_max_depth = 0;
    s_gate_lock_calls = 0;
    /* RX-sequence lock (issue #225): create the mutex radio_init would make and
     * point the instrumented semaphore layer at it, then reset the counters. */
    s_rx_seq_mutex = xSemaphoreCreateMutex();
    s_tracked_mutex = s_rx_seq_mutex;
    s_rxseq_depth = 0;
    s_rxseq_max_depth = 0;
    s_rxseq_takes = 0;
    s_fake_rx_len = 0;
    s_depth_in_get_pkt_status = -1;
    s_rx_read_hook = NULL;
    s_fake_needs_reinit = false;
    s_reinit_requests = 0;
    s_fake_now_us = 0;
    s_fail_delay_ms = 0;
    s_reinit_policy = (radio_reinit_policy_t){0};
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

/* ---------- reconfigure serialization (issue #82) ---------- */

static void test_reconfigure_holds_the_gate_lock_across_the_sequence(void) {
    radio_config_t cfg = test_config();
    TEST_ASSERT_EQUAL_INT(0, radio_reconfigure(&cfg));
    /* It actually acquired the transmit lock, held it once (not nested), and
     * released it: without this a reconfigure spliced into an in-flight
     * transmit. */
    TEST_ASSERT_TRUE(s_gate_lock_calls > 0);
    TEST_ASSERT_EQUAL_INT(1, s_gate_lock_max_depth);
    TEST_ASSERT_EQUAL_INT(0, s_gate_lock_depth);
}

static void test_reconfigure_releases_the_gate_lock_on_failure(void) {
    /* The configure_radio failure path returns early; it must still release the
     * lock or the next transmit would block forever. */
    arm_failure("write_register", -1);
    radio_config_t cfg = test_config();
    TEST_ASSERT_NOT_EQUAL_INT(0, radio_reconfigure(&cfg));
    TEST_ASSERT_TRUE(s_gate_lock_calls > 0);
    TEST_ASSERT_EQUAL_INT(0, s_gate_lock_depth);
}

/* ---------- RX-done read serialization (issue #225) ---------- */

/* radio_task's RX-done read runs the whole GetRxBufferStatus -> ReadBuffer ->
 * GetPacketStatus sequence under the RX-sequence lock, held (not nested) across
 * the reads and released after. */
static void test_rx_read_runs_under_the_rx_seq_lock(void) {
    s_fake_rx_len = 16; /* a frame is waiting */
    uint8_t buf[64];
    radio_handle_rx_done(buf, sizeof(buf));
    TEST_ASSERT_TRUE(s_rxseq_takes > 0);
    TEST_ASSERT_EQUAL_INT(1, s_depth_in_get_pkt_status); /* held during the read */
    TEST_ASSERT_EQUAL_INT(1, s_rxseq_max_depth);         /* held once, not nested */
    TEST_ASSERT_EQUAL_INT(0, s_rxseq_depth);             /* released */
}

/* An empty RX (GetRxBufferStatus reports zero length) must still take and
 * release the lock in balance, never leak it. */
static void test_rx_read_releases_lock_when_no_frame(void) {
    s_fake_rx_len = 0;
    uint8_t buf[64];
    radio_handle_rx_done(buf, sizeof(buf));
    TEST_ASSERT_TRUE(s_rxseq_takes > 0);
    TEST_ASSERT_EQUAL_INT(0, s_rxseq_depth);
}

/* radio_reconfigure takes and releases the SAME rx-sequence lock, so it is
 * mutually exclusive with the RX-done read. */
static void test_reconfigure_takes_the_rx_seq_lock(void) {
    radio_config_t cfg = test_config();
    TEST_ASSERT_EQUAL_INT(0, radio_reconfigure(&cfg));
    TEST_ASSERT_TRUE(s_rxseq_takes > 0);
    TEST_ASSERT_EQUAL_INT(0, s_rxseq_depth);
}

static void reconfigure_from_within_read(void) {
    radio_config_t cfg = test_config();
    radio_reconfigure(&cfg);
}

/* The interleave itself: a reconfigure arriving in the middle of the RX read
 * contends on the rx-sequence lock the read already holds (depth reaches 2). On
 * real hardware that second take blocks until the read releases, so reconfigure
 * cannot splice into the read. Non-vacuous: if reconfigure took a different
 * lock, or none, the depth would stay 1 and the interleave would be live. */
static void test_reconfigure_mid_read_contends_on_the_rx_seq_lock(void) {
    s_fake_rx_len = 16;
    s_rx_read_hook = reconfigure_from_within_read;
    uint8_t buf[64];
    radio_handle_rx_done(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(2, s_rxseq_max_depth);
    TEST_ASSERT_EQUAL_INT(0, s_rxseq_depth); /* both releases happened */
}

/* ---------- reinit recovery is never dropped ---------- */

/* With no request outstanding the mesh loop's per-pass call is free: it reports
 * nothing to do and issues no commands. */
static void test_reinit_check_is_a_noop_when_nothing_is_requested(void) {
    TEST_ASSERT_FALSE(radio_check_and_clear_reinit());
    TEST_ASSERT_EQUAL_INT(0, s_calls_set_rx);
}

/* A recovery that works clears the request and leaves the radio receiving. */
static void test_successful_reinit_clears_the_request(void) {
    sx1262_request_reinit();
    TEST_ASSERT_TRUE(radio_check_and_clear_reinit());
    TEST_ASSERT_FALSE(sx1262_needs_reinit());
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

/* The bug this closes: the latch is cleared before the attempt, so a
 * reconfigure that fails used to drop the request entirely. The chip then sat
 * at power-on defaults forever, transmitting nothing a neighbor could hear,
 * while the node stayed alive, fed its watchdog and looked healthy. */
static void test_failed_reinit_leaves_the_request_standing(void) {
    arm_failure("write_register", -1); /* sync word write fails the reconfigure */
    sx1262_request_reinit();
    int requests_before = s_reinit_requests;

    TEST_ASSERT_TRUE(radio_check_and_clear_reinit());
    TEST_ASSERT_TRUE(sx1262_needs_reinit());
    TEST_ASSERT_EQUAL_INT(requests_before + 1, s_reinit_requests);
}

/* The retry is real, and it is paced: the failed attempt does not turn the
 * mesh loop's 10ms cadence into a stream of chip resets, and once the backoff
 * expires a working chip recovers. */
static void test_failed_reinit_retries_after_the_backoff(void) {
    arm_failure("write_register", -1);
    sx1262_request_reinit();
    TEST_ASSERT_TRUE(radio_check_and_clear_reinit());

    /* Inside the backoff: the request stands but no attempt is made. */
    int requests_after_first = s_reinit_requests;
    advance_ms(BRAMBLE_RADIO_REINIT_RETRY_MS - 1u);
    TEST_ASSERT_FALSE(radio_check_and_clear_reinit());
    TEST_ASSERT_TRUE(sx1262_needs_reinit());
    TEST_ASSERT_EQUAL_INT(requests_after_first, s_reinit_requests);

    /* Backoff expired and the chip is answering again: the retry happens and
     * takes. */
    advance_ms(1u);
    s_fail_call = NULL;
    TEST_ASSERT_TRUE(radio_check_and_clear_reinit());
    TEST_ASSERT_FALSE(sx1262_needs_reinit());
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

/* A failing recovery against a wedged chip can take longer than the backoff.
 * The deadline must therefore be measured from when the attempt finished: a
 * caller that samples the clock once up front produces a deadline already in
 * the past, and the mesh loop, which calls this every 10ms, re-attempts back
 * to back forever with no gap. That is worse than the busy-loop the backoff
 * exists to prevent, because each iteration also hard-resets the chip. */
static void test_a_slow_failing_reinit_still_backs_off(void) {
    arm_failure("write_register", -1);
    s_fail_delay_ms = BRAMBLE_RADIO_REINIT_RETRY_MS + 4000u; /* wedged chip */
    sx1262_request_reinit();

    TEST_ASSERT_TRUE(radio_check_and_clear_reinit());
    TEST_ASSERT_TRUE(sx1262_needs_reinit());
    /* The attempt outlasted the backoff, so a start-sampled deadline is now
     * in the past and the next pass would re-attempt immediately. */
    int requests = s_reinit_requests;
    TEST_ASSERT_FALSE(radio_check_and_clear_reinit());
    TEST_ASSERT_EQUAL_INT(requests, s_reinit_requests);
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

    RUN_TEST(test_reconfigure_holds_the_gate_lock_across_the_sequence);
    RUN_TEST(test_reconfigure_releases_the_gate_lock_on_failure);

    RUN_TEST(test_rx_read_runs_under_the_rx_seq_lock);
    RUN_TEST(test_rx_read_releases_lock_when_no_frame);
    RUN_TEST(test_reconfigure_takes_the_rx_seq_lock);
    RUN_TEST(test_reconfigure_mid_read_contends_on_the_rx_seq_lock);

    RUN_TEST(test_reinit_check_is_a_noop_when_nothing_is_requested);
    RUN_TEST(test_successful_reinit_clears_the_request);
    RUN_TEST(test_failed_reinit_leaves_the_request_standing);
    RUN_TEST(test_failed_reinit_retries_after_the_backoff);
    RUN_TEST(test_a_slow_failing_reinit_still_backs_off);

    return UNITY_END();
}
