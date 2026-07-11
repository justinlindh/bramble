/*
 * Virtual radio backend for the Bramble emulator (emulator/DESIGN.md
 * sections 5 and 8). Implements components/radio/include/radio.h and
 * radio_transmit_raw (radio_internal.h) on top of the emu_link broker
 * client, replacing the spike's emulator/node/radio_null.c.
 *
 *   TX   radio_transmit_raw serializes the frame plus the active
 *        radio_config_t's modulation params (freq, sf, bw, cr, power) as a
 *        `tx` message and blocks until the broker prices time-on-air with a
 *        `txdone`. The broker is authoritative for airtime; it uses the same
 *        radio_airtime function this firmware does, so its price agrees with
 *        the tx_gate's local ToA accounting.
 *   RX   inbound `rx` messages decode the base64 frame and fire the
 *        registered rx callback with the broker's RSSI/SNR.
 *   CAD  radio_cad_check sends `cad` and waits for `cadres` (channel-busy
 *        verdict); radio_cad() is the async form firing the cad-done callback.
 *
 * Every transmission still funnels through the real tx_gate: tx_gate_esp.c
 * calls radio_transmit_raw as its transmit op, so the budget / LBT / ToA-debit
 * chokepoint is unchanged and this file never bypasses it.
 *
 * Threading: emu_link's reader thread runs the rx/txdone/cadres handlers.
 * radio_transmit_raw and radio_cad_check are called from the sender's thread
 * (through the tx_gate mutex) and block on a condvar the handlers signal, so
 * the tx-done callback fires on the caller's thread exactly as on device
 * (radio_esp.c), while the rx callback fires on the reader thread just as the
 * device's rx callback fires from the radio task. mesh_task's registered
 * consumers (on_rx queue-posts an rx_packet_t, on_tx_done just logs) do only
 * bounded work, so the reader thread never blocks.
 *
 * Host-only: compiled into the radio component only on the IDF linux target
 * (see CMakeLists.txt) and #included directly by test/test_radio_virt.c. On a
 * real esp32s3 device this file compiles to nothing; radio_esp.c owns the
 * SX1262. radio_get_profile_config lives in radio_profiles.c (single source
 * of truth shared by both drivers and the tests).
 */
#include "radio.h"
#include "radio_internal.h"
#include "emu_link.h"

/* sdkconfig.h carries CONFIG_IDF_TARGET_LINUX, the macro the guard below keys
 * on. It only exists under IDF; the plain-gcc test harness (no ESP_PLATFORM)
 * skips it and takes the host branch unconditionally. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Defense in depth: the CMake linux-branch gate already keeps this file out
 * of device builds; this makes a stray device compile a no-op instead of a
 * duplicate-symbol clash with radio_esp.c (mirror of emu_link.c's guard). */
#if !defined(ESP_PLATFORM) || defined(CONFIG_IDF_TARGET_LINUX)

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Real esp_log on the node; no-ops in the plain-gcc test harness (which does
 * not carry the debug log macros). */
#if defined(ESP_PLATFORM)
#include "esp_log.h"
static const char* TAG = "radio_virt";
#else
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#endif

#define RADIO_VIRT_TX_TIMEOUT_MS 5000u
#define RADIO_VIRT_CAD_TIMEOUT_MS 50u

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static radio_config_t s_config;
static atomic_int s_state = RADIO_STATE_IDLE; /* radio_state_t */
static radio_rx_callback_t s_rx_cb;
static radio_tx_done_callback_t s_tx_done_cb;
static radio_cad_done_callback_t s_cad_done_cb;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_tx_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t s_cad_cv = PTHREAD_COND_INITIALIZER;
static bool s_tx_done;
static bool s_cad_done;
static bool s_cad_busy;
static bool s_cad_async; /* an async radio_cad() is awaiting its cadres */

