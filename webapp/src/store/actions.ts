import { useStore } from './index';
import { createTransport, BrambleClient } from '../transport';
import { messageDb } from './messageDb';
import { deliveryEventStore, type DeliveryEventRecord } from './deliveryEventStore';
import { fetchConnectionCapabilities } from '../lib/connectionMode';
import { listDevices, forgetDevice, renameDevice, upsertDevice, setDeviceToken } from '../lib/deviceBook';
import { formatAddrHex } from '../utils/address';
import { parseAddr } from '../lib/addr';
import { isAndroidShell } from '../utils/platform';
import { friendlyErrorFrom } from '../lib/errors';
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
  LocationTier,
  Message,
} from '../types/bramble';

let client: BrambleClient | null = null;
const LAST_NODE_ADDR_KEY = 'bramble:last-node-addr';

function readLastKnownNodeAddrHex(): string | undefined {
  try {
    const raw = localStorage.getItem(LAST_NODE_ADDR_KEY);
    return raw ? raw.toUpperCase() : undefined;
  } catch {
    return undefined;
  }
}

export async function loadConnectionCapabilities(): Promise<void> {
  const capabilities = await fetchConnectionCapabilities();
  useStore.getState().setConnectionCapabilities(capabilities);
}

// ─── Message persistence ─────────────────────────────────────────────────

