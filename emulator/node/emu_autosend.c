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
 *   EMU_AUTO_SEND_TO         DM target for BOTH phases (also used by
 *                            park_test_task below):
 *                              "neighbor"   => the first learned neighbor's addr
 *                              "file:<name>" => poll a handoff file a sibling
 *                                              process wrote (see
 *                                              resolve_dest_from_file); lets a
 *                                              sender target a peer's real
 *                                              address without ever having
 *                                              heard a beacon from it
 *                              a hex addr   => that node
 *                              unset        => channel broadcast
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
 * EMU_AUTO_PARK=1 with the same phase-1/phase-2 shape drives a DIFFERENT
 * scenario: the peer-never-left parked-delivery case
 * (emu-parked-delivery-live.json). Phase 2 there is a single send (REPEAT=1)
 * against the peer's now-stale session, which fails via the ACTIVE-session
 * pending-ack path (not the DM-desync self-heal-and-redeliver flow); once it
 * reads MSG_STATUS_FAILED, wait_and_park_phase2 parks it. See
 * wait_and_park_phase2's own comment for the timing this relies on.
 *
 * Attestation cadence (EMU_ATTEST_EVERY_MS, EMU_ATTEST_REPEAT) and the phase-2
 * pin gate (EMU_WAIT_PEER_PINNED=1) exist for that same scenario, and together
 * they construct a precondition the firmware genuinely requires rather than
 * hoping for it. See attest_task and wait_peer_pinned below.
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
 * Park test (EMU_PARK_TEST=1): built for the parked-delivery scenario
 * (emu-parked-delivery.json). Runs park_test_task instead of autosend_task
 * (see emu_node_start_autosend): sends EMU_AUTO_SEND then EMU_AUTO_SEND2 as
 * two individually-timed DMs (EMU_PARK_MSG1_DELAY_MS / EMU_PARK_MSG2_DELAY_MS,
 * default 2000/5000) to a peer resolved via EMU_AUTO_SEND_TO=file:<name>,
 * waits for both to read MSG_STATUS_FAILED, then parks each individually
 * (mesh_park_message) when EMU_AUTO_PARK=1, or leaves them FAILED otherwise
 * (this scenario's negative control). See park_test_task's own comment for
 * why it cannot share autosend_task's phase-1/phase-2 timing.
 *
 * Host-only: built only by emulator/node (null_drivers) on the linux target and
 * started from app_main under a CONFIG_IDF_TARGET_LINUX guard; a real esp32s3
 * build never compiles or links it.
 */
#include <libgen.h>
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
#include "msg_store.h"

/* Public mesh API (main/mesh_task.h) plus the emulator neighbor helper, all
 * forward-declared rather than pulling in the main component's headers, mirroring
 * how emu_flash_persist_init is wired from main.c; the symbols live in libmain.a
 * and resolve at the final link. msg_store is a standalone lower-level
 * component (like network_key/location, already REQUIRES'd below), not part
 * of main, so its header is included directly rather than forward-declared:
 * park_test_task below needs the real stored_msg_t layout, not just a
 * function prototype. */
extern int mesh_send_broadcast(const uint8_t* data, size_t len);
extern uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t* data, size_t len);
extern void mesh_set_node_name(const char* name);
extern uint32_t emu_mesh_first_neighbor(void);
extern int emu_mesh_drop_dm_sessions(void);
extern int emu_mesh_dm_session_count(void);
extern uint32_t msg_store_total_incoming(void);
extern uint32_t msg_store_count_outgoing_delivered(void);
extern bool mesh_park_message(uint32_t uid);
extern void mesh_trigger_attestation(void);
extern bool mesh_get_peer_verify_flags(uint32_t addr, bool* verified, bool* key_changed);

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

/* Resolves a destination address a sibling process wrote via
 * emu_write_addr_handoff_file (main.c): the file lives at NODE_DIR's parent
 * (the run's shared temp base dir gosim mktemp's fresh per invocation) plus
 * fname. Polls for it, the same event-driven shape as the "neighbor" wait
 * below, since the sibling may not have written it yet. This is what lets a
 * sender target a peer's real address WITHOUT ever having received a beacon
 * from it, which "neighbor" cannot do (it blocks on an actual beacon by
 * design, see below) -- needed for a scenario that must construct a
 * genuinely never-yet-seen peer (mesh_flush_parked_for's rejoin trigger is
 * an is_new_peer edge, which only fires the first time an address is
 * admitted to the neighbor table; see mesh_beacon.c). */
