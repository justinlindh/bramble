# Bramble: LILYGO T-Deck Plus Complete Support Plan

**Author:** Agent (subagent task)  
**Date:** 2026-02-18  
**Status:** Phase 1 COMPLETE — merged to master 2026-02-19  
**Estimated effort:** 8–12 days (full support), ~3–4 days (radio + basic UI minimum viable)

### Phase 1 Completion Summary (2026-02-19)
All core hardware support implemented and working: board HAL, radio (SX1262 w/ TCXO), ST7789 display (320×240 fullscreen), keyboard, trackball, GPS, SD card, audio, resolution-aware UI, RPC extensions. 15 commits on `feature/tdeck-plus-support`, merged to master.

### Remaining for Phase 2 (Future)
- Rich graphical UI overhaul (the display can handle much more than text — 320×240 IPS is capable of full GUI)
- Font scaling — current text is quite small on the 320×240 display, needs larger fonts
- Battery monitoring (section 10)
- Power management / deep sleep
- Touch/trackball gesture refinement

---

## Overview

The T-Deck Plus is an ESP32-S3 + SX1262 device in a Blackberry form factor with a 2.8" ST7789 IPS LCD, full QWERTY keyboard (ESP32-C3 sub-MCU), Hall-effect trackball, MIA-M10Q GPS, audio codec, and a 2000 mAh battery. The core radio is identical to the Heltec V3 — the work is entirely in display, input, power management, and new peripheral support.

This plan gives step-by-step, file-level instructions for complete support. Every section specifies exactly what code changes to make, what to add, and what to test.

---

## 0. Prerequisites: Understand the Gotchas First

Before touching any code, internalize these constraints. They will cause hours of debugging if missed:

1. **BOARD_POWERON (GPIO10) = HIGH before ANY peripheral** — the T-Deck routes peripheral power through this pin. If it's not set, all peripherals fail silently. This must be the first thing that runs in app_main.
2. **Trackball center (GPIO0) conflicts with microphone enable** — GPIO0 is the BOOT pin and is also tied into the audio path. Never use both simultaneously. For Bramble (no audio in core), this is not a problem — just don't enable audio and trackball-center at the same time.
3. **ST7789 requires LilyGO's custom init sequence** — the standard ST7789 init (as in ESP-IDF LCD component) does NOT work. Must use the exact sequence from LilyGO commit 6adb888 (or the T-Deck Arduino sketch).
4. **SPI bus is SHARED** — display (CS=12), radio (CS=9), and SD card (CS=39) are all on the same SPI bus (MOSI=41, MISO=38, SCK=40). Bramble's current sx1262.c calls `spi_bus_initialize(SPI2_HOST, ...)` unconditionally. This must be refactored to initialize once at board level and add devices separately.
5. **Keyboard is a separate MCU** — the T-Keyboard ESP32-C3 runs independent firmware. Communicate via I2C (SDA=18, SCL=8) with interrupt on GPIO46. The C3 firmware sends one keycode per interrupt.
6. **Framebuffer must be in PSRAM** — 320×240×2 = 153,600 bytes. Internal DRAM is too small. Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`.
7. **Grove connector is NOT general-purpose** — on T-Deck Plus, those pins go to GPS UART. Don't try to use them for anything else.

---

## 1. Build System Changes

### 1.1 Kconfig.projbuild — Add Board Target

**File:** `main/Kconfig.projbuild`

Add the new board option to the existing `BRAMBLE_BOARD` choice:

```kconfig
choice BRAMBLE_BOARD
    prompt "Board selection"
    default BRAMBLE_BOARD_HELTEC_V3

    config BRAMBLE_BOARD_HELTEC_V3
        bool "Heltec WiFi LoRa 32 V3 (SX1262)"

    config BRAMBLE_BOARD_TDECK_PLUS
        bool "LILYGO T-Deck Plus (ESP32-S3 + SX1262 + GPS + Keyboard)"

    config BRAMBLE_BOARD_TTGO_LORA32
        bool "TTGO LoRa32 (SX1276)"

    config BRAMBLE_BOARD_CUSTOM
        bool "Custom board"
endchoice
```

Add a T-Deck Plus variant selection (Japan vs world):

```kconfig
if BRAMBLE_BOARD_TDECK_PLUS
    choice BRAMBLE_TDECK_PLUS_VARIANT
        prompt "T-Deck Plus regional variant"
        default BRAMBLE_TDECK_VARIANT_868

        config BRAMBLE_TDECK_VARIANT_868
            bool "Standard (868/915 MHz)"

        config BRAMBLE_TDECK_VARIANT_920
            bool "Japan MIC (920 MHz)"
    endchoice
endif
```

### 1.2 sdkconfig.defaults.tdeck_plus — New Build Config

**Create:** `sdkconfig.defaults.tdeck_plus`

```ini
# T-Deck Plus specific sdkconfig overrides
# Apply on top of sdkconfig.defaults with:
#   idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus" build

# Flash: 16 MB
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_FLASHMODE_QIO=y

# PSRAM: 8 MB OPI
CONFIG_SPIRAM=y
CONFIG_ESP32S3_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y

# CPU: 240 MHz (same as default)
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y

# Board selection
CONFIG_BRAMBLE_BOARD_TDECK_PLUS=y
```

### 1.3 Partition Table — Update for 16 MB Flash

**Create:** `partitions.tdeck_plus.csv`

```csv
# T-Deck Plus: 16 MB flash
# Name,   Type, SubType, Offset,   Size,    Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x3C0000,
app1,     app,  ota_1,   0x3D0000, 0x3C0000,
spiffs,   data, spiffs,  0x7D0000, 0x820000,
```

The app partitions are doubled (2× the current 1.75 MB each → 3.75 MB each) to accommodate the larger codebase. The SPIFFS partition grows substantially for potential SD-backed storage of fonts and assets.

Add to `sdkconfig.defaults.tdeck_plus`:
```ini
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.tdeck_plus.csv"
```

### 1.4 Build Script Update

**File:** `scripts/flash.sh`

Add a T-Deck Plus target:

```bash
elif [ "$BOARD" = "tdeck-plus" ]; then
    export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus"
    idf.py -p "$PORT" build flash monitor
```

---

## 2. Board Initialization Layer

### 2.1 New File: `main/board_tdeck_plus.h`

Create a board-specific header with all pin definitions and initialization:

```c
#ifndef BOARD_TDECK_PLUS_H
#define BOARD_TDECK_PLUS_H

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

/* Power control — MUST be HIGH before any peripheral */
#define BOARD_POWERON        10

/* Shared SPI bus */
#define BOARD_SPI_MOSI       41
#define BOARD_SPI_MISO       38
#define BOARD_SPI_SCK        40

/* SPI chip selects */
#define BOARD_TFT_CS         12
#define BOARD_SDCARD_CS      39
#define BOARD_RADIO_CS        9

/* ST7789 display control */
#define BOARD_TFT_DC         11
#define BOARD_TFT_BACKLIGHT  42

/* SX1262 radio */
#define BOARD_RADIO_BUSY     13
#define BOARD_RADIO_RST      17
#define BOARD_RADIO_DIO1     45

/* I2C */
#define BOARD_I2C_SDA        18
#define BOARD_I2C_SCL         8
#define BOARD_KEYBOARD_INT   46
#define BOARD_TOUCH_INT      16  /* GT911 capacitive touch interrupt */