export async function initMessageStore(nodeAddr?: string): Promise<void> {
  try {
    await Promise.all([
      messageDb.open(nodeAddr),
      deliveryEventStore.open(nodeAddr),
    ]);

    await deliveryEventStore.pruneOldEvents(retentionCutoffTs());

    const cached = await messageDb.getMessages();
    const hydrated = cached.length > 0 ? await hydrateMessagesWithDeliveryEvents(cached) : [];
    hydrateCorrelationMaps(hydrated);
    if (hydrated.length > 0) {
      useStore.getState().loadCachedMessages(hydrated);
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

const SERIAL_RPC_READY_ATTEMPTS = 8;
const SERIAL_RPC_READY_TIMEOUT_MS = 1500;
const SERIAL_RPC_READY_RETRY_DELAY_MS = 350;

function isUnknownMethodError(error: unknown): boolean {
  const message = (error as Error)?.message ?? '';
  return /not\s+found|unknown\s+method|method\s+not\s+found/i.test(message);
}

async function probeRpcReadiness(): Promise<void> {
  if (!client) throw new Error('Not connected');
  try {
    await client.rpc('bramble.ping', undefined, SERIAL_RPC_READY_TIMEOUT_MS);
    return;
  } catch (error) {
    if (!isUnknownMethodError(error)) throw error;
  }
  await client.rpc('bramble.getStatus', undefined, SERIAL_RPC_READY_TIMEOUT_MS);
}

const NODE_VERIFY_ATTEMPTS = 2;

// Confirm the freshly opened transport actually speaks Bramble before we report
// "Connected". A socket that opens but is not a Bramble node (a wrong IP/port
// that happens to host a WebSocket) would otherwise sit in a permanent empty
// Connected state while every RPC times out and the client reconnects forever
// (issue #91). ping/getStatus is on the unauthenticated allowlist, so this also
// works against an auth-required node.
async function verifyBrambleNode(): Promise<boolean> {
  for (let attempt = 1; attempt <= NODE_VERIFY_ATTEMPTS; attempt += 1) {
    try {
      await probeRpcReadiness();
      return true;
    } catch (error) {
      // An auth-required node answers the allowlisted ping, so a 1008/auth error
      // here means a real node we simply cannot fully use yet: treat it as
      // reachable and let the init RPCs surface the auth-required state.
      if (/1008|unauthorized|auth/i.test((error as Error)?.message ?? '')) return true;
      if (attempt < NODE_VERIFY_ATTEMPTS) {
        await new Promise(r => setTimeout(r, SERIAL_RPC_READY_RETRY_DELAY_MS));
      }
    }
  }
  return false;
}

async function ensureSerialRpcReady(): Promise<boolean> {
  let lastError: unknown;
  for (let attempt = 1; attempt <= SERIAL_RPC_READY_ATTEMPTS; attempt += 1) {
    try {
      await probeRpcReadiness();
      return true;
    } catch (error) {
      lastError = error;
      if (attempt < SERIAL_RPC_READY_ATTEMPTS) {
        await new Promise(r => setTimeout(r, SERIAL_RPC_READY_RETRY_DELAY_MS));
      }
    }
  }

  console.warn(`[serial-rpc] readiness probe exhausted: ${((lastError as Error)?.message ?? 'startup timeout')}`);
  return false;
}

export async function connect(
  type: TransportType,
  options?: {
    url?: string; token?: string; ip?: string; remember?: boolean; name?: string; expectAddressHex?: string;
    /** Pick-first flow: a device already chosen via BLETransport.pickDevice(). */
    bleDevice?: BluetoothDevice;
  },
): Promise<void> {
  const store = useStore.getState();

  // Guard against duplicate/re-entrant connects creating multiple active WS clients.
  if (client) {
    try { client.clearSubscriptions(); } catch { /* noop */ }
    try { await client.disconnect(); } catch { /* noop */ }
    client = null;
  }

  store.setManualDisconnect(false);
  store.setConnectionState('connecting');
  try {
    const transport = createTransport(type, options);
    await transport.connect();
    client = new BrambleClient(transport);
    store.setTransport(transport);

    // Verify the endpoint speaks Bramble before declaring Connected (issue #91).
    // Serial is a trusted physical link and keeps its existing best-effort
    // readiness flow below, so we only gate network transports here.
    if (type !== 'serial') {
      const reachable = await verifyBrambleNode();
      if (!reachable) {
        throw new Error('Endpoint is not a Bramble node');
      }
    }

    // Fail closed on missing or wrong auth. verifyBrambleNode only proves the
    // endpoint is a Bramble node: it uses the allowlisted getVersion, which an
    // auth-required node answers even to an unauthenticated client. Without
    // this probe a blank BLE/WiFi token would look Connected and then silently
    // reject every real RPC (issue: BLE connects with no token then Unauthorized).
    // A non-allowlisted RPC returns Unauthorized when the session did not
    // authenticate (wrong WiFi token closes 1008 earlier; a wrong BLE token is
    // rejected during the transport handshake, so this specifically catches the
    // no-token-on-an-auth-required-node case). Serial is a trusted link.
    if (type === 'ble' || type === 'wifi') {
      // For BLE WITH a token, the transport handshake already validated auth
      // (a wrong token rejects there), so a timeout here is a connectivity
      // stall, not an auth problem. Mapping -1005 to auth-required in that
      // case told a user with the CORRECT token that it was wrong.
      const handshakeValidated = type === 'ble' && !!options?.token;
      try {
        await client.rpc('bramble.getStatus', {}, 4000);
      } catch (e) {
        const msg = (e as Error)?.message ?? '';
        const authy = /unauthorized|1008|auth/i.test(msg) || (!handshakeValidated && /-1005/.test(msg));
        if (authy) {
          throw new Error('This node requires an auth token. Enter the token and reconnect.');
        }
        // A handshake-validated session's timeout is a connectivity stall,
        // not an auth problem; a truly wedged link is now killed at the
        // transport layer (write timeout), so let init proceed or fail on
        // its own terms rather than blocking here.
      }
    }

    // Transport is open and (for network transports) verified; reflect Connected.
    store.setConnectionState('connected');

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
            await opt(loadConfig());
            const nodeAddr = useStore.getState().config?.identity?.address;
            const addrHex = nodeAddr
              ? formatAddrHex(nodeAddr)
              : readLastKnownNodeAddrHex();
            await initMessageStore(addrHex);
            await Promise.all([loadNeighbors(), loadRoutes(), loadAirtime()]);
            // Keep loadMessages after initMessageStore so reconnect fetches persist into the right DB namespace.
            await opt(loadMessages());
            await opt(syncDeliveryEventReplay());
          } catch { /* best effort */ }
        },
      });
    }

    // Clear stale data from previous node connection BEFORE subscribing,
    // so early push events aren't wiped by a late reset (BUG-02 fix).
    store.resetNodeData();

    // Subscribe to push events
    client.subscribe('bramble.onMessage', (params) =>
      handleIncomingMessage(params)
    );
    client.subscribe('bramble.onAck', (params) => handleAck(params));
    client.subscribe('bramble.onBroadcastDelivery', (params) => handleBroadcastDelivery(params));
    // A GPS fix acquired mid-session must refresh the map: without this the
    // self position only appears after a manual reload.
    client.subscribe('bramble.onGpsEvent', () => { loadPeerLocations().catch(() => {}); });
    client.subscribe('delivery.update', (params) => handleDeliveryUpdate(params));
    client.subscribe('bramble.onNeighborChange', () => refreshNeighbors());
    client.subscribe('bramble.onRouteUpdate', () => loadRoutes());
    client.subscribe('bramble.onAirtimeWarning', () => loadAirtime());
    client.subscribe('bramble.onProbeResult', (params) => handleProbeAck(params));
    client.subscribe('bramble.onProbeComplete', (params) => handleProbeComplete(params));
    client.subscribe('location.update', (params) => handleLocationUpdate(params));
    client.subscribe('bramble.onPeerLocation', (params) => handleLocationUpdate(params));
    client.subscribe('bramble.onTrafficEvent', (params) => handleTrafficEvent(params));

    // Initial data load — all best-effort so a slow RPC doesn't kill the connection
    const opt = (p: Promise<void>) => p.catch((e) => console.warn('[init]', e.message));

    if (type === 'serial') {
      const rpcReady = await ensureSerialRpcReady();
      if (!rpcReady) {
        console.warn('[serial-rpc] proceeding with best-effort init after readiness timeout');
      }
    }

    // Load config first to get node address for IndexedDB namespacing
    // Retry once if the first attempt fails: the node address namespaces the
    // message DB. Read it from the LIVE store, not the `store` snapshot taken
    // at the top of connect(): loadConfig() replaces the store config, and the
    // stale snapshot still holds the PREVIOUS node's config. Reading it stale
    // namespaced the DB under the old node's address, leaking its messages and
    // DMs into this node's view (the bookAddrNum read below already reads fresh).
    await opt(loadConfig());
    let nodeAddr = useStore.getState().config?.identity?.address;
    if (!nodeAddr) {
      await new Promise(r => setTimeout(r, 500));
      await opt(loadConfig());
      nodeAddr = useStore.getState().config?.identity?.address;
    }
    const configAddrHex = nodeAddr ? formatAddrHex(nodeAddr) : undefined;
    // Persist last-known address so we can recover if config fails on next connect
    if (configAddrHex) {
      try { localStorage.setItem(LAST_NODE_ADDR_KEY, configAddrHex); } catch {}
    }
    const addrHex = configAddrHex ?? readLastKnownNodeAddrHex();

    // Device book: the node's real address is only knowable post-connect. Read it
    // fresh (the captured `store` snapshot predates loadConfig) and, on a TRUTHY
    // address, persist this device. Truthiness guards a partial loadConfig
    // failure that leaves config.identity.address at 0: a 0 address must never
    // create a "00000000" book entry.
    const bookAddrNum = useStore.getState().config?.identity?.address;
    if (bookAddrNum) {
      const bookAddrHex = formatAddrHex(bookAddrNum);
      // DHCP guard: one-click connect carries the address it expects for this
      // saved IP. If the IP now answers as a different node, drop the connection
      // instead of adopting the wrong node or rebinding its lastIp/token.
      if (options?.expectAddressHex && options.expectAddressHex.toUpperCase() !== bookAddrHex) {
        try { client?.clearSubscriptions(); } catch { /* noop */ }
        try { await client?.disconnect(); } catch { /* noop */ }
        client = null;
        store.setTransport(null);
        store.setConnectionState('error', 'That address now belongs to a different node. Check the device and reconnect.');
        return;
      }
      // A book write must never break a live connection.
      try {
        if (type === 'wifi' && options?.ip) {
          saveConnectedDevice({
            addr: bookAddrNum,
            name: options.name,
            ip: options.ip,
            token: options.token ?? '',
            remember: options.remember ?? false,
            transport: 'wifi',
          });
        } else if (type === 'ble') {
          // BLE has no address until after connect (like serial), but unlike
          // serial it needs the auth token, so persist it per the Remember
          // choice. The BLE identity (device id + name) enables zero-prompt
          // reconnect from the device book.
          saveConnectedDevice({
            addr: bookAddrNum,
            name: options?.name,
            ip: '',
            token: options?.token ?? '',
            remember: options?.remember ?? false,
            transport: 'ble',
            bleDeviceId: options?.bleDevice?.id,
            bleDeviceName: options?.bleDevice?.name ?? undefined,
          });
        } else if (type === 'serial') {
          saveConnectedDevice({ addr: bookAddrNum, name: options?.name, ip: '', token: '', remember: false, transport: 'serial' });
        }
      } catch { /* noop */ }
    }

    await initMessageStore(addrHex);

    if (type === 'serial') {
      await opt(loadStatus());
      await opt(loadAirtime());
      await Promise.all([
        opt(loadNeighbors()),
        opt(loadRoutes()),
      ]);
      await Promise.all([
        opt(loadMessages()),
        opt(loadPeerLocations()),
      ]);
    } else {
      await Promise.all([
        opt(loadStatus()),
        opt(loadAirtime()),
        opt(loadNeighbors()),
        opt(loadRoutes()),
        opt(loadMessages()),
        opt(loadPeerLocations()),
      ]);
    }

    await opt(syncDeliveryEventReplay());

    store.setConnectionState('connected');

    // Android shell: hand the live connection to the native notification
    // service so it can open its own authenticated WebSocket. No-op on
    // web and Electron, which have no such bridge.
    if (type === 'wifi' && options?.url && isAndroidShell()) {
      try { window.brambleAndroidNative?.updateConnection(options.url, options.token ?? ''); } catch { /* noop */ }
    }
  } catch (e) {
    // Clean up any partially-initialised client so we start fresh on retry.
    // DISCONNECT the transport, do not just drop the reference: a BLE
    // peripheral stops advertising while a GATT link is open, so a connect
    // that fails after the link is established (the auth probe rejecting, say)
    // would otherwise leave the node connected-but-unusable and invisible to
    // the system picker on the next attempt.
    try { client?.clearSubscriptions(); } catch { /* noop */ }
    try { await client?.disconnect(); } catch { /* noop */ }
    client = null;
    store.setTransport(null);
    // Show the overlay so the user can retry: 'disconnected' shows connect UI.
    store.setConnectionState('disconnected', friendlyErrorFrom(e));
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
  useStore.getState().setManualDisconnect(true);
  useStore.getState().setConnectionState('disconnected');
  useStore.getState().setTransport(null);
}

