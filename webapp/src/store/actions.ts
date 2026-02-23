import { useStore } from './index';
import { createTransport, BrambleClient } from '../transport';
import { messageDb } from './messageDb';
import type {
  TransportType,
  BrambleConfig,
  NodeStatus,
  AirtimeStatus,
  AirtimeTier,
  Neighbor,
  Route,
  IncomingMessage,
  RelayHop,
  MessageTier,
  ProbeResponse,
  PeerLocation,
  LocationConfig,
  LocationContact,
  LocationTier,
  Message,
} from '../types/bramble';

// Map technical error messages to human-friendly text
const ERROR_MAP: Array<[RegExp, string]> = [
  [/cancelled.*requestDevice/i, 'Bluetooth pairing was cancelled.'],
  [/cancelled.*requestPort/i, 'Serial port selection was cancelled.'],
  [/user cancel/i, 'Connection was cancelled.'],
  [/no compatible device/i, 'No Bramble device found nearby.'],
  [/NetworkError/i, 'Could not reach the node. Check the IP address and that it\'s on the same network.'],
  [/WebSocket.*failed/i, 'Could not connect. Check the IP address and that the node is powered on.'],
  [/GATT.*disconnect/i, 'Bluetooth connection was lost.'],
  [/SecurityError/i, 'Browser blocked the connection. Try using HTTPS or localhost.'],
  [/AbortError/i, 'Connection timed out.'],
  [/NotFoundError/i, 'No device found. Make sure your node is powered on and in range.'],
  [/already.*connect/i, 'Already connected to a device.'],
];

function friendlyError(raw: string): string {
  for (const [pattern, friendly] of ERROR_MAP) {
    if (pattern.test(raw)) return friendly;
  }
  if (raw.length > 100) return 'Connection failed. Please try again.';
  return raw;
}

let client: BrambleClient | null = null;

// ─── Message persistence ─────────────────────────────────────────────────

export async function initMessageStore(nodeAddr?: string): Promise<void> {
  try {
    await messageDb.open(nodeAddr);
    const cached = await messageDb.getMessages();
    if (cached.length > 0) {
      useStore.getState().loadCachedMessages(cached);
    }
  } catch {
    // IndexedDB unavailable (e.g. private browsing) — continue without persistence
  }
}

// crypto.randomUUID() requires secure context (HTTPS/localhost).
// Fallback for plain HTTP access over LAN.
function uuid(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID();
  }
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
    const r = (Math.random() * 16) | 0;
    return (c === 'x' ? r : (r & 0x3) | 0x8).toString(16);
  });
}

// ─── Connection ─────────────────────────────────────────────────────────

export async function connect(type: TransportType, options?: { url?: string }): Promise<void> {
  const store = useStore.getState();
  store.setConnectionState('connecting');
  try {
    const transport = createTransport(type, options);
    await transport.connect();
    client = new BrambleClient(transport);
    store.setConnectionState('connected');
    store.setTransport(transport);

    // Enable auto-reconnect for WiFi/WebSocket transports
    if ('enableAutoReconnect' in transport && typeof (transport as any).enableAutoReconnect === 'function') {
      (transport as any).enableAutoReconnect({
        onDisconnect: () => {
          useStore.getState().setConnectionState('error', 'Connection lost — reconnecting…');
        },
        onReconnect: async () => {
          useStore.getState().setConnectionState('connected');
          try {
            const opt = (p: Promise<void>) => p.catch(() => {});
            await Promise.all([loadConfig(), loadNeighbors(), loadRoutes(), opt(loadMessages()), loadAirtime()]);
          } catch { /* best effort */ }
        },
      });
    }

    // Subscribe to push events
    client.subscribe('bramble.onMessage', (params) =>
      handleIncomingMessage(params)
    );
    client.subscribe('bramble.onAck', (params) => handleAck(params));
    client.subscribe('bramble.onNeighborChange', () => refreshNeighbors());
    client.subscribe('bramble.onRouteUpdate', () => loadRoutes());
    client.subscribe('bramble.onAirtimeWarning', () => loadAirtime());
    client.subscribe('bramble.onProbeResult', (params) => handleProbeAck(params));
    client.subscribe('bramble.onProbeComplete', (params) => handleProbeComplete(params));
    client.subscribe('location.update', (params) => handleLocationUpdate(params));
    client.subscribe('bramble.onPeerLocation', (params) => handleLocationUpdate(params));
    client.subscribe('bramble.onTrafficEvent', (params) => handleTrafficEvent(params));

    // Clear stale data from previous node connection
    store.resetNodeData();

    // Initial data load — all best-effort so a slow RPC doesn't kill the connection
    const opt = (p: Promise<void>) => p.catch((e) => console.warn('[init]', e.message));

    // Load config first to get node address for IndexedDB namespacing
    await opt(loadConfig());
    const nodeAddr = store.config?.identity?.address;
    const addrHex = nodeAddr ? nodeAddr.toString(16).toUpperCase().padStart(8, '0') : undefined;
    await initMessageStore(addrHex);

    await Promise.all([
      opt(loadStatus()),
      opt(loadAirtime()),
      opt(loadNeighbors()),
      opt(loadRoutes()),
      opt(loadMessages()),
      opt(loadPeerLocations()),
    ]);
  } catch (e) {
    // Clean up any partially-initialised client so we start fresh on retry
    client?.clearSubscriptions();
    client = null;
    // Show overlay so user can retry — 'disconnected' shows the connect UI
    store.setConnectionState('disconnected', friendlyError((e as Error).message));
  }
}

