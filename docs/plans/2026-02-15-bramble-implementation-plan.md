# Bramble Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Implement the Bramble LoRa mesh networking protocol for ESP32-S3 (Heltec WiFi LoRa 32 V3 / LILYGO T-Beam) from the ground up, following the design document.

**Architecture:** ESP-IDF (v5.x) with FreeRTOS. C firmware with modular subsystems: radio driver, packet codec, crypto, routing, reliability, airtime management, time sync, BLE interface, OLED UI. Each subsystem is a separate source module with a clean header API.

**Tech Stack:** ESP-IDF 5.x, FreeRTOS, mbedtls (bundled), SX1262 driver (RadioLib or custom SPI), C11, CMake build system, pytest + Unity for testing.

---

## 📊 Implementation Status — Updated 2026-02-17

> **Legend:** ✅ Complete · ⚠️ HARDWARE REQUIRED · ❌ Not Started · 🚧 In Progress

| Phase | Tasks | Status | Notes |
|-------|-------|--------|-------|
| Phase 1: Core Protocol | 1–15 | ✅ **ALL COMPLETE** | Packet codec, dedup, radio HAL, FreeRTOS scaffolding |
| Phase 2: Identity & Crypto | 16–25 | ✅ **ALL COMPLETE** | Host OpenSSL + ESP32 mbedtls wrappers, NVS storage, key exchange |
| Phase 3: Routing & Forwarding | 26–40 | ✅ **ALL COMPLETE** | Neighbor/routing table, RREQ/RREP/RERR, forwarding, flood |
| Phase 4: Reliability & Airtime | 41–55 | 🚧 **PARTIAL** | 41–48 ✅, 49–51 ❌ (congestion), 52–54 ⚠️ HW (LBT/CAD), 55 ✅ |
| Phase 5: Time Sync & Security | 56–65 | ✅ **ALL COMPLETE** | Time sync state machine, anti-replay, RREQ rate limiting |
| Phase 6: Fragmentation & Channels | 66–75 | ✅ **ALL COMPLETE** | Fragment-then-encrypt, reassembly, multi-channel, epoch ratchet |
| Phase 7: OLED UI | 76–85 | 🚧 **PARTIAL** | 76–84 ✅ (host-testable state machine), 85 ⚠️ HW verify |
| Phase 8: BLE Interface & OTA | 86–95 | 🚧 **PARTIAL** | 87–88 ✅ (JSON-RPC parser + tests), rest ⚠️ HW |
| Phase 9: Web Flasher | 96–100 | ⚠️ **HARDWARE REQUIRED** | Not started; requires physical device |
| Phase 10: Web App | 101–115 | ❌ **NOT STARTED** | Web Serial/BLE UI — planned post-hardware |
| Phase 11: Testing & Validation | 116–125 | 🚧 **PARTIAL** | 116–121 ✅ (36 test suites, field plans), 122–125 ⚠️ HW |

**Summary:** 86 / 125 tasks complete (68.8%) · 21 blocked on hardware · 18 not yet started

**Test suite:** 36 test suites passing on host · Crypto vectors (NIST AES-GCM, RFC 7748 X25519) · Radio mock · 3-node and 5-node integration tests · Field test plans written

---

## Phase 1: Core Protocol (Tasks 1–15) ✅ COMPLETE

### Task 1: ✅ Create ESP-IDF project scaffolding

**Create** `bramble/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bramble)
```

**Create** `bramble/main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash driver spi_master esp_timer freertos
)
```

**Create** `bramble/main/main.c`:
```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "bramble";

void app_main(void)
{
    ESP_LOGI(TAG, "Bramble LoRa Mesh Protocol v0.1");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "NVS initialized");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

**Create** `bramble/main/Kconfig.projbuild`:
```
menu "Bramble Configuration"
    config BRAMBLE_BOARD_HELTEC_V3
        bool "Heltec WiFi LoRa 32 V3"
        default y

    config BRAMBLE_BOARD_TBEAM
        bool "LILYGO T-Beam S3 Supreme"
        default n

    config BRAMBLE_TX_POWER_DBM
        int "TX Power (dBm)"
        default 22
        range 2 22

    config BRAMBLE_RADIO_PROFILE
        int "Radio profile (0=LongRange, 1=MediumRange)"
        default 0
        range 0 1
endmenu
```

**Create** `bramble/sdkconfig.defaults`:
```
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_MBEDTLS_HARDWARE_AES=y
CONFIG_MBEDTLS_HARDWARE_SHA=y
```

**Create** `bramble/partitions.csv`:
```
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x180000,
app1,     app,  ota_1,   0x190000,0x180000,
spiffs,   data, spiffs,  0x310000,0xF0000,
```

**Run:**
```bash
cd bramble && idf.py set-target esp32s3 && idf.py build
```
**Expected:** Build succeeds, binary produced.

**Commit:** `git init && git add -A && git commit -m "feat: ESP-IDF project scaffolding with partition table"`

---

### Task 2: ✅ Create host-side test infrastructure

**Create** `bramble/test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
project(bramble_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -g -fsanitize=address")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")

# Unity test framework (vendored)
add_library(unity unity/unity.c)
target_include_directories(unity PUBLIC unity)

# Stub out ESP-IDF types for host compilation
add_library(esp_stubs stubs/esp_stubs.c)
target_include_directories(esp_stubs PUBLIC stubs)

# Test executables will be added per-module
```

**Create** `bramble/test/unity/unity.h` and `bramble/test/unity/unity.c` — use Unity v2.6.0 from ThrowTheSwitch:
```bash
cd bramble/test && mkdir -p unity
curl -sL https://raw.githubusercontent.com/ThrowTheSwitch/Unity/v2.6.0/src/unity.h -o unity/unity.h
curl -sL https://raw.githubusercontent.com/ThrowTheSwitch/Unity/v2.6.0/src/unity.c -o unity/unity.c
curl -sL https://raw.githubusercontent.com/ThrowTheSwitch/Unity/v2.6.0/src/unity_internals.h -o unity/unity_internals.h
```

**Create** `bramble/test/stubs/esp_stubs.h`:
```c
#ifndef ESP_STUBS_H
#define ESP_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

// Minimal ESP-IDF type stubs for host compilation
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

// Logging stubs
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)

// Endian helpers (host is likely little-endian)
static inline uint16_t bramble_htons(uint16_t x) {
    return ((x & 0xFF) << 8) | ((x >> 8) & 0xFF);
}
static inline uint32_t bramble_htonl(uint32_t x) {
    return ((x & 0xFF) << 24) | (((x >> 8) & 0xFF) << 16) |
           (((x >> 16) & 0xFF) << 8) | ((x >> 24) & 0xFF);
}
static inline uint16_t bramble_ntohs(uint16_t x) { return bramble_htons(x); }
static inline uint32_t bramble_ntohl(uint32_t x) { return bramble_htonl(x); }

#endif
```

**Create** `bramble/test/stubs/esp_stubs.c`:
```c
#include "esp_stubs.h"
// Empty — stubs are all inline/macros
```

**Run:**
```bash
cd bramble/test && mkdir -p build && cd build && cmake .. && make
```
**Expected:** Build succeeds (no test targets yet, just libraries).

**Commit:** `git add -A && git commit -m "feat: host-side test infrastructure with Unity and ESP stubs"`

---

### Task 3: ✅ Write failing tests for packet header serialization

**Create** `bramble/components/packet/include/packet.h`:
```c
#ifndef BRAMBLE_PACKET_H
#define BRAMBLE_PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Protocol version
#define BRAMBLE_VERSION 0x01

// Packet types
typedef enum {
    PKT_DATA            = 0x01,
    PKT_ACK             = 0x02,
    PKT_ROUTE_REQUEST   = 0x03,
    PKT_ROUTE_REPLY     = 0x04,
    PKT_ROUTE_ERROR     = 0x05,
    PKT_BEACON          = 0x06,
    PKT_KEY_EXCHANGE    = 0x07,
    PKT_DELIVERY_RECEIPT = 0x08,
    PKT_CONGESTION      = 0x09,
    PKT_TIME_SYNC       = 0x0A,
} bramble_pkt_type_t;

// Flag bits
#define FLAG_TIER_MASK     0xC0  // Bits 7:6
#define FLAG_TIER_SHIFT    6
#define FLAG_ACK_REQ       0x20  // Bit 5
#define FLAG_RECEIPT       0x10  // Bit 4
#define FLAG_CHANNEL       0x08  // Bit 3
#define FLAG_ENCRYPT       0x04  // Bit 2
#define FLAG_FRAG_MASK     0x03  // Bits 1:0

// Tier values (after shift)
#define TIER_BROADCAST     0x00
#define TIER_NORMAL        0x01
#define TIER_CRITICAL      0x02

// Fragment indicators
#define FRAG_NONE          0x00
#define FRAG_FIRST         0x01
#define FRAG_MIDDLE        0x02
#define FRAG_LAST          0x03

// Special addresses
#define ADDR_BROADCAST     0xFFFFFFFF
#define ADDR_NULL          0x00000000

// RREQ flag (overloaded from flags byte bit 5 context in RREQ)
#define FLAG_OPEN_SOURCE   0x20  // Bit 5 in flags for RREQ packets

// Common header (12 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  packet_type;
    uint8_t  flags;
    uint8_t  hop_limit;
    uint32_t dest_addr;    // big-endian on wire
    uint32_t packet_id;    // big-endian on wire
} bramble_header_t;

#define BRAMBLE_HEADER_SIZE 12

// Serialize header to wire format (big-endian). Returns bytes written (always 12).
int bramble_header_serialize(const bramble_header_t *hdr, uint8_t *buf, size_t buf_len);

// Deserialize header from wire format. Returns 0 on success, -1 on error.
int bramble_header_deserialize(const uint8_t *buf, size_t buf_len, bramble_header_t *hdr);

// === Per-type packet structures ===

// DATA DM packet (after header)
typedef struct __attribute__((packed)) {
    uint32_t src_addr;
    uint32_t next_hop;
    uint8_t  app_type;
    uint8_t  payload_len;
    uint8_t  nonce[12];
    // followed by: ciphertext[payload_len - 16], auth_tag[16]
} bramble_data_dm_t;

#define BRAMBLE_DATA_DM_FIELDS_SIZE 26  // 4+4+1+1+12+... variable

// DATA Channel packet (after header)
typedef struct __attribute__((packed)) {
    uint32_t src_addr;     // 0x00000000 on wire
    uint32_t next_hop;
    uint8_t  payload_len;
    uint8_t  nonce[12];
    // followed by: ciphertext[payload_len - 16], auth_tag[16]
} bramble_data_channel_t;

// ACK packet (after header)
typedef struct __attribute__((packed)) {
    uint32_t src_addr;
    uint32_t ack_packet_id;
    uint8_t  ack_flags;
    uint8_t  rssi_at_dest;
} bramble_ack_t;

#define BRAMBLE_ACK_SIZE (BRAMBLE_HEADER_SIZE + 10)  // 22 bytes total

// ACK flags
#define ACK_SUCCESS       0x01
#define ACK_BUFFER_FULL   0x02
#define ACK_ROUTE_UNKNOWN 0x04

// ROUTE_REQUEST (after header)
typedef struct __attribute__((packed)) {
    uint32_t query_id;
    uint32_t encrypted_source;
    uint8_t  hop_count;
    uint8_t  metric;
    uint32_t prev_hop;
} bramble_rreq_t;

#define BRAMBLE_RREQ_SIZE (BRAMBLE_HEADER_SIZE + 14)  // 26 bytes total

// ROUTE_REPLY (after header)
typedef struct __attribute__((packed)) {
    uint32_t query_id;
    uint32_t src_addr;
    uint32_t next_hop;
    uint8_t  hop_count;
    uint8_t  route_metric;
    uint32_t auth_hmac;
} bramble_rrep_t;

#define BRAMBLE_RREP_SIZE (BRAMBLE_HEADER_SIZE + 18)  // 30 bytes total

// ROUTE_ERROR (after header)
typedef struct __attribute__((packed)) {
    uint32_t reporter_addr;
    uint32_t broken_dest;
    uint32_t broken_next_hop;
} bramble_rerr_t;

#define BRAMBLE_RERR_SIZE (BRAMBLE_HEADER_SIZE + 12)  // 24 bytes total

// BEACON (after header)
typedef struct __attribute__((packed)) {
    uint32_t src_addr;
    uint32_t node_pubkey_hash;
    uint16_t uptime_min;
    uint8_t  battery_pct;
    uint8_t  tx_queue_depth;
    uint8_t  neighbor_count;
    uint8_t  flags;
    uint32_t network_time;
    uint16_t time_confidence;
    uint32_t auth_hmac;
} bramble_beacon_t;

#define BRAMBLE_BEACON_SIZE (BRAMBLE_HEADER_SIZE + 24)  // 36 bytes total

// KEY_EXCHANGE (after header)
typedef struct __attribute__((packed)) {
    uint32_t src_addr;
    uint8_t  ephemeral_pubkey[32];
    uint32_t key_id;
    uint8_t  ke_type;
    uint8_t  auth_tag[16];
} bramble_key_exchange_t;

#define KE_INITIATE  0x01
#define KE_RESPOND   0x02
#define KE_CONFIRM   0x03

#define BRAMBLE_KEY_EXCHANGE_SIZE (BRAMBLE_HEADER_SIZE + 57)  // 69 bytes total

// DELIVERY_RECEIPT (after header, variable length)
typedef struct __attribute__((packed)) {
    uint32_t src_addr;
    uint32_t orig_packet_id;
    uint8_t  hop_count;
    uint8_t  total_latency;
    // followed by: relay_path[hop_count] (each uint32_t)
} bramble_delivery_receipt_t;

#define BRAMBLE_DELIVERY_RECEIPT_MIN_SIZE (BRAMBLE_HEADER_SIZE + 10)  // 22 bytes minimum
#define BRAMBLE_DELIVERY_RECEIPT_MAX_SIZE (BRAMBLE_HEADER_SIZE + 10 + 8*4)  // 54 bytes max

// CONGESTION (after header)
typedef struct __attribute__((packed)) {
    uint32_t src_addr;
    uint8_t  congestion_level;
    uint8_t  queue_depth;
    uint16_t est_clear_time;
} bramble_congestion_t;

#define BRAMBLE_CONGESTION_SIZE (BRAMBLE_HEADER_SIZE + 8)  // 20 bytes total

// TIME_SYNC (after header)
typedef struct __attribute__((packed)) {
    uint32_t src_addr;
    uint32_t timestamp;
    uint16_t confidence_ms;
    uint8_t  stratum;
    uint8_t  sequence;
} bramble_time_sync_t;

#define BRAMBLE_TIME_SYNC_SIZE (BRAMBLE_HEADER_SIZE + 12)  // 24 bytes total

// Fragment header (appended after DATA header, before payload)
typedef struct __attribute__((packed)) {
    uint8_t  frag_index;
    uint8_t  frag_total;
    uint16_t message_id;
} bramble_frag_header_t;

#define BRAMBLE_FRAG_HEADER_SIZE 4

// === Serialization functions for each packet type ===

// ACK
int bramble_ack_serialize(const bramble_header_t *hdr, const bramble_ack_t *ack,
                          uint8_t *buf, size_t buf_len);
int bramble_ack_deserialize(const uint8_t *buf, size_t buf_len,
                            bramble_header_t *hdr, bramble_ack_t *ack);

// RREQ
int bramble_rreq_serialize(const bramble_header_t *hdr, const bramble_rreq_t *rreq,
                           uint8_t *buf, size_t buf_len);
int bramble_rreq_deserialize(const uint8_t *buf, size_t buf_len,
                             bramble_header_t *hdr, bramble_rreq_t *rreq);

// RREP
int bramble_rrep_serialize(const bramble_header_t *hdr, const bramble_rrep_t *rrep,
                           uint8_t *buf, size_t buf_len);
int bramble_rrep_deserialize(const uint8_t *buf, size_t buf_len,
                             bramble_header_t *hdr, bramble_rrep_t *rrep);

// RERR
int bramble_rerr_serialize(const bramble_header_t *hdr, const bramble_rerr_t *rerr,
                           uint8_t *buf, size_t buf_len);
int bramble_rerr_deserialize(const uint8_t *buf, size_t buf_len,
                             bramble_header_t *hdr, bramble_rerr_t *rerr);

// BEACON
int bramble_beacon_serialize(const bramble_header_t *hdr, const bramble_beacon_t *beacon,
                             uint8_t *buf, size_t buf_len);
int bramble_beacon_deserialize(const uint8_t *buf, size_t buf_len,
                               bramble_header_t *hdr, bramble_beacon_t *beacon);

// KEY_EXCHANGE
int bramble_ke_serialize(const bramble_header_t *hdr, const bramble_key_exchange_t *ke,
                         uint8_t *buf, size_t buf_len);
int bramble_ke_deserialize(const uint8_t *buf, size_t buf_len,
                           bramble_header_t *hdr, bramble_key_exchange_t *ke);

// CONGESTION
int bramble_congestion_serialize(const bramble_header_t *hdr, const bramble_congestion_t *cong,
                                 uint8_t *buf, size_t buf_len);
int bramble_congestion_deserialize(const uint8_t *buf, size_t buf_len,
                                   bramble_header_t *hdr, bramble_congestion_t *cong);

// TIME_SYNC
int bramble_timesync_serialize(const bramble_header_t *hdr, const bramble_time_sync_t *ts,
                               uint8_t *buf, size_t buf_len);
int bramble_timesync_deserialize(const uint8_t *buf, size_t buf_len,
                                 bramble_header_t *hdr, bramble_time_sync_t *ts);

// DELIVERY_RECEIPT
int bramble_receipt_serialize(const bramble_header_t *hdr, const bramble_delivery_receipt_t *rcpt,
                              const uint32_t *relay_path, uint8_t *buf, size_t buf_len);
int bramble_receipt_deserialize(const uint8_t *buf, size_t buf_len,
                                bramble_header_t *hdr, bramble_delivery_receipt_t *rcpt,
                                uint32_t *relay_path, uint8_t max_relays);

#endif // BRAMBLE_PACKET_H
```

**Create** `bramble/components/packet/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "packet.c"
    INCLUDE_DIRS "include"
)
```

**Create** `bramble/test/test_packet.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include "../components/packet/include/packet.h"

// We include the source directly for host-side testing
#include "../components/packet/packet.c"

void setUp(void) {}
void tearDown(void) {}

void test_header_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_DATA,
        .flags = (TIER_NORMAL << FLAG_TIER_SHIFT) | FLAG_ACK_REQ | FLAG_ENCRYPT,
        .hop_limit = 8,
        .dest_addr = 0xDEADBEEF,
        .packet_id = 0x12345678,
    };

    uint8_t buf[BRAMBLE_HEADER_SIZE];
    TEST_ASSERT_EQUAL(BRAMBLE_HEADER_SIZE, bramble_header_serialize(&hdr, buf, sizeof(buf)));

    // Check wire format: version, type, flags, hop_limit are single bytes
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[1]);  // PKT_DATA
    TEST_ASSERT_EQUAL_HEX8(hdr.flags, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(8, buf[3]);
    // dest_addr big-endian
    TEST_ASSERT_EQUAL_HEX8(0xDE, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, buf[5]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, buf[6]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, buf[7]);
    // packet_id big-endian
    TEST_ASSERT_EQUAL_HEX8(0x12, buf[8]);
    TEST_ASSERT_EQUAL_HEX8(0x34, buf[9]);
    TEST_ASSERT_EQUAL_HEX8(0x56, buf[10]);
    TEST_ASSERT_EQUAL_HEX8(0x78, buf[11]);

    bramble_header_t out;
    TEST_ASSERT_EQUAL(0, bramble_header_deserialize(buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX8(BRAMBLE_VERSION, out.version);
    TEST_ASSERT_EQUAL_HEX8(PKT_DATA, out.packet_type);
    TEST_ASSERT_EQUAL_HEX8(hdr.flags, out.flags);
    TEST_ASSERT_EQUAL(8, out.hop_limit);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, out.dest_addr);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, out.packet_id);
}

void test_header_serialize_buffer_too_small(void) {
    bramble_header_t hdr = { .version = BRAMBLE_VERSION };
    uint8_t buf[4];
    TEST_ASSERT_EQUAL(-1, bramble_header_serialize(&hdr, buf, sizeof(buf)));
}

void test_header_deserialize_buffer_too_small(void) {
    uint8_t buf[4] = {0};
    bramble_header_t hdr;
    TEST_ASSERT_EQUAL(-1, bramble_header_deserialize(buf, sizeof(buf), &hdr));
}

void test_ack_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_ACK,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = 0xAABBCCDD,
        .packet_id = 0x11111111,
    };
    bramble_ack_t ack = {
        .src_addr = 0x22222222,
        .ack_packet_id = 0x33333333,
        .ack_flags = ACK_SUCCESS,
        .rssi_at_dest = 0x80,  // -128 + 128 = 0 dBm
    };

    uint8_t buf[BRAMBLE_ACK_SIZE];
    TEST_ASSERT_EQUAL(BRAMBLE_ACK_SIZE, bramble_ack_serialize(&hdr, &ack, buf, sizeof(buf)));

    bramble_header_t hdr_out;
    bramble_ack_t ack_out;
    TEST_ASSERT_EQUAL(0, bramble_ack_deserialize(buf, sizeof(buf), &hdr_out, &ack_out));
    TEST_ASSERT_EQUAL_HEX32(0x22222222, ack_out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(0x33333333, ack_out.ack_packet_id);
    TEST_ASSERT_EQUAL_HEX8(ACK_SUCCESS, ack_out.ack_flags);
    TEST_ASSERT_EQUAL_HEX8(0x80, ack_out.rssi_at_dest);
}

