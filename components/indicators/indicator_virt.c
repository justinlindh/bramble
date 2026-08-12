/*
 * indicator_virt: the emulator's alert outputs. It implements indicators.h for
 * the host (IDF linux target and the plain-gcc harness). Each setter emits one
 * emu-link `ind` message carrying the FULL (led, buzzer_hz, vibra) state, so
 * the frontend renders a complete snapshot on every change regardless of which
 * field moved (emulator/DESIGN.md sections 5 and 8).
 *
 * Thread-safety: indicator_* may be called from any firmware task. s_ind_mu is
 * held across the read-modify-send so concurrent callers can never interleave
 * into an inconsistent snapshot. emu_link_send is itself thread-safe, so
 * holding s_ind_mu around it introduces no lock-ordering hazard.
 */

/* CONFIG_IDF_TARGET_LINUX lives in sdkconfig.h; pull it in on IDF builds so
 * the composite host-gate below sees it (same convention as board_config.h /
 * display.h). The plain-gcc test harness has no sdkconfig.h and takes the host
 * branch anyway. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)
/* On-device build: indicators.c owns the real GPIO/LEDC wrapper. */
#else

#include "indicators.h"
#include "emu_link.h"

#include <pthread.h>

static pthread_mutex_t s_ind_mu = PTHREAD_MUTEX_INITIALIZER;
static bool s_led = false;
static uint32_t s_buzzer_hz = 0;
static bool s_vibra = false;

/* Emits the current full state as one `ind` message. Call with s_ind_mu held. */
static void send_state_locked(void) {
    cJSON* m = cJSON_CreateObject();
    if (!m)
        return;
    cJSON_AddStringToObject(m, "t", "ind");
    cJSON_AddBoolToObject(m, "led", s_led);
    cJSON_AddNumberToObject(m, "buzzer_hz", (double)s_buzzer_hz);
    cJSON_AddBoolToObject(m, "vibra", s_vibra);
    emu_link_send(m); /* takes ownership on all paths */
}

void indicator_init(void) {
    /* The emu_link connection is owned by the node bootstrap; nothing to set
     * up here. State starts all-off and is reported on the first change. */
}

void indicator_set_led(bool on) {
    pthread_mutex_lock(&s_ind_mu);
    s_led = on;
    send_state_locked();
    pthread_mutex_unlock(&s_ind_mu);
}

void indicator_buzzer(uint32_t hz_or_0) {
    pthread_mutex_lock(&s_ind_mu);
    s_buzzer_hz = hz_or_0;
    send_state_locked();
    pthread_mutex_unlock(&s_ind_mu);
}

void indicator_vibra(bool on) {
    pthread_mutex_lock(&s_ind_mu);
    s_vibra = on;
    send_state_locked();
    pthread_mutex_unlock(&s_ind_mu);
}

#endif /* host build */
