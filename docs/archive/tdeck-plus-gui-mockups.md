# T-Deck Plus Graphical UI — MVP Mockups

**Display:** 320×240 RGB565 IPS LCD (landscape), capacitive touchscreen (GT911)
**Input:** Touchscreen + physical keyboard + 5-way trackball (up/down/left/right/press)
**I/O Expander:** PCA9535PW (I2C addr 0x20)
**Library:** LVGL v9
**Target:** ESP32-S3 with PSRAM

---

## Color Palette

Bramble brand colors — dark theme to save power and look good on IPS:

| Role            | Color      | Hex       | Use                              |
|-----------------|------------|-----------|----------------------------------|
| Background      | Near-black | `#1A1A2E` | All screen backgrounds           |
| Surface         | Dark blue  | `#16213E` | Cards, list items, input fields  |
| Primary         | Teal       | `#0F9B8E` | Headers, active tab, highlights  |
| Accent          | Amber      | `#F0A500` | Warnings, unread badges, battery |
| Text Primary    | White      | `#EAEAEA` | Main text                        |
| Text Secondary  | Gray       | `#8892A0` | Timestamps, hints, labels        |
| Sent Bubble     | Dark teal  | `#0D7377` | Outgoing messages                |
| Recv Bubble     | Slate      | `#2C3E6B` | Incoming messages                |
| Danger          | Red        | `#E74C3C` | Errors, low battery              |
| Success         | Green      | `#2ECC71` | Connected, online indicators     |

## Font Plan

LVGL bundles Montserrat. We'll use:
- **14px** — body text, message content, list items
- **12px** — timestamps, secondary labels, status bar
- **18px bold** — screen titles, node names in detail view
- **20px bold** — splash screen title only

At 320×240 these sizes are readable without wasting space.

## Required Assets

### Icons (16×16 and 20×20, embedded C arrays via LVGL image converter)

| Icon             | Use                                    |
|------------------|----------------------------------------|
| `chat`           | Tab bar, chat screen                   |
| `nodes`          | Tab bar, nodes screen                  |
| `stats`          | Tab bar, stats screen                  |
| `settings`       | Tab bar, settings screen               |
| `send`           | Compose bar send button                |
| `battery_full`   | Status bar (4 states: full/med/low/crit) |
| `battery_med`    |                                        |
| `battery_low`    |                                        |
| `battery_crit`   |                                        |
| `signal_0..3`    | Signal strength (4 bars)               |
| `gps`            | GPS fix indicator                      |
| `wifi`           | WiFi connected                         |
| `ble`            | BLE connected                          |
| `mesh`           | Mesh/radio active                      |
| `back`           | Navigation back arrow                  |
| `checkmark`      | Delivery confirmed                     |
| `clock`          | Pending delivery                       |
| `antenna`        | Hop count / relay indicator            |

### Splash Image
- Bramble logo, ~80×80, centered on boot screen
- Or render from icon font — TBD

### New Driver: GT911 Touch Controller
- I2C capacitive touch (same bus as keyboard, addr TBD — GT911 uses 0x5D or 0x14)
- Interrupt pin: GPIO16 (already defined as TOUCH_INT in board config — turns out it IS used!)
- Multi-touch capable but we only need single-touch for LVGL
- Need to init after display (GT911 uses reset timing to set I2C address)

### No other bitmaps needed
Everything else is rendered with LVGL widgets (bars, arcs, labels, containers).

---

## Screen Mockups

### Global: Status Bar (always visible, top 20px)

