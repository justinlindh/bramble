// Read-side telemetry: status, airtime, neighbors, routes, peer locations,
// and the traffic monitor (debug status, event decode, live event feed).
import { session, parseHexAddr } from './client';
import { useStore } from '../index';
import type { NodeStatus, AirtimeStatus, AirtimeTier, Neighbor, Route, PeerLocation } from '../../types/bramble';

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function normalizeStatus(raw: any): NodeStatus {
  return {
    uptimeSec: raw.uptime_s ?? raw.uptimeSec ?? 0,
    freeHeapBytes: raw.free_heap ?? raw.freeHeapBytes ?? 0,
    fwVersion: raw.firmware_version ?? raw.fwVersion ?? '',
    txCount: raw.packets_tx ?? raw.txCount ?? 0,
    rxCount: raw.packets_rx ?? raw.rxCount ?? 0,
    droppedCount: raw.dropped_count ?? raw.droppedCount ?? 0,
    neighborCount: raw.peers ?? raw.neighborCount ?? 0,
    routeCount: raw.route_count ?? raw.routeCount ?? 0,
    airtimeUsedMs: raw.airtime_used_ms ?? raw.air_used_ms ?? raw.airtimeUsedMs ?? 0,
    position: raw.position,
    gpsAvailable: raw.gps_available ?? raw.gpsAvailable ?? false,
    batteryMv: raw.battery_mv ?? raw.batteryMv,
    batteryPct: raw.battery_pct ?? raw.batteryPct,
    hardware: raw.hardware,
  } as NodeStatus;
}

export async function loadStatus(): Promise<void> {
  if (!session.client) return;
  const result = await session.client.rpc<any>('bramble.getStatus');
  useStore.getState().setStatus(normalizeStatus(result));
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function normalizeAirtime(raw: any): AirtimeStatus {
  // Firmware returns flat fields; webapp expects { tiers: [...] }
  if (raw.tiers) return raw as AirtimeStatus;

  // next_refill_ms is a duration (ms until next refill). 0 means "just refilled",
  // treat it as a full interval from now. Default to 1 hour if missing.
  const nextRefillMs = raw.next_refill_ms ?? 3600000;
  const refillAtMs = Date.now() + (nextRefillMs > 0 ? nextRefillMs : REFILL_INTERVAL_MS);

  const tiers: AirtimeTier[] = [
    { name: 'critical', remainingMs: raw.critical_remaining_ms ?? 0, maxMs: raw.critical_max_ms ?? 36000, usedPct: 0, refillAtMs },
    { name: 'normal', remainingMs: raw.normal_remaining_ms ?? 0, maxMs: raw.normal_max_ms ?? 18000, usedPct: 0, refillAtMs },
    { name: 'broadcast', remainingMs: raw.broadcast_remaining_ms ?? 0, maxMs: raw.broadcast_max_ms ?? 18000, usedPct: 0, refillAtMs },
  ];
  // The receipt lane (PR #82, firmware getAirtime) only appears when the
  // firmware reports it; older firmware omits it and we keep three lanes.
  if (raw.receipt_max_ms !== undefined || raw.receipt_remaining_ms !== undefined) {
    tiers.push({ name: 'receipt', remainingMs: raw.receipt_remaining_ms ?? 0, maxMs: raw.receipt_max_ms ?? 12000, usedPct: 0, refillAtMs });
  }
  return {
    tiers: tiers.map(t => ({ ...t, usedPct: t.maxMs > 0 ? Math.round(100 * (t.maxMs - t.remainingMs) / t.maxMs) : 0 })),
  };
}

const REFILL_INTERVAL_MS = 3600000;

export async function loadAirtime(): Promise<void> {
  if (!session.client) return;
  const result = await session.client.rpc<any>('bramble.getAirtime');
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
    airtimeRemaining: raw.airtimeRemaining ?? 0,
    ...(raw.name ? { name: raw.name } : {}),
  } as Neighbor & { name?: string };
}

export async function loadNeighbors(): Promise<void> {
  if (!session.client) return;
  const result = await session.client.rpc<{ neighbors: any[] }>('bramble.getNeighbors');
  useStore.getState().setNeighbors((result.neighbors ?? []).map(normalizeNeighbor));
}

