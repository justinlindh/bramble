/**
 * Bramble Mock Node: WebSocket JSON-RPC 2.0 handler module
 * Importable module for embedding in other servers.
 *
 * Simulates a realistic 5-node mesh in a fictional example town.
 * The "HomeBase" node anchors the mesh; peers are spread nearby.
 *
 * Implements the same JSON-RPC wire protocol as the real firmware:
 *   Request:      { jsonrpc: "2.0", id: N, method: "bramble.X", params: {...} }
 *   Response:     { jsonrpc: "2.0", id: N, result: {...} }
 *   Notification: { jsonrpc: "2.0", method: "bramble.X", params: {...} }
 */

import { sha256 } from '@noble/hashes/sha2.js';
import { bytesToHex, hexToBytes } from '@noble/hashes/utils.js';

// ─── Trust-anchor mock state ─────────────────────────────────────────────────
// This node's Ed25519 identity public key (64 hex). Fixed so getIdentity is
// stable across a session. The anchor flow signs a cert OVER this key.
const MOCK_ED25519_PUB = '8f500a6dbab3786da3eb56d5146157fa26577a361f2e3b52907f2acdc344fefa';
let mockAnchorPub = null; // 64-hex anchor public key, or null when unanchored
let mockEndorsed = false; // whether a well-formed cert has been applied

// ─── Network-key mock state ──────────────────────────────────────────────────
// The control-plane network key, 64 hex, or null while UNPROVISIONED. The mock
// boots unprovisioned exactly like real firmware, so the UNPROVISIONED banner
// and the found/join flow in Config -> Network Key are reachable without
// hardware. The key is write-only at the RPC boundary here too: nothing reads
// it back, only the one-way fingerprint.
let mockNetworkKey = null;
// The all-zero sentinel the firmware reports while unprovisioned.
const UNPROVISIONED_FINGERPRINT = '00000000';

// ─── Timezone mock state ─────────────────────────────────────────────────────
// The stored POSIX TZ spec, or null when nothing is stored and the compiled-in
// default applies. The preset list mirrors the firmware's k_presets table in
// components/timezone/bramble_tz.c: a short curated list, not a tzdata port.
let mockTimezone = null;
const TZ_DEFAULT = 'UTC0';
const TZ_PRESETS = [
  { label: 'UTC', spec: 'UTC0' },
  { label: 'US Pacific', spec: 'PST8PDT,M3.2.0,M11.1.0' },
  { label: 'US Mountain', spec: 'MST7MDT,M3.2.0,M11.1.0' },
  { label: 'US Arizona', spec: 'MST7' },
  { label: 'US Central', spec: 'CST6CDT,M3.2.0,M11.1.0' },
  { label: 'US Eastern', spec: 'EST5EDT,M3.2.0,M11.1.0' },
  { label: 'US Alaska', spec: 'AKST9AKDT,M3.2.0,M11.1.0' },
  { label: 'US Hawaii', spec: 'HST10' },
  { label: 'UK', spec: 'GMT0BST,M3.5.0/1,M10.5.0/2' },
  { label: 'Central Europe', spec: 'CET-1CEST,M3.5.0,M10.5.0/3' },
  { label: 'Eastern Europe', spec: 'EET-2EEST,M3.5.0/3,M10.5.0/4' },
  { label: 'India', spec: 'IST-5:30' },
  { label: 'China', spec: 'CST-8' },
  { label: 'Japan', spec: 'JST-9' },
  { label: 'Australia Eastern', spec: 'AEST-10AEDT,M10.1.0,M4.1.0/3' },
  { label: 'New Zealand', spec: 'NZST-12NZDT,M9.5.0,M4.1.0/3' },
];
// NOTE: this mock tracks anchor/endorsement STATE and validates cert shape,
// but does not cryptographically verify the endorsement signature (the real
// firmware and the webapp's own anchor.ts tests cover the crypto). It exists
// so the webapp's device-gated anchor UI flow works end to end without hardware.
// @noble/hashes is pure JS (no node:crypto), so this works unmodified whether
// handler.mjs runs under node (mock/server.mjs, server/unified-server.mjs) or
// gets bundled straight into the webapp for the in-page MockTransport.
// SHA256(key)[0:4] as 8 lowercase hex. The firmware derives the anchor and
// network-key fingerprints the same way, so one helper serves both.
const fingerprint8 = (keyHex) =>
  bytesToHex(sha256(hexToBytes(keyHex))).slice(0, 8);
const isHex = (s, len) => typeof s === 'string' && s.length === len && /^[0-9a-fA-F]+$/.test(s);

// ─── Node identities ─────────────────────────────────────────────────────────
// Our node, the mesh home base (fictional example coordinates)
const SELF_ADDR  = 0x1A2B3C4D;
const SELF_NAME  = 'HomeBase';
const SELF_POS   = { lat: 40.0000, lon: -105.0000, alt: 500, accuracy: 3 };

// Peer nodes, fictional example locations
const PEERS = {
  0xAABBCC01: {
    name: 'Ridge',      // node on a ridge with good line of sight
    pos: { lat: 40.0062, lon: -105.0053, alt: 620, accuracy: 5 },
    locTier: 'full',
  },
  0xAABBCC02: {
    name: 'Trailhead',  // node at a trailhead parking area
    pos: { lat: 39.9817, lon: -105.0173, alt: 480, accuracy: 12 },
    locTier: 'full',
  },
  0xAABBCC03: {
    name: 'Downtown',   // node in the town center
    pos: null,
    gridSquare: 'JJ00aa',  // coarse town-center grid square
    locTier: 'coarse',
  },
  0xAABBCC04: {
    name: 'Northside',  // node in a neighborhood ~8km north
    pos: { lat: 40.0537, lon: -105.0223, alt: 540, accuracy: 8 },
    locTier: 'full',
  },
  0xAABBCC05: {
    name: 'Ranger',     // Mobile node, presence only
    pos: null,
    locTier: 'presence',
  },
};

const PEER_ADDRS = Object.keys(PEERS).map(Number);