// ─── Data loading ────────────────────────────────────────────────────────

/**
 * Normalize firmware config response to match BrambleConfig interface.
 * Firmware returns flat structure; webapp expects nested identity/radio objects.
 */
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function normalizeConfig(raw: any): BrambleConfig {
  const rawLocation = raw.location ?? {};
  const legacyContacts = (rawLocation.contacts ?? []) as Array<{ addr: number; tier: LocationTier; intervalSec?: number }>;
  const contactRules = (rawLocation.contact_rules ?? legacyContacts.map((c) => ({
    address: formatAddrHex(c.addr),
    enabled: c.tier !== 'off',
    tier: c.tier,
    interval_s: c.intervalSec ?? rawLocation.interval_s ?? 300,
  }))) as LocationConfig['contact_rules'];

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
    channels: (raw.channels ?? []).map((ch: any) => {
      const candidates = [ch.name, ch.channel_name, ch.channelName];
      const firstNonBlankName = candidates.find((v: unknown) => typeof v === 'string' && v.trim().length > 0) as string | undefined;
      return {
        index: ch.id ?? ch.index ?? 0,
        name: firstNonBlankName ?? '',
        hasPsk: ch.hasPsk ?? ch.has_psk ?? ch.psk_enabled ?? ch.pskEnabled ?? false,
        epoch: ch.epoch ?? ch.key_epoch ?? ch.keyEpoch ?? 0,
        isDefault: ch.is_default ?? ch.isDefault ?? ch.default ?? ch.default_channel ?? ch.defaultChannel ?? false,
      };
    }),
    mailboxEnabled: raw.mailboxEnabled ?? false,
    location: {
      enabled: rawLocation.enabled ?? false,
      tier: rawLocation.tier ?? rawLocation.default_tier ?? 'coarse',
      default_tier: rawLocation.default_tier ?? rawLocation.tier ?? 'coarse',
      interval_s: rawLocation.interval_s ?? 300,
      source: rawLocation.source === 'auto' ? 'hybrid' : (rawLocation.source ?? 'hybrid'),
      lat: rawLocation.lat,
      lon: rawLocation.lon,
      contact_rules: contactRules,
      channel_targets: rawLocation.channel_targets ?? [],
      contacts: legacyContacts,
      defaultIntervalSec: rawLocation.defaultIntervalSec,
      defaultDistanceTriggerM: rawLocation.defaultDistanceTriggerM,
      stationaryBackoff: rawLocation.stationaryBackoff,
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
    droppedCount: raw.dropped_count ?? raw.droppedCount ?? 0,
    neighborCount: raw.peers ?? raw.neighborCount ?? 0,
    routeCount: raw.route_count ?? raw.routeCount ?? 0,
    airtimeUsedMs: raw.airtime_used_ms ?? raw.air_used_ms ?? raw.airtimeUsedMs ?? 0,
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
export function normalizeAirtime(raw: any): AirtimeStatus {
  // Firmware returns flat fields; webapp expects { tiers: [...] }
  if (raw.tiers) return raw as AirtimeStatus;

  // next_refill_ms is a duration (ms until next refill). 0 means "just refilled"
  // — treat it as a full interval from now. Default to 1 hour if missing.
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
  const newFromFirmware: Message[] = [];
  for (const m of result.messages ?? []) {
    const fromAddr = parseAddr(m.from);
    const toAddr = parseAddr(m.to);
    const dir = (m as any).direction;
    const isOutgoing = dir === 'outgoing' || dir === 'broadcast_out';
    const rawChannel = (m as any).channelIndex ?? (m as any).channel;
    const channelIndex = rawChannel !== undefined && rawChannel >= 0 ? rawChannel : undefined;
    const isBroadcast =
      dir === 'broadcast_in' ||
      dir === 'broadcast_out' ||
      (channelIndex === undefined && toAddr === 0xFFFFFFFF);
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
      channelIndex: isBroadcast ? undefined : channelIndex,
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
    newFromFirmware.push(fwMsg);
  }
  // Persist newly fetched messages to IndexedDB so they survive reconnects
  if (newFromFirmware.length > 0) {
    await messageDb.saveMessages(newFromFirmware).catch(() => {});
  }
}

// ─── Messaging ────────────────────────────────────────────────────────────

const packetIdToMsgId = new Map<string, string>();
const broadcastIdToMsgId = new Map<string, string>();
const pendingBroadcastTelemetry = new Map<string, BroadcastDeliveryNotification[]>();

/* Task 7 (audit gap #8): 'timeout' is a client-only UI status, distinct from
 * firmware's 'failed' (which only fires after the node exhausts its own
 * retry budget, up to ~15s for Normal tier and several minutes for Critical
 * -- docs/bramble-protocol-spec.md section 7.4). It exists purely to tell the
 * user "no confirmation yet" well before that, without asserting the send
 * failed. Firmware never reports 'timeout'; this map is how the UI produces
 * it and clears it again the moment a real ack/failure arrives. */
const sentStatusTimers = new Map<string, ReturnType<typeof setTimeout>>();
const SENT_TO_TIMEOUT_UI_MS = 10000;

function clearSentStatusTimer(msgId: string): void {
  const timer = sentStatusTimers.get(msgId);
  if (timer !== undefined) {
    clearTimeout(timer);
    sentStatusTimers.delete(msgId);
  }
}

function clearAllSentStatusTimers(): void {
  for (const timer of sentStatusTimers.values()) clearTimeout(timer);
  sentStatusTimers.clear();
}

function scheduleSentStatusTimeout(msgId: string): void {
  clearSentStatusTimer(msgId);
  const timer = setTimeout(() => {
    sentStatusTimers.delete(msgId);
    /* Only flip if still 'sent': a real ack/failed already superseded this,
     * or would have cleared the timer via clearSentStatusTimer. Re-checked
     * here too since the timer callback and an in-flight ack can race. */
    const current = useStore.getState().messages.find(m => m.id === msgId);
    if (current && current.status === 'sent') {
      useStore.getState().updateMessageStatus(msgId, 'timeout');
      messageDb.updateMessageStatus(msgId, 'timeout').catch(() => {});
    }
  }, SENT_TO_TIMEOUT_UI_MS);
  sentStatusTimers.set(msgId, timer);
}

const DEFAULT_DELIVERY_EVENT_RETENTION_DAYS = 30;
const DELIVERY_EVENT_RETENTION_DAYS = Number((import.meta as any).env?.VITE_DELIVERY_EVENT_RETENTION_DAYS ?? DEFAULT_DELIVERY_EVENT_RETENTION_DAYS);
const DELIVERY_EVENT_SYNC_SEQ_KEY_PREFIX = 'bramble:delivery-event-sync:last-seq:';

interface DeliveryReplayEventWire {
  eventId?: string;
  event_id?: string;
  eventSeq?: number;
  event_seq?: number;
  messageId?: string | number;
  message_id?: string | number;
  packetId?: string | number;
  packet_id?: string | number;
  broadcastId?: string | number;
  broadcast_id?: string | number;
  eventType?: string;
  event_type?: string;
  ts?: number;
  timestampMs?: number;
  timestamp_ms?: number;
  payload?: unknown;
  data?: unknown;
}

interface DeliveryReplayResponse {
  events?: DeliveryReplayEventWire[];
  latestEventSeq?: number;
  latest_event_seq?: number;
}

function retentionCutoffTs(nowMs = Date.now()): number {
  const days = Number.isFinite(DELIVERY_EVENT_RETENTION_DAYS) && DELIVERY_EVENT_RETENTION_DAYS > 0
    ? DELIVERY_EVENT_RETENTION_DAYS
    : DEFAULT_DELIVERY_EVENT_RETENTION_DAYS;
  return nowMs - days * 24 * 60 * 60 * 1000;
}

function currentNodeAddrHex(): string {
  const addr = useStore.getState().config?.identity?.address;
  return addr != null ? formatAddrHex(addr) : 'default';
}

function lastDeliverySeqKey(nodeAddr: string): string {
  return `${DELIVERY_EVENT_SYNC_SEQ_KEY_PREFIX}${nodeAddr}`;
}

function loadLastDeliveryEventSeq(nodeAddr: string): number {
  try {
    const raw = localStorage.getItem(lastDeliverySeqKey(nodeAddr));
    const parsed = raw ? Number(raw) : 0;
    return Number.isFinite(parsed) && parsed >= 0 ? parsed : 0;
  } catch {
    return 0;
  }
}

function saveLastDeliveryEventSeq(nodeAddr: string, seq: number): void {
  if (!Number.isFinite(seq) || seq < 0) return;
  try {
    localStorage.setItem(lastDeliverySeqKey(nodeAddr), String(Math.floor(seq)));
  } catch {
    // best effort
  }
}

function asNumber(v: unknown): number | undefined {
  if (typeof v === 'number' && Number.isFinite(v)) return v;
  if (typeof v === 'string' && v.trim() !== '') {
    const n = Number(v);
    return Number.isFinite(n) ? n : undefined;
  }
  return undefined;
}

function normalizeReplayDeliveryEvent(raw: DeliveryReplayEventWire): DeliveryEventRecord | null {
  const eventType = String(raw.eventType ?? raw.event_type ?? '').trim() || 'unknown';
  const ts = asNumber(raw.ts ?? raw.timestampMs ?? raw.timestamp_ms) ?? Date.now();

  const packetIdRaw = raw.packetId ?? raw.packet_id;
  const broadcastIdRaw = raw.broadcastId ?? raw.broadcast_id;
  const packetId = packetIdRaw !== undefined && packetIdRaw !== null ? String(packetIdRaw) : '';
  const broadcastId = broadcastIdRaw !== undefined && broadcastIdRaw !== null ? String(broadcastIdRaw) : '';

  const messageIdRaw = raw.messageId ?? raw.message_id;
  let messageId = messageIdRaw !== undefined && messageIdRaw !== null ? String(messageIdRaw) : '';

  if (!messageId && packetId) {
    messageId = packetIdToMsgId.get(packetId) ?? '';
  }
  if (!messageId && broadcastId) {
    messageId = broadcastIdToMsgId.get(broadcastId) ?? '';
  }
  if (!messageId) return null;

  const seq = asNumber(raw.eventSeq ?? raw.event_seq);
  const payload = raw.payload ?? raw.data;

  const eventId = String(raw.eventId ?? raw.event_id ?? (seq !== undefined
    ? `replay:${seq}:${messageId}`
    : `replay:${eventType}:${messageId}:${ts}`));

  return {
    eventId,
    messageId,
    packetId: packetId || undefined,
    conversationKey: `msg:${messageId}`,
    ts,
    nodeAddr: currentNodeAddrHex(),
    eventType,
    payload,
  };
}

async function syncDeliveryEventReplay(): Promise<void> {
  if (!client) return;

  let supportsDeliveryEventSync = false;
  try {
    const version = await client.rpc<Record<string, unknown>>('bramble.getVersion');
    supportsDeliveryEventSync = Boolean(
      version.supportsDeliveryEventSync ?? version.supports_delivery_event_sync,
    );
  } catch {
    return;
  }
  if (!supportsDeliveryEventSync) return;

  const nodeAddr = currentNodeAddrHex();
  const sinceEventSeq = loadLastDeliveryEventSeq(nodeAddr);

  let replay: DeliveryReplayResponse;
  try {
    replay = await client.rpc<DeliveryReplayResponse>('bramble.getDeliveryEvents', { sinceEventSeq });
  } catch (error) {
    const msg = (error as Error)?.message ?? '';
    const unsupported = /not\s+found|unknown\s+method|method\s+not\s+found/i.test(msg);
    if (unsupported) return;
    replay = await client.rpc<DeliveryReplayResponse>('bramble.getDeliveryEvents', { since_event_seq: sinceEventSeq });
  }

  const events = (replay.events ?? [])
    .map(normalizeReplayDeliveryEvent)
    .filter((e): e is DeliveryEventRecord => Boolean(e))
    .sort((a, b) => a.ts - b.ts);

  if (events.length > 0) {
    await deliveryEventStore.upsertDeliveryEvents(events);

    const eventsByMessage = new Map<string, DeliveryEventRecord[]>();
    for (const event of events) {
      const list = eventsByMessage.get(event.messageId) ?? [];
      list.push(event);
      eventsByMessage.set(event.messageId, list);
    }

    useStore.setState((state) => ({
      messages: state.messages.map((message) => {
        const applicable = eventsByMessage.get(message.id);
        if (!applicable || applicable.length === 0) return message;
        return applicable.reduce((acc, event) => applyDeliveryEventToMessage(acc, event), message);
      }),
    }));
  }

  const replayLatest = asNumber(replay.latestEventSeq ?? replay.latest_event_seq);
  const maxSeen = events.reduce((max, event) => {
    const seqMatch = /replay:(\d+):/.exec(event.eventId);
    const seq = seqMatch ? Number(seqMatch[1]) : undefined;
    return seq !== undefined && Number.isFinite(seq) ? Math.max(max, seq) : max;
  }, sinceEventSeq);
  saveLastDeliveryEventSeq(nodeAddr, Math.max(sinceEventSeq, replayLatest ?? 0, maxSeen));
}

function hydrateCorrelationMaps(messages: Message[]): void {
  packetIdToMsgId.clear();
  broadcastIdToMsgId.clear();
  pendingBroadcastTelemetry.clear();
  clearAllSentStatusTimers();
  for (const msg of messages) {
    if (msg.packetId) packetIdToMsgId.set(String(msg.packetId), msg.id);
    if (msg.broadcastId) broadcastIdToMsgId.set(msg.broadcastId, msg.id);
  }
}

function conversationKeyForMessage(message: Message): string {
  if (message.channelIndex !== undefined && message.channelIndex >= 0) return `ch:${message.channelIndex}`;
  if (message.to === 0xFFFFFFFF) return 'broadcast';
  return `dm:${message.direction === 'outgoing' ? message.to : message.from}`;
}

function applyDeliveryEventToMessage(message: Message, event: DeliveryEventRecord): Message {
  if (event.eventType === 'ack') {
    const payload = (event.payload ?? {}) as { status?: 'delivered' | 'failed' | 'sent' | 'sending'; relayPath?: RelayHop[] };
    return {
      ...message,
      status: payload.status ?? message.status,
      relayPath: payload.relayPath ?? message.relayPath,
    };
  }

  if (event.eventType === 'broadcast_delivery') {
    const payload = (event.payload ?? {}) as {
      addr?: number;
      status?: 'delivered' | 'failed';
      hopCount?: number;
      deliveredAtMs?: number;
    };
    if (payload.addr === undefined || !payload.status) return message;
    const existing = message.broadcastRecipients ?? [];
    const idx = existing.findIndex(r => r.addr === payload.addr);
    const incomingTs = payload.deliveredAtMs ?? event.ts;

    if (idx < 0) {
      return {
        ...message,
        broadcastRecipients: [...existing, {
          addr: payload.addr,
          status: payload.status,
          hopCount: payload.hopCount ?? 0,
          deliveredAtMs: incomingTs,
        }],
      };
    }

    if (existing[idx].deliveredAtMs > incomingTs) return message;

    const merged = [...existing];
    merged[idx] = {
      ...existing[idx],
      addr: payload.addr,
      status: payload.status,
      hopCount: payload.hopCount ?? existing[idx].hopCount,
      deliveredAtMs: incomingTs,
    };
    return { ...message, broadcastRecipients: merged };
  }

  return message;
}

async function hydrateMessagesWithDeliveryEvents(messages: Message[]): Promise<Message[]> {
  const hydrated = await Promise.all(messages.map(async (message) => {
    let events = await deliveryEventStore.listByMessage(message.id);
    if (events.length === 0 && message.packetId !== undefined && message.packetId !== null) {
      events = await deliveryEventStore.listByPacketId(String(message.packetId));
    }
    return events.reduce((acc, event) => applyDeliveryEventToMessage(acc, event), message);
  }));
  return hydrated;
}

interface BroadcastDeliveryNotification {
  broadcastId: string;
  packetId?: string;
  from: string | number;
  status: 'delivered' | 'pending' | 'failed';
  hopCount: number;
  deliveredAtMs: number;
  // firmware snake_case compatibility
  broadcast_id?: string;
  recipient?: string | number;
  packet_id?: string;
  hop_count?: number;
  delivered_at_ms?: number;
}

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

function parseHexAddr(addr: string | number | undefined): number {
  if (typeof addr === 'number') return addr;
  if (!addr) return 0;
  const raw = addr.trim();
  if (!raw) return 0;
  const stripped = raw.replace(/^0x/i, '');
  // If the string is plain decimal digits, treat as decimal.
  if (/^[0-9]+$/.test(stripped) && !/[A-Fa-f]/.test(stripped)) {
    return parseInt(stripped, 10);
  }
  return parseInt(stripped, 16);
}

export function registerBroadcastSendTelemetry(msgId: string, meta: { packetId?: string; broadcastId?: string }): void {
  const { packetId, broadcastId } = meta;
  if (packetId || broadcastId) {
    useStore.getState().updateMessageBroadcastMeta(msgId, { packetId, broadcastId });
  }
  if (packetId) {
    packetIdToMsgId.set(packetId, msgId);
  }
  if (broadcastId) {
    broadcastIdToMsgId.set(broadcastId, msgId);
    applyPendingBroadcastTelemetry(broadcastId);
  }
}

function applyPendingBroadcastTelemetry(broadcastId: string): void {
  const queued = pendingBroadcastTelemetry.get(broadcastId);
  if (!queued || queued.length === 0) return;
  pendingBroadcastTelemetry.delete(broadcastId);
  for (const event of queued) {
    applyBroadcastDelivery(event);
  }
}

function applyBroadcastDelivery(event: BroadcastDeliveryNotification): void {
  const msgId = broadcastIdToMsgId.get(event.broadcastId);
  if (!msgId) {
    const existing = pendingBroadcastTelemetry.get(event.broadcastId) ?? [];
    pendingBroadcastTelemetry.set(event.broadcastId, [...existing, event]);
    return;
  }

  const recipient = {
    addr: parseHexAddr(event.from),
    status: event.status,
    hopCount: event.hopCount ?? 0,
    deliveredAtMs: event.deliveredAtMs ?? Date.now(),
  };

  const message = useStore.getState().messages.find(m => m.id === msgId);
  deliveryEventStore.upsertDeliveryEvent({
    eventId: `broadcast:${event.broadcastId}:${recipient.addr}`,
    messageId: msgId,
    conversationKey: message ? conversationKeyForMessage(message) : 'broadcast',
    ts: recipient.deliveredAtMs,
    nodeAddr: currentNodeAddrHex(),
    eventType: 'broadcast_delivery',
    payload: recipient,
  }).catch(() => {});

  useStore.getState().mergeBroadcastDeliveryRecipient(event.broadcastId, recipient);
}

export function handleBroadcastDelivery(params: unknown): void {
  const p = params as Partial<BroadcastDeliveryNotification>;
  const broadcastId = p.broadcastId ?? p.broadcast_id;
  const from = p.from ?? p.recipient;
  const packetId = p.packetId ?? p.packet_id;
  const hopCount = p.hopCount ?? p.hop_count ?? 0;
  const deliveredAtMs = p.deliveredAtMs ?? p.delivered_at_ms ?? Date.now();
  if (!broadcastId || !p.status || from === undefined) return;
  applyBroadcastDelivery({
    broadcastId,
    packetId,
    from,
    status: p.status,
    hopCount,
    deliveredAtMs,
  });
}

export function handleDeliveryUpdate(params: unknown): void {
  const p = params as Record<string, unknown>;
  const kind = String(p.kind ?? p.eventType ?? p.event_type ?? '').toLowerCase();

  if (kind === 'ack' || kind === 'delivery_ack') {
    handleAck(params);
    return;
  }

  if (kind === 'broadcast_delivery' || kind === 'broadcast') {
    handleBroadcastDelivery(params);
    return;
  }

  if (p.packet_id || p.packetId) {
    handleAck(params);
    return;
  }

  if (p.broadcast_id || p.broadcastId) {
    handleBroadcastDelivery(params);
  }
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

  const fallbackAddr = (() => {
    try {
      const raw = localStorage.getItem(LAST_NODE_ADDR_KEY);
      if (!raw) return undefined;
      const parsed = parseInt(raw, 16);
      return Number.isFinite(parsed) ? parsed : undefined;
    } catch {
      return undefined;
    }
  })();
  const myAddr = store.config?.identity?.address ?? fallbackAddr ?? 0;
  const msg = {
    id: uuid(),
    direction: 'outgoing' as const,
    from: myAddr,
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
          dest: formatAddrHex(wireDest),
          text,
          ...(isChannelScoped ? { channel: channelIndex } : {}),
        };
    const result = await client.rpc<{
      message_id?: string;
      status?: string;
      packetId?: string;
      packet_id?: string;
      broadcastId?: string;
      broadcast_id?: string;
      fragmented?: boolean;
      fragments_total?: number;
    }>(method, params);
    
    // Log fragmentation info for debugging
    if (result?.fragmented && result?.fragments_total) {
      console.info(`[send] Message fragmented into ${result.fragments_total} packets`);
    }
    
    store.updateMessageStatus(msg.id, 'sent');
    messageDb.updateMessageStatus(msg.id, 'sent').catch(() => {});
    registerBroadcastSendTelemetry(msg.id, {
      packetId: result?.packetId ?? result?.packet_id,
      broadcastId: result?.broadcastId ?? result?.broadcast_id,
    });
    /* Unicast only: broadcasts have no single ack and are tracked instead
     * via broadcastRecipients/telemetry, which already has its own
     * per-recipient pending/delivered/failed state. */
    if (!isBroadcast) {
      scheduleSentStatusTimeout(msg.id);
    }
  } catch (e) {
    clearSentStatusTimer(msg.id);
    store.updateMessageStatus(msg.id, 'failed');
    messageDb.updateMessageStatus(msg.id, 'failed').catch(() => {});
    throw e;
  }
}

// ─── Notification handlers ────────────────────────────────────────────────

export function handleAck(params: unknown): void {
  const p = params as Record<string, unknown>;
  /* Firmware sends snake_case (packet_id), webapp convention is camelCase */
  const packetId = (p.packetId ?? p.packet_id) as string | undefined;
  const status = (p.status as string) ?? 'delivered';
  /* Normalize relayPath: firmware sends addr as hex string, webapp needs number */
  const rawPath = p.relayPath as Array<{ addr: string | number; rssi: number }> | undefined;
  const relayPath: RelayHop[] | undefined = rawPath?.map(hop => ({
    addr: parseAddr(hop.addr),
    rssi: hop.rssi ?? 0,
  }));

  if (!packetId) return;
  const msgId = packetIdToMsgId.get(packetId);
  if (msgId) {
    packetIdToMsgId.delete(packetId);
    clearSentStatusTimer(msgId);
    const newStatus = status === 'delivered' ? 'delivered' : 'failed';
    const nowTs = Date.now();

    const message = useStore.getState().messages.find(m => m.id === msgId);
    deliveryEventStore.upsertDeliveryEvent({
      eventId: `ack:${packetId}:${newStatus}`,
      messageId: msgId,
      packetId,
      conversationKey: message ? conversationKeyForMessage(message) : `dm:${msgId}`,
      ts: nowTs,
      nodeAddr: currentNodeAddrHex(),
      eventType: 'ack',
      payload: { status: newStatus, relayPath },
    }).catch(() => {});

    useStore.getState().updateMessageStatus(msgId, newStatus, relayPath);
    messageDb.updateMessageStatus(msgId, newStatus, relayPath).catch(() => {});
  }
}

export function normalizeIncomingRealtimeMessage(params: unknown) {
  const p = params as any;
  const fromAddr = parseAddr(p.from);
  const toAddr = parseAddr(p.to);
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
  const msg = normalizeIncomingRealtimeMessage(p);
  const store = useStore.getState();
  // The firmware includes the sender's display name when its neighbor table
  // knows it. Learn it BEFORE rendering anything (chat list, notification),
  // so a first message from a peer never shows as a bare hex address.
  if (typeof p.fromName === 'string' && p.fromName.length > 0) {
    store.setPeerName(msg.from, p.fromName);
  }
  store.addMessage(msg);
  messageDb.saveMessage(msg).catch(() => {});
  maybeNotifyIncoming(msg);
}

/** Test seam for the incoming-message + native-notification path. */
export function handleIncomingForTest(params: unknown): void {
  handleIncomingMessage(params);
}

/**
 * Raise a native Android notification for an incoming message. No-op outside
 * the Android shell (web and Electron have no bridge). Suppressed for a
 * message from this node and, following common messaging-app behavior, for
 * the conversation the user is currently looking at (app visible + that
 * conversation open).
 */
function maybeNotifyIncoming(msg: Message): void {
  if (!isAndroidShell()) return;
  const notify = window.brambleAndroidNotify;
  if (!notify) return;

  const store = useStore.getState();
  const selfAddr = store.config?.identity?.address;
  if (selfAddr !== undefined && msg.from === selfAddr) return;

  const conversationId = conversationKeyForMessage(msg);
  const appVisible = typeof document !== 'undefined' && document.visibilityState === 'visible';
  if (appVisible && store.activeConversationId === conversationId) return;

  const senderName = store.peerNames.get(msg.from) ?? `0x${(msg.from >>> 0).toString(16).toUpperCase()}`;
  const conversationTitle = conversationTitleFor(conversationId, senderName, store);

  try {
    notify.onMessage(JSON.stringify({
      conversationId,
      conversationTitle,
      sender: senderName,
      text: msg.text ?? '',
      timestamp: msg.timestampMs ?? Date.now(),
    }));
  } catch { /* notification is best-effort */ }
}

function conversationTitleFor(conversationId: string, senderName: string, store: ReturnType<typeof useStore.getState>): string {
  if (conversationId === 'broadcast') return 'Broadcast';
  if (conversationId.startsWith('ch:')) {
    const idx = Number(conversationId.slice(3));
    const ch = store.config?.channels?.find(c => c.index === idx);
    return ch?.name?.trim() ? ch.name : `Channel ${idx}`;
  }
  // dm: title is the peer, which is the sender for an incoming DM.
  return senderName;
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

  const roundsTotal = Math.max(1, Number(raw.rounds_total ?? raw.roundsTotal ?? (raw.seen_rounds ? 3 : 1)));
  const seenRounds = Math.max(1, Math.min(roundsTotal, raw.seen_rounds ?? raw.seenRounds ?? 1));
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
  if (!client) return;
  const result = await client.rpc<{ peerLocations: any[] }>('bramble.getPeerLocations');
  const normalized = (result.peerLocations ?? []).map(normalizePeerLocation);
  useStore.getState().setPeerLocations(normalized);
}

export async function setLocationConfig(config: Partial<LocationConfig>): Promise<void> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc('bramble.setLocationConfig', config as unknown as Record<string, unknown>);
  assertOk(result, 'Failed to save location config');
  await loadConfig();
  await loadPeerLocations().catch(() => {});
}

