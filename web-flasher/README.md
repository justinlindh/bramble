# Bramble Web Flasher

Web Serial-based firmware flasher for Bramble boards.

## UX model
- Simple mode is default: board select, connect, flash, compact status.
- Advanced panel (collapsed by default) includes:
  - release channel selector (`stable` / `dev`)
  - release selector
- Last selected board is persisted in localStorage.

## Release filtering contract
Only complete releases are shown. A release is considered complete only if it contains the canonical artifacts for **all** supported boards (`heltec-v3`, `tdeck-plus`, `heltec-v4`):
- `bootloader.bin`
- `partition-table.bin`
- `bramble.bin`