```
┌──────────────────────────────────────────────────────────────┐
│ 🔋88%  📡3  📍GPS   ⚡MESH          12:34 PM  BRAMBLE │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

- Left: battery icon + %, signal bars, GPS fix, mesh indicator
- Right: time (from GPS or RTC), "BRAMBLE" branding or node name
- 20px tall, dark background with subtle bottom border
- Always present on all screens

### Global: Tab Bar (bottom 32px)

```
┌──────────────────────────────────────────────────────────────┐
│   💬 Chat    📡 Nodes    📊 Stats    ⚙ Settings            │
└──────────────────────────────────────────────────────────────┘
```

- 4 tabs, icon + label, active tab highlighted in Primary color
- Trackball left/right to switch tabs (when not in a focusable widget)
- Unread badge (small amber dot) on Chat tab when new messages

### Usable content area: 320 × 180px (240 - 20 status - 40 tabs)

---

### Screen 1: Home / Status Dashboard

Not a separate tab — the Status Bar IS the persistent status. Chat is the default tab.

If we want a "home" feel, Chat screen shows a brief status summary when no conversation is selected:

```
┌─────────────────── 320px ───────────────────┐
│ 🔋92%  📡4  📍GPS  ⚡MESH     2:15 PM  DumNode │  ← status bar
├─────────────────────────────────────────────────┤
│                                                 │
│           ┌─────────────────────┐               │
│           │   B R A M B L E     │               │
│           │                     │               │
│           │   Node: DumNode     │               │
│           │   Addr: 6E1EE666    │               │
│           │   Peers: 4          │               │
│           │   Uptime: 2h 14m    │               │
│           │   Queue: 0          │               │
│           │   Ch: Default       │               │
│           └─────────────────────┘               │
│                                                 │
│  "Press → to start chatting"                    │
│                                                 │
├─────────────────────────────────────────────────┤
│  [💬 Chat]   📡 Nodes   📊 Stats   ⚙ Settings │  ← tab bar
└─────────────────────────────────────────────────┘
```

---

### Screen 2: Chat — Conversation List

When Chat tab is active and no conversation is open:

```
┌─────────────────── 320px ───────────────────┐
│ 🔋88%  📡3  📍GPS  ⚡MESH     2:15 PM       │
├─────────────────────────────────────────────────┤
│  Conversations                                  │
├─────────────────────────────────────────────────┤
│ ┌─────────────────────────────────────────────┐ │
│ │ ► Default Channel              2 unread  │ │  ← highlighted
│ │   "anyone on the mesh?"         1m ago   │ │
│ └─────────────────────────────────────────────┘ │
│ ┌─────────────────────────────────────────────┐ │
│ │   Trailcrew                              │ │
│ │   No messages                            │ │
│ └─────────────────────────────────────────────┘ │
│ ┌─────────────────────────────────────────────┐ │
│ │   DM: AlphaNode                          │ │
│ │   "thanks for relaying"        5m ago    │ │
│ └─────────────────────────────────────────────┘ │
│                                                 │
│                                                 │
├─────────────────────────────────────────────────┤
│  [💬 Chat]   📡 Nodes   📊 Stats   ⚙ Settings │
└─────────────────────────────────────────────────┘

Navigation: ▲▼ scroll list, ● (press) open conversation, ◄► switch tabs
```

---

### Screen 3: Chat — Message View

Inside a conversation:

```
┌─────────────────── 320px ───────────────────┐
│ 🔋88%  📡3  📍GPS  ⚡MESH     2:15 PM       │
├─────────────────────────────────────────────────┤
│  ◄ Default Channel                    4 users  │  ← header w/ back
├─────────────────────────────────────────────────┤
│                                                 │
│        ┌────────────────────┐                   │
│        │ AlphaNode   1:02 PM│                   │  ← received (left)
│        │ anyone on the mesh?│                   │
│        └────────────────────┘                   │
│                                                 │
│                   ┌─────────────────┐           │
│                   │ You      1:03 PM│           │  ← sent (right)
│                   │ yeah I'm here ✓ │           │
│                   └─────────────────┘           │
│                                                 │
│        ┌────────────────────┐                   │
│        │ BravoNode  1:05 PM │                   │
│        │ same, 2 hops out   │                   │
│        │        🔀 via Relay │                   │  ← relay indicator
│        └────────────────────┘                   │
│                                                 │
├─────────────────────────────────────────────────┤
│  > Type message...              [Send]          │  ← compose bar
└─────────────────────────────────────────────────┘