/* Trackball (Hall Effect) */
#define BOARD_TBOX_G01        3   /* up    */
#define BOARD_TBOX_G02        2   /* down  */
#define BOARD_TBOX_G03       15   /* left  */
#define BOARD_TBOX_G04        1   /* right */
#define BOARD_TBOX_CENTER     0   /* select / BOOT */

/* GPS UART */
#define BOARD_GPS_TX         43   /* ESP RX */
#define BOARD_GPS_RX         44   /* ESP TX */
#define BOARD_GPS_BAUD     9600

/* Audio (ES7210 + I2S) — for future use */
#define BOARD_I2S_WS          5
#define BOARD_I2S_BCK         7
#define BOARD_I2S_DOUT        6
#define BOARD_ES7210_MCLK    48
#define BOARD_ES7210_LRCK    21
#define BOARD_ES7210_SCK     47
#define BOARD_ES7210_DIN     14

/* Battery ADC */
#define BOARD_BAT_ADC         4

#endif /* CONFIG_BRAMBLE_BOARD_TDECK_PLUS */
#endif /* BOARD_TDECK_PLUS_H */
```

### 2.2 New File: `main/board.c` (and `main/board.h`)

Create a board abstraction layer that centralizes power-on sequencing and the shared SPI bus handle:

**`main/board.h`:**
```c
#ifndef BRAMBLE_BOARD_H
#define BRAMBLE_BOARD_H

#include "driver/spi_master.h"

/* Returns 0 on success. Must be called first in app_main, before any
 * peripheral init. Handles BOARD_POWERON and shared SPI bus init. */
int board_init(void);

/* Returns the shared SPI bus handle (T-Deck Plus only; NULL on other boards). */
spi_host_device_t board_get_spi_host(void);

/* Returns the battery percentage (0-100). Returns 0 if not supported. */
uint8_t board_get_battery_pct(void);

#endif
```

**`main/board.c`:**
```c
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/adc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "board_tdeck_plus.h"

static spi_host_device_t s_spi_host = SPI2_HOST;

int board_init(void) {
    /* Step 1: Assert peripheral power rail */
    gpio_config_t pwr_conf = {
        .pin_bit_mask = (1ULL << BOARD_POWERON),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_conf);
    gpio_set_level(BOARD_POWERON, 1);
    ESP_LOGI(TAG, "Peripheral power rail enabled (GPIO%d)", BOARD_POWERON);

    /* Allow rails to stabilize */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Step 2: Initialize shared SPI bus (radio + display + SD share SPI2_HOST) */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = BOARD_SPI_MOSI,
        .miso_io_num   = BOARD_SPI_MISO,
        .sclk_io_num   = BOARD_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 320 * 240 * 2,  /* Full framebuffer DMA */
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "Shared SPI bus initialized (SPI2_HOST)");

    return 0;
}

spi_host_device_t board_get_spi_host(void) {
    return s_spi_host;
}

uint8_t board_get_battery_pct(void) {
    /* T-Deck Plus: battery ADC on GPIO4 (ADC1_CH3 on ESP32-S3) */
    /* 3.7V LiPo: ~4.2V full, ~3.3V cutoff via TP4065B charger */
    /* ADC reference = 3.3V, but battery is on a divider — check schematic */
    /* TODO: verify divider ratio from T-Deck Plus schematic */
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11);
    int raw = adc1_get_raw(ADC1_CHANNEL_3);
    /* Approximate: raw 2048 = 3.3V * (R2/(R1+R2)), map 3.3V-4.2V → 0-100% */
    /* Placeholder until schematic is verified: */
    uint32_t mv = (uint32_t)raw * 3300 / 4095;
    if (mv >= 4100) return 100;
    if (mv <= 3300) return 0;
    return (uint8_t)((mv - 3300) * 100 / 800);
}

#else
/* Other boards — minimal stub */
int board_init(void) { return 0; }
spi_host_device_t board_get_spi_host(void) { return SPI2_HOST; }
uint8_t board_get_battery_pct(void) { return 0; }
#endif
```

### 2.3 Update `main/main.c` — Call `board_init()` First

In `app_main()`, add `board_init()` as the **very first call** after the log message, before NVS:

```c
void app_main(void)
{
    ESP_LOGI(TAG, "=== BOOT STAGE: app_main entry ===");
    
    /* FIRST: board-level power and SPI bus init */
    ESP_LOGI(TAG, "=== BOOT STAGE: board_init ===");
    if (board_init() != 0) {
        ESP_LOGE(TAG, "Board init failed — halting");
        return;
    }
    
    /* NVS init */
    ESP_LOGI(TAG, "=== BOOT STAGE: nvs_flash_init ===");
    // ... rest of existing code
```

---

## 3. Radio Driver Refactor

The SX1262 driver currently calls `spi_bus_initialize()` in `sx1262_init()`. For T-Deck Plus, the bus is already initialized by `board_init()`. The driver just needs to add a device.

### 3.1 Pin Definitions — Conditional in `sx1262.h`

Replace the hardcoded Heltec V3 pin block in `components/radio/include/sx1262.h`:

```c
/* ---------- Board-specific pin assignments ---------- */
#if defined(CONFIG_BRAMBLE_BOARD_HELTEC_V3) || !defined(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
/* Heltec WiFi LoRa 32 V3 */
#define SX1262_PIN_SCK   9
#define SX1262_PIN_MISO  11
#define SX1262_PIN_MOSI  10
#define SX1262_PIN_NSS   8
#define SX1262_PIN_RST   12
#define SX1262_PIN_BUSY  13
#define SX1262_PIN_DIO1  14

#elif defined(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
/* LILYGO T-Deck Plus — these are included via board_tdeck_plus.h,
 * but also defined here for the radio component's use */
#define SX1262_PIN_SCK   40
#define SX1262_PIN_MISO  38
#define SX1262_PIN_MOSI  41
#define SX1262_PIN_NSS   9
#define SX1262_PIN_RST   17
#define SX1262_PIN_BUSY  13
#define SX1262_PIN_DIO1  45
#endif
```

### 3.2 SPI Bus Init — Conditional in `sx1262.c`

In `sx1262_init()`, wrap the `spi_bus_initialize()` call:

```c
int sx1262_init(void) {
    /* ... GPIO config for NSS, RST, BUSY, DIO1 ... */

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    /* SPI bus already initialized by board_init(). Just add device. */
    spi_host_device_t host = board_get_spi_host();
#else
    /* Other boards: initialize SPI bus here */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = SX1262_PIN_MOSI,
        .miso_io_num   = SX1262_PIN_MISO,
        .sclk_io_num   = SX1262_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return -1;
    }
    spi_host_device_t host = SPI2_HOST;
#endif

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = -1,  /* manual CS */
        .queue_size     = 1,
    };
    esp_err_t err2 = spi_bus_add_device(host, &dev_cfg, &s_spi);
    if (err2 != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(err2));
        return -1;
    }

    /* Reset chip */
    if (sx1262_reset() != 0) return -1;
    if (sx1262_set_standby(0) != 0) return -1;

#ifdef CONFIG_BRAMBLE_BOARD_HELTEC_V3
    /* Heltec V3: DIO3 as TCXO (1.7V, 5ms timeout) */
    if (sx1262_set_dio3_as_tcxo(1.7f, 5) != 0) return -1;
