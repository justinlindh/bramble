# Heltec WiFi LoRa 32 V4 pin mapping status (Task 3)

This note records what is **currently verifiable from official Heltec references** and what is still pending before claiming full schematic-level pin validation.

## Validation checklist

- [x] SPI pins verified from official V4 references (Heltec library board config)
- [x] SX1262 pins verified (CS/RST/BUSY/DIO1) from official V4 references (Heltec library board config)
- [ ] Display bus + control pins verified from V4 schematic net labels (pending)
- [ ] Battery ADC path verified from V4 schematic net labels (pending)
- [ ] GNSS UART + power/enable pin verified from V4 schematic net labels (partially pending)

## Sources used (official Heltec)

1. Heltec WiFi LoRa 32 V4 product/docs page (links to official resources):
   - https://wiki.heltec.org/docs/devices/open-source-hardware/esp32-series/lora-32/wifi-lora-32-v4/
2. Official V4 schematic file listing + PDF:
   - https://resource.heltec.cn/download/WiFi_LoRa_32_V4/Schematic
   - https://resource.heltec.cn/download/WiFi_LoRa_32_V4/Schematic/WiFi_LoRa_32_V4.2.pdf
3. Official Heltec ESP32 library board pin definitions (contains `WIFI_LORA_32_V4` conditionals):
   - `src/driver/board-config.h` in https://github.com/HelTecAutomation/Heltec_ESP32
4. Official Heltec GPS examples (`VGNSS_CTRL` usage):
   - `examples/GPS/GPSToUart/GPSToUart.ino`
   - `examples/GPS/GPSDisplayOnTFT/GPSDisplayOnTFT.ino`

## Confirmed mappings (currently verifiable)

From Heltec `board-config.h` for `WIFI_LORA_32_V4`:

### LoRa SPI bus
- `LORA_CLK` = GPIO9
- `LORA_MISO` = GPIO11
- `LORA_MOSI` = GPIO10

### SX1262 control lines
- `RADIO_NSS` = GPIO8
- `RADIO_RESET` = GPIO12
- `RADIO_BUSY` = GPIO13
- `RADIO_DIO_1` = GPIO14

### GNSS power/enable (partial)
From Heltec GPS example sketches:
- `VGNSS_CTRL` = GPIO3 (used as GNSS power/enable control in examples)

## Pending / not yet proven from currently parsed sources

The following are **not yet line-item verified from V4 schematic net labels** in this task run:

- OLED I2C pins (`SDA`, `SCL`, OLED reset, Vext control)
- Battery ADC GPIO and divider path
- User button GPIO
- GNSS UART TX/RX GPIO mapping between ESP32-S3 and external L76K connector
- Whether V4 display/battery routing is fully identical to V3 on every hardware revision

## Practical implementation stance in `main/boards/heltec_v4.h`

- Applied confirmed SX1262/SPI mappings above.
- Kept display and battery pins aligned with existing V3 profile as a **conservative compatibility assumption**, but marked as **pending schematic net-level verification**.
- Disabled GPS capability for now (and kept UART pins unset) until GNSS UART mapping is verified from official V4 schematic/pinmap at net-label level.

## Hardware validation status

- Firmware build verification: completed (see command log in task output).
- Hardware smoke test (flash/boot/radio/display/GNSS): **pending** due to no hardware run in this task context.
