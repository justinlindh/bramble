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
#define NVS_NS_WIFI "bramble_wifi"          /**< Wi-Fi credentials       */
#define NVS_NS_IDENTITY "bramble_id"        /**< Node identity / keys    */
#define NVS_NS_BACKPRESSURE "bramble_bp"    /**< Backpressure state      */
#define NVS_NS_TELEMETRY_DBG "bramble_tdbg" /**< Telemetry debug flags   */
#define NVS_NS_OTA "bramble_ota"            /**< OTA origin + rollback   */

/* ── Frequently-used NVS keys (NVS_NS_BRAMBLE namespace) ────────────────── */
#define NVS_KEY_NODE_NAME "node_name"
#define NVS_KEY_AUTH_TOKEN "auth_token"
#define NVS_KEY_AUTH_OFF "auth_off"
#define NVS_KEY_WS_ORIGINS "ws_origins"
#define NVS_KEY_CONN_MODE "conn_mode"
#define NVS_KEY_OLED_ROT "oled_rot"

/* ── NVS_NS_OTA keys ─────────────────────────────────────────────────────── */
#define NVS_KEY_OTA_ORIGIN "origin"
#define NVS_KEY_OTA_VER_FLOOR "ver_floor"

/* ── NVS_NS_WIFI keys ────────────────────────────────────────────────────── */
#define NVS_KEY_WIFI_SSID "ssid"
#define NVS_KEY_WIFI_PASSWORD "password"
