import { useStore } from './index';
import { createTransport, BrambleClient } from '../transport';
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

let client: BrambleClient | null = null;

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

export async function connect(type: TransportType): Promise<void> {
  const store = useStore.getState();
  store.setConnectionState('connecting');
  try {
    const transport = createTransport(type);
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
    store.setConnectionState('error', (e as Error).message);
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

  try {
    const result = await client.rpc<{ packetId: number }>('bramble.sendMessage', {
      dest,
      text,
      tier,
      ...(channelIndex !== undefined ? { channelIndex } : {}),
    });
    store.updateMessageStatus(msg.id, 'sent');
    if (result?.packetId !== undefined) {
      packetIdToMsgId.set(result.packetId, msg.id);
    }
  } catch (e) {
    store.updateMessageStatus(msg.id, 'failed');
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
    useStore.getState().updateMessageStatus(
      msgId,
      status === 'delivered' ? 'delivered' : 'failed',
      relayPath
    );
  }
}

function handleIncomingMessage(params: unknown): void {
  const p = params as IncomingMessage;
  useStore.getState().addMessage({
    id: p.msgId,
    direction: 'incoming',
    from: p.from,
    to: p.to,
    text: p.text,
    tier: p.tier,
    channelIndex: p.channelIndex,
    timestampMs: Date.now(),
    status: 'delivered',
  });
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

export async function shareLocationOnce(addr: number): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.shareLocationOnce', { addr });
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
