/*
 * button_virt: the emulator's virtual buttons. It implements the button.h
 * contract for the host (IDF linux target and the plain-gcc harness). The
 * device driver (button.c) polls a GPIO with debounce; here, broker `btn`
 * messages (id = up|down|select|reset, edge = down|up) are turned into the
 * same ui_button_t events that button_poll() returns from the firmware's main
 * loop:
 *
 *   - a press (down) edge on up/down/select enqueues BTN_UP/BTN_DOWN/BTN_SELECT;
 *   - the release (up) edge is not an event (dropped);
 *   - `reset` is NOT a button event: it exits the process cleanly (exit(0)) so
 *     the gosim supervisor restarts the node. Nothing is drained or flushed;
 *     that is the emulated hardware reset button.
 *
 * Thread-safety: the btn handler runs on emu_link's reader thread and enqueues,
 * while button_poll runs on the firmware main-loop thread and dequeues. s_btn_mu
 * guards the ring buffer. The exit path is indirected through s_exit_hook so
 * tests can observe the reset behavior without terminating the test process.
 */

/* CONFIG_IDF_TARGET_LINUX lives in sdkconfig.h; pull it in on IDF builds so
 * the composite host-gate below sees it (same convention as board_config.h /
 * display.h). The plain-gcc test harness has no sdkconfig.h and takes the host
 * branch anyway. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)
/* On-device build: components/button/button.c owns the real GPIO driver. */
#else

#include "button.h"
#include "emu_link.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define BTN_VIRT_QUEUE_CAP 16

static pthread_mutex_t s_btn_mu = PTHREAD_MUTEX_INITIALIZER;
static ui_button_t s_queue[BTN_VIRT_QUEUE_CAP];
static int s_head = 0;
static int s_tail = 0;
static int s_count = 0;

/* Test seam: NULL means the real exit(). Overridden by tests so a reset edge
 * can be asserted without killing the harness. */
static void (*s_exit_hook)(int) = NULL;

static void enqueue(ui_button_t e) {
    pthread_mutex_lock(&s_btn_mu);
    if (s_count < BTN_VIRT_QUEUE_CAP) {
        s_queue[s_tail] = e;
        s_tail = (s_tail + 1) % BTN_VIRT_QUEUE_CAP;
        s_count++;
    }
    /* Full queue: drop the newest event (a backlog that deep means the UI
     * loop has stalled; dropping is preferable to blocking the reader). */
    pthread_mutex_unlock(&s_btn_mu);
}

static void btn_handler(const cJSON *msg, void *ctx) {
    (void)ctx;
    const cJSON *id = cJSON_GetObjectItem(msg, "id");
    if (!cJSON_IsString(id) || !id->valuestring)
        return;
    const char *name = id->valuestring;

    if (strcmp(name, "reset") == 0) {
        /* Clean exit so the supervisor restarts the node. */
        if (s_exit_hook)
            s_exit_hook(0);
        else
            exit(0);
        return;
    }

    /* Dispatch on the press (down) edge only; releases are not events. A
     * missing edge field is treated as a press for forgiving broker input. */
    const cJSON *edge = cJSON_GetObjectItem(msg, "edge");
    if (cJSON_IsString(edge) && edge->valuestring && strcmp(edge->valuestring, "down") != 0)
        return;

    ui_button_t e = BTN_NONE;
    if (strcmp(name, "up") == 0)
        e = BTN_UP;
    else if (strcmp(name, "down") == 0)
        e = BTN_DOWN;
    else if (strcmp(name, "select") == 0)
        e = BTN_SELECT;

    if (e != BTN_NONE)
        enqueue(e);
}

void button_init(void) { emu_link_on("btn", btn_handler, NULL); }

ui_button_t button_poll(uint32_t now_ms) {
    (void)now_ms;
    ui_button_t e = BTN_NONE;
    pthread_mutex_lock(&s_btn_mu);
    if (s_count > 0) {
        e = s_queue[s_head];
        s_head = (s_head + 1) % BTN_VIRT_QUEUE_CAP;
        s_count--;
    }
    pthread_mutex_unlock(&s_btn_mu);
    return e;
}

#endif /* host build */