static uint32_t resolve_dest_from_file(const char* fname) {
    const char* node_dir = getenv("NODE_DIR");
    if (!node_dir || !*node_dir) {
        ESP_LOGE(TAG, "EMU_AUTO_SEND_TO=file:%s but NODE_DIR is unset", fname);
        return 0;
    }
    char node_dir_copy[PATH_MAX];
    snprintf(node_dir_copy, sizeof(node_dir_copy), "%s", node_dir);
    char* base_dir = dirname(node_dir_copy); /* NODE_DIR's parent: the run's shared temp dir */
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", base_dir, fname);

    for (int i = 0; i < 240; i++) {
        FILE* f = fopen(path, "r");
        if (f) {
            char buf[32] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            if (n >= 8) {
                uint32_t addr = (uint32_t)strtoul(buf, NULL, 16);
                if (addr != 0) {
                    ESP_LOGI(TAG, "resolved dest %08X from handoff file %s", (unsigned)addr, path);
                    return addr;
                }
            }
        }
        if (i > 0 && i % 20 == 0)
            ESP_LOGW(TAG, "still waiting for handoff file %s (%d tries)", path, i);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG,
             "handoff file %s never appeared; falling back to broadcast "
             "(scenario construction is likely broken)",
             path);
    return 0;
}

/* Resolves the DM destination from EMU_AUTO_SEND_TO: 0 means broadcast. For
 * "neighbor" it retries briefly so a just-started sender waits for its first
 * beacon exchange rather than falling back to broadcast. "file:<name>" reads
 * a handoff file instead of the live neighbor table (see
 * resolve_dest_from_file above). */
