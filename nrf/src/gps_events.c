/* Trimmed copy of main.c's emit_gps_event()/on_gps_fix() for the nRF52840
 * GNSS driver: builds the same bramble.onGpsEvent payload and applies the
 * same 5s throttle on fix_acquired, so the webapp sees identical event
 * shapes regardless of which fleet the node runs on. */
#include "gps_events.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rpc_dispatcher.h"

static const char* TAG = "gps_events";

static void emit_gps_event(const char* event, const bramble_position_t* pos) {
    cJSON* params = cJSON_CreateObject();
    if (!params)
        return;
    cJSON_AddStringToObject(params, "event", event);
    if (pos) {
        cJSON_AddBoolToObject(params, "valid", pos->valid);
        cJSON_AddNumberToObject(params, "lat", pos->latitude_e7 / 1e7);
        cJSON_AddNumberToObject(params, "lon", pos->longitude_e7 / 1e7);
        cJSON_AddNumberToObject(params, "alt_m", pos->altitude_m);
        cJSON_AddNumberToObject(params, "accuracy_m", pos->accuracy_m);
    }
    rpc_notify("bramble.onGpsEvent", params);
    cJSON_Delete(params);
}

void nrf_on_gps_fix(const bramble_position_t* pos, void* ctx) {
    (void)ctx;
    if (pos && pos->valid) {
        /* Throttle RPC notifications + log to avoid ~60/min of GPS chatter. */
        static uint64_t s_last_gps_notify_us = 0;
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (s_last_gps_notify_us == 0 || (now_us - s_last_gps_notify_us) >= 5000000ULL) {
            s_last_gps_notify_us = now_us;
            emit_gps_event("fix_acquired", pos);
            ESP_LOGI(TAG, "GPS position updated: lat=%.6f lon=%.6f alt=%d", pos->latitude_e7 / 1e7,
                     pos->longitude_e7 / 1e7, pos->altitude_m);
        }
    }
}

void nrf_gps_emit_fix_lost(void) { emit_gps_event("fix_lost", NULL); }