export async function disconnect(): Promise<void> {
  try {
    await client?.rpc('bramble.disconnect');
  } catch {
    // Ignore — node may not have this method, or already disconnected
  }
  client?.clearSubscriptions();
  await client?.disconnect();
  client = null;
  useStore.getState().setConnectionState('disconnected');
  useStore.getState().setTransport(null);
}

// ─── Data loading ────────────────────────────────────────────────────────

/**
 * Normalize firmware config response to match BrambleConfig interface.
 * Firmware returns flat structure; webapp expects nested identity/radio objects.
 */
// eslint-disable-next-line @typescript-eslint/no-explicit-any
function normalizeConfig(raw: any): BrambleConfig {
  return {
    identity: {
      address: typeof raw.address === 'string' ? parseInt(raw.address, 16) : (raw.identity?.address ?? 0),
      pubkeyHash: typeof raw.pubkey_hash === 'string' ? parseInt(raw.pubkey_hash, 16) : (raw.identity?.pubkeyHash ?? 0),
      name: raw.node_name ?? raw.identity?.name ?? '',
      pubkeyB64: raw.identity?.pubkeyB64 ?? '',
    },
    radio: {
      txPowerDbm: raw.radio?.tx_power_dbm ?? raw.radio?.txPowerDbm ?? 0,
      sf: raw.radio?.sf ?? 9,
      bwKhz: raw.radio?.bw_hz ? Math.round(raw.radio.bw_hz / 1000) : (raw.radio?.bwKhz ?? 125),
      cr: raw.radio?.cr ?? 5,
      freqMhz: raw.radio?.frequency_mhz ?? raw.radio?.freqMhz ?? 915.0,
    },
    channels: (raw.channels ?? []).map((ch: any) => ({
      index: ch.id ?? ch.index ?? 0,
      name: ch.name ?? '',
      hasPsk: ch.hasPsk ?? false,
      epoch: ch.epoch ?? 0,
      isDefault: ch.is_default ?? ch.isDefault ?? false,
    })),
    mailboxEnabled: raw.mailboxEnabled ?? false,
    location: raw.location ?? {
      enabled: false,
      contacts: [],
      defaultIntervalSec: 60,
      defaultDistanceTriggerM: 100,
      stationaryBackoff: 3,
    },
  } as BrambleConfig;
}

