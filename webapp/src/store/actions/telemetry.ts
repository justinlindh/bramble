// Read-side telemetry: status, airtime, neighbors, routes, peer locations,
// and the traffic monitor (debug status, event decode, live event feed).
import { session, requireClient } from './client';
import { parseAddr } from '../../lib/addr';
import { useStore } from '../index';
import type { NodeStatus, AirtimeStatus, AirtimeTier, Neighbor, Route, PeerLocation, TrafficEvent, TrafficDebugStatus } from '../../types/bramble';
import type { RpcSchemas, WirePartial } from '../../types/rpc';

// Wire types: the contract schema made deep-optional plus the legacy key
// spellings older firmware has used. See types/rpc.ts for the rationale.
type StatusWire = WirePartial<RpcSchemas['StatusResponse']> & {
  uptimeSec?: number;
  freeHeapBytes?: number;
  fwVersion?: string;
  txCount?: number;
  rxCount?: number;
  dropped_count?: number;
  droppedCount?: number;
  neighborCount?: number;
  route_count?: number;
  routeCount?: number;
  airtime_used_ms?: number;
  air_used_ms?: number;
  airtimeUsedMs?: number;
  gpsAvailable?: boolean;
  gpsEnabled?: boolean;
  batteryMv?: number;
  batteryPct?: number;
  position?: NodeStatus['position'];
};

export function normalizeStatus(raw: StatusWire): NodeStatus {
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
    gpsEnabled: raw.gps_enabled ?? raw.gpsEnabled ?? true,
    batteryMv: raw.battery_mv ?? raw.batteryMv,
    batteryPct: raw.battery_pct ?? raw.batteryPct,
    // Both fields are optional on the wire for older-firmware compatibility.
    // Absent means unknown/undefined, never fabricate "no" or "not present".
    charging: raw.charging,
    present: raw.present,
    hardware: raw.hardware,
  } as NodeStatus;
}

export async function loadStatus(): Promise<void> {
  if (!session.client) return;
  const result = await session.client.rpc('bramble.getStatus');
  useStore.getState().setStatus(normalizeStatus(result));
}

// `tiers` is the already-normalized shape some callers pass straight through;
// the normalizer only checks its presence before returning the input as-is,
// so its element shape is not inspected here.
type AirtimeWire = WirePartial<RpcSchemas['AirtimeResponse']> & {
  tiers?: unknown[];
};

export function normalizeAirtime(raw: AirtimeWire): AirtimeStatus {
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
  const result = await session.client.rpc('bramble.getAirtime');
  useStore.getState().setAirtime(normalizeAirtime(result));
}

type NeighborWire = WirePartial<RpcSchemas['Neighbor']> & {
  addr?: number;
  lastHeardMs?: number;
};

function normalizeNeighbor(raw: NeighborWire): Neighbor & { name?: string } {
  return {
    addr: parseAddr(raw.address ?? raw.addr),
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
  const result = await session.client.rpc('bramble.getNeighbors');
  useStore.getState().setNeighbors((result.neighbors ?? []).map(normalizeNeighbor));
}

export async function loadRoutes(): Promise<void> {
  if (!session.client) return;
  const result = await session.client.rpc<{ routes: Route[] }>('bramble.getRoutes');
  useStore.getState().setRoutes(result.routes ?? []);
}

export function showOnMap(addr: number): void {
  const store = useStore.getState();
  store.setMapFocusAddr(addr);
  store.setActiveTab('map');
}

// ─── Location ─────────────────────────────────────────────────────────────

type LocationPeerWire = WirePartial<Omit<RpcSchemas['LocationPeer'], 'position'>> & {
  address?: string | number;
  node_name?: string;
  gridSquare?: string;
  grid_square?: string;
  last_updated_ms?: number;
  position?:
    | (WirePartial<RpcSchemas['Position']> & {
        latitude?: number;
        longitude?: number;
        altitude?: number;
        timestamp_ms?: number;
      })
    | null;
};

function normalizePeerLocation(raw: LocationPeerWire | null | undefined): PeerLocation {
  const addr = parseAddr(raw?.addr ?? raw?.address);
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
  const result = await session.client.rpc('bramble.getPeerLocations');
  const normalized = (result.peerLocations ?? []).map(normalizePeerLocation);
  useStore.getState().setPeerLocations(normalized);
}

export function handleLocationUpdate(params: unknown): void {
  const update = normalizePeerLocation(params as LocationPeerWire);
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

// The generated GetTrafficDebugResponse collapses to `Record<string, never> &
// TrafficDebugStatus` (an allOf artifact), which types every property as
// never, so this wire type is anchored on the TrafficDebugConfig schema plus
// the status fields and legacy spellings the firmware has sent.
type TrafficDebugWire = WirePartial<RpcSchemas['TrafficDebugConfig']> & {
  includeTx?: boolean;
  includeRx?: boolean;
  sampleRate?: number;
  ring_size?: number;
  ringSize?: number;
  ring_used?: number;
  ringUsed?: number;
  dropped_count?: number;
  droppedCount?: number;
  last_seq?: number;
  lastSeq?: number;
  buffer_capacity?: number;
  buffer_count?: number;
};

export async function loadTrafficDebugStatus(): Promise<void> {
  if (!session.client) return;
  try {
    const result = await session.client.rpc<TrafficDebugWire>('bramble.getTrafficDebug');
    const status: TrafficDebugStatus = {
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
  const client = requireClient();
  const params: Record<string, unknown> = {};
  if (config.enabled !== undefined) params.enabled = config.enabled;
  if (config.includeTx !== undefined) params.include_tx = config.includeTx;
  if (config.includeRx !== undefined) params.include_rx = config.includeRx;
  if (config.sampleRate !== undefined) params.sample_rate = config.sampleRate;
  
  await client.rpc('bramble.setTrafficDebug', params);
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

type TrafficEventWire = RpcSchemas['TrafficEvent'] & {
  timestampMs?: number;
  direction?: string;
  packet_type?: number | string;
  packetType?: number | string;
  tier?: string;
  airtime_bucket?: string;
  airtimeBucket?: string;
  airtime_debit_us?: number;
  airtimeDebitUs?: number;
  packet_len_bytes?: number;
  packetLen?: number;
  queue_depth?: number;
  queueDepth?: number;
  snr?: number;
};

function normalizeTrafficEvent(e: TrafficEventWire): TrafficEvent {
  const ts = e.timestamp_ms ?? e.timestampMs;
  // The contract enums for category and airtime_tier are wider than the
  // webapp's TrafficCategory/MessageTier unions ('unknown', 'none'); the
  // pre-typed version of this normalizer passed such values through, so the
  // final cast preserves that rather than silently rewriting them.
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
  } as TrafficEvent;
}

export async function loadTrafficEvents(sinceSeq?: number): Promise<void> {
  if (!session.client) return;
  try {
    const params: Record<string, unknown> = { limit: 500 };
    if (sinceSeq !== undefined) params.since_seq = sinceSeq;
    const result = await session.client.rpc<{ events: TrafficEventWire[] }>('bramble.getTrafficEvents', params);
    const events = (result.events ?? []).map(normalizeTrafficEvent);
    useStore.getState().addTrafficEvents(events);
  } catch (e) {
    console.warn('[loadTrafficEvents] failed:', (e as Error).message);
  }
}

export function handleTrafficEvent(params: unknown): void {
  const event = normalizeTrafficEvent(params as TrafficEventWire);
  useStore.getState().addTrafficEvents([event]);
}
