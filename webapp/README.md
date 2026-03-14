## Desktop App (Electron)

The webapp can be packaged as a cross-platform desktop application using Electron.

### Development

```bash
# Web-only development (no Electron)
npm run dev

# Electron development (hot-reload)
npm run dev:electron
```

### Building

```bash
# Build for current platform
npm run package

# Platform-specific builds
npm run package:linux   # AppImage + deb
npm run package:mac     # dmg
npm run package:win     # nsis installer
```

Build output goes to `release/`.

### Supported Platforms

| Platform | Installer | Serial | BLE | WebSocket |
|----------|-----------|--------|-----|-----------|
| Linux    | AppImage, deb | ✅ | ✅ | ✅ |
| macOS    | dmg       | ✅ | ✅ | ✅ |
| Windows  | exe (NSIS)| ✅ | ✅ | ✅ |

All three transport types (USB Serial, Bluetooth LE, WiFi WebSocket) work in the Electron app because it uses Chromium's Web Serial and Web Bluetooth APIs natively.
