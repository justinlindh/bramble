# Bramble Web Config & Messaging App — Phase 10 Implementation Plan

> ✅ **ALL PHASES COMPLETE**

> **For Agent:** REQUIRED SUB-SKILL: Execute this plan task-by-task. Each task is self-contained with explicit file paths, expected output, and a commit step.

**Date:** 2026-02-17  
**Branch:** `feature/sim-component-integration`  
**Status:** Ready to implement

---

## Overview

Build the companion web app for Bramble nodes. The app connects to a device via **Web Serial** (USB/UART) or **Web Bluetooth** (BLE) and provides:

- Live chat: DM conversations and channel group messages
- Configuration: identity, radio settings, channel management, peer management
- Network view: neighbor table, routes, relay path display for Critical messages
- Stats dashboard: airtime budgets per tier, packet counters, uptime

The app runs **100% in the browser** — no server, no backend. Static HTML/CSS/JS files that can be:
- Served via `python3 -m http.server` during development
- Deployed to any static CDN
- Served directly from the ESP32's SPIFFS partition (future)

### Prototype vs. Planned App

A vanilla-JS prototype already exists in `web-app/`. It proves the JSON-RPC over Web Serial transport works and covers the four core pages at a basic level. The Phase 10 app (`webapp/`) is a clean React + TypeScript + Vite rewrite with:
- Full TypeScript types for every protocol data structure
- Proper state management (Zustand)
- Tested components (Vitest + React Testing Library)
- Responsive layout for mobile BLE use
- Offline-capable PWA manifest
- CI build

The prototype (`web-app/`) is kept as-is for reference. All new work goes in `webapp/`.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Browser (webapp/)                           │
│                                                                     │
│  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐│
│  │   Chat UI        │   │  Config UI        │   │   Stats UI       ││
│  │  DMConversation  │   │  NodeIdentity     │   │  AirtimeDash     ││
│  │  ChannelView     │   │  RadioSettings    │   │  PacketCounters  ││
│  │  MessageBubble   │   │  ChannelManager   │   │  UptimeCard      ││
│  │  DeliveryStatus  │   │  PeerManager      │   │                  ││
│  └────────┬─────────┘   └────────┬──────────┘   └────────┬─────────┘│
│           │                      │                        │          │
│  ┌────────▼──────────────────────▼────────────────────────▼─────────┐│
│  │                     App State (Zustand)                           ││
│  │  messages[], contacts{}, channels[], routes{}, config{}, status{} ││
│  └────────────────────────────┬──────────────────────────────────────┘│
│                               │                                       │
│  ┌────────────────────────────▼──────────────────────────────────────┐│
│  │                BrambleClient (transport abstraction)               ││
│  │                                                                    ││
│  │   sendRPC(method, params) → Promise<result>                       ││
│  │   onNotification(method, params) → void                           ││
│  └──────────────┬──────────────────────────────────┬─────────────────┘│
│                 │                                  │                   │
│  ┌──────────────▼───────────┐    ┌─────────────────▼─────────────────┐│
│  │   SerialTransport        │    │    BLETransport                   ││
│  │  (Web Serial API)        │    │  (Web Bluetooth API)              ││
│  │  115200 8N1              │    │  Nordic UART Service UUID         ││
│  │  line-delimited JSON-RPC │    │  chunked 20-byte writes           ││
│  └──────────────────────────┘    └───────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────────┘
                          │ USB/UART or BLE
┌─────────────────────────▼─────────────────────────────────────────┐
│                    ESP32 Bramble Node                              │
│  components/ble/json_rpc.c — parses requests, builds responses    │
│  components/routing/ — neighbor/route tables                      │
│  components/airtime/ — budget tracking                            │
│  components/reliability/ — ACKs, delivery receipts               │
│  components/crypto/ — identity                                    │
└───────────────────────────────────────────────────────────────────┘
```

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Framework | React 19 + TypeScript | Same as simulator; reuse patterns and types |
| Build tool | Vite 6 | Fast HMR; same as simulator |
| State | Zustand | Minimal, no boilerplate; global store for shared data |
| Transport | Abstraction layer with Serial + BLE | Swap without touching UI |
| Styling | CSS Modules + CSS custom props | No runtime overhead; works without JS for static fallback |
| Testing | Vitest + RTL | Same as simulator toolchain |
| No router | Single-page tab switching | App is too small to need react-router |
| No server | Pure static | Works offline, serves from SPIFFS |

---

## JSON-RPC API Specification

The firmware exposes a JSON-RPC 2.0 interface over UART (line-delimited JSON, `\n` terminated) and optionally over BLE (NUS: Nordic UART Service). The web app is the sole client of this interface.

### Transport Protocol

**Request format:**
```json
{"jsonrpc":"2.0","id":1,"method":"bramble.getStatus","params":{}}
```

**Response format:**
```json
{"jsonrpc":"2.0","id":1,"result":{"uptime_s":3600,"free_heap":120000,...}}
```

**Error format:**
```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"Method not found"}}
```

**Notification (firmware → app, no id):**
```json
{"jsonrpc":"2.0","method":"bramble.onMessage","params":{"from":305441741,...}}
```

All messages are newline-terminated (`\n`). The firmware's existing `json_rpc.c` already parses these; new methods simply need to be registered in the dispatch table.

### Method Reference

#### Status & Health

| Method | Params | Returns | Description |
|--------|--------|---------|-------------|
| `bramble.getStatus` | `{}` | `StatusResult` | Uptime, heap, counters, firmware version |
| `bramble.getAirtime` | `{}` | `AirtimeResult` | Per-tier budgets (ms remaining, ms max, refill time) |

#### Identity & Config

| Method | Params | Returns | Description |
|--------|--------|---------|-------------|
| `bramble.getConfig` | `{}` | `ConfigResult` | Full config: identity + radio + channels |
| `bramble.setRadio` | `RadioParams` | `{}` | Set TX power, SF, BW, CR, frequency |
| `bramble.setNodeName` | `{name: string}` | `{}` | Set short name (max 8 chars) |

#### Neighbors & Routes

| Method | Params | Returns | Description |
|--------|--------|---------|-------------|
| `bramble.getNeighbors` | `{}` | `NeighborResult` | Direct neighbors with RSSI/SNR/PDR |
| `bramble.getRoutes` | `{}` | `RouteResult` | Routing table: dest, next_hop, hops, metric, state |
| `bramble.getNodeInfo` | `{addr: number}` | `NodeInfo` | Cached beacon data for a specific node |

#### Channels

| Method | Params | Returns | Description |
|--------|--------|---------|-------------|
| `bramble.getChannels` | `{}` | `ChannelResult` | Channel list (name, index, PSK present, epoch) |
| `bramble.addChannel` | `{name: string, psk?: string}` | `{index: number}` | Add a channel (PSK is UTF-8, hashed to key) |
| `bramble.removeChannel` | `{index: number}` | `{}` | Remove channel by index |
| `bramble.setDefaultChannel` | `{index: number}` | `{}` | Set default outbound channel |

#### Messaging

| Method | Params | Returns | Description |
|--------|--------|---------|-------------|
| `bramble.sendMessage` | `SendParams` | `{packet_id: number}` | Send DM or channel message |
| `bramble.getMessages` | `{since_id?: number, limit?: number}` | `MessageResult` | Fetch messages from node's ring buffer |

#### Key Backup (BLE only)

| Method | Params | Returns | Description |
|--------|--------|---------|-------------|
| `bramble.exportKey` | `{}` | `{key_b64: string}` | Export encrypted identity for backup |
| `bramble.importKey` | `{key_b64: string}` | `{}` | Restore identity from backup |

### Firmware Notifications (push events)

| Event | Params | When sent |
|-------|--------|-----------|
| `bramble.onMessage` | `IncomingMessage` | New message received (DM or channel) |
| `bramble.onAck` | `{packet_id: number, status: 'delivered'\|'failed', relay_path?: number[]}` | Delivery receipt for sent message |
| `bramble.onNeighborChange` | `{added: number[], removed: number[]}` | Neighbor table changed |
| `bramble.onRouteUpdate` | `{dest: number, state: string, hops: number}` | Route state changed |
| `bramble.onAirtimeWarning` | `{tier: number, remaining_ms: number}` | Airtime budget < 10% |

### Error Codes

| Code | Meaning |
|------|---------|
| `-32700` | Parse error (malformed JSON) |
| `-32600` | Invalid request |
| `-32601` | Method not found |
| `-32602` | Invalid params |
| `-32000` | Node busy (retry after 1s) |
| `-32001` | Airtime exhausted for tier |
| `-32002` | Destination unreachable (no route) |
| `-32003` | Message too long |
| `-32004` | Channel not found |
| `-32005` | Auth failed (bad PSK) |

---

## TypeScript Data Model

All types go in `webapp/src/types/bramble.ts`.

```typescript
// ─── Identity ──────────────────────────────────────────────────────────