#endif
/* T-Deck Plus uses a crystal, NOT DIO3-TCXO — skip this entirely */

    if (sx1262_calibrate(0x7F) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Calibrate image for configured frequency */
#ifdef CONFIG_BRAMBLE_REGION_EU868
    if (sx1262_calibrate_image(868.0f) != 0) return -1;
#else
    if (sx1262_calibrate_image(915.0f) != 0) return -1;
#endif

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    /* T-Deck Plus: LDO mode (check schematic — if no DC-DC, use LDO=0) */
    if (sx1262_set_regulator_mode(0) != 0) return -1;
#else
    /* Heltec V3: DC-DC */
    if (sx1262_set_regulator_mode(1) != 0) return -1;
#endif

    if (sx1262_set_packet_type(1) != 0) return -1;
    if (sx1262_set_buffer_base_address(0, 128) != 0) return -1;

    return 0;
}
```

> **TODO before implementation:** Verify regulator mode (LDO vs DC-DC) and TCXO wiring from the T-Deck Plus schematic PDF at https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/schematic/schematic.pdf

---

## 4. Display Driver — ST7789

The existing `components/display/ssd1306.c` targets a 128×64 I2C OLED. The T-Deck Plus has a 320×240 SPI IPS LCD with an ST7789 controller. A new driver is required.

### 4.1 New File: `components/display/st7789.c`

The new driver must:
- Use the shared SPI2_HOST bus (via `board_get_spi_host()`)
- Allocate a 153,600-byte framebuffer in PSRAM
- Implement the same `display.h` API so the rest of Bramble (main.c, mesh_task display calls) works without change
- Use LilyGO's specific ST7789 init sequence

**Key implementation points:**

```c
/* In st7789.c */
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

#include "display.h"
#include "board_tdeck_plus.h"
#include "board.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "st7789";

#define ST7789_WIDTH   320
#define ST7789_HEIGHT  240
#define ST7789_FB_SIZE (ST7789_WIDTH * ST7789_HEIGHT * 2)  /* 16bpp = 153600 bytes */

static uint16_t *s_framebuffer = NULL;  /* PSRAM allocation */
static spi_device_handle_t s_spi;
static bool s_initialized = false;

/* LilyGO custom init sequence (from commit 6adb888) */
/* Reference: https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/src/LilyGo_TWatchS3.cpp */
static const uint8_t st7789_init_seq[] = {
    /* cmd,  ndata,  data... */
    0x01, 0,                              /* Software reset */
    0x00, 0xFF,                           /* Delay 150ms (0xFF sentinel) */
    0x11, 0,                              /* Sleep out */
    0x00, 0xFF,                           /* Delay 150ms */
    0x3A, 1, 0x55,                        /* COLMOD: 16bpp */
    0x36, 1, 0x00,                        /* MADCTL: row/col addressing */
    0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33, /* PORCTRL */
    0xB7, 1, 0x35,                        /* GCTRL */
    0xBB, 1, 0x19,                        /* VCOMS */
    0xC0, 1, 0x2C,                        /* LCMCTRL */
    0xC2, 1, 0x01,                        /* VDVVRHEN */
    0xC3, 1, 0x12,                        /* VRH */
    0xC4, 1, 0x20,                        /* VDV */
    0xC6, 1, 0x0F,                        /* FRCTRL2 */
    0xD0, 2, 0xA4, 0xA1,                  /* PWCTRL1 */
    0xE0, 14, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54,
              0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23, /* PGC */
    0xE1, 14, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44,
              0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23, /* NGC */
    0x21, 0,                              /* Display inversion ON */
    0x29, 0,                              /* Display on */
    0xFF, 0xFF                            /* End marker */
};
```

> **Action required:** Fetch the exact init sequence bytes from LilyGO's T-Deck repository before implementing. The sequence above is illustrative — match it byte-for-byte from commit 6adb888 or the current master:
> ```bash
> # In the T-Deck repo:
> git show 6adb888 -- src/LilyGo_TWatchS3.cpp | grep -A 100 "ST7789"
> ```

**Framebuffer allocation (MUST be PSRAM):**
```c
int display_init(void) {
    /* Allocate framebuffer in PSRAM */
    s_framebuffer = (uint16_t *)heap_caps_malloc(ST7789_FB_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_framebuffer) {
        ESP_LOGE(TAG, "Failed to allocate %d byte framebuffer in PSRAM", ST7789_FB_SIZE);
        return -1;
    }
    memset(s_framebuffer, 0, ST7789_FB_SIZE);
    
    /* Add ST7789 to shared SPI bus */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,  /* 40 MHz for display */
        .mode           = 0,
        .spics_io_num   = BOARD_TFT_CS,       /* Hardware CS this time */
        .queue_size     = 7,
        .pre_cb         = lcd_spi_pre_transfer_callback,  /* D/C line control */
    };
    spi_bus_add_device(board_get_spi_host(), &dev_cfg, &s_spi);
    
    /* Configure DC and backlight GPIOs */
    gpio_set_direction(BOARD_TFT_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_TFT_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_TFT_BACKLIGHT, 1);  /* Backlight on */
    
    /* Run init sequence */
    st7789_run_init_sequence();
    
    s_initialized = true;
    return 0;
}
```

**display_flush() — DMA transfer to display:**

The flush must send the full framebuffer via SPI DMA. Because the display is 320×240 and SPI transfers are limited to 4096 bytes by default (configurable), flush in chunks of N rows:

```c
void display_flush(void) {
    if (!s_initialized || !s_framebuffer) return;
    
    /* Set address window: full screen */
    st7789_set_window(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);
    
    /* Transfer framebuffer in 16-row chunks (320*16*2 = 10240 bytes each) */
    const int CHUNK_ROWS = 16;
    const int chunk_bytes = ST7789_WIDTH * CHUNK_ROWS * 2;
    
    for (int row = 0; row < ST7789_HEIGHT; row += CHUNK_ROWS) {
        int rows_this = (row + CHUNK_ROWS <= ST7789_HEIGHT) ? CHUNK_ROWS : (ST7789_HEIGHT - row);
        spi_transaction_t t = {
            .length = rows_this * ST7789_WIDTH * 16,  /* bits */
            .tx_buffer = &s_framebuffer[row * ST7789_WIDTH],
        };
        gpio_set_level(BOARD_TFT_DC, 1);  /* Data mode */
        spi_device_transmit(s_spi, &t);
    }
}
```

### 4.2 Maintain `display.h` API Compatibility

The existing `display.h` API (display_init, display_clear, display_fill, display_draw_text, display_draw_text_large, display_hline, display_pixel, display_flush, display_power, display_set_contrast, display_invert) must be fully implemented in `st7789.c`.

**Important adaptations:**
- `DISPLAY_WIDTH` and `DISPLAY_HEIGHT` constants: Update to `320` and `240` in a T-Deck Plus-specific block or via board-level defines
- `display_draw_text()`: 6×8 font renders fine on 320×240; may want to use a larger default font for the bigger screen
- `display_draw_text_large()`: The 2× scaling from 6×8 → 12×16 works; for T-Deck Plus consider an additional `display_draw_text_xl()` at 3× (18×24) for headers

**In `components/display/CMakeLists.txt`:**

```cmake
if(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
    set(display_srcs "st7789.c")
else()
    set(display_srcs "ssd1306.c")
endif()

idf_component_register(
    SRCS ${display_srcs}
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_spi
)
```

### 4.3 Display Constants — Board-Conditional

Update `components/display/include/display.h` to set dimensions based on board:

```c
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240
#else
/* Heltec V3 SSD1306 */
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define DISPLAY_SDA_PIN     17
#define DISPLAY_SCL_PIN     18
#define DISPLAY_RST_PIN     21
#define DISPLAY_VEXT_PIN    36
#define DISPLAY_I2C_ADDR    0x3C
#define DISPLAY_I2C_PORT    0
#define DISPLAY_I2C_FREQ_HZ 400000
#endif
```

---

## 5. Keyboard Component (New)

### 5.1 Create `components/keyboard/`

**`components/keyboard/include/keyboard.h`:**

```c
#ifndef BRAMBLE_KEYBOARD_H
#define BRAMBLE_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

/* T-Deck keyboard I2C address (T-Keyboard ESP32-C3) */
#define KEYBOARD_I2C_ADDR  0x55  /* Verify from T-Deck source */

/* Special keycodes above ASCII */
#define KEY_BACKSPACE  0x08
#define KEY_ENTER      0x0D
#define KEY_TAB        0x09
#define KEY_ALT        0x80   /* Alt key (T-Deck's orange key) */
#define KEY_UP         0x81
#define KEY_DOWN       0x82
#define KEY_LEFT       0x83
#define KEY_RIGHT      0x84

typedef void (*keyboard_char_cb_t)(char c, void *ctx);

/**
 * Initialize the keyboard I2C link and interrupt pin.
 * Must be called after I2C bus is ready.
 */
int keyboard_init(void);

/**
 * Poll for pending keystrokes. Call from main loop.
 * Alternatively, use keyboard_set_callback for event-driven operation.
 * Returns true if a key was read, and places value in *out.
 */
bool keyboard_poll(char *out);

/**
 * Set a callback for keystroke events (called from task context, not ISR).
 */
void keyboard_set_callback(keyboard_char_cb_t cb, void *ctx);

/**
 * Returns true if there are pending keys in the buffer.
 */
bool keyboard_has_data(void);

#endif
```

**`components/keyboard/keyboard.c`:**

```c
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

#include "keyboard.h"
#include "board_tdeck_plus.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "keyboard";

/* Shared I2C bus handle (shared with display's touch chip area) */
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_kbd_dev;

/* Circular key buffer */
#define KBD_BUF_SIZE 32
static char s_buf[KBD_BUF_SIZE];
static volatile int s_buf_head = 0;
static volatile int s_buf_tail = 0;
static volatile bool s_data_pending = false;

static keyboard_char_cb_t s_callback = NULL;
static void *s_callback_ctx = NULL;

/* ISR: set pending flag, actual I2C read happens in a task */
static void IRAM_ATTR kbd_isr_handler(void *arg) {
    s_data_pending = true;
    /* Signal semaphore if we have a task waiting */
}

int keyboard_init(void) {
    /* Initialize I2C master bus (shared with future touch/sensors) */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* Add keyboard device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KEYBOARD_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_kbd_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Keyboard I2C device add failed");
        return -1;
    }

    /* Configure interrupt pin */
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << BOARD_KEYBOARD_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,  /* Active-low interrupt */
    };
    gpio_config(&int_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOARD_KEYBOARD_INT, kbd_isr_handler, NULL);

    ESP_LOGI(TAG, "Keyboard initialized (I2C addr 0x%02X, INT GPIO%d)",
             KEYBOARD_I2C_ADDR, BOARD_KEYBOARD_INT);
    return 0;
}

