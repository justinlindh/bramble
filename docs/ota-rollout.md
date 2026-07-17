# OTA Rollout Guide (Single Node)

Last verified: 2026-07-16 (heltec-v4 bench, HTTP dev loop, upgrade + floor-gated downgrade)

Use this runbook to deploy a `bramble.bin` build to one WiFi-connected node
without USB flashing.

Two things changed with signed OTA (see `docs/design/ota-signing.md`):

- **Images are signed and verified.** The node only installs images signed by
  a key that also signed its running firmware. A node running CI-built
  firmware accepts CI-signed images; a node USB-flashed with your local dev
  build accepts images signed with that same dev key.
- **URLs are gone from the OTA RPC.** The node downloads only from its
  configured OTA origin; `bramble.otaUpdate` takes a relative artifact path.

## Prerequisites

- Node reachable over JSON-RPC WebSocket (example: `ws://192.0.2.179/ws`)
- RPC auth token for the node (all OTA methods are authenticated)
- A JSON-RPC client. `bramble-cli`'s `ota --url` predates the path-based
  contract (tracked as bramble-cli#36); until that lands, drive the RPCs
  directly, e.g. with `websocat` as shown below (`TOKEN` is the device auth
  token)
- Choose target board: `heltec-v3`, `heltec-v4`, or `tdeck-plus`

## A0) Webapp rollout (recommended)

For most upgrades, skip the JSON-RPC recipes below and use the webapp: open
Config, then Device Management, then Firmware Update. Pick the version from
the list (release notes, if published, show alongside each entry), watch the
progress indicator while the node downloads and installs the image, and wait
for the node to reboot; the UI confirms the new running version once it
reconnects.

CORS caveat: when the webapp runs in a BROWSER from a different origin than
the OTA server (for example, the webapp on `https://app.example` fetching an
index from `https://ota.example`), the release-index fetch (`index.json`)
happens in the page, so the OTA origin server must send an
`Access-Control-Allow-Origin` header (the webapp's origin, or `*`) on
`index.json`, or the browser blocks the fetch. The packaged Electron desktop
app routes that fetch through its main process (`net.fetch`) instead of the
renderer, so it is not subject to CORS and works regardless of the OTA
server's headers. The Android app lives in a separate repository
(`bramble-android`) and is not covered by this document.

## A) Production rollout (official origin)

The default origin is `https://bramblemesh.org/ota/`. CI publishes signed
artifacts there for every release; the index at
`https://bramblemesh.org/ota/index.json` lists artifact paths per release
(`docs/ota-release-schema.md`).

Trigger the update with the artifact path relative to the origin:

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"bramble.otaUpdate","params":{"path":"stable/v1.4.0/heltec-v3/bramble.bin"}}' \
  | websocat -n1 "ws://192.0.2.179/ws?token=$TOKEN"
```

Expected: `ok=true` with the resolved URL echoed back, then device
reboot/disconnect.

## B) Dev-loop rollout (local origin, dev-signed build)

The node must already be running a build signed with YOUR dev key (USB-flash
it once with `scripts/flash.sh`; the build signs with
`$BRAMBLE_OTA_SIGNING_KEY` or a generated throwaway in `keys/`).

1. Build the board artifact:

   ```bash
   cd ~/src/bramble
   bash scripts/flash.sh local heltec-v4 build
   ```

2. Host the build directory over HTTP (requires firmware compiled with
   `CONFIG_BRAMBLE_OTA_ALLOW_HTTP`; https origins work on any build):

   ```bash
   cd ~/src/bramble/build-heltec-v4
   python3 -m http.server 8088
   ```

3. Point the node's OTA origin at your machine (persists in NVS until reset):

   ```bash
   printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"bramble.otaSetOrigin","params":{"origin":"http://203.0.113.34:8088/"}}' \
     | websocat -n1 "ws://192.0.2.179/ws?token=$TOKEN"
   ```

4. Trigger the update:

   ```bash
   printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"bramble.otaUpdate","params":{"path":"bramble.bin"}}' \
     | websocat -n1 "ws://192.0.2.179/ws?token=$TOKEN"
   ```

5. When done iterating, restore the official origin:

   ```bash
   printf '%s\n' '{"jsonrpc":"2.0","id":3,"method":"bramble.otaSetOrigin","params":{"reset":true}}' \
     | websocat -n1 "ws://192.0.2.179/ws?token=$TOKEN"
   ```

`bramble.otaGetOrigin` shows the effective origin, whether it is overridden,
the anti-rollback floor, and the running version.

## Downgrades

The node refuses images whose version is below its anti-rollback floor (the
highest version it has booted). For a deliberate downgrade:

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"bramble.otaUpdate","params":{"path":"stable/v1.3.9/heltec-v3/bramble.bin","allow_downgrade":true}}' \
  | websocat -n1 "ws://192.0.2.179/ws?token=$TOKEN"
```

This also lowers the floor to the downgraded version.

## Verify reconnect + version/health

After reboot, verify the node reconnects and responds:

```bash
cd ~/src/bramble-cli
./bramble --transport ws://192.0.2.179/ws ping
./bramble --transport ws://192.0.2.179/ws status --json
```

Optional post-checks:

```bash
./bramble --transport ws://192.0.2.179/ws monitor --topic gps --follow --since 2m
```

## Troubleshooting and rollback

- **"image signature verification failed"** in the device log (`ota` tag):
  the image is unsigned or signed with a key the running firmware does not
  trust. Dev-signed node + CI image (or vice versa) requires a USB flash to
  cross trust domains.
- **"below the anti-rollback floor"**: intentional downgrade needs
  `allow_downgrade:true`.
- The most recent failure reason is also returned as `last_error` on the next
  `bramble.otaUpdate` call.

If the node becomes unhealthy after OTA:

1. Re-run OTA with a known-good artifact path (add `allow_downgrade:true` if
   stepping back a version).
2. If the node is unreachable over WiFi, recover via USB flash:
   ```bash
   cd ~/src/bramble
   bash scripts/flash.sh local heltec-v4 flash /dev/ttyACM0
   ```
3. Re-verify with `ping` and `status --json` after recovery.
