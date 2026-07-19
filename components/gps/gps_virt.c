/*
 * gps_virt: the emulator's virtual GPS. It implements the device half of the
 * gps.h contract for the host (IDF linux target and the plain-gcc test
 * harness), mirroring components/gps/gps.c:
 *
 *   - The GPIO38 P-FET power gate becomes an emu-link `gpsgate` message: on at
 *     gps_init (P-FET low = powered), off at gps_deinit (P-FET high = cut).
 *   - While gated OFF, inbound `nmea` messages are dropped, exactly like the
 *     P-FET: no power, no sentences. The nmea handler stays registered across
 *     deinit (emu_link has no unregister) but a gate check makes it a no-op.
 *   - Sentences that arrive while gated ON are fed to the real pure
 *     nmea_parser.c (the same RMC/GGA/GSV/antenna dispatch gps.c's UART task
 *     runs), and a valid fix updates the position and fires the fix callback.
 *
 * THREADING CONTRACT (same as radio_virt/button_virt, see the block comment
 * in radio_virt.c for the full failure history): emu_link's reader is a RAW
 * pthread, and on the IDF-linux FreeRTOS port a foreign thread must never
 * call FreeRTOS APIs. The fix callback lands in FreeRTOS-managed code
 * (main.c's on_gps_fix -> rpc_notify -> httpd/BLE queues), so the reader
 * thread must NOT run it. On the node the nmea handler only copies the
 * sentence into a plain pthread-mutex ring; a FreeRTOS pump task drains the
 * ring and does all parsing, state updates, and the callback from legal task
 * context. That also keeps s_mu a task-only lock on the node: the reader
 * thread never touches it. The plain-gcc test harness (no ESP_PLATFORM) has
 * no scheduler and its tests drive dispatch synchronously, so it keeps the
 * direct call, and s_mu guards handler-vs-API state there. The fix callback
 * is invoked with the lock dropped so a callback that reads position back
 * cannot deadlock.
 */

/* CONFIG_IDF_TARGET_LINUX lives in sdkconfig.h; pull it in on IDF builds so
 * the composite host-gate below sees it (same convention as board_config.h /
 * display.h). The plain-gcc test harness defines no ESP_PLATFORM and has no
 * sdkconfig.h, and takes the host branch anyway. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)
/* On-device build: components/gps/gps.c owns the real UART implementation. */
#else

#include "gps.h"
#include "nmea_parser.h"
#include "emu_link.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

/* Real esp_log and FreeRTOS on the node; absent in the plain-gcc test
 * harness, which never reaches the pump-task code. */
#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char* TAG = "gps_virt";
#endif

#define GPS_VIRT_MAX_LINE 128

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static gps_fix_cb_t s_cb = NULL;
static void* s_cb_ctx = NULL;
static bramble_position_t s_pos = {0};
static nmea_position_t s_np = {0}; /* running accumulator across sentences */
static bool s_has_fix = false;
static bool s_gate_on = false; /* GPS power gate: false = powered off */
static uint8_t s_sats_used = 0;
static uint8_t s_sats_in_view = 0;
static bool s_antenna_warning = false;
static uint8_t s_utc_hour = 0;
static uint8_t s_utc_min = 0;
static bool s_utc_valid = false;

static void nmea_handler(const cJSON* msg, void* ctx);

static void send_gpsgate(bool on) {
    cJSON* m = cJSON_CreateObject();
    if (!m)
        return;
    cJSON_AddStringToObject(m, "t", "gpsgate");
    cJSON_AddBoolToObject(m, "on", on);
    emu_link_send(m); /* takes ownership on all paths */
}

#if defined(ESP_PLATFORM)
/* Reader-thread to pump-task sentence ring (radio_virt's rvirt_ev pattern). */
#define GVIRT_EVQ_CAP 32

static pthread_mutex_t s_evq_mu = PTHREAD_MUTEX_INITIALIZER;
static char s_evq[GVIRT_EVQ_CAP][GPS_VIRT_MAX_LINE];
static int s_evq_head = 0, s_evq_tail = 0, s_evq_count = 0;
static unsigned s_evq_dropped = 0; /* logged from the pump task, not here */
static bool s_pump_started = false;

static void process_sentence(const char* line);

