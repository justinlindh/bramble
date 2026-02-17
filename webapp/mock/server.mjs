/**
 * Bramble Mock Node — WebSocket JSON-RPC 2.0 server
 * Single-file development server. Port 3005.
 *
 * Implements the same JSON-RPC wire protocol as the real firmware:
 *   Request:      { jsonrpc: "2.0", id: N, method: "bramble.X", params: {...} }
 *   Response:     { jsonrpc: "2.0", id: N, result: {...} }
 *   Notification: { jsonrpc: "2.0", method: "bramble.X", params: {...} }
 */

import { WebSocketServer } from 'ws';

const PORT = 3005;
const MOCK_ADDR = 0x1B3C4D5E;
const MOCK_NAME = 'MockNode';
const BOOT_TIME = Date.now();

// ─── Mutable state ──────────────────────────────────────────────────────────

let txCount = 12;
let rxCount = 87;
let droppedCount = 1;
let airtimeUsedMs = 4320;
let packetIdCounter = 1000;
let msgIdCounter = 1;

const neighbors = [
  { addr: 0xAABBCCDD, rssi: -72,  snr: 8.5,  deliveryRate: 240, lastHeardMs: 3200,  isMailbox: false, airtimeRemaining: 85 },
  { addr: 0x11223344, rssi: -89,  snr: 4.2,  deliveryRate: 180, lastHeardMs: 12800, isMailbox: true,  airtimeRemaining: 62 },
  { addr: 0xDEADBEEF, rssi: -63,  snr: 11.0, deliveryRate: 255, lastHeardMs: 800,   isMailbox: false, airtimeRemaining: 91 },
  { addr: 0xFEEDFACE, rssi: -104, snr: 1.8,  deliveryRate: 120, lastHeardMs: 45000, isMailbox: false, airtimeRemaining: 30 },
];

const routes = [
  { dest: 0xAABBCCDD, nextHop: 0xAABBCCDD, hopCount: 1, metric: 72,  state: 'active',      lastUsedMs: 2000  },
  { dest: 0x11223344, nextHop: 0xAABBCCDD, hopCount: 2, metric: 161, state: 'active',      lastUsedMs: 15000 },
  { dest: 0xDEADBEEF, nextHop: 0xDEADBEEF, hopCount: 1, metric: 63,  state: 'active',      lastUsedMs: 1200  },
  { dest: 0xFEEDFACE, nextHop: 0xDEADBEEF, hopCount: 3, metric: 230, state: 'stale',       lastUsedMs: 90000 },
  { dest: 0xCAFEBABE, nextHop: 0xAABBCCDD, hopCount: 4, metric: 255, state: 'discovering', lastUsedMs: 0     },
];

const channels = [
  { index: 0, name: 'general',  hasPsk: false, epoch: 1, isDefault: true  },
  { index: 1, name: 'ops',      hasPsk: true,  epoch: 3, isDefault: false },
];

const locationContacts = [
  { addr: 0xAABBCCDD, tier: 'full', intervalSec: 300, distanceTriggerM: 100 },
  { addr: 0xDEADBEEF, tier: 'coarse', intervalSec: 600, distanceTriggerM: 500 },
];

const locationConfig = {
  enabled: true,
  contacts: locationContacts,
  defaultIntervalSec: 300,
  defaultDistanceTriggerM: 100,
  stationaryBackoff: 4,
};

// Henderson NV area coordinates
const peerLocations = [
  {
    addr: 0xAABBCCDD,
    name: 'Alpha',
    tier: 'full',
    position: {
      lat: 36.0395,
      lon: -114.9817,
      alt: 569,
      accuracy: 5,
      speed: 12,
      heading: 225,
      timestampMs: Date.now() - 45000,
    },
    online: true,
    lastUpdatedMs: Date.now() - 45000,
  },
  {
    addr: 0x11223344,
    name: 'Bravo',
    tier: 'coarse',
    position: null,
    gridSquare: 'DM26',
    online: true,
    lastUpdatedMs: Date.now() - 120000,
  },
  {
    addr: 0xDEADBEEF,
    name: 'Charlie',
    tier: 'full',
    position: {
      lat: 36.0725,
      lon: -115.0182,
      alt: 612,
      accuracy: 8,
      speed: 0,
      heading: 0,
      timestampMs: Date.now() - 300000,
    },
    online: true,
    lastUpdatedMs: Date.now() - 300000,
  },
  {
    addr: 0xFEEDFACE,
    name: 'Delta',
    tier: 'presence',
    position: null,
    online: false,
    lastUpdatedMs: Date.now() - 3600000,
  },
];