export interface NodeIdentity {
  address: number;          // 32-bit node address
  pubkeyHash: number;       // 32-bit hash of public key
  name: string;             // Short name, max 8 chars
  pubkeyB64: string;        // Base64-encoded public key (display only)
}

// ─── Radio ─────────────────────────────────────────────────────────────

export interface RadioConfig {
  txPowerDbm: number;       // 2-20
  sf: 7 | 8 | 9 | 10 | 11 | 12;
  bwKhz: 125 | 250 | 500;
  cr: 5 | 6 | 7 | 8;       // coding rate denominator (4/5 = 5, etc.)
  freqMhz: number;          // e.g. 915.0
}

// ─── Channels ──────────────────────────────────────────────────────────

export interface Channel {
  index: number;
  name: string;
  hasPsk: boolean;          // don't send PSK over the wire back to app
  epoch: number;            // key rotation epoch
  isDefault: boolean;
}

// ─── Neighbors & Routes ────────────────────────────────────────────────

export interface Neighbor {
  addr: number;
  rssi: number;
  snr: number;
  deliveryRate: number;     // 0-255, 255 = 100%
  lastHeardMs: number;      // milliseconds ago
  isMailbox: boolean;
  airtimeRemaining: number; // 0-100 %
}

export interface Route {
  dest: number;
  nextHop: number;
  hopCount: number;
  metric: number;
  state: 'active' | 'stale' | 'broken' | 'discovering';
  lastUsedMs: number;
}

// ─── Messages ──────────────────────────────────────────────────────────

export type MessageTier = 'broadcast' | 'normal' | 'critical';
export type MessageDirection = 'outgoing' | 'incoming';

export type DeliveryStatus =
  | 'queued'      // in app, not yet sent to node
  | 'sending'     // RPC call in flight
  | 'sent'        // node accepted (packet_id returned)
  | 'delivered'   // delivery receipt received (ACK from dest)
  | 'failed'      // all retries exhausted
  | 'timeout';    // no receipt within UI timeout

export interface RelayHop {
  addr: number;
  rssi: number;
}

export interface Message {
  id: string;               // UUID (client-generated for outgoing, server msg_id for incoming)
  packetId?: number;        // firmware packet_id, set on 'sent' status
  direction: MessageDirection;
  from: number;             // node address (0 = self)
  to: number;               // destination addr, 0xFFFFFFFF = broadcast
  channelIndex?: number;    // set for channel messages, undefined for DM
  text: string;
  timestampMs: number;      // client local time (outgoing) or decoded from packet (incoming)
  tier: MessageTier;
  status: DeliveryStatus;
  relayPath?: RelayHop[];   // populated from delivery receipt for Critical messages
}

// ─── Conversations ─────────────────────────────────────────────────────

export interface Conversation {
  /** 'dm:0x{addr}' or 'ch:{index}' */
  id: string;
  label: string;
  peerAddr?: number;        // set for DMs
  channelIndex?: number;    // set for channel convos
  lastMessage?: string;
  lastMessageTime?: number;
  unreadCount: number;
}

// ─── Airtime ───────────────────────────────────────────────────────────

export interface AirtimeTier {
  name: 'critical' | 'normal' | 'broadcast';
  remainingMs: number;
  maxMs: number;
  usedPct: number;          // 0-100
  refillAtMs: number;       // epoch ms
}

export interface AirtimeStatus {
  tiers: [AirtimeTier, AirtimeTier, AirtimeTier]; // critical, normal, broadcast
}

// ─── Status ────────────────────────────────────────────────────────────

export interface NodeStatus {
  uptimeSec: number;
  freeHeapBytes: number;
  fwVersion: string;
  txCount: number;
  rxCount: number;
  droppedCount: number;
  neighborCount: number;
  routeCount: number;
  airtimeUsedMs: number;    // total since boot
}

// ─── Config (full) ─────────────────────────────────────────────────────

export interface BrambleConfig {
  identity: NodeIdentity;
  radio: RadioConfig;
  channels: Channel[];
}

// ─── RPC types ─────────────────────────────────────────────────────────

export interface SendParams {
  dest: number;             // 0xFFFFFFFF for broadcast, 0xFFFFFFFE for default channel
  text: string;
  tier?: MessageTier;
  channelIndex?: number;    // set for channel messages
}

export interface IncomingMessage {
  from: number;
  to: number;
  text: string;
  tier: MessageTier;
  channelIndex?: number;
  timestamp: number;        // node epoch seconds
  msgId: string;
}

export interface AckNotification {
  packetId: number;
  status: 'delivered' | 'failed';
  relayPath?: RelayHop[];
}

// ─── Transport abstraction ─────────────────────────────────────────────

export interface Transport {
  readonly connected: boolean;
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  sendRPC<T = unknown>(method: string, params?: Record<string, unknown>, timeoutMs?: number): Promise<T>;
  onNotification(cb: (method: string, params: unknown) => void): void;
}

export type TransportType = 'serial' | 'ble';

// ─── App state ─────────────────────────────────────────────────────────

export type ConnectionState = 'disconnected' | 'connecting' | 'connected' | 'error';

export interface AppState {
  connectionState: ConnectionState;
  connectionError?: string;
  transport: Transport | null;
  config: BrambleConfig | null;
  status: NodeStatus | null;
  airtime: AirtimeStatus | null;
  neighbors: Neighbor[];
  routes: Route[];
  messages: Message[];
  conversations: Map<string, Conversation>;
  activeConversationId: string;
}
```

---

## UI Wireframes

### Global Layout (Mobile-first, responsive)

```
┌─────────────────────────────────────────────┐
│ 🌿 Bramble    [Serial ▾] [Connect] ● green  │  ← topbar
├─────────────────────────────────────────────┤
│                                             │
│              PAGE CONTENT                   │
│                                             │
│                                             │
│                                             │
│                                             │
│                                             │
├─────────────────────────────────────────────┤
│  💬 Chat   📡 Nodes   ⚙️ Config   📊 Stats  │  ← tabbar
└─────────────────────────────────────────────┘
```

Desktop: sidebar nav replaces the tabbar; content fills width. Breakpoint: 640px.

### Chat Page

```
┌─────────────────────────────────────────────┐
│ [📢 Broadcast] [0x1A3F ●] [0x3B7C]  [+ DM] │  ← conversation tabs
├─────────────────────────────────────────────┤
│                                             │
│         ┌────────────────────────────┐      │
│         │ Hello from the hilltop!    │  ←me │
│         │ 14:32  ✓✓ via 0x3B7C      │      │
│         └────────────────────────────┘      │
│  ┌──────────────────────────┐               │
│  │ Got you, what's your ETA?│ from 0x1A3F → │
│  │ 14:34                    │               │
│  └──────────────────────────┘               │
│                                             │
│         ┌────────────────────────────┐      │
│         │ About 30 min out           │  ←me │
│         │ 14:35  ● sending           │      │
│         └────────────────────────────┘      │
├─────────────────────────────────────────────┤
│  [Type a message…        ] [🔴 Critical][▶] │
└─────────────────────────────────────────────┘
```

**Delivery status icons:**
- `●` — sending (spinner)
- `✓` — sent to node (packet_id acknowledged)
- `✓✓` — delivered (receipt from destination)
- `✗` — failed (all retries exhausted)
- `!` — timeout (no receipt, ambiguous)

For **Critical** tier messages, a tappable relay path appears beneath the delivery status:
```
✓✓ via You → 0x3B7C → 0x5E1A → dest
```
Each node address in the path is tappable to focus that node in the Nodes tab.

**New DM flow:** Tap `[+ DM]` → enter/paste address or pick from discovered nodes → conversation tab opens.

### Nodes Page

```
┌─────────────────────────────────────────────┐
│ 📡 Neighbors (3)              [Refresh ↻]   │
├─────────────────────────────────────────────┤
│ ┌─────────────────────────────────────────┐ │
│ │ 0x1A3F  "Relay-West"                    │ │
│ │ RSSI: -78 dBm  SNR: +6 dB  PDR: 98%    │ │
│ │ Last heard: 12s ago    Mailbox: ✓       │ │
│ │ ─────────────────────────────────────── │ │
│ │ Routes through: 5 destinations          │ │
│ │ Airtime remaining: 72%                  │ │
│ │ [📨 Send DM]    [View Routes]           │ │
│ └─────────────────────────────────────────┘ │
│ ┌─────────────────────────────────────────┐ │
│ │ 0x3B7C  (unknown name)                  │ │
│ │ RSSI: -104 dBm  SNR: -2 dB  PDR: 71%   │ │
│ │ Last heard: 47s ago    Mailbox: ✗       │ │
│ └─────────────────────────────────────────┘ │
├─────────────────────────────────────────────┤
│ 🗺 Routes (12)                              │
│ ┌─────────────────────────────────────────┐ │
│ │ Dest      Next Hop   Hops  Metric State │ │
│ │ 0x5E1A    0x1A3F     2     187    active│ │
│ │ 0x9F2C    0x3B7C     3     142    stale │ │
│ └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