export async function loadConfig(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<any>('bramble.getConfig');
  useStore.getState().setConfig(normalizeConfig(result));
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function normalizeStatus(raw: any): NodeStatus {
  return {
    uptimeSec: raw.uptime_s ?? raw.uptimeSec ?? 0,
    freeHeapBytes: raw.free_heap ?? raw.freeHeapBytes ?? 0,
    fwVersion: raw.firmware_version ?? raw.fwVersion ?? '',
    txCount: raw.packets_tx ?? raw.txCount ?? 0,
    rxCount: raw.packets_rx ?? raw.rxCount ?? 0,
    droppedCount: raw.droppedCount ?? 0,
    neighborCount: raw.peers ?? raw.neighborCount ?? 0,
    routeCount: raw.routeCount ?? 0,
    airtimeUsedMs: raw.airtimeUsedMs ?? 0,
    position: raw.position,
    gpsAvailable: raw.gps_available ?? raw.gpsAvailable ?? false,
    batteryMv: raw.battery_mv ?? raw.batteryMv,
    batteryPct: raw.battery_pct ?? raw.batteryPct,
  } as NodeStatus;
}

export async function loadStatus(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<any>('bramble.getStatus');
  useStore.getState().setStatus(normalizeStatus(result));
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function normalizeAirtime(raw: any): AirtimeStatus {
  // Firmware returns flat fields; webapp expects { tiers: [...] }
  if (raw.tiers) return raw as AirtimeStatus;

  // next_refill_ms is a duration (ms until next refill). 0 means "just refilled"
  // — treat it as a full interval from now. Default to 1 hour if missing.
  const nextRefillMs = raw.next_refill_ms ?? 3600000;
  const refillAtMs = Date.now() + (nextRefillMs > 0 ? nextRefillMs : REFILL_INTERVAL_MS);

  return {
    tiers: [
      { name: 'critical', remainingMs: raw.critical_remaining_ms ?? 0, maxMs: raw.critical_max_ms ?? 36000, usedPct: 0, refillAtMs },
      { name: 'normal', remainingMs: raw.normal_remaining_ms ?? 0, maxMs: raw.normal_max_ms ?? 18000, usedPct: 0, refillAtMs },
      { name: 'broadcast', remainingMs: raw.broadcast_remaining_ms ?? 0, maxMs: raw.broadcast_max_ms ?? 18000, usedPct: 0, refillAtMs },
    ].map(t => ({ ...t, usedPct: t.maxMs > 0 ? Math.round(100 * (t.maxMs - t.remainingMs) / t.maxMs) : 0 })) as [AirtimeTier, AirtimeTier, AirtimeTier],
  };
}

const REFILL_INTERVAL_MS = 3600000;

export async function loadAirtime(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<any>('bramble.getAirtime');
  useStore.getState().setAirtime(normalizeAirtime(result));
}

export async function loadAirtimePolicy(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<any>('bramble.getAirtimePolicy');
  useStore.getState().setAirtimePolicy(result);
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function normalizeNeighbor(raw: any): Neighbor & { name?: string } {
  return {
    addr: typeof raw.address === 'string' ? parseInt(raw.address, 16) : (raw.addr ?? 0),
    rssi: raw.rssi ?? 0,
    snr: raw.snr ?? 0,
    deliveryRate: raw.deliveryRate ?? 0,
    lastHeardMs: raw.last_seen_ms ?? raw.lastHeardMs ?? 0,
    airtimeRemaining: raw.airtimeRemaining ?? 0,
    ...(raw.name ? { name: raw.name } : {}),
  } as Neighbor & { name?: string };
}

export async function loadNeighbors(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<{ neighbors: any[] }>('bramble.getNeighbors');
  useStore.getState().setNeighbors((result.neighbors ?? []).map(normalizeNeighbor));
}

export async function loadRoutes(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<{ routes: Route[] }>('bramble.getRoutes');
  useStore.getState().setRoutes(result.routes ?? []);
}

function isLikelyDuplicate(existing: Message, candidate: Message): boolean {
  if (existing.direction !== candidate.direction) return false;
  if (existing.from !== candidate.from || existing.to !== candidate.to) return false;
  if ((existing.channelIndex ?? -1) !== (candidate.channelIndex ?? -1)) return false;
  if (existing.text !== candidate.text) return false;
  return Math.abs(existing.timestampMs - candidate.timestampMs) < 5000;
}

export async function loadMessages(sinceId?: number): Promise<void> {
  if (!client) return;
  const params: Record<string, unknown> = { limit: 100 };
  if (sinceId !== undefined) params.since_id = sinceId;
  const result = await client.rpc<{ messages: IncomingMessage[] }>(
    'bramble.getMessages',
    params,
    10000, // longer timeout — serializing 20 messages can be slow on ESP32
  );
  const store = useStore.getState();
  // Get device uptime to convert uptime-based timestamps to wall clock
  const deviceUptime = store.status?.uptimeSec ?? 0;
  const now = Date.now();
  for (const m of result.messages ?? []) {
    const fromAddr = typeof m.from === 'string' ? parseInt(m.from, 16) : (m.from ?? 0);
    const toAddr = typeof m.to === 'string' ? parseInt(m.to, 16) : (m.to ?? 0);
    const dir = (m as any).direction;
    const isOutgoing = dir === 'outgoing' || dir === 'broadcast_out';
    const isBroadcast = dir === 'broadcast_in' || dir === 'broadcast_out';
    // Skip self-addressed messages (firmware bug: old messages stored with wrong dest)
    const myAddr = store.config?.identity?.address ?? 0;
    if (!isBroadcast && fromAddr === toAddr && fromAddr === myAddr) continue;
    // Convert uptime-based timestamp to wall clock: now - (uptime - msg_time)
    const msgUptimeS = (m as any).timestamp_s ?? 0;
    const wallMs = deviceUptime > 0 && msgUptimeS > 0
      ? now - (deviceUptime - msgUptimeS) * 1000
      : now;
    const fwMsg: Message = {
      id: m.msgId ?? `fw-${msgUptimeS || Date.now()}-${fromAddr}`,
      direction: isOutgoing ? 'outgoing' : 'incoming',
      from: fromAddr,
      to: isBroadcast ? 0xFFFFFFFF : toAddr,
      text: m.text,
      tier: m.tier,
      channelIndex: isBroadcast ? undefined : m.channelIndex,
      timestampMs: wallMs,
      status: 'delivered',
    };
    const existing = store.messages.find(ex => isLikelyDuplicate(ex, fwMsg));
    if (existing) {
      // Preserve richer cached message (e.g., relay path/status from web-side send path).
      if (existing.relayPath && !fwMsg.relayPath) continue;
      continue;
    }
    store.addMessage(fwMsg);
  }
}

// ─── Messaging ────────────────────────────────────────────────────────────

const packetIdToMsgId = new Map<string, string>();

// Fragmentation limits (aligned with firmware components/fragment):
// - Single packet max: 203 bytes
// - Fragment payload: 154 bytes/fragment
// - Max fragments: 4
// - True fragmented max: 154 × 4 = 616 bytes
const SINGLE_PACKET_MAX_BYTES = 203;
const FRAGMENTED_MAX_BYTES = 616;

function utf8ByteLength(s: string): number {
  return new TextEncoder().encode(s).length;
}

export async function sendMessage(
  dest: number,
  text: string,
  tier: MessageTier = 'normal',
  channelIndex?: number
): Promise<void> {
  if (!client) throw new Error('Not connected');
  const store = useStore.getState();

  const messageBytes = utf8ByteLength(text);
  if (messageBytes > FRAGMENTED_MAX_BYTES) {
    throw new Error(`Message too long (${messageBytes} bytes). Max is ${FRAGMENTED_MAX_BYTES} bytes.`);
  }

  const msg = {
    id: uuid(),
    direction: 'outgoing' as const,
    from: 0,
    to: dest,
    text,
    tier,
    channelIndex,
    timestampMs: Date.now(),
    status: 'sending' as const,
  };

  store.addMessage(msg);
  messageDb.saveMessage(msg).catch(() => {});

  try {
    const isChannelScoped = channelIndex !== undefined && channelIndex >= 0;
    const isBroadcast = dest === 0xFFFFFFFF && !isChannelScoped;
    const method = isBroadcast ? 'bramble.sendBroadcast' : 'bramble.sendMessage';
    const wireDest = (dest === 0xFFFFFFFE) ? 0xFFFFFFFF : dest;
    const params = isBroadcast
      ? { text }
      : {
          dest: wireDest.toString(16).toUpperCase().padStart(8, '0'),
          text,
          ...(isChannelScoped ? { channel: channelIndex } : {}),
        };
    const result = await client.rpc<{
      message_id?: string;
      status?: string;
      packetId?: string;
      fragmented?: boolean;
      fragments_total?: number;
    }>(method, params);
    
    // Log fragmentation info for debugging
    if (result?.fragmented && result?.fragments_total) {
      console.info(`[send] Message fragmented into ${result.fragments_total} packets`);
    }
    
    store.updateMessageStatus(msg.id, 'sent');
    messageDb.updateMessageStatus(msg.id, 'sent').catch(() => {});
    if (result?.packetId) {
      packetIdToMsgId.set(result.packetId, msg.id);
    }
  } catch (e) {
    store.updateMessageStatus(msg.id, 'failed');
    messageDb.updateMessageStatus(msg.id, 'failed').catch(() => {});
    throw e;
  }
}

// ─── Notification handlers ────────────────────────────────────────────────

function handleAck(params: unknown): void {
  const p = params as Record<string, unknown>;
  /* Firmware sends snake_case (packet_id), webapp convention is camelCase */
  const packetId = (p.packetId ?? p.packet_id) as string | undefined;
  const status = (p.status as string) ?? 'delivered';
  /* Normalize relayPath: firmware sends addr as hex string, webapp needs number */
  const rawPath = p.relayPath as Array<{ addr: string | number; rssi: number }> | undefined;
  const relayPath: RelayHop[] | undefined = rawPath?.map(hop => ({
    addr: typeof hop.addr === 'string' ? parseInt(hop.addr, 16) : hop.addr,
    rssi: hop.rssi ?? 0,
  }));

  if (!packetId) return;
  const msgId = packetIdToMsgId.get(packetId);
  if (msgId) {
    packetIdToMsgId.delete(packetId);
    const newStatus = status === 'delivered' ? 'delivered' : 'failed';
    useStore.getState().updateMessageStatus(msgId, newStatus, relayPath);
    messageDb.updateMessageStatus(msgId, newStatus, relayPath).catch(() => {});
  }
}

export function normalizeIncomingRealtimeMessage(params: unknown) {
  const p = params as any;
  const fromAddr = typeof p.from === 'string' ? parseInt(p.from, 16) : (p.from ?? 0);
  const toAddr = typeof p.to === 'string' ? parseInt(p.to, 16) : (p.to ?? 0);
  const rawChannel = p.channelIndex ?? (p.channel as number | undefined);
  const channelIndex = rawChannel !== undefined && rawChannel >= 0 ? rawChannel : undefined;
  const isBroadcast = channelIndex === undefined && (p.broadcast === true || toAddr === 0xFFFFFFFF);

  return {
    id: p.msgId ?? `rt-${Date.now()}`,
    direction: 'incoming' as const,
    from: fromAddr,
    to: isBroadcast ? 0xFFFFFFFF : toAddr,
    text: p.text,
    tier: p.tier,
    channelIndex,
    timestampMs: Date.now(),
    status: 'delivered' as const,
  };
}

function handleIncomingMessage(params: unknown): void {
  const p = params as any;
  console.log('[handleIncomingMessage] Raw params:', JSON.stringify(p, null, 2));
  const msg = normalizeIncomingRealtimeMessage(p);
  console.log('[handleIncomingMessage] Message object:', msg);
  const store = useStore.getState();
  store.addMessage(msg);
  messageDb.saveMessage(msg).catch(() => {});
}

async function refreshNeighbors(): Promise<void> {
  await loadNeighbors();
}

// ─── Config mutations ────────────────────────────────────────────────────

/** Throw if an RPC result has ok:false with an error message */
function assertOk(result: unknown, fallback = 'Operation failed'): void {
  const r = result as Record<string, unknown> | null;
  if (r && r.ok === false) {
    throw new Error((r.error as string) || fallback);
  }
}

export async function saveRadio(radio: import('../types/bramble').RadioConfig): Promise<void> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc('bramble.setRadio', radio as unknown as Record<string, unknown>);
  assertOk(result, 'Radio config failed');
  await loadConfig();
}

export async function saveNodeName(name: string): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.setNodeName', { name });
  await loadConfig();
}

export async function addChannel(name: string, psk?: string): Promise<number> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc<{ ok: boolean; index: number; error?: string }>('bramble.addChannel', {
    name,
    ...(psk ? { psk } : {}),
  });
  assertOk(result, 'Failed to add channel');
  await loadConfig();
  return result.index;
}

