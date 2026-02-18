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

export async function initMessageStore(): Promise<void> {
  try {
    await messageDb.open();
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
    client.subscribe('probe.ack', (params) => handleProbeAck(params));
    client.subscribe('probe.complete', (params) => handleProbeComplete(params));
    client.subscribe('location.update', (params) => handleLocationUpdate(params));

    // Initial data load — all best-effort so a slow RPC doesn't kill the connection
    const opt = (p: Promise<void>) => p.catch((e) => console.warn('[init]', e.message));
    await Promise.all([
      opt(loadConfig()),
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
      pubkeyHash: raw.identity?.pubkeyHash ?? 0,
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
    freeHeapBytes: raw.freeHeapBytes ?? 0,
    fwVersion: raw.firmware_version ?? raw.fwVersion ?? '',
    txCount: raw.packets_tx ?? raw.txCount ?? 0,
    rxCount: raw.packets_rx ?? raw.rxCount ?? 0,
    droppedCount: raw.droppedCount ?? 0,
    neighborCount: raw.peers ?? raw.neighborCount ?? 0,
    routeCount: raw.routeCount ?? 0,
    airtimeUsedMs: raw.airtimeUsedMs ?? 0,
    position: raw.position,
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
  return {
    tiers: [
      { name: 'critical', remainingMs: raw.critical_remaining_ms ?? 0, maxMs: raw.critical_max_ms ?? 36000, usedPct: 0, refillAtMs: 0 },
      { name: 'normal', remainingMs: raw.normal_remaining_ms ?? 0, maxMs: raw.normal_max_ms ?? 18000, usedPct: 0, refillAtMs: 0 },
      { name: 'broadcast', remainingMs: raw.broadcast_remaining_ms ?? 0, maxMs: raw.broadcast_max_ms ?? 18000, usedPct: 0, refillAtMs: 0 },
    ].map(t => ({ ...t, usedPct: t.maxMs > 0 ? Math.round(100 * (t.maxMs - t.remainingMs) / t.maxMs) : 0 })) as [AirtimeTier, AirtimeTier, AirtimeTier],
  };
}

export async function loadAirtime(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<any>('bramble.getAirtime');
  useStore.getState().setAirtime(normalizeAirtime(result));
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function normalizeNeighbor(raw: any): Neighbor & { name?: string } {
  return {
    addr: typeof raw.address === 'string' ? parseInt(raw.address, 16) : (raw.addr ?? 0),
    rssi: raw.rssi ?? 0,
    snr: raw.snr ?? 0,
    deliveryRate: raw.deliveryRate ?? 0,
    lastHeardMs: raw.last_seen_ms ?? raw.lastHeardMs ?? 0,
    isMailbox: raw.isMailbox ?? false,
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
  for (const m of result.messages ?? []) {
    const fromAddr = typeof m.from === 'string' ? parseInt(m.from, 16) : (m.from ?? 0);
    const toAddr = typeof m.to === 'string' ? parseInt(m.to, 16) : (m.to ?? 0);
    const dir = (m as any).direction;
    const isOutgoing = dir === 'outgoing' || dir === 'broadcast_out';
    const isBroadcast = dir === 'broadcast_in' || dir === 'broadcast_out';
    const fwMsg: Message = {
      id: m.msgId ?? `fw-${(m as any).timestamp_s ?? Date.now()}-${fromAddr}`,
      direction: isOutgoing ? 'outgoing' : 'incoming',
      from: fromAddr,
      to: isBroadcast ? 0xFFFFFFFF : toAddr,
      text: m.text,
      tier: m.tier,
      channelIndex: isBroadcast ? undefined : m.channelIndex,
      timestampMs: ((m as any).timestamp_s ?? m.timestamp ?? 0) * 1000,
      status: 'delivered',
    };
    /* Check if we already have a cached version (from IndexedDB) with relay path data.
     * Match by text + timestamp proximity since firmware IDs differ from webapp IDs. */
    const existing = store.messages.find(
      ex => ex.text === fwMsg.text && Math.abs(ex.timestampMs - fwMsg.timestampMs) < 5000
    );
    if (existing?.relayPath) continue; // keep cached version with relay path
    store.addMessage(fwMsg);
  }
}

// ─── Messaging ────────────────────────────────────────────────────────────

const packetIdToMsgId = new Map<string, string>();

export async function sendMessage(
  dest: number,
  text: string,
  tier: MessageTier = 'normal',
  channelIndex?: number
): Promise<void> {
  if (!client) throw new Error('Not connected');
  const store = useStore.getState();

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
    const isBroadcast = dest === 0xFFFFFFFF;
    const method = isBroadcast ? 'bramble.sendBroadcast' : 'bramble.sendMessage';
    const params = isBroadcast
      ? { text }
      : { dest: dest.toString(16).toUpperCase().padStart(8, '0'), text };
    const result = await client.rpc<{ message_id?: string; status?: string; packetId?: string }>(method, params);
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

function handleIncomingMessage(params: unknown): void {
  const p = params as any;
  const fromAddr = typeof p.from === 'string' ? parseInt(p.from, 16) : (p.from ?? 0);
  const toAddr = typeof p.to === 'string' ? parseInt(p.to, 16) : (p.to ?? 0);
  const msg = {
    id: p.msgId ?? `rt-${Date.now()}`,
    direction: 'incoming' as const,
    from: fromAddr,
    to: toAddr,
    text: p.text,
    tier: p.tier,
    channelIndex: p.channelIndex ?? (p.channel as number | undefined),
    timestampMs: Date.now(),
    status: 'delivered' as const,
  };
  useStore.getState().addMessage(msg);
  messageDb.saveMessage(msg).catch(() => {});
}

async function refreshNeighbors(): Promise<void> {
  await loadNeighbors();
}

// ─── Config mutations ────────────────────────────────────────────────────

export async function saveRadio(radio: import('../types/bramble').RadioConfig): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.setRadio', radio as unknown as Record<string, unknown>);
  await loadConfig();
}

export async function saveNodeName(name: string): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.setNodeName', { name });
  await loadConfig();
}

export async function addChannel(name: string, psk?: string): Promise<number> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc<{ index: number }>('bramble.addChannel', {
    name,
    ...(psk ? { psk } : {}),
  });
  await loadConfig();
  return result.index;
}

export async function removeChannel(index: number): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.removeChannel', { index });
  await loadConfig();
}

export async function setMailbox(enabled: boolean): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.setMailbox', { enabled });
  await loadConfig();
}

export async function setDefaultChannel(index: number): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.setDefaultChannel', { index });
  await loadConfig();
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

export async function sendProbe(): Promise<void> {
  if (!client) throw new Error('Not connected');
  const store = useStore.getState();

  const raw = await client.rpc<Record<string, unknown>>('bramble.sendProbe');
  /* Firmware returns probe_id (snake_case hex string), no ackWindow */
  const probeIdStr = (raw.probeId ?? raw.probe_id) as string | undefined;
  const probeId = probeIdStr ? parseInt(probeIdStr, 16) : Math.floor(Math.random() * 0xFFFFFFFF);
  const ackWindow = (raw.ackWindow ?? raw.ack_window ?? 30) as number;
  store.setProbeResult({
    probeId,
    sentAt: Date.now(),
    ackWindow,
    responses: [],
    complete: false,
  });
  store.setProbeCollecting(true);
}

function handleProbeAck(params: unknown): void {
  const ack = params as ProbeResponse;
  const store = useStore.getState();
  const prev = store.probeResult;
  if (!prev || prev.complete) return;
  store.setProbeResult({
    ...prev,
    responses: [...prev.responses, { ...ack, receivedAt: Date.now() }],
  });
}

function handleProbeComplete(params: unknown): void {
  const { probeId } = params as { probeId: number };
  const store = useStore.getState();
  const prev = store.probeResult;
  if (!prev || prev.probeId !== probeId) return;
  store.setProbeResult({ ...prev, complete: true });
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
  await client.rpc('bramble.setLocationConfig', config as unknown as Record<string, unknown>);
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
  await client.rpc('bramble.setLocationContact', params);
  await loadConfig();
}

export async function removeLocationContact(addr: number): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.removeLocationContact', { addr });
  await loadConfig();
}

export async function shareLocationOnce(addr: number, tier?: LocationTier): Promise<void> {
  if (!client) throw new Error('Not connected');
  const params: Record<string, unknown> = { addr };
  if (tier !== undefined) params.tier = tier;
  await client.rpc('bramble.shareLocationOnce', params);
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

export function getClient(): BrambleClient | null {
  return client;
}