### Config Page

```
┌─────────────────────────────────────────────┐
│ ⚙️ Configuration                            │
├─────────────────────────────────────────────┤
│ ▼ Identity                                  │
│   Address    0x1B3C4D5E                     │
│   Name       [Hilltop     ]  [Save]         │
│   Public Key 8f2a…3c9b (tap to copy)        │
│   [Export Key Backup]                       │
│                                             │
│ ▼ Radio                                     │
│   TX Power   [20 ▾] dBm   (14 options)     │
│   Spreading  [10 ▾]                         │
│   Bandwidth  [125 kHz ▾]                    │
│   Coding     [4/5 ▾]                        │
│   Frequency  [915.000 MHz    ]              │
│                          [Save Radio]       │
│                                             │
│ ▼ Channels                                  │
│   [📢 Default]  bramble-default   (public)  │
│   [🔒 Alpha]    alpha-team  (PSK)  [✕]     │
│   ───────────────────────────────────────── │
│   Name [           ] PSK [           ] [+] │
│                                             │
│ ▼ Peers                                     │
│   (nodes seen via routing, can add names)   │
│   0x1A3F  → "Relay-West"                   │
│   0x3B7C  → (no name)  [Set Name]          │
└─────────────────────────────────────────────┘
```

### Stats Page

```
┌─────────────────────────────────────────────┐
│ 📊 Statistics               [↻ Auto-refresh]│
├─────────────────────────────────────────────┤
│ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐│
│ │  3,124 │ │  8,891 │ │    3   │ │   12   ││
│ │  TX    │ │  RX    │ │Nbrs    │ │ Routes ││
│ └────────┘ └────────┘ └────────┘ └────────┘│
│ ┌────────┐ ┌────────┐                       │
│ │ 4h 12m │ │ 118 KB │                       │
│ │ Uptime │ │ Free   │                       │
│ └────────┘ └────────┘                       │
├─────────────────────────────────────────────┤
│ ⏱ Airtime Budget                            │
│                                             │
│ Critical   ████████████░░░  82%  remaining  │
│            [refills in 43m]                 │
│                                             │
│ Normal     ██████░░░░░░░░░  44%  remaining  │
│            [refills in 43m]                 │
│                                             │
│ Broadcast  ████████████████ 99%  remaining  │
│            [refills in 43m]                 │
└─────────────────────────────────────────────┘
```

---

## Directory Structure

```
webapp/
├── index.html
├── vite.config.ts
├── tsconfig.json
├── package.json
├── public/
│   ├── manifest.json           (PWA manifest)
│   └── favicon.svg
├── src/
│   ├── main.tsx                (entry: ReactDOM.createRoot)
│   ├── App.tsx                 (topbar + tabbar layout, connection overlay)
│   ├── types/
│   │   └── bramble.ts          (all TypeScript types from §Data Model)
│   ├── transport/
│   │   ├── index.ts            (transport factory + BrambleClient export)
│   │   ├── SerialTransport.ts  (Web Serial JSON-RPC)
│   │   └── BLETransport.ts     (Web Bluetooth JSON-RPC)
│   ├── store/
│   │   ├── index.ts            (Zustand store: AppState)
│   │   ├── actions.ts          (thunks: connect, loadConfig, sendMessage, etc.)
│   │   └── selectors.ts        (derived data: getConversation, getUnread, etc.)
│   ├── hooks/
│   │   ├── useRPC.ts           (React hook: call RPC with loading/error state)
│   │   ├── usePoll.ts          (polling hook with configurable interval)
│   │   └── useNotifications.ts (subscribe to push events from node)
│   ├── pages/
│   │   ├── Chat/
│   │   │   ├── Chat.tsx        (conversation list + active conversation)
│   │   │   ├── MessageBubble.tsx
│   │   │   ├── DeliveryBadge.tsx
│   │   │   ├── RelayPathDisplay.tsx
│   │   │   ├── ConversationTabs.tsx
│   │   │   └── Compose.tsx
│   │   ├── Nodes/
│   │   │   ├── Nodes.tsx
│   │   │   ├── NeighborCard.tsx
│   │   │   └── RouteTable.tsx
│   │   ├── Config/
│   │   │   ├── Config.tsx
│   │   │   ├── IdentitySection.tsx
│   │   │   ├── RadioForm.tsx
│   │   │   ├── ChannelManager.tsx
│   │   │   └── PeerManager.tsx
│   │   └── Stats/
│   │       ├── Stats.tsx
│   │       ├── AirtimeBars.tsx
│   │       └── StatCards.tsx
│   ├── components/
│   │   ├── ConnectionOverlay.tsx
│   │   ├── AddressLabel.tsx    (0xABCD with copy button and optional name)
│   │   ├── StatusDot.tsx
│   │   ├── Toast.tsx
│   │   └── ErrorBoundary.tsx
│   └── styles/
│       ├── global.css          (custom properties, resets)
│       ├── App.module.css
│       └── *.module.css        (per-component CSS modules)
├── test/
│   ├── setup.ts
│   ├── transport/
│   │   └── SerialTransport.test.ts
│   └── store/
│       └── actions.test.ts
└── docker/
    ├── Dockerfile              (nginx serving dist/)
    └── nginx.conf
```

---

## Transport Layer — Key Code Patterns

The transport layer already exists as prototype code in `web-app/serial.js` and `web-app/ble.js`. These are ported to TypeScript and typed. The key patterns are reproduced here as the reference implementation.

### SerialTransport.ts

```typescript
import type { Transport } from '../types/bramble';

// Espressif and common USB-serial bridge VIDs
const FILTERS = [
  { usbVendorId: 0x303A }, // Espressif native USB
  { usbVendorId: 0x10C4 }, // CP2102 (Silicon Labs)
  { usbVendorId: 0x1A86 }, // CH340
  { usbVendorId: 0x0403 }, // FTDI
];

interface Pending {
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

export class SerialTransport implements Transport {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private _connected = false;
  private rpcId = 0;
  private pending = new Map<number, Pending>();
  private notifyCb: ((method: string, params: unknown) => void) | null = null;
  private lineBuf = '';
  private readonly decoder = new TextDecoder();
  private readonly encoder = new TextEncoder();

  get connected() { return this._connected; }

  async connect(baudRate = 115200): Promise<void> {
    if (!('serial' in navigator)) throw new Error('Web Serial API not supported');
    this.port = await navigator.serial.requestPort({ filters: FILTERS });
    await this.port.open({ baudRate });
    this.writer = this.port.writable!.getWriter();
    this.reader = this.port.readable!.getReader();
    this._connected = true;
    this.startReadLoop();
  }

  private startReadLoop(): void {
    (async () => {
      try {
        while (this._connected) {
          const { value, done } = await this.reader!.read();
          if (done) break;
          this.lineBuf += this.decoder.decode(value, { stream: true });
          this.processLines();
        }
      } catch { /* port closed */ }
    })();
  }

  private processLines(): void {
    const lines = this.lineBuf.split('\n');
    this.lineBuf = lines.pop() ?? '';
    for (const raw of lines) {
      const line = raw.trim();
      if (!line) continue;
      let msg: Record<string, unknown>;
      try { msg = JSON.parse(line); } catch { continue; }

      if ('id' in msg && this.pending.has(msg.id as number)) {
        const { resolve, reject, timer } = this.pending.get(msg.id as number)!;
        clearTimeout(timer);
        this.pending.delete(msg.id as number);
        if (msg.error) reject(new Error((msg.error as { message: string }).message));
        else resolve(msg.result);
      } else if (msg.method && !('id' in msg)) {
        this.notifyCb?.(msg.method as string, msg.params);
      }
    }
  }

  async sendRPC<T>(method: string, params: Record<string, unknown> = {}, timeoutMs = 5000): Promise<T> {
    if (!this._connected) throw new Error('Not connected');
    const id = ++this.rpcId;
    const payload = this.encoder.encode(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    await this.writer!.write(payload);

    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`RPC timeout: ${method}`));
      }, timeoutMs);
      this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject, timer });
    });
  }

  onNotification(cb: (method: string, params: unknown) => void): void {
    this.notifyCb = cb;
  }

  async disconnect(): Promise<void> {
    this._connected = false;
    for (const [, { reject, timer }] of this.pending) {
      clearTimeout(timer);
      reject(new Error('Disconnected'));
    }
    this.pending.clear();
    try { await this.reader?.cancel(); this.reader?.releaseLock(); } catch {}
    try { this.writer?.releaseLock(); } catch {}
    try { await this.port?.close(); } catch {}
    this.reader = null; this.writer = null; this.port = null;
  }
}
```

