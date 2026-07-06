/**
 * Bramble Mock Node — WebSocket JSON-RPC 2.0 handler module
 * Importable module for embedding in other servers.
 *
 * Simulates a realistic 5-node mesh in the Example/Example area of NV.
 * "Our" node (HomeBase) sits in Example. Peers are spread across Example.
 *
 * Implements the same JSON-RPC wire protocol as the real firmware:
 *   Request:      { jsonrpc: "2.0", id: N, method: "bramble.X", params: {...} }
 *   Response:     { jsonrpc: "2.0", id: N, result: {...} }
 *   Notification: { jsonrpc: "2.0", method: "bramble.X", params: {...} }
 */

import { createHash } from 'node:crypto';

// ─── Trust-anchor mock state ─────────────────────────────────────────────────
// This node's Ed25519 identity public key (64 hex). Fixed so getIdentity is
// stable across a session. The anchor flow signs a cert OVER this key.
const MOCK_ED25519_PUB = '8f500a6dbab3786da3eb56d5146157fa26577a361f2e3b52907f2acdc344fefa';
let mockAnchorPub = null; // 64-hex anchor public key, or null when unanchored
let mockEndorsed = false; // whether a well-formed cert has been applied
// NOTE: this mock tracks anchor/endorsement STATE and validates cert shape,
// but does not cryptographically verify the endorsement signature (the real
// firmware and the webapp's own anchor.ts tests cover the crypto). It exists
// so the webapp's device-gated anchor UI flow works end to end without hardware.
const anchorFingerprint = (pubHex) =>
  createHash('sha256').update(Buffer.from(pubHex, 'hex')).digest('hex').slice(0, 8);
const isHex = (s, len) => typeof s === 'string' && s.length === len && /^[0-9a-fA-F]+$/.test(s);

// ─── Node identities ─────────────────────────────────────────────────────────
// Our node — Justin's house in Example, Example NV
const SELF_ADDR  = 0x1A2B3C4D;  // "JUST"
const SELF_NAME  = 'HomeBase';
const SELF_POS   = { lat: 40.0000, lon: -105.0000, alt: 789, accuracy: 3 };

// Peer nodes — realistic Example locations
const PEERS = {
  0xAABBCC01: {
    name: 'Ridge',    // Heltec on Example Hills ridge, great LOS
    pos: { lat: 40.0062, lon: -105.0053, alt: 856, accuracy: 5 },
    locTier: 'full',
  },
  0xAABBCC02: {
    name: 'Trailhead',  // Node at Example Canyon trailhead parking
    pos: { lat: 39.9817, lon: -105.0173, alt: 732, accuracy: 12 },
    locTier: 'full',
  },
  0xAABBCC03: {
    name: 'Downtown',    // Downtown Example, Water Street District
    pos: null,
    gridSquare: 'JJ00aa',  // coarse — downtown Example area (~36.04, -105.04)
    locTier: 'coarse',
  },
  0xAABBCC04: {
    name: 'Northside',     // Northside neighborhood, ~8km north
    pos: { lat: 40.0537, lon: -105.0223, alt: 615, accuracy: 8 },
    locTier: 'full',
  },
  0xAABBCC05: {
    name: 'Ranger',     // Mobile node, presence only
    pos: null,
    locTier: 'presence',
  },
};

const PEER_ADDRS = Object.keys(PEERS).map(Number);

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
let authToken = process.env.MOCK_AUTH_TOKEN || '';
const AUTH_ALLOWLIST = new Set(['bramble.ping', 'bramble.getVersion']);
const UNAUTHORIZED = { code: -1005, message: 'Unauthorized' };
function authRequired() { return authToken.length > 0; }

// Allowed-origins and OTA origin allowlist (issue #96, mirrors PR #83 / #85).
let allowedOrigins = [];
const OTA_DEFAULT_ORIGIN = 'https://bramblemesh.org/ota/';
let otaOrigin = OTA_DEFAULT_ORIGIN;
let otaOverridden = false;

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
// 95%-success path, leaving the stale-route failure case (Downtown) unreachable
// (issue #96, BUG-5).
function normalizeDest(dest) {
  if (typeof dest === 'string') {
    const n = parseInt(dest, 16);
    return Number.isNaN(n) ? 0xFFFFFFFF : n >>> 0;
  }
  return (dest ?? 0xFFFFFFFF) >>> 0;
}

