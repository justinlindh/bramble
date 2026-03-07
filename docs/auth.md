# Bramble WebSocket Authentication

Bramble devices expose a WebSocket API for wireless control and monitoring.
By default, this API is **open** — any client on the same network can connect.
You can optionally set an **auth token** to restrict access.

## Quick Summary

| Setting | Default | What happens |
|---------|---------|--------------|
| No token | ✅ Default | Anyone on the network can connect wirelessly |
| Token set | Opt-in | Clients must provide the token to connect |

## How It Works

When an auth token is configured on a device, WebSocket clients must provide
it to connect. Without it, the device sends a WebSocket close frame
(code 1008 — Policy Violation) and drops the connection.

The token is stored in the device's non-volatile storage (NVS) and persists
across reboots and firmware updates.

**Serial connections (USB) are always unauthenticated.** Physical access to
the device is treated as trusted — this is how you recover if you forget
your token.

## Setting a Token

### During Setup (Web Flasher)

When flashing a new device via the [web flasher](https://bramblemesh.org/web-flasher/),
expand the **🔒 Security** section in the Device Setup step. You can:

- Enter your own token
- Click 🎲 to generate a random one
- Leave it blank for open access (default)

**Save the token** — you'll need it for all wireless connections.

### From the CLI

```bash
# Check current auth status
bramble auth status

# Enable with a random token
bramble auth enable

# Enable with a specific token
bramble auth enable --set my-secret-token

# Disable auth (open access)
bramble auth disable
```

The `auth` commands work over serial (USB). You can also retrieve the
current token from a serial-connected device:

```bash
# Get the token from a USB-connected device
bramble pair
bramble pair -p /dev/ttyACM0        # specific port
bramble pair --json                  # machine-readable
bramble pair --save                  # save to ~/.config/bramble/tokens.json
```

### From the Web App

The web app's WiFi connection dialog has a token field. Enter the token
there when connecting to a device that has auth enabled.

### Via JSON-RPC (Advanced)

Over a serial connection, you can set the token directly:

```json
{"jsonrpc":"2.0","id":1,"method":"bramble.setAuthToken","params":{"token":"my-secret-token"}}
```

To clear the token (disable auth):
```json
{"jsonrpc":"2.0","id":1,"method":"bramble.setAuthToken","params":{"token":""}}
```

To read the current token:
```json
{"jsonrpc":"2.0","id":1,"method":"bramble.getAuthToken","params":{}}
```

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

The browser's WebSocket API doesn't support custom HTTP headers, so the
token is passed as a query parameter:

```
ws://192.168.1.100/ws?token=my-secret-token
```

### Other Clients (curl, Python, Go, etc.)

Use the `Authorization: Bearer` header:

```bash
# Example with websocat
websocat ws://192.168.1.100/ws -H "Authorization: Bearer my-secret-token"
```

```python
import websockets
headers = {"Authorization": f"Bearer {token}"}
async with websockets.connect(url, additional_headers=headers) as ws:
    ...
```

## Why Use Auth?

**You should set a token if:**
- Your device is on a shared network (office, apartment, café)
- You're running Bramble in a location where others could access your WiFi
- You want to prevent accidental or unauthorized configuration changes

**You probably don't need a token if:**
- The device is on your home network behind a firewall
- You're developing and frequently connecting from different tools
- You're at a hackathon or demo and want zero-friction access

## Security Notes

- Tokens are transmitted in cleartext over HTTP/WebSocket. On untrusted
  networks, consider using the web app's HTTPS proxy mode (`wss://`)
  which encrypts the connection.
- Serial (USB) connections always bypass auth. This is intentional —
  physical access to the device is considered root-level trust.
- The token is stored in plaintext in NVS on the device. A firmware
  dump could extract it. This is standard for embedded devices.
- BLE connections have their own separate auth mechanism (first-write
  token handshake).

## Defaults

- **New devices**: Open access (no token). Set one during web flasher setup
  or later via CLI/web app.
- **Firmware updates**: Token persists across OTA and USB firmware updates
  (stored in NVS, separate from firmware partition).
- **Factory reset**: Erasing NVS clears the token, returning to open access.
