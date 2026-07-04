#include "unity.h"
#include "gps.h"
#include "board_config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <setjmp.h>
#include <string.h>

/* Board stub */
static bramble_board_config_t g_cfg;
const bramble_board_config_t* board_get_config(void) { return &g_cfg; }

/* Control flow escape for gps_task infinite loop */
static jmp_buf g_jmp;
static int g_allow_jump = 0;
static int g_in_task = 0;
static size_t g_uart_pos = 0;
static const char* g_uart_stream = NULL;

/* Time stub */
static int64_t g_time_us = 0;
int64_t esp_timer_get_time(void) { return g_time_us; }

/* RTOS task stubs */
void vTaskDelay(int ticks) {
    (void)ticks;
    g_time_us += 1000;
}
int xTaskCreate(void (*task)(void*), const char* name, int stack, void* arg, int pri,
                TaskHandle_t* out) {
    (void)name;
    (void)stack;
    (void)pri;
    if (out)
        *out = (void*)0x1234;
    g_uart_pos = 0;
    g_in_task = 1;
    task(arg);
    return 1;
}
void vTaskDelete(TaskHandle_t t) { (void)t; }

/* UART stubs */
static int g_uart_driver_deleted = 0;
static int g_uart_set_baud_calls = 0;

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size,
                              int queue_size, void* uart_queue, int intr_alloc_flags) {
    (void)uart_num;
    (void)rx_buffer_size;
    (void)tx_buffer_size;
    (void)queue_size;
    (void)uart_queue;
    (void)intr_alloc_flags;
    return ESP_OK;
}
esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t* uart_config) {
    (void)uart_num;
    (void)uart_config;
    return ESP_OK;
}
esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num,
                       int cts_io_num) {
    (void)uart_num;
    (void)tx_io_num;
    (void)rx_io_num;
    (void)rts_io_num;
    (void)cts_io_num;
    return ESP_OK;
}
esp_err_t uart_set_baudrate(uart_port_t uart_num, int baudrate) {
    (void)uart_num;
    (void)baudrate;
    g_uart_set_baud_calls++;
    return ESP_OK;
}
esp_err_t uart_flush_input(uart_port_t uart_num) {
    (void)uart_num;
    return ESP_OK;
}
int uart_read_bytes(uart_port_t uart_num, void* buf, uint32_t length, uint32_t ticks_to_wait) {
    (void)uart_num;
    (void)ticks_to_wait;
    g_time_us += 100000;

    if (!g_in_task) {
        return 0;
    }

    if (!g_uart_stream) {
        if (g_allow_jump && g_in_task)
            longjmp(g_jmp, 1);
        return 0;
    }

    size_t remain = strlen(g_uart_stream) - g_uart_pos;
    if (remain == 0) {
        if (g_allow_jump && g_in_task)
            longjmp(g_jmp, 1);
        return 0;
    }

    size_t n = (remain < length) ? remain : length;
    memcpy(buf, g_uart_stream + g_uart_pos, n);
    g_uart_pos += n;
    return (int)n;
}
esp_err_t uart_driver_delete(uart_port_t uart_num) {
    (void)uart_num;
    g_uart_driver_deleted++;
    return ESP_OK;
}

/* GPIO stubs */
esp_err_t gpio_config(const gpio_config_t* pGPIOConfig) {
    (void)pGPIOConfig;
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level) {
    (void)gpio_num;
    (void)level;
    return ESP_OK;
}

static void reset_state(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.capabilities = BOARD_CAP_GPS;
    g_cfg.gps.tx = 17;
    g_cfg.gps.rx = 18;
    g_cfg.gps.baud = 9600;
    g_uart_stream = NULL;
    g_uart_pos = 0;
    g_uart_driver_deleted = 0;
    g_uart_set_baud_calls = 0;
    g_time_us = 0;
    g_allow_jump = 0;
    g_in_task = 0;
}

void setUp(void) { reset_state(); }
void tearDown(void) { gps_deinit(); }

void test_gps_init_and_deinit_without_fix(void) {
    g_uart_stream = "$GPXXX,1,2,3\x0d\x0a";
    if (setjmp(g_jmp) == 0) {
        g_allow_jump = 1;
        TEST_ASSERT_EQUAL(0, gps_init(NULL, NULL));
        TEST_FAIL_MESSAGE("expected jump out of gps task loop");
    }
    TEST_ASSERT_FALSE(gps_has_fix());
    bramble_position_t out = {0};
    TEST_ASSERT_FALSE(gps_get_position(&out));
    gps_deinit();
    TEST_ASSERT_EQUAL(1, g_uart_driver_deleted);
}

void test_gps_invalid_or_unparsed_sentence_keeps_no_fix(void) {
    g_uart_stream = "$GPXXX,1,2,3\x0d\x0a";
    if (setjmp(g_jmp) == 0) {
        g_allow_jump = 1;
        (void)gps_init(NULL, NULL);
        TEST_FAIL_MESSAGE("expected jump out of gps task loop");
    }
    TEST_ASSERT_FALSE(gps_has_fix());
    bramble_position_t out = {0};
    TEST_ASSERT_FALSE(gps_get_position(&out));
}

void test_gps_parses_rmc_and_sets_fix(void) {
    g_uart_stream = "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*6A\x0d\x0a";
    if (setjmp(g_jmp) == 0) {
        g_allow_jump = 1;
        TEST_ASSERT_EQUAL(0, gps_init(NULL, NULL));
        TEST_FAIL_MESSAGE("expected longjmp out of gps_task");
    }
    TEST_ASSERT_TRUE_MESSAGE(gps_has_fix(), "expected GPS fix after valid RMC");
    bramble_position_t out = {0};
    TEST_ASSERT_TRUE(gps_get_position(&out));
    TEST_ASSERT_TRUE(out.valid);
    TEST_ASSERT_INT_WITHIN(10000, 481173000, out.latitude_e7);
    TEST_ASSERT_INT_WITHIN(10000, 115166667, out.longitude_e7);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_gps_init_and_deinit_without_fix);
    RUN_TEST(test_gps_invalid_or_unparsed_sentence_keeps_no_fix);
    RUN_TEST(test_gps_parses_rmc_and_sets_fix);
    return UNITY_END();
}
