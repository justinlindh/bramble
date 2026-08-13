# Web App User Guide

## Keyboard shortcuts

- `Ctrl+1` Chat
- `Ctrl+2` Nodes
- `Ctrl+3` Map
- `Ctrl+4` Config
- `Ctrl+5` Stats
- `/` focuses quick navigation/search
- `Esc` clears search and closes quick-jump focus state
- In compose box: `Enter` sends, `Shift+Enter` adds newline

## New-message jump button

When you are scrolled up in a conversation and new messages arrive, a
`↓ New messages` button appears. Click it to jump to latest and clear
the indicator.

## Nodes route legend and route state semantics

Route table columns:

- **Destination**: node this route reaches
- **Next Hop**: neighbor used for forwarding
- **Hops**: relay count (lower is generally better)
- **Metric**: route quality score (0-255, higher is better), accumulated from per-hop RSSI/SNR link penalties
- **State**:
  - `active`: currently usable
  - `stale`: not recently confirmed
  - `discovering`: route discovery in progress

## Connection labels (staged states)

UI connection states:

- `disconnected`: no active transport
- `connecting`: transport handshake in progress
- `connected`: RPC/session active
- `error` (shown as `Reconnecting…` in header): transport dropped and auto-reconnect is running

Transport-specific in overlay while connecting:

- BLE: `Scanning…`
- Serial: `Opening serial…`
- Wi‑Fi: `Handshaking…`
- Anything else: `Connecting…`

## Location tiers

Location sharing tiers used in Config/Map/Chat:

- **Presence**: online status only
- **Zone (coarse)**: grid-square level (~1 km)
- **Exact (full)**: full GPS coordinates

Map legend uses distinct markers for exact, zone, and self position.