export async function setLocationContact(
  addr: number,
  tier: LocationTier,
  intervalSec?: number,
  distanceTriggerM?: number
): Promise<void> {
  if (!client) throw new Error('Not connected');
  // Firmware expects address as a hex string, not a numeric addr.
  const params: Record<string, unknown> = { address: formatAddrHex(addr), tier };
  if (intervalSec !== undefined) params.intervalSec = intervalSec;
  if (distanceTriggerM !== undefined) params.distanceTriggerM = distanceTriggerM;
  const result = await client.rpc('bramble.setLocationContact', params);
  assertOk(result, 'Failed to set location contact');
  await loadConfig();
}

export async function removeLocationContact(addr: number): Promise<void> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc('bramble.removeLocationContact', { address: formatAddrHex(addr) });
  assertOk(result, 'Failed to remove location contact');
  await loadConfig();
}

export async function shareLocationOnce(addr: number, tier?: LocationTier): Promise<void> {
  if (!client) throw new Error('Not connected');
  const params: Record<string, unknown> = { address: formatAddrHex(addr) };
  if (tier !== undefined) params.tier = tier;
  const result = await client.rpc('bramble.shareLocationOnce', params);
  assertOk(result, 'Failed to share location');
}

// ─── Peer verification (SAS) ──────────────────────────────────────────────