export async function removeChannel(index: number): Promise<void> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc('bramble.removeChannel', { index });
  assertOk(result, 'Failed to remove channel');
  await loadConfig();
}

export async function setMailbox(enabled: boolean): Promise<void> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc('bramble.setMailbox', { enabled });
  assertOk(result, 'Failed to set mailbox');
  await loadConfig();
}

export async function setDefaultChannel(index: number): Promise<void> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc('bramble.setDefaultChannel', { index });
  assertOk(result, 'Failed to set default channel');
  await loadConfig();
}

export async function setAirtimePolicy(config: {
  enabled?: boolean;
  baseIntervalMs?: number;
  minIntervalMs?: number;
  maxIntervalMs?: number;
  denseThreshold?: number;
  churnWindowSec?: number;
  churnThreshold?: number;
  cooldownSec?: number;
}): Promise<void> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc('bramble.setAirtimePolicy', config);
  assertOk(result, 'Failed to set airtime policy');
  await loadAirtimePolicy();
}

export function openDM(addr: number): void {
  const store = useStore.getState();
  store.setActiveConversation(`dm:${addr}`);
  store.setActiveTab('chat');
}

export function showOnMap(addr: number): void {
  const store = useStore.getState();
  store.setMapFocusAddr(addr);
  store.setActiveTab('map');
}

