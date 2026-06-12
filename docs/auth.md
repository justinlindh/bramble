# Bramble RPC Authentication

Bramble devices expose a JSON-RPC API over WebSocket, BLE, and serial.
WebSocket and BLE require a per-device **auth token**. Serial does not:
physical access is the trust anchor.

## Quick Summary

| Transport | Default | How it authenticates |
|-----------|---------|----------------------|
| WebSocket | Token required | `Authorization: Bearer <token>` header or `?token=` query parameter |
| BLE | Token required | First write on a new connection is the token |
| Serial (USB) | No auth, by design | Physical access is the pairing bootstrap |

On **first boot** the firmware generates a random 32-character hex token
and stores it in NVS. There is no out-of-the-box open access; auth is on
by default. The token persists across reboots and firmware updates.

## Pairing: Getting the Token

The token leaves the device over serial only. Connect via USB and run:

```bash
bramble pair                         # auto-detect serial device
bramble pair -p /dev/ttyACM0         # specific port
bramble pair --json                  # machine-readable
bramble pair --save                  # save to ~/.config/bramble/tokens.json
```

The token is also printed in the serial boot log when it is first
generated, so anyone watching the console during setup can copy it.

### What unauthenticated clients can do

A WS or BLE client without the token can connect, but may call only the
identification allowlist: `bramble.ping` and `bramble.getVersion`. That is
enough for a pairing UI to confirm it is talking to a Bramble device and
prompt for the token. Every other method answers `Unauthorized` (-1005).

## Setting a Custom Token

Tokens must be **at least 16 bytes**; shorter values are rejected
(`Invalid params`). The device-generated token is 32 hex chars (128 bits).

### During Setup (Web Flasher)

When flashing via the [web flasher](https://bramblemesh.org/web-flasher/),
expand the **🔒 Security** section in the Device Setup step to set your
own token or generate a random one. If you leave it blank, the device
generates its own on first boot; retrieve it with `bramble pair`.

### From the CLI

```bash
bramble auth status                  # check current auth status
bramble auth enable                  # enable with a random token
bramble auth enable --set <token>    # enable with a specific token (>= 16 bytes)
bramble auth disable                 # explicit opt-out (open access)
```

The `auth` commands work over serial (USB).

### Via JSON-RPC

```json
{"jsonrpc":"2.0","id":1,"method":"bramble.setAuthToken","params":{"token":"my-token-of-16-bytes-or-more"}}
```

To read the current token (serial or authenticated clients only):
```json
{"jsonrpc":"2.0","id":1,"method":"bramble.getAuthToken","params":{}}
```

## Disabling Auth (Explicit Opt-Out)

Setting an empty token disables auth and persists that choice:

```json
{"jsonrpc":"2.0","id":1,"method":"bramble.setAuthToken","params":{"token":""}}
```

The device will NOT regenerate a token on the next boot; it stays open
until a token is set again. The call itself requires auth (or serial), so
only a token holder or someone with physical access can open a device up.
The firmware logs a warning at every boot while auth is disabled.

## Browser Origin Allowlist

For connections that do not present the valid token, the WebSocket
endpoint validates the `Origin` header browsers send: same-origin
connections (the device's own IP or hostname, any port) are allowed,
everything else is rejected unless added to the device's origin
allowlist. Connections that present the valid token skip the Origin
check, so the web app works from any origin once you enter the token:

```json
{"jsonrpc":"2.0","id":1,"method":"bramble.setAllowedOrigins","params":{"origins":["https://app.example.com"]}}
{"jsonrpc":"2.0","id":1,"method":"bramble.getAllowedOrigins","params":{}}
```

Non-browser clients (CLI, SDKs) send no Origin header and are unaffected.

## Connecting with a Token

### CLI

```bash
bramble --token <token> -t ws://192.168.1.100/ws status
# or
export BRAMBLE_TOKEN=<token>
bramble -t ws://192.168.1.100/ws status
```

### Web App

Enter the token in the **Auth Token** field of the WiFi connection dialog.

### Browser WebSocket API

The browser's WebSocket API cannot set HTTP headers, so for web pages the
query parameter is the supported authentication mechanism, not a
deprecated one:

```
ws://192.168.1.100/ws?token=<token>
```

Non-browser clients should use the `Authorization` header instead, since
URLs leak via logs and history.

### Other Clients (curl, Python, Go, etc.)

Use the `Authorization: Bearer` header:

```bash
# Example with websocat
websocat ws://192.168.1.100/ws -H "Authorization: Bearer <token>"
```

```python
import websockets
headers = {"Authorization": f"Bearer {token}"}
async with websockets.connect(url, additional_headers=headers) as ws:
    ...
```

## Security Notes

- Tokens are transmitted in cleartext over HTTP/WebSocket. On untrusted
  networks, treat the token as exposed to anyone who can capture your
  traffic. See `docs/SECURITY-MODEL.md` for the full threat model.
- Serial (USB) connections always bypass auth. This is intentional:
  physical access to the device is root-level trust, and serial is how
  you recover a forgotten token.
- The token is stored in plaintext in NVS on the device. A flash dump
  extracts it (tracked in `docs/SECURITY-MODEL.md` known gaps).
- BLE uses a first-write token handshake with throttled retries.
- If the token store fails (NVS error), the device fails closed: full RPC
  access is unavailable rather than silently open.

## Defaults

- **New devices**: token auto-generated on first boot; auth required.
- **Firmware updates**: token persists across OTA and USB firmware
  updates (stored in NVS, separate from firmware partitions).
- **Factory reset**: erasing NVS clears the token AND the opt-out flag;
  a fresh token is generated on the next boot.
