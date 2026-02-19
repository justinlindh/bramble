#include "gps.h"
#include "nmea_parser.h"
#include "board_config.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "gps";

#define GPS_UART_NUM        UART_NUM_1
#define GPS_BUF_SIZE        1024
#define GPS_TASK_STACK_SIZE 4096
#define GPS_TASK_PRIORITY   5
#define GPS_MAX_LINE_LEN    128

static TaskHandle_t s_gps_task = NULL;
static gps_fix_cb_t s_callback = NULL;
static void *s_callback_ctx = NULL;
static bramble_position_t s_current_pos = {0};
static bool s_has_fix = false;

/* GPS background task */
static void gps_task(void *arg) {
    (void)arg;
    char line_buf[GPS_MAX_LINE_LEN];
    int line_pos = 0;
    uint8_t data[128];
    
    ESP_LOGI(TAG, "GPS task started");
    
    while (1) {
        /* Read available data */
        int len = uart_read_bytes(GPS_UART_NUM, data, sizeof(data) - 1, pdMS_TO_TICKS(100));
        if (len <= 0) continue;
        
        /* Process byte by byte to extract lines */
        for (int i = 0; i < len; i++) {
            char c = (char)data[i];
            
            /* Start of sentence */
            if (c == '$') {
                line_pos = 0;
                line_buf[line_pos++] = c;
            }
            /* End of sentence */
            else if (c == '\n' || c == '\r') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    
                    /* Parse NMEA sentence */
                    nmea_position_t nmea_pos;
                    memcpy(&nmea_pos, &s_current_pos, sizeof(nmea_position_t));
                    
                    bool parsed = false;
                    if (strncmp(line_buf, "$GPRMC", 6) == 0 || 
                        strncmp(line_buf, "$GNRMC", 6) == 0) {
                        /* Make a copy for strtok */
                        char sentence_copy[GPS_MAX_LINE_LEN];
                        strncpy(sentence_copy, line_buf, sizeof(sentence_copy) - 1);
                        sentence_copy[sizeof(sentence_copy) - 1] = '\0';
                        parsed = nmea_parse_rmc(sentence_copy, &nmea_pos);
                    }
                    else if (strncmp(line_buf, "$GPGGA", 6) == 0 || 
                             strncmp(line_buf, "$GNGGA", 6) == 0) {
                        char sentence_copy[GPS_MAX_LINE_LEN];
                        strncpy(sentence_copy, line_buf, sizeof(sentence_copy) - 1);
                        sentence_copy[sizeof(sentence_copy) - 1] = '\0';
                        parsed = nmea_parse_gga(sentence_copy, &nmea_pos);
                    }
                    
                    if (parsed && nmea_pos.valid) {
                        /* Update timestamp */
                        nmea_pos.timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);
                        
                        /* Copy to bramble_position_t */
                        bramble_position_t new_pos;
                        new_pos.latitude_e7 = nmea_pos.latitude_e7;
                        new_pos.longitude_e7 = nmea_pos.longitude_e7;
                        new_pos.altitude_m = nmea_pos.altitude_m;
                        new_pos.accuracy_m = nmea_pos.accuracy_m;
                        new_pos.speed_kmh = nmea_pos.speed_kmh;
                        new_pos.heading_deg2 = nmea_pos.heading_deg2;
                        new_pos.timestamp = nmea_pos.timestamp;
                        new_pos.valid = nmea_pos.valid;
                        
                        /* Update state */
                        bool was_fixed = s_has_fix;
                        memcpy(&s_current_pos, &new_pos, sizeof(bramble_position_t));
                        s_has_fix = true;
                        
                        /* Log first fix */
                        if (!was_fixed) {
                            ESP_LOGI(TAG, "GPS fix acquired: lat=%.6f lon=%.6f alt=%d",
                                     new_pos.latitude_e7 / 1e7, new_pos.longitude_e7 / 1e7,
                                     new_pos.altitude_m);
                        }
                        
                        /* Notify callback */
                        if (s_callback) {
                            s_callback(&new_pos, s_callback_ctx);
                        }
                    }
                    
                    line_pos = 0;
                }
            }
            /* Add to line buffer */
            else if (line_pos < GPS_MAX_LINE_LEN - 1) {
                line_buf[line_pos++] = c;
            }
        }
    }
}

int gps_init(gps_fix_cb_t cb, void *ctx) {
    const bramble_board_config_t *board = board_get_config();
    
    /* Check if board has GPS */
    if (!(board->capabilities & BOARD_CAP_GPS)) {
        ESP_LOGW(TAG, "Board does not support GPS");
        return -1;
    }
    
    if (board->gps.tx < 0 || board->gps.rx < 0) {
        ESP_LOGE(TAG, "GPS pins not configured");
        return -1;
    }
    
    s_callback = cb;
    s_callback_ctx = ctx;
    
    /* Configure UART */
    uart_config_t uart_config = {
        .baud_rate = board->gps.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, GPS_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, board->gps.tx, board->gps.rx,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    ESP_LOGI(TAG, "GPS UART initialized (TX=%d, RX=%d, baud=%d)",
             board->gps.tx, board->gps.rx, board->gps.baud);
    
    /* Create background task */
    BaseType_t res = xTaskCreate(gps_task, "gps_task", GPS_TASK_STACK_SIZE,
                                  NULL, GPS_TASK_PRIORITY, &s_gps_task);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS task");
        uart_driver_delete(GPS_UART_NUM);
        return -1;
    }
    
    ESP_LOGI(TAG, "GPS initialized successfully (cold start may take 30-90s)");
    return 0;
}

bool gps_has_fix(void) {
    return s_has_fix;
}

bool gps_get_position(bramble_position_t *out) {
    if (!out || !s_has_fix) return false;
    memcpy(out, &s_current_pos, sizeof(bramble_position_t));
    return true;
}

void gps_deinit(void) {
    if (s_gps_task) {
        vTaskDelete(s_gps_task);
        s_gps_task = NULL;
    }
    uart_driver_delete(GPS_UART_NUM);
    s_has_fix = false;
    ESP_LOGI(TAG, "GPS deinitialized");
}

#else
/* Host build stubs */
int gps_init(gps_fix_cb_t cb, void *ctx) { (void)cb; (void)ctx; return -1; }
bool gps_has_fix(void) { return false; }
bool gps_get_position(bramble_position_t *out) { (void)out; return false; }
void gps_deinit(void) {}
#endif /* ESP_PLATFORM */