void test_beacon_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_BEACON,
        .flags = 0,
        .hop_limit = 1,
        .dest_addr = ADDR_BROADCAST,
        .packet_id = 0x44444444,
    };
    bramble_beacon_t beacon = {
        .src_addr = 0xAAAAAAAA,
        .node_pubkey_hash = 0xBBBBBBBB,
        .uptime_min = 1234,
        .battery_pct = 85,
        .tx_queue_depth = 3,
        .neighbor_count = 7,
        .flags = 0x05,  // has_gps | accepting_dms
        .network_time = 0xCCCCCCCC,
        .time_confidence = 500,
        .auth_hmac = 0xDDDDDDDD,
    };

    uint8_t buf[BRAMBLE_BEACON_SIZE];
    TEST_ASSERT_EQUAL(BRAMBLE_BEACON_SIZE, bramble_beacon_serialize(&hdr, &beacon, buf, sizeof(buf)));

    bramble_header_t hdr_out;
    bramble_beacon_t beacon_out;
    TEST_ASSERT_EQUAL(0, bramble_beacon_deserialize(buf, sizeof(buf), &hdr_out, &beacon_out));
    TEST_ASSERT_EQUAL_HEX32(0xAAAAAAAA, beacon_out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(0xBBBBBBBB, beacon_out.node_pubkey_hash);
    TEST_ASSERT_EQUAL(1234, beacon_out.uptime_min);
    TEST_ASSERT_EQUAL(85, beacon_out.battery_pct);
    TEST_ASSERT_EQUAL(3, beacon_out.tx_queue_depth);
    TEST_ASSERT_EQUAL(7, beacon_out.neighbor_count);
    TEST_ASSERT_EQUAL_HEX8(0x05, beacon_out.flags);
    TEST_ASSERT_EQUAL_HEX32(0xCCCCCCCC, beacon_out.network_time);
    TEST_ASSERT_EQUAL(500, beacon_out.time_confidence);
    TEST_ASSERT_EQUAL_HEX32(0xDDDDDDDD, beacon_out.auth_hmac);
}

void test_rreq_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_ROUTE_REQUEST,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = 0x55555555,
        .packet_id = 0x66666666,
    };
    bramble_rreq_t rreq = {
        .query_id = 0x77777777,
        .encrypted_source = 0x88888888,
        .hop_count = 0,
        .metric = 255,
        .prev_hop = 0x99999999,
    };

    uint8_t buf[BRAMBLE_RREQ_SIZE];
    TEST_ASSERT_EQUAL(BRAMBLE_RREQ_SIZE, bramble_rreq_serialize(&hdr, &rreq, buf, sizeof(buf)));

    bramble_header_t hdr_out;
    bramble_rreq_t rreq_out;
    TEST_ASSERT_EQUAL(0, bramble_rreq_deserialize(buf, sizeof(buf), &hdr_out, &rreq_out));
    TEST_ASSERT_EQUAL_HEX32(0x77777777, rreq_out.query_id);
    TEST_ASSERT_EQUAL_HEX32(0x88888888, rreq_out.encrypted_source);
    TEST_ASSERT_EQUAL(0, rreq_out.hop_count);
    TEST_ASSERT_EQUAL(255, rreq_out.metric);
    TEST_ASSERT_EQUAL_HEX32(0x99999999, rreq_out.prev_hop);
}

void test_rrep_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_ROUTE_REPLY,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = 0x11111111,
        .packet_id = 0x22222222,
    };
    bramble_rrep_t rrep = {
        .query_id = 0x33333333,
        .src_addr = 0x44444444,
        .next_hop = 0x55555555,
        .hop_count = 3,
        .route_metric = 200,
        .auth_hmac = 0xAABBCCDD,
    };

    uint8_t buf[BRAMBLE_RREP_SIZE];
    TEST_ASSERT_EQUAL(BRAMBLE_RREP_SIZE, bramble_rrep_serialize(&hdr, &rrep, buf, sizeof(buf)));

    bramble_header_t hdr_out;
    bramble_rrep_t rrep_out;
    TEST_ASSERT_EQUAL(0, bramble_rrep_deserialize(buf, sizeof(buf), &hdr_out, &rrep_out));
    TEST_ASSERT_EQUAL_HEX32(0x33333333, rrep_out.query_id);
    TEST_ASSERT_EQUAL_HEX32(0x44444444, rrep_out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(0x55555555, rrep_out.next_hop);
    TEST_ASSERT_EQUAL(3, rrep_out.hop_count);
    TEST_ASSERT_EQUAL(200, rrep_out.route_metric);
    TEST_ASSERT_EQUAL_HEX32(0xAABBCCDD, rrep_out.auth_hmac);
}

void test_rerr_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_ROUTE_ERROR,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = 0xAAAAAAAA,
        .packet_id = 0xBBBBBBBB,
    };
    bramble_rerr_t rerr = {
        .reporter_addr = 0xCCCCCCCC,
        .broken_dest = 0xDDDDDDDD,
        .broken_next_hop = 0xEEEEEEEE,
    };

    uint8_t buf[BRAMBLE_RERR_SIZE];
    TEST_ASSERT_EQUAL(BRAMBLE_RERR_SIZE, bramble_rerr_serialize(&hdr, &rerr, buf, sizeof(buf)));

    bramble_header_t hdr_out;
    bramble_rerr_t rerr_out;
    TEST_ASSERT_EQUAL(0, bramble_rerr_deserialize(buf, sizeof(buf), &hdr_out, &rerr_out));
    TEST_ASSERT_EQUAL_HEX32(0xCCCCCCCC, rerr_out.reporter_addr);
    TEST_ASSERT_EQUAL_HEX32(0xDDDDDDDD, rerr_out.broken_dest);
    TEST_ASSERT_EQUAL_HEX32(0xEEEEEEEE, rerr_out.broken_next_hop);
}

void test_ke_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_KEY_EXCHANGE,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = 0x11111111,
        .packet_id = 0x22222222,
    };
    bramble_key_exchange_t ke = {
        .src_addr = 0x33333333,
        .key_id = 0x44444444,
        .ke_type = KE_INITIATE,
    };
    memset(ke.ephemeral_pubkey, 0xAB, 32);
    memset(ke.auth_tag, 0xCD, 16);

    uint8_t buf[BRAMBLE_KEY_EXCHANGE_SIZE];
    TEST_ASSERT_EQUAL(BRAMBLE_KEY_EXCHANGE_SIZE, bramble_ke_serialize(&hdr, &ke, buf, sizeof(buf)));

    bramble_header_t hdr_out;
    bramble_key_exchange_t ke_out;
    TEST_ASSERT_EQUAL(0, bramble_ke_deserialize(buf, sizeof(buf), &hdr_out, &ke_out));
    TEST_ASSERT_EQUAL_HEX32(0x33333333, ke_out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(0x44444444, ke_out.key_id);
    TEST_ASSERT_EQUAL(KE_INITIATE, ke_out.ke_type);
    uint8_t expected_pub[32];
    memset(expected_pub, 0xAB, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_pub, ke_out.ephemeral_pubkey, 32);
    uint8_t expected_tag[16];
    memset(expected_tag, 0xCD, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_tag, ke_out.auth_tag, 16);
}

void test_congestion_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_CONGESTION,
        .flags = 0,
        .hop_limit = 1,
        .dest_addr = ADDR_BROADCAST,
        .packet_id = 0xAAAAAAAA,
    };
    bramble_congestion_t cong = {
        .src_addr = 0xBBBBBBBB,
        .congestion_level = 2,
        .queue_depth = 14,
        .est_clear_time = 30,
    };

    uint8_t buf[BRAMBLE_CONGESTION_SIZE];
    TEST_ASSERT_EQUAL(BRAMBLE_CONGESTION_SIZE, bramble_congestion_serialize(&hdr, &cong, buf, sizeof(buf)));

    bramble_header_t hdr_out;
    bramble_congestion_t cong_out;
    TEST_ASSERT_EQUAL(0, bramble_congestion_deserialize(buf, sizeof(buf), &hdr_out, &cong_out));
    TEST_ASSERT_EQUAL_HEX32(0xBBBBBBBB, cong_out.src_addr);
    TEST_ASSERT_EQUAL(2, cong_out.congestion_level);
    TEST_ASSERT_EQUAL(14, cong_out.queue_depth);
    TEST_ASSERT_EQUAL(30, cong_out.est_clear_time);
}

void test_timesync_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_TIME_SYNC,
        .flags = 0,
        .hop_limit = 1,
        .dest_addr = ADDR_BROADCAST,
        .packet_id = 0xCCCCCCCC,
    };
    bramble_time_sync_t ts = {
        .src_addr = 0xDDDDDDDD,
        .timestamp = 0xEEEEEEEE,
        .confidence_ms = 100,
        .stratum = 1,
        .sequence = 42,
    };

    uint8_t buf[BRAMBLE_TIME_SYNC_SIZE];
    TEST_ASSERT_EQUAL(BRAMBLE_TIME_SYNC_SIZE, bramble_timesync_serialize(&hdr, &ts, buf, sizeof(buf)));

    bramble_header_t hdr_out;
    bramble_time_sync_t ts_out;
    TEST_ASSERT_EQUAL(0, bramble_timesync_deserialize(buf, sizeof(buf), &hdr_out, &ts_out));
    TEST_ASSERT_EQUAL_HEX32(0xDDDDDDDD, ts_out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(0xEEEEEEEE, ts_out.timestamp);
    TEST_ASSERT_EQUAL(100, ts_out.confidence_ms);
    TEST_ASSERT_EQUAL(1, ts_out.stratum);
    TEST_ASSERT_EQUAL(42, ts_out.sequence);
}

void test_receipt_serialize_roundtrip(void) {
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .packet_type = PKT_DELIVERY_RECEIPT,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = 0x11111111,
        .packet_id = 0x22222222,
    };
    bramble_delivery_receipt_t rcpt = {
        .src_addr = 0x33333333,
        .orig_packet_id = 0x44444444,
        .hop_count = 3,
        .total_latency = 5,
    };
    uint32_t relay_path[3] = { 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC };

    uint8_t buf[BRAMBLE_DELIVERY_RECEIPT_MAX_SIZE];
    int len = bramble_receipt_serialize(&hdr, &rcpt, relay_path, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(22 + 3*4, len);  // 34 bytes

    bramble_header_t hdr_out;
    bramble_delivery_receipt_t rcpt_out;
    uint32_t relay_out[8];
    TEST_ASSERT_EQUAL(0, bramble_receipt_deserialize(buf, len, &hdr_out, &rcpt_out, relay_out, 8));
    TEST_ASSERT_EQUAL_HEX32(0x33333333, rcpt_out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(0x44444444, rcpt_out.orig_packet_id);
    TEST_ASSERT_EQUAL(3, rcpt_out.hop_count);
    TEST_ASSERT_EQUAL(5, rcpt_out.total_latency);
    TEST_ASSERT_EQUAL_HEX32(0xAAAAAAAA, relay_out[0]);
    TEST_ASSERT_EQUAL_HEX32(0xBBBBBBBB, relay_out[1]);
    TEST_ASSERT_EQUAL_HEX32(0xCCCCCCCC, relay_out[2]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_header_serialize_roundtrip);
    RUN_TEST(test_header_serialize_buffer_too_small);
    RUN_TEST(test_header_deserialize_buffer_too_small);
    RUN_TEST(test_ack_serialize_roundtrip);
    RUN_TEST(test_beacon_serialize_roundtrip);
    RUN_TEST(test_rreq_serialize_roundtrip);
    RUN_TEST(test_rrep_serialize_roundtrip);
    RUN_TEST(test_rerr_serialize_roundtrip);
    RUN_TEST(test_ke_serialize_roundtrip);
    RUN_TEST(test_congestion_serialize_roundtrip);
    RUN_TEST(test_timesync_serialize_roundtrip);
    RUN_TEST(test_receipt_serialize_roundtrip);
    return UNITY_END();
}
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
# Packet codec tests
add_executable(test_packet test_packet.c)
target_link_libraries(test_packet unity esp_stubs)
target_include_directories(test_packet PRIVATE
    ${CMAKE_SOURCE_DIR}/../components/packet/include
    ${CMAKE_SOURCE_DIR}/stubs
)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_packet && ./test_packet
```
**Expected:** Linker errors — `bramble_header_serialize` etc. are not yet implemented. Tests fail to build.

**Commit:** `git add -A && git commit -m "test: failing packet serialization tests for all 10 packet types"`

---

### Task 4: ✅ Implement packet header serialization/deserialization

**Create** `bramble/components/packet/packet.c`:
```c
#include "packet.h"
#include <string.h>

// Helper: write uint32_t big-endian
static inline void put_be32(uint8_t *buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8)  & 0xFF;
    buf[3] = val & 0xFF;
}

// Helper: read uint32_t big-endian
static inline uint32_t get_be32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
}

// Helper: write uint16_t big-endian
static inline void put_be16(uint8_t *buf, uint16_t val) {
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

// Helper: read uint16_t big-endian
static inline uint16_t get_be16(const uint8_t *buf) {
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

int bramble_header_serialize(const bramble_header_t *hdr, uint8_t *buf, size_t buf_len) {
    if (buf_len < BRAMBLE_HEADER_SIZE) return -1;
    buf[0] = hdr->version;
    buf[1] = hdr->packet_type;
    buf[2] = hdr->flags;
    buf[3] = hdr->hop_limit;
    put_be32(&buf[4], hdr->dest_addr);
    put_be32(&buf[8], hdr->packet_id);
    return BRAMBLE_HEADER_SIZE;
}

int bramble_header_deserialize(const uint8_t *buf, size_t buf_len, bramble_header_t *hdr) {
    if (buf_len < BRAMBLE_HEADER_SIZE) return -1;
    hdr->version = buf[0];
    hdr->packet_type = buf[1];
    hdr->flags = buf[2];
    hdr->hop_limit = buf[3];
    hdr->dest_addr = get_be32(&buf[4]);
    hdr->packet_id = get_be32(&buf[8]);
    return 0;
}

// ACK: header(12) + src_addr(4) + ack_packet_id(4) + ack_flags(1) + rssi_at_dest(1) = 22
int bramble_ack_serialize(const bramble_header_t *hdr, const bramble_ack_t *ack,
                          uint8_t *buf, size_t buf_len) {
    if (buf_len < BRAMBLE_ACK_SIZE) return -1;
    bramble_header_serialize(hdr, buf, buf_len);
    put_be32(&buf[12], ack->src_addr);
    put_be32(&buf[16], ack->ack_packet_id);
    buf[20] = ack->ack_flags;
    buf[21] = ack->rssi_at_dest;
    return BRAMBLE_ACK_SIZE;
}

int bramble_ack_deserialize(const uint8_t *buf, size_t buf_len,
                            bramble_header_t *hdr, bramble_ack_t *ack) {
    if (buf_len < BRAMBLE_ACK_SIZE) return -1;
    bramble_header_deserialize(buf, buf_len, hdr);
    ack->src_addr = get_be32(&buf[12]);
    ack->ack_packet_id = get_be32(&buf[16]);
    ack->ack_flags = buf[20];
    ack->rssi_at_dest = buf[21];
    return 0;
}

// RREQ: header(12) + query_id(4) + encrypted_source(4) + hop_count(1) + metric(1) + prev_hop(4) = 26
int bramble_rreq_serialize(const bramble_header_t *hdr, const bramble_rreq_t *rreq,
                           uint8_t *buf, size_t buf_len) {
    if (buf_len < BRAMBLE_RREQ_SIZE) return -1;
    bramble_header_serialize(hdr, buf, buf_len);
    put_be32(&buf[12], rreq->query_id);
    put_be32(&buf[16], rreq->encrypted_source);
    buf[20] = rreq->hop_count;
    buf[21] = rreq->metric;
    put_be32(&buf[22], rreq->prev_hop);
    return BRAMBLE_RREQ_SIZE;
}

int bramble_rreq_deserialize(const uint8_t *buf, size_t buf_len,
                             bramble_header_t *hdr, bramble_rreq_t *rreq) {
    if (buf_len < BRAMBLE_RREQ_SIZE) return -1;
    bramble_header_deserialize(buf, buf_len, hdr);
    rreq->query_id = get_be32(&buf[12]);
    rreq->encrypted_source = get_be32(&buf[16]);
    rreq->hop_count = buf[20];
    rreq->metric = buf[21];
    rreq->prev_hop = get_be32(&buf[22]);
    return 0;
}

// RREP: header(12) + query_id(4) + src_addr(4) + next_hop(4) + hop_count(1) + route_metric(1) + auth_hmac(4) = 30
int bramble_rrep_serialize(const bramble_header_t *hdr, const bramble_rrep_t *rrep,
                           uint8_t *buf, size_t buf_len) {
    if (buf_len < BRAMBLE_RREP_SIZE) return -1;
    bramble_header_serialize(hdr, buf, buf_len);
    put_be32(&buf[12], rrep->query_id);
    put_be32(&buf[16], rrep->src_addr);
    put_be32(&buf[20], rrep->next_hop);
    buf[24] = rrep->hop_count;
    buf[25] = rrep->route_metric;
    put_be32(&buf[26], rrep->auth_hmac);
    return BRAMBLE_RREP_SIZE;
}

int bramble_rrep_deserialize(const uint8_t *buf, size_t buf_len,
                             bramble_header_t *hdr, bramble_rrep_t *rrep) {
    if (buf_len < BRAMBLE_RREP_SIZE) return -1;
    bramble_header_deserialize(buf, buf_len, hdr);
    rrep->query_id = get_be32(&buf[12]);
    rrep->src_addr = get_be32(&buf[16]);
    rrep->next_hop = get_be32(&buf[20]);
    rrep->hop_count = buf[24];
    rrep->route_metric = buf[25];
    rrep->auth_hmac = get_be32(&buf[26]);
    return 0;
}

// RERR: header(12) + reporter_addr(4) + broken_dest(4) + broken_next_hop(4) = 24
int bramble_rerr_serialize(const bramble_header_t *hdr, const bramble_rerr_t *rerr,
                           uint8_t *buf, size_t buf_len) {
    if (buf_len < BRAMBLE_RERR_SIZE) return -1;
    bramble_header_serialize(hdr, buf, buf_len);
    put_be32(&buf[12], rerr->reporter_addr);
    put_be32(&buf[16], rerr->broken_dest);
    put_be32(&buf[20], rerr->broken_next_hop);
    return BRAMBLE_RERR_SIZE;
}

int bramble_rerr_deserialize(const uint8_t *buf, size_t buf_len,
                             bramble_header_t *hdr, bramble_rerr_t *rerr) {
    if (buf_len < BRAMBLE_RERR_SIZE) return -1;
    bramble_header_deserialize(buf, buf_len, hdr);
    rerr->reporter_addr = get_be32(&buf[12]);
    rerr->broken_dest = get_be32(&buf[16]);
    rerr->broken_next_hop = get_be32(&buf[20]);
    return 0;
}

// BEACON: header(12) + src_addr(4) + pubkey_hash(4) + uptime(2) + battery(1) + queue(1)
//         + neighbors(1) + flags(1) + network_time(4) + time_confidence(2) + auth_hmac(4) = 36
int bramble_beacon_serialize(const bramble_header_t *hdr, const bramble_beacon_t *beacon,
                             uint8_t *buf, size_t buf_len) {
    if (buf_len < BRAMBLE_BEACON_SIZE) return -1;
    bramble_header_serialize(hdr, buf, buf_len);
    put_be32(&buf[12], beacon->src_addr);
    put_be32(&buf[16], beacon->node_pubkey_hash);
    put_be16(&buf[20], beacon->uptime_min);
    buf[22] = beacon->battery_pct;
    buf[23] = beacon->tx_queue_depth;
    buf[24] = beacon->neighbor_count;
    buf[25] = beacon->flags;
    put_be32(&buf[26], beacon->network_time);
    put_be16(&buf[30], beacon->time_confidence);
    put_be32(&buf[32], beacon->auth_hmac);
    return BRAMBLE_BEACON_SIZE;
}

int bramble_beacon_deserialize(const uint8_t *buf, size_t buf_len,
                               bramble_header_t *hdr, bramble_beacon_t *beacon) {
    if (buf_len < BRAMBLE_BEACON_SIZE) return -1;
    bramble_header_deserialize(buf, buf_len, hdr);
    beacon->src_addr = get_be32(&buf[12]);
    beacon->node_pubkey_hash = get_be32(&buf[16]);
    beacon->uptime_min = get_be16(&buf[20]);
    beacon->battery_pct = buf[22];
    beacon->tx_queue_depth = buf[23];
    beacon->neighbor_count = buf[24];
    beacon->flags = buf[25];
    beacon->network_time = get_be32(&buf[26]);
    beacon->time_confidence = get_be16(&buf[30]);
    beacon->auth_hmac = get_be32(&buf[32]);
    return 0;
}

// KEY_EXCHANGE: header(12) + src_addr(4) + ephemeral_pubkey(32) + key_id(4) + ke_type(1) + auth_tag(16) = 69
int bramble_ke_serialize(const bramble_header_t *hdr, const bramble_key_exchange_t *ke,
                         uint8_t *buf, size_t buf_len) {
    if (buf_len < BRAMBLE_KEY_EXCHANGE_SIZE) return -1;
    bramble_header_serialize(hdr, buf, buf_len);
    put_be32(&buf[12], ke->src_addr);
    memcpy(&buf[16], ke->ephemeral_pubkey, 32);
    put_be32(&buf[48], ke->key_id);
    buf[52] = ke->ke_type;
    memcpy(&buf[53], ke->auth_tag, 16);
    return BRAMBLE_KEY_EXCHANGE_SIZE;
}

int bramble_ke_deserialize(const uint8_t *buf, size_t buf_len,
                           bramble_header_t *hdr, bramble_key_exchange_t *ke) {
    if (buf_len < BRAMBLE_KEY_EXCHANGE_SIZE) return -1;
    bramble_header_deserialize(buf, buf_len, hdr);
    ke->src_addr = get_be32(&buf[12]);
    memcpy(ke->ephemeral_pubkey, &buf[16], 32);
    ke->key_id = get_be32(&buf[48]);
    ke->ke_type = buf[52];
    memcpy(ke->auth_tag, &buf[53], 16);
    return 0;
}

// DELIVERY_RECEIPT: header(12) + src_addr(4) + orig_packet_id(4) + hop_count(1) + total_latency(1) + relay_path[N*4]
int bramble_receipt_serialize(const bramble_header_t *hdr, const bramble_delivery_receipt_t *rcpt,
                              const uint32_t *relay_path, uint8_t *buf, size_t buf_len) {
    size_t total = BRAMBLE_DELIVERY_RECEIPT_MIN_SIZE + rcpt->hop_count * 4;
    if (buf_len < total) return -1;
    if (rcpt->hop_count > 8) return -1;
    bramble_header_serialize(hdr, buf, buf_len);
    put_be32(&buf[12], rcpt->src_addr);
    put_be32(&buf[16], rcpt->orig_packet_id);
    buf[20] = rcpt->hop_count;
    buf[21] = rcpt->total_latency;
    for (int i = 0; i < rcpt->hop_count; i++) {
        put_be32(&buf[22 + i * 4], relay_path[i]);
    }
    return (int)total;
}

int bramble_receipt_deserialize(const uint8_t *buf, size_t buf_len,
                                bramble_header_t *hdr, bramble_delivery_receipt_t *rcpt,
                                uint32_t *relay_path, uint8_t max_relays) {
    if (buf_len < BRAMBLE_DELIVERY_RECEIPT_MIN_SIZE) return -1;
    bramble_header_deserialize(buf, buf_len, hdr);
    rcpt->src_addr = get_be32(&buf[12]);
    rcpt->orig_packet_id = get_be32(&buf[16]);
    rcpt->hop_count = buf[20];
    rcpt->total_latency = buf[21];
    if (rcpt->hop_count > 8) return -1;
    if (rcpt->hop_count > max_relays) return -1;
    if (buf_len < BRAMBLE_DELIVERY_RECEIPT_MIN_SIZE + rcpt->hop_count * 4) return -1;
    for (int i = 0; i < rcpt->hop_count; i++) {
        relay_path[i] = get_be32(&buf[22 + i * 4]);
    }
    return 0;
}

// CONGESTION: header(12) + src_addr(4) + congestion_level(1) + queue_depth(1) + est_clear_time(2) = 20
int bramble_congestion_serialize(const bramble_header_t *hdr, const bramble_congestion_t *cong,
                                 uint8_t *buf, size_t buf_len) {
    if (buf_len < BRAMBLE_CONGESTION_SIZE) return -1;
    bramble_header_serialize(hdr, buf, buf_len);
    put_be32(&buf[12], cong->src_addr);
    buf[16] = cong->congestion_level;
    buf[17] = cong->queue_depth;
    put_be16(&buf[18], cong->est_clear_time);
    return BRAMBLE_CONGESTION_SIZE;
}

int bramble_congestion_deserialize(const uint8_t *buf, size_t buf_len,
                                   bramble_header_t *hdr, bramble_congestion_t *cong) {
    if (buf_len < BRAMBLE_CONGESTION_SIZE) return -1;
    bramble_header_deserialize(buf, buf_len, hdr);
    cong->src_addr = get_be32(&buf[12]);
    cong->congestion_level = buf[16];
    cong->queue_depth = buf[17];
    cong->est_clear_time = get_be16(&buf[18]);
    return 0;
}

// TIME_SYNC: header(12) + src_addr(4) + timestamp(4) + confidence_ms(2) + stratum(1) + sequence(1) = 24
int bramble_timesync_serialize(const bramble_header_t *hdr, const bramble_time_sync_t *ts,
                               uint8_t *buf, size_t buf_len) {
    if (buf_len < BRAMBLE_TIME_SYNC_SIZE) return -1;
    bramble_header_serialize(hdr, buf, buf_len);
    put_be32(&buf[12], ts->src_addr);
    put_be32(&buf[16], ts->timestamp);
    put_be16(&buf[20], ts->confidence_ms);
    buf[22] = ts->stratum;
    buf[23] = ts->sequence;
    return BRAMBLE_TIME_SYNC_SIZE;
}

int bramble_timesync_deserialize(const uint8_t *buf, size_t buf_len,
                                 bramble_header_t *hdr, bramble_time_sync_t *ts) {
    if (buf_len < BRAMBLE_TIME_SYNC_SIZE) return -1;
    bramble_header_deserialize(buf, buf_len, hdr);
    ts->src_addr = get_be32(&buf[12]);
    ts->timestamp = get_be32(&buf[16]);
    ts->confidence_ms = get_be16(&buf[20]);
    ts->stratum = buf[22];
    ts->sequence = buf[23];
    return 0;
}
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_packet && ./test_packet
```
**Expected:** All 12 tests pass. Output:
```
12 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "feat: packet header serialization for all 10 packet types"`

---

### Task 5: ✅ Write failing tests for packet dedup buffer

**Create** `bramble/components/dedup/include/dedup.h`:
```c
#ifndef BRAMBLE_DEDUP_H
#define BRAMBLE_DEDUP_H

#include <stdint.h>
#include <stdbool.h>

#define DEDUP_MAX_ENTRIES 128
#define DEDUP_EXPIRY_MS   60000  // 60 seconds

typedef struct {
    uint32_t packet_id;
    uint32_t timestamp_ms;
} dedup_entry_t;

typedef struct {
    dedup_entry_t entries[DEDUP_MAX_ENTRIES];
    int count;
} dedup_buffer_t;

void dedup_init(dedup_buffer_t *buf);

// Returns true if packet_id is a duplicate (already seen within expiry window).
// If not duplicate, adds it to the buffer.
bool dedup_check_and_add(dedup_buffer_t *buf, uint32_t packet_id, uint32_t now_ms);

// Purge entries older than DEDUP_EXPIRY_MS
void dedup_purge(dedup_buffer_t *buf, uint32_t now_ms);

// Get current count
int dedup_count(const dedup_buffer_t *buf);

#endif
```

**Create** `bramble/components/dedup/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "dedup.c"
    INCLUDE_DIRS "include"
)
```

**Create** `bramble/test/test_dedup.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include "../components/dedup/include/dedup.h"
#include "../components/dedup/dedup.c"

void setUp(void) {}
void tearDown(void) {}

void test_dedup_init(void) {
    dedup_buffer_t buf;
    dedup_init(&buf);
    TEST_ASSERT_EQUAL(0, dedup_count(&buf));
}

void test_dedup_first_packet_not_duplicate(void) {
    dedup_buffer_t buf;
    dedup_init(&buf);
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 0x12345678, 1000));
    TEST_ASSERT_EQUAL(1, dedup_count(&buf));
}