// ─── Probe / Network Reach ────────────────────────────────────────────────

export function upsertProbeResponse(responses: ProbeResponse[], next: ProbeResponse): ProbeResponse[] {
  const idx = responses.findIndex(r => r.responderAddr === next.responderAddr);
  if (idx < 0) return [...responses, next];

  const prev = responses[idx];
  const merged: ProbeResponse = {
    ...prev,
    ...next,
    // keep best quality samples
    rssi: Math.max(prev.rssi ?? -999, next.rssi ?? -999),
    snr: Math.max(prev.snr ?? -999, next.snr ?? -999),
    // keep latest receive timestamp/latency if present in next
    receivedAt: next.receivedAt ?? prev.receivedAt,
    latencyMs: next.latencyMs ?? prev.latencyMs,
    seenRounds: Math.max(prev.seenRounds ?? 1, next.seenRounds ?? 1),
    confidence: Math.max(prev.confidence ?? 0, next.confidence ?? 0),
  };

  const out = [...responses];
  out[idx] = merged;
  return out;
}

export async function sendProbe(): Promise<void> {
  if (!client) throw new Error('Not connected');
  const store = useStore.getState();

  const raw = await client.rpc<Record<string, unknown>>('bramble.sendProbe');
  /* Firmware returns probe_id (snake_case hex string), no ackWindow */
  const probeIdStr = (raw.probeId ?? raw.probe_id) as string | undefined;
  const probeId = probeIdStr ? parseInt(probeIdStr, 16) : Math.floor(Math.random() * 0xFFFFFFFF);
  const ackWindow = (raw.ackWindow ?? raw.ack_window ?? 30) as number;
  const sentAt = Date.now();

  store.setProbeResult({
    probeId,
    sentAt,
    ackWindow,
    responses: [],
    complete: false,
  });
  store.setProbeCollecting(true);

  /* Firmware currently emits onProbeResult but not onProbeComplete.
   * Auto-finalize at ack-window expiry to avoid stuck Collecting state. */
  setTimeout(() => {
    const s = useStore.getState();
    const cur = s.probeResult;
    if (!cur) return;
    if (cur.probeId !== probeId) return;      /* newer probe started */
    if (!s.probeCollecting || cur.complete) return;
    s.setProbeResult({ ...cur, complete: true });
    s.setProbeCollecting(false);
  }, Math.max(1, ackWindow) * 1000 + 150);
}

