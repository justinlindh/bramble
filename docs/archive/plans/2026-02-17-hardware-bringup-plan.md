# Bramble Hardware Bringup Plan

**Date:** 2026-02-17
**Target:** Heltec WiFi LoRa 32 V3 × 2
**Goal:** Two-node mesh communicating over LoRa with OLED status and webapp connectivity

## Status (2026-02-17 18:13 PST)
- ✅ Phase 1A: Frequency Plan (US915/EU868/AU915 + Kconfig)
- ✅ Phase 1B: Identity persistence (X25519 keypair in NVS)
- ✅ Phase 2: SX1262 radio driver (SPI, TX/RX, DIO1 ISR)
- ✅ Phase 3: Beacon TX/RX + neighbor discovery — **TWO NODES COMMUNICATING**
  - Board 1: 1191C6E0, Board 2: 6EEA8967
  - RSSI: -40/-44 dBm, SNR: 10 (excellent signal, boards on same desk)
- ✅ Phase 4: Packet dispatch & routing
- ✅ Phase 5: WiFi + WebSocket server
- ✅ Phase 6: Two-node mesh end-to-end messaging
- ✅ Phase 7: Polish (battery, sleep, OTA, 8MB flash)

---

## Phase 1A: Frequency Plan System ✅ COMPLETE
*Estimated: 30 min*

Regulatory-compliant radio configuration with per-region frequency plans.

### Task 1A.1: Create freq_plan component
- New `components/freq_plan/` with `freq_plan.h` and `freq_plan.c`
- `bramble_freq_plan_t` struct: name, freq range, default freq, max TX power, duty cycle limits, enforcement flag
- Predefined plans: `FREQ_PLAN_US915`, `FREQ_PLAN_EU868`, `FREQ_PLAN_AU915`
- `freq_plan_get_default()` returns compile-time default (Kconfig)
- `freq_plan_validate_tx()` — checks freq bounds + TX power cap before any transmission
- `freq_plan_get_duty_cycle_pct()` — returns max duty cycle (100=unlimited for US, 1 for EU)
- **Kconfig:** `BRAMBLE_REGION` choice (US915/EU868/AU915) in `main/Kconfig.projbuild`
- **Files:** `components/freq_plan/include/freq_plan.h`, `components/freq_plan/freq_plan.c`, `components/freq_plan/CMakeLists.txt`

### Task 1A.2: Wire freq_plan into radio and airtime
- `radio_init()` reads freq plan for frequency + TX power
- `airtime_can_send()` respects freq plan duty cycle
- Future: NVS runtime override, webapp Config page selection

---

## Phase 1B: Identity & NVS Persistence ✅ COMPLETE
*Estimated: 30 min*

The `identity` component already has NVS load/save. Wire it into main.c.

### Task 1B.1: Wire identity into main.c
- Load identity from NVS on boot; generate + save if first boot
- Display real address (derived from X25519 pubkey) on OLED
- Log public key hash for debugging
- **Files:** `main/main.c`

### Task 1B.2: Verify NVS persistence across reboots
- Flash, note address, reset, confirm same address
- **Validation:** Address stable across 3 reboots

---

## Phase 2: SX1262 LoRa Radio Driver ✅ COMPLETE
*Estimated: 3–4 hours*

This is the big one. The SX1262 is an SPI peripheral with a complex command interface. We need a low-level driver that implements our existing `radio.h` interface.

### Heltec V3 SX1262 Pinout
| Function | GPIO |
|----------|------|
| SPI SCK  | 9    |
| SPI MISO | 11   |
| SPI MOSI | 10   |
| SPI NSS  | 8    |
| RESET    | 12   |
| BUSY     | 13   |
| DIO1     | 14   |

### Task 2.1: SX1262 SPI low-level driver
- Create `components/radio/sx1262.c` and `components/radio/include/sx1262.h`
- SPI master init (SPI2_HOST, 8MHz clock)
- Basic commands: `sx1262_write_command()`, `sx1262_read_command()`, `sx1262_write_register()`, `sx1262_read_register()`, `sx1262_write_buffer()`, `sx1262_read_buffer()`
- `sx1262_wait_busy()` — poll BUSY pin with timeout
- `sx1262_reset()` — toggle RST pin
- **Files:** `components/radio/sx1262.h`, `components/radio/sx1262.c`

### Task 2.2: SX1262 initialization sequence
- Set standby mode (STDBY_RC)
- Set packet type: LoRa
- Set RF frequency (915.0 MHz for US ISM band)
- Configure modulation: SF9, BW125kHz, CR4/5 (our LONG_RANGE profile)
- Set PA config: +17 dBm, PA duty cycle, HP max
- Set TX params: ramp time 200µs
- Set buffer base addresses
- Configure DIO1 for TxDone + RxDone + Timeout interrupts
- Clear IRQ status
- **Validation:** No SPI errors, chip responds to GetStatus