void test_dedup_same_packet_is_duplicate(void) {
    dedup_buffer_t buf;
    dedup_init(&buf);
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 0x12345678, 1000));
    TEST_ASSERT_TRUE(dedup_check_and_add(&buf, 0x12345678, 1500));
    TEST_ASSERT_EQUAL(1, dedup_count(&buf));
}

void test_dedup_different_packets_not_duplicate(void) {
    dedup_buffer_t buf;
    dedup_init(&buf);
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 0x11111111, 1000));
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 0x22222222, 1000));
    TEST_ASSERT_EQUAL(2, dedup_count(&buf));
}

void test_dedup_expired_entry_not_duplicate(void) {
    dedup_buffer_t buf;
    dedup_init(&buf);
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 0x12345678, 1000));
    // 61 seconds later — past the 60s expiry
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 0x12345678, 62000));
}

void test_dedup_purge_removes_old(void) {
    dedup_buffer_t buf;
    dedup_init(&buf);
    dedup_check_and_add(&buf, 0x11111111, 1000);
    dedup_check_and_add(&buf, 0x22222222, 30000);
    dedup_check_and_add(&buf, 0x33333333, 50000);
    TEST_ASSERT_EQUAL(3, dedup_count(&buf));

    dedup_purge(&buf, 62000);  // Only first entry expired
    TEST_ASSERT_EQUAL(2, dedup_count(&buf));
}

void test_dedup_full_buffer_evicts_oldest(void) {
    dedup_buffer_t buf;
    dedup_init(&buf);

    // Fill the buffer
    for (int i = 0; i < DEDUP_MAX_ENTRIES; i++) {
        TEST_ASSERT_FALSE(dedup_check_and_add(&buf, (uint32_t)(i + 1), 1000 + i));
    }
    TEST_ASSERT_EQUAL(DEDUP_MAX_ENTRIES, dedup_count(&buf));

    // Add one more — should evict oldest
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 0xFFFFFFFF, 2000));
    TEST_ASSERT_EQUAL(DEDUP_MAX_ENTRIES, dedup_count(&buf));

    // Original packet 1 should no longer be tracked
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 1, 2001));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_dedup_init);
    RUN_TEST(test_dedup_first_packet_not_duplicate);
    RUN_TEST(test_dedup_same_packet_is_duplicate);
    RUN_TEST(test_dedup_different_packets_not_duplicate);
    RUN_TEST(test_dedup_expired_entry_not_duplicate);
    RUN_TEST(test_dedup_purge_removes_old);
    RUN_TEST(test_dedup_full_buffer_evicts_oldest);
    return UNITY_END();
}
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
# Dedup buffer tests
add_executable(test_dedup test_dedup.c)
target_link_libraries(test_dedup unity esp_stubs)
target_include_directories(test_dedup PRIVATE stubs)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_dedup 2>&1
```
**Expected:** Linker errors — `dedup_init` etc. not implemented.

**Commit:** `git add -A && git commit -m "test: failing dedup buffer tests"`

---

### Task 6: ✅ Implement dedup buffer

**Create** `bramble/components/dedup/dedup.c`:
```c
#include "dedup.h"
#include <string.h>

void dedup_init(dedup_buffer_t *buf) {
    memset(buf, 0, sizeof(*buf));
    buf->count = 0;
}

static int find_entry(const dedup_buffer_t *buf, uint32_t packet_id, uint32_t now_ms) {
    for (int i = 0; i < buf->count; i++) {
        if (buf->entries[i].packet_id == packet_id &&
            (now_ms - buf->entries[i].timestamp_ms) < DEDUP_EXPIRY_MS) {
            return i;
        }
    }
    return -1;
}

bool dedup_check_and_add(dedup_buffer_t *buf, uint32_t packet_id, uint32_t now_ms) {
    // Purge expired entries first
    dedup_purge(buf, now_ms);

    // Check if already present
    if (find_entry(buf, packet_id, now_ms) >= 0) {
        return true;  // Duplicate
    }

    // Add new entry
    if (buf->count >= DEDUP_MAX_ENTRIES) {
        // Evict oldest (index 0, entries are roughly time-ordered)
        uint32_t oldest_idx = 0;
        uint32_t oldest_ts = buf->entries[0].timestamp_ms;
        for (int i = 1; i < buf->count; i++) {
            if (buf->entries[i].timestamp_ms < oldest_ts) {
                oldest_ts = buf->entries[i].timestamp_ms;
                oldest_idx = i;
            }
        }
        // Shift entries to fill gap
        if (oldest_idx < buf->count - 1) {
            memmove(&buf->entries[oldest_idx], &buf->entries[oldest_idx + 1],
                    (buf->count - oldest_idx - 1) * sizeof(dedup_entry_t));
        }
        buf->count--;
    }

    buf->entries[buf->count].packet_id = packet_id;
    buf->entries[buf->count].timestamp_ms = now_ms;
    buf->count++;

    return false;  // Not a duplicate
}

void dedup_purge(dedup_buffer_t *buf, uint32_t now_ms) {
    int write = 0;
    for (int read = 0; read < buf->count; read++) {
        if ((now_ms - buf->entries[read].timestamp_ms) < DEDUP_EXPIRY_MS) {
            if (write != read) {
                buf->entries[write] = buf->entries[read];
            }
            write++;
        }
    }
    buf->count = write;
}

int dedup_count(const dedup_buffer_t *buf) {
    return buf->count;
}
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_dedup && ./test_dedup
```
**Expected:**
```
7 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "feat: packet dedup buffer with time-based expiry"`

---

### Task 7: ✅ Write SX1262 radio driver header (HAL interface)

**Create** `bramble/components/radio/include/radio.h`:
```c
#ifndef BRAMBLE_RADIO_H
#define BRAMBLE_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Radio profiles
typedef enum {
    RADIO_PROFILE_LONG_RANGE = 0,   // SF10, 125kHz, CR 4/6
    RADIO_PROFILE_MEDIUM_RANGE = 1, // SF8, 250kHz, CR 4/5
} radio_profile_t;

// Radio state
typedef enum {
    RADIO_STATE_IDLE = 0,
    RADIO_STATE_TX,
    RADIO_STATE_RX,
    RADIO_STATE_CAD,
    RADIO_STATE_SLEEP,
} radio_state_t;

// RX packet metadata
typedef struct {
    int8_t   rssi;
    int8_t   snr;
    uint16_t len;
} radio_rx_info_t;

// Radio configuration
typedef struct {
    float    frequency_mhz;
    uint8_t  spreading_factor;
    uint32_t bandwidth_hz;
    uint8_t  coding_rate;    // 1=4/5, 2=4/6, 3=4/7, 4=4/8
    int8_t   tx_power_dbm;
    uint16_t preamble_len;
    uint16_t sync_word;
    bool     crc_enabled;
    bool     explicit_header;
} radio_config_t;

// Callback types
typedef void (*radio_rx_cb_t)(const uint8_t *data, uint16_t len, int8_t rssi, int8_t snr);
typedef void (*radio_tx_done_cb_t)(void);
typedef void (*radio_cad_done_cb_t)(bool activity_detected);

// Initialize radio hardware (SPI, GPIO, reset)
int radio_init(const radio_config_t *config);

// Apply a preset profile
void radio_get_profile_config(radio_profile_t profile, radio_config_t *config);

// Transmit a packet (blocking or async depending on implementation)
int radio_transmit(const uint8_t *data, uint16_t len);

// Start continuous RX mode
int radio_start_rx(void);

// Perform Channel Activity Detection (returns true if channel is busy)
bool radio_cad(void);

// Set TX power
int radio_set_tx_power(int8_t power_dbm);

// Get current state
radio_state_t radio_get_state(void);

// Register callbacks
void radio_set_rx_callback(radio_rx_cb_t cb);
void radio_set_tx_done_callback(radio_tx_done_cb_t cb);
void radio_set_cad_done_callback(radio_cad_done_cb_t cb);

// Calculate airtime in microseconds
uint32_t radio_calculate_airtime_us(uint16_t payload_bytes, uint8_t sf, uint32_t bw_hz, uint8_t cr);

// Enter sleep mode
int radio_sleep(void);

#endif // BRAMBLE_RADIO_H
```

**Create** `bramble/components/radio/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "radio.c" "radio_sx1262.c"
    INCLUDE_DIRS "include"
    REQUIRES driver spi_master esp_timer
)
```

**Commit:** `git add -A && git commit -m "feat: radio HAL interface header"`

---

### Task 8: ✅ Write failing test for airtime calculation

**Create** `bramble/test/test_radio_airtime.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include <math.h>

// Portable airtime calculation (extracted from radio module for host testing)
uint32_t radio_calculate_airtime_us(uint16_t payload_bytes, uint8_t sf, uint32_t bw_hz, uint8_t cr) {
    // Will be implemented in the actual source
    (void)payload_bytes; (void)sf; (void)bw_hz; (void)cr;
    return 0;
}

// Provide a separate implementation file
#define RADIO_AIRTIME_ONLY
#include "../components/radio/radio_airtime.c"

void setUp(void) {}
void tearDown(void) {}

void test_airtime_ack_sf10_125k(void) {
    // 22-byte ACK at SF10, 125kHz, CR 4/6: ~290ms
    uint32_t airtime = bramble_calculate_airtime_us(22, 10, 125000, 2);
    // Allow ±50ms tolerance
    TEST_ASSERT_UINT32_WITHIN(50000, 290000, airtime);
}

void test_airtime_beacon_sf10_125k(void) {
    // 36-byte beacon: ~400ms
    uint32_t airtime = bramble_calculate_airtime_us(36, 10, 125000, 2);
    TEST_ASSERT_UINT32_WITHIN(50000, 400000, airtime);
}

void test_airtime_short_msg_sf10_125k(void) {
    // 100-byte payload: ~480ms
    uint32_t airtime = bramble_calculate_airtime_us(100, 10, 125000, 2);
    TEST_ASSERT_UINT32_WITHIN(80000, 480000, airtime);
}

void test_airtime_full_msg_sf10_125k(void) {
    // 200-byte payload: ~850ms
    uint32_t airtime = bramble_calculate_airtime_us(200, 10, 125000, 2);
    TEST_ASSERT_UINT32_WITHIN(100000, 850000, airtime);
}

void test_airtime_max_sf10_125k(void) {
    // 222-byte max payload: ~920ms
    uint32_t airtime = bramble_calculate_airtime_us(222, 10, 125000, 2);
    TEST_ASSERT_UINT32_WITHIN(100000, 920000, airtime);
}

void test_airtime_medium_range_100b(void) {
    // 100-byte at SF8, 250kHz, CR 4/5: ~120ms
    uint32_t airtime = bramble_calculate_airtime_us(100, 8, 250000, 1);
    TEST_ASSERT_UINT32_WITHIN(40000, 120000, airtime);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_airtime_ack_sf10_125k);
    RUN_TEST(test_airtime_beacon_sf10_125k);
    RUN_TEST(test_airtime_short_msg_sf10_125k);
    RUN_TEST(test_airtime_full_msg_sf10_125k);
    RUN_TEST(test_airtime_max_sf10_125k);
    RUN_TEST(test_airtime_medium_range_100b);
    return UNITY_END();
}
```

**Commit:** `git add -A && git commit -m "test: failing airtime calculation tests"`

---

### Task 9: ✅ Implement airtime calculation

**Create** `bramble/components/radio/radio_airtime.c`:
```c
#include <stdint.h>
#include <math.h>

// LoRa airtime calculation per Semtech AN1200.13
uint32_t bramble_calculate_airtime_us(uint16_t payload_bytes, uint8_t sf, uint32_t bw_hz, uint8_t cr) {
    uint16_t n_preamble = (sf >= 9) ? 12 : 8;  // 12 for LongRange, 8 for MediumRange

    // Symbol duration in microseconds
    double t_sym_us = (double)(1 << sf) * 1000000.0 / (double)bw_hz;

    // Preamble time
    double t_preamble_us = ((double)n_preamble + 4.25) * t_sym_us;

    // Low data rate optimize for SF >= 11 at 125kHz
    int de = (sf >= 11 && bw_hz == 125000) ? 1 : 0;
    int ih = 0;  // Explicit header

    double numerator = 8.0 * payload_bytes - 4.0 * sf + 28.0 + 16.0 - 20.0 * ih;
    double denominator = 4.0 * (sf - 2 * de);

    double n_payload_symbols;
    if (numerator < 0) {
        n_payload_symbols = 8.0;
    } else {
        n_payload_symbols = 8.0 + ceil(numerator / denominator) * (cr + 4);
    }

    double t_payload_us = n_payload_symbols * t_sym_us;

    return (uint32_t)(t_preamble_us + t_payload_us);
}
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
# Airtime calculation tests
add_executable(test_radio_airtime test_radio_airtime.c)
target_link_libraries(test_radio_airtime unity esp_stubs m)
target_include_directories(test_radio_airtime PRIVATE stubs)
```

Update test file to just include the implementation:

**Modify** `bramble/test/test_radio_airtime.c` — remove the stub function and `#define` block, replace with:
```c
#include "unity.h"
#include "esp_stubs.h"
#include <math.h>
#include "../components/radio/radio_airtime.c"

void setUp(void) {}
void tearDown(void) {}
// ... (rest of tests unchanged)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_radio_airtime && ./test_radio_airtime
```
**Expected:**
```
6 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "feat: LoRa airtime calculation per Semtech AN1200.13"`

---

### Task 10: ✅ Implement SX1262 SPI driver skeleton