/**
 * Fetch a peer's SAS/verification state and cache it in the store. An
 * unpinned peer (never DM'd, or DM'd before the identity handshake landed)
 * is not an error: the firmware returns an empty SAS and verified:false.
 */
export async function loadPeerVerification(
  addr: number,
): Promise<import('../types/bramble').PeerVerification> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc<{ sas?: string; verified?: boolean; keyChanged?: boolean }>(
    'bramble.getPeerVerification',
    { address: formatAddrHex(addr) },
  );
  const v: import('../types/bramble').PeerVerification = {
    sas: result.sas ?? '',
    verified: !!result.verified,
    keyChanged: !!result.keyChanged,
  };
  useStore.getState().setPeerVerification(addr, v);
  return v;
}

/** Mark (or unmark) a peer verified. Refreshes the cached state on success. */
export async function setPeerVerified(addr: number, verified: boolean): Promise<boolean> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc<{ ok: boolean }>('bramble.setPeerVerified', {
    address: formatAddrHex(addr),
    verified,
  });
  if (result?.ok) {
    await loadPeerVerification(addr).catch(() => {});
  }
  return !!result?.ok;
}

function handleLocationUpdate(params: unknown): void {
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

// ─── Device management (auth token, allowed origins, OTA): issue #95 ───────

export interface AuthTokenInfo { token: string; enabled: boolean; }

export async function getAuthToken(): Promise<AuthTokenInfo> {
  if (!client) throw new Error('Not connected');
  const r = await client.rpc<any>('bramble.getAuthToken');
  return { token: r.token ?? '', enabled: !!r.enabled };
}

export async function setAuthToken(token: string): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.setAuthToken', { token });
}