bool keyboard_has_data(void) {
    return s_data_pending || (s_buf_head != s_buf_tail);
}

bool keyboard_poll(char *out) {
    if (s_data_pending) {
        s_data_pending = false;
        uint8_t key = 0;
        esp_err_t err = i2c_master_receive(s_kbd_dev, &key, 1, 100);
        if (err == ESP_OK && key != 0) {
            /* Buffer it */
            s_buf[s_buf_head] = (char)key;
            s_buf_head = (s_buf_head + 1) % KBD_BUF_SIZE;
        }
    }

    if (s_buf_head != s_buf_tail) {
        *out = s_buf[s_buf_tail];
        s_buf_tail = (s_buf_tail + 1) % KBD_BUF_SIZE;
        if (s_callback) s_callback(*out, s_callback_ctx);
        return true;
    }
    return false;
}

void keyboard_set_callback(keyboard_char_cb_t cb, void *ctx) {
    s_callback = cb;
    s_callback_ctx = ctx;
}

#endif /* CONFIG_BRAMBLE_BOARD_TDECK_PLUS */
```

> **Action required:** Verify the T-Keyboard I2C address (typically 0x55) from the T-Deck Arduino library source. Also verify the read protocol — some versions require reading a register, others just read 1 byte on interrupt.
>
> Reference: https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/src/T_Keyboard/T_Keyboard.cpp

**`components/keyboard/CMakeLists.txt`:**
```cmake
idf_component_register(
    SRCS "keyboard.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
)
```

---

## 6. Input Handler — Trackball

The trackball uses 4× AN48841B Hall Effect sensors (one per axis). Each sensor outputs a pulse when the trackball moves in that direction. Unlike a quadrature encoder, each GPIO fires independently.

### 6.1 Extend `ui.h` — New Button Events

Add directional events to `ui_button_t` in `components/ui/include/ui.h`:

```c
typedef enum {
    BTN_NONE = 0,
    BTN_SHORT_PRESS,
    BTN_LONG_PRESS,
    BTN_DOUBLE_PRESS,
    /* Trackball directions (T-Deck Plus only) */
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_SELECT,    /* Trackball center press */
} ui_button_t;
```

### 6.2 New `components/trackball/` Component

**`components/trackball/include/trackball.h`:**
```c
#ifndef BRAMBLE_TRACKBALL_H
#define BRAMBLE_TRACKBALL_H

#include "ui.h"

int trackball_init(void);
ui_button_t trackball_poll(void);  /* Returns BTN_NONE or directional/select */

#endif
```

**`components/trackball/trackball.c`:**

```c
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

#include "trackball.h"
#include "board_tdeck_plus.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "trackball";

/* Counts incremented by ISR */
static volatile int s_up_count = 0;
static volatile int s_down_count = 0;
static volatile int s_left_count = 0;
static volatile int s_right_count = 0;
static volatile bool s_center_pressed = false;

static void IRAM_ATTR isr_up(void *arg)    { s_up_count++;    }
static void IRAM_ATTR isr_down(void *arg)  { s_down_count++;  }
static void IRAM_ATTR isr_left(void *arg)  { s_left_count++;  }
static void IRAM_ATTR isr_right(void *arg) { s_right_count++; }
static void IRAM_ATTR isr_center(void *arg){ s_center_pressed = true; }