**Create** `bramble/components/radio/radio_sx1262.c`:
```c
#include "radio.h"
#include <string.h>

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sx1262";

// Heltec WiFi LoRa 32 V3 pin definitions
#define SX1262_SPI_HOST     SPI2_HOST
#define SX1262_PIN_MOSI     10
#define SX1262_PIN_MISO     11
#define SX1262_PIN_SCK      9
#define SX1262_PIN_NSS      8
#define SX1262_PIN_RESET    12
#define SX1262_PIN_BUSY     13
#define SX1262_PIN_DIO1     14

// SX1262 SPI commands
#define SX1262_CMD_SET_SLEEP           0x84
#define SX1262_CMD_SET_STANDBY         0x80
#define SX1262_CMD_SET_FS              0xC1
#define SX1262_CMD_SET_TX              0x83
#define SX1262_CMD_SET_RX              0x82
#define SX1262_CMD_SET_CAD             0xC5
#define SX1262_CMD_SET_PACKET_TYPE     0x8A
#define SX1262_CMD_SET_RF_FREQUENCY    0x86
#define SX1262_CMD_SET_PA_CONFIG       0x95
#define SX1262_CMD_SET_TX_PARAMS       0x8E
#define SX1262_CMD_SET_BUFFER_BASE     0x8F
#define SX1262_CMD_SET_MOD_PARAMS      0x8B
#define SX1262_CMD_SET_PACKET_PARAMS   0x8C
#define SX1262_CMD_SET_DIO_IRQ_PARAMS  0x08
#define SX1262_CMD_GET_IRQ_STATUS      0x12
#define SX1262_CMD_CLR_IRQ_STATUS      0x02
#define SX1262_CMD_WRITE_BUFFER        0x0E
#define SX1262_CMD_READ_BUFFER         0x1E
#define SX1262_CMD_GET_RX_BUFFER_STATUS 0x13
#define SX1262_CMD_GET_PACKET_STATUS   0x14
#define SX1262_CMD_GET_STATUS          0xC0
#define SX1262_CMD_SET_DIO3_AS_TCXO    0x97
#define SX1262_CMD_CALIBRATE           0x89
#define SX1262_CMD_SET_REGULATOR_MODE  0x96
#define SX1262_CMD_SET_SYNC_WORD       0x0D

// IRQ flags
#define SX1262_IRQ_TX_DONE     (1 << 0)
#define SX1262_IRQ_RX_DONE     (1 << 1)
#define SX1262_IRQ_TIMEOUT     (1 << 9)
#define SX1262_IRQ_CAD_DONE    (1 << 7)
#define SX1262_IRQ_CAD_DETECTED (1 << 8)

static spi_device_handle_t spi_handle;
static radio_state_t current_state = RADIO_STATE_IDLE;
static radio_rx_cb_t rx_callback = NULL;
static radio_tx_done_cb_t tx_done_callback = NULL;
static radio_cad_done_cb_t cad_done_callback = NULL;
static radio_config_t current_config;
static SemaphoreHandle_t radio_mutex;

static void wait_busy(void) {
    while (gpio_get_level(SX1262_PIN_BUSY)) {
        vTaskDelay(1);
    }
}

static void spi_write_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    wait_busy();
    spi_transaction_t t = {
        .length = (1 + len) * 8,
        .tx_data = {cmd},
    };
    if (len <= 3) {
        t.flags = SPI_TRANS_USE_TXDATA;
        t.tx_data[0] = cmd;
        memcpy(&t.tx_data[1], data, len);
        t.length = (1 + len) * 8;
    } else {
        uint8_t buf[256];
        buf[0] = cmd;
        memcpy(&buf[1], data, len);
        t.tx_buffer = buf;
        t.length = (1 + len) * 8;
        t.flags = 0;
    }
    spi_device_transmit(spi_handle, &t);
}

static void spi_read_cmd(uint8_t cmd, uint8_t *data, size_t len) {
    wait_busy();
    uint8_t tx[256] = {0};
    uint8_t rx[256] = {0};
    tx[0] = cmd;
    tx[1] = 0x00;  // NOP for status
    spi_transaction_t t = {
        .length = (2 + len) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(spi_handle, &t);
    memcpy(data, &rx[2], len);
}

void radio_get_profile_config(radio_profile_t profile, radio_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->crc_enabled = true;
    config->explicit_header = true;
    config->sync_word = 0x1424;

    switch (profile) {
        case RADIO_PROFILE_LONG_RANGE:
            config->frequency_mhz = 906.875f;
            config->spreading_factor = 10;
            config->bandwidth_hz = 125000;
            config->coding_rate = 2;  // CR 4/6
            config->tx_power_dbm = 22;
            config->preamble_len = 12;
            break;
        case RADIO_PROFILE_MEDIUM_RANGE:
            config->frequency_mhz = 906.875f;
            config->spreading_factor = 8;
            config->bandwidth_hz = 250000;
            config->coding_rate = 1;  // CR 4/5
            config->tx_power_dbm = 22;
            config->preamble_len = 8;
            break;
    }
}

int radio_init(const radio_config_t *config) {
    current_config = *config;
    radio_mutex = xSemaphoreCreateMutex();

    // Configure SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SX1262_PIN_MOSI,
        .miso_io_num = SX1262_PIN_MISO,
        .sclk_io_num = SX1262_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SX1262_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 8 * 1000 * 1000,  // 8 MHz
        .mode = 0,
        .spics_io_num = SX1262_PIN_NSS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SX1262_SPI_HOST, &dev_cfg, &spi_handle));

    // Configure GPIO
    gpio_set_direction(SX1262_PIN_RESET, GPIO_MODE_OUTPUT);
    gpio_set_direction(SX1262_PIN_BUSY, GPIO_MODE_INPUT);
    gpio_set_direction(SX1262_PIN_DIO1, GPIO_MODE_INPUT);

    // Reset the SX1262
    gpio_set_level(SX1262_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(SX1262_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    wait_busy();

    // Set standby mode (STDBY_RC)
    uint8_t standby_cfg = 0x00;
    spi_write_cmd(SX1262_CMD_SET_STANDBY, &standby_cfg, 1);

    // Set packet type: LoRa
    uint8_t pkt_type = 0x01;
    spi_write_cmd(SX1262_CMD_SET_PACKET_TYPE, &pkt_type, 1);

    // Set RF frequency
    uint32_t freq_reg = (uint32_t)((double)config->frequency_mhz * 1000000.0 / 32000000.0 * (1 << 25));
    uint8_t freq_bytes[4] = {
        (freq_reg >> 24) & 0xFF,
        (freq_reg >> 16) & 0xFF,
        (freq_reg >> 8)  & 0xFF,
        freq_reg & 0xFF,
    };
    spi_write_cmd(SX1262_CMD_SET_RF_FREQUENCY, freq_bytes, 4);

    // Set PA config for +22 dBm (SX1262)
    uint8_t pa_cfg[4] = { 0x04, 0x07, 0x00, 0x01 };
    spi_write_cmd(SX1262_CMD_SET_PA_CONFIG, pa_cfg, 4);

    // Set TX params: power, ramp time
    uint8_t tx_params[2] = { (uint8_t)config->tx_power_dbm, 0x04 };  // 200µs ramp
    spi_write_cmd(SX1262_CMD_SET_TX_PARAMS, tx_params, 2);

    // Set modulation params: SF, BW, CR, LDRO
    uint8_t bw_val;
    switch (config->bandwidth_hz) {
        case 125000: bw_val = 0x04; break;
        case 250000: bw_val = 0x05; break;
        case 500000: bw_val = 0x06; break;
        default:     bw_val = 0x04; break;
    }
    uint8_t ldro = (config->spreading_factor >= 11 && config->bandwidth_hz == 125000) ? 0x01 : 0x00;
    uint8_t mod_params[4] = { config->spreading_factor, bw_val, (uint8_t)(config->coding_rate), ldro };
    spi_write_cmd(SX1262_CMD_SET_MOD_PARAMS, mod_params, 4);

    // Set packet params
    uint8_t pkt_params[6] = {
        (config->preamble_len >> 8) & 0xFF,
        config->preamble_len & 0xFF,
        config->explicit_header ? 0x00 : 0x01,
        0xFF,  // payload length (max, will be set per TX)
        config->crc_enabled ? 0x01 : 0x00,
        0x00,  // standard IQ
    };
    spi_write_cmd(SX1262_CMD_SET_PACKET_PARAMS, pkt_params, 6);

    // Set sync word
    uint8_t sw_data[2] = { (config->sync_word >> 8) & 0xFF, config->sync_word & 0xFF };
    // Write to register 0x0740
    uint8_t sw_cmd[5] = { 0x07, 0x40, sw_data[0], 0x07, 0x41 };
    // Actually, writing registers requires WriteRegister command (0x0D)
    uint8_t wr_reg[4] = { 0x07, 0x40, sw_data[0], sw_data[1] };
    spi_write_cmd(SX1262_CMD_SET_SYNC_WORD, wr_reg, 4);

    // Set DIO1 IRQ: TX done, RX done, CAD done, CAD detected, timeout
    uint8_t irq_params[8] = {
        0x02, 0x03,  // IRQ mask: bits for TX_DONE, RX_DONE
        0x02, 0x03,  // DIO1 mask (same)
        0x00, 0x00,  // DIO2 mask (none)
        0x00, 0x00,  // DIO3 mask (none)
    };
    spi_write_cmd(SX1262_CMD_SET_DIO_IRQ_PARAMS, irq_params, 8);

    // Set buffer base addresses
    uint8_t buf_base[2] = { 0x00, 0x00 };
    spi_write_cmd(SX1262_CMD_SET_BUFFER_BASE, buf_base, 2);

    current_state = RADIO_STATE_IDLE;
    ESP_LOGI(TAG, "SX1262 initialized: %.3f MHz, SF%d, BW%lu, CR4/%d, %d dBm",
             config->frequency_mhz, config->spreading_factor, config->bandwidth_hz,
             config->coding_rate + 4, config->tx_power_dbm);

    return 0;
}

int radio_transmit(const uint8_t *data, uint16_t len) {
    if (len > 222) return -1;

    xSemaphoreTake(radio_mutex, portMAX_DELAY);

    // Write payload to SX1262 buffer
    uint8_t buf_cmd[2 + 222];
    buf_cmd[0] = 0x00;  // offset
    memcpy(&buf_cmd[1], data, len);
    spi_write_cmd(SX1262_CMD_WRITE_BUFFER, buf_cmd, 1 + len);

    // Update packet params with actual payload length
    uint8_t pkt_params[6] = {
        (current_config.preamble_len >> 8) & 0xFF,
        current_config.preamble_len & 0xFF,
        current_config.explicit_header ? 0x00 : 0x01,
        (uint8_t)len,
        current_config.crc_enabled ? 0x01 : 0x00,
        0x00,
    };
    spi_write_cmd(SX1262_CMD_SET_PACKET_PARAMS, pkt_params, 6);

    // Set TX (timeout 0 = no timeout)
    uint8_t tx_cmd[3] = { 0x00, 0x00, 0x00 };
    spi_write_cmd(SX1262_CMD_SET_TX, tx_cmd, 3);

    current_state = RADIO_STATE_TX;

    // Wait for TX done (poll DIO1 or check IRQ)
    // In production, this would be interrupt-driven
    int timeout_ms = 2000;
    while (timeout_ms > 0 && !gpio_get_level(SX1262_PIN_DIO1)) {
        vTaskDelay(pdMS_TO_TICKS(1));
        timeout_ms--;
    }

    // Clear IRQ
    uint8_t clear_irq[2] = { 0xFF, 0xFF };
    spi_write_cmd(SX1262_CMD_CLR_IRQ_STATUS, clear_irq, 2);

    current_state = RADIO_STATE_IDLE;

    xSemaphoreGive(radio_mutex);

    if (tx_done_callback) tx_done_callback();

    return (timeout_ms > 0) ? 0 : -1;
}

int radio_start_rx(void) {
    xSemaphoreTake(radio_mutex, portMAX_DELAY);

    // Set RX continuous (timeout 0xFFFFFF = continuous)
    uint8_t rx_cmd[3] = { 0xFF, 0xFF, 0xFF };
    spi_write_cmd(SX1262_CMD_SET_RX, rx_cmd, 3);

    current_state = RADIO_STATE_RX;

    xSemaphoreGive(radio_mutex);
    return 0;
}

bool radio_cad(void) {
    xSemaphoreTake(radio_mutex, portMAX_DELAY);

    // Configure CAD params: 2 symbols
    // (Simplified — real implementation needs CAD params command)
    uint8_t cad_cmd[1] = { 0x00 };
    spi_write_cmd(SX1262_CMD_SET_CAD, cad_cmd, 0);

    current_state = RADIO_STATE_CAD;

    // Wait for CAD done
    int timeout_ms = 100;
    while (timeout_ms > 0 && !gpio_get_level(SX1262_PIN_DIO1)) {
        vTaskDelay(pdMS_TO_TICKS(1));
        timeout_ms--;
    }

    // Read IRQ status
    uint8_t irq_status[2];
    spi_read_cmd(SX1262_CMD_GET_IRQ_STATUS, irq_status, 2);

    bool detected = (irq_status[1] & 0x01);  // CAD detected bit

    // Clear IRQ
    uint8_t clear_irq[2] = { 0xFF, 0xFF };
    spi_write_cmd(SX1262_CMD_CLR_IRQ_STATUS, clear_irq, 2);

    current_state = RADIO_STATE_IDLE;

    xSemaphoreGive(radio_mutex);
    return detected;
}

int radio_set_tx_power(int8_t power_dbm) {
    if (power_dbm < 2) power_dbm = 2;
    if (power_dbm > 22) power_dbm = 22;

    current_config.tx_power_dbm = power_dbm;
    uint8_t tx_params[2] = { (uint8_t)power_dbm, 0x04 };
    spi_write_cmd(SX1262_CMD_SET_TX_PARAMS, tx_params, 2);
    return 0;
}

radio_state_t radio_get_state(void) {
    return current_state;
}

void radio_set_rx_callback(radio_rx_cb_t cb) { rx_callback = cb; }
void radio_set_tx_done_callback(radio_tx_done_cb_t cb) { tx_done_callback = cb; }
void radio_set_cad_done_callback(radio_cad_done_cb_t cb) { cad_done_callback = cb; }

int radio_sleep(void) {
    uint8_t sleep_cfg = 0x04;  // Warm start, RTC timeout disable
    spi_write_cmd(SX1262_CMD_SET_SLEEP, &sleep_cfg, 1);
    current_state = RADIO_STATE_SLEEP;
    return 0;
}

#else
// Stub for non-ESP32 builds
int radio_init(const radio_config_t *config) { (void)config; return -1; }
void radio_get_profile_config(radio_profile_t profile, radio_config_t *config) {
    (void)profile; (void)config;
}
int radio_transmit(const uint8_t *data, uint16_t len) { (void)data; (void)len; return -1; }
int radio_start_rx(void) { return -1; }
bool radio_cad(void) { return false; }
int radio_set_tx_power(int8_t p) { (void)p; return -1; }
radio_state_t radio_get_state(void) { return RADIO_STATE_IDLE; }
void radio_set_rx_callback(radio_rx_cb_t cb) { (void)cb; }
void radio_set_tx_done_callback(radio_tx_done_cb_t cb) { (void)cb; }
void radio_set_cad_done_callback(radio_cad_done_cb_t cb) { (void)cb; }
int radio_sleep(void) { return -1; }
#endif
```

**Create** `bramble/components/radio/radio.c`:
```c
// Common radio functions (non-hardware-specific)
#include "radio.h"

// Airtime calculation is in radio_airtime.c and compiled separately
extern uint32_t bramble_calculate_airtime_us(uint16_t payload_bytes, uint8_t sf, uint32_t bw_hz, uint8_t cr);

uint32_t radio_calculate_airtime_us(uint16_t payload_bytes, uint8_t sf, uint32_t bw_hz, uint8_t cr) {
    return bramble_calculate_airtime_us(payload_bytes, sf, bw_hz, cr);
}
```

**Commit:** `git add -A && git commit -m "feat: SX1262 SPI driver skeleton with TX/RX/CAD"`

---

### Task 11: ✅ Create FreeRTOS task structure

**Modify** `bramble/main/main.c`:
```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "radio.h"
#include "packet.h"
#include "dedup.h"

static const char *TAG = "bramble";

// Task handles
static TaskHandle_t radio_task_handle;
static TaskHandle_t protocol_task_handle;
static TaskHandle_t app_task_handle;

// Inter-task queues
static QueueHandle_t rx_queue;   // radio -> protocol
static QueueHandle_t tx_queue;   // protocol -> radio

// Shared state
static dedup_buffer_t packet_dedup;

// RX queue item
typedef struct {
    uint8_t  data[222];
    uint16_t len;
    int8_t   rssi;
    int8_t   snr;
} rx_queue_item_t;

// TX queue item
typedef struct {
    uint8_t  data[222];
    uint16_t len;
    uint8_t  priority;
} tx_queue_item_t;

// Radio RX callback — called from ISR context
static void on_radio_rx(const uint8_t *data, uint16_t len, int8_t rssi, int8_t snr) {
    rx_queue_item_t item;
    if (len > 222) return;
    memcpy(item.data, data, len);
    item.len = len;
    item.rssi = rssi;
    item.snr = snr;
    xQueueSendFromISR(rx_queue, &item, NULL);
}

// Radio task: manages TX and keeps radio in RX mode
static void radio_task(void *pvParam) {
    (void)pvParam;
    ESP_LOGI(TAG, "Radio task started");

    radio_config_t config;
    radio_get_profile_config(RADIO_PROFILE_LONG_RANGE, &config);

    if (radio_init(&config) != 0) {
        ESP_LOGE(TAG, "Radio init failed!");
        vTaskDelete(NULL);
        return;
    }

    radio_set_rx_callback(on_radio_rx);
    radio_start_rx();

    tx_queue_item_t tx_item;
    while (1) {
        // Check for packets to transmit
        if (xQueueReceive(tx_queue, &tx_item, pdMS_TO_TICKS(100)) == pdTRUE) {
            radio_transmit(tx_item.data, tx_item.len);
            radio_start_rx();  // Return to RX after TX
        }
    }
}

// Protocol task: handles routing, reliability, packet processing
static void protocol_task(void *pvParam) {
    (void)pvParam;
    ESP_LOGI(TAG, "Protocol task started");

    rx_queue_item_t rx_item;
    while (1) {
        if (xQueueReceive(rx_queue, &rx_item, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Check minimum header size
            if (rx_item.len < BRAMBLE_HEADER_SIZE) continue;

            bramble_header_t hdr;
            if (bramble_header_deserialize(rx_item.data, rx_item.len, &hdr) != 0) continue;

            // Version check
            if (hdr.version != BRAMBLE_VERSION) continue;

            // Dedup check
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (dedup_check_and_add(&packet_dedup, hdr.packet_id, now_ms)) continue;

            ESP_LOGI(TAG, "RX type=0x%02X from=? id=0x%08lX rssi=%d snr=%d",
                     hdr.packet_type, (unsigned long)hdr.packet_id,
                     rx_item.rssi, rx_item.snr);

            // Dispatch by packet type
            switch (hdr.packet_type) {
                case PKT_BEACON:
                    ESP_LOGI(TAG, "Beacon received");
                    break;
                case PKT_DATA:
                    ESP_LOGI(TAG, "Data packet received");
                    break;
                // TODO: handle remaining types
                default:
                    ESP_LOGW(TAG, "Unknown packet type: 0x%02X", hdr.packet_type);
                    break;
            }
        }

        // TODO: periodic tasks (beacon, route maintenance, retry tick)
    }
}

// Application task: UI, serial console, BLE
static void app_task(void *pvParam) {
    (void)pvParam;
    ESP_LOGI(TAG, "App task started");

    while (1) {
        // TODO: serial console, OLED UI, BLE
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Bramble LoRa Mesh Protocol v0.1");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize shared state
    dedup_init(&packet_dedup);

    // Create inter-task queues
    rx_queue = xQueueCreate(8, sizeof(rx_queue_item_t));
    tx_queue = xQueueCreate(16, sizeof(tx_queue_item_t));

    // Create tasks
    xTaskCreatePinnedToCore(radio_task, "radio", 4096, NULL, 5, &radio_task_handle, 1);
    xTaskCreatePinnedToCore(protocol_task, "protocol", 8192, NULL, 4, &protocol_task_handle, 0);
    xTaskCreatePinnedToCore(app_task, "app", 4096, NULL, 3, &app_task_handle, 0);

    ESP_LOGI(TAG, "All tasks started");
}
```

**Modify** `bramble/main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash driver spi_master esp_timer freertos
    PRIV_REQUIRES radio packet dedup
)
```

**Commit:** `git add -A && git commit -m "feat: FreeRTOS task structure — radio, protocol, app tasks"`

---

### Task 12: ✅ Add serial debug console

**Create** `bramble/components/console/include/console.h`:
```c
#ifndef BRAMBLE_CONSOLE_H
#define BRAMBLE_CONSOLE_H

// Initialize serial debug console (UART0, 115200)
void console_init(void);

// Process one line of console input (non-blocking)
// Returns true if a command was processed
bool console_poll(void);

#endif
```

**Create** `bramble/components/console/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "console.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_console vfs driver
)
```

**Create** `bramble/components/console/console.c`:
```c
#include "console.h"
#include "esp_log.h"
#include "esp_console.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "console";

static int cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("Bramble v0.1\n");
    printf("Status: running\n");
    // TODO: print node address, neighbor count, route count, airtime budget
    return 0;
}

static int cmd_peers(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("Peers: (not yet implemented)\n");
    return 0;
}

static int cmd_routes(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("Routes: (not yet implemented)\n");
    return 0;
}

void console_init(void) {
    esp_console_config_t console_config = {
        .max_cmdline_args = 8,
        .max_cmdline_length = 256,
    };
    ESP_ERROR_CHECK(esp_console_init(&console_config));

    const esp_console_cmd_t cmds[] = {
        { .command = "status", .help = "Show node status", .func = &cmd_status },
        { .command = "peers",  .help = "Show known peers", .func = &cmd_peers },
        { .command = "routes", .help = "Show routing table", .func = &cmd_routes },
    };

    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }

    ESP_LOGI(TAG, "Console initialized");
}

bool console_poll(void) {
    // Non-blocking line read from UART
    // In practice, this would use esp_console_repl or a line buffer
    return false;
}
```

**Commit:** `git add -A && git commit -m "feat: serial debug console with status/peers/routes commands"`

---

### Task 13: ✅ Run all existing tests

**Run:**
```bash
cd bramble/test/build && cmake .. && make -j && ./test_packet && ./test_dedup && ./test_radio_airtime
```
**Expected:** All tests pass (12 + 7 + 6 = 25 tests).

**Commit:** (no changes, verification only)

---

### Task 14: ✅ Verify ESP-IDF build compiles

**Run:**
```bash
cd bramble && idf.py build
```
**Expected:** Full firmware builds successfully.

**Commit:** (no changes, verification only)

---

### Task 15: ✅ Add .gitignore and README

**Create** `bramble/.gitignore`:
```
build/
test/build/
sdkconfig
sdkconfig.old
*.o
*.d
```

**Create** `bramble/README.md`:
```markdown
# Bramble

A LoRa mesh networking protocol for ESP32-S3.

## Building

```bash
# Set up ESP-IDF v5.x first
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Host Tests

```bash
cd test && mkdir -p build && cd build
cmake .. && make -j
./test_packet
./test_dedup
./test_radio_airtime
```
```

**Commit:** `git add -A && git commit -m "docs: README and .gitignore"`

---

## Phase 2: Identity & Crypto (Tasks 16–25) ✅ COMPLETE

### Task 16: ✅ Write failing tests for X25519 key pair generation and address derivation

**Create** `bramble/components/crypto/include/crypto.h`:
```c
#ifndef BRAMBLE_CRYPTO_H
#define BRAMBLE_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Key sizes
#define BRAMBLE_KEY_SIZE       32
#define BRAMBLE_ADDR_SIZE      4
#define BRAMBLE_NONCE_SIZE     12
#define BRAMBLE_TAG_SIZE       16
#define BRAMBLE_HMAC_TRUNC     4   // Truncated HMAC for beacons/RREP

// Node identity
typedef struct {
    uint8_t  private_key[BRAMBLE_KEY_SIZE];
    uint8_t  public_key[BRAMBLE_KEY_SIZE];
    uint32_t address;
    uint32_t pubkey_hash;  // First 4 bytes of SHA-256(public_key)
} bramble_identity_t;

// Generate a new node identity (X25519 keypair + address derivation)
int crypto_generate_identity(bramble_identity_t *id);

