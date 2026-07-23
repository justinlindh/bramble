/*
 * Emulator scripted send + reboot for the IDF linux target.
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
 *   EMU_AUTO_SEND            phase-1 message text; unset/blank => never sends
 *   EMU_AUTO_SEND_TO         DM target for BOTH phases:
 *                              "neighbor" => the first learned neighbor's addr
 *                              a hex addr => that node
 *                              unset      => channel broadcast
 *   EMU_AUTO_SEND_DELAY_MS   delay before phase 1 (default 12000). Must clear
 *                            UI_MESSAGE_IDLE_THRESHOLD_MS (10s) on the receiver
 *                            so an inbound message auto-opens its Messages
 *                            screen, which is what a screen assertion sees.
 *   EMU_AUTO_SEND_REPEAT     phase-1 sends (default 3), for delivery headroom
 *   EMU_AUTO_SEND_INTERVAL_MS  gap between phase-1 repeats (default 4000)
 *   EMU_AUTO_SEND2           optional phase-2 message text (distinct string)
 *   EMU_AUTO_SEND2_DELAY_MS  additional delay before phase 2, measured from
 *                            phase 1's DM session existing (see autosend_task)
 *   EMU_AUTO_SEND2_REPEAT / EMU_AUTO_SEND2_INTERVAL_MS  phase-2 cadence
 *
 * Phase 2 exists for the DM-desync scenario: phase 1 establishes a DM session,
 * the receiver drops its half (EMU_DROP_DM_SESSION_AT_MS), and phase 2's repeats
 * drive the post-#138 heal (the first lands on a receiver with no session and
 * fires the re-handshake; a later one decrypts and renders).
 *
 * Drop-session (EMU_DROP_DM_SESSION_AT_MS): at that time the node tears down its
 * own DM sessions in-process (emu_mesh_drop_dm_sessions) while staying up and
 * still neighboring the peer, so the peer keeps its stale session. That is the
 * one-sided-session precondition, constructed DETERMINISTICALLY at a fixed
 * instant. The desync scenario uses this instead of a reboot precisely because
 * it removes reboot's real-time races (RAM-clear timing, beacon re-acquisition,
 * and the stale DM having to land inside that window) from CI.
 *
 * Reboot (EMU_REBOOT_AT_MS): the node exits at that time; the gosim supervisor's
 * restart-on-exit brings it back with the same NVS identity but cleared RAM
 * (its DM sessions). A heavier way to reach the same one-sided-session state,
 * kept as a general emulator primitive; the desync scenario prefers the
 * deterministic drop above.
 *
 * Host-only: built only by emulator/node (null_drivers) on the linux target and
 * started from app_main under a CONFIG_IDF_TARGET_LINUX guard; a real esp32s3
 * build never compiles or links it.
 */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Public mesh API (main/mesh_task.h) plus the emulator neighbor helper, all
 * forward-declared rather than pulling in the main component's headers, mirroring
 * how emu_flash_persist_init is wired from main.c; the symbols live in libmain.a
 * and resolve at the final link. */
extern int mesh_send_broadcast(const uint8_t* data, size_t len);
extern uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t* data, size_t len);
extern void mesh_set_node_name(const char* name);
extern uint32_t emu_mesh_first_neighbor(void);
extern int emu_mesh_drop_dm_sessions(void);
extern int emu_mesh_dm_session_count(void);
extern uint32_t msg_store_total_incoming(void);

static const char* TAG = "emu_autosend";

/* Reads an unsigned env var, or def if unset/blank/unparseable. */
static unsigned env_uint(const char* name, unsigned def) {
    const char* v = getenv(name);
    if (!v || !*v)
        return def;
    char* end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v)
        return def;
    return (unsigned)n;
}

/* Resolves the DM destination from EMU_AUTO_SEND_TO: 0 means broadcast. For
 * "neighbor" it retries briefly so a just-started sender waits for its first
 * beacon exchange rather than falling back to broadcast. */
