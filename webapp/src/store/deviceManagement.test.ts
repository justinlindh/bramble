import { beforeEach, describe, expect, it, vi } from 'vitest';
import * as actions from './actions';

// The OTA status/event actions guard on the module-level `client` exactly like
// the anchor and network-key actions (see __tests__/anchor.test.ts and
// __tests__/networkKey.test.ts): the client is created inside connect() from a
// mocked transport, so we drive a real connect() to populate it, then exercise
// the RPC mapping the same way the sibling store tests do. This mock is
// file-wide; the "not connected" tests below still pass because they never
// call connect(), so the module-level `client` in actions.ts stays null.
const rpcMock = vi.fn<(
  method: string,
  params?: Record<string, unknown>,
  timeoutMs?: number,
) => Promise<any>>();
// The disposer returned by subscribe() must itself be a spy (not a fresh
// no-op closure) so tests can assert the caller actually invoked it.
const unsubscribeSpy = vi.fn();
const subscribeMock = vi.fn((_event: string, _cb: (params: unknown) => void) => unsubscribeSpy);
const transportConnectMock = vi.fn(async () => {});
const transportDisconnectMock = vi.fn(async () => {});

vi.mock('../transport', () => {
  class MockBrambleClient {
    subscribe = vi.fn((event: string, cb: (params: unknown) => void) => subscribeMock(event, cb));
    clearSubscriptions = vi.fn();
    disconnect = vi.fn(async () => {});
    rpc = vi.fn((method: string, params?: Record<string, unknown>, timeoutMs?: number) => rpcMock(method, params, timeoutMs));
    constructor(_transport: unknown) {}
  }

  return {
    createTransport: vi.fn(() => ({
      connect: transportConnectMock,
      disconnect: transportDisconnectMock,
      connected: true,
      onNotification: vi.fn(),
      sendRPC: vi.fn(),
    })),
    BrambleClient: MockBrambleClient,
  };
});

vi.mock('./messageDb', () => ({
  messageDb: {
    open: vi.fn(async () => {}),
    getMessages: vi.fn(async () => []),
    saveMessages: vi.fn(async () => {}),
  },
}));

vi.mock('./deliveryEventStore', () => ({
  deliveryEventStore: {
    open: vi.fn(async () => {}),
    pruneOldEvents: vi.fn(async () => {}),
    listByMessage: vi.fn(async () => []),
    upsertDeliveryEvents: vi.fn(async () => {}),
  },
}));

function setDefaultRpcBehavior() {
  rpcMock.mockImplementation(async (method: string) => {
    switch (method) {
      case 'bramble.ping':
        return { ok: true };
      case 'bramble.getConfig':
        return { identity: { address: 0x1234 } };
      case 'bramble.getStatus':
        return {};
      case 'bramble.getAirtime':
        return {};
      case 'bramble.getNeighbors':
        return { neighbors: [] };
      case 'bramble.getRoutes':
        return { routes: [] };
      case 'bramble.getMessages':
        return { messages: [] };
      case 'bramble.getPeerLocations':
        return { peerLocations: [] };
      case 'bramble.getVersion':
        return { supportsDeliveryEventSync: false };
      default:
        return {};
    }
  });
}

// The device-management actions (issue #95) guard on an active client. Without
// a connection they must reject rather than silently no-op, so the UI surfaces
// the error. Exercising the live RPC mapping is covered end to end against the
// mock node in the e2e smoke and the mock handler tests.
describe('device management actions (issue #95)', () => {
  it('exposes the auth, origins, and OTA actions', () => {
    for (const fn of [
      'getAuthToken', 'setAuthToken',
      'getAllowedOrigins', 'setAllowedOrigins',
      'getOtaOrigin', 'setOtaOrigin', 'resetOtaOrigin', 'startOtaUpdate',
      'getOtaStatus', 'subscribeOtaEvents',
    ] as const) {
      expect(typeof actions[fn]).toBe('function');
    }
  });

  it('rejects when not connected', async () => {
    await expect(actions.getAuthToken()).rejects.toThrow('Not connected');
    await expect(actions.setAuthToken('x')).rejects.toThrow('Not connected');
    await expect(actions.getAllowedOrigins()).rejects.toThrow('Not connected');
    await expect(actions.setAllowedOrigins([])).rejects.toThrow('Not connected');
    await expect(actions.getOtaOrigin()).rejects.toThrow('Not connected');
    await expect(actions.setOtaOrigin('x')).rejects.toThrow('Not connected');
    await expect(actions.resetOtaOrigin()).rejects.toThrow('Not connected');
    await expect(actions.startOtaUpdate('x')).rejects.toThrow('Not connected');
    await expect(actions.getOtaStatus()).rejects.toThrow('Not connected');
  });
});

