## Web Dev Workflow

### Testing WiFi transport under `npm run dev`

Plain `npm run dev` cannot exercise the WiFi transport or local-mode capabilities
on its own: `/api/capabilities` is only served by `server/unified-server.mjs`, so
without a backend the client falls back to hosted-mode defaults and the WiFi button
is disabled.

The vite dev server has a proxy configured that forwards `/api/*` and `/ws*` to a
local unified-server instance. To use it:

1. Start the unified server in a terminal:

   ```bash
   # default port 8085; MODE=local enables the WiFi/ws-proxy features
   MODE=local node server/unified-server.mjs
   ```

2. In a second terminal, start the dev server:

   ```bash
   npm run dev
   ```

3. Verify the proxy is working:

   ```bash
   curl http://localhost:5173/api/capabilities
   # expected: {"mode":"local","proxyEnabled":true,"localLanAllowed":true,...}
   ```

If your unified server runs on a non-default port, set `VITE_API_PROXY_TARGET`
before starting vite:

```bash
VITE_API_PROXY_TARGET=http://localhost:9000 npm run dev
```

The proxy target defaults to `http://localhost:8085` (the default unified-server
port). Without a backend running the vite dev server still works; capabilities just
fall back to hosted mode (USB and BLE remain available).

## Desktop App (Electron)

The webapp can be packaged as a cross-platform desktop application using Electron.

For user-facing docs (installing, connecting to a node, Nearby nodes, troubleshooting), see [../docs/webapp/desktop.md](../docs/webapp/desktop.md).

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

All three transport types (USB Serial, Bluetooth LE, WiFi WebSocket) work in the Electron app via Chromium's Web Serial and Web Bluetooth APIs. Web Bluetooth is not on by default in Electron: the shell enables it with `app.commandLine.appendSwitch('enable-features', 'WebBluetooth')` and handles `select-serial-port` / `select-bluetooth-device` events with its own device-picker modal (`electron/main.ts`, `DevicePickerModal.tsx`) instead of a browser chooser.

### LAN discovery

The desktop app discovers Bramble nodes on the local network via mDNS
(`_bramble._tcp`) and lists them under "Nearby nodes" in the WiFi connect
panel. Nodes running firmware with mDNS TXT records (addr/name) are matched
against the device book for one-click reconnect with the saved token. The
desktop app connects directly to `ws://<node-ip>/ws`; no ws-proxy or unified
server is involved.