static uint32_t resolve_dest(void) {
    const char* to = getenv("EMU_AUTO_SEND_TO");
    if (!to || !*to)
        return 0; /* broadcast */
    if (strncmp(to, "file:", 5) == 0)
        return resolve_dest_from_file(to + 5);
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

/* Waits (event-driven, wide budget) for one outgoing DM matching text/dest to
 * read MSG_STATUS_FAILED, then parks it via mesh_park_message. Built for the
 * peer-never-left scenario (emu-parked-delivery-live.json): the peer stays a
 * live, beaconing neighbor throughout, so this is NOT park_test_task's
 * unreachable-peer/route-queue-TTL construction. Here phase 2 sends under a
 * session the receiver can no longer decrypt (its half was dropped via
 * EMU_DROP_DM_SESSION_AT_MS while it kept beaconing normally), so the row
 * fails via the ACTIVE-session pending-ack path instead: MSG_TIER_NORMAL's 3
 * attempts with exponential backoff from a 2s base land FAILED well inside a
 * minute, no route-queue TTL involved. 150 tries * 2s = 300s of headroom over
 * that budget covers CPU-starvation slack. Only called when EMU_AUTO_PARK is
 * set; a scenario that wants phase 2 to simply succeed or fail on its own
 * never reaches this, and it is a no-op if text is unset. */
static void wait_and_park_phase2(const char* text, uint32_t dest) {
    if (!text || !*text)
        return;
    const char* park_env = getenv("EMU_AUTO_PARK");
    if (!park_env || !*park_env || strcmp(park_env, "0") == 0)
        return;
    size_t text_len = strlen(text);
    uint32_t uid = 0;
    for (int i = 0; i < 150; i++) {
        int n = msg_store_count();
        for (int idx = 0; idx < n; idx++) {
            stored_msg_t m;
            if (!msg_store_get_copy(idx, &m))
                continue;
            if (m.direction != MSG_DIR_OUTGOING || m.channel_index >= 0 || m.peer_addr != dest ||
                m.status != MSG_STATUS_FAILED)
                continue;
            if (m.text_len == text_len && memcmp(m.text, text, text_len) == 0) {
                uid = m.uid;
                break;
            }
        }
        if (uid != 0)
            break;
        if (i > 0 && i % 10 == 0)
            ESP_LOGW(TAG, "auto-park: still waiting for '%s' to FAIL (%d tries)", text, i);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (uid == 0) {
        ESP_LOGE(TAG, "auto-park: timed out waiting for '%s' to FAIL", text);
        return;
    }
    bool ok = mesh_park_message(uid);
    ESP_LOGI(TAG, "auto-park: parked uid=%08X ok=%d after phase 2 failed", (unsigned)uid, ok);
}

/* Blocks until this node holds an ATTESTATION PIN for dest, when
 * EMU_WAIT_PEER_PINNED is set. Returns true if the pin is in place.
 *
 * This gates the peer-never-left scenario on a precondition the firmware
 * really has, instead of letting the scenario win or lose a coin flip. When
 * the receiver drops its session half and then fails to decrypt, its #138
 * self-heal INIT is a FIRST-CONTACT init carrying a zero auth tag (it no
 * longer holds a peer_id to tag with). The sender still holds its stale
 * session, so process_ke_init takes dm_verify_init's strict tag path and
 * rejects that zero tag by design (the Item 2 downgrade defense). The one
 * escape is main/mesh_dm.c's pinned-peer branch, which needs an
 * attestation-verified X25519 pin for the peer. With no pin the session never
 * heals, every redelivery is encrypted under the dead session, and the
 * scenario can prove nothing about parked_retry no matter how long it runs.
 *
 * Observed directly: a run where the receiver's single boot-time attestation
 * collided with the sender's own beacon TX (half-duplex) left the sender
 * unpinned, the sender logged "INIT verify failed" twice, and the parked
 * message was flushed onto the air and lost. So this is a real gate, not
 * defensive padding: mesh_get_peer_verify_flags returns false for an unpinned
 * address, and if the pin never lands the phase-2 send never happens and the
 * suite fails honestly rather than passing on a healthy-looking accident. */
static bool wait_peer_pinned(uint32_t dest) {
    const char* want = getenv("EMU_WAIT_PEER_PINNED");
    if (!want || !*want || strcmp(want, "0") == 0)
        return true;
    if (dest == 0)
        return true;
    bool verified = false, key_changed = false;
    for (int i = 0; i < 240; i++) {
        if (mesh_get_peer_verify_flags(dest, &verified, &key_changed)) {
            ESP_LOGI(TAG, "phase 2: peer %08X is attestation-pinned", (unsigned)dest);
            return true;
        }
        if (i > 0 && i % 20 == 0)
            ESP_LOGW(TAG, "phase 2: still waiting to pin peer %08X (%d tries)", (unsigned)dest, i);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG, "phase 2: never pinned peer %08X; the desync cannot self-heal", (unsigned)dest);
    return false;
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
         * precondition is that phase 1's DM was actually DELIVERED, not that
         * a fixed delay elapsed. A DM to a peer with no session waits in the
         * mesh task's small awaiting-session queue until the KE handshake
         * completes, and under channel contention (the scenario's 3s beacons
         * carry ~650ms of airtime each, running the ether at 50-65% duty)
         * that handshake can outlast any fixed delay. Phase 2's fixed-cadence
         * burst then overflows the queue, "evicting oldest" discards the
         * phase-1 payload unsent, and the desync gate's baseline ALPHA render
         * becomes impossible no matter how long the suite waits (observed
         * 2026-07-23, locally and on two CI runs: every flushed DM was a
         * 7-byte BETA; ALPHA never hit the air).
         *
         * The predicate must be CONFIRMED DELIVERY (the receipt arriving back
         * here), not this node's session record: mesh_send_dm allocates the
         * session slot in HANDSHAKING state synchronously on phase 1's first
         * send, so a session-count check is already true before the KE ever
         * completes and gates nothing (review finding on the first version
         * of this change). Delivery is also the very event the receiver's
         * drop task keys on (its msg_store_total_incoming gate flips when
         * ALPHA lands, and the receipt rides back on the same exchange), so
         * both sides pace from the same instant: the drop fires ~1.5s after
         * it, phase 2 delay2 (16-20s) later, keeping the first BETA on the
         * post-drop side with seconds to spare at any starvation factor. The
         * cap only guards a genuinely broken run, where the gate fails
         * anyway. Broadcast phase 2 (dest 0) has no receipts and skips the
         * wait. */
        if (dest != 0) {
            for (int i = 0; i < 240; i++) {
                if (msg_store_count_outgoing_delivered() > 0)
                    break;
                if (i > 0 && i % 20 == 0)
                    ESP_LOGW(TAG, "phase 2: still waiting for phase-1 delivery (%d tries)", i);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(delay2));
        /* Re-resolve: after a peer reboot its address is unchanged, but this also
         * recovers if the neighbor was only learned during phase 1. */
        if (getenv("EMU_AUTO_SEND_TO"))
            dest = resolve_dest();
        /* After the re-resolve, so the pin is checked against the address the
         * send will actually use. A false return leaves phase 2 unsent, which
         * is the honest outcome: without the pin the scenario's heal cannot
         * happen and a delivered payload would have to come from somewhere
         * this scenario does not claim. */
        if (!wait_peer_pinned(dest)) {
            vTaskDelete(NULL);
            return;
        }
        send_burst(text2, dest, repeat2, interval2, "auto-sent2");
        wait_and_park_phase2(text2, dest);
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

/* Re-announces this node's identity attestation EMU_ATTEST_REPEAT times, every
 * EMU_ATTEST_EVERY_MS, through the same public entry point the setEndorsement
 * RPC uses (mesh_trigger_attestation, itself airtime-budget gated).
 *
 * This COMPRESSES A REAL CADENCE, exactly as EMU_BEACON_INTERVAL_MS does for
 * beacons: the firmware re-attests every ATTESTATION_INTERVAL_MS (15 minutes),
 * which is far outside any scenario's run, so a scenario that needs its peer
 * pinned would otherwise depend on the single post-boot attestation landing.
 * That send is a coin flip, not a construction: at the emulator's 3s beacon
 * interval each node is transmitting a large fraction of the time, and a
 * half-duplex peer that is mid-beacon simply does not hear it (observed
 * directly, and the run it cost is described in wait_peer_pinned). Repeating
 * on a short cadence makes the exchange happen, and the receiving side's
 * EMU_WAIT_PEER_PINNED gate is what proves it actually did rather than
 * absorbing the failure. */
static void attest_task(void* arg) {
    (void)arg;
    unsigned every_ms = env_uint("EMU_ATTEST_EVERY_MS", 0);
    if (every_ms == 0) {
        vTaskDelete(NULL);
        return;
    }
    unsigned repeat = env_uint("EMU_ATTEST_REPEAT", 6);
    for (unsigned i = 0; i < repeat; i++) {
        vTaskDelay(pdMS_TO_TICKS(every_ms));
        mesh_trigger_attestation();
    }
    ESP_LOGI(TAG, "attestation cadence done (%u re-announcements)", repeat);
    vTaskDelete(NULL);
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

/* park_test_task: a self-contained alternative to autosend_task, built for
 * the parked-delivery scenario (emu-parked-delivery.json). It does NOT share
 * autosend_task's phase-1/phase-2 timing: phase 2 there waits for phase 1's
 * CONFIRMED DELIVERY before proceeding (see autosend_task above), which
 * would never happen here by design, since both sends in this task are
 * meant to genuinely fail. Runs as its own task, mutually exclusive with
 * autosend_task (see emu_node_start_autosend below), reusing EMU_AUTO_SEND /
 * EMU_AUTO_SEND2 as its two message texts and EMU_AUTO_SEND_TO to resolve
 * the destination (normally "file:<name>": see resolve_dest_from_file).
 *
 * Sends the two DMs a few seconds apart, to a peer that has, by
 * construction, never beaconed to this node (that is the whole point of the
 * file handoff instead of "neighbor" resolution): dest is not a neighbor,
 * route discovery finds nothing, and each queued-for-route entry expires
 * FAILED after the flat 60s route TTL, checked on mesh_task's 60s purge
 * cadence (mesh_task.c, ~line 1594-1623), so worst case is just under two
 * purge intervals if a send lands right after a tick fires. Once both rows
 * read FAILED, EMU_AUTO_PARK=1 parks each individually via
 * mesh_park_message; left unset, both stay FAILED, which is this
 * scenario's negative control (mesh_flush_parked_for has nothing to
 * redeliver on the peer's later rejoin, since msg_store_parked_uids_for_peer
 * only ever selects MSG_STATUS_QUEUED rows). */
static void park_test_task(void* arg) {
    (void)arg;
    const char* text1 = getenv("EMU_AUTO_SEND");
    const char* text2 = getenv("EMU_AUTO_SEND2");
    if (!text1 || !*text1 || !text2 || !*text2) {
        ESP_LOGE(TAG, "EMU_PARK_TEST set but EMU_AUTO_SEND/EMU_AUTO_SEND2 missing");
        vTaskDelete(NULL);
        return;
    }
    bool do_park = env_uint("EMU_AUTO_PARK", 0) != 0;

    uint32_t dest = resolve_dest();
    if (dest == 0) {
        ESP_LOGE(TAG, "park test: could not resolve a destination; aborting");
        vTaskDelete(NULL);
        return;
    }

    unsigned delay1 = env_uint("EMU_PARK_MSG1_DELAY_MS", 2000);
    unsigned delay2 = env_uint("EMU_PARK_MSG2_DELAY_MS", 5000);

    vTaskDelay(pdMS_TO_TICKS(delay1));
    send_one(text1, strlen(text1), dest, "park-test", 0, 2);
    if (delay2 > delay1)
        vTaskDelay(pdMS_TO_TICKS(delay2 - delay1));
    send_one(text2, strlen(text2), dest, "park-test", 1, 2);

    /* Wait for BOTH rows to read FAILED, event-driven with a wide budget
     * (150 tries * 2s = 300s): the worst case derived above is just under
     * 120s, so this leaves comfortable headroom for CPU-starvation slack
     * without hard-coding a tight number. */
    size_t text1_len = strlen(text1);
    size_t text2_len = strlen(text2);
    uint32_t uid1 = 0, uid2 = 0;
    for (int i = 0; i < 150; i++) {
        int n = msg_store_count();
        for (int idx = 0; idx < n; idx++) {
            stored_msg_t m;
            if (!msg_store_get_copy(idx, &m))
                continue;
            if (m.direction != MSG_DIR_OUTGOING || m.channel_index >= 0 || m.peer_addr != dest ||
                m.status != MSG_STATUS_FAILED)
                continue;
            if (uid1 == 0 && m.text_len == text1_len && memcmp(m.text, text1, text1_len) == 0) {
                uid1 = m.uid;
            } else if (uid2 == 0 && m.text_len == text2_len &&
                       memcmp(m.text, text2, text2_len) == 0) {
                uid2 = m.uid;
            }
        }
        if (uid1 != 0 && uid2 != 0)
            break;
        if (i > 0 && i % 10 == 0)
            ESP_LOGW(
                TAG,
                "park test: still waiting for both sends to fail (%d tries, uid1=%08X uid2=%08X)",
                i, (unsigned)uid1, (unsigned)uid2);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (uid1 == 0 || uid2 == 0) {
        ESP_LOGE(TAG, "park test: timed out waiting for both DMs to FAIL (uid1=%08X uid2=%08X)",
                 (unsigned)uid1, (unsigned)uid2);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "park test: both DMs FAILED (uid1=%08X uid2=%08X)", (unsigned)uid1,
             (unsigned)uid2);

    if (do_park) {
        bool ok1 = mesh_park_message(uid1);
        bool ok2 = mesh_park_message(uid2);
        ESP_LOGI(TAG, "park test: parked uid=%08X ok=%d, uid=%08X ok=%d", (unsigned)uid1, ok1,
                 (unsigned)uid2, ok2);
    } else {
        ESP_LOGI(TAG,
                 "park test: EMU_AUTO_PARK not set, leaving both rows FAILED (negative control)");
    }

    vTaskDelete(NULL);
}

/* Starts the scripted-send task if EMU_AUTO_SEND is set, the reboot timer if
 * EMU_REBOOT_AT_MS is set, and the session-drop timer if
 * EMU_DROP_DM_SESSION_AT_MS is set. Returns 0 if any was started, -1 if none.
 * EMU_PARK_TEST takes priority over the normal EMU_AUTO_SEND path (mutually
 * exclusive, not merely by scenario convention): park_test_task reuses
 * EMU_AUTO_SEND/EMU_AUTO_SEND2 as its own message texts, so starting
 * autosend_task too would double-send against the same env vars. */
int emu_node_start_autosend(void) {
    int started = -1;

    /* EMU_NODE_NAME gives a scenario node a friendly beacon name (e.g. Alice),
     * so neighbors render it on their Nodes and Messages screens instead of a
     * bare address. Set at startup; it rides the next beacon. */
    const char* node_name = getenv("EMU_NODE_NAME");
    if (node_name && *node_name)
        mesh_set_node_name(node_name);

    bool park_test = getenv("EMU_PARK_TEST") && *getenv("EMU_PARK_TEST") &&
                     strcmp(getenv("EMU_PARK_TEST"), "0") != 0;

    if (park_test) {
        /* 8 KB stack: same rationale as autosend_task below (the send path
         * runs crypto (channel/DM encrypt + MAC) like the DM handshake
         * worker). */
        if (xTaskCreate(park_test_task, "emu_parktest", 8192, NULL, 5, NULL) == pdPASS) {
            ESP_LOGI(TAG, "park test armed");
            started = 0;
        } else {
            ESP_LOGW(TAG, "could not start park test task");
        }
    } else if (getenv("EMU_AUTO_SEND") && *getenv("EMU_AUTO_SEND")) {
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
    if (getenv("EMU_ATTEST_EVERY_MS") && *getenv("EMU_ATTEST_EVERY_MS")) {
        /* 4 KB: mesh_trigger_attestation builds and Ed25519-signs the
         * attestation on the calling task, so this needs more than the 2 KB
         * the pure timer tasks around it run on. */
        if (xTaskCreate(attest_task, "emu_attest", 4096, NULL, 5, NULL) == pdPASS) {
            ESP_LOGI(TAG, "attestation cadence armed");
            started = 0;
        } else {
            ESP_LOGW(TAG, "could not start attestation cadence task");
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
