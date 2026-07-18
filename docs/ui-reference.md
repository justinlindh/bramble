# Bramble T-Deck Plus UI Reference

> Generated from source analysis of `components/ui_graphics/`; last full reconcile 2026-07-08; color table re-verified against `bramble_theme.h` 2026-07-18.
> Use this as the canonical reference for UI iteration. Update when screens change.

**Display:** 320×240 px, RGB565 IPS LCD (landscape)  
**Input:** Touchscreen (GT911), physical QWERTY keyboard (I2C polling), 5-way trackball  
**Library:** LVGL v9  
**Target:** ESP32-S3, T-Deck Plus  

---

## Table of Contents

1. [Theme & Colors](#theme--colors)
2. [Layout Constants](#layout-constants)
3. [Splash Screen](#1-splash-screen)
4. [Main Layout Shell](#2-main-layout-shell)
5. [Chat Tab — Message List](#3-chat-tab--message-list)
6. [Chat — Message Detail View](#4-chat--message-detail-view)
7. [Nodes Tab](#5-nodes-tab)
8. [Map Tab](#6-map-tab)
9. [Stats Tab](#7-stats-tab)
10. [Settings Tab](#8-settings-tab)
11. [User Flows](#user-flows)
12. [Input Model](#input-model)
13. [Known Limitations & TODOs](#known-limitations--todos)

---

## Theme & Colors

Dark theme. All colors defined in `components/ui_graphics/theme/bramble_theme.h`.

| Role          | Name             | Hex       | Usage                                         |
|---------------|------------------|-----------|-----------------------------------------------|
| Background    | `BR_COLOR_BG`    | `#0D1117` | Screen backgrounds                            |
| Surface       | `BR_COLOR_SURFACE` | `#161B22` | Cards, list rows, compose bar, header rows, status/tab bars |
| Surface 2     | `BR_COLOR_SURFACE_2` | `#21262D` | Secondary surfaces, incoming bubbles      |
| Border        | `BR_COLOR_BORDER` | `#30363D` | Card and control borders                    |
| Primary       | `BR_COLOR_PRIMARY` | `#238636` | Tab active highlight, titles, peer address labels, primary buttons (green) |
| Accent        | `BR_COLOR_ACCENT` | `#1F6FEB` | Battery warning (30% threshold) (blue)      |
| Text          | `BR_COLOR_TEXT`   | `#E6EDF3` | Primary body text                            |
| Text Secondary| `BR_COLOR_TEXT_SEC` | `#8B949E` | Subtitles, hints, secondary info, GPS label |
| Sent Bubble   | `BR_COLOR_SENT`   | `#1A4B91` | Outgoing message bubbles; outgoing row indicators (blue) |
| On-Sent Marks | `BR_COLOR_ON_SENT`| `#E6EDF3` | Muted marks drawn on a sent bubble |
| Recv Bubble   | `BR_COLOR_RECV`   | `#21262D` | Incoming message bubbles                    |
| Danger        | `BR_COLOR_DANGER` | `#DA3633` | Error states, low battery (<15%), Reboot button |
| Success       | `BR_COLOR_SUCCESS`| `#238636` | Online node dot, signal bar fill            |
| Warning       | `BR_COLOR_WARNING`| `#E3B341` | Warning states                               |
| Critical      | `BR_COLOR_CRITICAL`| `#BC8CFF` | Critical-tier indicators                    |

This is the GitHub-dark palette shared with the webapp; see `docs/color-scheme.md` for the cross-surface mapping and migration history.

**Fonts:** Montserrat (bundled with LVGL). Sizes in use: 12, 14, 16, 18.

---

## Layout Constants

Defined in `bramble_theme.h`:

| Constant           | Value  | Description                                      |
|--------------------|--------|--------------------------------------------------|
| `BR_STATUS_BAR_H`  | 20 px  | Top status bar height                            |
| `BR_TAB_BAR_H`     | 40 px  | Bottom tab bar height                            |
| `BR_CONTENT_H`     | 180 px | Middle content area height (240 - 20 - 40)       |
| `BR_COMPOSE_BAR_H` | 44 px  | Chat compose bar height (used in message view)   |
| `BR_TAP_TARGET_MIN`| 40 px  | Minimum height for tappable setting rows         |
| `BR_PADDING`       | 8 px   | Standard content padding                         |
| `BR_RADIUS`        | 6 px   | Standard card border radius                      |

---

## 1. Splash Screen

**Source:** `screens/scr_splash.c`  
**Duration:** 2 seconds (then transitions to main layout)

```
┌──────────────────────────────────────┐  (320×240)
│                                      │
│                                      │
│          ┌──────────────┐            │
│          │  [100×100]   │            │
│          │  bramble     │            │
│          │  logo img    │            │
│          └──────────────┘            │
│                                      │
│              BRAMBLE                 │  ← Montserrat 18, PRIMARY color
│             LoRa Mesh                │  ← Montserrat 12, TEXT_SEC
│              v0.9.1                  │  ← Montserrat 12, TEXT_SEC, 50% opacity
│                                      │
│                                      │
└──────────────────────────────────────┘
  Background: #0D1117 (BR_COLOR_BG)
  All items centered horizontally and vertically as a column flex group, 8px row gap
```

**Elements:**
- Bramble logo image (`img_bramble_logo`, 100×100, from flash)
- Title: "BRAMBLE" — Montserrat 18, primary green
- Subtitle: "LoRa Mesh" — Montserrat 12, secondary gray
- Version: "v{app_version}" from `esp_app_desc_t` — Montserrat 12, secondary gray, 50% opacity

**Transition:** After 2s, LVGL timer fires `splash_timer_cb`:
1. `lv_refr_now()` — flush pending layouts (critical: avoids indev timer corruption)
2. `lv_obj_clean()` — destroy splash
3. Initialize touch, trackball, keyboard ports
4. `layout_create()` — build main shell

---

## 2. Main Layout Shell

**Source:** `screens/scr_layout.c`  
**Applies to:** All tabs (Chat, Nodes, Map, Stats, Settings)

```
┌──────────────────────────────────────┐  y=0
│ 🔋 85%  📶 3   GPS  --:--  BRAMBLE  │  ← Status Bar (h=20)
├──────────────────────────────────────┤  y=20
│                                      │
│          [Content Area]              │  ← 320×180 px
│                                      │
│                                      │
│                                      │
│                                      │
│                                      │
├──────────────────────────────────────┤  y=200
│ ✉Chat 📶Nodes 🧭Map ≡Stats ⚙Set   │  ← Tab Bar (h=40)
└──────────────────────────────────────┘  y=240
```

### Status Bar (y=0, h=20, bg=`#161B22 (BR_COLOR_SURFACE)`)

Horizontal flex row, space-between alignment, 2px padding all sides, Montserrat 12:

| Position | Label          | Content                                | Color           |
|----------|----------------|----------------------------------------|-----------------|
| Left     | Battery        | `{sym} {pct}%` (sym varies by level)  | TEXT (colored by level; thresholds below) |
| —        | Signal         | `📶 {neighbor_count}`                 | TEXT            |
| —        | GPS            | `GPS` (static)                         | TEXT_SEC        |
| —        | Time           | `--:--` (static placeholder)          | TEXT            |
| Right    | Node Name      | `BRAMBLE` (static)                     | PRIMARY (green)  |

Battery icon thresholds:
- `>75%` → BATTERY_FULL, TEXT color
- `>50%` → BATTERY_3, TEXT color
- `>25%` → BATTERY_2, TEXT color
- `≤25%` → BATTERY_1, TEXT color
- `≤30%` → BATTERY_2, ACCENT (blue) color
- `≤15%` → BATTERY_1, DANGER (red) color

### Content Area (y=20, h=180, bg=`BR_COLOR_BG`)

Replaced in-place (via `lv_obj_clean()`) each time a tab is selected. No scroll on the container itself — scroll is handled per-tab.

### Tab Bar (y=200, h=40, bg=`#161B22 (BR_COLOR_SURFACE)`)

Five equal buttons at 60×36 px each, space-evenly aligned, Montserrat 12:

| Index | Label            | Symbol       | Tab enum       |
|-------|------------------|--------------|----------------|
| 0     | `✉ Chat`         | ENVELOPE     | `TAB_CHAT`     |
| 1     | `📶 Nodes`       | WIFI         | `TAB_NODES`    |
| 2     | `🧭 Map`         | GPS          | `TAB_MAP`      |
| 3     | `≡ Stats`        | BARS         | `TAB_STATS`    |
| 4     | `⚙ Set`          | SETTINGS     | `TAB_SETTINGS` |

Active tab: background fills with PRIMARY at 30% opacity.  
Inactive tabs: transparent background.

**Unread Badge** (Chat tab only): When messages arrive while not on Chat tab, a red circle badge (20×20, radius 10) appears top-right of the Chat button, showing count (capped at 99). Cleared when Chat tab is opened.

### Refresh Timers

| Timer | Period | Behavior |
|-------|--------|----------|
| Status refresh | 2s | Updates battery %, neighbor count in status bar; processes pending events (new messages) |
| Tab refresh | 5s | Rebuilds content area if active tab is Stats or Nodes (live data refresh) |

---

## 3. Chat Tab — Message List

**Source:** `screens/scr_chat_list.c`  
**LVGL trigger:** `scr_chat_list_create()` called from `layout_set_tab(TAB_CHAT)`

```
[Content Area: 320×180]
┌──────────────────────────────────────┐  y=20 (within screen)
│ Messages                  [+ New]   │  ← Header row (h=28)
├──────────────────────────────────────┤
│ ┌────────────────────────────────┐  │
│ │ → You                          │  │  ← Outgoing (SENT color)
│ │ Hello from Bramble!            │  │  ← Preview text, truncated with "..."
│ └────────────────────────────────┘  │
│ ┌────────────────────────────────┐  │
│ │ ← 0A1B2C3D                    │  │  ← Incoming (PRIMARY color)
│ │ Got your beacon, -89dBm        │  │
│ └────────────────────────────────┘  │
│  ...                               │  ← (up to 10 items, newest-first)
└──────────────────────────────────────┘
  Empty state: "No messages yet.\nTap 'New' to compose." (centered, TEXT_SEC)
```

### Header Row (h=28, transparent bg)

| Element      | Position    | Details                              |
|--------------|-------------|--------------------------------------|
| "Messages"   | Left, +4px  | Montserrat 16, TEXT color            |
| [+ New] btn  | Right, -4px | 80×24, PRIMARY bg, BR_RADIUS, Montserrat 12 |

Tapping [+ New] → opens Chat Message Detail View (channel 0).

### Message List

- Scrollable vertical flex column, 4px padding, 4px row gap
- Height: `BR_CONTENT_H - 28 = 152 px`
- Shows last 10 messages from `msg_store`, newest-first
- Each message card: 304×48, SURFACE bg, BR_RADIUS

**Message Card Layout (48px tall):**
```
┌─────────────────────────────────────┐
│ → You   [or]  ← 0A1B2C3D           │  y=0, Montserrat 12 (SENT or PRIMARY color)
│ Preview text truncated to width...  │  y=16, Montserrat 14, TEXT color, max-width 290px, "..."
└─────────────────────────────────────┘
```

- Outgoing (`MSG_DIR_OUTGOING` / `MSG_DIR_BROADCAST_OUT`): indicator "→ You", SENT green color
- Incoming: indicator "← {8-char hex peer addr}", PRIMARY green color
- Tapping a card → opens Chat Message Detail View (channel 0)
- Trackball focus: PRIMARY bg at 30% opacity on focused card

---

## 4. Chat — Message Detail View

**Source:** `screens/scr_chat_messages.c`  
**LVGL trigger:** `scr_chat_messages_open(layout, channel_idx)`

This is a full-screen overlay over the content area. The **tab bar is hidden** and content area expands to fill its space.

```
[Expanded Content: 320×220 (240 - 20px status bar)]
┌──────────────────────────────────────┐  y=20
│ [←]          Chat                   │  ← Header (h=28, SURFACE bg)
├──────────────────────────────────────┤  y=48
│                                      │
│  ┌──────────────────────────┐        │  ← Incoming bubble (RECV color, left-aligned)
│  │ 0A1B2C3D                 │        │     Sender name in PRIMARY (Montserrat 12)
│  │ Hey, what's your signal? │        │     Message text (Montserrat 14)
│  └──────────────────────────┘        │
│                                      │
│       ┌──────────────────────────┐   │  ← Outgoing bubble (SENT color, right-aligned)
│       │ -87dBm on my end, solid  │   │     No sender name
│       └──────────────────────────┘   │
│                                      │
│   [message list area, scrollable]    │  h = 220 - 28 - 44 = 148px
│                                      │
├──────────────────────────────────────┤  y=196
│ [Type message...         ] [✓ Send] │  ← Compose Bar (h=44, SURFACE bg)
└──────────────────────────────────────┘  y=240
```

### Header Row (h=28, SURFACE bg)

| Element   | Position         | Details                                        |
|-----------|------------------|------------------------------------------------|
| [←] Back  | Left             | 40×24 btn, transparent bg, SYMBOL_LEFT         |
| "Chat"    | Center           | Montserrat 14, TEXT color                       |

Tapping [←] → restores tab bar, restores content area size (320×180), rebuilds chat list.

### Message Area (h=148, scrollable vertical flex, 4px gap)

Each bubble is a `row` container (304px wide, transparent) containing a `bubble` object:

**Incoming bubble (left-aligned):**
```
┌─────────────────────────────┐   max-width: 220px
│ {sender_hex_addr}            │   Montserrat 12, PRIMARY color
│ {message text wrapping}      │   Montserrat 14, TEXT color
└─────────────────────────────┘   RECV bg (#21262D), radius=8
```

**Outgoing bubble (right-aligned):**
```
              ┌────────────────────────┐   max-width: 220px
              │ {message text wrapping} │   Montserrat 14, TEXT color
              └────────────────────────┘   SENT bg (#1A4B91), radius=8
```

After loading all messages, list auto-scrolls to bottom (`lv_obj_scroll_to_y(..., LV_COORD_MAX)`).

### Compose Bar (h=44, SURFACE bg, positioned at y=196)

| Element        | Position | Size    | Details                               |
|----------------|----------|---------|---------------------------------------|
| Text area      | Left     | 260×36  | Placeholder "Type message...", one-line, BG bg, PRIMARY border when focused |
| [✓ Send] btn   | Right    | 44×36   | PRIMARY bg, SYMBOL_OK label; sends broadcast, clears textarea |

Keyboard focus is placed on the textarea immediately on open — physical keyboard types directly into the compose field.

---

## 5. Nodes Tab

**Source:** `screens/scr_nodes.c`  
**LVGL trigger:** `scr_nodes_create()` from `layout_set_tab(TAB_NODES)`  
**Auto-refresh:** Every 5 seconds (rebuilds via `tab_refresh_timer_cb`)

```
[Content Area: 320×180]
┌──────────────────────────────────────┐
│ Nodes (3 peers)                      │  ← Title, Montserrat 16, +8px pad left
├──────────────────────────────────────┤
│ ┌────────────────────────────────┐   │
│ │ NodeAlpha         ████░░  🟢   │   │  ← Card: name, signal bar, online dot
│ │ -82dBm  SNR:7                  │   │
│ └────────────────────────────────┘   │
│ ┌────────────────────────────────┐   │
│ │ 0A1B2C3D          ██░░░░  🟢   │   │  ← Card: hex addr (no name set)
│ │ -95dBm  SNR:2                  │   │
│ └────────────────────────────────┘   │
│ ...                                  │
└──────────────────────────────────────┘
  Empty state: "No peers discovered yet.\nWaiting for beacons..." (centered, TEXT_SEC)
```

### Title

`"Nodes ({N} peer{s})"` — Montserrat 16, TEXT color, 8px left pad, 4px top pad.

### Node List

- Scrollable vertical flex column, starts at y=22, height `BR_CONTENT_H - 24 = 156px`
- 4px padding, 4px row gap
- Each card: 304×48, SURFACE bg, BR_RADIUS
- Trackball focus: PRIMARY bg at 30% opacity

### Node Card Layout (48px tall):

```
┌─────────────────────────────────────────┐
│ {name or 8-char hex addr}    [bar] 🟢   │  y=0, name: Montserrat 14 TEXT
│ {-XXdBm  SNR:X}                         │  y=20, Montserrat 12 TEXT_SEC
└─────────────────────────────────────────┘
```

| Element     | Details                                                               |
|-------------|-----------------------------------------------------------------------|
| Name/addr   | `n->name` if set, else `%08lX` hex addr; Montserrat 14, TEXT         |
| Info row    | `"{rssi}dBm  SNR:{snr}"` — Montserrat 12, TEXT_SEC                   |
| Signal bar  | `lv_bar`, 40×8 px, top-right; value = `(rssi + 120) * 100 / 70`, clamped 0–100; fill = SUCCESS green |
| Status dot  | 8×8 circle, top-right +0,-6; SUCCESS green if age < 600s (10 min), TEXT_SEC gray if stale |

Cards are clickable (no action currently implemented).

---

## 6. Map Tab

**Source:** `screens/scr_map.c`

Displays node positions from the location manager (`mesh_get_location_state()`) on a simple equirectangular projection, fixed 5 km radius centered on self.

- Canvas 280x140 px inside a 312x148 px container, LVGL v9 static draw buffer, RGB565.
- Self position: blue circle marker labeled "You"; peers: green circle markers labeled by node name/address.
- Crosshair grid centered on self; status line shows current coordinates and peer count.
- Shows "No GPS data" when no position is available.

---

## 7. Stats Tab

**Source:** `screens/scr_stats.c`  
**LVGL trigger:** `scr_stats_create()` from `layout_set_tab(TAB_STATS)`  
**Auto-refresh:** Every 5 seconds (rebuilds via `tab_refresh_timer_cb`)

```
[Content Area: 320×180, vertical flex column, 8px padding, 6px row gap]
┌──────────────────────────────────────┐
│ Stats                                │  ← Title, Montserrat 16
│ ┌──────┐  ┌──────┐  ┌──────┐        │
│ │  42  │  │ 137  │  │  3   │        │  ← Counter row (3 cards, 96×52 each)
│ │TX pkt│  │RX pkt│  │peers │        │
│ └──────┘  └──────┘  └──────┘        │
│ Radio: OK  Last RSSI: -89dBm        │  ← Montserrat 12; TEXT_SEC or DANGER
│ ─────────────────────────────────── │  ← Separator (1px, TEXT_SEC 30%)
│ System                               │  ← Montserrat 12, TEXT_SEC
│ Free heap:  142 KB                   │  ← Montserrat 14, TEXT
│ PSRAM free: 4.1 MB                   │
│ Uptime:     0h 47m                   │
└──────────────────────────────────────┘
```

### Counter Cards (row, space-evenly)

Each card: 96×52, SURFACE bg, vertical flex, centered:

| Value label | Montserrat 18, PRIMARY green    |
|-------------|-------------------------------|
| Unit label  | Montserrat 12, TEXT_SEC gray  |

Three cards: **TX pkts**, **RX pkts**, **peers** (from mesh state snapshot).

### Radio Status Row

`"Radio: {OK|ERROR}  Last RSSI: {rssi}dBm"` — Montserrat 12:
- `radio_ok=true` → TEXT_SEC gray
- `radio_ok=false` → DANGER red

### System Section

- "System" header — Montserrat 12, TEXT_SEC
- Multi-line label (Montserrat 14, TEXT):
  - `Free heap: {N} KB`
  - `PSRAM free: {N.N} MB`
  - `Uptime: {h}h {m}m`

---

## 8. Settings Tab

**Source:** `screens/scr_settings.c`  
**LVGL trigger:** `scr_settings_create()` from `layout_set_tab(TAB_SETTINGS)`  
**No auto-refresh** (static content + user-driven controls)

```
[Content Area: 320×180, vertical flex column, 8px padding, 4px row gap]
┌──────────────────────────────────────┐
│ Settings                             │  ← Title, Montserrat 16
│ ┌──────────────────────────────────┐ │
│ │ Node Name              [BRAMBLE] │ │  ← Read-only row (40px)
│ └──────────────────────────────────┘ │
│ ┌──────────────────────────────────┐ │
│ │ Backlight           [──●──────]  │ │  ← Slider row (48px), default 80
│ └──────────────────────────────────┘ │
│ ┌──────────────────────────────────┐ │
│ │ 🔊 Volume           [──●──────]  │ │  ← Slider row (48px), current value
│ └──────────────────────────────────┘ │
│ ┌──────────────────────────────────┐ │
│ │ 🔇 Silent                 [○  ] │ │  ← Toggle switch row (40px)
│ └──────────────────────────────────┘ │
│ ┌──────────────────────────────────┐ │
│ │ Board              [T-Deck Plus] │ │  ← Info row (read-only, 40px)
│ └──────────────────────────────────┘ │
│  ──────────────────────────────────  │  ← Separator
│ ┌──────────────────────────────────┐ │
│ │ Version              [0.9.1-tdeck]│ │  ← Info row (read-only, 40px)
│ └──────────────────────────────────┘ │
│ ┌──────────────────────────────────┐ │
│ │         Reboot Device            │ │  ← Danger button (DANGER red, 40px)
│ └──────────────────────────────────┘ │
└──────────────────────────────────────┘
```

### Setting Row Template

Each row: 304×`BR_TAP_TARGET_MIN` (40px), SURFACE bg, BR_RADIUS.  
Trackball focus: PRIMARY bg at 30% opacity.

| Element | Position     | Details                                   |
|---------|--------------|-------------------------------------------|
| Label   | Left-mid     | Montserrat 14, TEXT color                 |
| Value   | Right-mid    | Montserrat 12, TEXT_SEC color (info rows) |

### Individual Settings

| Row          | Type       | Control              | Behavior                                  |
|--------------|------------|----------------------|-------------------------------------------|
| Node Name    | Info       | Label (right-mid)    | Shows `board_config->name`, read-only    |
| Backlight    | Slider     | 140×10 px (right)    | Range 0–100; maps → 0–255 via I2C to keyboard MCU on `VALUE_CHANGED`; default 80 |
| Volume 🔊   | Slider     | 140×10 px (right)    | Range 0–100; calls `audio_set_volume()` on `VALUE_CHANGED`; plays 880Hz beep on release (unless muted); dimmed 40% opacity when muted |
| Silent 🔇   | Switch     | Toggle (right)       | Calls `audio_set_muted()`; dims Volume slider when enabled; reflects NVS-persisted state on load |
| Board        | Info       | Label (right-mid)    | Shows `board_config->name`, read-only    |
| *(separator)*| Visual     | 1px line, 30% opacity | ─                                        |
| Version      | Info       | Label (right-mid)    | Static `"0.9.1-tdeck"` — not from `esp_app_desc_t` here |
| Reboot       | Button     | Full-width (DANGER)  | Calls `esp_restart()` immediately on click |

**Slider styling:** track=`#333344`, indicator=PRIMARY green, knob=TEXT white.  
**Switch styling:** track off=`#333344`, track on=PRIMARY green, knob=TEXT white.

---

## User Flows

### Boot Flow
```
Power ON
  └──▶ ui_graphics_init()
         └──▶ scr_splash_create()   [Splash shown]
                └──▶ 2s LVGL timer
                       └──▶ splash_timer_cb()
                              ├── lv_refr_now()        [flush pending layouts]
                              ├── lv_obj_clean()        [destroy splash]
                              ├── lv_port_touch_init()
                              ├── lv_port_trackball_init()
                              ├── lv_port_keyboard_init()
                              └── layout_create()      [Main UI ready, Chat tab default]
```

### Tab Navigation
```
User taps tab button
  └──▶ tab_click_cb()
         └──▶ layout_set_tab(tab)
                ├── Highlight active tab button (PRIMARY 30% bg)
                ├── lv_obj_clean(content_area)    [destroy current tab content]
                ├── If TAB_CHAT: clear unread badge
                └──▶ scr_{tab}_create(layout)    [build new content]
```

### Sending a Message
```
Chat List → tap [+ New]  (or tap any message card)
  └──▶ scr_chat_messages_open(layout, 0)
         ├── Hide tab bar
         ├── Expand content area to 320×220
         ├── Build header, message list, compose bar
         └── Focus keyboard on textarea

User types + taps [✓ Send]
  └──▶ send_click_cb()
         ├── mesh_send_broadcast(text, len)
         └── Clear textarea (message list NOT yet refreshed — TODO)

User taps [←] Back
  └──▶ back_click_cb()
         ├── Show tab bar
         ├── Restore content area to 320×180
         └── scr_chat_list_create(layout)
```

### Receiving a Message
```
mesh_task receives packet
  └──▶ msg_store_add()
  └──▶ ui_graphics_notify(UI_EVT_MSG_RECEIVED)

2s status_refresh_timer_cb fires:
  ├── If active tab == TAB_CHAT:
  │     └── layout_set_tab(TAB_CHAT)   [refresh list, clear badge]
  └── Else:
        └── layout_set_unread(count++) [show/update red badge on Chat tab]
```

### Live Data Refresh (Nodes / Stats)
```
5s tab_refresh_timer_cb fires:
  └── If active_tab == TAB_STATS or TAB_NODES:
        └── layout_set_tab(active_tab)   [rebuild with fresh mesh state]
```

---

## Input Model

The T-Deck Plus has three input mechanisms that all feed into LVGL's input device system:

| Device    | LVGL Type   | Implementation               | Notes                                   |
|-----------|-------------|------------------------------|-----------------------------------------|
| Touch     | Pointer     | `lv_port_touch.c` (GT911 polling) | INT pin GPIO16 left floating; 0x5D I2C |
| Trackball | Encoder     | `lv_port_trackball.c`        | 5-way nav: up/down/left/right/click    |
| Keyboard  | Keypad      | `lv_port_keyboard.c` (I2C polling, 20ms) | No ISR (GPIO46 unreliable); 0x55 I2C |

### Focus / Navigation

- A default LVGL group is created; interactive widgets (buttons, sliders, switches, message cards) are added to it.
- Trackball moves focus between widgets; click = activate.
- Keyboard types into the focused textarea (e.g., compose field in message view).
- Chat message card focus shows PRIMARY-tinted highlight.

---

## Known Limitations & TODOs

| Area         | Issue / TODO                                                              |
|--------------|---------------------------------------------------------------------------|
| Chat         | After sending, message list does NOT refresh — sent message not shown until tab re-enter |
| Chat         | `scr_chat_messages_on_recv()` is a stub — new messages don't appear if message view is open |
| Chat         | Channel concept stubbed at idx=0 only — no multi-channel support          |
| Chat         | Message preview truncation uses `LV_LABEL_LONG_DOT` with fixed 290px width |
| Nodes        | Node cards are clickable but no action is defined on click                |
| Status bar   | GPS label is static text "GPS" — no real GPS integration                  |
| Status bar   | Time label shows `--:--` — no RTC integration                             |
| Status bar   | Node name is hardcoded "BRAMBLE" — `identity` component doesn't expose `get_name()` yet |
| Settings     | Version is hardcoded `"0.9.1-tdeck"` — should read from `esp_app_desc_t` like splash does |
| Settings     | Node name is read-only; no edit flow exists                               |
| Settings     | Backlight slider sets keyboard MCU backlight; display backlight (GPIO42, active-low) not yet controlled here |
| WiFi         | No WiFi setting — disabled (ESP_ERR_NO_MEM with mesh+BLE loaded)          |
| General      | No visual feedback on Reboot button before `esp_restart()` fires          |