int trackball_init(void) {
    /* Hall effect sensors: each GPIO fires NEGEDGE when ball moves that direction */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_TBOX_G01) | (1ULL << BOARD_TBOX_G02) |
                        (1ULL << BOARD_TBOX_G03) | (1ULL << BOARD_TBOX_G04) |
                        (1ULL << BOARD_TBOX_CENTER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOARD_TBOX_G01, isr_up,     NULL);
    gpio_isr_handler_add(BOARD_TBOX_G02, isr_down,   NULL);
    gpio_isr_handler_add(BOARD_TBOX_G03, isr_left,   NULL);
    gpio_isr_handler_add(BOARD_TBOX_G04, isr_right,  NULL);
    gpio_isr_handler_add(BOARD_TBOX_CENTER, isr_center, NULL);

    ESP_LOGI(TAG, "Trackball initialized (UP:%d DN:%d LT:%d RT:%d CTR:%d)",
             BOARD_TBOX_G01, BOARD_TBOX_G02, BOARD_TBOX_G03,
             BOARD_TBOX_G04, BOARD_TBOX_CENTER);
    return 0;
}

ui_button_t trackball_poll(void) {
    /* Priority: center > up > down > left > right */
    if (s_center_pressed) {
        s_center_pressed = false;
        return BTN_SELECT;
    }
    if (s_up_count > 0)    { s_up_count--;    return BTN_UP;    }
    if (s_down_count > 0)  { s_down_count--;  return BTN_DOWN;  }
    if (s_left_count > 0)  { s_left_count--;  return BTN_LEFT;  }
    if (s_right_count > 0) { s_right_count--; return BTN_RIGHT; }
    return BTN_NONE;
}

#endif
```

> **Note:** GPIO directions may not map up/down/left/right as labeled in the spec. Verify by testing physical movement and adjust the ISR-to-direction mapping accordingly.

### 6.3 Update `button.c` — T-Deck Plus Compatibility

On T-Deck Plus, `button_init()` should be a no-op (trackball handles center) or optionally still configure GPIO0 as a BOOT button for firmware flashing. Since GPIO0 = BOOT = trackball center, the existing code works as a fallback:

```c
void button_init(void) {
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    /* GPIO0 is the trackball center — handled by trackball component.
     * No separate button init needed. GPIO0 is also BOOT, which is
     * handled by the ESP32-S3 ROM automatically. */
    ESP_LOGI(TAG, "T-Deck Plus: button_init is a no-op (trackball handles center)");
#else
    /* Heltec V3: PRG button on GPIO0 */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);
    ESP_LOGI(TAG, "Button initialized on GPIO%d", BUTTON_GPIO);
#endif
}
```

### 6.4 Update `main.c` — Integrate Both Input Sources

```c
/* In main loop, poll both sources: */
ui_button_t btn = BTN_NONE;
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    btn = trackball_poll();
    if (btn == BTN_NONE) {
        /* Also handle keyboard for text composition */
        char key;
        if (keyboard_poll(&key)) {
            ui_handle_key(&ui, key, now_ms);  /* new function - see UI section */
        }
    }
#else
    btn = button_poll(now_ms);
#endif
if (btn != BTN_NONE) {
    ui_handle_button(&ui, btn, now_ms);
}
```

---

## 7. GPS Component (New)

### 7.1 Create `components/gps/`

**`components/gps/include/gps.h`:**
```c
#ifndef BRAMBLE_GPS_H
#define BRAMBLE_GPS_H

#include "location.h"
#include <stdbool.h>

typedef void (*gps_fix_cb_t)(const bramble_position_t *pos, void *ctx);

/**
 * Initialize GPS UART (MIA-M10Q on T-Deck Plus).
 * Starts a background task that reads NMEA and calls cb on valid fixes.
 */
int gps_init(gps_fix_cb_t cb, void *ctx);

/** Returns true if we have a valid GPS fix. */
bool gps_has_fix(void);

/** Get the most recent GPS position. Returns false if no fix. */
bool gps_get_position(bramble_position_t *out);

/** Stop GPS task and power down module (if supported). */
void gps_deinit(void);

#endif
```

**`components/gps/gps.c`:**

```c
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

#include "gps.h"
#include "board_tdeck_plus.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "gps";

#define GPS_UART_PORT  UART_NUM_1
#define GPS_UART_BAUD  BOARD_GPS_BAUD   /* 9600 by default */
#define GPS_BUF_SIZE   512

static gps_fix_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static bramble_position_t s_last_pos = {0};
static bool s_has_fix = false;
static TaskHandle_t s_task = NULL;

/* --- NMEA parser --- */

/* Parse degrees+minutes: "DDMM.MMMM" format */
static float nmea_dm_to_degrees(const char *field, char dir) {
    if (!field || !field[0]) return 0.0f;
    float dm = atof(field);
    int deg = (int)(dm / 100);
    float min = dm - (deg * 100.0f);
    float result = deg + min / 60.0f;
    if (dir == 'S' || dir == 'W') result = -result;
    return result;
}

/* Parse $GPRMC or $GNRMC */
static bool parse_rmc(char *sentence, bramble_position_t *pos) {
    /* $GPRMC,HHMMSS.ss,A,LLLL.LL,a,YYYYY.YY,a,x.x,x.x,DDMMYY,x.x,a*hh */
    char *tok = strtok(sentence, ",");
    if (!tok) return false;
    
    /* Skip: HHMMSS */
    tok = strtok(NULL, ","); if (!tok) return false;
    /* Status: A=valid, V=void */
    tok = strtok(NULL, ","); if (!tok) return false;
    bool valid = (tok[0] == 'A');
    if (!valid) return false;
    
    /* Latitude */
    char *lat_str = strtok(NULL, ","); if (!lat_str) return false;
    char *lat_dir = strtok(NULL, ","); if (!lat_dir) return false;
    /* Longitude */
    char *lon_str = strtok(NULL, ","); if (!lon_str) return false;
    char *lon_dir = strtok(NULL, ","); if (!lon_dir) return false;
    /* Speed in knots */
    char *spd_str = strtok(NULL, ","); if (!spd_str) return false;
    /* Course */
    char *crs_str = strtok(NULL, ","); if (!crs_str) return false;
    
    float lat = nmea_dm_to_degrees(lat_str, lat_dir[0]);
    float lon = nmea_dm_to_degrees(lon_str, lon_dir[0]);
    float spd_knots = atof(spd_str);
    float course = atof(crs_str);
    
    pos->latitude_e7 = (int32_t)(lat * 1e7f);
    pos->longitude_e7 = (int32_t)(lon * 1e7f);
    pos->speed_kmh = (uint8_t)(spd_knots * 1.852f);
    pos->heading_deg2 = (uint8_t)(course / 2.0f);
    pos->valid = true;
    pos->timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    
    return true;
}

/* Parse $GPGGA or $GNGGA (for altitude and fix quality) */
static bool parse_gga(char *sentence, bramble_position_t *pos) {
    /* $GPGGA,HHMMSS.ss,LLLL.LL,a,YYYYY.YY,a,x,xx,x.x,x.x,M,x.x,M,,*hh */
    strtok(sentence, ",");          /* $GPGGA */
    strtok(NULL, ",");              /* time */
    strtok(NULL, ",");              /* lat */
    strtok(NULL, ",");              /* N/S */
    strtok(NULL, ",");              /* lon */
    strtok(NULL, ",");              /* E/W */
    char *fix_q = strtok(NULL, ",");  /* fix quality: 0=invalid,1=GPS,2=DGPS */
    if (!fix_q || fix_q[0] == '0') return false;
    strtok(NULL, ",");              /* satellites */
    strtok(NULL, ",");              /* HDOP */
    char *alt_str = strtok(NULL, ",");  /* altitude */
    
    if (alt_str) {
        pos->altitude_m = (int16_t)atof(alt_str);
    }
    return true;
}

