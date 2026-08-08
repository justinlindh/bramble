/*
 * Emulator interactive control path for the IDF linux target (the playground).
 *
 * The scripted paths next door (emu_provision.c, emu_autosend.c) key up and
 * send from the environment at boot, which is what a headless CI scenario
 * needs. A person driving the emulator in a browser needs the opposite: the
 * fleet boots UNPROVISIONED and INERT, exactly as real hardware ships, and the
 * operator provisions it and originates messages when they choose. This file
 * is that path. It adds three broker-to-node emu-link message types:
 *
 *   {"t":"prov","key":"<64 hex chars>"}
 *       Provisions the control-plane network key at runtime through
 *       network_key_set_from_hex (the same component call the setNetworkKey
 *       RPC reaches) and rederives the beacon subkey, which is exactly what
 *       rpc_set_network_key does on a real device. Nothing here bypasses the
 *       fail-closed state: before the message arrives the node is genuinely
 *       inert, and afterwards it is genuinely provisioned.
 *
 *   {"t":"send","text":"...","to":"<8 hex addr>"}
 *       Originates one message through the public mesh API: mesh_send_message
 *       for a DM when "to" names a peer, mesh_send_broadcast otherwise. Same
 *       entry point the compose UI, the CLI and the RPC layer all call, so the
 *       real encrypt/route/tx path runs; only the trigger is remote.
 *
 *   {"t":"attest"}
 *       Announces this node's identity now (mesh_trigger_attestation, the same
 *       public call the setEndorsement RPC makes). A peer pins an identity only
 *       from an attestation, and a pinned identity is what a safety number is
 *       derived from, so this is the operator-facing "introduce yourself" that
 *       has to happen before two nodes can compare safety numbers.
 *
 * THREADING. Every handler runs on emu_link's reader thread, which must never
 * enter FreeRTOS (see the signal-mask note in emulator/DESIGN.md and the same
 * constraint gps_virt.c and button_virt.c design around). They therefore only
 * copy the request into a pthread-mutex-guarded ring; emu_control_task, a real
 * FreeRTOS task, drains it and makes every mesh/NVS call from task context.
 *
 * ATTESTATION AFTER PROVISIONING. A peer's 7-digit SAS is derived from its
 * pinned identity, and pins come from identity attestations, which an
 * unprovisioned node neither sends nor accepts. Boot attestation therefore
 * fails on an inert fleet and reschedules 60 s out, and a fleet provisioned
 * one node at a time can have the first announcement land on peers that are
 * still inert. So a provision schedules two attestations of its own (see
 * ATTEST_FIRST_MS / ATTEST_SECOND_MS): the delay lets every node in the fleet
 * take its key first, and the second announcement covers a collision with the
 * first, which would otherwise leave a hole until the 15-minute periodic
 * attestation. mesh_trigger_attestation is the same public call the
 * setEndorsement RPC uses, and it is budget-gated like every attestation.
 *
 * Host-only: built only by emulator/node (null_drivers) on the linux target
 * and started from app_main under a CONFIG_IDF_TARGET_LINUX guard, so a real
 * esp32s3 build never compiles or links it.
 */
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "emu_link.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_key.h"

/* Public mesh API (main/mesh_task.h), forward-declared rather than including
 * the main component's headers: this file builds as part of null_drivers,
 * which does not carry main/ on its include path (emu_autosend.c does the
 * same). The symbols live in libmain.a and resolve at the final link. */
extern int mesh_send_broadcast(const uint8_t* data, size_t len);
extern uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t* data, size_t len);
extern void mesh_rederive_beacon_key(void);
extern void mesh_trigger_attestation(void);

static const char* TAG = "emu_control";

#define EMU_CTL_QUEUE_CAP 8
#define EMU_CTL_TEXT_MAX 64
#define EMU_CTL_KEY_HEX_LEN 64

/* Drain cadence. Fast enough that a click feels immediate, slow enough to cost
 * nothing on a CPU-shared runner. */
#define EMU_CTL_TICK_MS 100