/* Reader thread: enqueue only (pthread mutex, no FreeRTOS, no logging). A
 * full ring drops the newest sentence and counts it; a real GNSS module's
 * UART overrun loses sentences the same way and the next periodic fix
 * resupplies the state. */
static void gvirt_ev_push(const char* line) {
    pthread_mutex_lock(&s_evq_mu);
    if (s_evq_count < GVIRT_EVQ_CAP) {
        strncpy(s_evq[s_evq_tail], line, GPS_VIRT_MAX_LINE - 1);
        s_evq[s_evq_tail][GPS_VIRT_MAX_LINE - 1] = '\0';
        s_evq_tail = (s_evq_tail + 1) % GVIRT_EVQ_CAP;
        s_evq_count++;
    } else {
        s_evq_dropped++;
    }
    pthread_mutex_unlock(&s_evq_mu);
}

/* Pump task: the only place reader-thread sentences meet FreeRTOS. Polls
 * with vTaskDelay (a condvar cannot wake a FreeRTOS task on this port; see
 * radio_virt's txdone poll note). */
static void gvirt_pump_task(void* arg) {
    (void)arg;
    for (;;) {
        char line[GPS_VIRT_MAX_LINE];
        bool have;
        unsigned dropped;
        pthread_mutex_lock(&s_evq_mu);
        have = s_evq_count > 0;
        if (have) {
            memcpy(line, s_evq[s_evq_head], GPS_VIRT_MAX_LINE);
            s_evq_head = (s_evq_head + 1) % GVIRT_EVQ_CAP;
            s_evq_count--;
        }
        dropped = s_evq_dropped;
        s_evq_dropped = 0;
        pthread_mutex_unlock(&s_evq_mu);

        if (dropped > 0)
            ESP_LOGW(TAG, "nmea sentence ring overflowed; dropped %u sentence(s)", dropped);
        if (!have) {
            vTaskDelay(1);
            continue;
        }
        process_sentence(line);
    }
}
#endif /* ESP_PLATFORM */

int gps_init(gps_fix_cb_t cb, void* ctx) {
    pthread_mutex_lock(&s_mu);
    s_cb = cb;
    s_cb_ctx = ctx;
    pthread_mutex_unlock(&s_mu);
    return gps_set_enabled(true);
}