// Neighbors — direct radio contacts (not all peers are direct neighbors)
const neighbors = [
  {
    addr: 0xAABBCC01, name: 'Ridge', // strong direct link (1.8km LOS)
    rssi: -68, snr: 9.5, deliveryRate: 245,
    lastHeardMs: 1200, isMailbox: true, airtimeRemaining: 88,
  },
  {
    addr: 0xAABBCC02, name: 'Trailhead', // moderate link (3.5km, some obstruction)
    rssi: -87, snr: 4.8, deliveryRate: 195,
    lastHeardMs: 8400, isMailbox: false, airtimeRemaining: 72,
  },
  {
    addr: 0xAABBCC03, name: 'Downtown', // weak link (5km through buildings)
    rssi: -102, snr: 2.1, deliveryRate: 130,
    lastHeardMs: 34000, isMailbox: false, airtimeRemaining: 45,
  },
  {
    addr: 0xAABBCC05, name: 'Ranger', // mobile, marginal link
    rssi: -109, snr: 0.8, deliveryRate: 85,
    lastHeardMs: 62000, isMailbox: false, airtimeRemaining: 31,
  },
];

// Routes — includes both direct and multi-hop
const routes = [
  { dest: 0xAABBCC01, nextHop: 0xAABBCC01, hopCount: 1, metric: 68,  state: 'active',      lastUsedMs: 1500  },
  { dest: 0xAABBCC02, nextHop: 0xAABBCC02, hopCount: 1, metric: 112, state: 'active',      lastUsedMs: 9000  },
  { dest: 0xAABBCC03, nextHop: 0xAABBCC03, hopCount: 1, metric: 185, state: 'stale',       lastUsedMs: 45000 },
  { dest: 0xAABBCC04, nextHop: 0xAABBCC01, hopCount: 2, metric: 142, state: 'active',      lastUsedMs: 5200  },  // via Ridge
  { dest: 0xAABBCC05, nextHop: 0xAABBCC05, hopCount: 1, metric: 210, state: 'active',      lastUsedMs: 62000 },
  { dest: 0xBEEF0001, nextHop: 0xAABBCC01, hopCount: 3, metric: 240, state: 'discovering', lastUsedMs: 0     },  // unknown far node
];

const channels = [
  { index: 0, name: 'Bramble Common', hasPsk: false, epoch: 1, isDefault: true  },
  { index: 1, name: 'Example SAR',  hasPsk: true,  epoch: 5, isDefault: false },
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
      anchor_fingerprint: anchorFingerprint(mockAnchorPub),
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
    // Broadcasts reach neighbors directly — simulate realistic delivery timing
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

    // Also simulate 2-hop delivery to Northside (via Ridge) with longer delay
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
      version_floor: '1.0.0',
      running_version: '0.4.2-dev',
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
    return {
      ok: true,
      note: 'OTA update started (mock); the node would reboot on success',
      url: otaOrigin.replace(/\/?$/, '/') + path,
      partition: 'ota_1',
    };
  },

  'bramble.shareLocationOnce'(params) {
    setTimeout(() => {
      notify('location.update', {
        addr: params?.addr,
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

// Simulate incoming messages from mesh — realistic traffic
const MESH_CHATTER = [
  { from: 0xAABBCC01, texts: ['Ridge node checking in. Strong signal today.', 'Wind picking up on the ridge, antenna holding.', 'Relayed 3 packets this hour.'] },
  { from: 0xAABBCC02, texts: ['Trailhead here. Hikers passing through.', 'Solar panel at 14.2V, all good.', 'Forwarded a message to Northside via Ridge.'] },
  { from: 0xAABBCC03, texts: ['Downtown reporting. Downtown is noisy on 915.', 'Switching to SF10 for better range.', 'Anyone seeing interference today?'] },
  { from: 0xAABBCC04, texts: ['Northside node online. 2 hops to HomeBase confirmed.', 'Battery swap complete, back on air.', 'Can someone relay to Trailhead? Lost direct path.'] },
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