### BLETransport.ts

```typescript
const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_TX      = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // write (app→device)
const NUS_RX      = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // notify (device→app)

export class BLETransport implements Transport {
  // ... (same Pending/notifyCb/rpcId/pending structure as SerialTransport)
  
  async connect(): Promise<void> {
    if (!('bluetooth' in navigator)) throw new Error('Web Bluetooth not supported');
    const device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [NUS_SERVICE] }],
      optionalServices: [NUS_SERVICE],
    });
    device.addEventListener('gattserverdisconnected', () => {
      this._connected = false;
      this.rejectAll('BLE disconnected');
    });
    const server = await device.gatt!.connect();
    const service = await server.getPrimaryService(NUS_SERVICE);
    this.txChar = await service.getCharacteristic(NUS_TX);
    this.rxChar = await service.getCharacteristic(NUS_RX);
    await this.rxChar.startNotifications();
    this.rxChar.addEventListener('characteristicvaluechanged', this.onBLEData.bind(this));
    this._connected = true;
    this.device = device;
  }

  private onBLEData(e: Event): void {
    const target = e.target as BluetoothRemoteGATTCharacteristic;
    this.lineBuf += new TextDecoder().decode(target.value!, { stream: true });
    this.processLines(); // same as SerialTransport.processLines()
  }

  async sendRPC<T>(method: string, params = {}, timeoutMs = 5000): Promise<T> {
    // BLE MTU ≈ 20 bytes on most implementations; chunk writes
    const payload = new TextEncoder().encode(JSON.stringify({ jsonrpc: '2.0', id: ++this.rpcId, method, params }) + '\n');
    const CHUNK = 20;
    for (let i = 0; i < payload.length; i += CHUNK) {
      await this.txChar!.writeValueWithResponse(payload.slice(i, i + CHUNK));
    }
    // ... (same Promise/timeout pattern as SerialTransport)
  }
}
```

### BrambleClient (unified API)

```typescript
// transport/index.ts
import { SerialTransport } from './SerialTransport';
import { BLETransport } from './BLETransport';
import type { Transport, TransportType } from '../types/bramble';

export function createTransport(type: TransportType): Transport {
  return type === 'ble' ? new BLETransport() : new SerialTransport();
}

// Convenience wrapper: handles notifications, auto-reconnect backoff
export class BrambleClient {
  private transport: Transport;
  private notifySubs = new Map<string, Set<(params: unknown) => void>>();

  constructor(transport: Transport) {
    this.transport = transport;
    transport.onNotification((method, params) => {
      this.notifySubs.get(method)?.forEach(fn => fn(params));
    });
  }

  async rpc<T>(method: string, params?: Record<string, unknown>): Promise<T> {
    return this.transport.sendRPC<T>(method, params);
  }

  subscribe(method: string, cb: (params: unknown) => void): () => void {
    if (!this.notifySubs.has(method)) this.notifySubs.set(method, new Set());
    this.notifySubs.get(method)!.add(cb);
    return () => this.notifySubs.get(method)?.delete(cb);
  }
}
```

---

## Zustand Store

```typescript
// store/index.ts
import { create } from 'zustand';
import type { AppState, Message, Neighbor, Route } from '../types/bramble';

export const useStore = create<AppState & Actions>((set, get) => ({
  // Initial state
  connectionState: 'disconnected',
  transport: null,
  config: null,
  status: null,
  airtime: null,
  neighbors: [],
  routes: [],
  messages: [],
  conversations: new Map(),
  activeConversationId: 'broadcast',

  // Actions
  setConnectionState: (s, err?) => set({ connectionState: s, connectionError: err }),
  setConfig: (c) => set({ config: c }),
  setStatus: (s) => set({ status: s }),
  setAirtime: (a) => set({ airtime: a }),
  setNeighbors: (n) => set({ neighbors: n }),
  setRoutes: (r) => set({ routes: r }),

  addMessage: (msg: Message) => set(state => {
    const msgs = [...state.messages, msg].slice(-500); // cap at 500
    const convId = msg.channelIndex !== undefined
      ? `ch:${msg.channelIndex}`
      : `dm:${msg.direction === 'outgoing' ? msg.to : msg.from}`;
    // Update conversation summary
    const convs = new Map(state.conversations);
    const prev = convs.get(convId);
    convs.set(convId, {
      id: convId,
      label: prev?.label ?? formatAddr(convId),
      peerAddr: msg.direction === 'outgoing' ? msg.to : msg.from,
      channelIndex: msg.channelIndex,
      lastMessage: msg.text.slice(0, 60),
      lastMessageTime: msg.timestampMs,
      unreadCount: (prev?.unreadCount ?? 0) + (msg.direction === 'incoming' ? 1 : 0),
    });
    return { messages: msgs, conversations: convs };
  }),

  updateMessageStatus: (id: string, status, relayPath?) => set(state => ({
    messages: state.messages.map(m =>
      m.id === id ? { ...m, status, relayPath: relayPath ?? m.relayPath } : m
    ),
  })),

  setActiveConversation: (id: string) => set(state => {
    const convs = new Map(state.conversations);
    const conv = convs.get(id);
    if (conv) convs.set(id, { ...conv, unreadCount: 0 });
    return { activeConversationId: id, conversations: convs };
  }),
}));
```

---

## Task Breakdown

### Phase 1: Project Scaffold & Transport (Tasks 101–104)
**Goal:** Vite project builds; connect to a real node over serial and receive a response.

---

#### Task 101: Scaffold Vite + React + TypeScript project

**Files to create:**
- `webapp/package.json`
- `webapp/vite.config.ts`
- `webapp/tsconfig.json`
- `webapp/index.html`
- `webapp/src/main.tsx`
- `webapp/src/App.tsx`

**Step 1: Create package.json**

```json
{
  "name": "bramble-webapp",
  "version": "0.1.0",
  "private": true,
  "scripts": {
    "dev": "vite",
    "build": "tsc --noEmit && vite build",
    "preview": "vite preview",
    "test": "vitest run"
  },
  "dependencies": {
    "react": "^19.0.0",
    "react-dom": "^19.0.0",
    "zustand": "^5.0.0"
  },
  "devDependencies": {
    "@types/react": "^19.0.0",
    "@types/react-dom": "^19.0.0",
    "@vitejs/plugin-react": "^4.0.0",
    "typescript": "^5.4.0",
    "vite": "^6.0.0",
    "vitest": "^2.0.0",
    "@testing-library/react": "^16.0.0",
    "@testing-library/jest-dom": "^6.0.0",
    "jsdom": "^24.0.0"
  }
}
```

**Step 2: Run `npm install` in `webapp/`**

```bash
cd /home/justin/.openclaw/workspace/bramble/webapp && npm install
```

**Step 3: Create `vite.config.ts`**

```typescript
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'jsdom',
    setupFiles: ['./test/setup.ts'],
    globals: true,
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
});
```

**Step 4: Create minimal App.tsx + main.tsx + index.html**

Just render `<h1>🌿 Bramble</h1>` for now.

**Step 5: Verify**

Run: `cd webapp && npm run dev`  
Expected: Vite dev server starts, browser shows "🌿 Bramble"

**Step 6: Commit**

```bash
git add webapp/
git commit -m "feat(webapp): scaffold React+TypeScript+Vite project"
```

---

#### Task 102: Create TypeScript types

