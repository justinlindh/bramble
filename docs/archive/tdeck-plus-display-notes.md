# T-Deck Plus Display: Debugging Notes & Discoveries

**Date:** 2026-02-19  
**Hardware:** LILYGO T-Deck Plus, ST7789 320×240 IPS LCD

## Root Cause of "Small Corner" Bug

The display was only rendering to a small portion of the screen. After extensive debugging (GRAM clears, CASET/RASET swaps, DMA buffer changes, CS pin pre-init), the root cause was:

**`display.h` was not properly including `sdkconfig.h`**, so `DISPLAY_WIDTH` and `DISPLAY_HEIGHT` always resolved to the Heltec V3 defaults (128×64) instead of the T-Deck Plus values (320×240). The framebuffer, flush region, and all UI layout were sized for 128×64 on a 320×240 screen — hence only a small corner was used.

**Fix:** Commit `854d404` — ensure `sdkconfig.h` is included in `display.h` so board-specific display dimensions are used.

## What Didn't Fix It (But Was Still Correct)

These fixes were applied during debugging. They didn't solve the core issue but are correct for other reasons:

1. **TCXO + DC-DC config** (`0cd844a`) — The research doc said T-Deck Plus uses crystal + LDO. **Wrong.** Meshtastic source confirms: `SX126X_DIO3_TCXO_VOLTAGE 1.8` + DC-DC. This fixed the radio (peers discovered, TX/RX working).

2. **CS pins HIGH before SPI bus init** (`8c512fb`) — Meshtastic's `earlyInitVariant()` drives all SPI CS pins HIGH before bus initialization. Prevents shared bus crosstalk. Good practice for shared SPI.

3. **PSRAM framebuffer with internal DMA copy buffer** — 153,600 bytes for RGB565 framebuffer must be in PSRAM. ESP32 DMA can't always read from PSRAM directly, so flush copies chunks to an internal RAM buffer before SPI transfer.

## Key Hardware Facts (Corrected)

| Parameter | Research Doc Said | Actual (Meshtastic-confirmed) |
|-----------|-------------------|-------------------------------|
| Radio oscillator | Crystal | **TCXO via DIO3 (1.8V)** |
| Radio regulator | LDO | **DC-DC** |
| Display orientation | Landscape native | **Portrait 240×320, rotated via MADCTL** |
| SPI bus | Per-device init | **Shared bus, single init, CS arbitration** |

## Current Display State

- Full 320×240 screen renders correctly
- Text is small — font was designed for 128×64 OLED and hasn't been scaled
- **Future work:** Rich graphical UI with proper font scaling, icons, layout for the IPS display

## Meshtastic Reference Points

- Same `t-deck` variant covers both T-Deck and T-Deck Plus
- Uses LovyanGFX with `bus_shared=true`, `use_lock=true`
- Panel: `TFT_WIDTH=240, TFT_HEIGHT=320` with `SCREEN_ROTATE`
- `cfg.invert = true` for ST7789
- Source: `variants/esp32s3/t-deck/variant.h`