// ─── Attested roll-call mock state ───────────────────────────────────────────
// Constants mirror components/rollcall (ROLLCALL_MAX_ROUNDS, the 30s-doubled
// round schedule plus the 45s collection tail, ROLLCALL_MIN_INTERVAL_MS,
// ROLLCALL_TEXT_MAX and ROLLCALL_ANSWER_MAX_PER_HOUR), so the panel's
// countdown and caps match what a real node would report.
const ROLLCALL_ROUNDS_TOTAL = 3;
const ROLLCALL_WINDOW_MS = 135_000;
const ROLLCALL_MIN_INTERVAL_MS = 300_000;
const ROLLCALL_TEXT_MAX = 48;
const ROLLCALL_ANSWER_MAX_PER_HOUR = 12;
// Who answers, when, and over which relay path. Fixed rather than random: the
// docs/rollcall.md screenshot is captured from this mock and has to be
// reproducible. Ranger (0xAABBCC05), the mobile presence-only node, is absent
// on purpose so the ledger always has one member to report missing.
const ROLLCALL_ANSWERS = [
  { addr: 0xAABBCC01, atMs: 1_240, round: 1, path: [SELF_ADDR, 0xAABBCC01] },
  { addr: 0xAABBCC02, atMs: 2_610, round: 1 },
  { addr: 0xAABBCC04, atMs: 4_180, round: 1, path: [SELF_ADDR, 0xAABBCC01, 0xAABBCC04] },
  { addr: 0xAABBCC03, atMs: 6_320, round: 1 },
];
let rollcall = null;
let rollcallCounter = 0;

// The expected set is only meaningful when it is anchor-certified, exactly as
// on a real node: un-anchored, the ledger reports observed responders only and
// names nobody missing.
const rollcallExpected = () => (mockAnchorPub ? PEER_ADDRS : []);

const BOOT_TIME = Date.now();

// ─── Mutable state ──────────────────────────────────────────────────────────

let txCount = 47;
let rxCount = 312;
let droppedCount = 3;
let airtimeUsedMs = 18720;
let packetIdCounter = 1000;
let msgIdCounter = 1;

// Auth (issue #96): when MOCK_AUTH_TOKEN is set, the mock enforces the firmware
// auth model from PR #83: unauthenticated connections may call only the
// ping/getVersion allowlist, a wrong ?token= closes the WS with 1008, and
// notifications are withheld from unauthenticated connections. Default off so
// existing dev flows keep working; export MOCK_AUTH_TOKEN=secret to enable.
// `process` does not exist when this module is bundled into the webapp for
// the in-page MockTransport, so guard the lookup with typeof (a plain
// `process.env` reference would throw ReferenceError in that environment).
let authToken = (typeof process !== 'undefined' && process.env?.MOCK_AUTH_TOKEN) || '';
const AUTH_ALLOWLIST = new Set(['bramble.ping', 'bramble.getVersion']);
const UNAUTHORIZED = { code: -1005, message: 'Unauthorized' };
function authRequired() { return authToken.length > 0; }

// Allowed-origins and OTA origin allowlist (issue #96, mirrors PR #83 / #85).
let allowedOrigins = [];

/* BLE pairing posture (bramble.getBleSecurity / bramble.setBlePasskey). The
 * demo node presents itself as a board with no display, so the passkey
 * controls render; MOCK_BLE_MODE=passkey-display demos a board that shows a
 * random code on its own screen and therefore refuses a static passkey. */
let blePasskey = '';
const bleDisplayBoard =
  (typeof process !== 'undefined' && process.env?.MOCK_BLE_MODE) === 'passkey-display';
// The mock's OTA origin must be fetchable by fetchOtaIndex, which does a real
// fetch() against it. 'https://mock.local/ota/' resolves nowhere, so the
// index.json GET always failed and the version picker never rendered in the
// no-hardware journey. Use the page origin instead (same-origin, so vite
// serves webapp/public/ota/index.json there in dev and dist builds alike);
// the localhost fallback only matters for non-browser test environments
// where `location` is undefined.
const OTA_DEFAULT_ORIGIN = (typeof location !== 'undefined' && location.origin
  ? location.origin
  : 'http://localhost:5173') + '/ota/';
let otaOrigin = OTA_DEFAULT_ORIGIN;
let otaOverridden = false;

// OTA journey simulation (Bramble webapp OTA UX plan, Task 9): drives the
// Firmware Update card end to end without hardware. Both the browser dev
// server (mock/server.mjs -> handleConnection) and the in-page MockTransport
// used by embedded shells share this module, so simulating the event stream
// here (rather than in MockTransport.ts, which has no dispatch of its own
// and just forwards to handleConnection) covers both entry points.
const OTA_MOCK_TOTAL_BYTES = 1048576; // 1 MiB, arbitrary mock artifact size
let otaVersionFloor = '0.4.0';
let otaRunningVersion = '0.4.0';
let otaSnapshot = { state: 'idle', bytes: 0, total: 0, percent: 0 };

function otaEmit(partial) {
  otaSnapshot = { ...otaSnapshot, error: undefined, ...partial };
  notify('bramble.onOtaEvent', { ...otaSnapshot });
}

// Plays a flat list of onOtaEvent snapshots, one per 10ms tick, in order.
// Stays setTimeout-based (rather than a single interval or promise chain of
// arbitrary shape) so the existing fake-timer tests, which call
// vi.advanceTimersByTimeAsync to flush the whole sequence, keep working.
async function playOta(steps) {
  for (const s of steps) {
    await new Promise((resolve) => setTimeout(resolve, 10));
    otaEmit(s);
  }
}

const OTA_FAIL_STEPS = [
  { state: 'downloading', bytes: 0, total: OTA_MOCK_TOTAL_BYTES, percent: 0 },
  {
    state: 'downloading',
    bytes: Math.round(OTA_MOCK_TOTAL_BYTES * 0.4),
    total: OTA_MOCK_TOTAL_BYTES,
    percent: 40,
  },
  { state: 'failed', percent: 40, error: 'mock: simulated failure' },
];

const OTA_SUCCESS_STEPS = [
  ...[0, 25, 50, 75, 100].map((percent) => ({
    state: 'downloading',
    bytes: Math.round(OTA_MOCK_TOTAL_BYTES * percent / 100),
    total: OTA_MOCK_TOTAL_BYTES,
    percent,
  })),
  { state: 'verifying' },
  { state: 'rebooting' },
];

// Traffic debug (issue #96, BUG-6): persisted toggle plus synthesized events.
const trafficDebug = {
  enabled: false, include_tx: true, include_rx: true, sample_rate: 100,
  ring_size: 512, ring_used: 0, dropped_count: 0, last_seq: 0,
};
const trafficEvents = [];
let trafficSeq = 0;

const hex8 = (n) => `0x${(n >>> 0).toString(16).toUpperCase().padStart(8, '0')}`;