/* Attestation schedule after a provision; see the header comment. */
#define ATTEST_FIRST_MS 4000u
#define ATTEST_SECOND_MS 20000u

typedef enum {
    EMU_CTL_OP_PROVISION = 1,
    EMU_CTL_OP_SEND = 2,
    EMU_CTL_OP_ATTEST = 3,
} emu_ctl_op_t;

typedef struct {
    emu_ctl_op_t op;
    char key_hex[EMU_CTL_KEY_HEX_LEN + 1];
    char text[EMU_CTL_TEXT_MAX];
    uint32_t dest; /* 0 = channel broadcast */
} emu_ctl_cmd_t;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static emu_ctl_cmd_t s_ring[EMU_CTL_QUEUE_CAP];
static int s_head, s_tail, s_count;

/* Copies cmd into the ring. Runs on the emu_link reader thread. A full ring
 * drops the newest request (the drain task has stalled; dropping is preferable
 * to blocking the reader, matching button_virt.c). */
static void enqueue(const emu_ctl_cmd_t* cmd) {
    pthread_mutex_lock(&s_mu);
    if (s_count < EMU_CTL_QUEUE_CAP) {
        s_ring[s_tail] = *cmd;
        s_tail = (s_tail + 1) % EMU_CTL_QUEUE_CAP;
        s_count++;
    }
    pthread_mutex_unlock(&s_mu);
}

/* Pops one request into out. Returns 1 if one was taken, 0 if the ring is
 * empty. Runs on the FreeRTOS drain task. */
static int dequeue(emu_ctl_cmd_t* out) {
    int taken = 0;
    pthread_mutex_lock(&s_mu);
    if (s_count > 0) {
        *out = s_ring[s_head];
        s_head = (s_head + 1) % EMU_CTL_QUEUE_CAP;
        s_count--;
        taken = 1;
    }
    pthread_mutex_unlock(&s_mu);
    return taken;
}

static void prov_handler(const cJSON* msg, void* ctx) {
    (void)ctx;
    const cJSON* key = cJSON_GetObjectItem(msg, "key");
    if (!cJSON_IsString(key) || !key->valuestring)
        return;
    if (strlen(key->valuestring) != EMU_CTL_KEY_HEX_LEN)
        return; /* the drain task would reject it anyway; drop it here */

    emu_ctl_cmd_t cmd = {.op = EMU_CTL_OP_PROVISION};
    memcpy(cmd.key_hex, key->valuestring, EMU_CTL_KEY_HEX_LEN);
    cmd.key_hex[EMU_CTL_KEY_HEX_LEN] = '\0';
    enqueue(&cmd);
}

static void attest_handler(const cJSON* msg, void* ctx) {
    (void)msg;
    (void)ctx;
    emu_ctl_cmd_t cmd = {.op = EMU_CTL_OP_ATTEST};
    enqueue(&cmd);
}

static void send_handler(const cJSON* msg, void* ctx) {
    (void)ctx;
    const cJSON* text = cJSON_GetObjectItem(msg, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0])
        return;

    emu_ctl_cmd_t cmd = {.op = EMU_CTL_OP_SEND};
    snprintf(cmd.text, sizeof(cmd.text), "%s", text->valuestring);

    const cJSON* to = cJSON_GetObjectItem(msg, "to");
    if (cJSON_IsString(to) && to->valuestring && to->valuestring[0])
        cmd.dest = (uint32_t)strtoul(to->valuestring, NULL, 16);

    enqueue(&cmd);
}

/* Applies one provision request from task context. */
static void apply_provision(const emu_ctl_cmd_t* cmd, uint32_t now_ms, uint32_t* attest_1_at,
                            uint32_t* attest_2_at) {
    if (network_key_set_from_hex(cmd->key_hex) != 0) {
        ESP_LOGW(TAG, "provision rejected: malformed network key");
        return;
    }
    mesh_rederive_beacon_key(); /* beacons pick up the key live, no reboot */

    uint8_t fp[4];
    network_key_fingerprint(fp);
    ESP_LOGI(TAG, "network key provisioned over emu-link (fingerprint %02X%02X%02X%02X)", fp[0],
             fp[1], fp[2], fp[3]);

    *attest_1_at = now_ms + ATTEST_FIRST_MS;
    *attest_2_at = now_ms + ATTEST_SECOND_MS;
}