static uint32_t resolve_dest(void) {
    const char* to = getenv("EMU_AUTO_SEND_TO");
    if (!to || !*to)
        return 0; /* broadcast */
    if (strcmp(to, "neighbor") == 0) {
        /* EVENT-DRIVEN, with a cap far beyond any plausible discovery time.
         * The old ~20s cap was calibrated in subjective (tick) time, and the
         * linux port loses ticks under CI pod jitter at different rates per
         * code path, so a beacon exchange that takes 3 subjective seconds of
         * mesh-task time can outlast 20 subjective seconds of this task's
         * waiting (observed on CI: the fallback fired and silently degraded
         * the DM scenario to a broadcast, collapsing its whole construction).
         * Waiting longer costs nothing on a healthy run and the scenario's
         * wall-clock budget still bounds the process. */
        for (int i = 0; i < 240; i++) {
            uint32_t a = emu_mesh_first_neighbor();
            if (a != 0)
                return a;
            if (i > 0 && i % 20 == 0)
                ESP_LOGW(TAG, "still waiting for a neighbor (%d tries)", i);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGE(TAG, "no neighbor learned after extended wait; falling back to broadcast "
                      "(scenario construction is likely broken)");
        return 0;
    }
    return (uint32_t)strtoul(to, NULL, 16);
}

/* Sends text once, as a DM to dest or a broadcast when dest is 0. */
static void send_one(const char* text, size_t len, uint32_t dest, const char* tag, unsigned i,
                     unsigned n) {
    if (dest != 0) {
        mesh_send_message(dest, (const uint8_t*)text, len);
        ESP_LOGI(TAG, "%s DM to %08X (%u/%u): %s", tag, dest, i + 1, n, text);
    } else {
        mesh_send_broadcast((const uint8_t*)text, len);
        ESP_LOGI(TAG, "%s broadcast (%u/%u): %s", tag, i + 1, n, text);
    }
}

/* Runs a burst of `repeat` sends of text spaced interval_ms apart. */
static void send_burst(const char* text, uint32_t dest, unsigned repeat, unsigned interval_ms,
                       const char* tag) {
    if (!text || !*text || repeat == 0)
        return;
    size_t len = strlen(text);
    for (unsigned i = 0; i < repeat; i++) {
        send_one(text, len, dest, tag, i, repeat);
        if (i + 1 < repeat)
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

/* Runs as a FreeRTOS task (not a raw pthread): the mesh send API posts to the
 * mesh task's queue, which is only safe from a real task context under the
 * IDF-linux FreeRTOS port. */
static void autosend_task(void* arg) {
    (void)arg;
    const char* text1 = getenv("EMU_AUTO_SEND");
    if (!text1 || !*text1) {
        vTaskDelete(NULL);
        return;
    }

    unsigned delay1 = env_uint("EMU_AUTO_SEND_DELAY_MS", 12000);
    unsigned repeat1 = env_uint("EMU_AUTO_SEND_REPEAT", 3);
    unsigned interval1 = env_uint("EMU_AUTO_SEND_INTERVAL_MS", 4000);

    vTaskDelay(pdMS_TO_TICKS(delay1));
    uint32_t dest = resolve_dest(); /* may block briefly awaiting a neighbor */
    send_burst(text1, dest, repeat1, interval1, "auto-sent");

    const char* text2 = getenv("EMU_AUTO_SEND2");
    if (text2 && *text2) {
        unsigned delay2 = env_uint("EMU_AUTO_SEND2_DELAY_MS", 16000);
        unsigned repeat2 = env_uint("EMU_AUTO_SEND2_REPEAT", 4);
        unsigned interval2 = env_uint("EMU_AUTO_SEND2_INTERVAL_MS", 4000);
        /* EVENT-DRIVEN, like resolve_dest and the drop task: phase 2's real
         * precondition is that phase 1's DM actually flushed to the wire, not
         * that a fixed delay elapsed. A DM to a peer with no session waits in
         * the mesh task's small awaiting-session queue until the KE handshake
         * completes, and under channel contention (the scenario's 3s beacons
         * carry ~650ms of airtime each, running the ether at 50-65% duty)
         * that handshake can outlast any fixed delay. Phase 2's fixed-cadence
         * burst then overflows the queue, "evicting oldest" discards the
         * phase-1 payload unsent, and the desync gate's baseline ALPHA render
         * becomes impossible no matter how long the suite waits (observed
         * 2026-07-23, locally and on two CI runs: every flushed DM was a
         * 7-byte BETA; ALPHA never hit the air). Waiting for this node's own
         * session record means the queue has flushed in order and phase 1 is
         * on the wire; delay2 then paces phase 2 from that point, preserving
         * its land-after-the-receiver-drop ordering (the drop waits for the
         * delivered phase-1 DM plus a 1.5s settle, well inside the scenario's
         * 20s delay2). The cap only guards a genuinely broken run, where the
         * gate fails anyway. Broadcast phase 2 (dest 0) needs no session and
         * skips the wait. */
        if (dest != 0) {
            for (int i = 0; i < 240; i++) {
                if (emu_mesh_dm_session_count() > 0)
                    break;
                if (i > 0 && i % 20 == 0)
                    ESP_LOGW(TAG, "phase 2: still waiting for the phase-1 session (%d tries)", i);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(delay2));
        /* Re-resolve: after a peer reboot its address is unchanged, but this also
         * recovers if the neighbor was only learned during phase 1. */
        if (getenv("EMU_AUTO_SEND_TO"))
            dest = resolve_dest();
        send_burst(text2, dest, repeat2, interval2, "auto-sent2");
    }

    vTaskDelete(NULL);
}

/* Path of the one-shot reboot marker in NODE_DIR, or "" if NODE_DIR is unset.
 * Its presence means this node already did its scheduled reboot, so the timer is
 * a no-op on the restarted process (otherwise the node would reboot every boot). */
static void reboot_marker_path(char* out, size_t out_len) {
    const char* dir = getenv("NODE_DIR");
    if (!dir || !*dir) {
        out[0] = '\0';
        return;
    }
    int w = snprintf(out, out_len, "%s/emu_rebooted", dir);
    if (w < 0 || (size_t)w >= out_len)
        out[0] = '\0';
}

/* Exits the process ONCE at EMU_REBOOT_AT_MS so the supervisor restarts the node
 * (a reboot: same identity, cleared RAM). The NODE_DIR marker makes it one-shot
 * so the restarted node stays up. */
static void reboot_task(void* arg) {
    (void)arg;
    unsigned at_ms = env_uint("EMU_REBOOT_AT_MS", 0);
    if (at_ms == 0) {
        vTaskDelete(NULL);
        return;
    }
    char marker[PATH_MAX];
    reboot_marker_path(marker, sizeof(marker));
    struct stat st;
    if (marker[0] && stat(marker, &st) == 0) {
        ESP_LOGI(TAG, "reboot already done (marker present); not rebooting again");
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(at_ms));
    if (marker[0]) {
        FILE* f = fopen(marker, "w");
        if (f)
            fclose(f);
    }
    ESP_LOGI(TAG, "EMU_REBOOT_AT_MS reached; exiting for supervisor restart");
    exit(0);
}

/* Tears down this node's DM sessions ONCE at EMU_DROP_DM_SESSION_AT_MS, leaving
 * the peer's stale half in place: the deterministic one-sided-desync inject the
 * emu-dm-desync scenario relies on (see the header comment). No NODE_DIR marker
 * is needed the way reboot_task needs one, because the node never restarts, so
 * this task runs at most once per process life. */
static void drop_session_task(void* arg) {
    (void)arg;
    unsigned at_ms = env_uint("EMU_DROP_DM_SESSION_AT_MS", 0);
    if (at_ms == 0) {
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(at_ms));
    /* EVENT-DRIVEN: the drop constructs a one-sided desync, which only
     * exists if there IS a session to drop. Under CI tick-loss skew the
     * ALPHA handshake can complete well after this task's subjective at_ms,
     * and dropping zero sessions silently voids the scenario. Wait for the
     * session, settle briefly so the in-flight receipt exchange lands (the
     * purge in emu_mesh_drop_dm_sessions handles whatever remains), then
     * drop. The cap only guards a genuinely broken run. */
    /* Gate on the ALPHA actually ARRIVING, not merely on a session record: a
     * session exists in HANDSHAKING/ACTIVE state as soon as the peer's INIT
     * is processed, which is BEFORE the queued ALPHA payload is delivered
     * and rendered; dropping in that gap kills the session out from under
     * the in-flight ALPHA and the scenario's baseline render never happens
     * (observed locally). A stored incoming message plus a live session
     * means the handshake completed AND the payload landed. */
    for (int i = 0; i < 240; i++) {
        if (emu_mesh_dm_session_count() > 0 && msg_store_total_incoming() > 0)
            break;
        if (i > 0 && i % 20 == 0)
            ESP_LOGW(TAG, "drop: still waiting for session + delivered DM (%d tries)", i);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    /* Settle: let the delivery receipt/ACK exchange for the ALPHA finish so
     * the reboot-faithful purge has less in flight to clean up. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    int n = emu_mesh_drop_dm_sessions();
    ESP_LOGI(TAG, "EMU_DROP_DM_SESSION_AT_MS reached; dropped %d DM session(s)", n);
    if (n == 0)
        ESP_LOGE(TAG, "drop found NO session; the desync construction did not happen");
    vTaskDelete(NULL);
}

/* Starts the scripted-send task if EMU_AUTO_SEND is set, the reboot timer if
 * EMU_REBOOT_AT_MS is set, and the session-drop timer if
 * EMU_DROP_DM_SESSION_AT_MS is set. Returns 0 if any was started, -1 if none. */
int emu_node_start_autosend(void) {
    int started = -1;

    /* EMU_NODE_NAME gives a scenario node a friendly beacon name (e.g. Alice),
     * so neighbors render it on their Nodes and Messages screens instead of a
     * bare address. Set at startup; it rides the next beacon. */
    const char* node_name = getenv("EMU_NODE_NAME");
    if (node_name && *node_name)
        mesh_set_node_name(node_name);

    if (getenv("EMU_AUTO_SEND") && *getenv("EMU_AUTO_SEND")) {
        /* 8 KB stack: the send path runs crypto (channel/DM encrypt + MAC) like
         * the DM handshake worker, bumped to the same for its stack (PR #133). */
        if (xTaskCreate(autosend_task, "emu_autosend", 8192, NULL, 5, NULL) == pdPASS) {
            ESP_LOGI(TAG, "auto-send armed");
            started = 0;
        } else {
            ESP_LOGW(TAG, "could not start auto-send task");
        }
    }
    if (getenv("EMU_REBOOT_AT_MS") && *getenv("EMU_REBOOT_AT_MS")) {
        if (xTaskCreate(reboot_task, "emu_reboot", 2048, NULL, 5, NULL) == pdPASS) {
            ESP_LOGI(TAG, "reboot timer armed");
            started = 0;
        } else {
            ESP_LOGW(TAG, "could not start reboot task");
        }
    }
    if (getenv("EMU_DROP_DM_SESSION_AT_MS") && *getenv("EMU_DROP_DM_SESSION_AT_MS")) {
        if (xTaskCreate(drop_session_task, "emu_dropdm", 2048, NULL, 5, NULL) == pdPASS) {
            ESP_LOGI(TAG, "DM session-drop timer armed");
            started = 0;
        } else {
            ESP_LOGW(TAG, "could not start DM session-drop task");
        }
    }
    return started;
}