Navigation: ▲▼ scroll messages, ◄ back to list, keyboard activates compose
Tab bar hidden when in message view (compose bar replaces it)
```

---

### Screen 4: Nodes

```
┌─────────────────── 320px ───────────────────┐
│ 🔋88%  📡3  📍GPS  ⚡MESH     2:15 PM       │
├─────────────────────────────────────────────────┤
│  Nodes (4 peers)                                │
├─────────────────────────────────────────────────┤
│ ┌─────────────────────────────────────────────┐ │
│ │ ► AlphaNode                          │ │  ← highlighted
│ │   6E1EE666  ████░░ -72dBm  1 hop  ●online │ │
│ └─────────────────────────────────────────────┘ │
│ ┌─────────────────────────────────────────────┐ │
│ │   BravoNode                                │ │
│ │   63929F02  ██░░░░ -91dBm  2 hops ●online │ │
│ └─────────────────────────────────────────────┘ │
│ ┌─────────────────────────────────────────────┐ │
│ │   CharlieNode                              │ │
│ │   04CAAF8   █░░░░░ -104dBm 3 hops ○stale  │ │
│ └─────────────────────────────────────────────┘ │
│ ┌─────────────────────────────────────────────┐ │
│ │   DeltaNode                                │ │
│ │   A1B2C3D4  ███░░░ -85dBm  1 hop  ●online │ │
│ └─────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────┤
│   💬 Chat   [📡 Nodes]   📊 Stats   ⚙ Settings│
└─────────────────────────────────────────────────┘

Navigation: ▲▼ scroll, ● press for node detail (future), ◄► switch tabs
```

---

### Screen 5: Stats

```
┌─────────────────── 320px ───────────────────┐
│ 🔋88%  📡3  📍GPS  ⚡MESH     2:15 PM       │
├─────────────────────────────────────────────────┤
│  Stats                                          │
├─────────────────────────────────────────────────┤
│                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐      │
│  │ TX: 142  │  │ RX: 891  │  │ FWD: 56  │      │  ← counter cards
│  │ packets  │  │ packets  │  │ relayed  │      │
│  └──────────┘  └──────────┘  └──────────┘      │
│                                                 │
│  Airtime (last hour)                            │
│  ┌─────────────────────────────────────┐        │
│  │ ██████████████░░░░░░░░░░░░░░  42%  │        │  ← bar chart
│  └─────────────────────────────────────┘        │
│                                                 │
│  Free heap:  186 KB                             │
│  PSRAM free: 3.2 MB                             │
│  Flash used: 1.8 / 16 MB                       │
│  Uptime:     4h 22m                             │
│  GPS sats:   7                                  │
│                                                 │
├─────────────────────────────────────────────────┤
│   💬 Chat    📡 Nodes   [📊 Stats]  ⚙ Settings│
└─────────────────────────────────────────────────┘
```

---

### Screen 6: Settings

```
┌─────────────────── 320px ───────────────────┐
│ 🔋88%  📡3  📍GPS  ⚡MESH     2:15 PM       │
├─────────────────────────────────────────────────┤
│  Settings                                       │
├─────────────────────────────────────────────────┤
│                                                 │
│  ► Node Name         DumNode              [▸]  │  ← highlighted
│                                                 │
│    Backlight         ████████░░  80%            │  ← slider
│                                                 │
│    Volume            ██████░░░░  60%            │
│                                                 │
│    Connectivity      WiFi + BLE           [▸]  │  ← dropdown
│                                                 │
│    GPS               Enabled              [▸]  │
│                                                 │
│    Audio Alerts      On                   [▸]  │
│                                                 │
│  ──────────────────────────────────              │
│    About / Version   v0.9.1-tdeck               │
│    Reboot Device                          [▸]  │
│                                                 │
├─────────────────────────────────────────────────┤
│   💬 Chat    📡 Nodes    📊 Stats  [⚙ Settings]│
└─────────────────────────────────────────────────┘