/* Applies one send request from task context.
 *
 * The send path's return value is reported, never discarded: both entry points
 * refuse work in ordinary conditions (an exhausted airtime lane, a busy
 * channel at the TX gate, a DM whose handshake slots are all taken), and a
 * refusal that logged the same line as a success would make an operator
 * driving the fleet believe a message went out when nothing was transmitted.
 * A refused send is not retried here: the caller asked for one message, and
 * broadcast tier has no retransmission by design, so inventing one would
 * misrepresent what the protocol does. */
static void apply_send(const emu_ctl_cmd_t* cmd) {
    size_t len = strlen(cmd->text);
    if (cmd->dest != 0) {
        /* Non-zero covers both an immediate transmit and a DM queued behind a
         * key exchange; zero is the send path's failure return. */
        if (mesh_send_message(cmd->dest, (const uint8_t*)cmd->text, len) == 0) {
            ESP_LOGW(TAG, "control DM to %08lX refused by the send path: %s",
                     (unsigned long)cmd->dest, cmd->text);
            return;
        }
        ESP_LOGI(TAG, "control DM to %08lX: %s", (unsigned long)cmd->dest, cmd->text);
    } else {
        int rc = mesh_send_broadcast((const uint8_t*)cmd->text, len);
        if (rc != 0) {
            ESP_LOGW(TAG, "control broadcast refused by the send path (rc=%d): %s", rc, cmd->text);
            return;
        }
        ESP_LOGI(TAG, "control broadcast: %s", cmd->text);
    }
}

/* Runs as a FreeRTOS task (not a raw pthread): the mesh send API posts to the
 * mesh task's queue, and network_key_set_from_hex writes NVS, neither of which
 * is safe from the emu_link reader thread under the IDF-linux FreeRTOS port. */
static void emu_control_task(void* arg) {
    (void)arg;
    uint32_t elapsed_ms = 0;
    uint32_t attest_1_at = 0, attest_2_at = 0; /* 0 = nothing scheduled */

    for (;;) {
        emu_ctl_cmd_t cmd;
        while (dequeue(&cmd)) {
            switch (cmd.op) {
            case EMU_CTL_OP_PROVISION:
                apply_provision(&cmd, elapsed_ms, &attest_1_at, &attest_2_at);
                break;
            case EMU_CTL_OP_SEND:
                apply_send(&cmd);
                break;
            case EMU_CTL_OP_ATTEST:
                /* Budget-gated inside, like every attestation; the node logs
                 * its own "Identity attestation TX" line on success. */
                ESP_LOGI(TAG, "control identity attestation requested");
                mesh_trigger_attestation();
                break;
            default:
                break;
            }
        }

        if (attest_1_at != 0 && elapsed_ms >= attest_1_at) {
            attest_1_at = 0;
            mesh_trigger_attestation();
        }
        if (attest_2_at != 0 && elapsed_ms >= attest_2_at) {
            attest_2_at = 0;
            mesh_trigger_attestation();
        }

        vTaskDelay(pdMS_TO_TICKS(EMU_CTL_TICK_MS));
        elapsed_ms += EMU_CTL_TICK_MS;
    }
}

/*
 * Registers the emu-link control handlers and starts the drain task. Returns 0
 * on success, -1 if the task could not be created (the handlers are left
 * registered either way; they only ever enqueue).
 */
int emu_node_start_control(void) {
    emu_link_on("prov", prov_handler, NULL);
    emu_link_on("send", send_handler, NULL);
    emu_link_on("attest", attest_handler, NULL);

    /* 8 KB stack, matching emu_autosend: the send path runs channel/DM crypto,
     * and the provision path runs HKDF for the beacon subkey. */
    if (xTaskCreate(emu_control_task, "emu_control", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start the control task");
        return -1;
    }
    ESP_LOGI(TAG, "emu-link control path ready (prov, send, attest)");
    return 0;
}
