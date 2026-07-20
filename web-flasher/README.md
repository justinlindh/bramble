# Bramble Web Flasher

Web Serial-based firmware flasher for Bramble boards, powered by [esptool-js](https://github.com/espressif/esptool-js) (Espressif official).

## Dependencies

- **esptool-js v0.5.7**, loaded via CDN: `https://unpkg.com/esptool-js@0.5.7/bundle.js`
  - Exposes a global `esptool` object (`esptool.Transport`, `esptool.ESPLoader`)
  - No build step required; all files are vanilla JS served as static assets

## Supported Boards

| Board | Chip | Flash |
| ------- | ------ | ------- |
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | 8MB |
| LILYGO T-Deck Plus | ESP32-S3 | 16MB |
| Heltec V4 | ESP32-S3 | 8MB |

## UX model

- Simple mode is default: board select, connect, flash, compact status.
- Advanced panel (collapsed by default) includes:
  - release channel selector (`stable` / `dev`)
  - release selector
- Last selected board is persisted in localStorage.
- On connect, the chip is auto-detected and shown in the status bar.

## Release filtering contract

Only complete releases are shown. A release is considered complete only if it contains the canonical artifacts for **all** supported boards (`heltec-v3`, `tdeck-plus`, `heltec-v4`):

- `bootloader.bin`
- `partition-table.bin`
- `bramble.bin`

## Architecture

- `index.html`: UI shell; loads `release-index.js`, `esptool-js` bundle, then `flasher.js`
- `release-index.js`: release index normalization/filtering logic (unchanged by flasher migration)
- `flasher.js`: UI controller; uses `esptool.Transport` + `esptool.ESPLoader` for connect/sync/flash

- `wifi-config.js`: WiFi credential provisioning during flash
- `style.css`: styling

## Browser requirements

Web Serial API is required. Supported in Chrome and Edge (desktop). Not supported in Firefox or Safari.

## Running the tests

The `*.test.js` files use node:test and run in CI (the web-flasher-tests job). Locally:

```bash
node --test web-flasher/
```

Use Node 20 (the CI version). Node 25 fails on a bare directory argument with MODULE_NOT_FOUND; pass explicit file paths there instead.