function handleProbeAck(params: unknown): void {
  const raw = params as any;
  const parsedAddr = typeof raw.address === 'string'
    ? parseInt(raw.address.replace(/^0x/i, ''), 16)
    : (raw.responderAddr ?? 0);

  const roundsTotal = 3;
  const seenRounds = Math.max(1, Math.min(roundsTotal, raw.seen_rounds ?? 1));
  const ack: ProbeResponse = {
    responderAddr: Number.isFinite(parsedAddr) ? parsedAddr : 0,
    hopCount: raw.hops ?? raw.hopCount ?? 0,
    rssi: raw.rssi ?? 0,
    snr: raw.snr ?? 0,
    pathLen: raw.hops ?? raw.pathLen ?? 0,
    latencyMs: raw.latency_ms ?? raw.latencyMs ?? 0,
    seenRounds,
    confidence: seenRounds / roundsTotal,
  };
  const probeId = typeof raw.probeId === 'string'
    ? parseInt(raw.probeId, 16)
    : typeof raw.probe_id === 'string'
    ? parseInt(raw.probe_id, 16)
    : (raw.probeId ?? raw.probe_id ?? undefined);

  const store = useStore.getState();
  const prev = store.probeResult;
  if (!prev || prev.complete) return;
  if (probeId !== undefined && probeId !== prev.probeId) return;

  const selfAddr = store.config?.identity?.address;
  if (selfAddr !== undefined && ack.responderAddr === selfAddr) return;

  store.setProbeResult({
    ...prev,
    responses: upsertProbeResponse(prev.responses, { ...ack, receivedAt: Date.now() }),
  });
}