export async function loadRoutes(): Promise<void> {
  if (!session.client) return;
  const result = await session.client.rpc<{ routes: Route[] }>('bramble.getRoutes');
  useStore.getState().setRoutes(result.routes ?? []);
}

export async function refreshNeighbors(): Promise<void> {
  await loadNeighbors();
}

export function showOnMap(addr: number): void {
  const store = useStore.getState();
  store.setMapFocusAddr(addr);
  store.setActiveTab('map');
}

// ─── Location ─────────────────────────────────────────────────────────────

function normalizePeerLocation(raw: any): PeerLocation {
  const addr = parseHexAddr(raw?.addr ?? raw?.address);
  const rawPos = raw?.position;
  const position = rawPos
    ? {
        lat: Number(rawPos.lat ?? rawPos.latitude ?? 0),
        lon: Number(rawPos.lon ?? rawPos.longitude ?? 0),
        alt: Number(rawPos.alt ?? rawPos.altitude ?? 0),
        accuracy: Number(rawPos.accuracy ?? 0),
        speed: Number(rawPos.speed ?? 0),
        heading: Number(rawPos.heading ?? 0),
        timestampMs: Number(rawPos.timestampMs ?? rawPos.timestamp_ms ?? Date.now()),
      }
    : null;

  return {
    addr,
    name: String(raw?.name ?? raw?.node_name ?? ''),
    tier: (raw?.tier ?? 'presence') as PeerLocation['tier'],
    position,
    gridSquare: raw?.gridSquare ?? raw?.grid_square,
    online: Boolean(raw?.online ?? true),
    lastUpdatedMs: Number(raw?.lastUpdatedMs ?? raw?.last_updated_ms ?? Date.now()),
  };
}

export async function loadPeerLocations(): Promise<void> {
  if (!session.client) return;
  const result = await session.client.rpc<{ peerLocations: any[] }>('bramble.getPeerLocations');
  const normalized = (result.peerLocations ?? []).map(normalizePeerLocation);
  useStore.getState().setPeerLocations(normalized);
}

export function handleLocationUpdate(params: unknown): void {
  const update = normalizePeerLocation(params as any);
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
  if (!session.client) return;
  try {
    const result = await session.client.rpc<any>('bramble.getTrafficDebug');
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
  if (!session.client) throw new Error('Not connected');
  const params: Record<string, unknown> = {};
  if (config.enabled !== undefined) params.enabled = config.enabled;
  if (config.includeTx !== undefined) params.include_tx = config.includeTx;
  if (config.includeRx !== undefined) params.include_rx = config.includeRx;
  if (config.sampleRate !== undefined) params.sample_rate = config.sampleRate;
  
  await session.client.rpc('bramble.setTrafficDebug', params);
  await loadTrafficDebugStatus();
}

export function decodePacketType(pktType: number | string | undefined): string {
  if (typeof pktType === 'string') return pktType;
  switch (pktType) {
    case 0x01: return 'ack';
    case 0x02: return 'rreq';
    case 0x03: return 'rrep';
    case 0x04: return 'rerr';
    case 0x05: return 'beacon';
    case 0x06: return 'key_exchange';
    case 0x07: return 'delivery_receipt';
    case 0x08: return 'congestion';
    case 0x09: return 'time_sync';
    case 0x0A: return 'data';
    case 0x0B: return 'store_request';
    case 0x0C: return 'store_ack';
    case 0x0D: return 'mailbox_delivery';
    case 0x0E: return 'mailbox_query';
    case 0x0F: return 'emergency';
    case 0x10: return 'emergency_cancel';
    case 0x11: return 'coded';
    case 0x12: return 'probe';
    case 0x13: return 'probe_ack';
    case 0x14: return 'location';
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
  if (!session.client) return;
  try {
    const params: Record<string, unknown> = { limit: 500 };
    if (sinceSeq !== undefined) params.since_seq = sinceSeq;
    const result = await session.client.rpc<{ events: any[] }>('bramble.getTrafficEvents', params);
    const events = (result.events ?? []).map(normalizeTrafficEvent);
    useStore.getState().addTrafficEvents(events);
  } catch (e) {
    console.warn('[loadTrafficEvents] failed:', (e as Error).message);
  }
}

export function handleTrafficEvent(params: unknown): void {
  const event = normalizeTrafficEvent(params as any);
  useStore.getState().addTrafficEvent(event);
}