// Normalize a dest param to a number. The web client sends the firmware wire
// format (an 8-char hex string, e.g. "AABBCC03"); older callers may send a
// number. Without this, route lookups always miss and every DM takes the
// 95%-success path, leaving the stale-route failure case (Example) unreachable
// (issue #96, BUG-5).
function normalizeDest(dest) {
  if (typeof dest === 'string') {
    const n = parseInt(dest, 16);
    return Number.isNaN(n) ? 0xFFFFFFFF : n >>> 0;
  }
  return (dest ?? 0xFFFFFFFF) >>> 0;
}

// Neighbors: direct radio contacts (not all peers are direct neighbors)
const neighbors = [
  {
    addr: 0xAABBCC01, name: 'Example', // strong direct link (1.8km LOS)
    rssi: -68, snr: 9.5, deliveryRate: 245,
    lastHeardMs: 1200, isMailbox: true, airtimeRemaining: 88,
  },
  {
    addr: 0xAABBCC02, name: 'Example', // moderate link (3.5km, some obstruction)
    rssi: -87, snr: 4.8, deliveryRate: 195,
    lastHeardMs: 8400, isMailbox: false, airtimeRemaining: 72,
  },
  {
    addr: 0xAABBCC03, name: 'Example', // weak link (5km through buildings)
    rssi: -102, snr: 2.1, deliveryRate: 130,
    lastHeardMs: 34000, isMailbox: false, airtimeRemaining: 45,
  },
  {
    addr: 0xAABBCC05, name: 'Ranger', // mobile, marginal link
    rssi: -109, snr: 0.8, deliveryRate: 85,
    lastHeardMs: 62000, isMailbox: false, airtimeRemaining: 31,
  },
];

// Routes: includes both direct and multi-hop
const routes = [
  { dest: 0xAABBCC01, nextHop: 0xAABBCC01, hopCount: 1, metric: 68,  state: 'active',      lastUsedMs: 1500  },
  { dest: 0xAABBCC02, nextHop: 0xAABBCC02, hopCount: 1, metric: 112, state: 'active',      lastUsedMs: 9000  },
  { dest: 0xAABBCC03, nextHop: 0xAABBCC03, hopCount: 1, metric: 185, state: 'stale',       lastUsedMs: 45000 },
  { dest: 0xAABBCC04, nextHop: 0xAABBCC01, hopCount: 2, metric: 142, state: 'active',      lastUsedMs: 5200  },  // via Example
  { dest: 0xAABBCC05, nextHop: 0xAABBCC05, hopCount: 1, metric: 210, state: 'active',      lastUsedMs: 62000 },
  { dest: 0xBEEF0001, nextHop: 0xAABBCC01, hopCount: 3, metric: 240, state: 'discovering', lastUsedMs: 0     },  // unknown far node
];

const channels = [
  { index: 0, name: 'Bramble Common', hasPsk: false, epoch: 1, isDefault: true  },
  { index: 1, name: 'Mesh Net',  hasPsk: true,  epoch: 5, isDefault: false },
  { index: 2, name: 'Family',         hasPsk: true,  epoch: 2, isDefault: false },
];

const locationContacts = [
  { addr: 0xAABBCC01, tier: 'full',   intervalSec: 300, distanceTriggerM: 100 },
  { addr: 0xAABBCC02, tier: 'full',   intervalSec: 600, distanceTriggerM: 200 },
  { addr: 0xAABBCC03, tier: 'coarse', intervalSec: 900, distanceTriggerM: 500 },
  { addr: 0xAABBCC04, tier: 'full',   intervalSec: 300, distanceTriggerM: 100 },
];

const locationConfig = {
  enabled: true,
  contacts: locationContacts,
  defaultIntervalSec: 300,
  defaultDistanceTriggerM: 100,
  stationaryBackoff: 4,
};

// Build peer location array from PEERS data
function buildPeerLocations() {
  return PEER_ADDRS.map(addr => {
    const p = PEERS[addr];
    const isExact = p.locTier === 'full' && p.pos;
    const isCoarse = p.locTier === 'coarse';
    const isOnline = addr !== 0xAABBCC05 || Math.random() > 0.3; // Ranger sometimes offline
    return {
      addr,
      name: p.name,
      tier: p.locTier,
      position: isExact ? {
        lat: p.pos.lat + (Math.random() - 0.5) * 0.0003,
        lon: p.pos.lon + (Math.random() - 0.5) * 0.0003,
        alt: p.pos.alt,
        accuracy: p.pos.accuracy,
        speed: addr === 0xAABBCC05 ? 3 + Math.random() * 8 : Math.random() * 2,
        heading: Math.floor(Math.random() * 360),
        timestampMs: Date.now() - Math.floor(Math.random() * 60000),
      } : null,
      gridSquare: isCoarse ? p.gridSquare : undefined,
      online: isOnline,
      lastUpdatedMs: Date.now() - Math.floor(Math.random() * 120000),
    };
  });
}

const config = {
  identity: {
    address: SELF_ADDR,
    pubkeyHash: 0x3A7F2B8C,
    name: SELF_NAME,
    pubkeyB64: 'bW9jay1wdWJrZXktaG9tZWJhc2UtZXhhbXBsZQ==',
  },
  radio: {
    txPowerDbm: 17,
    sf: 9,
    bwKhz: 125,
    cr: 5,
    freqMhz: 915.0,
  },
  channels,
  mailboxEnabled: false,
  location: locationConfig,
};

// ─── Active clients ─────────────────────────────────────────────────────────

const clients = new Set();

function broadcast(obj) {
  const msg = JSON.stringify(obj);
  for (const ws of clients) {
    // Notifications are withheld from unauthenticated connections (issue #96).
    if (ws.readyState === 1 && ws._authed) ws.send(msg);
  }
}

function notify(method, params) {
  broadcast({ jsonrpc: '2.0', method, params });
}

// ─── RPC handlers ────────────────────────────────────────────────────────────