**File to create:** `webapp/src/types/bramble.ts`

Copy in the complete types from the **Data Model** section above. No logic, just types.

**Step 1:** Create the file with all types.

**Step 2: Verify compilation**

Run: `cd webapp && npx tsc --noEmit`  
Expected: No errors.

**Step 3: Commit**

```bash
git add webapp/src/types/
git commit -m "feat(webapp): add bramble TypeScript data model"
```

---

#### Task 103: Implement SerialTransport

**File to create:** `webapp/src/transport/SerialTransport.ts`

Implement the `SerialTransport` class from the **Transport Layer** section above. No changes from the reference; port it exactly.

**Step 1:** Create the file.

**Step 2: Create a manual smoke-test page (temporary)**

Add a button to `App.tsx` that calls `new SerialTransport().connect()` and logs the result. Test against a real node or the simulator's mock serial endpoint.

**Step 3: Verify TypeScript compiles**

Run: `npx tsc --noEmit`

**Step 4: Commit**

```bash
git add webapp/src/transport/
git commit -m "feat(webapp): implement Web Serial JSON-RPC transport"
```

---

#### Task 104: Implement BLETransport + BrambleClient

**Files to create:**
- `webapp/src/transport/BLETransport.ts`
- `webapp/src/transport/index.ts`

**Step 1:** Implement `BLETransport` from reference code above. Key addition: BLE MTU chunking (20-byte slices via `writeValueWithResponse`).

**Step 2:** Implement `BrambleClient` wrapper + `createTransport` factory in `index.ts`.

**Step 3: Write unit tests**

Create `test/transport/SerialTransport.test.ts`:

```typescript
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { SerialTransport } from '../../src/transport/SerialTransport';

// Mock navigator.serial
const mockPort = {
  open: vi.fn(),
  close: vi.fn(),
  writable: { getWriter: vi.fn() },
  readable: { getReader: vi.fn() },
};

describe('SerialTransport', () => {
  it('throws if Web Serial not supported', async () => {
    Object.defineProperty(navigator, 'serial', { value: undefined, configurable: true });
    const t = new SerialTransport();
    await expect(t.connect()).rejects.toThrow('Web Serial API not supported');
  });

  it('rejects pending RPCs on disconnect', async () => {
    // Setup mock, send RPC, then disconnect before response
    // Verify the promise rejects with 'Disconnected'
    // ... (test implementation)
  });

  it('handles RPC timeout', async () => {
    // Verify timeout rejection after timeoutMs
    // ... (test implementation)
  });

  it('routes notifications correctly', async () => {
    // Verify onNotification callback is called for push events
    // ... (test implementation)
  });
});
```

**Step 4: Verify tests pass**

Run: `cd webapp && npm run test`  
Expected: All tests pass.

**Step 5: Commit**

```bash
git add webapp/src/transport/ webapp/test/
git commit -m "feat(webapp): implement BLE transport + BrambleClient + transport tests"
```

---

### Phase 2: Global App Shell (Tasks 105–106)
**Goal:** Connection UI, Zustand store, tab navigation all working.

---

#### Task 105: Implement Zustand store

**Files to create:**
- `webapp/src/store/index.ts`
- `webapp/src/store/actions.ts`
- `webapp/src/store/selectors.ts`

**Step 1: Create `store/index.ts`** from the store definition in the **Zustand Store** section above.

**Step 2: Create `store/actions.ts`** — async thunks:

```typescript
// store/actions.ts
import { useStore } from './index';
import { createTransport } from '../transport';
import { BrambleClient } from '../transport';
import type { TransportType, BrambleConfig, NodeStatus, AirtimeStatus } from '../types/bramble';

let client: BrambleClient | null = null;

export async function connect(type: TransportType): Promise<void> {
  const store = useStore.getState();
  store.setConnectionState('connecting');
  try {
    const transport = createTransport(type);
    await transport.connect();
    client = new BrambleClient(transport);
    store.setConnectionState('connected');

    // Subscribe to push events
    client.subscribe('bramble.onMessage', (params) => handleIncomingMessage(params));
    client.subscribe('bramble.onAck', (params) => handleAck(params));
    client.subscribe('bramble.onNeighborChange', () => refreshNeighbors());

    // Initial data load
    await Promise.all([loadConfig(), loadStatus(), loadAirtime(), loadNeighbors(), loadRoutes()]);

  } catch (e) {
    store.setConnectionState('error', (e as Error).message);
  }
}

export async function disconnect(): Promise<void> {
  await client?.rpc('bramble.disconnect');
  client = null;
  useStore.getState().setConnectionState('disconnected');
}

export async function loadConfig(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<BrambleConfig>('bramble.getConfig');
  useStore.getState().setConfig(result);
}

export async function loadStatus(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<NodeStatus>('bramble.getStatus');
  useStore.getState().setStatus(result);
}

export async function loadAirtime(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<AirtimeStatus>('bramble.getAirtime');
  useStore.getState().setAirtime(result);
}

export async function loadNeighbors(): Promise<void> { /* ... */ }
export async function loadRoutes(): Promise<void> { /* ... */ }

export async function sendMessage(dest: number, text: string, tier = 'normal', channelIndex?: number): Promise<void> {
  if (!client) return;
  const store = useStore.getState();
  const msg = {
    id: crypto.randomUUID(),
    direction: 'outgoing' as const,
    from: 0,
    to: dest,
    text, tier,
    channelIndex,
    timestampMs: Date.now(),
    status: 'sending' as const,
  };
  store.addMessage(msg);
  try {
    const { packetId } = await client.rpc<{ packetId: number }>('bramble.sendMessage', { dest, text, tier, channelIndex });
    store.updateMessageStatus(msg.id, 'sent');
    // Store packet_id→message_id mapping for ACK correlation
    packetIdToMsgId.set(packetId, msg.id);
  } catch (e) {
    store.updateMessageStatus(msg.id, 'failed');
  }
}

const packetIdToMsgId = new Map<number, string>();

function handleAck(params: unknown) {
  const { packetId, status, relayPath } = params as { packetId: number; status: string; relayPath?: unknown[] };
  const msgId = packetIdToMsgId.get(packetId);
  if (msgId) {
    packetIdToMsgId.delete(packetId);
    useStore.getState().updateMessageStatus(
      msgId,
      status === 'delivered' ? 'delivered' : 'failed',
      relayPath as import('../types/bramble').RelayHop[]
    );
  }
}

function handleIncomingMessage(params: unknown) {
  const p = params as import('../types/bramble').IncomingMessage;
  useStore.getState().addMessage({
    id: p.msgId,
    direction: 'incoming',
    from: p.from, to: p.to, text: p.text, tier: p.tier,
    channelIndex: p.channelIndex,
    timestampMs: Date.now(),
    status: 'delivered',
  });
}

async function refreshNeighbors() { await loadNeighbors(); }
```

**Step 3: Create `store/selectors.ts`**

```typescript
import { useStore } from './index';
import type { Conversation, Message } from '../types/bramble';

export function useConversation(id: string): { conv: Conversation | undefined; messages: Message[] } {
  const conv = useStore(s => s.conversations.get(id));
  const messages = useStore(s => s.messages.filter(m => {
    if (id.startsWith('ch:')) {
      const chIdx = parseInt(id.slice(3));
      return m.channelIndex === chIdx;
    }
    const peerAddr = parseInt(id.slice(3));
    return (m.direction === 'outgoing' && m.to === peerAddr) ||
           (m.direction === 'incoming' && m.from === peerAddr);
  }));
  return { conv, messages };
}
```

**Step 4: Write store unit tests**

`test/store/actions.test.ts` — mock BrambleClient, verify message status transitions.

**Step 5: Commit**

```bash
git add webapp/src/store/ webapp/test/store/
git commit -m "feat(webapp): implement Zustand store with connect/send/notification actions"
```

---

#### Task 106: Build app shell

**Files to modify/create:**
- `webapp/src/App.tsx`
- `webapp/src/components/ConnectionOverlay.tsx`
- `webapp/src/styles/global.css`
- `webapp/src/styles/App.module.css`

**Step 1: Create `global.css`** — CSS custom properties for the design system:

