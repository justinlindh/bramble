import { useStore } from './index';
import { createTransport, BrambleClient } from '../transport';
import { messageDb } from './messageDb';
import type {
  TransportType,
  BrambleConfig,
  NodeStatus,
  AirtimeStatus,
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

    // Initial data load
    await Promise.all([
      loadConfig(),
      loadStatus(),
      loadAirtime(),
      loadNeighbors(),
      loadRoutes(),
      loadMessages(),
      loadPeerLocations(),
    ]);
  } catch (e) {
    // Clean up any partially-initialised client so we start fresh on retry
    client?.clearSubscriptions();
    client = null;
    store.setConnectionState('error', friendlyError((e as Error).message));
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

export async function loadNeighbors(): Promise<void> {
  if (!client) return;
  const result = await client.rpc<{ neighbors: Neighbor[] }>('bramble.getNeighbors');
  useStore.getState().setNeighbors(result.neighbors ?? []);
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
    params
  );
  const store = useStore.getState();
  for (const m of result.messages ?? []) {
    store.addMessage({
      id: m.msgId,
      direction: 'incoming',
      from: m.from,
      to: m.to,
      text: m.text,
      tier: m.tier,
      channelIndex: m.channelIndex,
      timestampMs: m.timestamp * 1000,
      status: 'delivered',
    });
  }
}

// ─── Messaging ────────────────────────────────────────────────────────────

const packetIdToMsgId = new Map<number, string>();

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
    const result = await client.rpc<{ packetId: number }>('bramble.sendMessage', {
      dest,
      text,
      tier,
      ...(channelIndex !== undefined ? { channelIndex } : {}),
    });
    store.updateMessageStatus(msg.id, 'sent');
    messageDb.updateMessageStatus(msg.id, 'sent').catch(() => {});
    if (result?.packetId !== undefined) {
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
  const { packetId, status, relayPath } = params as {
    packetId: number;
    status: string;
    relayPath?: RelayHop[];
  };
  const msgId = packetIdToMsgId.get(packetId);
  if (msgId) {
    packetIdToMsgId.delete(packetId);
    const newStatus = status === 'delivered' ? 'delivered' : 'failed';
    useStore.getState().updateMessageStatus(msgId, newStatus, relayPath);
    messageDb.updateMessageStatus(msgId, newStatus, relayPath).catch(() => {});
  }
}

function handleIncomingMessage(params: unknown): void {
  const p = params as IncomingMessage;
  const msg = {
    id: p.msgId,
    direction: 'incoming' as const,
    from: p.from,
    to: p.to,
    text: p.text,
    tier: p.tier,
    channelIndex: p.channelIndex,
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

// ─── Probe / Network Reach ────────────────────────────────────────────────

export async function sendProbe(): Promise<void> {
  if (!client) throw new Error('Not connected');
  const store = useStore.getState();

  const result = await client.rpc<{ probeId: number; ackWindow: number }>('bramble.sendProbe');
  store.setProbeResult({
    probeId: result.probeId,
    sentAt: Date.now(),
    ackWindow: result.ackWindow,
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