function handleProbeComplete(params: unknown): void {
  const p = params as any;
  const probeId = typeof p.probeId === 'string'
    ? parseInt(p.probeId, 16)
    : typeof p.probe_id === 'string'
    ? parseInt(p.probe_id, 16)
    : (p.probeId ?? p.probe_id);

  const store = useStore.getState();
  const prev = store.probeResult;
  if (!prev || prev.probeId !== probeId) return;

  const roundsTotal = Math.max(1, Number(p.rounds_total ?? p.roundsTotal ?? 3));
  const responders = Array.isArray(p.responders) ? p.responders : [];

  let responses = prev.responses;
  for (const r of responders) {
    const addr = typeof r.address === 'string'
      ? parseInt(r.address.replace(/^0x/i, ''), 16)
      : (r.responderAddr ?? 0);
    const seenRounds = Math.max(1, Math.min(roundsTotal, Number(r.seen_rounds ?? r.seenRounds ?? 1)));
    responses = upsertProbeResponse(responses, {
      responderAddr: Number.isFinite(addr) ? addr : 0,
      hopCount: r.hops ?? r.hopCount ?? 0,
      rssi: r.rssi ?? 0,
      snr: r.snr ?? 0,
      pathLen: r.hops ?? r.pathLen ?? 0,
      latencyMs: r.latency_ms ?? r.latencyMs ?? 0,
      seenRounds,
      confidence: seenRounds / roundsTotal,
    });
  }

  store.setProbeResult({ ...prev, responses, complete: true });
  store.setProbeCollecting(false);
}

// ─── Location ─────────────────────────────────────────────────────────────

export async function loadPeerLocations(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<{ peerLocations: PeerLocation[] }>('bramble.getPeerLocations');
  useStore.getState().setPeerLocations(result.peerLocations ?? []);
}

export async function setLocationConfig(config: Partial<LocationConfig>): Promise<void> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc('bramble.setLocationConfig', config as unknown as Record<string, unknown>);
  assertOk(result, 'Failed to save location config');
  await loadConfig();
}

export async function setLocationContact(
  addr: number,
  tier: LocationTier,
  intervalSec?: number,
  distanceTriggerM?: number
): Promise<void> {
  if (!client) throw new Error('Not connected');
  const params: Record<string, unknown> = { addr, tier };
  if (intervalSec !== undefined) params.intervalSec = intervalSec;
  if (distanceTriggerM !== undefined) params.distanceTriggerM = distanceTriggerM;
  const result = await client.rpc('bramble.setLocationContact', params);
  assertOk(result, 'Failed to set location contact');
  await loadConfig();
}

export async function removeLocationContact(addr: number): Promise<void> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc('bramble.removeLocationContact', { addr });
  assertOk(result, 'Failed to remove location contact');
  await loadConfig();
}

export async function shareLocationOnce(addr: number, tier?: LocationTier): Promise<void> {
  if (!client) throw new Error('Not connected');
  const params: Record<string, unknown> = { addr };
  if (tier !== undefined) params.tier = tier;
  const result = await client.rpc('bramble.shareLocationOnce', params);
  assertOk(result, 'Failed to share location');
}

function handleLocationUpdate(params: unknown): void {
  const update = params as PeerLocation;
  const store = useStore.getState();
  const existing = store.peerLocations;
  const idx = existing.findIndex(p => p.addr === update.addr);
  if (idx >= 0) {
    const updated = [...existing];
    updated[idx] = update;
    store.setPeerLocations(updated);
  } else {
    store.setPeerLocations([...existing, update]);
  }
}

// ─── Traffic Debug ────────────────────────────────────────────────────────

