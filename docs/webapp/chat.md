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

- **Presence**: online status only, no coordinates
- **Zone (coarse)**: a quantized cell of 0.003° latitude by 0.006° longitude, about 334 m north-south and 668 m east-west at the equator, narrowing east-west toward the poles
- **Exact (full)**: full GPS coordinates

The map draws an exact share as a point marker and a zone share as a
rectangle covering the whole cell, because a zone share names an area and not
a point inside it. A presence share carries no coordinates, so the map cannot
place it and the Nodes list is where that peer appears.
