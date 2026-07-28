/**
 * mesh_persist.c: NVS persistence: nonce ceiling, pin store, replay store, network key.
 *
 * Split out of mesh_task.c (issue #86); pure code motion, no behavior change.
 * Shared state and cross-module entry points come from mesh_internal.h.
 */
#include "mesh_internal.h"

#include "nvs.h"
#include "nvs_keys.h"

static const char* TAG = "mesh";

/* Forward declarations for intra-module static helpers. */
static void mesh_replay_store_save_one(nvs_handle_t h, const char* key, replay_table_t* t);
static void mesh_replay_store_load_one(nvs_handle_t h, const char* key, replay_table_t* t);

/* Nonce counter NVS persistence: reserve-ahead ceiling under NVS_NS_NONCE.
 * Not-found (first boot) resumes from ceiling 0, matching nonce_counter's own
 * zero-initialized static state. */
int mesh_nonce_read(uint64_t* ceiling_out, void* ctx) {
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NONCE, NVS_READONLY, &h) != ESP_OK) {
        *ceiling_out = 0;
        return 0;
    }
    size_t len = sizeof(*ceiling_out);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_NONCE_CEILING, ceiling_out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        *ceiling_out = 0;
    }
    return 0;
}

int mesh_nonce_write(uint64_t ceiling, void* ctx) {
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NONCE, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    nvs_set_blob(h, NVS_KEY_NONCE_CEILING, &ceiling, sizeof(ceiling));
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

/* Verified TOFU pin-store persistence (DM forward-secrecy + SAS): serialize the
 * whole pin table (bindings + verified bit + SAS-at-verification) into one blob
 * under NVS_NS_IDENTITY and commit it. Mirrors identity.c's design note: the
 * IN-MEMORY s_identity_pins is authoritative, so a write failure is logged and
 * swallowed, never un-pinning the live table. Runs on the mesh task, the only
 * mutator of s_identity_pins, so the serialize sees a consistent snapshot. */
void mesh_pin_store_save(void) {
    uint8_t buf[IDENTITY_STORE_BLOB_MAX];
    int n = identity_store_serialize(&s_identity_pins, buf, sizeof(buf));
    if (n < 0) {
        ESP_LOGW(TAG, "pin-store serialize failed (buf too small); live pins kept");
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS_IDENTITY, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "pin-store NVS open failed; live pins kept");
        return;
    }
    esp_err_t err = nvs_set_blob(h, ID_KEY_PIN_STORE, buf, (size_t)n);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "pin-store NVS write failed (%s); live pins kept", esp_err_to_name(err));
}

/* Boot-time load of the persisted pin table into s_identity_pins. Called AFTER
 * identity_store_init and AFTER the anchor is provisioned (identity_store_
 * deserialize preserves the anchor while it rebuilds the pin table). A missing
 * blob (fresh device) is not an error: the store simply stays empty and TOFU
 * re-establishes. A corrupt/wrong-version blob is rejected by deserialize,
 * which leaves the store initialized and empty. */
void mesh_pin_store_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_IDENTITY, NVS_READONLY, &h) != ESP_OK)
        return; /* namespace not created yet: fresh device, empty store */
    uint8_t buf[IDENTITY_STORE_BLOB_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(h, ID_KEY_PIN_STORE, buf, &len);
    nvs_close(h);
    if (err != ESP_OK)
        return; /* no persisted pins yet */
    if (identity_store_deserialize(&s_identity_pins, buf, len, now_ms()) != 0) {
        ESP_LOGW(TAG, "persisted pin-store rejected (corrupt/old format); starting empty");
        return;
    }
    ESP_LOGI(TAG, "Loaded %d persisted identity pin(s)", identity_store_count(&s_identity_pins));
}

/*
 * Replay-window persistence (issue #72).
 *
 * The replay windows were RAM-only while the SENDER-side nonce counter is
 * durable (nonce_counter's reserve-ahead ceiling). That asymmetry is the
 * bug: after we reboot, our high-water marks are all zero, so a batch
 * captured off the air before the reboot replays cleanly in ascending
 * counter order. Replayed RERR tears down routes; replayed LOCATION and
 * CHAT are integrity and privacy problems. OTA reboots the node on demand,
 * which hands an attacker the trigger.
 *
 * Write strategy, because NVS lives on NOR flash with finite erase
 * endurance and this table is touched on EVERY received packet: never write
 * on the receive path. The table sets a dirty flag, and a periodic tick
 * flushes at most once per REPLAY_FLUSH_INTERVAL_MS, and only when
 * something actually changed. See docs/quality-policy.md style: the
 * endurance arithmetic is in the PR body.
 *
 * The residual exposure is bounded by the flush interval rather than by
 * "everything ever captured": an unclean crash can lose at most one
 * interval of high-water advance. Two things narrow that further. Restored
 * slots come back with a full below-window bitmap (fail closed), and the
 * tier-1 CHAT path below now also applies the authenticated sent_at
 * freshness bound that previously only ran on the below-window path.
 *
 * Mirrors mesh_pin_store_save: the IN-MEMORY table is authoritative, so a
 * write failure is logged and swallowed. Runs on the mesh task, the only
 * mutator of the tables, so the serialize sees a consistent snapshot.
 */
#define REPLAY_FLUSH_INTERVAL_MS (15u * 60u * 1000u)
static uint32_t s_last_replay_flush_ms;

/* Static rather than a stack buffer: the flush runs from
 * mesh_periodic_maintenance, which already sits at the bottom of a deep call
 * chain on the 10 KB mesh task stack, and this repo has burned itself on
 * stack overflows that only reproduced on real hardware. Both the save and
 * load paths run on the mesh task and never overlap, so one buffer serves
 * both. */
