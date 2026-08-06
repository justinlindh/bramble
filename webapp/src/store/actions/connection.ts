// Connection lifecycle: transport creation, node verification, auto-reconnect,
// push-event subscriptions, the initial data load, and disconnect. This module
// is the fan-out hub: it imports from every sibling seam and nothing imports
// from it (except the barrel).
import { session, requireClient, LAST_NODE_ADDR_KEY } from './client';
import { useStore } from '../index';
import { createTransport, BrambleClient } from '../../transport';
import { fetchConnectionCapabilities } from '../../lib/connectionMode';
import { formatAddrHex } from '../../utils/address';
import { isAndroidShell } from '../../utils/platform';
import { friendlyErrorFrom, isAuthError, isUnknownMethodError } from '../../lib/errors';
import type { TransportType } from '../../types/bramble';
import {
  initMessageStore,
  loadMessages,
  syncDeliveryEventReplay,
  handleIncomingMessage,
  handleAck,
  handleBroadcastDelivery,
  handleProbeAck,
  handleProbeComplete,
} from './messaging';
import {
  loadNeighbors,
  loadRoutes,
  loadAirtime,
  loadStatus,
  loadPeerLocations,
  handleLocationUpdate,
  handleTrafficEvent,
} from './telemetry';
import { loadConfig } from './config';
import { saveConnectedDevice } from './deviceBook';

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

// ─── Connection ─────────────────────────────────────────────────────────

const SERIAL_RPC_READY_ATTEMPTS = 8;
const NODE_VERIFY_ATTEMPTS = 2;
const RPC_READY_TIMEOUT_MS = 1500;
const RPC_READY_RETRY_DELAY_MS = 350;

async function probeRpcReadiness(): Promise<void> {
  const client = requireClient();
  try {
    await client.rpc('bramble.ping', undefined, RPC_READY_TIMEOUT_MS);
    return;
  } catch (error) {
    if (!isUnknownMethodError(error)) throw error;
  }
  await client.rpc('bramble.getStatus', undefined, RPC_READY_TIMEOUT_MS);
}

// Poll `probe` up to `attempts` times, sleeping RPC_READY_RETRY_DELAY_MS between
// tries, and resolve true on the first attempt that returns true. Each caller's
// distinct "what counts as ready" rule (an auth error is still a real node; a
// failure just means keep waiting) lives in its own probe closure, so this owns
// only the shared retry cadence.
async function pollReady(attempts: number, probe: () => Promise<boolean>): Promise<boolean> {
  for (let attempt = 1; attempt <= attempts; attempt += 1) {
    if (await probe()) return true;
    if (attempt < attempts) {
      await new Promise(r => setTimeout(r, RPC_READY_RETRY_DELAY_MS));
    }
  }
  return false;
}

// Confirm the freshly opened transport actually speaks Bramble before we report
// "Connected". A socket that opens but is not a Bramble node (a wrong IP/port
// that happens to host a WebSocket) would otherwise sit in a permanent empty
// Connected state while every RPC times out and the client reconnects forever
// (issue #91). ping/getStatus is on the unauthenticated allowlist, so this also
// works against an auth-required node.
async function verifyBrambleNode(): Promise<boolean> {
  return pollReady(NODE_VERIFY_ATTEMPTS, async () => {
    try {
      await probeRpcReadiness();
      return true;
    } catch (error) {
      // An auth-required node answers the allowlisted ping, so a 1008/auth error
      // here means a real node we simply cannot fully use yet: treat it as
      // reachable and let the init RPCs surface the auth-required state.
      return isAuthError(error);
    }
  });
}

async function ensureSerialRpcReady(): Promise<boolean> {
  let lastError: unknown;
  const ready = await pollReady(SERIAL_RPC_READY_ATTEMPTS, async () => {
    try {
      await probeRpcReadiness();
      return true;
    } catch (error) {
      lastError = error;
      return false;
    }
  });
  if (!ready) {
    console.warn(`[serial-rpc] readiness probe exhausted: ${((lastError as Error)?.message ?? 'startup timeout')}`);
  }
  return ready;
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
  if (session.client) {
    try { session.client.clearSubscriptions(); } catch { /* noop */ }
    try { await session.client.disconnect(); } catch { /* noop */ }
    session.client = null;
  }

  store.setConnectionState('connecting');
  try {
    const transport = createTransport(type, options);
    await transport.connect();
    session.client = new BrambleClient(transport);
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
        await session.client.rpc('bramble.getStatus', {}, 4000);
      } catch (e) {
        const msg = (e as Error)?.message ?? '';
        const authy = isAuthError(msg) || (!handshakeValidated && /-1005/.test(msg));
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

    // Enable auto-reconnect for WiFi/WebSocket transports. Not every
    // transport implements it, so it is feature-detected structurally.
    const reconnectable = transport as typeof transport & {
      enableAutoReconnect?: (handlers: { onDisconnect: () => void; onReconnect: () => Promise<void> }) => void;
    };
    if ('enableAutoReconnect' in transport && typeof reconnectable.enableAutoReconnect === 'function') {
      reconnectable.enableAutoReconnect({
        onDisconnect: () => {
          useStore.getState().setConnectionState('error', 'Connection lost, reconnecting…');
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
    session.client.subscribe('bramble.onMessage', (params) =>
      handleIncomingMessage(params)
    );
    session.client.subscribe('bramble.onAck', (params) => handleAck(params));
    session.client.subscribe('bramble.onBroadcastDelivery', (params) => handleBroadcastDelivery(params));
    // A GPS fix acquired mid-session must refresh the map: without this the
    // self position only appears after a manual reload.
    session.client.subscribe('bramble.onGpsEvent', () => { loadPeerLocations().catch(() => {}); });
    session.client.subscribe('bramble.onNeighborChange', () => loadNeighbors());
    // NOTE: no firmware build emits bramble.onRouteUpdate today (nothing in
    // main/ or components/ calls rpc_notify with it, and it is absent from
    // api/openapi.yaml). Kept for forward compatibility, but route freshness
    // currently depends on the slow poll in the Nodes page, not on this event.
    session.client.subscribe('bramble.onRouteUpdate', () => loadRoutes());
    session.client.subscribe('bramble.onAirtimeWarning', () => loadAirtime());
    session.client.subscribe('bramble.onProbeResult', (params) => handleProbeAck(params));
    session.client.subscribe('bramble.onProbeComplete', (params) => handleProbeComplete(params));
    session.client.subscribe('location.update', (params) => handleLocationUpdate(params));
    session.client.subscribe('bramble.onPeerLocation', (params) => handleLocationUpdate(params));
    session.client.subscribe('bramble.onTrafficEvent', (params) => handleTrafficEvent(params));

    // Initial data load: all best-effort so a slow RPC doesn't kill the connection
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
        try { session.client?.clearSubscriptions(); } catch { /* noop */ }
        try { await session.client?.disconnect(); } catch { /* noop */ }
        session.client = null;
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
    try { session.client?.clearSubscriptions(); } catch { /* noop */ }
    try { await session.client?.disconnect(); } catch { /* noop */ }
    session.client = null;
    store.setTransport(null);
    // Show the overlay so the user can retry: 'disconnected' shows connect UI.
    store.setConnectionState('disconnected', friendlyErrorFrom(e));
  }
}

export async function disconnect(): Promise<void> {
  session.client?.clearSubscriptions();
  await session.client?.disconnect();
  session.client = null;
  useStore.getState().setConnectionState('disconnected');
  useStore.getState().setTransport(null);
}