static void gps_task(void *arg) {
    uint8_t buf[GPS_BUF_SIZE];
    char line[128];
    int line_pos = 0;
    
    while (1) {
        int len = uart_read_bytes(GPS_UART_PORT, buf, sizeof(buf) - 1, 
                                  pdMS_TO_TICKS(100));
        if (len <= 0) continue;
        buf[len] = 0;
        
        for (int i = 0; i < len; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (line_pos > 6) {
                    line[line_pos] = 0;
                    
                    bramble_position_t pos = s_last_pos;
                    bool updated = false;
                    
                    if (strncmp(line, "$GPRMC", 6) == 0 ||
                        strncmp(line, "$GNRMC", 6) == 0) {
                        char tmp[128];
                        strncpy(tmp, line, sizeof(tmp)-1);
                        updated = parse_rmc(tmp, &pos);
                    } else if (strncmp(line, "$GPGGA", 6) == 0 ||
                               strncmp(line, "$GNGGA", 6) == 0) {
                        char tmp[128];
                        strncpy(tmp, line, sizeof(tmp)-1);
                        if (parse_gga(tmp, &pos) && s_has_fix) updated = true;
                    }
                    
                    if (updated && pos.valid) {
                        s_last_pos = pos;
                        s_has_fix = true;
                        if (s_cb) s_cb(&s_last_pos, s_cb_ctx);
                        ESP_LOGD(TAG, "Fix: lat=%.6f lon=%.6f alt=%dm",
                                 pos.latitude_e7 / 1e7, pos.longitude_e7 / 1e7,
                                 pos.altitude_m);
                    }
                }
                line_pos = 0;
            } else if (line_pos < (int)sizeof(line) - 1) {
                line[line_pos++] = c;
            }
        }
    }
}

int gps_init(gps_fix_cb_t cb, void *ctx) {
    s_cb = cb;
    s_cb_ctx = ctx;
    
    uart_config_t uart_cfg = {
        .baud_rate = GPS_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(GPS_UART_PORT, GPS_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(GPS_UART_PORT, &uart_cfg);
    uart_set_pin(GPS_UART_PORT, BOARD_GPS_RX, BOARD_GPS_TX, 
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    
    xTaskCreatePinnedToCore(gps_task, "gps_task", 4096, NULL, 
                            2, &s_task, 0);  /* CPU0, low priority */
    
    ESP_LOGI(TAG, "GPS initialized (UART%d, %d baud, TX:%d RX:%d)",
             GPS_UART_PORT, GPS_UART_BAUD, BOARD_GPS_TX, BOARD_GPS_RX);
    return 0;
}

bool gps_has_fix(void) { return s_has_fix; }

bool gps_get_position(bramble_position_t *out) {
    if (!s_has_fix) return false;
    *out = s_last_pos;
    return true;
}

void gps_deinit(void) {
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    uart_driver_delete(GPS_UART_PORT);
}

#endif /* CONFIG_BRAMBLE_BOARD_TDECK_PLUS */
```

### 7.2 Wire GPS into Location Subsystem

In `main/main.c`, after board_init and before mesh_task_start:

```c
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    #include "gps.h"
    
    /* GPS callback — feed position into location subsystem */
    static location_manager_t g_location_mgr;
    
    static void on_gps_fix(const bramble_position_t *pos, void *ctx) {
        location_set_position(&g_location_mgr, pos);
        ESP_LOGI(TAG, "GPS fix: %.6f, %.6f, alt=%dm",
                 pos->latitude_e7 / 1e7f, pos->longitude_e7 / 1e7f, pos->altitude_m);
    }
    
    location_init(&g_location_mgr);
    gps_init(on_gps_fix, NULL);
#endif
```

---

## 8. SD Card Component (New)

The SD card shares the SPI bus. It enables offline message storage, configuration backup, and log files.

### 8.1 Create `components/sdcard/`

**`components/sdcard/include/sdcard.h`:**
```c
#ifndef BRAMBLE_SDCARD_H
#define BRAMBLE_SDCARD_H

#include <stdbool.h>

int sdcard_init(void);
bool sdcard_is_present(void);
void sdcard_deinit(void);

/* Returns mount point, e.g. "/sdcard" */
const char *sdcard_get_mount_point(void);

#endif
```

**`components/sdcard/sdcard.c`:**

```c
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

#include "sdcard.h"
#include "board.h"
#include "board_tdeck_plus.h"
#include "driver/spi_master.h"
#include "driver/sdmmc_host.h"  /* for sdspi */
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

#define MOUNT_POINT "/sdcard"

int sdcard_init(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = BOARD_SDCARD_CS;
    slot_cfg.host_id = board_get_spi_host();
    
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = board_get_spi_host();
    
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_cfg, 
                                             &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "SD card mount failed (card present but unformatted?)");
        } else {
            ESP_LOGW(TAG, "SD card not detected or init error: %s", 
                     esp_err_to_name(err));
        }
        return -1;
    }
    
    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return 0;
}

bool sdcard_is_present(void) { return s_mounted; }
const char *sdcard_get_mount_point(void) { return MOUNT_POINT; }

void sdcard_deinit(void) {
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_mounted = false;
    }
}