```css
:root {
  --bg: #0d1117;
  --surface: #161b22;
  --surface-2: #21262d;
  --border: #30363d;
  --text: #e6edf3;
  --text-muted: #8b949e;
  --accent: #238636;          /* green — connected, success */
  --accent-blue: #1f6feb;     /* primary action */
  --danger: #da3633;          /* error, failed */
  --warning: #e3b341;         /* warning */
  --critical: #bc8cff;        /* critical tier */
  --font-mono: 'JetBrains Mono', 'Fira Code', 'Consolas', monospace;
  --radius: 8px;
  --radius-sm: 4px;
}

* { box-sizing: border-box; margin: 0; padding: 0; }
body { background: var(--bg); color: var(--text); font-family: system-ui, sans-serif; }
.mono { font-family: var(--font-mono); font-size: 0.85em; }
button { cursor: pointer; }
```

**Step 2: Create `App.tsx`** with:
- Topbar: title, transport selector, connect button, status dot
- Main content area: renders active page
- Tabbar: Chat, Nodes, Config, Stats tabs
- Wires connect button to `connect()` action
- Shows `ConnectionOverlay` when disconnected/connecting

**Step 3: Create `ConnectionOverlay.tsx`**

Full-screen overlay shown when not connected. Shows transport selector, connect button, and error state. Never blocks the stats tab (useful even when disconnected to review cached data).

**Step 4: Verify layout renders correctly in browser**

**Step 5: Commit**

```bash
git add webapp/src/App.tsx webapp/src/components/ webapp/src/styles/
git commit -m "feat(webapp): implement app shell with topbar, tabbar, connection overlay"
```

---

### Phase 3: Chat Page (Tasks 107–109)
**Goal:** Full send/receive with delivery status and relay path display.

---

#### Task 107: Conversation list + message bubbles

**Files to create:** `webapp/src/pages/Chat/`

**Step 1: Create `Chat.tsx`**

Renders: `<ConversationTabs />` + `<MessageList />` + `<Compose />`. Subscribes to active conversation messages from store.

**Step 2: Create `ConversationTabs.tsx`**

Horizontal scrollable tab list. Each tab: label + unread badge. `[+ DM]` button opens a dialog to enter an address.

**Step 3: Create `MessageBubble.tsx`**

```tsx
interface MessageBubbleProps {
  message: Message;
  myAddr: number;
}

export function MessageBubble({ message, myAddr }: MessageBubbleProps) {
  const isOut = message.direction === 'outgoing';
  return (
    <div className={`${styles.bubble} ${isOut ? styles.outgoing : styles.incoming}`}>
      {!isOut && (
        <AddressLabel addr={message.from} className={styles.sender} />
      )}
      <p className={styles.text}>{message.text}</p>
      <div className={styles.meta}>
        <time>{formatTime(message.timestampMs)}</time>
        {isOut && <DeliveryBadge status={message.status} tier={message.tier} />}
      </div>
      {message.relayPath && message.relayPath.length > 0 && (
        <RelayPathDisplay path={message.relayPath} myAddr={myAddr} destAddr={message.to} />
      )}
    </div>
  );
}
```

**Step 4: Create `DeliveryBadge.tsx`**

Shows `●` / `✓` / `✓✓` / `✗` / `!` with appropriate color and tooltip.
Critical tier messages: badge in purple (`--critical`).

**Step 5: Verify messages render**

Use store's `addMessage` to inject test messages via browser console, verify bubbles render.

**Step 6: Commit**

```bash
git add webapp/src/pages/Chat/
git commit -m "feat(webapp): chat page with conversation tabs and message bubbles"
```

---

#### Task 108: Relay path display

**File to create:** `webapp/src/pages/Chat/RelayPathDisplay.tsx`

**Step 1: Create component**

```tsx
interface RelayPathDisplayProps {
  path: RelayHop[];   // ordered: [relay1, relay2, ..., dest]
  myAddr: number;
  destAddr: number;
}

export function RelayPathDisplay({ path, myAddr, destAddr }: RelayPathDisplayProps) {
  const hops = [{ addr: myAddr, rssi: 0 }, ...path];
  return (
    <div className={styles.path}>
      {hops.map((hop, i) => (
        <React.Fragment key={hop.addr}>
          {i > 0 && (
            <span className={styles.arrow} title={`RSSI: ${hop.rssi} dBm`}>
              →
            </span>
          )}
          <AddressLabel addr={hop.addr} className={styles.hop} short />
        </React.Fragment>
      ))}
    </div>
  );
}
```

**Step 2: Wire up via delivery receipts**

In `store/actions.ts`, the `handleAck` function already calls `updateMessageStatus` with `relayPath`. Verify the relay path flows through to `MessageBubble → RelayPathDisplay`.

**Step 3: Test with mock data**

Inject a mock delivered message with a 3-hop relay path into the store. Verify it renders correctly.

**Step 4: Commit**

```bash
git add webapp/src/pages/Chat/RelayPathDisplay.tsx
git commit -m "feat(webapp): relay path display for Critical message delivery receipts"
```

---

#### Task 109: Compose + send + tier selection

**File to create:** `webapp/src/pages/Chat/Compose.tsx`

**Step 1: Create `Compose.tsx`**

```tsx
export function Compose({ conversationId }: { conversationId: string }) {
  const [text, setText] = useState('');
  const [tier, setTier] = useState<MessageTier>('normal');
  const connected = useStore(s => s.connectionState === 'connected');

  const handleSend = async () => {
    const trimmed = text.trim();
    if (!trimmed || !connected) return;
    setText('');
    const dest = conversationIdToAddr(conversationId); // parse dm:0x... or ch:N
    await sendMessage(dest, trimmed, tier);
  };

  return (
    <div className={styles.compose}>
      <input
        value={text}
        onChange={e => setText(e.target.value)}
        onKeyDown={e => e.key === 'Enter' && !e.shiftKey && handleSend()}
        placeholder={connected ? 'Type a message…' : 'Connect to send'}
        disabled={!connected}
      />
      <TierSelector value={tier} onChange={setTier} disabled={!connected} />
      <button onClick={handleSend} disabled={!connected || !text.trim()}>Send</button>
    </div>
  );
}
```

**Step 2: Create `TierSelector`** (small segmented control: Normal | 🔴 Critical)

**Step 3: Wire `loadMessages` on connect**

When the app connects, call `bramble.getMessages` to fetch the node's ring buffer (last N messages). Merge into store.

**Step 4: Test complete send flow**

Against a real node: type a message, hit send, verify:
1. Message appears with `●` sending status
2. Changes to `✓` after RPC returns packet_id
3. Changes to `✓✓` when delivery receipt arrives (if dest is alive)
4. For Critical messages: relay path appears below `✓✓`

**Step 5: Commit**

```bash
git add webapp/src/pages/Chat/Compose.tsx
git commit -m "feat(webapp): message compose with tier selection and send flow"
```

---

### Phase 4: Nodes Page (Tasks 110–111)
**Goal:** Live neighbor table and routing table with auto-refresh.

---

#### Task 110: Neighbor cards

**Files to create:** `webapp/src/pages/Nodes/`

**Step 1: Create `Nodes.tsx`**

Fetches on mount and on 5s interval. Uses `usePoll(loadNeighbors, 5000)` hook.

**Step 2: Create `NeighborCard.tsx`**

```tsx
export function NeighborCard({ neighbor }: { neighbor: Neighbor }) {
  const [expanded, setExpanded] = useState(false);
  const health = neighborHealth(neighbor); // 'good' | 'fair' | 'poor'

  return (
    <article className={`${styles.card} ${styles[health]}`} onClick={() => setExpanded(e => !e)}>
      <div className={styles.header}>
        <AddressLabel addr={neighbor.addr} />
        <span className={styles.rssi} title="Received Signal Strength">
          {neighbor.rssi} dBm
        </span>
      </div>
      <div className={styles.row}>
        <span>PDR: {pdrPercent(neighbor.deliveryRate)}%</span>
        <span>Last: {formatAgo(neighbor.lastHeardMs)}</span>
        {neighbor.isMailbox && <span className={styles.badge}>📬 Mailbox</span>}
      </div>
      {expanded && (
        <div className={styles.detail}>
          <div>SNR: {neighbor.snr} dB</div>
          <div>Airtime remaining: {neighbor.airtimeRemaining}%</div>
          <button onClick={e => { e.stopPropagation(); openDM(neighbor.addr); }}>
            📨 Send DM
          </button>
        </div>
      )}
    </article>
  );
}
```

**Step 3: Health color coding**

- Green border: PDR > 90% AND RSSI > -90
- Yellow border: PDR 70–90% OR RSSI -90 to -110
- Red border: PDR < 70% OR RSSI < -110

**Step 4: Commit**