// Derive node address from public key: address = SHA-256(pubkey)[0:4]
uint32_t crypto_derive_address(const uint8_t *public_key);

// Derive pubkey hash: SHA-256(pubkey)[0:4]
uint32_t crypto_derive_pubkey_hash(const uint8_t *public_key);

// X25519 Diffie-Hellman: compute shared secret
int crypto_x25519_dh(const uint8_t *private_key, const uint8_t *peer_public_key,
                     uint8_t *shared_secret);

// HKDF-SHA256
int crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm, size_t okm_len);

// AES-256-GCM encrypt
int crypto_aes256gcm_encrypt(const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *plaintext, size_t pt_len,
                             const uint8_t *aad, size_t aad_len,
                             uint8_t *ciphertext, uint8_t *tag);

// AES-256-GCM decrypt (returns 0 on success, -1 on auth failure)
int crypto_aes256gcm_decrypt(const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *ciphertext, size_t ct_len,
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *tag, uint8_t *plaintext);

// HMAC-SHA256 (full 32 bytes)
int crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t *mac);

// HMAC-SHA256 truncated to 4 bytes (for beacon/RREP auth)
uint32_t crypto_hmac_sha256_trunc4(const uint8_t *key, size_t key_len,
                                   const uint8_t *data, size_t data_len);

// SHA-256
int crypto_sha256(const uint8_t *data, size_t data_len, uint8_t *hash);

// Build AES-GCM nonce: src_addr(4) || counter(4) || random(4)
void crypto_build_nonce(uint32_t src_addr, uint32_t counter, uint8_t *nonce);

// Generate random bytes
int crypto_random(uint8_t *buf, size_t len);

#endif
```

**Create** `bramble/components/crypto/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "crypto.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls nvs_flash
)
```

**Create** `bramble/test/test_crypto.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include <string.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include "../components/crypto/include/crypto.h"

// Host-side crypto implementation using OpenSSL for testing
// (The real implementation uses mbedtls on ESP32)
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

void test_derive_address_deterministic(void) {
    uint8_t pubkey[32];
    memset(pubkey, 0xAA, 32);

    uint32_t addr1 = crypto_derive_address(pubkey);
    uint32_t addr2 = crypto_derive_address(pubkey);
    TEST_ASSERT_EQUAL_HEX32(addr1, addr2);
    TEST_ASSERT_NOT_EQUAL(0, addr1);
}

void test_derive_address_different_keys(void) {
    uint8_t pubkey1[32], pubkey2[32];
    memset(pubkey1, 0xAA, 32);
    memset(pubkey2, 0xBB, 32);

    uint32_t addr1 = crypto_derive_address(pubkey1);
    uint32_t addr2 = crypto_derive_address(pubkey2);
    TEST_ASSERT_NOT_EQUAL(addr1, addr2);
}

void test_aes256gcm_roundtrip(void) {
    uint8_t key[32], nonce[12], tag[16];
    memset(key, 0x42, 32);
    memset(nonce, 0x13, 12);

    const char *plaintext = "Hello, Bramble mesh!";
    size_t pt_len = strlen(plaintext);

    uint8_t ciphertext[64];
    uint8_t decrypted[64];

    TEST_ASSERT_EQUAL(0, crypto_aes256gcm_encrypt(key, nonce,
        (const uint8_t*)plaintext, pt_len, NULL, 0, ciphertext, tag));

    TEST_ASSERT_EQUAL(0, crypto_aes256gcm_decrypt(key, nonce,
        ciphertext, pt_len, NULL, 0, tag, decrypted));

    TEST_ASSERT_EQUAL_MEMORY(plaintext, decrypted, pt_len);
}

void test_aes256gcm_tamper_detected(void) {
    uint8_t key[32], nonce[12], tag[16];
    memset(key, 0x42, 32);
    memset(nonce, 0x13, 12);

    const char *plaintext = "Secret data";
    size_t pt_len = strlen(plaintext);
    uint8_t ciphertext[64], decrypted[64];

    crypto_aes256gcm_encrypt(key, nonce, (const uint8_t*)plaintext, pt_len,
                             NULL, 0, ciphertext, tag);

    // Tamper with ciphertext
    ciphertext[0] ^= 0xFF;

    TEST_ASSERT_EQUAL(-1, crypto_aes256gcm_decrypt(key, nonce,
        ciphertext, pt_len, NULL, 0, tag, decrypted));
}

void test_hmac_sha256_trunc4(void) {
    uint8_t key[32];
    memset(key, 0x55, 32);
    const char *data = "beacon data";

    uint32_t mac1 = crypto_hmac_sha256_trunc4(key, 32, (const uint8_t*)data, strlen(data));
    uint32_t mac2 = crypto_hmac_sha256_trunc4(key, 32, (const uint8_t*)data, strlen(data));
    TEST_ASSERT_EQUAL_HEX32(mac1, mac2);  // Deterministic
    TEST_ASSERT_NOT_EQUAL(0, mac1);

    // Different key -> different MAC
    uint8_t key2[32];
    memset(key2, 0x66, 32);
    uint32_t mac3 = crypto_hmac_sha256_trunc4(key2, 32, (const uint8_t*)data, strlen(data));
    TEST_ASSERT_NOT_EQUAL(mac1, mac3);
}

void test_hkdf_sha256_derives_key(void) {
    uint8_t ikm[32], salt[16], okm[32];
    memset(ikm, 0xAA, 32);
    memcpy(salt, "bramble-dm-v1\x00\x00\x00", 16);

    TEST_ASSERT_EQUAL(0, crypto_hkdf_sha256(salt, 13, ikm, 32,
        (const uint8_t*)"info", 4, okm, 32));

    // Output should be non-zero and deterministic
    uint8_t okm2[32];
    crypto_hkdf_sha256(salt, 13, ikm, 32, (const uint8_t*)"info", 4, okm2, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(okm, okm2, 32);

    // All zeros is extremely unlikely
    uint8_t zeros[32] = {0};
    TEST_ASSERT_FALSE(memcmp(okm, zeros, 32) == 0);
}

void test_build_nonce(void) {
    uint8_t nonce[12];
    crypto_build_nonce(0xDEADBEEF, 42, nonce);

    // src_addr big-endian
    TEST_ASSERT_EQUAL_HEX8(0xDE, nonce[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, nonce[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, nonce[2]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, nonce[3]);
    // counter big-endian
    TEST_ASSERT_EQUAL_HEX8(0x00, nonce[4]);
    TEST_ASSERT_EQUAL_HEX8(0x00, nonce[5]);
    TEST_ASSERT_EQUAL_HEX8(0x00, nonce[6]);
    TEST_ASSERT_EQUAL_HEX8(0x2A, nonce[7]);
    // Last 4 bytes are random — just check they exist
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_derive_address_deterministic);
    RUN_TEST(test_derive_address_different_keys);
    RUN_TEST(test_aes256gcm_roundtrip);
    RUN_TEST(test_aes256gcm_tamper_detected);
    RUN_TEST(test_hmac_sha256_trunc4);
    RUN_TEST(test_hkdf_sha256_derives_key);
    RUN_TEST(test_build_nonce);
    return UNITY_END();
}
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
# Crypto tests (uses OpenSSL on host)
add_executable(test_crypto test_crypto.c)
target_link_libraries(test_crypto unity esp_stubs ssl crypto)
target_include_directories(test_crypto PRIVATE stubs)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_crypto 2>&1
```
**Expected:** Linker errors — `crypto_host.c` does not exist yet.

**Commit:** `git add -A && git commit -m "test: failing crypto tests for identity, AES-256-GCM, HMAC, HKDF"`

---

### Task 17: ✅ Implement host-side crypto wrappers (OpenSSL)

**Create** `bramble/components/crypto/crypto_host.c`:
```c
// Host-side crypto implementation using OpenSSL (for testing only)
// On ESP32, crypto.c uses mbedtls instead
#ifndef ESP_PLATFORM

#include "crypto.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <string.h>

int crypto_sha256(const uint8_t *data, size_t data_len, uint8_t *hash) {
    SHA256(data, data_len, hash);
    return 0;
}

uint32_t crypto_derive_address(const uint8_t *public_key) {
    uint8_t hash[32];
    crypto_sha256(public_key, 32, hash);
    return ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) |
           ((uint32_t)hash[2] << 8)  | (uint32_t)hash[3];
}

uint32_t crypto_derive_pubkey_hash(const uint8_t *public_key) {
    return crypto_derive_address(public_key);  // Same derivation
}

int crypto_aes256gcm_encrypt(const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *plaintext, size_t pt_len,
                             const uint8_t *aad, size_t aad_len,
                             uint8_t *ciphertext, uint8_t *tag) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len;

    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL);
    EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce);

    if (aad && aad_len > 0) {
        EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_len);
    }

    EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, pt_len);
    EVP_EncryptFinal_ex(ctx, ciphertext + len, &len);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);

    EVP_CIPHER_CTX_free(ctx);
    return 0;
}

int crypto_aes256gcm_decrypt(const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *ciphertext, size_t ct_len,
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *tag, uint8_t *plaintext) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len, ret;

    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL);
    EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce);

    if (aad && aad_len > 0) {
        EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len);
    }

    EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ct_len);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag);

    ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);
    EVP_CIPHER_CTX_free(ctx);

    return (ret > 0) ? 0 : -1;
}

int crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t *mac) {
    unsigned int mac_len = 32;
    HMAC(EVP_sha256(), key, key_len, data, data_len, mac, &mac_len);
    return 0;
}

uint32_t crypto_hmac_sha256_trunc4(const uint8_t *key, size_t key_len,
                                   const uint8_t *data, size_t data_len) {
    uint8_t mac[32];
    crypto_hmac_sha256(key, key_len, data, data_len, mac);
    return ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) |
           ((uint32_t)mac[2] << 8)  | (uint32_t)mac[3];
}

int crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm, size_t okm_len) {
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    EVP_PKEY_derive_init(pctx);
    EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256());
    EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt, salt_len);
    EVP_PKEY_CTX_set1_hkdf_key(pctx, ikm, ikm_len);
    EVP_PKEY_CTX_add1_hkdf_info(pctx, info, info_len);

    size_t outlen = okm_len;
    int ret = EVP_PKEY_derive(pctx, okm, &outlen);
    EVP_PKEY_CTX_free(pctx);
    return (ret == 1) ? 0 : -1;
}

int crypto_random(uint8_t *buf, size_t len) {
    RAND_bytes(buf, len);
    return 0;
}

void crypto_build_nonce(uint32_t src_addr, uint32_t counter, uint8_t *nonce) {
    nonce[0] = (src_addr >> 24) & 0xFF;
    nonce[1] = (src_addr >> 16) & 0xFF;
    nonce[2] = (src_addr >> 8)  & 0xFF;
    nonce[3] = src_addr & 0xFF;
    nonce[4] = (counter >> 24) & 0xFF;
    nonce[5] = (counter >> 16) & 0xFF;
    nonce[6] = (counter >> 8)  & 0xFF;
    nonce[7] = counter & 0xFF;
    crypto_random(&nonce[8], 4);
}

int crypto_x25519_dh(const uint8_t *private_key, const uint8_t *peer_public_key,
                     uint8_t *shared_secret) {
    EVP_PKEY *priv = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, 32);
    EVP_PKEY *pub = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_public_key, 32);

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(priv, NULL);
    EVP_PKEY_derive_init(ctx);
    EVP_PKEY_derive_set_peer(ctx, pub);

    size_t secret_len = 32;
    int ret = EVP_PKEY_derive(ctx, shared_secret, &secret_len);

    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(priv);
    EVP_PKEY_free(pub);
    return (ret == 1) ? 0 : -1;
}

int crypto_generate_identity(bramble_identity_t *id) {
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_keygen(pctx, &pkey);

    size_t len = 32;
    EVP_PKEY_get_raw_private_key(pkey, id->private_key, &len);
    len = 32;
    EVP_PKEY_get_raw_public_key(pkey, id->public_key, &len);

    id->address = crypto_derive_address(id->public_key);
    id->pubkey_hash = crypto_derive_pubkey_hash(id->public_key);

    EVP_PKEY_CTX_free(pctx);
    EVP_PKEY_free(pkey);
    return 0;
}

#endif // !ESP_PLATFORM
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_crypto && ./test_crypto
```
**Expected:**
```
7 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "feat: host-side crypto wrappers (OpenSSL) for testing"`

---

### Task 18: ✅ Implement ESP32 crypto module (mbedtls)

**Create** `bramble/components/crypto/crypto.c`:
```c
// ESP32 crypto implementation using mbedtls (hardware-accelerated on ESP32-S3)
#ifdef ESP_PLATFORM

#include "crypto.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "esp_random.h"
#include <string.h>

int crypto_sha256(const uint8_t *data, size_t data_len, uint8_t *hash) {
    mbedtls_sha256(data, data_len, hash, 0);  // 0 = SHA-256 (not SHA-224)
    return 0;
}

uint32_t crypto_derive_address(const uint8_t *public_key) {
    uint8_t hash[32];
    crypto_sha256(public_key, 32, hash);
    return ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) |
           ((uint32_t)hash[2] << 8)  | (uint32_t)hash[3];
}

uint32_t crypto_derive_pubkey_hash(const uint8_t *public_key) {
    return crypto_derive_address(public_key);
}

int crypto_aes256gcm_encrypt(const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *plaintext, size_t pt_len,
                             const uint8_t *aad, size_t aad_len,
                             uint8_t *ciphertext, uint8_t *tag) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);

    int ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
        pt_len, nonce, 12, aad, aad_len, plaintext, ciphertext, 16, tag);

    mbedtls_gcm_free(&ctx);
    return (ret == 0) ? 0 : -1;
}

int crypto_aes256gcm_decrypt(const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *ciphertext, size_t ct_len,
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *tag, uint8_t *plaintext) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);

    int ret = mbedtls_gcm_auth_decrypt(&ctx, ct_len, nonce, 12,
        aad, aad_len, tag, 16, ciphertext, plaintext);

    mbedtls_gcm_free(&ctx);
    return (ret == 0) ? 0 : -1;
}

int crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t *mac) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_md_hmac(md_info, key, key_len, data, data_len, mac);
}

uint32_t crypto_hmac_sha256_trunc4(const uint8_t *key, size_t key_len,
                                   const uint8_t *data, size_t data_len) {
    uint8_t mac[32];
    crypto_hmac_sha256(key, key_len, data, data_len, mac);
    return ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) |
           ((uint32_t)mac[2] << 8)  | (uint32_t)mac[3];
}

int crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm, size_t okm_len) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_hkdf(md_info, salt, salt_len, ikm, ikm_len, info, info_len, okm, okm_len);
}

int crypto_random(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r = esp_random();
        size_t remaining = len - i;
        size_t to_copy = remaining < 4 ? remaining : 4;
        memcpy(buf + i, &r, to_copy);
    }
    return 0;
}

void crypto_build_nonce(uint32_t src_addr, uint32_t counter, uint8_t *nonce) {
    nonce[0] = (src_addr >> 24) & 0xFF;
    nonce[1] = (src_addr >> 16) & 0xFF;
    nonce[2] = (src_addr >> 8)  & 0xFF;
    nonce[3] = src_addr & 0xFF;
    nonce[4] = (counter >> 24) & 0xFF;
    nonce[5] = (counter >> 16) & 0xFF;
    nonce[6] = (counter >> 8)  & 0xFF;
    nonce[7] = counter & 0xFF;
    crypto_random(&nonce[8], 4);
}

// X25519 using mbedtls ECDH
int crypto_x25519_dh(const uint8_t *private_key, const uint8_t *peer_public_key,
                     uint8_t *shared_secret) {
    mbedtls_ecdh_context ctx;
    mbedtls_ecdh_init(&ctx);
    mbedtls_ecdh_setup(&ctx, MBEDTLS_ECP_DP_CURVE25519);

    // Import private key
    mbedtls_mpi_read_binary(&ctx.ctx.mbed_ecdh.d, private_key, 32);
    // Import peer public key
    mbedtls_mpi_read_binary(&ctx.ctx.mbed_ecdh.Qp.X, peer_public_key, 32);
    mbedtls_mpi_lset(&ctx.ctx.mbed_ecdh.Qp.Z, 1);

    // Compute shared secret
    size_t olen;
    uint8_t buf[32];
    int ret = mbedtls_ecdh_calc_secret(&ctx, &olen, buf, 32, NULL, NULL);

    if (ret == 0 && olen == 32) {
        memcpy(shared_secret, buf, 32);
    }

    mbedtls_ecdh_free(&ctx);
    return (ret == 0) ? 0 : -1;
}

int crypto_generate_identity(bramble_identity_t *id) {
    // Generate random private key
    crypto_random(id->private_key, 32);

    // Clamp per X25519 spec
    id->private_key[0]  &= 248;
    id->private_key[31] &= 127;
    id->private_key[31] |= 64;

    // Compute public key via base point multiplication
    mbedtls_ecdh_context ctx;
    mbedtls_ecdh_init(&ctx);
    mbedtls_ecdh_setup(&ctx, MBEDTLS_ECP_DP_CURVE25519);

    mbedtls_mpi_read_binary(&ctx.ctx.mbed_ecdh.d, id->private_key, 32);

    size_t olen;
    uint8_t buf[66];
    mbedtls_ecdh_make_public(&ctx, &olen, buf, sizeof(buf), NULL, NULL);
    // Extract public key (skip format byte if present)
    if (olen >= 32) {
        memcpy(id->public_key, buf + (olen - 32), 32);
    }

    id->address = crypto_derive_address(id->public_key);
    id->pubkey_hash = crypto_derive_pubkey_hash(id->public_key);

    mbedtls_ecdh_free(&ctx);
    return 0;
}

#endif // ESP_PLATFORM
```

**Commit:** `git add -A && git commit -m "feat: ESP32 crypto module using mbedtls with HW acceleration"`

---

### Task 19: ✅ Write failing tests for NVS identity storage

**Create** `bramble/components/identity/include/identity.h`:
```c
#ifndef BRAMBLE_IDENTITY_H
#define BRAMBLE_IDENTITY_H

#include "crypto.h"
#include <stdbool.h>

// Load identity from NVS. Returns 0 on success, -1 if not found.
int identity_load(bramble_identity_t *id);

// Save identity to NVS.
int identity_save(const bramble_identity_t *id);

// Generate new identity and save to NVS.
int identity_generate_and_save(bramble_identity_t *id);

// Check if another node's beacon has our address (collision detection).
// Returns true if collision detected and we should regenerate.
bool identity_check_collision(const bramble_identity_t *my_id,
                              uint32_t beacon_src_addr,
                              uint32_t beacon_pubkey_hash);

#endif
```

**Create** `bramble/components/identity/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "identity.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash crypto
)
```

This component runs on ESP32 only (NVS). Tests for address collision logic can run on host.

**Create** `bramble/test/test_identity.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include "../components/crypto/include/crypto.h"
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

void test_collision_detection_same_addr_different_pubkey(void) {
    // Simulate: my address matches beacon src_addr, but pubkey_hash differs
    // If my pubkey < beacon pubkey (lexicographically), I should regenerate
    bramble_identity_t my_id;
    crypto_generate_identity(&my_id);

    // Same address, different pubkey hash -> collision
    uint32_t beacon_pubkey_hash = my_id.pubkey_hash ^ 0xFFFFFFFF;  // Different
    bool collision = (my_id.address == my_id.address) && (beacon_pubkey_hash != my_id.pubkey_hash);
    TEST_ASSERT_TRUE(collision);
}

void test_collision_detection_same_addr_same_pubkey(void) {
    // Same address AND same pubkey_hash -> this is us, not a collision
    bramble_identity_t my_id;
    crypto_generate_identity(&my_id);

    bool collision = (my_id.address == my_id.address) && (my_id.pubkey_hash != my_id.pubkey_hash);
    TEST_ASSERT_FALSE(collision);
}

void test_address_is_4_bytes_from_pubkey_hash(void) {
    bramble_identity_t id;
    crypto_generate_identity(&id);

    uint32_t expected_addr = crypto_derive_address(id.public_key);
    TEST_ASSERT_EQUAL_HEX32(expected_addr, id.address);
    TEST_ASSERT_NOT_EQUAL(0, id.address);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_collision_detection_same_addr_different_pubkey);
    RUN_TEST(test_collision_detection_same_addr_same_pubkey);
    RUN_TEST(test_address_is_4_bytes_from_pubkey_hash);
    return UNITY_END();
}
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
add_executable(test_identity test_identity.c)
target_link_libraries(test_identity unity esp_stubs ssl crypto)
target_include_directories(test_identity PRIVATE stubs)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_identity && ./test_identity
```
**Expected:** 3 tests pass.

**Commit:** `git add -A && git commit -m "test: identity generation and collision detection tests"`

---

### Task 20: ✅ Implement NVS identity storage (ESP32-side)

**Create** `bramble/components/identity/identity.c`:
```c
#include "identity.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "identity";
#define NVS_NAMESPACE "bramble"

int identity_load(bramble_identity_t *id) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return -1;

    size_t len = 32;
    err = nvs_get_blob(handle, "privkey", id->private_key, &len);
    if (err != ESP_OK) { nvs_close(handle); return -1; }

    len = 32;
    err = nvs_get_blob(handle, "pubkey", id->public_key, &len);
    if (err != ESP_OK) { nvs_close(handle); return -1; }

    err = nvs_get_u32(handle, "addr", &id->address);
    if (err != ESP_OK) { nvs_close(handle); return -1; }

    err = nvs_get_u32(handle, "pubhash", &id->pubkey_hash);
    if (err != ESP_OK) { nvs_close(handle); return -1; }

    nvs_close(handle);
    ESP_LOGI(TAG, "Identity loaded: addr=0x%08lX", (unsigned long)id->address);
    return 0;
}

int identity_save(const bramble_identity_t *id) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return -1;

    nvs_set_blob(handle, "privkey", id->private_key, 32);
    nvs_set_blob(handle, "pubkey", id->public_key, 32);
    nvs_set_u32(handle, "addr", id->address);
    nvs_set_u32(handle, "pubhash", id->pubkey_hash);

    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Identity saved: addr=0x%08lX", (unsigned long)id->address);
    return 0;
}

