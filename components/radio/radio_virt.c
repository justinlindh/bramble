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
 * (through the tx_gate mutex) and poll a flag the handlers set, so the
 * tx-done callback fires on the caller's thread exactly as on device
 * (radio_esp.c). The rx and async cad-done callbacks do NOT fire on the
 * reader thread: it is a raw pthread and must never enter FreeRTOS, so those
 * events are deferred through a pthread ring to a FreeRTOS pump task (see
 * the THREADING CONTRACT comment above the handlers), which then invokes the
 * registered callbacks from legal task context.
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char* TAG = "radio_virt";
#else
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#endif

#define RADIO_VIRT_TX_TIMEOUT_MS 8000u
/* txdone poll interval on the node. The broker's reply is delivered by the
 * emu_link reader thread (a raw pthread); signalling a condvar from it does not
 * reliably wake a blocked FreeRTOS task under the IDF-linux port (the scheduler
 * suspends non-running task pthreads), so the transmitting task polls the flag
 * with vTaskDelay, a real scheduler yield, instead of blocking on the condvar. */
#define RADIO_VIRT_TX_POLL_MS 5u

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

/* The virtual radio imposes no PA limit of its own, so it enforces the range
 * the physical fleet uses. Advertising a range without enforcing it would let
 * the emulator and the simulator run at powers no real node could program,
 * which is exactly the divergence they exist to rule out. */
#define RADIO_VIRT_TX_POWER_MIN_DBM (-9)
#define RADIO_VIRT_TX_POWER_MAX_DBM 22

static int8_t clamp_and_log_tx_power(int8_t requested) {
    int8_t clamped =
        radio_clamp_tx_power(requested, RADIO_VIRT_TX_POWER_MIN_DBM, RADIO_VIRT_TX_POWER_MAX_DBM);
    if (clamped != requested) {
        ESP_LOGW(TAG, "TX power %d dBm outside range %d..%d, using %d dBm", requested,
                 RADIO_VIRT_TX_POWER_MIN_DBM, RADIO_VIRT_TX_POWER_MAX_DBM, clamped);
    }
    return clamped;
}

static radio_config_t s_config;
static atomic_int s_state = RADIO_STATE_IDLE; /* radio_state_t */
static radio_rx_callback_t s_rx_cb;
static radio_tx_done_callback_t s_tx_done_cb;
static radio_cad_done_callback_t s_cad_done_cb;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;

/* Task-side acquisition of s_mu on the node: trylock and yield a tick on
 * contention, NEVER a blocking futex wait. A FreeRTOS task blocked in a
 * foreign futex desyncs the linux port's current-task tracking (the
 * suspension signal EINTRs the wait and the task can resume while another
 * task is considered current), which detonates later FreeRTOS asserts such
 * as xTaskPriorityDisinherit's holder check when the task next gives a
 * mutex. s_mu is only ever held for flag reads/writes, so the trylock
 * virtually always succeeds on the first attempt; the reader thread (no TCB,
 * nothing to desync) keeps plain pthread_mutex_lock. Plain-gcc harness has
 * no scheduler and also keeps the plain lock. */
#if defined(ESP_PLATFORM)
static void mu_lock_task(void) {
    while (pthread_mutex_trylock(&s_mu) != 0)
        vTaskDelay(1);
}
#else
static void mu_lock_task(void) { pthread_mutex_lock(&s_mu); }
#endif
static pthread_cond_t s_tx_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t s_cad_cv = PTHREAD_COND_INITIALIZER;
/* Outstanding-txdone COUNTER, not a bool: a txdone that arrives after its TX
 * already timed out (and was counted as sent) must not release the NEXT
 * transmit's wait early. With a bool, a late txdone under sustained broker
 * lag lets the sender run one-ahead of time-on-air pacing indefinitely
 * (each TX consumes the PREVIOUS frame's txdone the instant it starts
 * waiting). The counter makes staleness self-accounting: each send
 * increments, each txdone decrements, and a waiter waits for zero, so a
 * stale txdone merely cancels the stale send it belongs to. */
static int s_txdone_outstanding;
static bool s_cad_done;
static bool s_cad_busy;
static bool s_cad_async; /* an async radio_cad() is awaiting its cadres */