export async function getAllowedOrigins(): Promise<string[]> {
  if (!client) throw new Error('Not connected');
  const r = await client.rpc<any>('bramble.getAllowedOrigins');
  return Array.isArray(r.origins) ? r.origins : [];
}

export async function setAllowedOrigins(origins: string[]): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.setAllowedOrigins', { origins });
}

export interface OtaOriginInfo {
  origin: string;
  defaultOrigin: string;
  overridden: boolean;
  versionFloor?: string;
  runningVersion?: string;
}

export async function getOtaOrigin(): Promise<OtaOriginInfo> {
  if (!client) throw new Error('Not connected');
  const r = await client.rpc<any>('bramble.otaGetOrigin');
  return {
    origin: r.origin ?? '',
    defaultOrigin: r.default_origin ?? r.defaultOrigin ?? '',
    overridden: !!r.overridden,
    versionFloor: r.version_floor ?? r.versionFloor,
    runningVersion: r.running_version ?? r.runningVersion,
  };
}

export async function setOtaOrigin(origin: string): Promise<{ ok: boolean; error?: string }> {
  if (!client) throw new Error('Not connected');
  const r = await client.rpc<any>('bramble.otaSetOrigin', { origin });
  return { ok: !!r.ok, error: r.error };
}

export async function resetOtaOrigin(): Promise<void> {
  if (!client) throw new Error('Not connected');
  await client.rpc('bramble.otaSetOrigin', { reset: true });
}

