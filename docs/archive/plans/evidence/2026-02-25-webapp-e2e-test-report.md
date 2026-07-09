# Bramble Webapp — E2E Test Report

**Date:** 2026-02-25  
**Tester:** OpenClaw E2E Agent (2-session run, resumed from prior session)  
**Stack:** React 19 + TypeScript + Vite 6 + Zustand 5, CSS Modules  
**Transport tested:** Mock Node (WebSocket on port 3005)  
**App URL:** https://localhost:3443  
**Screenshots:** `e2e-screenshots/` (69 total)

---

## Summary

| Category | Count |
|---|---|
| Journeys Tested | 9 |
| Screenshots Captured | 69 |
| Issues Found | 6 |
| Blocking issues fixed | 0 (none blocking; all documented) |
| High severity | 1 |
| Medium severity | 3 |
| Low severity / UX | 2 |

---

## Journeys Tested

1. Connection overlay & transport selection
2. Mock WebSocket connect / connecting stage label
3. Chat — broadcast messaging, byte counter, fragmentation, delivery badges
4. Chat — DM conversations, new DM flow, QR share modal
5. Chat — keyboard shortcuts (Ctrl+1-5, `/` focus, Esc blur)
6. Chat — "Show all routes" toggle, auto-scroll, "↓ New messages" button
7. Nodes — neighbor cards, expand/detail, route table, DM from node
8. Config — identity, radio, channels (PSK handling), peer manager
9. Stats — packet counters with deltas, airtime budget tiers, system info
10. Map — node markers, legend, route line styles
11. Responsive — mobile/tablet/desktop viewports (prior session)
12. Reconnection — disconnect/reconnect, IndexedDB persistence (prior + deep-dive)

---

## Critical / High Issues

### BUG-1 — `loadMessages()` never persists fetched messages to IndexedDB ⚠️ HIGH

**File:** `src/store/actions.ts`, lines 344–393  
**Root cause confirmed** by code review.

When the app connects to a node and calls `bramble.getMessages` RPC to fetch historical messages, those messages are added to in-memory Zustand state via `store.addMessage(fwMsg)` but are **never written to IndexedDB**.

The `messageDb.saveMessages()` method exists and is ready to use, but `loadMessages()` never calls it. As a result:

- Messages fetched from the node on each connect session exist only in memory
- After disconnect/reconnect, those messages disappear from the UI
- Only messages that went through `sendMessage()` (line 785: `messageDb.saveMessage()`) or `handleIncomingMessage()` (line 893: `messageDb.saveMessage()`) survive across sessions
- This explains the previous agent's observation: ~30 messages from session 1 disappeared after reconnect; only the 3 messages individually sent/received via RPC/realtime events remained in IndexedDB

**Evidence:** `messageDb.saveMessages()` exists in `src/store/messageDb.ts` (lines 63–76) but is never called from `loadMessages()`.

**Fix:** Collect messages that pass the duplicate check and save them at the end of `loadMessages()`:
```ts
// End of loadMessages() — after the for loop
if (newMessages.length > 0) {
  messageDb.saveMessages(newMessages).catch(() => {});
}
```

---

## Medium Issues

### BUG-2 — Auto-reconnect path doesn't re-initialize IndexedDB ⚠️ MEDIUM

**File:** `src/store/actions.ts`, lines 113–121

The `onReconnect` callback (WiFi/WebSocket auto-reconnect) calls:
```ts
await Promise.all([loadConfig(), loadNeighbors(), loadRoutes(), opt(loadMessages()), loadAirtime()]);
```

But it does NOT call `initMessageStore()`. This means:
- After an auto-reconnect (e.g., brief WiFi drop), `loadMessages()` fires and fetches messages from the node
- These messages are deduplicated against the already-loaded in-memory state (fine)
- But any messages that arrived while offline and were fetched via `loadMessages()` will not be persisted to IndexedDB