### Task 2.3: SX1262 transmit path
- `sx1262_transmit(data, len)`:
  1. Write payload to SX1262 FIFO buffer
  2. Set packet params (preamble, header type, payload len, CRC, IQ)
  3. Clear IRQ, set TX mode with timeout
  4. Wait for DIO1 interrupt (TxDone) or timeout
- Wire into `radio_transmit()` in new `radio_esp.c`
- **Validation:** TX LED blinks, spectrum analyzer (or second board) sees signal

### Task 2.4: SX1262 receive path
- `sx1262_start_rx()`:
  1. Set RX mode (continuous)
  2. On DIO1 interrupt → read IRQ status
  3. If RxDone: read buffer, get packet status (RSSI, SNR), call rx_callback
  4. If CRC error: discard, log
- DIO1 ISR → FreeRTOS task notification → read in task context (no SPI in ISR)
- Wire into `radio_start_rx()` and callback system
- **Validation:** Two boards can hear each other's raw packets

### Task 2.5: Implement radio_esp.c (radio.h interface)
- Implement all `radio_*()` functions for real hardware
- `radio_init()` → sx1262_init + configure
- `radio_transmit()` → sx1262_transmit
- `radio_start_rx()` → sx1262_start_rx (continuous mode)
- `radio_get_state()` → track state machine
- `radio_sleep()` → SX1262 sleep mode
- `radio_set_tx_power()` → reconfigure PA
- `radio_cad()` → Channel Activity Detection mode
- Conditional compilation: `#ifdef ESP_PLATFORM` uses real SPI, else mock
- **Files:** `components/radio/radio_esp.c`
- **CMakeLists:** Add `radio_esp.c` to SRCS (conditionally, only when ESP_PLATFORM)

### Task 2.6: DIO1 interrupt handler
- GPIO ISR on DIO1 (GPIO14) → gives semaphore/task notification
- Radio task wakes up, reads IRQ flags, dispatches:
  - TxDone → call tx_done_callback
  - RxDone → read buffer → call rx_callback with RSSI/SNR
  - Timeout → back to RX
  - CadDone → call cad_done_callback
- Use `gpio_install_isr_service()` + `gpio_isr_handler_add()`
- **Files:** Part of `radio_esp.c`

---

## Phase 3: Beacon TX/RX — First Over-the-Air Communication ✅ COMPLETE
*Estimated: 2–3 hours*

### Task 3.1: Create mesh task (FreeRTOS)
- New file `main/mesh_task.c` + `main/mesh_task.h`
- FreeRTOS task: `mesh_task(void *param)` on CPU1 (leave CPU0 for UI)
- Owns: identity, neighbor table, route table, security state
- Receives radio RX packets via callback → queue → task processes
- Timer-based beacon TX (every 30s initially, configurable)
- **Stack size:** 8KB (crypto operations need stack space)

### Task 3.2: Beacon transmission
- Build `bramble_beacon_t` from current state:
  - src_addr, pubkey_hash from identity
  - uptime, battery (ADC read), tx_queue_depth, neighbor_count
  - HMAC auth using identity private key
- Serialize → `radio_transmit()`
- Respect airtime budget (check `airtime_can_send()`)
- **Files:** `main/mesh_task.c`

### Task 3.3: Beacon reception & neighbor tracking
- Radio RX callback → parse header → if PKT_TYPE_BEACON:
  - Deserialize beacon
  - Verify HMAC (optional initially — can skip for bringup)
  - `neighbor_update()` with RSSI/SNR from radio_rx_info
  - Log: "New neighbor: AABBCCDD RSSI:-65 SNR:8"
- **Validation:** Board A sees Board B's beacons and vice versa

### Task 3.4: Update OLED with live neighbor data
- Main screen shows real neighbor count from neighbor table
- Nodes screen shows list of discovered neighbors with RSSI
- Add shared state (mutex-protected) between mesh task and UI task
- **Files:** `main/main.c`, `main/mesh_task.c`

---

## Phase 4: Packet Dispatch & Routing
*Estimated: 2–3 hours*

### Task 4.1: Packet receive dispatcher
- On radio RX: deserialize header → switch on type:
  - BEACON → handle_beacon()
  - ACK → handle_ack()
  - RREQ → handle_rreq()
  - RREP → handle_rrep()
  - RERR → handle_rerr()
  - DATA → handle_data()
  - KEY_EXCHANGE → handle_key_exchange()
  - DELIVERY_RECEIPT → handle_delivery_receipt()
  - etc.
- Dedup check before processing (existing `dedup` component)
- Hop limit decrement + forwarding logic
- **Files:** `main/mesh_task.c`

### Task 4.2: Route discovery (RREQ/RREP)
- Wire existing `routing` component:
  - On DATA send with no route → initiate RREQ
  - On RREQ receive → update route, forward or reply
  - On RREP receive → install route, forward toward originator
  - On RERR → invalidate route