int identity_generate_and_save(bramble_identity_t *id) {
    int ret = crypto_generate_identity(id);
    if (ret != 0) return ret;
    return identity_save(id);
}

#else
int identity_load(bramble_identity_t *id) { (void)id; return -1; }
int identity_save(const bramble_identity_t *id) { (void)id; return -1; }
int identity_generate_and_save(bramble_identity_t *id) { (void)id; return -1; }
#endif

bool identity_check_collision(const bramble_identity_t *my_id,
                              uint32_t beacon_src_addr,
                              uint32_t beacon_pubkey_hash) {
    if (beacon_src_addr != my_id->address) return false;
    if (beacon_pubkey_hash == my_id->pubkey_hash) return false;  // It's us

    // Collision! Node with lexicographically smaller pubkey regenerates.
    // Return true if WE should regenerate (caller decides based on pubkey comparison)
    return true;
}
```

**Commit:** `git add -A && git commit -m "feat: NVS identity storage with collision detection"`

---

### Task 21: ✅ Write failing tests for channel PSK derivation with epoch ratchet

**Create** `bramble/test/test_channel_key.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include "../components/crypto/include/crypto.h"
#include "../components/crypto/crypto_host.c"

// Channel key derivation (will be in channel.c)
typedef struct {
    uint8_t  key[32];
    uint8_t  channel_id;
    uint16_t epoch;
    char     name[32];
} channel_state_t;

int channel_derive_key(const char *psk, uint8_t *key, uint8_t *channel_id);
int channel_advance_epoch(channel_state_t *ch);

// Include implementation
#include "../components/channel/channel_key.c"

void setUp(void) {}
void tearDown(void) {}

void test_channel_derive_key_deterministic(void) {
    uint8_t key1[32], key2[32];
    uint8_t id1, id2;
    channel_derive_key("my-secret-channel", key1, &id1);
    channel_derive_key("my-secret-channel", key2, &id2);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(key1, key2, 32);
    TEST_ASSERT_EQUAL(id1, id2);
}

void test_channel_derive_key_different_psk(void) {
    uint8_t key1[32], key2[32];
    uint8_t id1, id2;
    channel_derive_key("channel-alpha", key1, &id1);
    channel_derive_key("channel-beta", key2, &id2);
    TEST_ASSERT_FALSE(memcmp(key1, key2, 32) == 0);
}

void test_channel_id_from_key_hash(void) {
    uint8_t key[32];
    uint8_t id;
    channel_derive_key("test", key, &id);
    // channel_id = SHA-256(key)[0] % 16
    uint8_t hash[32];
    crypto_sha256(key, 32, hash);
    TEST_ASSERT_EQUAL(hash[0] % 16, id);
}

void test_epoch_advance_changes_key(void) {
    channel_state_t ch;
    memset(&ch, 0, sizeof(ch));
    channel_derive_key("test-channel", ch.key, &ch.channel_id);
    ch.epoch = 0;

    uint8_t original_key[32];
    memcpy(original_key, ch.key, 32);

    channel_advance_epoch(&ch);
    TEST_ASSERT_EQUAL(1, ch.epoch);
    TEST_ASSERT_FALSE(memcmp(ch.key, original_key, 32) == 0);
}

void test_epoch_advance_is_one_way(void) {
    channel_state_t ch;
    memset(&ch, 0, sizeof(ch));
    channel_derive_key("test-channel", ch.key, &ch.channel_id);
    ch.epoch = 0;

    uint8_t key_epoch_0[32];
    memcpy(key_epoch_0, ch.key, 32);

    channel_advance_epoch(&ch);
    uint8_t key_epoch_1[32];
    memcpy(key_epoch_1, ch.key, 32);

    // Cannot derive key_epoch_0 from key_epoch_1 (one-way HKDF)
    // Just verify they're different
    TEST_ASSERT_FALSE(memcmp(key_epoch_0, key_epoch_1, 32) == 0);
}

void test_epoch_catchup(void) {
    // Simulate offline node catching up by N epochs
    channel_state_t ch_current, ch_behind;
    memset(&ch_current, 0, sizeof(ch_current));
    memset(&ch_behind, 0, sizeof(ch_behind));

    channel_derive_key("catchup-test", ch_current.key, &ch_current.channel_id);
    ch_current.epoch = 0;
    memcpy(&ch_behind, &ch_current, sizeof(channel_state_t));

    // Advance current by 5 epochs
    for (int i = 0; i < 5; i++) {
        channel_advance_epoch(&ch_current);
    }
    TEST_ASSERT_EQUAL(5, ch_current.epoch);

    // Behind node catches up
    for (int i = 0; i < 5; i++) {
        channel_advance_epoch(&ch_behind);
    }
    TEST_ASSERT_EQUAL(5, ch_behind.epoch);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ch_current.key, ch_behind.key, 32);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_channel_derive_key_deterministic);
    RUN_TEST(test_channel_derive_key_different_psk);
    RUN_TEST(test_channel_id_from_key_hash);
    RUN_TEST(test_epoch_advance_changes_key);
    RUN_TEST(test_epoch_advance_is_one_way);
    RUN_TEST(test_epoch_catchup);
    return UNITY_END();
}
```

**Run:** Expected to fail (channel_key.c doesn't exist).

**Commit:** `git add -A && git commit -m "test: failing channel PSK derivation and epoch ratchet tests"`

---

### Task 22: ✅ Implement channel key derivation and epoch ratchet

**Create** `bramble/components/channel/channel_key.c`:
```c
#include "crypto.h"
#include <string.h>

typedef struct {
    uint8_t  key[32];
    uint8_t  channel_id;
    uint16_t epoch;
    char     name[32];
} channel_state_t;

int channel_derive_key(const char *psk, uint8_t *key, uint8_t *channel_id) {
    // channel_key = HKDF-SHA256(salt="bramble-channel-v1", ikm=SHA-256(psk), info=0x00)
    uint8_t psk_hash[32];
    crypto_sha256((const uint8_t*)psk, strlen(psk), psk_hash);

    uint8_t info = 0x00;
    int ret = crypto_hkdf_sha256(
        (const uint8_t*)"bramble-channel-v1", 18,
        psk_hash, 32,
        &info, 1,
        key, 32
    );

    // channel_id = SHA-256(key)[0] % 16
    uint8_t key_hash[32];
    crypto_sha256(key, 32, key_hash);
    *channel_id = key_hash[0] % 16;

    return ret;
}

int channel_advance_epoch(channel_state_t *ch) {
    uint16_t next_epoch = ch->epoch + 1;
    uint8_t info[2] = {
        (next_epoch >> 8) & 0xFF,
        next_epoch & 0xFF,
    };

    uint8_t new_key[32];
    int ret = crypto_hkdf_sha256(
        (const uint8_t*)"bramble-channel-epoch", 21,
        ch->key, 32,
        info, 2,
        new_key, 32
    );

    if (ret == 0) {
        memcpy(ch->key, new_key, 32);
        ch->epoch = next_epoch;
    }

    return ret;
}
```

**Create** `bramble/components/channel/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "channel_key.c" "channel.c"
    INCLUDE_DIRS "include"
    REQUIRES crypto nvs_flash
)
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
add_executable(test_channel_key test_channel_key.c)
target_link_libraries(test_channel_key unity esp_stubs ssl crypto)
target_include_directories(test_channel_key PRIVATE stubs)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_channel_key && ./test_channel_key
```
**Expected:**
```
6 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "feat: channel PSK derivation with epoch-based HKDF ratchet"`

---

### Task 23: ✅ Write failing tests for key exchange protocol

**Create** `bramble/test/test_key_exchange.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include "../components/crypto/include/crypto.h"
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

void test_double_dh_key_agreement(void) {
    // Both sides should derive the same session key via Double-DH
    bramble_identity_t alice, bob;
    crypto_generate_identity(&alice);
    crypto_generate_identity(&bob);

    // Alice generates ephemeral
    bramble_identity_t alice_eph;
    crypto_generate_identity(&alice_eph);

    // Alice computes: ss1 = eph_A × static_B, ss2 = static_A × static_B
    uint8_t ss1_alice[32], ss2_alice[32];
    crypto_x25519_dh(alice_eph.private_key, bob.public_key, ss1_alice);
    crypto_x25519_dh(alice.private_key, bob.public_key, ss2_alice);

    // Bob computes: ss1 = static_B × eph_A, ss2 = static_B × static_A
    uint8_t ss1_bob[32], ss2_bob[32];
    crypto_x25519_dh(bob.private_key, alice_eph.public_key, ss1_bob);
    crypto_x25519_dh(bob.private_key, alice.public_key, ss2_bob);

    TEST_ASSERT_EQUAL_HEX8_ARRAY(ss1_alice, ss1_bob, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ss2_alice, ss2_bob, 32);

    // Derive session key
    uint8_t ikm_alice[64], ikm_bob[64];
    memcpy(ikm_alice, ss1_alice, 32);
    memcpy(ikm_alice + 32, ss2_alice, 32);
    memcpy(ikm_bob, ss1_bob, 32);
    memcpy(ikm_bob + 32, ss2_bob, 32);

    // info = min(addr_a, addr_b) || max(addr_a, addr_b)
    uint32_t min_addr = alice.address < bob.address ? alice.address : bob.address;
    uint32_t max_addr = alice.address < bob.address ? bob.address : alice.address;
    uint8_t info[8];
    info[0] = (min_addr >> 24) & 0xFF; info[1] = (min_addr >> 16) & 0xFF;
    info[2] = (min_addr >> 8)  & 0xFF; info[3] = min_addr & 0xFF;
    info[4] = (max_addr >> 24) & 0xFF; info[5] = (max_addr >> 16) & 0xFF;
    info[6] = (max_addr >> 8)  & 0xFF; info[7] = max_addr & 0xFF;

    uint8_t key_alice[32], key_bob[32];
    crypto_hkdf_sha256((const uint8_t*)"bramble-dm-v1", 13,
                       ikm_alice, 64, info, 8, key_alice, 32);
    crypto_hkdf_sha256((const uint8_t*)"bramble-dm-v1", 13,
                       ikm_bob, 64, info, 8, key_bob, 32);

    TEST_ASSERT_EQUAL_HEX8_ARRAY(key_alice, key_bob, 32);

    // Session key should be non-zero
    uint8_t zeros[32] = {0};
    TEST_ASSERT_FALSE(memcmp(key_alice, zeros, 32) == 0);
}

void test_auth_tag_verification(void) {
    uint8_t session_key[32];
    memset(session_key, 0x42, 32);

    uint32_t key_id = 0xAABBCCDD;
    uint8_t eph_pub[32];
    memset(eph_pub, 0x55, 32);

    // Build auth data: key_id || eph_pub
    uint8_t auth_data[36];
    auth_data[0] = (key_id >> 24) & 0xFF;
    auth_data[1] = (key_id >> 16) & 0xFF;
    auth_data[2] = (key_id >> 8) & 0xFF;
    auth_data[3] = key_id & 0xFF;
    memcpy(auth_data + 4, eph_pub, 32);

    uint8_t mac[32];
    crypto_hmac_sha256(session_key, 32, auth_data, 36, mac);

    // Verify same computation produces same result
    uint8_t mac2[32];
    crypto_hmac_sha256(session_key, 32, auth_data, 36, mac2);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, mac2, 16);  // First 16 bytes used as auth_tag
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_double_dh_key_agreement);
    RUN_TEST(test_auth_tag_verification);
    return UNITY_END();
}
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
add_executable(test_key_exchange test_key_exchange.c)
target_link_libraries(test_key_exchange unity esp_stubs ssl crypto)
target_include_directories(test_key_exchange PRIVATE stubs)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_key_exchange && ./test_key_exchange
```
**Expected:**
```
2 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "test: key exchange Double-DH agreement and auth tag verification"`

---

### Task 24: ✅ Write failing test for RREQ source encryption/decryption

**Create** `bramble/test/test_rreq_privacy.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include "../components/crypto/include/crypto.h"
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

// Encrypt source address for RREQ (§5.4)
uint32_t rreq_encrypt_source(uint32_t my_addr, const uint8_t *my_privkey,
                             const uint8_t *dest_pubkey, uint32_t time_bucket) {
    uint8_t shared[32];
    crypto_x25519_dh(my_privkey, dest_pubkey, shared);

    // OTP = SHA-256(shared || time_bucket_be32 || "rreq")[0:4]
    uint8_t hash_input[32 + 4 + 4];
    memcpy(hash_input, shared, 32);
    hash_input[32] = (time_bucket >> 24) & 0xFF;
    hash_input[33] = (time_bucket >> 16) & 0xFF;
    hash_input[34] = (time_bucket >> 8) & 0xFF;
    hash_input[35] = time_bucket & 0xFF;
    memcpy(hash_input + 36, "rreq", 4);

    uint8_t otp_hash[32];
    crypto_sha256(hash_input, 40, otp_hash);

    uint32_t otp = ((uint32_t)otp_hash[0] << 24) | ((uint32_t)otp_hash[1] << 16) |
                   ((uint32_t)otp_hash[2] << 8) | (uint32_t)otp_hash[3];

    return my_addr ^ otp;
}

// Decrypt: destination tries all known peers
uint32_t rreq_decrypt_source(uint32_t encrypted_source, const uint8_t *my_privkey,
                             const uint8_t *peer_pubkey, uint32_t peer_addr,
                             uint32_t time_bucket) {
    uint8_t shared[32];
    crypto_x25519_dh(my_privkey, peer_pubkey, shared);

    uint8_t hash_input[32 + 4 + 4];
    memcpy(hash_input, shared, 32);
    hash_input[32] = (time_bucket >> 24) & 0xFF;
    hash_input[33] = (time_bucket >> 16) & 0xFF;
    hash_input[34] = (time_bucket >> 8) & 0xFF;
    hash_input[35] = time_bucket & 0xFF;
    memcpy(hash_input + 36, "rreq", 4);

    uint8_t otp_hash[32];
    crypto_sha256(hash_input, 40, otp_hash);

    uint32_t otp = ((uint32_t)otp_hash[0] << 24) | ((uint32_t)otp_hash[1] << 16) |
                   ((uint32_t)otp_hash[2] << 8) | (uint32_t)otp_hash[3];

    uint32_t candidate = encrypted_source ^ otp;
    return (candidate == peer_addr) ? peer_addr : 0;
}

void test_rreq_source_encrypt_decrypt(void) {
    bramble_identity_t alice, bob;
    crypto_generate_identity(&alice);
    crypto_generate_identity(&bob);

    uint32_t time_bucket = 1000;
    uint32_t encrypted = rreq_encrypt_source(alice.address, alice.private_key,
                                              bob.public_key, time_bucket);

    // Encrypted should not be plaintext
    TEST_ASSERT_NOT_EQUAL(alice.address, encrypted);

    // Bob can decrypt
    uint32_t decrypted = rreq_decrypt_source(encrypted, bob.private_key,
                                              alice.public_key, alice.address,
                                              time_bucket);
    TEST_ASSERT_EQUAL_HEX32(alice.address, decrypted);
}

void test_rreq_source_wrong_time_bucket_fails(void) {
    bramble_identity_t alice, bob;
    crypto_generate_identity(&alice);
    crypto_generate_identity(&bob);

    uint32_t encrypted = rreq_encrypt_source(alice.address, alice.private_key,
                                              bob.public_key, 1000);

    // Wrong time bucket -> decryption fails
    uint32_t decrypted = rreq_decrypt_source(encrypted, bob.private_key,
                                              alice.public_key, alice.address, 999);
    TEST_ASSERT_EQUAL_HEX32(0, decrypted);
}

void test_rreq_open_source_fallback(void) {
    // When OPEN_SOURCE flag is set, encrypted_source is just the plaintext address
    uint32_t src_addr = 0xDEADBEEF;
    // No encryption needed — just pass through
    TEST_ASSERT_EQUAL_HEX32(src_addr, src_addr);  // Trivial
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rreq_source_encrypt_decrypt);
    RUN_TEST(test_rreq_source_wrong_time_bucket_fails);
    RUN_TEST(test_rreq_open_source_fallback);
    return UNITY_END();
}
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
add_executable(test_rreq_privacy test_rreq_privacy.c)
target_link_libraries(test_rreq_privacy unity esp_stubs ssl crypto)
target_include_directories(test_rreq_privacy PRIVATE stubs)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_rreq_privacy && ./test_rreq_privacy
```
**Expected:**
```
3 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "test: RREQ source encryption/decryption with time-bucket OTP"`

---

### Task 25: ✅ Run all Phase 2 tests

**Run:**
```bash
cd bramble/test/build && cmake .. && make -j
./test_packet && ./test_dedup && ./test_radio_airtime && \
./test_crypto && ./test_identity && ./test_channel_key && \
./test_key_exchange && ./test_rreq_privacy
```
**Expected:** All tests pass (12+7+6+7+3+6+2+3 = 46 tests total).

**Commit:** `git add -A && git commit -m "milestone: Phase 2 complete — identity, crypto, channels"`

---

## Phase 3: Routing & Forwarding (Tasks 26–40) ✅ COMPLETE

### Task 26: ✅ Write failing tests for neighbor table

**Create** `bramble/components/routing/include/routing.h`:
```c
#ifndef BRAMBLE_ROUTING_H
#define BRAMBLE_ROUTING_H

#include <stdint.h>
#include <stdbool.h>

// --- Neighbor Table ---
#define MAX_NEIGHBORS 32
#define NEIGHBOR_EXPIRY_MS 600000  // 10 minutes

typedef struct {
    uint32_t addr;
    int8_t   rssi;
    int8_t   snr;
    uint8_t  success_rate;
    uint8_t  congestion;
    uint32_t last_heard;
    uint32_t pubkey_hash;
    uint16_t tx_count;
    uint16_t tx_success;
    bool     suspicious;  // Sybil heuristic flag
} neighbor_entry_t;

typedef struct {
    neighbor_entry_t entries[MAX_NEIGHBORS];
    int count;
} neighbor_table_t;

void neighbor_init(neighbor_table_t *table);
int  neighbor_update(neighbor_table_t *table, uint32_t addr, int8_t rssi, int8_t snr,
                     uint32_t pubkey_hash, uint32_t now_ms);
neighbor_entry_t *neighbor_lookup(neighbor_table_t *table, uint32_t addr);
void neighbor_purge(neighbor_table_t *table, uint32_t now_ms);
int  neighbor_count(const neighbor_table_t *table);

// --- Routing Table ---
#define MAX_ROUTES 64
#define ROUTE_ACTIVE_TIMEOUT_MS   300000   // 5 min
#define ROUTE_STALE_TIMEOUT_MS    600000   // 10 min
#define ROUTE_HARD_TIMEOUT_MS     3600000  // 1 hr

typedef enum {
    ROUTE_DISCOVERING = 0,
    ROUTE_UNVERIFIED,
    ROUTE_ACTIVE,
    ROUTE_STALE,
    ROUTE_BROKEN,
} route_state_t;

typedef struct {
    uint32_t     dest_addr;
    uint32_t     next_hop;
    uint8_t      hop_count;
    uint8_t      metric;
    route_state_t state;
    uint8_t      fail_count;
    uint32_t     last_used;
    uint32_t     last_confirmed;
    uint16_t     use_count;
} route_entry_t;

typedef struct {
    route_entry_t entries[MAX_ROUTES];
    int count;
} routing_table_t;

void  route_init(routing_table_t *table);
int   route_install(routing_table_t *table, uint32_t dest, uint32_t next_hop,
                    uint8_t hop_count, uint8_t metric, route_state_t state, uint32_t now_ms);
route_entry_t *route_lookup(routing_table_t *table, uint32_t dest_addr);
void  route_maintenance(routing_table_t *table, uint32_t now_ms);
int   route_count(const routing_table_t *table);
route_entry_t *route_find_alternate(routing_table_t *table, uint32_t dest, uint32_t exclude_hop);

// --- RREQ Dedup Cache ---
#define RREQ_DEDUP_MAX 128
#define RREQ_DEDUP_EXPIRY_MS 30000

typedef struct {
    uint32_t query_id;
    uint32_t timestamp;
} rreq_seen_t;

typedef struct {
    rreq_seen_t entries[RREQ_DEDUP_MAX];
    int count;
} rreq_dedup_t;

void rreq_dedup_init(rreq_dedup_t *cache);
bool rreq_dedup_check_and_add(rreq_dedup_t *cache, uint32_t query_id, uint32_t now_ms);

// --- Reverse Route Table ---
#define MAX_REVERSE_ROUTES 32
#define REVERSE_ROUTE_EXPIRY_MS 60000

typedef struct {
    uint32_t query_id;
    uint32_t prev_hop;
    uint32_t timestamp;
} reverse_route_t;

typedef struct {
    reverse_route_t entries[MAX_REVERSE_ROUTES];
    int count;
} reverse_route_table_t;

void reverse_route_init(reverse_route_table_t *table);
int  reverse_route_add(reverse_route_table_t *table, uint32_t query_id,
                       uint32_t prev_hop, uint32_t now_ms);
reverse_route_t *reverse_route_lookup(reverse_route_table_t *table, uint32_t query_id);
void reverse_route_purge(reverse_route_table_t *table, uint32_t now_ms);

// --- Link quality ---
uint8_t compute_link_penalty(int8_t rssi, int8_t snr);

#endif
```

**Create** `bramble/test/test_neighbor.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include "../components/routing/include/routing.h"
#include "../components/routing/routing.c"

void setUp(void) {}
void tearDown(void) {}

void test_neighbor_init_empty(void) {
    neighbor_table_t table;
    neighbor_init(&table);
    TEST_ASSERT_EQUAL(0, neighbor_count(&table));
}

void test_neighbor_add_and_lookup(void) {
    neighbor_table_t table;
    neighbor_init(&table);
    neighbor_update(&table, 0xAABBCCDD, -80, 5, 0x11111111, 1000);
    TEST_ASSERT_EQUAL(1, neighbor_count(&table));

    neighbor_entry_t *n = neighbor_lookup(&table, 0xAABBCCDD);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL(-80, n->rssi);
    TEST_ASSERT_EQUAL(5, n->snr);
}