/* CAD-timeout fail-open/closed policy, mirroring radio_esp.c so the emulator
 * exercises the same decision. The counter is only touched on the transmitting
 * task, so it needs no lock; the reinit flag is set there and
 * read/cleared by radio_check_and_clear_reinit() on the mesh task, so it is
 * atomic. */
static cad_timeout_policy_t s_cad_timeout_policy;
static atomic_bool s_needs_reinit;

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

/* THREADING CONTRACT (the hard-won part): emu_link's reader is a RAW pthread,
 * not a FreeRTOS task, and on the IDF-linux FreeRTOS port a foreign thread
 * must never call FreeRTOS APIs. The port keeps global scheduler state
 * (uxCriticalNesting in port.c) that only registered task pthreads may touch;
 * an xQueueSend from the reader thread races the scheduler and intermittently
 * dies on `vPortExitCritical: Assertion uxCriticalNesting >= 0' failed`, or
 * silently wedges the scheduler (both observed on CI: node processes exiting
 * with status 1 mid-scenario, and "mute" nodes that received frames but whose
 * mesh task never ran again). The app's rx callback (mesh_task's on_rx) and
 * the async cad-done callback both land in FreeRTOS queues, so the reader
 * thread must NOT invoke them directly. Instead the handlers below push into
 * a plain pthread-mutex ring (same pattern button_virt uses for the same
 * reason) and a FreeRTOS pump task drains it and runs the callbacks from
 * legal task context. txdone/cadres flag signalling stays on the reader
 * thread: pthread primitives only, no FreeRTOS.
 *
 * The plain-gcc test harness (no ESP_PLATFORM) has no scheduler and its
 * socketpair tests drive dispatch synchronously, so it keeps the direct
 * call. */

#if defined(ESP_PLATFORM)
#define RVIRT_EVQ_CAP 64
typedef struct {
    uint8_t kind; /* 0 = rx frame, 1 = async cad verdict */
    uint8_t len;
    int16_t rssi;
    int8_t snr;
    bool cad_busy;
    uint8_t data[256];
} rvirt_ev_t;

static pthread_mutex_t s_evq_mu = PTHREAD_MUTEX_INITIALIZER;
static rvirt_ev_t s_evq[RVIRT_EVQ_CAP];
static int s_evq_head = 0, s_evq_tail = 0, s_evq_count = 0;
static unsigned s_evq_dropped = 0; /* logged from the pump task, not here */
static bool s_pump_started = false;

/* Reader thread: enqueue only (pthread mutex, no FreeRTOS, no logging). A
 * full ring drops the newest event and counts it; on the virtual ether that
 * is indistinguishable from RF loss and the mesh's retry machinery owns it. */
static void rvirt_ev_push(const rvirt_ev_t* ev) {
    pthread_mutex_lock(&s_evq_mu);
    if (s_evq_count < RVIRT_EVQ_CAP) {
        s_evq[s_evq_tail] = *ev;
        s_evq_tail = (s_evq_tail + 1) % RVIRT_EVQ_CAP;
        s_evq_count++;
    } else {
        s_evq_dropped++;
    }
    pthread_mutex_unlock(&s_evq_mu);
}

/* Pump task: the only place reader-thread events meet FreeRTOS. Polls with
 * vTaskDelay (a condvar cannot wake a FreeRTOS task on this port, see the
 * txdone poll note above RADIO_VIRT_TX_POLL_MS). */
static void rvirt_pump_task(void* arg) {
    (void)arg;
    for (;;) {
        rvirt_ev_t ev;
        bool have;
        unsigned dropped;
        pthread_mutex_lock(&s_evq_mu);
        have = s_evq_count > 0;
        if (have) {
            ev = s_evq[s_evq_head];
            s_evq_head = (s_evq_head + 1) % RVIRT_EVQ_CAP;
            s_evq_count--;
        }
        dropped = s_evq_dropped;
        s_evq_dropped = 0;
        pthread_mutex_unlock(&s_evq_mu);

        if (dropped > 0)
            ESP_LOGW(TAG, "inbound event ring overflowed; dropped %u event(s)", dropped);
        if (!have) {
            vTaskDelay(1);
            continue;
        }
        if (ev.kind == 0) {
            radio_rx_info_t info = {0};
            info.len = ev.len;
            info.rssi = ev.rssi;
            info.snr = ev.snr;
            radio_rx_callback_t cb = s_rx_cb;
            if (cb)
                cb(ev.data, ev.len, &info);
        } else {
            radio_cad_done_callback_t cb = s_cad_done_cb;
            if (cb)
                cb(ev.cad_busy);
        }
    }
}
#endif /* ESP_PLATFORM */

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