export const handlers = {
  'bramble.getIdentity'(_params) {
    return {
      address: hex8(SELF_ADDR >>> 0).slice(2),
      pubkey_hash: '1118D963',
      ed25519_pub: MOCK_ED25519_PUB,
    };
  },

  'bramble.getTimezone'(_params) {
    return {
      ok: true,
      timezone: mockTimezone ?? TZ_DEFAULT,
      default_timezone: TZ_DEFAULT,
      configured: mockTimezone !== null,
      presets: TZ_PRESETS,
    };
  },

  'bramble.setTimezone'(params) {
    const tz = params?.timezone;
    if (typeof tz !== 'string' || !tz || tz.length > 63) {
      throw { code: -32602, message: 'timezone must be a POSIX TZ spec of 1 to 63 chars' };
    }
    mockTimezone = tz;
    return { ok: true };
  },

  'bramble.setNetworkKey'(params) {
    const key = params?.key;
    if (!isHex(key, 64)) {
      throw { code: -32602, message: 'key must be 64 hex chars' };
    }
    mockNetworkKey = key.toLowerCase();
    return { ok: true };
  },

  'bramble.getNetworkKeyStatus'(_params) {
    if (!mockNetworkKey) {
      return { provisioned: false, fingerprint: UNPROVISIONED_FINGERPRINT };
    }
    return { provisioned: true, fingerprint: fingerprint8(mockNetworkKey) };
  },

  // Mints a key on the "device" and provisions it atomically, matching the
  // firmware's network_key_generate_provision: the key is returned exactly
  // once and never read back afterwards.
  'bramble.generateNetworkKey'(_params) {
    const key = new Uint8Array(32);
    globalThis.crypto.getRandomValues(key);
    mockNetworkKey = bytesToHex(key);
    return { key: mockNetworkKey, fingerprint: fingerprint8(mockNetworkKey) };
  },

  'bramble.setAnchor'(params) {
    const pub = params?.anchor_pubkey;
    if (!isHex(pub, 64)) {
      throw { code: -32602, message: 'anchor_pubkey must be 64 hex chars' };
    }
    if (pub.toLowerCase() !== mockAnchorPub) {
      mockAnchorPub = pub.toLowerCase();
      mockEndorsed = false; // anchor change drops the old endorsement
    }
    return { ok: true };
  },

  'bramble.getAnchorStatus'(_params) {
    if (!mockAnchorPub) {
      return { anchored: false, endorsed: false };
    }
    return {
      anchored: true,
      anchor_fingerprint: fingerprint8(mockAnchorPub),
      endorsed: mockEndorsed,
    };
  },

  'bramble.setEndorsement'(params) {
    if (!mockAnchorPub) {
      throw { code: -32602, message: 'no anchor provisioned' };
    }
    const na = params?.not_after;
    const sig = params?.endorsement_sig;
    if (!isHex(na, 16) || na === '0000000000000000') {
      throw { code: -32602, message: 'not_after must be 16 hex chars and non-zero' };
    }
    if (!isHex(sig, 128)) {
      throw { code: -32602, message: 'endorsement_sig must be 128 hex chars' };
    }
    mockEndorsed = true;
    return { ok: true };
  },

  'bramble.getStatus'(_params) {
    const uptimeSec = Math.floor((Date.now() - BOOT_TIME) / 1000);
    return {
      uptimeSec,
      freeHeapBytes: 187432 - Math.floor(uptimeSec * 0.5) % 20000,
      fwVersion: '0.4.2-dev',
      txCount,
      rxCount,
      droppedCount,
      neighborCount: neighbors.length,
      routeCount: routes.filter(r => r.state === 'active').length,
      airtimeUsedMs,
      position: {
        lat: SELF_POS.lat + (Math.random() - 0.5) * 0.00005,
        lon: SELF_POS.lon + (Math.random() - 0.5) * 0.00005,
        alt: SELF_POS.alt,
        accuracy: SELF_POS.accuracy,
        speed: 0,
        heading: 0,
        timestampMs: Date.now() - 2000,
      },
    };
  },

  'bramble.getConfig'(_params) {
    return {
      identity: { ...config.identity },
      radio: { ...config.radio },
      channels: channels.map(c => ({ ...c })),
      mailboxEnabled: config.mailboxEnabled,
      location: { ...locationConfig, contacts: locationContacts.map(c => ({ ...c })) },
    };
  },

  'bramble.getAirtime'(_params) {
    const now = Date.now();
    return {
      tiers: [
        { name: 'critical',  remainingMs: 9200,  maxMs: 10000, usedPct: 8,  refillAtMs: now + 55000  },
        { name: 'normal',    remainingMs: 41000, maxMs: 60000, usedPct: 32, refillAtMs: now + 120000 },
        { name: 'broadcast', remainingMs: 22500, maxMs: 30000, usedPct: 25, refillAtMs: now + 300000 },
        // Receipt lane (PR #82 four-lane budget); issue #96 so the web client's
        // receipt lane is live-testable against the mock.
        { name: 'receipt',   remainingMs: 10800, maxMs: 12000, usedPct: 10, refillAtMs: now + 90000  },
      ],
    };
  },

  'bramble.getNeighbors'(_params) {
    return {
      neighbors: neighbors.map(n => ({
        ...n,
        lastHeardMs: n.lastHeardMs + Math.floor(Math.random() * 500),
      })),
    };
  },

  'bramble.getRoutes'(_params) {
    return { routes: routes.map(r => ({ ...r })) };
  },

  'bramble.getMessages'(_params) {
    return { messages: [] };
  },

  'bramble.sendMessage'(params) {
    const packetId = ++packetIdCounter;
    txCount++;
    airtimeUsedMs += 350 + Math.floor(Math.random() * 200);

    const dest = normalizeDest(params?.dest);
    const delayMs = 800 + Math.floor(Math.random() * 2500);

    // Simulate delivery
    setTimeout(() => {
      // Build realistic relay path based on routes
      let relayPath = [];
      if (dest !== 0xFFFFFFFF) {
        const route = routes.find(r => r.dest === dest);
        if (route && route.hopCount > 1) {
          // Multi-hop: show intermediate relayers
          relayPath.push({ addr: route.nextHop, rssi: -70 - Math.floor(Math.random() * 20) });
          if (route.hopCount > 2) {
            relayPath.push({ addr: dest, rssi: -80 - Math.floor(Math.random() * 15) });
          }
        }
      }

      // 95% delivery for active routes, 40% for stale
      const route = routes.find(r => r.dest === dest);
      const success = !route || route.state === 'active'
        ? Math.random() < 0.95
        : route.state === 'stale' ? Math.random() < 0.4 : false;

      notify('bramble.onAck', {
        packetId,
        status: success ? 'delivered' : 'failed',
        relayPath: success ? relayPath : [],
      });
    }, delayMs);

    return { packetId };
  },

  'bramble.sendBroadcast'(params) {
    const packetId = ++packetIdCounter;
    const broadcastId = `bcast-${Date.now().toString(36)}-${packetId}`;
    txCount++;
    airtimeUsedMs += 450 + Math.floor(Math.random() * 300);

    console.log(`[mock-node] Broadcast: "${params?.text?.slice(0, 40)}..." (packetId=${packetId})`);

    // Simulate broadcast delivery notifications from each reachable peer
    // Broadcasts reach neighbors directly: simulate realistic delivery timing
    const reachablePeers = neighbors.filter(n => n.rssi > -115); // Only peers with reasonable signal

    for (let i = 0; i < reachablePeers.length; i++) {
      const peer = reachablePeers[i];
      // Closer peers (better RSSI) respond faster
      const rssiOffset = Math.abs(peer.rssi + 70); // -70 is excellent, -110 is marginal
      const baseDelay = 500 + rssiOffset * 30;
      const jitter = Math.floor(Math.random() * 1500);
      const delayMs = baseDelay + jitter + i * 800;

      // Delivery probability based on link quality
      const deliveryChance = peer.rssi > -90 ? 0.95 : peer.rssi > -105 ? 0.7 : 0.4;
      const delivered = Math.random() < deliveryChance;

      setTimeout(() => {
        notify('bramble.onBroadcastDelivery', {
          broadcastId,
          packetId,
          from: peer.addr,
          status: delivered ? 'delivered' : 'failed',
          hopCount: 1,
          deliveredAtMs: Date.now(),
        });
      }, delayMs);
    }

    // Also simulate 2-hop delivery to Example (via Example) with longer delay
    const anthemAddr = 0xAABBCC04;
    setTimeout(() => {
      notify('bramble.onBroadcastDelivery', {
        broadcastId,
        packetId,
        from: anthemAddr,
        status: Math.random() < 0.85 ? 'delivered' : 'failed',
        hopCount: 2,
        deliveredAtMs: Date.now(),
      });
    }, 3000 + Math.floor(Math.random() * 2000));

    return { packetId, broadcastId };
  },

  'bramble.setRadio'(params) {
    if (params) Object.assign(config.radio, params);
    return { ok: true };
  },

  'bramble.setNodeName'(params) {
    if (params?.name) config.identity.name = String(params.name).slice(0, 8);
    return { ok: true };
  },

  'bramble.addChannel'(params) {
    const index = channels.length;
    channels.push({
      index,
      name: params?.name ?? `ch${index}`,
      hasPsk: !!(params?.psk),
      epoch: 1,
      isDefault: false,
    });
    return { index };
  },

  'bramble.removeChannel'(params) {
    const idx = params?.index;
    const pos = channels.findIndex(c => c.index === idx);
    if (pos !== -1) channels.splice(pos, 1);
    return { ok: true };
  },

  'bramble.setMailbox'(params) {
    config.mailboxEnabled = !!(params?.enabled);
    return { ok: true };
  },

  'bramble.setDefaultChannel'(params) {
    const idx = params?.index;
    for (const ch of channels) ch.isDefault = ch.index === idx;
    return { ok: true };
  },

  // ─── Attested roll-call ────────────────────────────────────────────────
  //
  // The ledger is DERIVED from elapsed time rather than accumulated in a
  // timer, so a page reload mid-roll-call reads the same state the mock would
  // have reported all along. The schedule below is fixed rather than random:
  // the doc screenshot in docs/rollcall.md is captured from this mock, and a
  // random ledger would make that image unreproducible.
  //
  // Ranger is the mesh's mobile presence-only node and never answers, so the
  // ledger always has exactly one member to report missing: the whole point of
  // the primitive is showing who did NOT answer.

  'bramble.startRollCall'(params) {
    const text = typeof params?.text === 'string' ? params.text : '';
    if (text.length > ROLLCALL_TEXT_MAX) {
      throw { code: -32602, message: `text over ${ROLLCALL_TEXT_MAX} bytes` };
    }
    const now = Date.now();
    if (rollcall) {
      const since = now - rollcall.startedAt;
      if (since < ROLLCALL_WINDOW_MS) {
        return {
          ok: false,
          reason: 'busy',
          retry_after_ms: Math.max(ROLLCALL_WINDOW_MS - since, ROLLCALL_MIN_INTERVAL_MS - since),
          min_interval_ms: ROLLCALL_MIN_INTERVAL_MS,
        };
      }
      if (since < ROLLCALL_MIN_INTERVAL_MS) {
        return {
          ok: false,
          reason: 'rate_limited',
          retry_after_ms: ROLLCALL_MIN_INTERVAL_MS - since,
          min_interval_ms: ROLLCALL_MIN_INTERVAL_MS,
        };
      }
    }

    rollcallCounter++;
    rollcall = {
      id: (0x00C0FFE0 + rollcallCounter) >>> 0,
      startedAt: now,
      text,
    };

    // The same events real firmware raises, so a client that subscribes sees
    // the roll-call fill in rather than only discovering it by polling.
    for (const a of ROLLCALL_ANSWERS) {
      setTimeout(() => {
        if (!rollcall || rollcall.startedAt !== now) return;
        notify('bramble.onRollCallResponse', {
          rollcall_id: hex8(rollcall.id).slice(2),
          address: hex8(a.addr).slice(2),
          round: a.round,
          responded: ROLLCALL_ANSWERS.filter((x) => x.atMs <= a.atMs).length,
          expected: rollcallExpected().length,
        });
      }, a.atMs);
    }
    setTimeout(() => {
      if (!rollcall || rollcall.startedAt !== now) return;
      notify('bramble.onRollCallComplete', {
        rollcall_id: hex8(rollcall.id).slice(2),
        responded: ROLLCALL_ANSWERS.length,
        expected: rollcallExpected().length,
        anchored: mockAnchorPub !== null,
        rounds: ROLLCALL_ROUNDS_TOTAL,
        unattested: 0,
      });
    }, ROLLCALL_WINDOW_MS);

    return {
      ok: true,
      rollcall_id: hex8(rollcall.id).slice(2),
      window_ms: ROLLCALL_WINDOW_MS,
      rounds_total: ROLLCALL_ROUNDS_TOTAL,
      expected: rollcallExpected().length,
      anchored: mockAnchorPub !== null,
    };
  },

  'bramble.getRollCall'(_params) {
    const base = {
      rounds_total: ROLLCALL_ROUNDS_TOTAL,
      window_ms: ROLLCALL_WINDOW_MS,
      min_interval_ms: ROLLCALL_MIN_INTERVAL_MS,
      max_text_bytes: ROLLCALL_TEXT_MAX,
      pending_dropped: 0,
      answer_limited: 0,
      answer_max_per_hour: ROLLCALL_ANSWER_MAX_PER_HOUR,
    };
    if (!rollcall) return { ...base, active: false };

    const elapsed = Date.now() - rollcall.startedAt;
    const expected = rollcallExpected();
    const answered = ROLLCALL_ANSWERS.filter((a) => a.atMs <= elapsed);
    const answeredAddrs = new Set(answered.map((a) => a.addr));
    // Rounds go out at 0s, 30s and 90s, the firmware schedule (30s doubled
    // per round already sent).
    const roundsSent = elapsed >= 90_000 ? 3 : elapsed >= 30_000 ? 2 : 1;
    const missing = expected.filter((addr) => !answeredAddrs.has(addr));

    return {
      ...base,
      active: true,
      rollcall_id: hex8(rollcall.id).slice(2),
      open: elapsed < ROLLCALL_WINDOW_MS,
      text: rollcall.text,
      rounds_sent: roundsSent,
      elapsed_ms: elapsed,
      anchored: expected.length > 0,
      expected: expected.length,
      responded: answered.length,
      unattested: 0,
      overflow: 0,
      late: 0,
      missing_count: expected.length > 0 ? missing.length : 0,
      missing: expected.length > 0 ? missing.map((a) => hex8(a).slice(2)) : [],
      responders: answered.map((a) => ({
        address: hex8(a.addr).slice(2),
        responded: true,
        at_ms: a.atMs,
        round: a.round,
        ...(a.path ? { hops: a.path.length, path: a.path.map((h) => hex8(h).slice(2)) } : {}),
      })),
    };
  },

  'bramble.sendProbe'(_params) {
    const probeId = 0xa000 + Math.floor(Math.random() * 0xfff);
    const ackWindow = 30;

    // Simulate realistic probe responses from known peers
    const responders = [
      { addr: 0xAABBCC01, hopCount: 1, baseRssi: -68,  baseSNR: 9.5  },
      { addr: 0xAABBCC02, hopCount: 1, baseRssi: -87,  baseSNR: 4.8  },
      { addr: 0xAABBCC04, hopCount: 2, baseRssi: -92,  baseSNR: 3.2, relay: [0xAABBCC01] },
      { addr: 0xAABBCC03, hopCount: 1, baseRssi: -102, baseSNR: 2.1  },
      { addr: 0xAABBCC05, hopCount: 1, baseRssi: -109, baseSNR: 0.8  },
    ];

    for (let i = 0; i < responders.length; i++) {
      const r = responders[i];
      // Closer/stronger nodes respond faster
      const delay = 1500 + i * 2500 + Math.floor(Math.random() * 3000);
      // Ranger (CC05) only responds 60% of the time
      if (r.addr === 0xAABBCC05 && Math.random() > 0.6) continue;

      setTimeout(() => {
        notify('bramble.onProbeResult', {
          responderAddr: r.addr,
          hopCount: r.hopCount,
          rssi: r.baseRssi + Math.floor((Math.random() - 0.5) * 8),
          snr: Math.round((r.baseSNR + (Math.random() - 0.5) * 1.5) * 10) / 10,
          pathLen: r.hopCount,
          relayPath: r.relay ?? [],
          receivedAt: Date.now(),
        });
      }, delay);
    }

    setTimeout(() => notify('bramble.onProbeComplete', { probeId }), ackWindow * 1000);
    return { probeId, ackWindow };
  },

  'bramble.getPeerLocations'(_params) {
    return { peerLocations: buildPeerLocations() };
  },

  'bramble.setLocationConfig'(params) {
    if (params?.enabled !== undefined) locationConfig.enabled = params.enabled;
    if (params?.defaultIntervalSec !== undefined) locationConfig.defaultIntervalSec = params.defaultIntervalSec;
    if (params?.defaultDistanceTriggerM !== undefined) locationConfig.defaultDistanceTriggerM = params.defaultDistanceTriggerM;
    if (params?.stationaryBackoff !== undefined) locationConfig.stationaryBackoff = params.stationaryBackoff;
    return { ok: true };
  },

  'bramble.setLocationContact'(params) {
    const { addr, tier, intervalSec, distanceTriggerM } = params ?? {};
    const existing = locationContacts.find(c => c.addr === addr);
    if (existing) {
      if (tier !== undefined) existing.tier = tier;
      if (intervalSec !== undefined) existing.intervalSec = intervalSec;
      if (distanceTriggerM !== undefined) existing.distanceTriggerM = distanceTriggerM;
    } else {
      locationContacts.push({
        addr,
        tier: tier ?? 'full',
        intervalSec: intervalSec ?? locationConfig.defaultIntervalSec,
        distanceTriggerM: distanceTriggerM ?? locationConfig.defaultDistanceTriggerM,
      });
    }
    return { ok: true };
  },

  'bramble.removeLocationContact'(params) {
    const idx = locationContacts.findIndex(c => c.addr === params?.addr);
    if (idx !== -1) locationContacts.splice(idx, 1);
    return { ok: true };
  },

  'bramble.getTrafficDebug'(_params) {
    return { ...trafficDebug, ring_used: trafficEvents.length, last_seq: trafficSeq };
  },

  'bramble.setTrafficDebug'(params) {
    // Persist the toggle and options so the Config switch sticks and the Stats
    // Traffic Monitor can render live events (issue #96, BUG-6).
    if (params?.enabled !== undefined) trafficDebug.enabled = !!params.enabled;
    if (params?.include_tx !== undefined) trafficDebug.include_tx = !!params.include_tx;
    if (params?.include_rx !== undefined) trafficDebug.include_rx = !!params.include_rx;
    if (params?.sample_rate !== undefined) trafficDebug.sample_rate = params.sample_rate;
    if (!trafficDebug.enabled) {
      trafficEvents.length = 0;
    }
    return { ...trafficDebug, ring_used: trafficEvents.length, last_seq: trafficSeq };
  },

  'bramble.getTrafficEvents'(params) {
    const since = params?.since_seq ?? 0;
    return { events: trafficEvents.filter(e => e.seq > since) };
  },

  // Auth and management RPCs (issue #96, mirrors PR #83 and PR #85).

  'bramble.ping'(_params) {
    return { pong: true, address: hex8(SELF_ADDR), protocol_version: '0.5.0' };
  },

  'bramble.getVersion'(_params) {
    return {
      firmware_version: '0.4.2-dev',
      protocol_version: '0.5.0',
      hardware: 'heltec-v3',
      supports_delivery_event_sync: true,
    };
  },

  'bramble.setAuthToken'(params, ctx) {
    // Rotate the required token. The connection that sets it stays authenticated
    // (it just proved possession); new connections must present the new token.
    authToken = String(params?.token ?? '');
    if (ctx?.ws) ctx.ws._authed = true;
    return { ok: true };
  },

  'bramble.getAuthToken'(_params) {
    return { token: authToken, enabled: authRequired() };
  },

  'bramble.getBleSecurity'(_params) {
    return {
      mode: bleDisplayBoard ? 'passkey-display' : blePasskey ? 'static-passkey' : 'just-works',
      staticPasskeySet: !bleDisplayBoard && blePasskey !== '',
    };
  },

  'bramble.setBlePasskey'(params) {
    // Same answers the firmware gives (main/rpc_methods.c), so the demo
    // exercises the real client paths including the rejections.
    if (bleDisplayBoard) {
      return { ok: false, error: 'board shows a random pairing code; static passkey unsupported' };
    }
    const pk = params?.passkey;
    if (pk === undefined) return { ok: false, error: 'missing passkey parameter' };
    if (pk === null || pk === '') {
      blePasskey = '';
      return { ok: true, mode: 'just-works' };
    }
    if (typeof pk !== 'string' || !/^[0-9]{6}$/.test(pk)) {
      return { ok: false, error: 'passkey must be exactly 6 digits' };
    }
    blePasskey = pk;
    return { ok: true, mode: 'static-passkey' };
  },

  'bramble.setAllowedOrigins'(params) {
    allowedOrigins = Array.isArray(params?.origins) ? params.origins.slice() : [];
    return { ok: true, origins: allowedOrigins.slice() };
  },

  'bramble.getAllowedOrigins'(_params) {
    return { origins: allowedOrigins.slice() };
  },

  'bramble.otaGetOrigin'(_params) {
    return {
      ok: true,
      origin: otaOrigin,
      default_origin: OTA_DEFAULT_ORIGIN,
      overridden: otaOverridden,
      version_floor: otaVersionFloor,
      running_version: otaRunningVersion,
    };
  },

  'bramble.otaSetOrigin'(params) {
    if (params?.reset) {
      otaOrigin = OTA_DEFAULT_ORIGIN;
      otaOverridden = false;
    } else if (typeof params?.origin === 'string' && params.origin) {
      if (!/^https:\/\//.test(params.origin)) {
        return { ok: false, origin: otaOrigin, overridden: otaOverridden, error: 'origin must be https' };
      }
      otaOrigin = params.origin;
      otaOverridden = true;
    }
    return { ok: true, origin: otaOrigin, overridden: otaOverridden };
  },

  'bramble.otaUpdate'(params) {
    const path = String(params?.path ?? '');
    if (!path || /^[a-z]+:\/\//i.test(path) || path.includes('..')) {
      return { ok: false, error: 'invalid artifact path' };
    }

    if (path.includes('fail')) {
      // Simulated failure: two downloading ticks then a failed state.
      void playOta(OTA_FAIL_STEPS);
    } else {
      // Deliberately no disconnect/reconnect simulation here: the
      // rebooting -> reconnected transition is covered by the otaFlow unit
      // tests against a real transport disconnect.
      void playOta(OTA_SUCCESS_STEPS).then(() => {
        otaRunningVersion = '0.5.0';
      });
    }

    return {
      ok: true,
      note: 'OTA update started (mock); the node would reboot on success',
      url: otaOrigin.replace(/\/?$/, '/') + path,
      partition: 'ota_1',
    };
  },

  'bramble.otaStatus'(_params) {
    const { error, ...rest } = otaSnapshot;
    return {
      ...rest,
      last_error: error,
      running_version: otaRunningVersion,
      version_floor: otaVersionFloor,
    };
  },

  'bramble.shareLocationOnce'(params) {
    // Match firmware: address is a required hex string; addr was never
    // accepted by real nodes and the mock's leniency masked the bug.
    if (typeof params?.address !== 'string') {
      return { error: { code: -32602, message: 'Invalid params' } };
    }
    setTimeout(() => {
      notify('location.update', {
        addr: params?.address,
        name: SELF_NAME,
        tier: 'full',
        position: {
          lat: SELF_POS.lat + (Math.random() - 0.5) * 0.0002,
          lon: SELF_POS.lon + (Math.random() - 0.5) * 0.0002,
          alt: SELF_POS.alt, accuracy: SELF_POS.accuracy,
          speed: 0, heading: 0, timestampMs: Date.now(),
        },
        online: true,
        lastUpdatedMs: Date.now(),
      });
    }, 500);
    return { ok: true };
  },
};