**Fix:** Add `await initMessageStore(addrHex)` before `loadMessages()` in the `onReconnect` handler, or ensure `loadMessages()` itself saves new messages (which BUG-1 fix would address).

---

### BUG-3 — Outgoing `from: 0` causes deduplication failure across reconnects ⚠️ MEDIUM

**File:** `src/store/actions.ts`, lines ~785 (`sendMessage`) and ~387 (`loadMessages`)

Outgoing messages created by `sendMessage()` are stored with `from: 0`:
```ts
const msg = {
  id: uuid(),
  direction: 'outgoing',
  from: 0,        // ← always 0
  to: dest,
  ...
};
```

But when `loadMessages()` fetches the same message from firmware, it arrives with the real node address as `from` (e.g., `0x4A555354`).

`isLikelyDuplicate()` checks `existing.from !== candidate.from` (line 339), which returns `false` because `0 !== 0x4A555354`, so the message is **not recognized as a duplicate** and gets added again.

**Result:** After a reconnect (with BUG-1 fixed), the same outgoing message could appear twice in the UI — once from IndexedDB (with `from: 0`) and once from firmware history (with `from: actualAddr`).

**Fix:** Either normalize `from` to the real node address in `sendMessage()`, or make `isLikelyDuplicate()` treat `from: 0` as "self" and match against the configured node address.

---

### BUG-4 — `loadConfig()` error-swallowing can open wrong IndexedDB namespace ⚠️ MEDIUM

**File:** `src/store/actions.ts`, lines 144–150

In `connect()`:
```ts
const opt = (p: Promise<void>) => p.catch((e) => console.warn('[init]', e.message));
await opt(loadConfig());  // errors swallowed
const nodeAddr = store.config?.identity?.address;  // undefined if loadConfig failed
const addrHex = nodeAddr ? nodeAddr.toString(16)... : undefined;
await initMessageStore(addrHex);  // opens 'bramble-messages-default' if addrHex is undefined
```

If `loadConfig()` fails (e.g., RPC timeout, slow ESP32 startup), `addrHex` is `undefined`, and `initMessageStore('default')` is called. This opens the `bramble-messages-default` IndexedDB database instead of `bramble-messages-DEADBEEF`, silently discarding all previously cached messages for that node.

**Fix:** Retry `loadConfig()` separately from the `opt()` wrapping, or ensure `initMessageStore` falls back to the last known address from localStorage.

---

## Low / UX Issues

### BUG-5 — SNR values displayed without rounding 🔵 LOW

**File:** `src/pages/Nodes/NeighborCard.tsx`, line 100

```tsx
<span title="Signal-to-Noise Ratio">SNR: {neighbor.snr} dB</span>
```

The raw floating-point value is displayed, showing values like `8.673362300646515 dB` instead of `8.7 dB`. The mock node sends floating-point SNR values and the UI displays them with full IEEE 754 precision.

**Fix:**
```tsx
<span title="Signal-to-Noise Ratio">SNR: {neighbor.snr?.toFixed(1)} dB</span>
```

---

### BUG-6 — Clipboard copy permission error 🔵 LOW

**File:** UI — copy address buttons throughout  
**Error observed:** `Failed to execute 'writeText' on 'Clipboard': Write permission denied.`

Copy-to-clipboard fails silently for users in contexts where `navigator.clipboard.writeText` doesn't have permission (e.g., non-HTTPS in some browsers, or browser automation). There's no visible error feedback to the user when copy fails.

**Fix:** Add a fallback to the legacy `document.execCommand('copy')` approach, or show a toast notification on clipboard failure.

---

## Features Verified ✅

### Connection Overlay
- Transport selection (USB/Serial, Bluetooth, WiFi, Mock Node/WebSocket) ✅
- WiFi IP input field appears only when WiFi selected ✅
- PSK field for new channels is `type="password"` (masked) ✅
- Connecting label: shows "Connecting…" for WebSocket transport (correct fallthrough) ✅
- Connection error display ✅