describe('otaStatus/onOtaEvent actions', () => {
  beforeEach(async () => {
    vi.resetModules();
    vi.clearAllMocks();
    transportConnectMock.mockResolvedValue(undefined);
    transportDisconnectMock.mockResolvedValue(undefined);
    setDefaultRpcBehavior();

    const { useStore } = await import('./index');
    useStore.setState({
      connectionState: 'disconnected',
      connectionError: undefined,
      transport: null,
      config: null,
      status: null,
      airtime: null,
      neighbors: [],
      routes: [],
      messages: [],
      conversations: new Map(),
    } as any);
  });

  it('getOtaStatus maps snake_case fields', async () => {
    const { connect, getOtaStatus } = await import('./actions');
    await connect('serial');

    rpcMock.mockResolvedValueOnce({
      state: 'downloading', bytes: 500, total: 1000, percent: 50,
      last_error: 'boom', running_version: '0.0.0-local', version_floor: '0.0.0-local',
    });
    const s = await getOtaStatus();
    expect(s.state).toBe('downloading');
    expect(s.percent).toBe(50);
    expect(s.lastError).toBe('boom');
    expect(s.runningVersion).toBe('0.0.0-local');
    expect(rpcMock).toHaveBeenCalledWith('bramble.otaStatus', undefined, undefined);
  });

  it('startOtaUpdate surfaces last_error from the previous attempt', async () => {
    const { connect, startOtaUpdate } = await import('./actions');
    await connect('serial');

    rpcMock.mockResolvedValueOnce({ ok: true, note: 'started', last_error: 'previous failure' });
    const r = await startOtaUpdate('stable/v1/heltec-v4/bramble.bin');
    expect(r.lastError).toBe('previous failure');
  });

  it('subscribeOtaEvents wraps bramble.onOtaEvent and maps snake_case fields', async () => {
    const { connect, subscribeOtaEvents } = await import('./actions');
    await connect('serial');

    const cb = vi.fn();
    subscribeOtaEvents(cb);
    expect(subscribeMock).toHaveBeenCalledWith('bramble.onOtaEvent', expect.any(Function));

    const handler = subscribeMock.mock.calls.find(([event]) => event === 'bramble.onOtaEvent')?.[1];
    expect(handler).toBeTypeOf('function');
    handler!({ state: 'failed', bytes: 10, total: 100, percent: 10, error: 'crc mismatch' });
    expect(cb).toHaveBeenCalledWith(expect.objectContaining({
      state: 'failed', bytes: 10, total: 100, percent: 10, lastError: 'crc mismatch',
    }));
  });

  it('subscribeOtaEvents returns the disposer client.subscribe hands back, so callers can tear it down', async () => {
    const { connect, subscribeOtaEvents } = await import('./actions');
    await connect('serial');

    const unsubscribe = subscribeOtaEvents(vi.fn());
    expect(unsubscribe).toBeTypeOf('function');
    expect(unsubscribeSpy).not.toHaveBeenCalled();

    unsubscribe();
    expect(unsubscribeSpy).toHaveBeenCalledTimes(1);
  });

  it('subscribeOtaEvents returns a no-op disposer when there is no client', async () => {
    const { subscribeOtaEvents } = await import('./actions');
    // No connect() call in this test: the module-level client stays null.
    const unsubscribe = subscribeOtaEvents(vi.fn());
    expect(unsubscribe).toBeTypeOf('function');
    expect(() => unsubscribe()).not.toThrow();
  });
});
