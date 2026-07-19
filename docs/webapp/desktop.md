# Desktop App

The desktop app is the same webapp (chat, nodes, map, stats, config) packaged
as an Electron shell for Linux, Windows, and macOS. It exists for one reason:
the hosted web client cannot open a direct WebSocket to a node on your local
network. Browsers block insecure `ws://` connections from a secure (`https://`)
page and restrict requests from public pages into private IP ranges, so the
hosted app relies on a server-side proxy to reach your node. The desktop app's
renderer loads from `file://`, where neither restriction applies, so it
connects straight to `ws://<node-ip>/ws` with nothing in between. On top of
that it can discover nodes on your LAN automatically instead of you typing in
an IP.

Use the desktop app when your node is on WiFi (Station mode, joined to your
LAN) and you want that direct connection and auto-discovery. USB serial and
Bluetooth work identically in the hosted web app and the desktop app, so if
you only ever connect over USB or BLE there is no real difference between
them.

## Installing

Prebuilt installers are attached to every webapp release on the GitHub
releases page (the `webapp-vX.Y.Z` tags), with a `SHA256SUMS` manifest:

| Platform | Installer |
|----------|-----------|
| Linux    | AppImage, .deb, .pacman (x64) |
| macOS    | .dmg (arm64 and x64) |
| Windows  | .exe (NSIS, x64) |

The installers are not code-signed (there are no Apple or Windows signing
certificates yet), so macOS Gatekeeper and Windows SmartScreen will warn on
first launch. Building from source works too:

```bash
cd webapp
npm install
npm run package         # build for your current platform
npm run package:linux   # AppImage + deb + pacman
npm run package:mac     # dmg
npm run package:win     # NSIS installer (.exe)
```

Output lands in `webapp/release/`.

See [webapp/README.md](../../webapp/README.md) for the full dev workflow,
including `npm run dev:electron` for hot-reload while developing.

## Connecting to a node

The connect screen offers the same three transports as the web app: USB
serial, Bluetooth, and WiFi. WiFi is where the desktop app differs.

### WiFi

If you've connected to a node before and chose "Remember this device," it
shows up under "Your devices" with a one-click Connect button; no need to
re-enter the IP or token.

For nodes you haven't connected to yet, the desktop app shows a "Nearby
nodes" list above the manual entry fields, populated by scanning the LAN for
nodes advertising themselves over mDNS. Clicking an entry fills in its IP (and
name, if the node reports one) automatically.

If a nearby node isn't in your device book yet, or you'd rather type things
in by hand, fill in:

- **Node address**: the node's IP. In Station mode, check your router's
  client list or the node's own display. In AP mode, it's always
  `192.168.4.1`.
- **Auth token**: required the first time you connect to a given node. Get it
  by running the `pair` command with the `bramble` CLI over USB, or from the
  node's Config page in the web/desktop UI once you're already connected some
  other way.
- **Name** (optional): a friendly label shown in your device list.
- **Remember this device**: saves the name and token in the app so future
  connections are one-click. Leave this off on a shared machine.

AP mode (connecting directly to the node's own WiFi hotspot rather than your
LAN) is not discoverable, since the node isn't reachable over mDNS from a
network it's hosting itself. Connect to the node's hotspot in your OS WiFi
settings first, then use manual entry with `192.168.4.1`.

## The Nearby nodes list

Nearby nodes is desktop-only; it does not appear in the hosted or local
web app.

- Nodes running current firmware advertise their real name (if one is set)
  and their full address, so a node you've already saved is recognized and
  offered as a one-click connect even if its IP has changed since last time.
- Nodes running older firmware without that advertisement show up by their
  `bramble-XXXX` hostname (the last four hex digits of the node's address).
  If that suffix matches exactly one saved device, it's offered as a probable
  match, but you'll only see the real name after connecting once and the
  node's identity is verified.
- Renaming a node (from its Config page) updates the name shown in Nearby
  nodes on other machines live, without needing a reboot, as long as that
  firmware supports the newer advertisement.

## Troubleshooting

**Node doesn't show up in Nearby nodes.** Discovery relies on multicast DNS,
which some networks block or don't route between subnets: separate VLANs,
most VPNs, guest WiFi with client isolation, and some enterprise/mesh routers
all commonly interfere with it. If a node doesn't appear, or you're not
sure it's the same LAN, fall back to manual IP entry.

**Connection fails with an authentication error.** The token is wrong,
expired, or the node was re-paired since it was saved. Re-run `pair` over USB
or grab a fresh token from the node's Config page and re-enter it.

**"That address now belongs to a different node."** The app checks the
node's actual identity against what it expected before accepting a one-click
or remembered connection, and refuses if they don't match. This shows up when
a node's IP has been reassigned by DHCP to a different device since you last
connected. Update the saved IP (or reconnect from Nearby nodes, which uses
the node's current IP) rather than assuming the old one is still right.