/* ------------------------------------------------------------------ */
/*  base64 (RFC 4648, standard alphabet, padded)                       */
/* ------------------------------------------------------------------ */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encodes n bytes into out (NUL-terminated). out_sz must hold 4*ceil(n/3)+1. */
static size_t b64_encode(const uint8_t* in, size_t n, char* out, size_t out_sz) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        if (o + 4 >= out_sz)
            break;
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n)
            v |= (uint32_t)in[i + 2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

/* Decodes a NUL-terminated base64 string into out, returning byte count
 * (never more than out_sz). Skips whitespace; stops at '=' padding. */
static size_t b64_decode(const char* in, uint8_t* out, size_t out_sz) {
    size_t o = 0;
    int quad[4];
    int qi = 0;
    for (const char* p = in; *p; p++) {
        if (*p == '=')
            break;
        int v = b64_val(*p);
        if (v < 0)
            continue; /* skip whitespace / stray chars */
        quad[qi++] = v;
        if (qi == 4) {
            if (o + 3 > out_sz)
                return o;
            out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
            out[o++] = (uint8_t)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
            out[o++] = (uint8_t)(((quad[2] & 0x3) << 6) | quad[3]);
            qi = 0;
        }
    }
    if (qi >= 2 && o < out_sz)
        out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
    if (qi >= 3 && o < out_sz)
        out[o++] = (uint8_t)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
    return o;
}

static void deadline_in_ms(struct timespec* ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += (time_t)(ms / 1000u);
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* ------------------------------------------------------------------ */
/*  emu_link inbound handlers (reader thread)                          */
/* ------------------------------------------------------------------ */

static void on_rx_msg(const cJSON* msg, void* ctx) {
    (void)ctx;
    const cJSON* p = cJSON_GetObjectItem(msg, "payload");
    if (!cJSON_IsString(p) || !p->valuestring)
        return;

    uint8_t buf[256];
    size_t n = b64_decode(p->valuestring, buf, sizeof(buf));
    if (n == 0 || n > 255)
        return; /* radio frames are never empty and cap at 255 bytes */

    radio_rx_info_t info = {0};
    info.len = (uint8_t)n;
    const cJSON* rssi = cJSON_GetObjectItem(msg, "rssi");
    const cJSON* snr = cJSON_GetObjectItem(msg, "snr");
    info.rssi = cJSON_IsNumber(rssi) ? (int16_t)rssi->valuedouble : 0;
    info.snr = cJSON_IsNumber(snr) ? (int8_t)snr->valuedouble : 0;

    radio_rx_callback_t cb = s_rx_cb;
    if (cb)
        cb(buf, (uint8_t)n, &info);
}

static void on_txdone_msg(const cJSON* msg, void* ctx) {
    (void)ctx;
    (void)msg; /* toa_ms is the broker's accounting; the node just unblocks */
    pthread_mutex_lock(&s_mu);
    s_tx_done = true;
    pthread_cond_signal(&s_tx_cv);
    pthread_mutex_unlock(&s_mu);
}

static void on_cadres_msg(const cJSON* msg, void* ctx) {
    (void)ctx;
    const cJSON* busy = cJSON_GetObjectItem(msg, "busy");
    bool b;
    if (cJSON_IsBool(busy))
        b = cJSON_IsTrue(busy);
    else if (cJSON_IsNumber(busy))
        b = busy->valuedouble != 0;
    else
        b = false;

    bool async;
    pthread_mutex_lock(&s_mu);
    s_cad_busy = b;
    s_cad_done = true;
    async = s_cad_async;
    s_cad_async = false;
    pthread_cond_signal(&s_cad_cv);
    pthread_mutex_unlock(&s_mu);

    /* Async radio_cad() path: deliver the verdict to the cad-done callback and
     * return to IDLE. The synchronous radio_cad_check() path never sets
     * s_cad_async, so it consumes the condvar signal instead. */
    if (async) {
        atomic_store(&s_state, RADIO_STATE_IDLE);
        radio_cad_done_callback_t cb = s_cad_done_cb;
        if (cb)
            cb(b);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int radio_init(const radio_config_t* config) {
    s_config = *config;

    emu_link_on("rx", on_rx_msg, NULL);
    emu_link_on("txdone", on_txdone_msg, NULL);
    emu_link_on("cadres", on_cadres_msg, NULL);

    atomic_store(&s_state, RADIO_STATE_IDLE);
    ESP_LOGI(TAG, "virtual radio up: %.1f MHz SF%u BW %lu", (double)config->frequency_mhz,
             config->sf, (unsigned long)config->bw_hz);

    radio_start_rx();
    return 0;
}

int radio_reconfigure(const radio_config_t* config) {
    s_config = *config;
    radio_start_rx();
    return 0;
}

void radio_get_config(radio_config_t* config) { *config = s_config; }

int radio_transmit_raw(const uint8_t* data, uint8_t len) {
    atomic_store(&s_state, RADIO_STATE_TX);

    pthread_mutex_lock(&s_mu);
    s_tx_done = false;
    pthread_mutex_unlock(&s_mu);

    cJSON* tx = cJSON_CreateObject();
    if (!tx) {
        atomic_store(&s_state, RADIO_STATE_IDLE);
        radio_start_rx();
        return -1;
    }
    char b64[352]; /* 4*ceil(255/3)+1 = 341, rounded up */
    b64_encode(data, len, b64, sizeof(b64));
    cJSON_AddStringToObject(tx, "t", "tx");
    cJSON_AddStringToObject(tx, "payload", b64);
    cJSON_AddNumberToObject(tx, "freq", s_config.frequency_mhz);
    cJSON_AddNumberToObject(tx, "sf", s_config.sf);
    cJSON_AddNumberToObject(tx, "bw", s_config.bw_hz);
    cJSON_AddNumberToObject(tx, "cr", s_config.coding_rate);
    cJSON_AddNumberToObject(tx, "power", s_config.tx_power);

    if (emu_link_send(tx) != 0) {
        ESP_LOGW(TAG, "tx send failed (broker down?)");
        atomic_store(&s_state, RADIO_STATE_IDLE);
        radio_start_rx();
        return -1;
    }

    struct timespec deadline;
    deadline_in_ms(&deadline, RADIO_VIRT_TX_TIMEOUT_MS);
    int wrc = 0;
    pthread_mutex_lock(&s_mu);
    while (!s_tx_done && wrc == 0)
        wrc = pthread_cond_timedwait(&s_tx_cv, &s_mu, &deadline);
    bool done = s_tx_done;
    pthread_mutex_unlock(&s_mu);

    if (!done) {
        ESP_LOGE(TAG, "tx timed out waiting for txdone");
        atomic_store(&s_state, RADIO_STATE_IDLE);
        radio_start_rx();
        return -1;
    }

    /* Mirror radio_esp.c: TX complete -> fire the caller-thread tx-done
     * callback, then resume continuous RX. */
    atomic_store(&s_state, RADIO_STATE_IDLE);
    if (s_tx_done_cb)
        s_tx_done_cb();
    radio_start_rx();
    return 0;
}

void radio_start_rx(void) { atomic_store(&s_state, RADIO_STATE_RX); }

void radio_cad(void) {
    pthread_mutex_lock(&s_mu);
    s_cad_done = false;
    s_cad_async = true;
    pthread_mutex_unlock(&s_mu);
    atomic_store(&s_state, RADIO_STATE_CAD);

    cJSON* cad = cJSON_CreateObject();
    if (!cad) {
        pthread_mutex_lock(&s_mu);
        s_cad_async = false;
        pthread_mutex_unlock(&s_mu);
        atomic_store(&s_state, RADIO_STATE_IDLE);
        return;
    }
    cJSON_AddStringToObject(cad, "t", "cad");
    cJSON_AddNumberToObject(cad, "freq", s_config.frequency_mhz);
    if (emu_link_send(cad) != 0) {
        pthread_mutex_lock(&s_mu);
        s_cad_async = false;
        pthread_mutex_unlock(&s_mu);
        atomic_store(&s_state, RADIO_STATE_IDLE);
    }
}

bool radio_cad_check(void) {
    pthread_mutex_lock(&s_mu);
    s_cad_done = false;
    s_cad_async = false;
    pthread_mutex_unlock(&s_mu);
    atomic_store(&s_state, RADIO_STATE_CAD);

    cJSON* cad = cJSON_CreateObject();
    if (!cad) {
        radio_start_rx();
        return false;
    }
    cJSON_AddStringToObject(cad, "t", "cad");
    cJSON_AddNumberToObject(cad, "freq", s_config.frequency_mhz);
    if (emu_link_send(cad) != 0) {
        radio_start_rx();
        return false;
    }

    struct timespec deadline;
    deadline_in_ms(&deadline, RADIO_VIRT_CAD_TIMEOUT_MS);
    int wrc = 0;
    pthread_mutex_lock(&s_mu);
    while (!s_cad_done && wrc == 0)
        wrc = pthread_cond_timedwait(&s_cad_cv, &s_mu, &deadline);
    bool done = s_cad_done;
    bool busy = s_cad_busy;
    pthread_mutex_unlock(&s_mu);

    /* Timeout fails safe to "clear" so a silent broker never starves TX,
     * matching radio_esp.c's CAD-timeout behavior. */
    radio_start_rx();
    return done ? busy : false;
}

void radio_set_tx_power(int8_t power) { s_config.tx_power = power; }

radio_state_t radio_get_state(void) { return (radio_state_t)atomic_load(&s_state); }

void radio_set_rx_callback(radio_rx_callback_t cb) { s_rx_cb = cb; }

void radio_set_tx_done_callback(radio_tx_done_callback_t cb) { s_tx_done_cb = cb; }

void radio_set_cad_done_callback(radio_cad_done_callback_t cb) { s_cad_done_cb = cb; }

void radio_sleep(void) { atomic_store(&s_state, RADIO_STATE_SLEEP); }

bool radio_check_and_clear_reinit(void) { return false; }

#endif /* host build (IDF linux target or plain-gcc test harness) */