```bash
git add webapp/src/pages/Nodes/
git commit -m "feat(webapp): neighbor cards with health color-coding and mailbox indicator"
```

---

#### Task 111: Route table

**File to create:** `webapp/src/pages/Nodes/RouteTable.tsx`

**Step 1: Create `RouteTable.tsx`**

Sortable table: Destination | Next Hop | Hops | Metric | State | Age.

State colors: `active` = green, `stale` = yellow, `broken` = red, `discovering` = blue spinner.

**Step 2: Fetch routes on connect and every 10s**

`usePoll(loadRoutes, 10000)`.

**Step 3: Commit**

```bash
git add webapp/src/pages/Nodes/RouteTable.tsx
git commit -m "feat(webapp): routing table with state color-coding and auto-refresh"
```

---

### Phase 5: Config Page (Tasks 112–113)
**Goal:** Full configuration with save + validation.

---

#### Task 112: Identity, radio, and channel config

**Files to create:** `webapp/src/pages/Config/`

**Step 1: Create `Config.tsx`** — collapsible sections, loads `config` from store.

**Step 2: Create `IdentitySection.tsx`**

Shows address (hex), public key (truncated with copy button), name field with inline save. Includes `[Export Key Backup]` button that calls `bramble.exportKey` and prompts download of a `.bramble-backup` file.

**Step 3: Create `RadioForm.tsx`**

```tsx
export function RadioForm({ radio, onSave }: { radio: RadioConfig; onSave: (r: RadioConfig) => void }) {
  const [form, setForm] = useState(radio);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  const handleSave = async () => {
    setSaving(true);
    try {
      await rpc('bramble.setRadio', form);
      onSave(form);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSaving(false);
    }
  };

  return (
    <form onSubmit={e => { e.preventDefault(); handleSave(); }}>
      <label>TX Power (dBm)
        <input type="number" min={2} max={20} value={form.txPowerDbm}
               onChange={e => setForm(f => ({ ...f, txPowerDbm: +e.target.value }))} />
      </label>
      <label>Spreading Factor
        <select value={form.sf} onChange={e => setForm(f => ({ ...f, sf: +e.target.value as 7|8|9|10|11|12 }))}>
          {[7,8,9,10,11,12].map(n => <option key={n}>{n}</option>)}
        </select>
      </label>
      {/* ... bandwidth, coding rate, frequency */}
      {error && <p className={styles.error}>{error}</p>}
      <button type="submit" disabled={saving}>{saving ? 'Saving…' : 'Save Radio'}</button>
    </form>
  );
}
```

**Step 4: Create `ChannelManager.tsx`**

List of channels (name, PSK indicator, default badge, delete button).
"Add channel" row: name input + PSK input (optional) + Add button.
Calls `bramble.addChannel` / `bramble.removeChannel` / `bramble.setDefaultChannel`.

**Step 5: Commit**

```bash
git add webapp/src/pages/Config/
git commit -m "feat(webapp): config page with identity, radio, and channel management"
```

---

#### Task 113: Peer manager

**File to create:** `webapp/src/pages/Config/PeerManager.tsx`

**Step 1: Build peer list from neighbors + routing table**

Union of `neighbors[]` and `routes[].dest`. Show address, any known name, last heard time.

**Step 2: Name assignment (client-side only)**

Store `Map<address, name>` in `localStorage`. The node has no name storage for remote peers — names are UI-only.

**Step 3: Commit**

```bash
git add webapp/src/pages/Config/PeerManager.tsx
git commit -m "feat(webapp): peer manager with client-side name assignment"
```

---

### Phase 6: Stats Page & Polish (Tasks 114–115)
**Goal:** Working stats dashboard, responsive layout, PWA, CI build.

---

#### Task 114: Stats page

**Files to create:** `webapp/src/pages/Stats/`

**Step 1: Create `Stats.tsx`** — loads on mount and refreshes every 5s.

**Step 2: Create `StatCards.tsx`**

Grid of 6 cards: TX count, RX count, Neighbors, Routes, Uptime, Free heap.

```tsx
function StatCard({ value, label }: { value: string; label: string }) {
  return (
    <div className={styles.card}>
      <div className={styles.value}>{value}</div>
      <div className={styles.label}>{label}</div>
    </div>
  );
}
```

**Step 3: Create `AirtimeBars.tsx`**

```tsx
export function AirtimeBars({ airtime }: { airtime: AirtimeStatus }) {
  return (
    <section className={styles.section}>
      <h2>Airtime Budget</h2>
      {airtime.tiers.map(tier => (
        <div key={tier.name} className={styles.tier}>
          <span className={styles.name}>{capitalize(tier.name)}</span>
          <div className={styles.track}>
            <div
              className={`${styles.fill} ${styles[tier.name]}`}
              style={{ width: `${100 - tier.usedPct}%` }}
            />
          </div>
          <span className={styles.pct}>{(100 - tier.usedPct).toFixed(1)}%</span>
          <span className={styles.refill}>
            refills {formatRelativeTime(tier.refillAtMs)}
          </span>
        </div>
      ))}
    </section>
  );
}
```

**Step 4: Auto-refresh on tab focus**

When the Stats tab is activated, immediately refresh status + airtime.

**Step 5: Commit**

```bash
git add webapp/src/pages/Stats/
git commit -m "feat(webapp): stats page with airtime bars and packet counters"
```

---

#### Task 115: Polish, PWA, Docker, CI

**Files to create/modify:**
- `webapp/public/manifest.json`
- `webapp/public/favicon.svg`
- `webapp/docker/Dockerfile`
- `webapp/docker/nginx.conf`
- `webapp/.github/workflows/build.yml` (or add to root CI)

**Step 1: PWA manifest**

```json
{
  "name": "Bramble",
  "short_name": "Bramble",
  "description": "Bramble LoRa mesh companion app",
  "start_url": "/",
  "display": "standalone",
  "background_color": "#0d1117",
  "theme_color": "#238636",
  "icons": [
    { "src": "/favicon.svg", "sizes": "any", "type": "image/svg+xml" }
  ]
}
```

**Step 2: Responsive breakpoints**

At ≥640px:
- Topbar spans full width
- Left sidebar replaces bottom tabbar (nav items vertical)
- Content area grows to fill remaining width

Test at 375px (iPhone SE) and 1280px desktop.

**Step 3: Favicon** — Create a simple SVG bramble leaf in green.

**Step 4: Docker build**

```dockerfile
# webapp/docker/Dockerfile
FROM node:22-alpine AS build
WORKDIR /app
COPY package*.json .
RUN npm ci
COPY . .
RUN npm run build

FROM nginx:alpine
COPY --from=build /app/dist /usr/share/nginx/html
COPY docker/nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 80
```

```nginx
# webapp/docker/nginx.conf
server {
  listen 80;
  root /usr/share/nginx/html;
  index index.html;
  location / { try_files $uri /index.html; }
  gzip on;
  gzip_types text/plain application/javascript text/css application/json;
}
```

**Step 5: Verify full build**

Run: `cd webapp && npm run build`  
Expected: `dist/` created with no TypeScript errors, all assets < 500KB total.

**Step 6: Accessibility pass**

- All interactive elements reachable by keyboard
- ARIA labels on icon-only buttons
- Sufficient color contrast (WCAG AA)

**Step 7: Final end-to-end test**

Connect to a real node via Web Serial. Walk through every page:
- Chat: send a DM, verify delivery status progression to `✓✓`
- Chat: send Critical, verify relay path appears
- Nodes: verify neighbors + routes populate and refresh
- Config: change TX power, save, verify node accepts
- Config: add a channel, verify it appears in chat conversation tabs
- Stats: verify all counters display, airtime bars update

**Step 8: Commit**

```bash
git add webapp/public/ webapp/docker/
git commit -m "feat(webapp): PWA manifest, responsive layout, Docker build, accessibility"
```

---

## Testing Strategy

### Unit Tests (Vitest)

| Module | What to test |
|--------|-------------|
| `SerialTransport` | Timeout, disconnect rejection, notification routing, line buffering across chunks |
| `BLETransport` | 20-byte chunking math, disconnect event handling |
| `BrambleClient` | Multi-subscriber fan-out, unsubscribe cleanup |
| `store/actions` | Message status transitions: queued→sending→sent→delivered/failed |
| `store/selectors` | `useConversation` returns correct messages for DM vs. channel |
| `DeliveryBadge` | Renders correct icon for each status + tier |
| `RelayPathDisplay` | Renders N hops with correct separators |
| `AirtimeBars` | Bar width percentage math, refill time formatting |