#if defined(ESP_PLATFORM)
    /* Reader thread: defer to the pump task (see the threading contract). */
    rvirt_ev_t ev = {0};
    ev.kind = 0;
    ev.len = (uint8_t)n;
    ev.rssi = info.rssi;
    ev.snr = info.snr;
    memcpy(ev.data, buf, n);
    rvirt_ev_push(&ev);
#else
    radio_rx_callback_t cb = s_rx_cb;
    if (cb)
        cb(buf, (uint8_t)n, &info);
#endif
}

static void on_txdone_msg(const cJSON* msg, void* ctx) {
    (void)ctx;
    (void)msg; /* toa_ms is the broker's accounting; the node just unblocks */
    pthread_mutex_lock(&s_mu);
    if (s_txdone_outstanding > 0)
        s_txdone_outstanding--;
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
     * s_cad_async, so it consumes the condvar signal instead. On the node the
     * callback lands in FreeRTOS-managed code, so it must run on the pump
     * task, not this reader thread (see the threading contract above). */
    if (async) {
        atomic_store(&s_state, RADIO_STATE_IDLE);
#if defined(ESP_PLATFORM)
        rvirt_ev_t ev = {0};
        ev.kind = 1;
        ev.cad_busy = b;
        rvirt_ev_push(&ev);
#else
        radio_cad_done_callback_t cb = s_cad_done_cb;
        if (cb)
            cb(b);
#endif
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int radio_init(const radio_config_t* config) {
    radio_config_t applied = *config;
    applied.tx_power = clamp_and_log_tx_power(applied.tx_power);
    s_config = applied;

    emu_link_on("rx", on_rx_msg, NULL);
    emu_link_on("txdone", on_txdone_msg, NULL);
    emu_link_on("cadres", on_cadres_msg, NULL);

#if defined(ESP_PLATFORM)
    /* One pump task per process, surviving radio_reconfigure: it is the only
     * legal FreeRTOS context for the reader thread's rx/cad events (see the
     * threading contract above the handlers). */
    if (!s_pump_started) {
        if (xTaskCreate(rvirt_pump_task, "rvirt_pump", 4096, NULL, 10, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create rx pump task");
            return -1;
        }
        s_pump_started = true;
    }
#endif

    atomic_store(&s_state, RADIO_STATE_IDLE);
    ESP_LOGI(TAG, "virtual radio up: %.1f MHz SF%u BW %lu", (double)config->frequency_mhz,
             config->sf, (unsigned long)config->bw_hz);

    radio_start_rx();
    return 0;
}

int radio_reconfigure(const radio_config_t* config) {
    radio_config_t applied = *config;
    applied.tx_power = clamp_and_log_tx_power(applied.tx_power);
    s_config = applied;
    radio_start_rx();
    return 0;
}

void radio_get_config(radio_config_t* config) { *config = s_config; }

int radio_transmit_raw(const uint8_t* data, uint8_t len) {
    atomic_store(&s_state, RADIO_STATE_TX);

    mu_lock_task();
    s_txdone_outstanding++;
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
        mu_lock_task();
        if (s_txdone_outstanding > 0)
            s_txdone_outstanding--; /* no txdone will ever come for this one */
        pthread_mutex_unlock(&s_mu);
        atomic_store(&s_state, RADIO_STATE_IDLE);
        radio_start_rx();
        return -1;
    }

    bool done = false;
#if defined(ESP_PLATFORM)
    /* Node (FreeRTOS): poll the flag with vTaskDelay rather than block on the
     * condvar, so the reader thread's flag write is observed promptly (see
     * RADIO_VIRT_TX_POLL_MS). */
    for (uint32_t waited = 0; waited < RADIO_VIRT_TX_TIMEOUT_MS; waited += RADIO_VIRT_TX_POLL_MS) {
        mu_lock_task();
        done = (s_txdone_outstanding == 0);
        pthread_mutex_unlock(&s_mu);
        if (done)
            break;
        vTaskDelay(pdMS_TO_TICKS(RADIO_VIRT_TX_POLL_MS));
    }
#else
    /* Plain-gcc test harness (no FreeRTOS): the condvar wait is correct and the
     * signaller runs on an ordinary pthread. */
    struct timespec deadline;
    deadline_in_ms(&deadline, RADIO_VIRT_TX_TIMEOUT_MS);
    int wrc = 0;
    pthread_mutex_lock(&s_mu);
    while (s_txdone_outstanding > 0 && wrc == 0)
        wrc = pthread_cond_timedwait(&s_tx_cv, &s_mu, &deadline);
    done = (s_txdone_outstanding == 0);
    pthread_mutex_unlock(&s_mu);
#endif

    if (!done) {
        /* A late txdone is NOT a failed transmission on the virtual radio.
         * The broker folds the frame into the ether the moment it receives
         * the tx message (gosim handleTx calls sim_radio_broadcast BEFORE
         * scheduling the txdone reply), so by the time this wait even
         * started, the frame had already been delivered to every receiver
         * in range; txdone only paces the sender. Under CPU-throttled CI the
         * broker's reply can lag arbitrarily, and treating that latency as a
         * TX failure caused false "Beacon TX failed" cascades and mesh-layer
         * retransmit storms for frames that had in fact been delivered
         * (observed on the CI pods: a receiver serialized behind these
         * timeouts transmitted 3 frames in 80s and could never complete a DM
         * handshake). The real SX1262 driver treats a missing TX-done IRQ as
         * fatal because there the frame really may not have left the chip;
         * the virtual radio's contract is the opposite, so warn and count
         * the TX as sent.
         *
         * s_txdone_outstanding is intentionally NOT decremented here: gosim
         * always schedules the late txdone eventually (scheduleBrokerAction
         * in extnode.go), so the counter self-corrects when it lands. Under
         * sustained broker lag each unresolved timeout carries +1 of debt,
         * making subsequent sends wait the full timeout until the backlog
         * drains; that slower-but-correct pacing is expected and absorbed
         * by the scenario budgets. */
        ESP_LOGW(TAG,
                 "txdone still pending after %ums; counting TX as sent (virtual ether delivers at "
                 "tx start)",
                 RADIO_VIRT_TX_TIMEOUT_MS);
        atomic_store(&s_state, RADIO_STATE_IDLE);
        if (s_tx_done_cb)
            s_tx_done_cb();
        radio_start_rx();
        return 0;
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
    mu_lock_task();
    s_cad_done = false;
    s_cad_async = true;
    pthread_mutex_unlock(&s_mu);
    atomic_store(&s_state, RADIO_STATE_CAD);

    cJSON* cad = cJSON_CreateObject();
    if (!cad) {
        mu_lock_task();
        s_cad_async = false;
        pthread_mutex_unlock(&s_mu);
        atomic_store(&s_state, RADIO_STATE_IDLE);
        return;
    }
    cJSON_AddStringToObject(cad, "t", "cad");
    cJSON_AddNumberToObject(cad, "freq", s_config.frequency_mhz);
    if (emu_link_send(cad) != 0) {
        mu_lock_task();
        s_cad_async = false;
        pthread_mutex_unlock(&s_mu);
        atomic_store(&s_state, RADIO_STATE_IDLE);
    }
}

bool radio_cad_check(void) {
    mu_lock_task();
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

    bool done = false;
    bool busy = false;
    /* Derived from the live radio config exactly as radio_esp.c does it, so
     * the emulator surfaces an SF-dependent CAD stall instead of hiding it
     * behind a fixed budget. */
    uint32_t cad_timeout_ms =
        bramble_cad_timeout_ms(s_config.sf, s_config.bw_hz, BRAMBLE_CAD_SYMBOL_NUM_REG);
#if defined(ESP_PLATFORM)
    /* Node: poll the flag with vTaskDelay, NEVER a condvar wait. The caller
     * is a FreeRTOS task, typically holding the tx_gate FreeRTOS mutex, and
     * blocking a task in a foreign futex desyncs the linux port's
     * current-task tracking: the port's suspension signal EINTRs the futex
     * wait, and the task can resume execution while the kernel considers
     * another task current. The tx_gate mutex give after this call is then
     * exactly where FreeRTOS's `xTaskPriorityDisinherit: pxTCB ==
     * pxCurrentTCBs[0]' assertion fires (captured on CI, run 29642008877,
     * crash-looping a node at radio start). Same rule and pattern as the
     * txdone poll above; a tick of extra CAD latency is irrelevant here. */
    for (uint32_t waited = 0; waited < cad_timeout_ms;) {
        mu_lock_task();
        done = s_cad_done;
        busy = s_cad_busy;
        pthread_mutex_unlock(&s_mu);
        if (done)
            break;
        vTaskDelay(1);
        uint32_t tick_ms = (uint32_t)portTICK_PERIOD_MS;
        waited += (tick_ms > 0) ? tick_ms : 1;
    }
#else
    /* Plain-gcc test harness: no scheduler to desync; the condvar is fine. */
    struct timespec deadline;
    deadline_in_ms(&deadline, cad_timeout_ms);
    int wrc = 0;
    pthread_mutex_lock(&s_mu);
    while (!s_cad_done && wrc == 0)
        wrc = pthread_cond_timedwait(&s_cad_cv, &s_mu, &deadline);
    done = s_cad_done;
    busy = s_cad_busy;
    pthread_mutex_unlock(&s_mu);
#endif

    radio_start_rx();

    if (!done) {
        /* Fail-open/closed policy, identical to radio_esp.c: the first
         * BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD-1 consecutive timeouts
         * fail open (transmit anyway) so a briefly silent broker never starves
         * TX; the threshold-th fails closed (report busy) and flags a reinit. */
        cad_timeout_action_t action = cad_timeout_policy_on_timeout(&s_cad_timeout_policy);
        if (action == CAD_TIMEOUT_FAIL_CLOSED) {
            ESP_LOGE(TAG, "CAD timed out %u consecutive times, failing closed, requesting reinit",
                     (unsigned)BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD);
            atomic_store(&s_needs_reinit, true);
            return true;
        }
        return false;
    }

    /* A completed CAD means the broker answered; reset the timeout streak. */
    cad_timeout_policy_on_success(&s_cad_timeout_policy);
    return busy;
}

void radio_set_tx_power(int8_t power) { s_config.tx_power = clamp_and_log_tx_power(power); }

/* There is no radio chip behind the virtual driver, so there is nothing to
 * interrogate. Report the programmed power and supported=false rather than
 * inventing verdicts, so a caller cannot mistake the emulator for evidence
 * about a real transmit path. */
int radio_get_health(radio_health_t* health) {
    if (!health)
        return -1;
    memset(health, 0, sizeof(*health));
    health->supported = false;
    health->tx_power_dbm = s_config.tx_power;
    return 0;
}

/* The virtual radio imposes no PA limit of its own, so it reports the range
 * the physical fleet uses; that keeps emulator configs interchangeable with
 * hardware ones instead of accepting values no real node could program. */
int8_t radio_tx_power_min_dbm(void) { return RADIO_VIRT_TX_POWER_MIN_DBM; }
int8_t radio_tx_power_max_dbm(void) { return RADIO_VIRT_TX_POWER_MAX_DBM; }

radio_state_t radio_get_state(void) { return (radio_state_t)atomic_load(&s_state); }

void radio_set_rx_callback(radio_rx_callback_t cb) { s_rx_cb = cb; }

void radio_set_tx_done_callback(radio_tx_done_callback_t cb) { s_tx_done_cb = cb; }

void radio_sleep(void) { atomic_store(&s_state, RADIO_STATE_SLEEP); }

bool radio_check_and_clear_reinit(void) {
    /* Honor the CAD-timeout fail-closed path: if a run of CAD timeouts flagged
     * the radio as wedged, report it once so the mesh loop
     * runs radio_reconfigure, and clear the flag. Normal emulator runs never
     * set it, since the broker answers every CAD.
     *
     * No radio_reinit_policy_t here, unlike the SX1262 and LR1110 backends.
     * That policy exists because a hardware recovery can fail and must then be
     * retried rather than dropped; there is no chip to bring up here, nothing
     * to fail, and so nothing to owe a retry. Clearing unconditionally is
     * correct for this backend. */
    return atomic_exchange(&s_needs_reinit, false);
}

#endif /* host build (IDF linux target or plain-gcc test harness) */
