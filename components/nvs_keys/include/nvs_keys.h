/**
 * @file nvs_keys.h
 * @brief Central registry for NVS namespace names and frequently-used key names.
 *
 * Use these defines everywhere NVS namespaces or keys are referenced so that
 * string literals are never scattered across multiple translation units.
 * Maximum NVS namespace length is 15 characters.
 */

#pragma once

/* ── NVS partition ───────────────────────────────────────────────────────── */
#define NVS_PARTITION "nvs"

/* ── NVS namespaces ──────────────────────────────────────────────────────── */
#define NVS_NS_BRAMBLE "bramble"            /**< General device config   */
#define NVS_NS_RADIO "bramble_radio"        /**< Radio / LoRa settings   */
#define NVS_NS_LOCATION "bramble_loc"       /**< Location & contacts     */
#define NVS_NS_CHANNEL "bramble_ch"         /**< Channel storage         */
#define NVS_NS_MAILBOX "bramble_mb"         /**< Mailbox policy          */
#define NVS_NS_FLOOD "bramble_flood"        /**< Flood transport toggle  */
#define NVS_NS_WIFI "bramble_wifi"          /**< Wi-Fi credentials       */
#define NVS_NS_IDENTITY "bramble_id"        /**< Node identity / keys    */
#define NVS_NS_BACKPRESSURE "bramble_bp"    /**< Backpressure state      */
#define NVS_NS_TELEMETRY_DBG "bramble_tdbg" /**< Telemetry debug flags   */
#define NVS_NS_OTA "bramble_ota"            /**< OTA origin + rollback   */
#define NVS_NS_NONCE "bramble_nonce"        /**< AEAD nonce counter ceiling */
#define NVS_NS_NETKEY "bramble_netkey"      /**< Control-plane network key (PART 3, staged) */
#define NVS_NS_REPLAY "bramble_rp"          /**< Per-sender replay high-water marks */
#define NVS_NS_BLE_BOND "bramble_ble"       /**< BLE bonds (nRF only) */

/* ── Frequently-used NVS keys (NVS_NS_BRAMBLE namespace) ────────────────── */
#define NVS_KEY_NODE_NAME "node_name"
#define NVS_KEY_AUTH_TOKEN "auth_token"
#define NVS_KEY_AUTH_OFF "auth_off"
#define NVS_KEY_WS_ORIGINS "ws_origins"
#define NVS_KEY_CONN_MODE "conn_mode"
#define NVS_KEY_OLED_ROT "oled_rot"
#define NVS_KEY_GPS_EN "gps_en"
#define NVS_KEY_TZ "tz"
#define NVS_KEY_BLE_PASSKEY "ble_passkey" /**< Static SMP passkey, u32 0..999999 (displayless boards) */

/* ── NVS_NS_OTA keys ─────────────────────────────────────────────────────── */
#define NVS_KEY_OTA_ORIGIN "origin"
#define NVS_KEY_OTA_VER_FLOOR "ver_floor"

/* ── NVS_NS_WIFI keys ────────────────────────────────────────────────────── */
#define NVS_KEY_WIFI_SSID "ssid"
#define NVS_KEY_WIFI_PASSWORD "password"

/* ── NVS_NS_NONCE keys ───────────────────────────────────────────────────── */
#define NVS_KEY_NONCE_CEILING "ceiling"

/* -- NVS_NS_IDENTITY keys -------------------------------------------------- */
/* Fleet trust-anchor PUBLIC key (trust-anchor campaign, P0). Persisted per
 * node; the device never holds the anchor private key. Name must be <= 15
 * chars. */
#define ID_KEY_ANCHOR_PUB "anchor_pub"

/* Own endorsement certificate (trust-anchor campaign, P1): the anchor's
 * signature vouching for THIS node's identity key, provisioned via
 * setEndorsement. not_after is an 8-byte big-endian blob; the signature is a
 * 64-byte blob. Names must be <= 15 chars. */
#define ID_KEY_ENDORSE_NA "endorse_na"
#define ID_KEY_ENDORSE_SIG "endorse_sig"

/* Serialized verified TOFU pin table (DM forward-secrecy + SAS): the pin
 * bindings plus each pin's verified bit and SAS-at-verification, so a
 * "verified once, stays verified" model survives reboot. See
 * identity_store_serialize. Name must be <= 15 chars. */
#define ID_KEY_PIN_STORE "pin_store"

/* ── NVS_NS_NETKEY keys ──────────────────────────────────────────────────── */
#define NVS_KEY_NETKEY "key"

/* ── NVS_NS_BLE_BOND keys (nRF only) ─────────────────────────────────────── */
/* NimBLE's three bond records, one blob each: our and the peer's security
 * material and the peer's CCCD subscriptions. See nrf/src/ble_store_nvs.c.
 * Names must be <= 15 chars. */
#define BLE_KEY_OUR_SECS "our_secs"
#define BLE_KEY_PEER_SECS "peer_secs"
#define BLE_KEY_CCCDS "cccds"

/* ── NVS_NS_REPLAY keys ──────────────────────────────────────────────────── */
/* Serialized per-sender replay high-water marks, one blob per window, so a
 * reboot does not reopen the replay window (issue #72). See
 * replay_table_serialize. Names must be <= 15 chars. */
#define RP_KEY_DATA_WINDOW "data_win"
#define RP_KEY_CTRL_WINDOW "ctrl_win"
