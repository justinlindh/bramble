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

## Provisioning model

A node with no network key is INERT: it neither emits nor accepts authenticated control-plane traffic, so it does not mesh at all. The Device Setup step therefore offers a **Network Key** field, and the flow is deliberately asymmetric:

- **Join** (offered here): paste a `bramble://net/v1?k=...` share string or a bare 64-hex key from a node already on the mesh. It is validated in the browser first, then provisioned over the already-open serial link with `bramble.setNetworkKey`, before the closing reboot. The completion screen shows the key's fingerprint so the operator can compare it against the founder node.
- **Found** (not offered here): minting a fleet's root key belongs in the web app, which has the QR code, the copy-confirm, the persistent fingerprint readout, and the re-key guard. A one-shot page the user closes cannot offer those, and the key is never recoverable from a device afterwards.

Skipping setup, or submitting with the field blank, ends on a completion screen that states plainly that the node is UNPROVISIONED and will not mesh, and points at the web app. A node that looks configured but is silently inert is the failure this avoids.

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
- `network-key.js`: network-key share-string parsing and fingerprint derivation
- `style.css`: styling

## Browser requirements

Web Serial API is required. Supported in Chrome and Edge (desktop). Not supported in Firefox or Safari.

## Running the tests

The `*.test.js` files use node:test and run in CI (the web-flasher-tests job). Locally:

```bash
node --test 'web-flasher/**/*.test.js'
```

Use Node 24 (the CI version, pinned by [`.nvmrc`](../.nvmrc)). Quote the glob: it is expanded by the test runner, and the older bare-directory form (`node --test web-flasher/`) resolves the directory as a module entry point and fails with MODULE_NOT_FOUND on Node 22 and newer.
