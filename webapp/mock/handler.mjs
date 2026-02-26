/**
 * Bramble Mock Node — WebSocket JSON-RPC 2.0 handler module
 * Importable module for embedding in other servers.
 *
 * Simulates a realistic 5-node mesh in the Henderson/Inspirada area of NV.
 * "Our" node (HomeBase) sits in Inspirada. Peers are spread across Henderson.
 *
 * Implements the same JSON-RPC wire protocol as the real firmware:
 *   Request:      { jsonrpc: "2.0", id: N, method: "bramble.X", params: {...} }
 *   Response:     { jsonrpc: "2.0", id: N, result: {...} }
 *   Notification: { jsonrpc: "2.0", method: "bramble.X", params: {...} }
 */

// ─── Node identities ─────────────────────────────────────────────────────────
// Our node — Justin's house in Inspirada, Henderson NV
const SELF_ADDR  = 0x4A555354;  // "JUST"
const SELF_NAME  = 'HomeBase';
const SELF_POS   = { lat: 36.0043, lon: -115.0267, alt: 789, accuracy: 3 };

// Peer nodes — realistic Henderson locations
const PEERS = {
  0xAABBCC01: {
    name: 'Hilltop',    // Heltec on McCullough Hills ridge, great LOS
    pos: { lat: 36.0105, lon: -115.0320, alt: 856, accuracy: 5 },
    locTier: 'full',
  },
  0xAABBCC02: {
    name: 'TrailHead',  // Node at Sloan Canyon trailhead parking
    pos: { lat: 35.9860, lon: -115.0440, alt: 732, accuracy: 12 },
    locTier: 'full',
  },
  0xAABBCC03: {
    name: 'WaterSt',    // Downtown Henderson, Water Street District
    pos: null,
    gridSquare: 'DM26la',  // coarse — downtown Henderson area (~36.04, -115.04)
    locTier: 'coarse',
  },
  0xAABBCC04: {
    name: 'Anthem',     // Anthem neighborhood, ~8km north
    pos: { lat: 36.0580, lon: -115.0490, alt: 615, accuracy: 8 },
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

// Neighbors — direct radio contacts (not all peers are direct neighbors)
const neighbors = [
  {
    addr: 0xAABBCC01, name: 'Hilltop', // strong direct link (1.8km LOS)
    rssi: -68, snr: 9.5, deliveryRate: 245,
    lastHeardMs: 1200, isMailbox: true, airtimeRemaining: 88,
  },
  {
    addr: 0xAABBCC02, name: 'TrailHead', // moderate link (3.5km, some obstruction)
    rssi: -87, snr: 4.8, deliveryRate: 195,
    lastHeardMs: 8400, isMailbox: false, airtimeRemaining: 72,
  },
  {
    addr: 0xAABBCC03, name: 'WaterSt', // weak link (5km through buildings)
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
  { dest: 0xAABBCC04, nextHop: 0xAABBCC01, hopCount: 2, metric: 142, state: 'active',      lastUsedMs: 5200  },  // via Hilltop
  { dest: 0xAABBCC05, nextHop: 0xAABBCC05, hopCount: 1, metric: 210, state: 'active',      lastUsedMs: 62000 },
  { dest: 0xBEEF0001, nextHop: 0xAABBCC01, hopCount: 3, metric: 240, state: 'discovering', lastUsedMs: 0     },  // unknown far node
];

const channels = [
  { index: 0, name: 'Bramble Common', hasPsk: false, epoch: 1, isDefault: true  },
  { index: 1, name: 'Henderson SAR',  hasPsk: true,  epoch: 5, isDefault: false },
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
    if (ws.readyState === 1) ws.send(msg);
  }
}

function notify(method, params) {
  broadcast({ jsonrpc: '2.0', method, params });
}

// ─── RPC handlers ────────────────────────────────────────────────────────────

const handlers = {
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

    const dest = params?.dest ?? 0xFFFFFFFF;
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

    // Also simulate 2-hop delivery to Anthem (via Hilltop) with longer delay
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
        notify('probe.ack', {
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

    setTimeout(() => notify('probe.complete', { probeId }), ackWindow * 1000);
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
  { from: 0xAABBCC01, texts: ['Hilltop node checking in. Strong signal today.', 'Wind picking up on the ridge, antenna holding.', 'Relayed 3 packets this hour.'] },
  { from: 0xAABBCC02, texts: ['TrailHead here. Hikers passing through.', 'Solar panel at 14.2V, all good.', 'Forwarded a message to Anthem via Hilltop.'] },
  { from: 0xAABBCC03, texts: ['WaterSt reporting. Downtown is noisy on 915.', 'Switching to SF10 for better range.', 'Anyone seeing interference today?'] },
  { from: 0xAABBCC04, texts: ['Anthem node online. 2 hops to HomeBase confirmed.', 'Battery swap complete, back on air.', 'Can someone relay to TrailHead? Lost direct path.'] },
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
  if (!ranger._lat) { ranger._lat = 36.02; ranger._lon = -115.04; }
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
  console.log(`[mock-node] Client connected from ${ip}`);
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

    try {
      const result = handler(params ?? {});
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