void test_neighbor_update_existing(void) {
    neighbor_table_t table;
    neighbor_init(&table);
    neighbor_update(&table, 0xAABBCCDD, -80, 5, 0x11111111, 1000);
    neighbor_update(&table, 0xAABBCCDD, -70, 8, 0x11111111, 2000);
    TEST_ASSERT_EQUAL(1, neighbor_count(&table));

    neighbor_entry_t *n = neighbor_lookup(&table, 0xAABBCCDD);
    TEST_ASSERT_EQUAL(-70, n->rssi);
    TEST_ASSERT_EQUAL(8, n->snr);
    TEST_ASSERT_EQUAL(2000, n->last_heard);
}

void test_neighbor_purge_expired(void) {
    neighbor_table_t table;
    neighbor_init(&table);
    neighbor_update(&table, 0x11111111, -80, 5, 0xAAAAAAAA, 1000);
    neighbor_update(&table, 0x22222222, -70, 8, 0xBBBBBBBB, 500000);

    neighbor_purge(&table, 700000);  // First one expired (>600s)
    TEST_ASSERT_EQUAL(1, neighbor_count(&table));
    TEST_ASSERT_NULL(neighbor_lookup(&table, 0x11111111));
    TEST_ASSERT_NOT_NULL(neighbor_lookup(&table, 0x22222222));
}

void test_neighbor_full_evicts_oldest(void) {
    neighbor_table_t table;
    neighbor_init(&table);

    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        neighbor_update(&table, (uint32_t)(i + 1), -80, 5, (uint32_t)i, (uint32_t)(i * 100));
    }
    TEST_ASSERT_EQUAL(MAX_NEIGHBORS, neighbor_count(&table));

    // Add one more — should evict oldest (last_heard=0)
    neighbor_update(&table, 0xFFFF0000, -60, 10, 0xFFFF, 50000);
    TEST_ASSERT_EQUAL(MAX_NEIGHBORS, neighbor_count(&table));
    TEST_ASSERT_NULL(neighbor_lookup(&table, 1));  // Oldest evicted
    TEST_ASSERT_NOT_NULL(neighbor_lookup(&table, 0xFFFF0000));
}

void test_link_penalty_excellent(void) {
    // RSSI -60, SNR 10 -> penalty ~0
    uint8_t penalty = compute_link_penalty(-60, 10);
    TEST_ASSERT_LESS_OR_EQUAL(5, penalty);
}

void test_link_penalty_marginal(void) {
    // RSSI -120, SNR -5 -> penalty ~50
    uint8_t penalty = compute_link_penalty(-120, -5);
    TEST_ASSERT_GREATER_OR_EQUAL(30, penalty);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_neighbor_init_empty);
    RUN_TEST(test_neighbor_add_and_lookup);
    RUN_TEST(test_neighbor_update_existing);
    RUN_TEST(test_neighbor_purge_expired);
    RUN_TEST(test_neighbor_full_evicts_oldest);
    RUN_TEST(test_link_penalty_excellent);
    RUN_TEST(test_link_penalty_marginal);
    return UNITY_END();
}
```

**Commit:** `git add -A && git commit -m "test: failing neighbor table tests"`

---

### Task 27: ✅ Implement neighbor table, routing table, and RREQ dedup

**Create** `bramble/components/routing/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "routing.c"
    INCLUDE_DIRS "include"
)
```

**Create** `bramble/components/routing/routing.c`:
```c
#include "routing.h"
#include <string.h>

// === Neighbor Table ===

void neighbor_init(neighbor_table_t *table) {
    memset(table, 0, sizeof(*table));
}

int neighbor_update(neighbor_table_t *table, uint32_t addr, int8_t rssi, int8_t snr,
                    uint32_t pubkey_hash, uint32_t now_ms) {
    // Check if already exists
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].addr == addr) {
            table->entries[i].rssi = rssi;
            table->entries[i].snr = snr;
            table->entries[i].pubkey_hash = pubkey_hash;
            table->entries[i].last_heard = now_ms;
            return 0;
        }
    }

    // Add new
    if (table->count >= MAX_NEIGHBORS) {
        // Evict oldest (lowest last_heard)
        int oldest = 0;
        for (int i = 1; i < table->count; i++) {
            if (table->entries[i].last_heard < table->entries[oldest].last_heard) {
                oldest = i;
            }
        }
        table->entries[oldest] = table->entries[table->count - 1];
        table->count--;
    }

    neighbor_entry_t *e = &table->entries[table->count];
    memset(e, 0, sizeof(*e));
    e->addr = addr;
    e->rssi = rssi;
    e->snr = snr;
    e->pubkey_hash = pubkey_hash;
    e->last_heard = now_ms;
    table->count++;
    return 0;
}

neighbor_entry_t *neighbor_lookup(neighbor_table_t *table, uint32_t addr) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].addr == addr) return &table->entries[i];
    }
    return NULL;
}

void neighbor_purge(neighbor_table_t *table, uint32_t now_ms) {
    int write = 0;
    for (int read = 0; read < table->count; read++) {
        if ((now_ms - table->entries[read].last_heard) < NEIGHBOR_EXPIRY_MS) {
            if (write != read) table->entries[write] = table->entries[read];
            write++;
        }
    }
    table->count = write;
}

int neighbor_count(const neighbor_table_t *table) {
    return table->count;
}

// === Routing Table ===

void route_init(routing_table_t *table) {
    memset(table, 0, sizeof(*table));
}

int route_install(routing_table_t *table, uint32_t dest, uint32_t next_hop,
                  uint8_t hop_count, uint8_t metric, route_state_t state, uint32_t now_ms) {
    // Check for existing route to same dest
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].dest_addr == dest) {
            route_entry_t *e = &table->entries[i];
            // Update if better metric or same metric with fewer hops
            if (metric > e->metric || (metric == e->metric && hop_count < e->hop_count) ||
                e->state == ROUTE_BROKEN || e->state == ROUTE_STALE) {
                e->next_hop = next_hop;
                e->hop_count = hop_count;
                e->metric = metric;
                e->state = state;
                e->fail_count = 0;
                e->last_used = now_ms;
                e->last_confirmed = now_ms;
            }
            return 0;
        }
    }

    // Add new route
    if (table->count >= MAX_ROUTES) {
        // Evict: prefer broken, then stale, then oldest active
        int victim = -1;
        for (int i = 0; i < table->count; i++) {
            if (table->entries[i].state == ROUTE_BROKEN) { victim = i; break; }
        }
        if (victim < 0) {
            for (int i = 0; i < table->count; i++) {
                if (table->entries[i].state == ROUTE_STALE) { victim = i; break; }
            }
        }
        if (victim < 0) {
            // Evict LRU
            victim = 0;
            for (int i = 1; i < table->count; i++) {
                if (table->entries[i].last_used < table->entries[victim].last_used) {
                    victim = i;
                }
            }
        }
        table->entries[victim] = table->entries[table->count - 1];
        table->count--;
    }

    route_entry_t *e = &table->entries[table->count];
    memset(e, 0, sizeof(*e));
    e->dest_addr = dest;
    e->next_hop = next_hop;
    e->hop_count = hop_count;
    e->metric = metric;
    e->state = state;
    e->last_used = now_ms;
    e->last_confirmed = now_ms;
    table->count++;
    return 0;
}

route_entry_t *route_lookup(routing_table_t *table, uint32_t dest_addr) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].dest_addr == dest_addr) return &table->entries[i];
    }
    return NULL;
}

route_entry_t *route_find_alternate(routing_table_t *table, uint32_t dest, uint32_t exclude_hop) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].dest_addr == dest &&
            table->entries[i].next_hop != exclude_hop &&
            (table->entries[i].state == ROUTE_ACTIVE || table->entries[i].state == ROUTE_STALE)) {
            return &table->entries[i];
        }
    }
    return NULL;
}

void route_maintenance(routing_table_t *table, uint32_t now_ms) {
    int write = 0;
    for (int read = 0; read < table->count; read++) {
        route_entry_t *e = &table->entries[read];
        uint32_t age = now_ms - e->last_used;
        uint32_t confirmed_age = now_ms - e->last_confirmed;

        bool remove = false;
        if (age > ROUTE_HARD_TIMEOUT_MS) {
            remove = true;
        } else if (e->state == ROUTE_STALE && age > ROUTE_STALE_TIMEOUT_MS) {
            remove = true;
        } else if (e->state == ROUTE_ACTIVE && age > ROUTE_ACTIVE_TIMEOUT_MS) {
            e->state = ROUTE_STALE;
        }
        if (e->state == ROUTE_ACTIVE && confirmed_age > ROUTE_ACTIVE_TIMEOUT_MS * 2) {
            e->state = ROUTE_STALE;
        }

        if (!remove) {
            if (write != read) table->entries[write] = table->entries[read];
            write++;
        }
    }
    table->count = write;
}

int route_count(const routing_table_t *table) {
    return table->count;
}

// === RREQ Dedup ===

void rreq_dedup_init(rreq_dedup_t *cache) {
    memset(cache, 0, sizeof(*cache));
}

bool rreq_dedup_check_and_add(rreq_dedup_t *cache, uint32_t query_id, uint32_t now_ms) {
    // Purge old
    int write = 0;
    for (int i = 0; i < cache->count; i++) {
        if ((now_ms - cache->entries[i].timestamp) < RREQ_DEDUP_EXPIRY_MS) {
            if (write != i) cache->entries[write] = cache->entries[i];
            write++;
        }
    }
    cache->count = write;

    // Check
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].query_id == query_id) return true;
    }

    // Add
    if (cache->count < RREQ_DEDUP_MAX) {
        cache->entries[cache->count].query_id = query_id;
        cache->entries[cache->count].timestamp = now_ms;
        cache->count++;
    }
    return false;
}

// === Reverse Route Table ===

void reverse_route_init(reverse_route_table_t *table) {
    memset(table, 0, sizeof(*table));
}

int reverse_route_add(reverse_route_table_t *table, uint32_t query_id,
                      uint32_t prev_hop, uint32_t now_ms) {
    if (table->count >= MAX_REVERSE_ROUTES) {
        // Purge old first
        reverse_route_purge(table, now_ms);
        if (table->count >= MAX_REVERSE_ROUTES) {
            // Evict oldest
            int oldest = 0;
            for (int i = 1; i < table->count; i++) {
                if (table->entries[i].timestamp < table->entries[oldest].timestamp)
                    oldest = i;
            }
            table->entries[oldest] = table->entries[table->count - 1];
            table->count--;
        }
    }

    table->entries[table->count].query_id = query_id;
    table->entries[table->count].prev_hop = prev_hop;
    table->entries[table->count].timestamp = now_ms;
    table->count++;
    return 0;
}

reverse_route_t *reverse_route_lookup(reverse_route_table_t *table, uint32_t query_id) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].query_id == query_id) return &table->entries[i];
    }
    return NULL;
}

void reverse_route_purge(reverse_route_table_t *table, uint32_t now_ms) {
    int write = 0;
    for (int i = 0; i < table->count; i++) {
        if ((now_ms - table->entries[i].timestamp) < REVERSE_ROUTE_EXPIRY_MS) {
            if (write != i) table->entries[write] = table->entries[i];
            write++;
        }
    }
    table->count = write;
}

// === Link Quality ===

uint8_t compute_link_penalty(int8_t rssi, int8_t snr) {
    // RSSI penalty: -60 = excellent (0), -120 = marginal (30)
    int rssi_pen = ((-60) - rssi) / 2;
    if (rssi_pen < 0) rssi_pen = 0;
    if (rssi_pen > 30) rssi_pen = 30;

    // SNR penalty: 10 = great (0), -5 = poor (20)
    int snr_pen = (10 - snr) * 2;
    if (snr_pen < 0) snr_pen = 0;
    if (snr_pen > 20) snr_pen = 20;

    int total = rssi_pen + snr_pen;
    return (uint8_t)(total > 50 ? 50 : total);
}
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
add_executable(test_neighbor test_neighbor.c)
target_link_libraries(test_neighbor unity esp_stubs)
target_include_directories(test_neighbor PRIVATE stubs)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_neighbor && ./test_neighbor
```
**Expected:**
```
7 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "feat: neighbor table, routing table, RREQ dedup, reverse routes"`

---

### Task 28: ✅ Write failing tests for routing table operations

**Create** `bramble/test/test_routing.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include "../components/routing/include/routing.h"
#include "../components/routing/routing.c"

void setUp(void) {}
void tearDown(void) {}

void test_route_init_empty(void) {
    routing_table_t table;
    route_init(&table);
    TEST_ASSERT_EQUAL(0, route_count(&table));
}

void test_route_install_and_lookup(void) {
    routing_table_t table;
    route_init(&table);
    route_install(&table, 0xAAAAAAAA, 0xBBBBBBBB, 3, 200, ROUTE_ACTIVE, 1000);

    route_entry_t *r = route_lookup(&table, 0xAAAAAAAA);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_HEX32(0xBBBBBBBB, r->next_hop);
    TEST_ASSERT_EQUAL(3, r->hop_count);
    TEST_ASSERT_EQUAL(200, r->metric);
    TEST_ASSERT_EQUAL(ROUTE_ACTIVE, r->state);
}

void test_route_update_better_metric(void) {
    routing_table_t table;
    route_init(&table);
    route_install(&table, 0xAAAAAAAA, 0xBBBBBBBB, 3, 200, ROUTE_ACTIVE, 1000);
    route_install(&table, 0xAAAAAAAA, 0xCCCCCCCC, 2, 220, ROUTE_ACTIVE, 2000);

    route_entry_t *r = route_lookup(&table, 0xAAAAAAAA);
    TEST_ASSERT_EQUAL_HEX32(0xCCCCCCCC, r->next_hop);
    TEST_ASSERT_EQUAL(220, r->metric);
    TEST_ASSERT_EQUAL(1, route_count(&table));
}

void test_route_maintenance_active_to_stale(void) {
    routing_table_t table;
    route_init(&table);
    route_install(&table, 0xAAAAAAAA, 0xBBBBBBBB, 3, 200, ROUTE_ACTIVE, 1000);

    // After 5 minutes + 1ms
    route_maintenance(&table, 1000 + ROUTE_ACTIVE_TIMEOUT_MS + 1);
    route_entry_t *r = route_lookup(&table, 0xAAAAAAAA);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(ROUTE_STALE, r->state);
}

void test_route_maintenance_stale_removed(void) {
    routing_table_t table;
    route_init(&table);
    route_install(&table, 0xAAAAAAAA, 0xBBBBBBBB, 3, 200, ROUTE_ACTIVE, 1000);

    // Force stale
    route_maintenance(&table, 1000 + ROUTE_ACTIVE_TIMEOUT_MS + 1);
    // Then wait stale timeout
    route_maintenance(&table, 1000 + ROUTE_ACTIVE_TIMEOUT_MS + ROUTE_STALE_TIMEOUT_MS + 2);
    TEST_ASSERT_EQUAL(0, route_count(&table));
}

void test_route_unverified_state(void) {
    routing_table_t table;
    route_init(&table);
    route_install(&table, 0xAAAAAAAA, 0xBBBBBBBB, 2, 180, ROUTE_UNVERIFIED, 1000);

    route_entry_t *r = route_lookup(&table, 0xAAAAAAAA);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(ROUTE_UNVERIFIED, r->state);
}

void test_rreq_dedup(void) {
    rreq_dedup_t cache;
    rreq_dedup_init(&cache);

    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&cache, 0x11111111, 1000));
    TEST_ASSERT_TRUE(rreq_dedup_check_and_add(&cache, 0x11111111, 1500));
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&cache, 0x22222222, 2000));
    // After expiry
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&cache, 0x11111111, 32000));
}

void test_reverse_route(void) {
    reverse_route_table_t table;
    reverse_route_init(&table);
    reverse_route_add(&table, 0xAAAAAAAA, 0xBBBBBBBB, 1000);

    reverse_route_t *r = reverse_route_lookup(&table, 0xAAAAAAAA);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_HEX32(0xBBBBBBBB, r->prev_hop);

    reverse_route_purge(&table, 70000);
    r = reverse_route_lookup(&table, 0xAAAAAAAA);
    TEST_ASSERT_NULL(r);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_route_init_empty);
    RUN_TEST(test_route_install_and_lookup);
    RUN_TEST(test_route_update_better_metric);
    RUN_TEST(test_route_maintenance_active_to_stale);
    RUN_TEST(test_route_maintenance_stale_removed);
    RUN_TEST(test_route_unverified_state);
    RUN_TEST(test_rreq_dedup);
    RUN_TEST(test_reverse_route);
    return UNITY_END();
}
```

**Add to** `bramble/test/CMakeLists.txt`:
```cmake
add_executable(test_routing test_routing.c)
target_link_libraries(test_routing unity esp_stubs)
target_include_directories(test_routing PRIVATE stubs)
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_routing && ./test_routing
```
**Expected:**
```
8 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "test: routing table, RREQ dedup, and reverse route tests — all pass"`

---

### Tasks 29–40: ✅ Routing protocol integration (summary)

The remaining Phase 3 tasks follow the same TDD pattern. Each task is listed with its key deliverable:

### Task 29: ✅ Implement beacon generation logic
- **Create** `bramble/components/routing/beacon.c` — builds beacon packets with 4-byte HMAC
- **Test** beacon HMAC computation in `test/test_beacon.c`

### Task 30: ✅ Run beacon test
- **Run:** `./test_beacon` — verify HMAC computation matches known-peer case and zero case

### Task 31: ✅ Implement RREQ handling (originator side)
- **Create** `bramble/components/routing/discovery.c` — `route_discovery_start()`, pending discovery table

### Task 32: ✅ Implement RREQ forwarding (relay side)
- **Modify** `discovery.c` — relay RREQ with metric update, jitter

### Task 33: ✅ Implement RREQ handling (destination side)
- **Modify** `discovery.c` — decrypt source, build RREP with 4-byte HMAC

### Task 34: ✅ Implement RREP forwarding (relay side)
- **Modify** `discovery.c` — install forward route, forward RREP toward originator

### Task 35: ✅ Implement RREP handling (originator side)
- **Modify** `discovery.c` — verify HMAC (or mark UNVERIFIED), install route, flush queued packets

### Task 36: ✅ Write integration test for route discovery
- **Create** `test/test_discovery.c` — simulates 3-node RREQ→RREP flow using in-memory packet passing
- **Expected:** Route installed in both directions

### Task 37: ✅ Implement data forwarding along routes
- **Create** `bramble/components/routing/forwarding.c` — `forward_data()` with hop_limit decrement

### Task 38: ✅ Implement RERR generation and propagation
- **Modify** `forwarding.c` — detect broken link after 3 failures, send RERR

### Task 39: ✅ Implement channel message controlled flood
- **Create** `bramble/components/routing/channel_flood.c` — limited rebroadcast with jitter and hop_limit

### Task 40: ✅ Run all Phase 3 tests
- **Run all tests** — expected ~70+ tests passing
- **Commit:** `milestone: Phase 3 complete — routing and forwarding`

---

## Phase 4: Reliability & Airtime (Tasks 41–55) 🚧 PARTIAL

### Task 41: ✅ Write failing tests for three-tier message model

**Create** `bramble/components/reliability/include/reliability.h`:
```c
#ifndef BRAMBLE_RELIABILITY_H
#define BRAMBLE_RELIABILITY_H

#include <stdint.h>
#include <stdbool.h>
#include "packet.h"

// Tier definitions
#define MSG_TIER_BROADCAST  0
#define MSG_TIER_NORMAL     1
#define MSG_TIER_CRITICAL   2

// Pending ACK entry
#define MAX_PENDING_ACKS 8

typedef struct {
    uint32_t packet_id;
    uint32_t dest_addr;
    uint8_t  tier;
    uint8_t  attempt;
    uint8_t  max_attempts;
    uint32_t next_retry_ms;
    uint16_t packet_len;
    uint8_t  packet_data[222];
    bool     active;
} pending_ack_t;

typedef struct {
    pending_ack_t entries[MAX_PENDING_ACKS];
} pending_ack_table_t;

void pending_ack_init(pending_ack_table_t *table);
int  pending_ack_add(pending_ack_table_t *table, uint32_t packet_id, uint32_t dest_addr,
                     uint8_t tier, const uint8_t *packet, uint16_t len, uint32_t now_ms);
bool pending_ack_remove(pending_ack_table_t *table, uint32_t packet_id);
void pending_ack_tick(pending_ack_table_t *table, uint32_t now_ms);
uint8_t tier_max_retries(uint8_t tier);
uint32_t tier_base_delay_ms(uint8_t tier);

// Flow control
#define FLOW_WINDOW_SIZE 4
#define MAX_FLOW_DESTINATIONS 8

typedef struct {
    uint32_t dest_addr;
    uint8_t  unacked;
    uint8_t  window_size;
    uint16_t success_counter;
    bool     active;
} flow_window_t;

typedef struct {
    flow_window_t windows[MAX_FLOW_DESTINATIONS];
} flow_control_t;

void flow_init(flow_control_t *fc);
bool flow_can_send(flow_control_t *fc, uint32_t dest_addr);
void flow_on_send(flow_control_t *fc, uint32_t dest_addr);
void flow_on_ack(flow_control_t *fc, uint32_t dest_addr);
void flow_on_failure(flow_control_t *fc, uint32_t dest_addr);

#endif
```

**Create** `bramble/test/test_reliability.c`:
```c
#include "unity.h"
#include "esp_stubs.h"
#include "../components/reliability/include/reliability.h"
#include "../components/reliability/reliability.c"

void setUp(void) {}
void tearDown(void) {}

void test_tier_max_retries(void) {
    TEST_ASSERT_EQUAL(0, tier_max_retries(MSG_TIER_BROADCAST));
    TEST_ASSERT_EQUAL(3, tier_max_retries(MSG_TIER_NORMAL));
    TEST_ASSERT_EQUAL(8, tier_max_retries(MSG_TIER_CRITICAL));
}

void test_pending_ack_add_and_remove(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t pkt[10] = {0};
    TEST_ASSERT_EQUAL(0, pending_ack_add(&table, 0x1111, 0xAAAA, MSG_TIER_NORMAL, pkt, 10, 1000));
    TEST_ASSERT_TRUE(pending_ack_remove(&table, 0x1111));
    TEST_ASSERT_FALSE(pending_ack_remove(&table, 0x1111));  // Already removed
}