export async function startOtaUpdate(path: string, allowDowngrade = false): Promise<{ ok: boolean; note?: string; url?: string; error?: string }> {
  if (!client) throw new Error('Not connected');
  const r = await client.rpc<any>('bramble.otaUpdate', { path, allow_downgrade: allowDowngrade });
  return { ok: !!r.ok, note: r.note, url: r.url, error: r.error };
}

// ─── Network key provisioning ──────────────────────────────────────────────

/**
 * Push a freshly-generated network key to the device out-of-band (QR / paste).
 * The key is write-only: this call never returns the key, only whether the
 * device accepted it. Returns false rather than throwing on rejection so
 * callers can show an inline error instead of an unhandled promise rejection.
 */
export async function setNetworkKey(keyHex: string): Promise<boolean> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc<{ ok: boolean; error?: string }>('bramble.setNetworkKey', { key: keyHex });
  return !!result?.ok;
}

export async function getNetworkKeyStatus(): Promise<{ provisioned: boolean; fingerprint: string }> {
  if (!client) throw new Error('Not connected');
  return await client.rpc<{ provisioned: boolean; fingerprint: string }>('bramble.getNetworkKeyStatus');
}

/**
 * Mint a fresh network key on the device and provision THIS node as the fleet
 * founder. The device generates the key from its entropy-gated source and
 * persists it; the raw key is returned once so the operator can copy it to the
 * other nodes. On an already-provisioned node this RE-KEYS it, so callers must
 * confirm first. The returned key is a secret: never log it.
 */