// ─── Periodic simulation ─────────────────────────────────────────────────────

// Counters tick realistically
setInterval(() => {
  if (Math.random() < 0.3) rxCount++;
  if (Math.random() < 0.02) droppedCount++;
  airtimeUsedMs += Math.floor(Math.random() * 40);
}, 1000);

// Synthesize traffic events while traffic debug is enabled (issue #96, BUG-6)
// so the Stats Traffic Monitor renders live data instead of an empty state.
const TRAFFIC_KINDS = [
  { pkt_type: 0x01, category: 'beacon', airtime_tier: 'broadcast' },
  { pkt_type: 0x07, category: 'chat', airtime_tier: 'normal' },
  { pkt_type: 0x02, category: 'ack', airtime_tier: 'critical' },
  { pkt_type: 0x03, category: 'routing', airtime_tier: 'normal' },
];
setInterval(() => {
  if (!trafficDebug.enabled || clients.size === 0) return;
  const k = TRAFFIC_KINDS[Math.floor(Math.random() * TRAFFIC_KINDS.length)];
  const is_tx = Math.random() < 0.5;
  if ((is_tx && !trafficDebug.include_tx) || (!is_tx && !trafficDebug.include_rx)) return;
  const event = {
    seq: ++trafficSeq,
    timestamp_ms: Date.now() - BOOT_TIME,
    pkt_type: k.pkt_type,
    category: k.category,
    airtime_tier: k.airtime_tier,
    packet_len: 16 + Math.floor(Math.random() * 200),
    rssi: is_tx ? 0 : -60 - Math.floor(Math.random() * 50),
    is_tx,
  };
  trafficEvents.push(event);
  if (trafficEvents.length > trafficDebug.ring_size) trafficEvents.shift();
  trafficDebug.last_seq = trafficSeq;
  notify('bramble.onTrafficEvent', event);
}, 1500);