### Chat Tab
- Broadcast conversation: send messages, view in UI ✅
- Byte counter appears when text is typed (`148/616` format) ✅
- Fragmentation indicator: `250/616 2 fragments` shown for 250-byte message (>203 single-packet limit) ✅
- Delivery badge: shows colored status dot with title ("Sent to next hop") ✅
- "Show all routes" toggle persists (localStorage) ✅
- Keyboard shortcut Ctrl+1 switches to Chat tab ✅
- `/` shortcut focuses compose textarea ✅
- Esc blurs active element ✅
- Channel conversations with PSK indicators in sidebar ✅
- DM conversations ✅
- New DM flow ✅
- QR share modal (prior session) ✅

### Nodes Tab
- Neighbor cards display: address, RSSI, PDR, SNR, last seen, location tier ✅
- Neighbor card expand: shows full address, airtime remaining, GPS coordinates, "Send DM" button ✅
- Route table: Destination, Next Hop, Hops, Metric, State, Age columns ✅
- Route count badge (6 routes shown) ✅
- "DM from node" → navigates to Chat tab, creates DM conversation ✅
- Ctrl+2 keyboard shortcut ✅

### Map Tab
- Node markers shown with friendly names (Hilltop, TrailHead, WaterSt, Anthem, E2E-Test) ✅
- Map legend: route types (You, Exact peer, Zone peer, Direct (1 hop), Multi-hop) ✅
- Route state legend (Active, Stale, Broken, Discovering) ✅
- Location sharing status displayed ✅
- Zoom in/out controls ✅
- Ctrl+3 keyboard shortcut ✅

### Config Tab
- Identity: address, key hash, node name edit, mailbox toggle ✅
- Radio: TX power slider, SF/BW/CR/freq dropdowns ✅
- Channels: list with PSK/epoch info, set default, delete, share ✅
- New channel form with PSK input (password type) ✅
- Peer manager: peer list with inline name editing ✅
- Location config: contact rules with tier/interval controls ✅
- Ctrl+4 keyboard shortcut ✅

### Stats Tab
- Packet counters with delta indicators (e.g., `55 ↑1 Sent`) ✅
- Airtime budget: Critical/Normal/Broadcast tiers with % remaining and refill time ✅
- System info: uptime, free heap, firmware version, node name, address, pubkey hash ✅
- Refresh button ✅
- Traffic Monitor (disabled, shows prompt to enable in Config) ✅
- Ctrl+5 keyboard shortcut ✅

### Responsive (prior session)
- Mobile (375×812): layouts adapt, tabs visible ✅
- Tablet (768×1024): two-panel layout ✅
- Desktop (1440×900): full sidebar navigation ✅

---

## Code Review Findings (Static Analysis)

### Race condition risk: `loadConfig()` optional but required for IndexedDB namespace
`src/store/actions.ts` lines 144–150. Covered by BUG-4.

### `isLikelyDuplicate` 5-second window risk
`src/store/actions.ts` lines 336–342. The 5-second timestamp window for deduplication could false-positive for users who rapidly send identical short messages (e.g., "ok" × 2 within 5 seconds). Low probability in practice.

### Missing error handler visibility for `saveMessage` failures
All `messageDb.saveMessage()` calls use `.catch(() => {})` which silently swallows IndexedDB errors (e.g., quota exceeded, private browsing mode). There is a comment mentioning this but no telemetry or user feedback.

### `resetNodeData` clears everything including pending messages
`src/store/index.ts` lines 270–281. On reconnect, `resetNodeData()` clears all in-memory messages before `initMessageStore()` reloads them from IndexedDB. If `initMessageStore()` fails (e.g., IndexedDB unavailable), the user loses all in-memory message history with no notification.