Navigation: ▲▼ move selection, ● enter/toggle, ◄► adjust sliders, ◄ back from sub-menu
```

---

## Input Model

Three input methods — all first-class, all work everywhere. Touch is primary for direct interaction, trackball for one-handed/precise nav, keyboard for text entry.

### Touch Gestures

| Gesture              | Action                                      |
|----------------------|---------------------------------------------|
| Tap                  | Select item, press button, switch tab        |
| Tap + hold           | Context action (future: message options)      |
| Swipe up/down        | Scroll lists, scroll messages                |
| Swipe left           | Back (from message view → conversation list) |
| Swipe left/right     | Switch tabs (on tab bar area)                |
| Tap compose bar      | Activate keyboard input                      |
| Tap send button      | Send message                                 |
| Drag on slider       | Adjust backlight/volume                      |

### Trackball

| Context              | Trackball ▲▼       | Trackball ◄►       | Trackball ●        |
|----------------------|--------------------|--------------------|--------------------|
| Tab bar focused      | —                  | Switch tabs        | Enter tab          |
| List (convos/nodes)  | Scroll selection   | Switch tabs        | Open item          |
| Message view         | Scroll messages    | ◄ = back to list   | —                  |
| Compose active       | —                  | Move cursor        | Send message       |
| Settings list        | Move selection     | Adjust slider      | Toggle/enter       |

### Keyboard

| Context              | Action                                       |
|----------------------|----------------------------------------------|
| Any screen           | Start typing → auto-activates compose        |
| Compose active       | Type text, Enter = send, Esc = cancel        |
| Settings text field  | Type value, Enter = confirm                  |

### Design Implications

- **Tap targets ≥ 40×40px** — fingers on a 2.8" screen need generous hit areas
- **Visual focus indicator** — trackball users need to see what's selected (highlight ring)
- **Scrollbars** — thin persistent scrollbar for touch users, highlight-based scroll for trackball
- **Tab bar 40px** (was 32px) — needs to be finger-friendly
- **Compose bar 44px** — tall enough to tap reliably
- All interactive elements work with both touch tap AND trackball select

## Architecture Notes

- `components/ui_graphics/` — new component, T-Deck Plus only
- `components/ui/` — stays as-is for Heltec
- Kconfig `BRAMBLE_UI_GRAPHICAL` — selects between them
- LVGL runs its own timer task (5ms tick), display flush callback writes to existing ST7789 framebuffer
- **Touch (GT911):** I2C capacitive touch controller, registers as LVGL touchpad input device. INT on GPIO16.
- **Trackball:** maps to LVGL encoder input device (secondary nav)
- **Keyboard:** maps to LVGL keyboard input device (text entry)
- **PCA9535PW I/O expander** (I2C 0x20): may control peripheral enables — need to check schematic
- LVGL allocates from PSRAM (plenty available — current FB is 150KB, PSRAM is 8MB)
- All widgets designed touch-first (≥40px targets) with trackball focus as fallback

## MVP Scope

1. Status bar with real data
2. Tab navigation (4 tabs)
3. Chat: conversation list → message view → compose with keyboard
4. Nodes: scrollable peer list with signal/hop/status
5. Stats: counter cards + airtime bar + system info
6. Settings: backlight, volume, connectivity toggle, node name, reboot
7. Splash screen on boot

## Not in MVP (future)

- Map / relative position plot
- Node detail view (tap a node for full info)
- Channel management (add/remove/share)
- Peer management (trust/block)
- Radio config (frequency, power, spreading factor)
- QR code display for channel sharing
- Message search
- File transfer progress
- OTA update screen