// Neighbor RSSI drift every 15s (realistic fading)
setInterval(() => {
  for (const n of neighbors) {
    n.rssi += Math.floor((Math.random() - 0.5) * 4);
    n.rssi = Math.max(-120, Math.min(-55, n.rssi));
    n.snr += (Math.random() - 0.5) * 0.8;
    n.snr = Math.max(-2, Math.min(14, n.snr));
    // Delivery rate drifts slowly
    n.deliveryRate += Math.floor((Math.random() - 0.5) * 8);
    n.deliveryRate = Math.max(50, Math.min(255, n.deliveryRate));
    n.lastHeardMs = 500 + Math.floor(Math.random() * 3000);
  }
  if (clients.size > 0) notify('bramble.onNeighborChange', {});
}, 15000);

// Simulate incoming messages from mesh: realistic traffic
const MESH_CHATTER = [
  { from: 0xAABBCC01, texts: ['Example node checking in. Strong signal today.', 'Wind picking up on the ridge, antenna holding.', 'Relayed 3 packets this hour.'] },
  { from: 0xAABBCC02, texts: ['Example here. Hikers passing through.', 'Solar panel at 14.2V, all good.', 'Forwarded a message to Example via Example.'] },
  { from: 0xAABBCC03, texts: ['Example reporting. Downtown is noisy on 915.', 'Switching to SF10 for better range.', 'Anyone seeing interference today?'] },
  { from: 0xAABBCC04, texts: ['Example node online. 2 hops to HomeBase confirmed.', 'Battery swap complete, back on air.', 'Can someone relay to Example? Lost direct path.'] },
  { from: 0xAABBCC05, texts: ['Ranger mobile, heading south on 95.', 'Signal fading, might lose you.', 'Back in range. RSSI improved.'] },
];