### `loadMessages()` timestamp conversion assumes live uptime
`src/store/actions.ts` lines 381–384. Uptime-based timestamps are converted to wall clock using `now - (deviceUptime - msgUptimeS) * 1000`. If the firmware's `uptimeSec` is stale (from a previous `loadStatus()` call), timestamps could be slightly off. Low impact.

### Location section - no label for spinbutton fields
`src/pages/Config/LocationSection.tsx` (inferred from snapshot). The location contact rows show `spinbutton [ref=e92]` with value "300" but no visible label in the accessibility tree, only a placeholder. Accessibility concern.

---

## Screenshots Index

| # | Filename | Description |
|---|---|---|
| 00–42 | (prior session) | Initial load through reconnection test |
| 43 | 43-reconnect-overlay.png | Connection overlay after reload |
| 44 | 44-mock-node-selected.png | Mock Node WebSocket transport selected |
| 45 | 45-connecting-state.png | "Connecting…" state during WebSocket connect |
| 46 | 46-connected-state.png | Connected state - app loaded |
| 47 | 47-byte-counter-fragment.png | Byte counter at 148/616 (single packet) |
| 48 | 48-delivery-badge-sent.png | Delivery badge "Sent to next hop" |
| 49 | 49-fragmentation-indicator.png | Fragmentation screenshot attempt |
| 50 | 50-fragmentation-counter.png | "250/616 2 fragments" indicator |
| 51 | 51-routes-toggle-on.png | Routes toggle → "Hide all routes" |
| 52 | 52-nodes-tab.png | Nodes tab with neighbors and route table |
| 53 | 53-neighbor-expanded.png | Neighbor card expanded (SNR precision visible) |
| 54 | 54-neighbor-expanded-detail.png | Neighbor card with DM button |
| 55 | 55-dm-from-node.png | DM opened from Nodes tab |
| 56 | 56-config-tab.png | Config tab overview |
| 57 | 57-peer-edit.png | Peer name editing inline |
| 58 | 58-peer-manager.png | Peer manager with location contacts |
| 59 | 59-stats-tab.png | Stats tab with airtime budgets |
| 60 | 60-map-tab.png | Map with node markers and legend |
| 61 | 61-messages-sent.png | Broadcast messages sent |
| 62 | 62-jump-button.png | Auto-scroll test |

---

## Issue Priority Summary

| ID | Severity | File | Line | Description |
|---|---|---|---|---|
| BUG-1 | **HIGH** | `src/store/actions.ts` | 344–393 | `loadMessages()` never saves fetched messages to IndexedDB |
| BUG-2 | MEDIUM | `src/store/actions.ts` | 113–121 | Auto-reconnect path skips `initMessageStore()` |
| BUG-3 | MEDIUM | `src/store/actions.ts` | ~785, ~387 | Outgoing `from: 0` breaks dedup against firmware history |
| BUG-4 | MEDIUM | `src/store/actions.ts` | 144–150 | `loadConfig()` error → wrong IndexedDB namespace opened |
| BUG-5 | LOW | `src/pages/Nodes/NeighborCard.tsx` | 100 | SNR displayed with raw float precision |
| BUG-6 | LOW | UI (copy buttons) | — | Clipboard `writeText` fails silently with no user feedback |

---

## Recommended Fix Order

1. **BUG-1** — Add `messageDb.saveMessages(newMessages)` at the end of `loadMessages()`. This is the primary cause of message history loss.
2. **BUG-3** — Fix outgoing `from: 0` so dedup works correctly (prevents duplicates once BUG-1 is fixed)
3. **BUG-2** — Add `initMessageStore()` to the auto-reconnect handler
4. **BUG-4** — Protect `initMessageStore()` from receiving `undefined` when `loadConfig()` fails; consider persisting last-known node address
5. **BUG-5** — One-line fix: `neighbor.snr?.toFixed(1)` in NeighborCard
6. **BUG-6** — Add clipboard fallback + toast on failure

---

*Report generated by OpenClaw E2E Agent. All findings are from code analysis and browser automation against the live mock-node environment.*
