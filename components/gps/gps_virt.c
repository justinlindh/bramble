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
 * Thread-safety: the nmea handler runs on emu_link's reader thread, while
 * gps_get_position / gps_has_fix / gps_get_stats are called from firmware
 * tasks (mesh_task, the main UI loop). s_mu guards all shared state; the fix
 * callback is invoked with the lock dropped so a callback that reads position
 * back cannot deadlock.
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

#define GPS_VIRT_MAX_LINE 128

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static gps_fix_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static bramble_position_t s_pos = {0};
static nmea_position_t s_np = {0}; /* running accumulator across sentences */
static bool s_has_fix = false;
static bool s_gate_on = false; /* GPS power gate: false = powered off */
static uint8_t s_sats_used = 0;
static uint8_t s_sats_in_view = 0;
static bool s_antenna_warning = false;

static void nmea_handler(const cJSON *msg, void *ctx);

static void send_gpsgate(bool on) {
    cJSON *m = cJSON_CreateObject();
    if (!m)
        return;
    cJSON_AddStringToObject(m, "t", "gpsgate");
    cJSON_AddBoolToObject(m, "on", on);
    emu_link_send(m); /* takes ownership on all paths */
}

int gps_init(gps_fix_cb_t cb, void *ctx) {
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

    /* Registering the same handler twice is idempotent (emu_link replaces).
     * The callback registered by a prior gps_init() is retained across a
     * disable, so re-enabling resumes fixes without re-registering. */
    emu_link_on("nmea", nmea_handler, NULL);
    send_gpsgate(true);
    return 0;
}

static void nmea_handler(const cJSON *msg, void *ctx) {
    (void)ctx;
    const cJSON *s = cJSON_GetObjectItem(msg, "sentence");
    if (!cJSON_IsString(s) || !s->valuestring)
        return;

    pthread_mutex_lock(&s_mu);
    if (!s_gate_on) {
        pthread_mutex_unlock(&s_mu);
        return; /* powered off: no sentences, like an open P-FET */
    }
    nmea_position_t np = s_np; /* accumulator snapshot */
    pthread_mutex_unlock(&s_mu);

    char line[GPS_VIRT_MAX_LINE];
    strncpy(line, s->valuestring, sizeof(line) - 1);
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
    void *cb_ctx = NULL;
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
        cb = s_cb;
        cb_ctx = s_cb_ctx;
        out = s_pos;
    }
    pthread_mutex_unlock(&s_mu);

    if (cb)
        cb(&out, cb_ctx);
}

bool gps_has_fix(void) {
    pthread_mutex_lock(&s_mu);
    bool f = s_has_fix;
    pthread_mutex_unlock(&s_mu);
    return f;
}

bool gps_get_position(bramble_position_t *out) {
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

void gps_get_stats(gps_stats_t *out) {
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
    memset(&s_np, 0, sizeof(s_np));
    pthread_mutex_unlock(&s_mu);
    send_gpsgate(false);
}

#endif /* host build */