function scheduleIncoming() {
  const delayMs = 12000 + Math.floor(Math.random() * 25000);
  setTimeout(() => {
    if (clients.size > 0) {
      const source = MESH_CHATTER[Math.floor(Math.random() * MESH_CHATTER.length)];
      const text = source.texts[Math.floor(Math.random() * source.texts.length)];
      const tier = Math.random() < 0.15 ? 'critical' : 'normal';
      const msgId = `mock-${++msgIdCounter}-${Date.now()}`;
      const name = PEERS[source.from]?.name ?? '???';

      console.log(`[mock-node] → incoming from ${name} (0x${source.from.toString(16).toUpperCase()})`);
      notify('bramble.onMessage', {
        from: source.from,
        fromName: name !== '???' ? name : undefined,
        to: SELF_ADDR,
        text,
        tier,
        timestamp: Math.floor(Date.now() / 1000),
        msgId,
      });
      rxCount++;
    }
    scheduleIncoming();
  }, delayMs);
}

scheduleIncoming();

// Periodic location updates from mobile node (Ranger)
setInterval(() => {
  if (clients.size === 0) return;
  // Ranger moves slowly
  const ranger = PEERS[0xAABBCC05];
  if (!ranger._lat) { ranger._lat = 40.02; ranger._lon = -105.04; }
  ranger._lat += (Math.random() - 0.5) * 0.002;
  ranger._lon += (Math.random() - 0.5) * 0.002;
  notify('location.update', {
    addr: 0xAABBCC05,
    name: 'Ranger',
    tier: 'presence',  // Ranger only shares presence
    position: null,
    online: Math.random() > 0.2,
    lastUpdatedMs: Date.now(),
  });
}, 45000);