export async function loadTrafficDebugStatus(): Promise<void> {
  if (!client) return;
  try {
    const result = await client.rpc<any>('bramble.getTrafficDebug');
    const status: any = {
      config: {
        enabled: result.enabled ?? false,
        includeTx: result.include_tx ?? result.includeTx ?? true,
        includeRx: result.include_rx ?? result.includeRx ?? true,
        sampleRate: result.sample_rate ?? result.sampleRate ?? 100,
      },
      ringSize: result.ring_size ?? result.ringSize ?? 512,
      ringUsed: result.ring_used ?? result.ringUsed ?? 0,
      droppedCount: result.dropped_count ?? result.droppedCount ?? 0,
      lastSeq: result.last_seq ?? result.lastSeq ?? 0,
    };
    useStore.getState().setTrafficDebugStatus(status);
  } catch (e) {
    console.warn('[loadTrafficDebugStatus] failed:', (e as Error).message);
  }
}

export async function setTrafficDebugConfig(config: {
  enabled?: boolean;
  includeTx?: boolean;
  includeRx?: boolean;
  sampleRate?: number;
}): Promise<void> {
  if (!client) throw new Error('Not connected');
  const params: Record<string, unknown> = {};
  if (config.enabled !== undefined) params.enabled = config.enabled;
  if (config.includeTx !== undefined) params.include_tx = config.includeTx;
  if (config.includeRx !== undefined) params.include_rx = config.includeRx;
  if (config.sampleRate !== undefined) params.sample_rate = config.sampleRate;
  
  await client.rpc('bramble.setTrafficDebug', params);
  await loadTrafficDebugStatus();
}

function decodePacketType(pktType: number | string | undefined): string {
  if (typeof pktType === 'string') return pktType;
  switch (pktType) {
    case 0x01: return 'data';
    case 0x02: return 'ack';
    case 0x03: return 'rreq';
    case 0x04: return 'rrep';
    case 0x05: return 'beacon';
    case 0x06: return 'rerr';
    case 0x07: return 'key_exchange';
    case 0x08: return 'congestion';
    case 0x09: return 'timesync';
    case 0x0A: return 'channel_data';
    case 0x0B: return 'channel_ack';
    case 0x0C: return 'probe';
    case 0x0D: return 'probe_ack';
    case 0x0E: return 'location';
    case 0x0F: return 'mailbox_offer';
    case 0x10: return 'mailbox_fetch';
    case 0x11: return 'mailbox_data';
    case 0x12: return 'broadcast_probe';
    case 0x13: return 'broadcast_ack';
    default: return 'unknown';
  }
}

function estimateAirtimeUs(packetLen: number | undefined): number {
  if (!packetLen || packetLen <= 0) return 0;
  // Coarse fallback until firmware reports explicit airtime_debit_us.
  return Math.round(packetLen * 4300);
}

function normalizeTrafficEvent(e: any): any {
  const ts = e.timestamp_ms ?? e.timestampMs;
  return {
    seq: e.seq,
    timestampMs: ts && ts > 0 ? ts : Date.now(),
    direction: e.direction ?? (e.is_tx ? 'tx' : 'rx'),
    category: e.category ?? 'other',
    packetType: decodePacketType(e.packet_type ?? e.packetType ?? e.pkt_type),
    tier: e.tier ?? e.airtime_tier ?? 'normal',
    airtimeBucket: e.airtime_bucket ?? e.airtimeBucket ?? e.airtime_tier ?? 'normal',
    airtimeDebitUs: e.airtime_debit_us ?? e.airtimeDebitUs ?? estimateAirtimeUs(e.packet_len ?? e.packet_len_bytes ?? e.packetLen),
    queueDepth: e.queue_depth ?? e.queueDepth,
    rssi: e.rssi,
    snr: e.snr,
  };
}

export async function loadTrafficEvents(sinceSeq?: number): Promise<void> {
  if (!client) return;
  try {
    const params: Record<string, unknown> = { limit: 500 };
    if (sinceSeq !== undefined) params.since_seq = sinceSeq;
    const result = await client.rpc<{ events: any[] }>('bramble.getTrafficEvents', params);
    const events = (result.events ?? []).map(normalizeTrafficEvent);
    useStore.getState().addTrafficEvents(events);
  } catch (e) {
    console.warn('[loadTrafficEvents] failed:', (e as Error).message);
  }
}

function handleTrafficEvent(params: unknown): void {
  const event = normalizeTrafficEvent(params as any);
  useStore.getState().addTrafficEvent(event);
}

export function getClient(): BrambleClient | null {
  return client;
}