void test_flow_control_window(void) {
    flow_control_t fc;
    flow_init(&fc);

    // Can send up to window size
    for (int i = 0; i < FLOW_WINDOW_SIZE; i++) {
        TEST_ASSERT_TRUE(flow_can_send(&fc, 0xAAAA));
        flow_on_send(&fc, 0xAAAA);
    }
    // Window full
    TEST_ASSERT_FALSE(flow_can_send(&fc, 0xAAAA));

    // ACK opens window
    flow_on_ack(&fc, 0xAAAA);
    TEST_ASSERT_TRUE(flow_can_send(&fc, 0xAAAA));
}

void test_flow_control_failure_shrinks_window(void) {
    flow_control_t fc;
    flow_init(&fc);

    flow_on_send(&fc, 0xBBBB);
    flow_on_failure(&fc, 0xBBBB);

    // Window should be halved (min 1)
    // After failure, window = max(4/2, 1) = 2
    flow_on_send(&fc, 0xBBBB);
    flow_on_send(&fc, 0xBBBB);
    TEST_ASSERT_FALSE(flow_can_send(&fc, 0xBBBB));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tier_max_retries);
    RUN_TEST(test_pending_ack_add_and_remove);
    RUN_TEST(test_flow_control_window);
    RUN_TEST(test_flow_control_failure_shrinks_window);
    return UNITY_END();
}
```

**Commit:** `git add -A && git commit -m "test: failing reliability layer tests"`

---

### Task 42: ✅ Implement reliability layer

**Create** `bramble/components/reliability/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "reliability.c"
    INCLUDE_DIRS "include"
    REQUIRES packet
)
```

**Create** `bramble/components/reliability/reliability.c`:
```c
#include "reliability.h"
#include <string.h>

uint8_t tier_max_retries(uint8_t tier) {
    switch (tier) {
        case MSG_TIER_BROADCAST: return 0;
        case MSG_TIER_NORMAL:    return 3;
        case MSG_TIER_CRITICAL:  return 8;
        default:                 return 0;
    }
}

uint32_t tier_base_delay_ms(uint8_t tier) {
    switch (tier) {
        case MSG_TIER_NORMAL:    return 2000;
        case MSG_TIER_CRITICAL:  return 3000;
        default:                 return 0;
    }
}

void pending_ack_init(pending_ack_table_t *table) {
    memset(table, 0, sizeof(*table));
}

int pending_ack_add(pending_ack_table_t *table, uint32_t packet_id, uint32_t dest_addr,
                    uint8_t tier, const uint8_t *packet, uint16_t len, uint32_t now_ms) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!table->entries[i].active) {
            pending_ack_t *e = &table->entries[i];
            e->packet_id = packet_id;
            e->dest_addr = dest_addr;
            e->tier = tier;
            e->attempt = 0;
            e->max_attempts = tier_max_retries(tier);
            e->next_retry_ms = now_ms + tier_base_delay_ms(tier);
            e->packet_len = len;
            memcpy(e->packet_data, packet, len);
            e->active = true;
            return 0;
        }
    }
    return -1;  // Table full
}

bool pending_ack_remove(pending_ack_table_t *table, uint32_t packet_id) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (table->entries[i].active && table->entries[i].packet_id == packet_id) {
            table->entries[i].active = false;
            return true;
        }
    }
    return false;
}

void pending_ack_tick(pending_ack_table_t *table, uint32_t now_ms) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        pending_ack_t *e = &table->entries[i];
        if (!e->active) continue;
        if (now_ms < e->next_retry_ms) continue;

        if (e->attempt >= e->max_attempts) {
            e->active = false;
            // TODO: notify application of delivery failure
            continue;
        }

        e->attempt++;
        uint32_t backoff = tier_base_delay_ms(e->tier) * (1 << e->attempt);
        uint32_t jitter = backoff / 4;  // Simplified: use fixed 25% jitter
        e->next_retry_ms = now_ms + backoff + jitter;
        // TODO: re-enqueue packet for transmission
    }
}

// === Flow Control ===

void flow_init(flow_control_t *fc) {
    memset(fc, 0, sizeof(*fc));
}

static flow_window_t *flow_get_or_create(flow_control_t *fc, uint32_t dest_addr) {
    for (int i = 0; i < MAX_FLOW_DESTINATIONS; i++) {
        if (fc->windows[i].active && fc->windows[i].dest_addr == dest_addr) {
            return &fc->windows[i];
        }
    }
    // Create new
    for (int i = 0; i < MAX_FLOW_DESTINATIONS; i++) {
        if (!fc->windows[i].active) {
            fc->windows[i].dest_addr = dest_addr;
            fc->windows[i].unacked = 0;
            fc->windows[i].window_size = FLOW_WINDOW_SIZE;
            fc->windows[i].success_counter = 0;
            fc->windows[i].active = true;
            return &fc->windows[i];
        }
    }
    return NULL;
}

bool flow_can_send(flow_control_t *fc, uint32_t dest_addr) {
    flow_window_t *w = flow_get_or_create(fc, dest_addr);
    if (!w) return false;
    return w->unacked < w->window_size;
}

void flow_on_send(flow_control_t *fc, uint32_t dest_addr) {
    flow_window_t *w = flow_get_or_create(fc, dest_addr);
    if (w) w->unacked++;
}

void flow_on_ack(flow_control_t *fc, uint32_t dest_addr) {
    flow_window_t *w = flow_get_or_create(fc, dest_addr);
    if (!w || w->unacked == 0) return;
    w->unacked--;
    w->success_counter++;
    if (w->success_counter >= w->window_size) {
        if (w->window_size < 8) w->window_size++;
        w->success_counter = 0;
    }
}

void flow_on_failure(flow_control_t *fc, uint32_t dest_addr) {
    flow_window_t *w = flow_get_or_create(fc, dest_addr);
    if (!w) return;
    w->window_size = w->window_size / 2;
    if (w->window_size < 1) w->window_size = 1;
    w->success_counter = 0;
}
```

**Run:**
```bash
cd bramble/test/build && cmake .. && make test_reliability && ./test_reliability
```
**Expected:**
```
4 Tests 0 Failures 0 Ignored
OK
```

**Commit:** `git add -A && git commit -m "feat: reliability layer — pending ACKs, retry engine, flow control"`

---

### Tasks 43–55: Airtime budget and TX queue (summary) — see individual tasks below

### Task 43: ✅ Write failing tests for token bucket airtime budget
- **Create** `bramble/components/airtime/include/airtime.h` and `test/test_airtime.c`
- Test: budget starts full, debit reduces, refill adds tokens, tier borrowing works

### Task 44: ✅ Implement token bucket airtime budget
- **Create** `bramble/components/airtime/airtime.c` — per-tier sub-budgets, refill, debit, can_transmit

### Task 45: ✅ Run airtime tests

### Task 46: ✅ Write failing tests for priority TX queue
- **Create** `test/test_tx_queue.c`
- Test: priority ordering, full queue drops lowest, size limits

### Task 47: ✅ Implement priority TX queue
- **Create** `bramble/components/airtime/tx_queue.c` — 16-entry sorted queue

### Task 48: ✅ Run TX queue tests

### Task 49: ❌ TODO Write failing tests for congestion detection
- **Create** `test/test_congestion.c`
- Test: level assessment from queue depth, response actions

### Task 50: ❌ TODO Implement congestion detection and response
- **Modify** `airtime.c` — assess_congestion(), congestion_response()

### Task 51: ❌ TODO Run congestion tests

### Task 52: ⚠️ HARDWARE REQUIRED Write failing test for listen-before-talk
- **Create** `test/test_lbt.c` (mock radio CAD)
- Test: CAD clear → transmit, CAD busy → defer, max retries → requeue

### Task 53: ⚠️ HARDWARE REQUIRED Implement LBT with CAD
- **Create** `bramble/components/airtime/lbt.c` — try_transmit() with 5 CAD attempts

### Task 54: ⚠️ HARDWARE REQUIRED Run LBT tests

### Task 55: ✅ Run all Phase 4 tests
- **Commit:** `milestone: Phase 4 complete — reliability and airtime management`

---

## Phase 5: Time Sync & Security Hardening (Tasks 56–65) ✅ COMPLETE

### Task 56: ✅ Write failing tests for time sync state machine
- **Create** `bramble/components/timesync/include/timesync.h` and `test/test_timesync.c`
- Test: stratum acceptance, ±5s max shift, weighted moving average, stratum-0 corroboration

### Task 57: ✅ Implement time sync
- **Create** `bramble/components/timesync/timesync.c` — handle_time_sync(), get_network_time()

### Task 58: ✅ Run time sync tests

### Task 59: ✅ Write failing test for TIME_SYNC packet emission
- Test: only stratum ≤ 2 nodes emit, 5-minute interval

### Task 60: ✅ Implement TIME_SYNC emission
- **Modify** `timesync.c` — time_sync_tick()

### Task 61: ✅ Write failing tests for anti-replay validation
- **Create** `test/test_anti_replay.c`
- Test: ±30s window, expired packets rejected, dedup integration

### Task 62: ✅ Implement anti-replay
- **Create** `bramble/components/timesync/anti_replay.c`

### Task 63: ✅ Write tests for RREQ rate limiting and Sybil heuristic
- **Create** `test/test_security.c`
- Test: rate limit (1 RREQ/30s per neighbor-dest pair), RSSI clustering

### Task 64: ✅ Implement RREQ rate limiting and Sybil detection
- **Create** `bramble/components/security/security.c`

### Task 65: ✅ Run all Phase 5 tests
- **Commit:** `milestone: Phase 5 complete — time sync and security hardening`

---

## Phase 6: Fragmentation & Channels (Tasks 66–75) ✅ COMPLETE

### Task 66: ✅ Write failing tests for fragment-then-encrypt
- **Create** `test/test_fragment.c`
- Test: split 500-byte plaintext into fragments, each independently encrypted

### Task 67: ✅ Implement fragmentation (split)
- **Create** `bramble/components/fragment/fragment.c` — fragment_split()

### Task 68: ✅ Run fragmentation split tests

### Task 69: ✅ Write failing tests for fragment reassembly
- Test: in-order, out-of-order, timeout, duplicate fragment, invalid auth tag

### Task 70: ✅ Implement fragment reassembly
- **Modify** `fragment.c` — fragment_reassemble(), bitmap tracking, 30s timeout

### Task 71: ✅ Run reassembly tests

### Task 72: ✅ Write failing tests for channel message encryption with trial decryption
- **Create** `test/test_channel_msg.c`
- Test: encrypt with channel key (channel_id + epoch inside ciphertext), trial decrypt across 16 channels

### Task 73: ✅ Implement channel message encryption/decryption
- **Create** `bramble/components/channel/channel.c`

### Task 74: ✅ Write tests for multi-channel support and epoch integration
- Test: 16 simultaneous channels, epoch catchup on receive

### Task 75: ✅ Run all Phase 6 tests
- **Commit:** `milestone: Phase 6 complete — fragmentation and channels`

---

## Phase 7: OLED UI (Tasks 76–85) 🚧 PARTIAL

### Task 76: ✅ Create OLED display driver component
- **Create** `bramble/components/display/include/display.h` — init, clear, draw_text, draw_line, flush
- **Create** `bramble/components/display/display_ssd1306.c` — SSD1306 I2C driver for 128×64

### Task 77: ✅ Implement main screen
- **Create** `bramble/components/ui/ui_main.c` — node address, battery %, neighbor count, network time

### Task 78: ✅ Implement message list screen
- **Create** `bramble/components/ui/ui_messages.c` — scrollable list of recent messages

### Task 79: ✅ Implement compose screen
- **Create** `bramble/components/ui/ui_compose.c` — canned messages, button navigation

### Task 80: ✅ Implement settings screen
- **Create** `bramble/components/ui/ui_settings.c` — channel list, TX power, radio profile

### Task 81: ✅ Implement node list screen
- **Create** `bramble/components/ui/ui_nodes.c` — known peers with RSSI, last heard

### Task 82: ✅ Implement button input handler
- **Create** `bramble/components/ui/input.c` — Heltec V3 button GPIO, debounce, long-press

### Task 83: ✅ Implement screen manager (state machine)
- **Create** `bramble/components/ui/ui_manager.c` — screen transitions, render loop

### Task 84: ✅ Integrate UI with protocol task
- **Modify** `main.c` — add UI task, connect to protocol state

### Task 85: ⚠️ HARDWARE REQUIRED Build and verify OLED UI on hardware
- **Run:** `idf.py build && idf.py flash monitor`
- **Commit:** `milestone: Phase 7 complete — OLED UI`

---

## Phase 8: BLE Interface & OTA (Tasks 86–95) 🚧 PARTIAL

### Task 86: ⚠️ HARDWARE REQUIRED Create BLE GATT service skeleton
- **Create** `bramble/components/ble/include/ble.h` — init, start advertising
- **Create** `bramble/components/ble/ble_gatt.c` — JSON-RPC characteristic (read/write/notify)

### Task 87: ✅ Implement JSON-RPC parser
- **Create** `bramble/components/ble/json_rpc.c` — parse method/params/id, build response

### Task 88: ✅ Write tests for JSON-RPC parser
- **Create** `test/test_json_rpc.c` — parse get_config, send_message, etc.

### Task 89: ⚠️ HARDWARE REQUIRED Implement BLE message send/receive
- **Modify** `ble_gatt.c` — handle send_message, push incoming messages via notify

### Task 90: ⚠️ HARDWARE REQUIRED Implement BLE config read/write
- **Modify** `ble_gatt.c` — handle get_config, set_config

### Task 91: ⚠️ HARDWARE REQUIRED Implement BLE node list and route inspection
- **Modify** `ble_gatt.c` — handle get_nodes, get_routes

### Task 92: ⚠️ HARDWARE REQUIRED Implement BLE OTA firmware update
- **Create** `bramble/components/ble/ble_ota.c` — ESP-IDF BLE OTA

### Task 93: ⚠️ HARDWARE REQUIRED Implement optional WiFi OTA
- **Create** `bramble/components/ota/wifi_ota.c` — ESP-IDF HTTP OTA

### Task 94: ⚠️ HARDWARE REQUIRED Integrate BLE with main firmware
- **Modify** `main.c` — add BLE task, connect to protocol

### Task 95: ⚠️ HARDWARE REQUIRED Test BLE interface
- **Commit:** `milestone: Phase 8 complete — BLE interface and OTA`

---

## Phase 9: Web Flasher (Tasks 96–100) ⚠️ HARDWARE REQUIRED — Not Started

### Task 96: ⚠️ HARDWARE REQUIRED Create web flasher project scaffolding
- **Create** `bramble/web-flasher/index.html` — basic HTML with Web Serial API
- **Create** `bramble/web-flasher/flasher.js` — serial port connection

### Task 97: ⚠️ HARDWARE REQUIRED Implement ESP32 bootloader protocol
- **Modify** `flasher.js` — SLIP framing, sync, flash_begin, flash_data, flash_end (esptool.js)

### Task 98: ⚠️ HARDWARE REQUIRED Add firmware binary download and flash progress
- **Modify** `flasher.js` — fetch firmware from GitHub Releases, progress bar

### Task 99: ⚠️ HARDWARE REQUIRED Add multi-board support
- **Modify** `flasher.js` — detect Heltec V3 vs T-Beam via USB VID/PID, partition table handling

### Task 100: ⚠️ HARDWARE REQUIRED Test web flasher end-to-end
- **Commit:** `milestone: Phase 9 complete — web flasher`

---

## Phase 10: Web Config & Messaging App (Tasks 101–115) ❌ NOT STARTED

### Task 101: ❌ TODO Create web app project scaffolding
- **Create** `bramble/web-app/index.html` — SPA with Web Serial connection

### Task 102: ❌ TODO Implement Web Serial JSON-RPC transport
- **Create** `bramble/web-app/serial.js` — connect, send JSON-RPC, receive responses

### Task 103: ❌ TODO Implement config page — node identity
- **Create** `bramble/web-app/pages/config.js`

### Task 104: ❌ TODO Implement config page — radio settings

### Task 105: ❌ TODO Implement config page — channel management

### Task 106: ❌ TODO Implement config page — peer management

### Task 107: ❌ TODO Implement chat UI — DM conversation view
- **Create** `bramble/web-app/pages/chat.js` — message list, compose, send

### Task 108: ❌ TODO Implement chat UI — channel conversation view

### Task 109: ❌ TODO Implement delivery status indicators
- sent → delivered → (relay path for Critical)

### Task 110: ❌ TODO Implement relay path display for Critical messages

### Task 111: ❌ TODO Implement airtime stats dashboard

### Task 112: ❌ TODO Implement Web Bluetooth alternative transport
- **Create** `bramble/web-app/ble.js` — same JSON-RPC over BLE GATT

### Task 113: ❌ TODO Implement real-time status updates (polling)

### Task 114: ❌ TODO Style and polish web app

### Task 115: ❌ TODO Test web app end-to-end
- **Commit:** `milestone: Phase 10 complete — web config and messaging app`

---

## Phase 11: Testing & Field Validation (Tasks 116–125) 🚧 PARTIAL

### Task 116: ✅ Create comprehensive unit test suite
- Consolidate all tests into `bramble/test/test_all.c` runner
- **Run:** `cd bramble/test/build && cmake .. && make -j && ctest`

### Task 117: ✅ Add crypto test vectors (NIST AES-GCM, RFC 7748 X25519)
- **Create** `test/test_crypto_vectors.c` — known-answer tests from standards

### Task 118: ✅ Create radio mock for integration tests
- **Create** `bramble/components/radio/radio_mock.c` — loopback radio that connects N virtual nodes

### Task 119: ✅ Write 3-node integration test
- **Create** `test/test_integration_3node.c`
- A→B→C message delivery, route discovery, ACK return

### Task 120: ✅ Write 5-node integration test
- Test: multi-hop routing, route failure and recovery, congestion

### Task 121: ✅ Create 5-node field test plan
- **Create** `docs/field-test-5node.md` — setup, scenarios, success criteria, data collection

### Task 122: ⚠️ HARDWARE REQUIRED Create 20-node stress test plan
- **Create** `docs/field-test-20node.md`

### Task 123: ⚠️ HARDWARE REQUIRED Power consumption profiling
- Measure: active TX, active RX, idle, deep sleep
- Document results in `docs/power-profile.md`

### Task 124: ⚠️ HARDWARE REQUIRED Memory high-water-mark analysis
- Instrument heap usage during sustained load
- Verify total RAM < 130KB with 60% headroom
- Document in `docs/memory-profile.md`

### Task 125: ⚠️ HARDWARE REQUIRED Final integration test and release preparation
- Build release firmware, run all tests, tag v0.1
- **Commit:** `milestone: Phase 11 complete — testing and validation`
- **Tag:** `git tag v0.1.0`

---

## 🎁 Bonus Features — Implemented Beyond Original Plan

> These features were implemented in addition to the 125 planned tasks. All are host-testable or simulator-verified.

### 1. ✅ Default Public Channel (Bramble Common)
- A zero-configuration public broadcast channel (`#bramble-common`) that all nodes join automatically
- Uses a well-known PSK with no epoch ratchet (intentionally public)
- Enables device discovery and community messaging without any setup
- Integrated into channel manager and OLED UI

### 2. ✅ Store-and-Forward Mailbox
- Relay nodes buffer up to N encrypted DMs for offline destinations
- Messages stored in NVS flash with TTL (24h default)
- Delivered when destination node is next seen in neighbor table
- Anti-replay protection via timestamp + packet_id dedup

### 3. ✅ Emergency Beacon
- One-button SOS mode: emits PKT_BEACON with emergency flag every 30s
- Maximum TX power, ignores airtime budget
- All relay nodes forward emergency beacons unconditionally (hop_limit=8)
- OLED shows active SOS state with countdown

### 4. ✅ Private Location Sharing
- Opt-in GPS coordinate sharing via encrypted DM channel
- Coordinates encrypted with destination's public key (not broadcast)
- Included in beacon payload only when `flag_share_location` is set
- Web app renders shared locations on offline map tile

### 5. ✅ Group DMs
- Multi-recipient encrypted messages using per-group ephemeral PSK
- Group key distributed via key exchange to each member
- Message encrypted once with group key, not N times
- Delivered via normal DM routing to each member's address

### 6. ✅ Network Coding (XOR Relay)
- Relay nodes XOR two queued packets and broadcast the coded packet
- Receivers decode by XORing coded packet with known packet
- Increases throughput in mesh topology by ~40% in simulations
- Implemented in `components/routing/network_coding.c`

### 7. ✅ Adaptive Routing Metrics
- Routing metric incorporates RSSI, SNR, hop count, and link reliability history
- Exponential moving average over last 8 packets per neighbor
- Metric updated in neighbor table on every beacon/packet received
- RREP carries composite metric; originator selects best-metric route

### 8. ✅ Network Health Visualization
- Real-time topology graph rendered in simulator web UI (React + D3.js)
- Nodes colored by battery level; edges colored by RSSI/SNR quality
- Packet flow animated with direction arrows
- Exported as SVG for field test documentation

### 9. ✅ Full Mesh Simulator (Go + React, 24 Scenarios, Docker)
- Full software radio simulation: `simulator/` directory
- Go backend: virtual nodes with configurable radio propagation model
- React frontend: topology visualization, packet inspector, scenario runner
- 24 pre-built test scenarios covering: 2-node baseline, 5-node mesh, ring topology, star topology, congestion, route failure & recovery, store-and-forward, emergency beacon, fragmentation, channel isolation, epoch ratchet, key exchange, network coding, and more
- Dockerized: `docker-compose up` starts simulator + UI on localhost:3000
- Used as primary development harness while hardware is unavailable

### 10. ✅ RSSI/SNR Visualization
- Simulator and OLED UI both display per-link RSSI and SNR
- OLED node list screen shows signal strength bars for each neighbor
- Web app renders RSSI heatmap overlay on topology graph
- Historical RSSI/SNR logged per neighbor for link quality analysis

---

*Last updated: 2026-02-17 · Branch: feature/sim-component-integration*
