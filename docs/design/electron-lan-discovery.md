# Electron Desktop: Direct LAN Connect + Node Discovery

**Date:** 2026-07-07
**Status:** Approved design, pre-implementation

## Problem

The hosted webapp (https) cannot open `ws://<node-ip>/ws` connections to nodes on
the local network: Chrome blocks insecure WebSockets from secure origins (mixed
content) and restricts public-to-private requests (Private Network Access). The
hosted deployment works around this with a server-side ws-proxy, but a browser
pointed at the hosted app on an arbitrary LAN still cannot reach nodes directly.

The repo already ships an Electron shell (`webapp/electron/`, electron-vite,
electron-builder for Linux/macOS/Windows) whose renderer loads from `file://`,
where neither restriction applies. But the WiFi transport is dead there today:
capability detection fetches `/api/capabilities`, which only the unified server
serves. Under `file://` the fetch fails and the app falls back to hosted-mode
defaults (`localLanAllowed: false`), disabling the WiFi connect UI.

## Goals

1. WiFi (LAN WebSocket) transport works in the packaged Electron app, connecting
   directly to `ws://<node-ip>/ws` with no proxy.
2. The desktop app discovers nodes on the LAN via mDNS and offers one-click
   connect, merged with the device book (saved names + tokens).
3. Zero behavior change for the web deployments (hosted and local Docker).

Non-goals: auto-update, tray, deep links, packaging/signing changes, AP-mode
(192.168.4.1) discovery (manual IP entry continues to cover AP mode).

## Design

### 1. Capabilities short-circuit (the unblock)

`webapp/src/lib/connectionMode.ts`: `fetchConnectionCapabilities()` returns
`{ mode: 'local', localLanAllowed: true }` immediately when `isElectron()`
(from `src/utils/platform.ts`, backed by the preload's `window.isElectron`)
is true, skipping the `/api/capabilities` fetch. No new runtime mode: desktop
is local mode as far as the UI cares.

This alone enables direct connect. `buildWifiUrl()` already yields
`ws://<ip>/ws` for non-https origins, and the renderer's `WebSocketTransport`
needs no changes under `file://`.

### 2. Spike risk to verify first

The one empirical unknown is Chromium's mixed-content treatment of `ws://` from
a `file://` document in Electron 41. Expected: allowed. Implementation step one
is a manual test from the packaged shell against a real node. Fallback if
blocked (not expected): move the WebSocket into the main process behind an IPC
transport. The rest of the design assumes direct connect from the renderer.

### 3. Firmware: mDNS TXT records

Nodes already advertise `_bramble._tcp` on port 80 with hostname
`bramble-<low-16-bits-of-addr>` (`main/main.c` mDNS boot stage). Change:

- Advertise TXT records on the service: `addr=<full 8-hex node address>` and
  `name=<mesh_get_node_name()>` (omit `name` when unset).
- Update the TXT records in the `bramble.setNodeName` RPC handler
  (`main/rpc_methods.c`) so renames propagate without a reboot
  (`mdns_service_txt_item_set`).

Old firmware (no TXT) degrades gracefully: the desktop app falls back to
matching device-book entries by the 16-bit hostname suffix, and names appear
only after connecting.

### 4. Main-process discovery module

New `webapp/electron/discovery.ts` using `bonjour-service` (pure JS, so no
native module rebuilds across the three packaged platforms).

- Renderer requests start/stop over IPC (`discovery:start` / `discovery:stop`).
- Main browses `_bramble._tcp`; on every service up/down event it pushes a
  full deduped snapshot to the renderer (`discovery:update`):
  `Array<{ addrHex?: string; name?: string; hostname: string; ip: string; port: number }>`
  (`addrHex`/`name` parsed from TXT when present; dedup key: `addrHex`,
  falling back to `hostname`).
- Browsing runs only while the connection UI is open; main stops it on request
  and when the window closes.
- Errors (mDNS socket bind failure: VPNs, firewalls, multicast-hostile
  networks): log a warning, deliver empty snapshots; the UI degrades to manual
  IP entry. Discovery failure is never surfaced as a blocking error.

### 5. Preload bridge

Extend `webapp/electron/preload.ts` with a typed API via `contextBridge`:

```ts
window.brambleDesktop = {
  startDiscovery(): void,
  stopDiscovery(): void,
  onDiscovered(cb: (nodes: DiscoveredNode[]) => void): () => void, // returns unsubscribe
}
```

A shared `.d.ts` (`webapp/src/types/desktop.d.ts`) declares the global so
renderer code typechecks. The renderer feature-detects `window.brambleDesktop`;
the same bundle runs on web unchanged.

### 6. Renderer UX: Nearby nodes

`ConnectionOverlay`'s WiFi section gains a desktop-only "Nearby nodes" list:

- Discovery snapshots are merged with the device book by full node address
  (pure function, unit-tested). A discovered node matching a book entry shows
  its saved name and one-click connects with the saved token, reusing the
  existing one-click flow, including the DHCP guard (`expectAddressHex`)
  for IP-moved cases.
- Unknown nodes show `name` (TXT) or hostname, with the normal first-connect
  token prompt and the IP prefilled.
- Hostname-suffix fallback matching (old firmware): a book entry whose address
  low 16 bits match the `bramble-XXXX` hostname is presented as a *probable*
  match; the DHCP guard still verifies the full address post-connect.
- Manual IP entry remains below the list, unchanged.

### 7. Testing

- **Unit (vitest):** capabilities short-circuit under `isElectron`; discovery
  snapshot dedup/TXT parsing (module logic testable without sockets);
  book-merge function including suffix-fallback and token selection.
- **Firmware:** build for Heltec; `avahi-browse -r _bramble._tcp` shows TXT
  `addr`/`name`; rename via RPC updates TXT live.
- **End-to-end (manual):** packaged Linux app discovers a live node, one-click
  connects with saved token, and connects direct `ws://` with no proxy running.

## Components touched

| Area | Files | Change |
|------|-------|--------|
| Renderer capabilities | `webapp/src/lib/connectionMode.ts` | Electron short-circuit |
| Firmware mDNS | `main/main.c`, `main/rpc_methods.c` | TXT records + live update on rename |
| Electron main | `webapp/electron/main.ts`, new `webapp/electron/discovery.ts` | discovery module + IPC wiring |
| Preload | `webapp/electron/preload.ts`, new `webapp/src/types/desktop.d.ts` | `brambleDesktop` bridge |
| Renderer UI | `webapp/src/components/ConnectionOverlay.tsx`, store | Nearby nodes list + merge |
| Deps | `webapp/package.json` | `bonjour-service` |