#endif
```

---

## 9. UI Overhaul for Larger Display

The T-Deck Plus's 320×240 screen is 7.5× larger in pixel area than the 128×64 OLED. The UI needs a redesign to take advantage of this.

### 9.1 New Screen Architecture

Add new screens to `ui_screen_t` in `ui.h`:

```c
typedef enum {
    SCREEN_MAIN = 0,
    SCREEN_MESSAGES,
    SCREEN_COMPOSE,        /* Text entry via keyboard (T-Deck Plus) */
    SCREEN_NODES,
    SCREEN_SETTINGS,
    SCREEN_MAP,            /* GPS map / node positions (T-Deck Plus) */
    SCREEN_COUNT
} ui_screen_t;
```

### 9.2 New Compose Screen

The SCREEN_COMPOSE screen lets users type messages using the physical keyboard. This is only available on T-Deck Plus. The screen shows:
- Conversation header (destination node name/address)
- Received message history (scrollable)
- Input line with cursor
- Status bar (char count, send instructions)

**Key handling for SCREEN_COMPOSE:**
```c
void ui_handle_key(ui_state_t *state, char key, uint32_t now_ms) {
    if (state->current_screen != SCREEN_COMPOSE) return;
    
    if (key == KEY_ENTER) {
        /* Send message — call into mesh layer */
        ui_send_composed_message(state);
        state->compose_buf[0] = 0;
        state->compose_len = 0;
    } else if (key == KEY_BACKSPACE && state->compose_len > 0) {
        state->compose_buf[--state->compose_len] = 0;
    } else if (key >= 0x20 && key <= 0x7E && 
               state->compose_len < COMPOSE_BUF_MAX - 1) {
        state->compose_buf[state->compose_len++] = key;
        state->compose_buf[state->compose_len] = 0;
    }
    state->screen_dirty = true;
}
```

Add to `ui_state_t`:
```c
#define COMPOSE_BUF_MAX 200
char compose_buf[COMPOSE_BUF_MAX];
int  compose_len;
int  msg_scroll_pos;    /* scroll position in message list */
int  node_scroll_pos;   /* scroll position in node list */
```

### 9.3 Navigation Mapping (Trackball → UI)

| Trackball Event | Action |
|----------------|--------|
| `BTN_UP` | Scroll up in list / previous screen |
| `BTN_DOWN` | Scroll down in list / next screen |
| `BTN_LEFT` | Go back / cancel |
| `BTN_RIGHT` | Open / select / detail view |
| `BTN_SELECT` | Confirm / enter compose mode |
| Keyboard ENTER | Send message (SCREEN_COMPOSE) |
| Keyboard ESC/Alt | Exit compose / cancel |

Update `ui_handle_button()` to handle the new events:

```c
void ui_handle_button(ui_state_t *state, ui_button_t btn, uint32_t now_ms) {
    state->last_activity = now_ms;
    
    switch (state->current_screen) {
    case SCREEN_MAIN:
        if (btn == BTN_DOWN || btn == BTN_SHORT_PRESS) {
            state->current_screen = SCREEN_MESSAGES;
            state->screen_dirty = true;
        }
        break;
    case SCREEN_MESSAGES:
        if (btn == BTN_UP) { state->msg_scroll_pos--; state->screen_dirty = true; }
        if (btn == BTN_DOWN) { state->msg_scroll_pos++; state->screen_dirty = true; }
        if (btn == BTN_SELECT || btn == BTN_RIGHT) {
            state->current_screen = SCREEN_COMPOSE;
            state->screen_dirty = true;
        }
        if (btn == BTN_LEFT) {
            state->current_screen = SCREEN_MAIN;
            state->screen_dirty = true;
        }
        break;
    /* ... etc ... */
    }
}
```

### 9.4 Render Functions for T-Deck Plus

Update `render_screen()` in `main.c` to use the larger display area. Key changes:
- More message history visible (10+ lines instead of 3)
- Larger fonts for headers
- Status bar at bottom with battery %, GPS fix indicator, node count
- Map screen (if GPS has fix): simple text grid showing nearby node positions

**Status bar (bottom 20px):**
```c
static void render_status_bar(void) {
    int y = DISPLAY_HEIGHT - 20;
    display_hline(0, y, DISPLAY_WIDTH);
    
    /* Battery */
    uint8_t batt = board_get_battery_pct();
    char batt_str[16];
    snprintf(batt_str, sizeof(batt_str), "BAT:%d%%", batt);
    display_draw_text(0, y + 4, batt_str);
    
    /* GPS fix indicator */
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    display_draw_text(80, y + 4, gps_has_fix() ? "GPS:OK" : "GPS:--");
#endif

    /* Mesh uptime */
    // ...
}
```

---

## 10. Battery Monitoring

Battery ADC is on GPIO4 (ADC1_CH3 on ESP32-S3). Implementation is in `board.c` (`board_get_battery_pct()`), already described in section 2.2.

**Wire into UI:**
- Main screen: show battery % in status bar
- RPC API: expose `bramble.getBattery` method that returns `{"pct": N}`
- Update `mesh_shared_state_t` to include `uint8_t battery_pct`

**Add to `rpc_methods.c`:**
```c
static cJSON *rpc_get_battery(cJSON *params, void *ctx) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "pct", board_get_battery_pct());
    return result;
}
// Register: rpc_register("bramble.getBattery", rpc_get_battery, NULL);
```

---

## 11. Audio Subsystem (Scaffolding)

Audio is not in Bramble's core MVP but the T-Deck Plus has full hardware. Set up the foundation:

### 11.1 Create `components/audio/` (Stub)

```c
/* components/audio/include/audio.h */
#ifndef BRAMBLE_AUDIO_H
#define BRAMBLE_AUDIO_H

/* T-Deck Plus audio: ES7210 microphone codec + MAX98357A speaker amp */
/* This is a stub — full implementation deferred post-MVP */

int audio_init(void);           /* Init I2S + ES7210 */
void audio_deinit(void);        /* Release I2S resources */
int audio_record_start(void);   /* Start mic capture */
int audio_record_stop(void);    /* Stop mic capture */
int audio_play(const uint8_t *pcm, size_t len);  /* Play samples */

#endif
```

**Constraint documented:** When audio is active and ES7210 MCLK (GPIO48) is enabled, do NOT simultaneously use the trackball center (GPIO0) as it shares circuitry. Enforce this in software with a mutex or state flag.

---

## 12. RPC API Extensions

Add new T-Deck Plus-specific RPC methods to `main/rpc_methods.c`:

| Method | Parameters | Returns | Notes |
|--------|-----------|---------|-------|
| `bramble.getBattery` | — | `{"pct": N}` | Battery percentage |
| `bramble.getGpsPosition` | — | `{"lat": F, "lon": F, "alt": N, "valid": B}` | Current GPS fix |
| `bramble.setBacklight` | `{"level": N}` | `{}` | Display brightness 0-255 |
| `bramble.getStorageInfo` | — | `{"sd_present": B, "sd_free_kb": N}` | SD card status |

---

## 13. Testing Plan

### 13.1 Unit Tests (Host-side, existing test framework)

No changes needed to existing tests. Add:

- `test/test_nmea_parser.c` — Test NMEA sentence parsing for GPRMC and GPGGA
- `test/test_keyboard.c` — Test keyboard buffer/callback logic (mock I2C)
- `test/test_trackball.c` — Test event counting and priority logic

### 13.2 Hardware Integration Test Sequence

Test in this exact order to avoid confusion from the BOARD_POWERON issue:

**Phase 1: Power + Radio (Day 1)**
1. Flash minimal build (board init only, LED blink, no display)
2. Verify GPIO10 powers peripherals (measure with multimeter)
3. Bring up radio: verify SX1262 responds to GetStatus command
4. Send LoRa packet from T-Deck Plus, receive on Heltec V3
5. Verify RSSI/SNR values are reasonable

**Phase 2: Display (Day 2)**
1. Flash with ST7789 driver enabled
2. Verify PSRAM allocation (check heap stats)
3. Render test pattern (red/green/blue stripes)
4. Render Bramble splash screen
5. Verify no SPI conflicts with radio (alternate TX and display flush)

**Phase 3: Keyboard (Day 3)**
1. Flash with keyboard component enabled
2. Verify I2C scan detects keyboard at expected address
3. Type characters and verify they arrive in the callback
4. Test special keys (backspace, enter, alt)
5. Test compose screen end-to-end

**Phase 4: Trackball (Day 3-4)**
1. Flash with trackball component enabled
2. Verify ISR fires on ball movement (log count via CLI)
3. Test all 4 directions + center
4. Verify UI navigation with trackball

**Phase 5: GPS (Day 4-5)**
1. Flash with GPS enabled
2. Connect to serial monitor, verify raw NMEA appears on console
3. Wait for outdoor fix (MIA-M10Q can take 30-90s cold start)
4. Verify position appears in `bramble.getGpsPosition` RPC response
5. Verify location subsystem broadcasts position to mesh

**Phase 6: SD Card (Day 5)**
1. Insert formatted FAT32 microSD card
2. Flash with SD card enabled
3. Verify mount in serial log
4. Write/read test file
5. Test graceful behavior with no card (should not crash)

**Phase 7: Full Integration (Day 6)**
1. All subsystems enabled simultaneously
2. Boot and verify all init messages in serial log
3. Send messages via compose screen
4. Verify messages received on Heltec V3
5. Verify GPS position visible in webapp
6. Verify battery % visible in webapp

### 13.3 Regression Tests

After all T-Deck Plus work, build for Heltec V3 and verify:
- No compile errors (all board-specific code behind `#ifdef`)
- All existing unit tests still pass
- Heltec V3 functionality unchanged