static uint8_t s_replay_blob[REPLAY_TABLE_BLOB_MAX];

static void mesh_replay_store_save_one(nvs_handle_t h, const char* key, replay_table_t* t) {
    uint8_t* buf = s_replay_blob;
    int n = replay_table_serialize(t, buf, sizeof(s_replay_blob));
    if (n < 0) {
        ESP_LOGW(TAG, "replay-window serialize failed for '%s'; live window kept", key);
        return;
    }
    esp_err_t err = nvs_set_blob(h, key, buf, (size_t)n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "replay-window NVS write failed for '%s' (%s); live window kept", key,
                 esp_err_to_name(err));
        return;
    }
    replay_table_mark_clean(t);
}

/* Flush both windows if either is dirty, rather than gating each blob's
 * write independently on its own dirty flag. Writing only the dirty table
 * was considered and rejected: it would need a second rate-limit
 * timestamp so the two blobs' flush cadence cannot skew apart, for a
 * savings the endurance budget does not need. The endurance arithmetic in
 * PR #150 already assumes two full blobs per flush round and still lands
 * at roughly 27 years of NOR headroom, so always writing both blobs is
 * not a hidden cost, just simpler. force=true bypasses the rate limit
 * (used before a deliberate reboot, e.g. OTA). */
void mesh_replay_store_save(bool force) {
    if (!replay_table_is_dirty(&s_replay) && !replay_table_is_dirty(&s_control_replay))
        return;
    uint32_t t = now_ms();
    if (!force && (uint32_t)(t - s_last_replay_flush_ms) < REPLAY_FLUSH_INTERVAL_MS)
        return;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_REPLAY, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "replay-window NVS open failed; live windows kept");
        return;
    }
    mesh_replay_store_save_one(h, RP_KEY_DATA_WINDOW, &s_replay);
    mesh_replay_store_save_one(h, RP_KEY_CTRL_WINDOW, &s_control_replay);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "replay-window NVS commit failed (%s); live windows kept",
                 esp_err_to_name(err));
        /* The mark_clean above was optimistic; re-arm so the next tick
         * retries rather than assuming the blobs landed. */
        s_replay.dirty = 1;
        s_control_replay.dirty = 1;
        return;
    }
    s_last_replay_flush_ms = t;
}

static void mesh_replay_store_load_one(nvs_handle_t h, const char* key, replay_table_t* t) {
    uint8_t* buf = s_replay_blob;
    size_t len = sizeof(s_replay_blob);
    esp_err_t err = nvs_get_blob(h, key, buf, &len);
    if (err != ESP_OK)
        return; /* nothing persisted yet: fresh device */
    if (replay_table_deserialize(t, buf, len, now_ms()) != 0) {
        /* Corruption is DETECTED, not silently loaded. There is no data to
         * fall back to, so the window starts empty exactly as it did before
         * this change, but it is loud instead of invisible. */
        ESP_LOGW(TAG,
                 "persisted replay window '%s' rejected (corrupt/old format); "
                 "replay protection for known senders restarts empty",
                 key);
        return;
    }
    ESP_LOGI(TAG, "Loaded persisted replay window '%s'", key);
}

/* Boot-time restore, called immediately after replay_table_init. */
void mesh_replay_store_load(void) {
    /* Set unconditionally, including on the fresh-device path below, so the
     * first flush is one full interval after boot rather than immediately. */
    s_last_replay_flush_ms = now_ms();
    nvs_handle_t h;
    if (nvs_open(NVS_NS_REPLAY, NVS_READONLY, &h) != ESP_OK)
        return; /* namespace not created yet: fresh device */
    mesh_replay_store_load_one(h, RP_KEY_DATA_WINDOW, &s_replay);
    mesh_replay_store_load_one(h, RP_KEY_CTRL_WINDOW, &s_control_replay);
    nvs_close(h);
}

/*
 * Mandatory-provisioning (Task 2): consolidate boot-time key load onto the
 * network_key component (single source of truth for the NVS namespace/key and
 * the in-memory provisioning state). A stored key -> provisioned; none stored
 * -> the node stays UNPROVISIONED and INERT (no public-PSK fallback), which is
 * the shipped default until an operator provisions one. Logged clearly so the
 * boot state is unambiguous (a status field for Task 3's provisioning UX).
 */
void mesh_load_network_key(void) {
#ifdef CONFIG_IDF_TARGET_LINUX
    /* Emulator only: seed the shared network key from EMU_NETWORK_KEY so a gosim
     * scenario can key up a headless fleet (there is no emu-link provisioning
     * RPC). No-op when the env var is unset. Implemented in emulator/node and
     * linked only on the linux target; a device build never sees this. */
    extern int emu_node_seed_network_key_from_env(void);
    emu_node_seed_network_key_from_env();
#endif
#ifdef BRAMBLE_PLATFORM_NRF
    /* Bench only: the nRF target has no RPC transport until P2, so a local
     * build may seed the key at compile time (nrf/src/nrf_provision.c). No-op
     * without the define; the key value is never committed. */
    extern int nrf_seed_network_key_from_build(void);
    nrf_seed_network_key_from_build();
#endif
    if (network_key_load_from_nvs() == 0) {
        ESP_LOGI(TAG, "Network key loaded from NVS (provisioned)");
    } else {
        ESP_LOGW(TAG, "No network key provisioned: node is INERT until provisioned "
                      "(setNetworkKey or generate)");
    }
}