- All the logic exists in `routing.c` — just needs to call `radio_transmit()`
- **Validation:** Board A discovers route to Board B (1 hop)

### Task 4.3: Data packet send/receive
- Wire `channel_msg_encrypt()` / `channel_msg_decrypt()` for encrypted messages
- Key exchange → shared secret → encrypted data packets
- Wire `reliability` component for ACK/retry
- **Validation:** Send text message A→B, see decrypted text in serial log

---

## Phase 5: WiFi + WebSocket Server
*Estimated: 3–4 hours*

### Task 5.1: WiFi Station mode
- Connect to home WiFi (credentials in NVS or `menuconfig`)
- DHCP, log IP address to OLED
- Use `esp_wifi` + `esp_event` (already in REQUIRES)
- **Files:** `main/wifi_task.c`, `main/wifi_task.h`

### Task 5.2: WebSocket server
- ESP-IDF `esp_http_server` with WebSocket support
- JSON-RPC 2.0 protocol (same as webapp mock server)
- Methods: `getNodeInfo`, `getNeighbors`, `getRoutes`, `sendMessage`, `getMessages`
- This is how the webapp connects to real hardware
- **Files:** `main/ws_server.c`, `main/ws_server.h`

### Task 5.3: WiFi AP fallback
- If station connect fails → start AP mode ("Bramble-XXXX")
- Captive portal for WiFi config (stretch goal)
- **Files:** Part of `wifi_task.c`

### Task 5.4: OLED shows WiFi status
- Connected: show IP address
- Disconnected: show "No WiFi"
- AP mode: show SSID + "192.168.4.1"

---

## Phase 6: Two-Node Mesh Testing
*Estimated: 2 hours*

### Task 6.1: Flash second Heltec V3
- Same firmware, different identity (auto-generated)
- Connect to same WiFi
- Both visible on OLED: neighbor count = 1

### Task 6.2: End-to-end message test
- Board A sends message to Board B via serial command or webapp
- Board B receives, decrypts, displays
- Delivery receipt comes back to Board A
- OLED shows message count

### Task 6.3: Webapp connects to real hardware
- Open webapp on phone/laptop
- WiFi transport → board's WebSocket server
- See real neighbors, send real messages
- This is the demo moment

---

## Phase 7: Polish & Robustness ✅ COMPLETE (2026-02-18)

### Task 7.1: Battery ADC reading ✅
- `components/battery/` — GPIO1 ADC with curve fitting calibration
- LiPo discharge curve (piecewise linear), OLED header, beacon, getStatus RPC, webapp Stats

### Task 7.2: Deep sleep ✅
- `bramble.sleep` RPC — opt-in deep sleep with DIO1 (LoRa) wake + optional timer
- Not automatic — triggered via RPC or CLI

### Task 7.3: OTA updates ✅
- `components/ota/` — esp_https_ota in background FreeRTOS task
- `bramble.otaUpdate` RPC accepts URL, auto-reboot on success

### Task 7.4: Flash size fix ✅
- Partition table for 8MB: app0/app1 = 3MB each (was 1.75MB), SPIFFS = 2MB (was 448KB)
- sdkconfig.defaults: FLASHSIZE_8MB

---

## Dependency Graph

```
Phase 1 (Identity)
    ↓
Phase 2 (SX1262 Driver)
    ↓
Phase 3 (Beacons) ← First RF communication
    ↓
Phase 4 (Routing + Messages) ← Full mesh protocol
    ↓               ↓
Phase 5 (WiFi)   Phase 6 (Two-node test)
    ↓               ↓
Phase 6 (Webapp ← → Real hardware)
    ↓
Phase 7 (Polish)
```

## Estimated Total: ~15-20 hours of implementation

**Key milestone:** Phase 3 complete = two boards discovering each other over LoRa.
**Demo milestone:** Phase 6 complete = webapp talking to real mesh hardware.

## Hardware Notes

- **Heltec V3 #1:** Connected to GPU box, `/dev/ttyUSB0`, MAC 10:51:DB:57:9A:10
- **Heltec V3 #2:** Just arrived, needs USB-C cable, will be `/dev/ttyUSB1` or `/dev/ttyACM1`
- **SX1262 chip:** Semtech, 150MHz–960MHz, LoRa + FSK
- **US ISM band:** 902–928 MHz, Bramble uses 915.0 MHz center
- **Antenna:** Both boards have onboard IPEX antenna connector + PCB trace antenna
- **Power:** USB-C (development) or LiPo battery (3.7V, JST connector)

## Build & Flash Workflow

```bash
# Edit on openclaw box
vim components/radio/sx1262.c

# Commit + push
git add -A && git commit -m "feat: SX1262 driver" && git push

# Build + flash via GPU box
bash scripts/flash.sh
```