int gps_set_enabled(bool enabled) {
    if (!enabled) {
        gps_deinit(); /* cuts the gate and emits gpsgate off */
        return 0;
    }

    pthread_mutex_lock(&s_mu);
    s_has_fix = false;
    s_gate_on = true; /* power the GNSS on (P-FET low) */
    pthread_mutex_unlock(&s_mu);

#if defined(ESP_PLATFORM)
    /* One pump task per process, surviving disable/enable cycles: it is the
     * only legal FreeRTOS context for the reader thread's sentences (see the
     * threading contract in the file header). */
    if (!s_pump_started) {
        if (xTaskCreate(gvirt_pump_task, "gvirt_pump", 4096, NULL, 10, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create nmea pump task");
            return -1;
        }
        s_pump_started = true;
    }
#endif

    /* Registering the same handler twice is idempotent (emu_link replaces).
     * The callback registered by a prior gps_init() is retained across a
     * disable, so re-enabling resumes fixes without re-registering. */
    emu_link_on("nmea", nmea_handler, NULL);
    send_gpsgate(true);
    return 0;
}

/* Parse one sentence and apply it: accumulator merge, position/stats update,
 * fix callback. On the node this runs on the pump task; in the plain-gcc
 * harness it runs synchronously on the caller of the nmea handler. */
static void process_sentence(const char* str) {
    pthread_mutex_lock(&s_mu);
    if (!s_gate_on) {
        pthread_mutex_unlock(&s_mu);
        return; /* powered off: no sentences, like an open P-FET */
    }
    nmea_position_t np = s_np; /* accumulator snapshot */
    pthread_mutex_unlock(&s_mu);

    char line[GPS_VIRT_MAX_LINE];
    strncpy(line, str, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    bool parsed = false;
    bool is_gga = false;
    bool got_gsv = false;
    uint8_t sats_in_view = 0;

    if (strncmp(line, "$GPRMC", 6) == 0 || strncmp(line, "$GNRMC", 6) == 0) {
        char copy[GPS_VIRT_MAX_LINE];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        parsed = nmea_parse_rmc(copy, &np);
    } else if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
        char copy[GPS_VIRT_MAX_LINE];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        parsed = nmea_parse_gga(copy, &np);
        is_gga = true; /* satellites-used is reported even without a fix */
    } else if (strncmp(line, "$GPGSV", 6) == 0 || strncmp(line, "$GNGSV", 6) == 0) {
        char copy[GPS_VIRT_MAX_LINE];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        got_gsv = nmea_parse_gsv(copy, &sats_in_view);
    } else if (nmea_is_antenna_open(line)) {
        pthread_mutex_lock(&s_mu);
        s_antenna_warning = true;
        pthread_mutex_unlock(&s_mu);
        return;
    }

    gps_fix_cb_t cb = NULL;
    void* cb_ctx = NULL;
    bramble_position_t out;

    pthread_mutex_lock(&s_mu);
    s_np = np; /* persist merged accumulator (incl. sats_used) */
    if (is_gga)
        s_sats_used = np.sats_used;
    if (got_gsv)
        s_sats_in_view = sats_in_view;
    if (parsed && np.valid) {
        s_pos.latitude_e7 = np.latitude_e7;
        s_pos.longitude_e7 = np.longitude_e7;
        s_pos.altitude_m = np.altitude_m;
        s_pos.accuracy_m = np.accuracy_m;
        s_pos.speed_kmh = np.speed_kmh;
        s_pos.heading_deg2 = np.heading_deg2;
        s_pos.timestamp = (uint32_t)time(NULL);
        s_pos.valid = np.valid;
        s_has_fix = true;
        if (np.utc_valid) {
            s_utc_hour = np.utc_hour;
            s_utc_min = np.utc_min;
            s_utc_valid = true;
        }
        cb = s_cb;
        cb_ctx = s_cb_ctx;
        out = s_pos;
    }
    pthread_mutex_unlock(&s_mu);

    if (cb)
        cb(&out, cb_ctx);
}

static void nmea_handler(const cJSON* msg, void* ctx) {
    (void)ctx;
    const cJSON* s = cJSON_GetObjectItem(msg, "sentence");
    if (!cJSON_IsString(s) || !s->valuestring)
        return;

#if defined(ESP_PLATFORM)
    /* Reader thread: defer to the pump task (see the threading contract).
     * The gate check happens on the pump side; a sentence enqueued while
     * gated off is dropped there, same observable behavior as an open
     * P-FET. */
    gvirt_ev_push(s->valuestring);
#else
    process_sentence(s->valuestring);
#endif
}

bool gps_has_fix(void) {
    pthread_mutex_lock(&s_mu);
    bool f = s_has_fix;
    pthread_mutex_unlock(&s_mu);
    return f;
}

bool gps_get_position(bramble_position_t* out) {
    if (!out)
        return false;
    bool ok;
    pthread_mutex_lock(&s_mu);
    ok = s_has_fix;
    if (ok)
        *out = s_pos;
    pthread_mutex_unlock(&s_mu);
    return ok;
}

bool gps_get_utc_hm(uint8_t* hour, uint8_t* min) {
    bool ok;
    pthread_mutex_lock(&s_mu);
    ok = s_has_fix && s_utc_valid;
    if (ok) {
        if (hour)
            *hour = s_utc_hour;
        if (min)
            *min = s_utc_min;
    }
    pthread_mutex_unlock(&s_mu);
    return ok;
}

void gps_get_stats(gps_stats_t* out) {
    if (!out)
        return;
    pthread_mutex_lock(&s_mu);
    out->sats_used = s_sats_used;
    out->sats_in_view = s_sats_in_view;
    out->antenna_warning = s_antenna_warning;
    pthread_mutex_unlock(&s_mu);
}

void gps_deinit(void) {
    pthread_mutex_lock(&s_mu);
    s_gate_on = false; /* cut GNSS power (P-FET high) */
    s_has_fix = false;
    s_sats_used = 0;
    s_sats_in_view = 0;
    s_antenna_warning = false;
    s_utc_valid = false;
    memset(&s_np, 0, sizeof(s_np));
    pthread_mutex_unlock(&s_mu);
    send_gpsgate(false);
}

#endif /* host build */