---

## 14. Component CMakeLists.txt Summary

Update `main/CMakeLists.txt` to include new components conditionally:

```cmake
set(main_srcs
    "main.c"
    "board.c"
    "mesh_task.c"
    "cli.c"
    "rpc_methods.c"
    "ws_server.c"
)

set(main_requires
    display button ui crypto identity radio
    routing packet dedup airtime reliability
    coding fragment channel group mailbox
    emergency location timesync security
    rpc wifi ble bramble_probe msg_store
    nvs_flash esp_wifi mdns
)

if(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
    list(APPEND main_srcs "")  # board.c already included
    list(APPEND main_requires keyboard trackball gps sdcard)
endif()

idf_component_register(
    SRCS ${main_srcs}
    INCLUDE_DIRS "."
    REQUIRES ${main_requires}
)
```

---

## 15. Implementation Order (Critical Path)

Work in this order to maximize early testability:

```
Week 1:
Day 1:  Kconfig + sdkconfig + board.h/board.c + BOARD_POWERON fix
Day 2:  sx1262.c SPI refactor + TCXO fix + radio verification
Day 3:  ST7789 driver (skeleton → framebuffer → flush → text)
Day 4:  UI resize + new screens + status bar
Day 5:  Keyboard component + compose screen

Week 2:
Day 6:  Trackball component + navigation
Day 7:  GPS component + NMEA parser + location integration
Day 8:  SD card component
Day 9:  Battery ADC + RPC extensions
Day 10: Full integration test + regression on Heltec V3
Day 11: Bug fixes, polish
Day 12: Documentation + tag release
```

---

## 16. Key Files Modified (Summary)

| File | Change Type | Description |
|------|------------|-------------|
| `main/Kconfig.projbuild` | Modify | Add TDECK_PLUS board choice |
| `sdkconfig.defaults.tdeck_plus` | Create | T-Deck Plus sdkconfig |
| `partitions.tdeck_plus.csv` | Create | 16 MB partition layout |
| `main/board.h` | Create | Board abstraction API |
| `main/board.c` | Create | Power init + shared SPI bus |
| `main/board_tdeck_plus.h` | Create | All pin definitions |
| `main/main.c` | Modify | Call board_init(), add GPS/keyboard/trackball init |
| `components/radio/include/sx1262.h` | Modify | Board-conditional pin defines |
| `components/radio/sx1262.c` | Modify | Conditional SPI bus init + TCXO |
| `components/display/include/display.h` | Modify | Board-conditional dimensions |
| `components/display/st7789.c` | Create | New ST7789 driver |
| `components/display/CMakeLists.txt` | Modify | Select driver by board |
| `components/ui/include/ui.h` | Modify | New button events, compose buf |
| `components/ui/ui_manager.c` | Modify | Handle new events |
| `components/button/button.c` | Modify | No-op on T-Deck Plus |
| `components/keyboard/` | Create | Full keyboard component |
| `components/trackball/` | Create | Full trackball component |
| `components/gps/` | Create | NMEA GPS component |
| `components/sdcard/` | Create | SD card VFS component |
| `components/audio/` | Create | Audio stub |
| `main/rpc_methods.c` | Modify | New battery/GPS/backlight/SD RPCs |
| `scripts/flash.sh` | Modify | Add tdeck-plus target |
| `main/CMakeLists.txt` | Modify | Add new components |

---

## 17. Open Questions (Resolve Before Starting)

1. **TCXO vs Crystal on T-Deck Plus** — Check schematic at https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/schematic/schematic.pdf. Does DIO3 control a TCXO or is a crystal fitted directly? This affects whether to call `sx1262_set_dio3_as_tcxo()`.

2. **DC-DC vs LDO mode** — Heltec V3 uses DC-DC (`set_regulator_mode(1)`). T-Deck Plus schematic will reveal if DC-DC is present. Wrong setting causes radio instability.

3. **T-Keyboard I2C address** — Likely 0x55 based on the T-Keyboard library, but verify from source code. Also confirm whether the C3 sends the key byte directly or uses a register-based protocol.

4. **Battery voltage divider ratio** — The schematic shows the divider for GPIO4. Without this, battery % calculations will be wrong. Measure actual ADC value at known battery voltage to calibrate if schematic is unavailable.

5. **ST7789 init sequence** — Use exactly the bytes from commit 6adb888 in the T-Deck repo. Do not use the standard Arduino ST7789 library sequence; it does not work for this display.

6. **Trackball direction mapping** — G01/G02/G03/G04 are labeled by the spec as up/down/left/right but physical verification is needed. Run a test that logs which GPIO fires when you roll in each direction.

---

## Appendix: T-Deck Plus Pin Map (Complete)

```
GPIO  | Function            | Notes
------|--------------------|-----------------------------------------
0     | BOOT / Trackball ↓ | Active-low boot button = trackball center
1     | TBOX_G04 (right)   | Hall Effect sensor
2     | TBOX_G02 (down)    | Hall Effect sensor
3     | TBOX_G01 (up)      | Hall Effect sensor
4     | BAT_ADC            | Battery voltage via voltage divider
5     | I2S_WS             | Audio
6     | I2S_DOUT           | Audio speaker out
7     | I2S_BCK            | Audio bit clock
8     | I2C_SCL            | Keyboard + touch
9     | RADIO_CS           | SX1262 chip select
10    | BOARD_POWERON      | Peripheral power rail (HIGH = on)
11    | TFT_DC             | ST7789 data/command
12    | TFT_CS             | ST7789 chip select
13    | RADIO_BUSY         | SX1262 BUSY line
14    | ES7210_DIN         | Audio codec
15    | TBOX_G03 (left)    | Hall Effect sensor
16    | TOUCH_INT          | (unused — no touch on T-Deck Plus)
17    | RADIO_RST          | SX1262 reset
18    | I2C_SDA            | Keyboard + touch
21    | ES7210_LRCK        | Audio codec
38    | SPI_MISO           | Shared SPI bus
39    | SDCARD_CS          | SD card chip select
40    | SPI_SCK            | Shared SPI bus
41    | SPI_MOSI           | Shared SPI bus
42    | TFT_BACKLIGHT      | Display backlight (HIGH = on)
43    | GPS_TX (ESP RX)    | MIA-M10Q receive pin
44    | GPS_RX (ESP TX)    | MIA-M10Q transmit pin
45    | RADIO_DIO1         | SX1262 interrupt
46    | KEYBOARD_INT       | T-Keyboard interrupt (active-low)
47    | ES7210_SCK         | Audio codec
48    | ES7210_MCLK        | Audio codec master clock
```