export async function generateNetworkKey(): Promise<{ key: string; fingerprint: string }> {
  if (!client) throw new Error('Not connected');
  return await client.rpc<{ key: string; fingerprint: string }>('bramble.generateNetworkKey');
}

/**
 * Refresh the global network-key provisioning status in the store. Polled from
 * the app shell so the UNPROVISIONED (inert) banner stays live on every tab.
 */
export async function loadNetworkKeyStatus(): Promise<void> {
  if (!client) return;
  const s = await client.rpc<{ provisioned: boolean; fingerprint: string }>('bramble.getNetworkKeyStatus');
  useStore.getState().setNetworkKeyStatus(s);
}

// --- Trust anchor ----------------------------------------------------------

/**
 * Pin THIS node to a fleet trust anchor by its PUBLIC key. The anchor private
 * seed stays offline on the operator's client and is NEVER sent: setAnchor
 * carries the public key only. Returns false rather than throwing on rejection
 * so callers can show an inline error.
 */
export async function setAnchor(anchorPubHex: string): Promise<boolean> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc<{ ok: boolean; error?: string }>('bramble.setAnchor', { anchor_pubkey: anchorPubHex });
  return !!result?.ok;
}

export async function getAnchorStatus(): Promise<import('../types/bramble').AnchorStatus> {
  if (!client) throw new Error('Not connected');
  return await client.rpc<import('../types/bramble').AnchorStatus>('bramble.getAnchorStatus');
}

export async function getIdentity(): Promise<import('../types/bramble').NodeIdentityWire> {
  if (!client) throw new Error('Not connected');
  return await client.rpc<import('../types/bramble').NodeIdentityWire>('bramble.getIdentity');
}

/**
 * Install the endorsement cert the operator signed for this node. The cert is
 * public (the signature over the node's own identity key); it travels back to
 * the node so an anchored fleet will pin this identity. Returns false rather
 * than throwing on rejection.
 */
export async function setEndorsement(notAfterHex: string, sigHex: string): Promise<boolean> {
  if (!client) throw new Error('Not connected');
  const result = await client.rpc<{ ok: boolean; error?: string }>('bramble.setEndorsement', {
    not_after: notAfterHex,
    endorsement_sig: sigHex,
  });
  return !!result?.ok;
}

/**
 * Refresh the node's anchor provisioning status in the store, mirroring
 * loadNetworkKeyStatus so the enrollment UI can reflect anchored/endorsed live.
 */
export async function loadAnchorStatus(): Promise<void> {
  if (!client) return;
  const s = await client.rpc<import('../types/bramble').AnchorStatus>('bramble.getAnchorStatus');
  useStore.getState().setAnchorStatus(s);
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

export function __resetBroadcastTelemetryForTests(): void {
  packetIdToMsgId.clear();
  broadcastIdToMsgId.clear();
  pendingBroadcastTelemetry.clear();
  clearAllSentStatusTimers();
}

// Test isolation: actions.ts holds several module-level singletons (the
// transport client, the packet/broadcast correlation maps, the pending
// sent-status timers). Nothing resets them between test files, so state can
// leak across suites once restoreMocks/afterEach hygiene is in place. Call
// this from test/setup.ts's afterEach.
export function __resetActionsForTests(): void {
  client = null;
  packetIdToMsgId.clear();
  broadcastIdToMsgId.clear();
  pendingBroadcastTelemetry.clear();
  clearAllSentStatusTimers();
}

export function __normalizeReplayDeliveryEventForTests(raw: DeliveryReplayEventWire): DeliveryEventRecord | null {
  return normalizeReplayDeliveryEvent(raw);
}

export function __clearDeliveryEventSyncStateForTests(nodeAddr?: string): void {
  packetIdToMsgId.clear();
  broadcastIdToMsgId.clear();
  pendingBroadcastTelemetry.clear();
  clearAllSentStatusTimers();

  try {
    if (nodeAddr) {
      localStorage.removeItem(lastDeliverySeqKey(nodeAddr));
      return;
    }
    const keys: string[] = [];
    for (let i = 0; i < localStorage.length; i += 1) {
      const key = localStorage.key(i);
      if (key?.startsWith(DELIVERY_EVENT_SYNC_SEQ_KEY_PREFIX)) keys.push(key);
    }
    for (const key of keys) {
      localStorage.removeItem(key);
    }
  } catch {
    // noop
  }
}

export function getClient(): BrambleClient | null {
  return client;
}

// Persist a device to the book once its real node address is known (post-connect).
// A book write must never break a live connection, so callers wrap this in the
// same try/catch used around the rest of the post-connect save path.
export function saveConnectedDevice(args: {
  addr: number;
  name?: string;
  ip: string;
  token: string;
  remember: boolean;
  transport: 'wifi' | 'serial' | 'ble';
  bleDeviceId?: string;
  bleDeviceName?: string;
}): void {
  const address = formatAddrHex(args.addr);
  upsertDevice({
    address, name: args.name, lastIp: args.ip, transport: args.transport, remember: args.remember,
    bleDeviceId: args.bleDeviceId, bleDeviceName: args.bleDeviceName,
  });
  if (args.token) setDeviceToken(address, args.token, args.remember);
  refreshDevices();
}

export function refreshDevices(): void {
  useStore.getState().setDevices(listDevices());
}
export function forgetSavedDevice(address: string): void {
  forgetDevice(address);
  refreshDevices();
}
export function renameSavedDevice(address: string, name: string): void {
  renameDevice(address, name);
  refreshDevices();
}