### Integration Tests (Playwright or manual)

| Scenario | Steps | Expected |
|----------|-------|----------|
| Connect via Serial | Click Connect, select port, wait | Status dot goes green; config loads |
| Send Normal DM | Type text, click Send | `●` → `✓` → `✓✓` |
| Send Critical DM | Toggle Critical, send | `✓✓` + relay path visible |
| Failed delivery | Send to unreachable addr | `✓` → `✗` after retries |
| Receive message | Node sends push notification | New bubble appears in correct conversation |
| Add channel | Config page, enter name+PSK | Channel appears in chat tabs |
| Radio save | Change SF, click Save | No error; node resets to new SF |
| BLE connect | Mobile Chrome, click Connect | BLE picker, connects, full parity with serial |
| Disconnect + reconnect | Disconnect, reconnect | State preserved in store, new data loaded |
| PWA install | Chrome "Install" prompt | Works standalone, topbar title visible |

### Browser Compatibility

| Browser | Serial | BLE | Notes |
|---------|--------|-----|-------|
| Chrome 120+ | ✅ | ✅ | Primary target |
| Edge 120+ | ✅ | ✅ | Same Chromium engine |
| Firefox | ❌ | ❌ | Web Serial/BT not supported; show clear error message |
| Safari iOS | ❌ | ❌ | No API support; show clear error |
| Chrome Android | ✅ | ✅ | BLE particularly useful on mobile |

Show a prominent unsupported-browser notice when neither API is available.

### Performance Budget

| Metric | Target |
|--------|--------|
| Initial JS bundle | < 200 KB gzipped |
| First contentful paint | < 1s on WiFi |
| Time to interactive | < 2s |
| Message render (500 msgs) | < 50ms (no jank) |
| Store update → re-render | < 16ms (1 frame) |

---

## Docker Development Setup

For local development without a real node, a mock serial server can simulate the firmware's JSON-RPC responses.

```yaml
# webapp/docker-compose.yml
services:
  webapp:
    build: .
    ports:
      - "3001:80"
    volumes:
      - ./dist:/usr/share/nginx/html:ro

  mock-node:
    image: node:22-alpine
    working_dir: /app
    volumes:
      - ./mock:/app
    command: node server.js
    ports:
      - "3002:3002"
    # Creates a virtual serial port pair for testing
```

The mock server (`webapp/mock/server.js`) responds to every JSON-RPC method with realistic-looking data so UI development doesn't require physical hardware.

```javascript
// webapp/mock/server.js
// Simple mock: read from stdin, write to stdout (pipe to socat for serial emulation)
// socat PTY,raw,echo=0 EXEC:'node mock/server.js'
process.stdin.on('data', chunk => {
  const lines = chunk.toString().split('\n').filter(l => l.trim());
  for (const line of lines) {
    try {
      const req = JSON.parse(line);
      const result = handleRPC(req.method, req.params ?? {});
      process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id: req.id, result }) + '\n');
    } catch {}
  }
});

const MOCK_DATA = {
  'bramble.getStatus': () => ({
    uptimeSec: 3600 + Math.floor(Math.random() * 100),
    freeHeapBytes: 120000,
    fwVersion: '0.5.0-dev',
    txCount: 1247, rxCount: 3891, droppedCount: 12,
    neighborCount: 3, routeCount: 8, airtimeUsedMs: 18000,
  }),
  'bramble.getConfig': () => ({
    identity: { address: 0x1B3C4D5E, pubkeyHash: 0xDEADBEEF, name: 'MockNode', pubkeyB64: 'AAAA' },
    radio: { txPowerDbm: 20, sf: 10, bwKhz: 125, cr: 5, freqMhz: 915.0 },
    channels: [{ index: 0, name: 'bramble-default', hasPsk: false, epoch: 0, isDefault: true }],
  }),
  'bramble.getAirtime': () => ({
    tiers: [
      { name: 'critical',  remainingMs: 29520, maxMs: 36000, usedPct: 18, refillAtMs: Date.now() + 2580000 },
      { name: 'normal',    remainingMs: 12240, maxMs: 18000, usedPct: 32, refillAtMs: Date.now() + 2580000 },
      { name: 'broadcast', remainingMs: 17820, maxMs: 18000, usedPct: 1,  refillAtMs: Date.now() + 2580000 },
    ]
  }),
  'bramble.getNeighbors': () => ({ neighbors: [
    { addr: 0x1A3F2B4C, rssi: -78, snr: 6, deliveryRate: 250, lastHeardMs: 12000, isMailbox: true, airtimeRemaining: 72 },
    { addr: 0x3B7C9E1A, rssi: -104, snr: -2, deliveryRate: 181, lastHeardMs: 47000, isMailbox: false, airtimeRemaining: 44 },
  ]}),
  'bramble.getRoutes': () => ({ routes: [
    { dest: 0x5E1A9F2C, nextHop: 0x1A3F2B4C, hopCount: 2, metric: 187, state: 'active', lastUsedMs: 5000 },
    { dest: 0x9F2C3B7C, nextHop: 0x3B7C9E1A, hopCount: 3, metric: 142, state: 'stale', lastUsedMs: 120000 },
  ]}),
  'bramble.getMessages': () => ({ messages: [] }),
  'bramble.sendMessage': (p) => ({ packetId: Math.floor(Math.random() * 0xFFFFFFFF) }),
  'bramble.setRadio': () => ({}),
  'bramble.setNodeName': () => ({}),
  'bramble.addChannel': () => ({ index: 1 }),
  'bramble.removeChannel': () => ({}),
  'bramble.setDefaultChannel': () => ({}),
};

function handleRPC(method, params) {
  const handler = MOCK_DATA[method];
  if (!handler) throw { code: -32601, message: 'Method not found' };
  return handler(params);
}

// Emit a mock incoming message after 5 seconds
setTimeout(() => {
  const notif = {
    jsonrpc: '2.0',
    method: 'bramble.onMessage',
    params: {
      from: 0x1A3F2B4C, to: 0, text: 'Hello from mock node!',
      tier: 'normal', timestamp: Math.floor(Date.now() / 1000),
      msgId: crypto.randomUUID(),
    }
  };
  process.stdout.write(JSON.stringify(notif) + '\n');
}, 5000);
```

---

## Summary Table

| Task | Description | Deliverable |
|------|-------------|-------------|
| 101 | Scaffold Vite project | `webapp/` builds, renders title |
| 102 | TypeScript types | `types/bramble.ts` complete |
| 103 | SerialTransport | Web Serial JSON-RPC works |
| 104 | BLETransport + BrambleClient | BLE transport + unified client |
| 105 | Zustand store + actions | State management, send/receive flow |
| 106 | App shell | Topbar, tabs, connection overlay |
| 107 | Chat: bubbles + conversations | Messages render with history |
| 108 | Chat: relay path display | Critical message paths visible |
| 109 | Chat: compose + tier select | Full send/receive loop |
| 110 | Nodes: neighbor cards | Neighbor health display |
| 111 | Nodes: route table | Route table with states |
| 112 | Config: identity + radio + channels | Full config read/write |
| 113 | Config: peer manager | Client-side name assignment |
| 114 | Stats: counters + airtime bars | Live stats with refresh |
| 115 | Polish + PWA + Docker + CI | Production-ready build |

**Estimated total: 2–3 weeks for one agent working sequentially.**  
Phases 1–2 (scaffold + shell) can be parallelized with Phase 6 polish work.

---

## Open Questions

1. **SPIFFS deployment:** Should the build target a specific max bundle size to fit in ESP32 SPIFFS? Typical 2MB SPIFFS partition → ~1.8MB available for the app. Current estimate: ~300KB gzipped. Should fit comfortably.

2. **Message encryption display:** The app receives decrypted message text from the node (the node decrypts before sending to the app over serial/BLE). Should the app show a lock icon to indicate E2E encryption was used? Needs a flag in the message response.

3. **Channel message encryption in compose:** When composing to a channel with PSK, the PSK is on the node — the app doesn't need to handle crypto. But the UX should make clear the message will be encrypted.

4. **Key backup format:** The `exportKey` output is currently opaque base64. Should it be a documented format so third-party tools can restore? Possibly: JSON envelope with version, encrypted key bytes, and BLAKE2s checksum.

5. **Auto-reconnect:** Should the transport layer attempt to reconnect automatically if the serial connection drops? Web Serial doesn't support it gracefully (user must re-grant). Could catch the disconnect and prompt user to reconnect.