// ─── Export ───────────────────────────────────────────────────────────────────

/**
 * Handle a new WebSocket connection using the mock node simulation.
 * Can be called directly from a WebSocketServer 'connection' event or
 * from a noServer handleUpgrade callback.
 *
 * @param {import('ws').WebSocket} ws
 * @param {import('http').IncomingMessage} req
 */
export function handleConnection(ws, req) {
  const ip = req?.socket?.remoteAddress ?? 'unknown';

  // Auth handshake (issue #96): the web client sends the token as a ?token=
  // query parameter on the WS URL. A wrong token closes 1008 (the client maps
  // that to "Authentication required"); a missing token leaves the connection
  // unauthenticated (ping/getVersion only, no notifications); a correct token
  // or no configured token authenticates fully.
  if (authRequired()) {
    let provided;
    try {
      provided = new URL(req?.url ?? '', 'ws://localhost').searchParams.get('token');
    } catch { provided = null; }
    if (provided && provided !== authToken) {
      console.log(`[mock-node] Rejecting ${ip}: wrong token (1008)`);
      ws.close(1008, 'unauthorized');
      return;
    }
    ws._authed = provided === authToken;
  } else {
    ws._authed = true;
  }

  console.log(`[mock-node] Client connected from ${ip} (authed=${ws._authed})`);
  clients.add(ws);

  ws.on('message', (data) => {
    let msg;
    try { msg = JSON.parse(data.toString()); } catch { return; }

    const { id, method, params } = msg;
    console.log(`[mock-node] RPC → ${method} (id=${id})`);

    const handler = handlers[method];
    if (!handler) {
      if (id !== undefined) {
        ws.send(JSON.stringify({
          jsonrpc: '2.0', id,
          error: { code: -32601, message: `Method not found: ${method}` },
        }));
      }
      return;
    }

    // Enforce the unauthenticated allowlist (issue #96).
    if (!ws._authed && !AUTH_ALLOWLIST.has(method)) {
      if (id !== undefined) {
        ws.send(JSON.stringify({ jsonrpc: '2.0', id, error: { ...UNAUTHORIZED } }));
      }
      return;
    }

    try {
      const result = handler(params ?? {}, { ws });
      if (id !== undefined) ws.send(JSON.stringify({ jsonrpc: '2.0', id, result }));
    } catch (err) {
      console.error(`[mock-node] Handler error for ${method}:`, err);
      if (id !== undefined) {
        ws.send(JSON.stringify({
          jsonrpc: '2.0', id,
          error: { code: -32603, message: String(err.message ?? err) },
        }));
      }
    }
  });

  ws.on('close', () => { clients.delete(ws); });
  ws.on('error', (err) => console.error(`[mock-node] WS error:`, err.message));
}
