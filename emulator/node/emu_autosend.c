/*
 * Emulator scripted send for the IDF linux target.
 *
 * A headless scenario needs a node to originate a message on cue, but the
 * emu-link protocol has no compose/send RPC and driving the button UI to type a
 * message char-by-char is neither deterministic nor practical for CI. This is
 * the emulator's programmatic stand-in for a compose-and-send: after a delay it
 * calls the SAME public mesh send API the compose UI, CLI, and RPC all call
 * (mesh_send_broadcast / mesh_send_message), so the real encrypt / mesh / tx
 * path is exercised end to end; only the trigger is scripted.
 *
 * Driven entirely by environment (a scenario sets these per node via the
 * firmware-node "env" map):
 *   EMU_AUTO_SEND            message text; unset/blank => this node never sends
 *   EMU_AUTO_SEND_TO         hex dest addr => DM via mesh_send_message;
 *                            unset => channel broadcast via mesh_send_broadcast
 *   EMU_AUTO_SEND_DELAY_MS   delay before the first send (default 12000). Must
 *                            clear UI_MESSAGE_IDLE_THRESHOLD_MS on the receiver
 *                            so the inbound message auto-opens its Messages
 *                            screen (10s), which is what a screen assertion sees.
 *   EMU_AUTO_SEND_REPEAT     number of sends (default 3), for delivery headroom
 *   EMU_AUTO_SEND_INTERVAL_MS  gap between repeats (default 4000)
 *
 * Host-only: built only by emulator/node (null_drivers) on the linux target and
 * started from app_main under a CONFIG_IDF_TARGET_LINUX guard; a real esp32s3
 * build never compiles or links it.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Public mesh send API (main/mesh_task.h). Forward-declared rather than pulling
 * in the main component's headers, mirroring how emu_flash_persist_init is wired
 * from main.c; the symbols live in libmain.a and resolve at the final link. */
extern int mesh_send_broadcast(const uint8_t *data, size_t len);
extern uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t *data, size_t len);

static const char *TAG = "emu_autosend";

/* Reads an unsigned env var, or def if unset/blank/unparseable. */
static unsigned env_uint(const char *name, unsigned def) {
    const char *v = getenv(name);
    if (!v || !*v)
        return def;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v)
        return def;
    return (unsigned)n;
}

/* Runs as a FreeRTOS task (not a raw pthread): the mesh send API posts to the
 * mesh task's queue, which is only safe from a real task context under the
 * IDF-linux FreeRTOS port. vTaskDelay yields to the scheduler like any task. */
static void autosend_task(void *arg) {
    (void)arg;
    const char *text = getenv("EMU_AUTO_SEND");
    if (!text || !*text) {
        vTaskDelete(NULL);
        return;
    }
    size_t len = strlen(text);

    unsigned delay_ms = env_uint("EMU_AUTO_SEND_DELAY_MS", 12000);
    unsigned repeat = env_uint("EMU_AUTO_SEND_REPEAT", 3);
    unsigned interval_ms = env_uint("EMU_AUTO_SEND_INTERVAL_MS", 4000);
    if (repeat == 0)
        repeat = 1;

    const char *to = getenv("EMU_AUTO_SEND_TO");
    uint32_t dest = (to && *to) ? (uint32_t)strtoul(to, NULL, 16) : 0;

    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    for (unsigned i = 0; i < repeat; i++) {
        if (dest != 0) {
            mesh_send_message(dest, (const uint8_t *)text, len);
            ESP_LOGI(TAG, "auto-sent DM to %08X (%u/%u): %s", dest, i + 1, repeat, text);
        } else {
            mesh_send_broadcast((const uint8_t *)text, len);
            ESP_LOGI(TAG, "auto-sent broadcast (%u/%u): %s", i + 1, repeat, text);
        }
        if (i + 1 < repeat)
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
    vTaskDelete(NULL);
}

/* Starts the scripted-send task if EMU_AUTO_SEND is set. Returns 0 if a task was
 * created, -1 otherwise (env unset or task create failed). */
int emu_node_start_autosend(void) {
    const char *text = getenv("EMU_AUTO_SEND");
    if (!text || !*text)
        return -1;
    /* 8 KB stack: the send path runs crypto (channel encrypt + MAC) like the
     * DM handshake worker, which was bumped to the same for its stack (PR #133). */
    if (xTaskCreate(autosend_task, "emu_autosend", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start auto-send task");
        return -1;
    }
    ESP_LOGI(TAG, "auto-send armed");
    return 0;
}