const config = {
  identity: {
    address: MOCK_ADDR,
    pubkeyHash: 0x3A7F2B8C,
    name: MOCK_NAME,
    pubkeyB64: 'mock+pubkey+base64+placeholder==',
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
    if (ws.readyState === 1 /* OPEN */) {
      ws.send(msg);
    }
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
      fwVersion: '0.4.2-mock',
      txCount,
      rxCount,
      droppedCount,
      neighborCount: neighbors.length,
      routeCount: routes.filter(r => r.state === 'active').length,
      airtimeUsedMs,
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
        {
          name: 'critical',
          remainingMs: 8500,
          maxMs: 10000,
          usedPct: 15,
          refillAtMs: now + 55000,
        },
        {
          name: 'normal',
          remainingMs: 32000,
          maxMs: 60000,
          usedPct: 47,
          refillAtMs: now + 120000,
        },
        {
          name: 'broadcast',
          remainingMs: 4200,
          maxMs: 30000,
          usedPct: 86,
          refillAtMs: now + 300000,
        },
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
    return {
      routes: routes.map(r => ({ ...r })),
    };
  },

  'bramble.getMessages'(_params) {
    return { messages: [] };
  },

  'bramble.sendMessage'(params) {
    const packetId = ++packetIdCounter;
    txCount++;
    airtimeUsedMs += 350 + Math.floor(Math.random() * 200);

    const dest = params?.dest ?? 0xFFFFFFFF;
    const delayMs = 1000 + Math.floor(Math.random() * 2000);

    // Simulate delivery after 1-3s
    setTimeout(() => {
      const relayPath = dest !== 0xFFFFFFFF ? [
        { addr: 0xDEADBEEF, rssi: -68 },
        { addr: 0xAABBCCDD, rssi: -79 },
      ] : [];

      notify('bramble.onAck', {
        packetId,
        status: 'delivered',
        relayPath,
      });
    }, delayMs);

    return { packetId };
  },

  'bramble.setRadio'(params) {
    if (params) {
      Object.assign(config.radio, params);
    }
    return { ok: true };
  },

  'bramble.setNodeName'(params) {
    if (params?.name) {
      config.identity.name = String(params.name).slice(0, 8);
    }
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

  'bramble.sendProbe'(_params) {
    const probeId = 0xa000 + Math.floor(Math.random() * 0xfff);
    const ackWindow = 30;

    // Simulate 4-6 ACKs over 10-20s
    const ackCount = 4 + Math.floor(Math.random() * 3);
    const mockAddrs = [0xAABBCCDD, 0x11223344, 0xDEADBEEF, 0xFEEDFACE, 0xCAFEBABE, 0xBEEF1234];
    for (let i = 0; i < ackCount; i++) {
      const delay = 2000 + Math.floor(Math.random() * 18000);
      const hopCount = 1 + Math.floor(Math.random() * 3);
      const rssi = -72 - Math.floor(Math.random() * 33);
      const snr = -1 + Math.random() * 10;
      const relayPath = [];
      for (let h = 0; h < hopCount - 1; h++) {
        relayPath.push(mockAddrs[Math.floor(Math.random() * mockAddrs.length)]);
      }
      setTimeout(() => {
        notify('probe.ack', {
          responderAddr: mockAddrs[i % mockAddrs.length],
          hopCount,
          rssi,
          snr: Math.round(snr * 10) / 10,
          pathLen: hopCount,
          relayPath,
          receivedAt: Date.now(),
        });
      }, delay);
    }

    // Send probe.complete after ackWindow
    setTimeout(() => {
      notify('probe.complete', { probeId });
    }, ackWindow * 1000);

    return { probeId, ackWindow };
  },

  'bramble.getPeerLocations'(_params) {
    // Drift positions slightly for realism
    for (const pl of peerLocations) {
      if (pl.position) {
        pl.position.lat += (Math.random() - 0.5) * 0.0002;
        pl.position.lon += (Math.random() - 0.5) * 0.0002;
        pl.position.timestampMs = Date.now() - Math.floor(Math.random() * 60000);
      }
      pl.lastUpdatedMs = Date.now() - Math.floor(Math.random() * 60000);
    }
    return { peerLocations: peerLocations.map(p => ({ ...p, position: p.position ? { ...p.position } : null })) };
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
    const addr = params?.addr;
    // Simulate sending a one-time location update after a short delay
    setTimeout(() => {
      notify('location.update', {
        addr,
        name: 'MockNode',
        tier: 'full',
        position: {
          lat: 36.0395 + (Math.random() - 0.5) * 0.001,
          lon: -114.9817 + (Math.random() - 0.5) * 0.001,
          alt: 569,
          accuracy: 5,
          speed: 0,
          heading: 0,
          timestampMs: Date.now(),
        },
        online: true,
        lastUpdatedMs: Date.now(),
      });
    }, 500);
    return { ok: true };
  },

  'bramble.setDefaultChannel'(params) {
    const idx = params?.index;
    for (const ch of channels) {
      ch.isDefault = ch.index === idx;
    }
    return { ok: true };
  },
};

// ─── Server ──────────────────────────────────────────────────────────────────

const wss = new WebSocketServer({ port: PORT });

wss.on('listening', () => {
  console.log(`[mock-node] WebSocket server listening on ws://0.0.0.0:${PORT}`);
  console.log(`[mock-node] Mock address: 0x${MOCK_ADDR.toString(16).toUpperCase()}, name: ${MOCK_NAME}`);
});

wss.on('connection', (ws, req) => {
  const ip = req.socket.remoteAddress;
  console.log(`[mock-node] Client connected from ${ip}`);
  clients.add(ws);

  ws.on('message', (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch {
      console.warn('[mock-node] Received invalid JSON, ignoring');
      return;
    }

    const { id, method, params } = msg;
    console.log(`[mock-node] RPC → ${method} (id=${id})`);

    const handler = handlers[method];
    if (!handler) {
      if (id !== undefined) {
        ws.send(JSON.stringify({
          jsonrpc: '2.0',
          id,
          error: { code: -32601, message: `Method not found: ${method}` },
        }));
      }
      return;
    }

    try {
      const result = handler(params ?? {});
      if (id !== undefined) {
        ws.send(JSON.stringify({ jsonrpc: '2.0', id, result }));
      }
    } catch (err) {
      console.error(`[mock-node] Handler error for ${method}:`, err);
      if (id !== undefined) {
        ws.send(JSON.stringify({
          jsonrpc: '2.0',
          id,
          error: { code: -32603, message: String(err.message ?? err) },
        }));
      }
    }
  });

  ws.on('close', () => {
    console.log(`[mock-node] Client disconnected`);
    clients.delete(ws);
  });

  ws.on('error', (err) => {
    console.error(`[mock-node] WebSocket error:`, err.message);
  });
});

// ─── Periodic notifications ───────────────────────────────────────────────────

// Counters tick up realistically every second
setInterval(() => {
  if (Math.random() < 0.3) rxCount++;
  if (Math.random() < 0.05) droppedCount++;
  airtimeUsedMs += Math.floor(Math.random() * 50);
}, 1000);

// Neighbor RSSI drift every 15s
setInterval(() => {
  for (const n of neighbors) {
    n.rssi += Math.floor((Math.random() - 0.5) * 6);
    n.rssi = Math.max(-120, Math.min(-55, n.rssi));
    n.snr += (Math.random() - 0.5) * 1.0;
    n.snr = Math.max(0, Math.min(14, n.snr));
    n.lastHeardMs = 500 + Math.floor(Math.random() * 3000);
  }
  if (clients.size > 0) {
    console.log('[mock-node] → bramble.onNeighborChange');
    notify('bramble.onNeighborChange', {});
  }
}, 15000);

// Random incoming messages every 10-30s
function scheduleIncoming() {
  const delayMs = 10000 + Math.floor(Math.random() * 20000);
  setTimeout(() => {
    if (clients.size > 0) {
      const sender = neighbors[Math.floor(Math.random() * neighbors.length)];
      const texts = [
        'Anyone copy? Testing mesh range.',
        'Node 3 reporting in. All green.',
        'RSSI looking good on my end.',
        'Can someone relay to sector 4?',
        'Battery at 67%. Will monitor.',
        'Route to base confirmed, 3 hops.',
        'Interference on 915, switching SF.',
        'Mesh stable. 4 neighbors visible.',
      ];
      const text = texts[Math.floor(Math.random() * texts.length)];
      const tiers = ['normal', 'normal', 'normal', 'normal', 'critical'];
      const tier = tiers[Math.floor(Math.random() * tiers.length)];
      const msgId = `mock-${++msgIdCounter}-${Date.now()}`;

      console.log(`[mock-node] → bramble.onMessage from 0x${sender.addr.toString(16).toUpperCase()}`);
      notify('bramble.onMessage', {
        from: sender.addr,
        to: MOCK_ADDR,
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
