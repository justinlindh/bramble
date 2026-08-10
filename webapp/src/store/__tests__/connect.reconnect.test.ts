import { beforeEach, describe, expect, it, vi } from 'vitest';

// Auto-reconnect must refresh the SAME per-node slices the initial network
// connect pulls. It used to hand-copy a subset and had drifted, silently
// skipping node status and peer locations, so after a dropped-and-restored
// link the status panel and the map went stale until a manual reload. Both
// paths now share refreshNodeData; this pins that the reconnect handler pulls
// status and peer locations (the two that had gone missing) along with the
// rest.

const rpcMock = vi.fn<(
  method: string,
  params?: Record<string, unknown>,
  timeoutMs?: number,
) => Promise<any>>();

// Captured from the transport's enableAutoReconnect so the test can drive a
// reconnect the way the live transport would.
let capturedOnReconnect: (() => Promise<void>) | null = null;

vi.mock('../../transport', () => {
  class MockBrambleClient {
    subscribe = vi.fn(() => () => {});
    clearSubscriptions = vi.fn();
    disconnect = vi.fn(async () => {});
    rpc = vi.fn((method: string, params?: Record<string, unknown>, timeoutMs?: number) =>
      rpcMock(method, params, timeoutMs));
    constructor(_transport: unknown) {}
  }

  return {
    createTransport: vi.fn(() => ({
      connect: vi.fn(async () => {}),
      disconnect: vi.fn(async () => {}),
      connected: true,
      onNotification: vi.fn(),
      sendRPC: vi.fn(),
      enableAutoReconnect: vi.fn((handlers: { onReconnect: () => Promise<void> }) => {
        capturedOnReconnect = handlers.onReconnect;
      }),
    })),
    BrambleClient: MockBrambleClient,
  };
});

vi.mock('../messageDb', () => ({
  messageDb: {
    open: vi.fn(async () => {}),
    getMessages: vi.fn(async () => []),
    saveMessages: vi.fn(async () => {}),
  },
}));

vi.mock('../deliveryEventStore', () => ({
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

describe('auto-reconnect refreshes the full node data set', () => {
  beforeEach(async () => {
    vi.resetModules();
    vi.clearAllMocks();
    capturedOnReconnect = null;
    setDefaultRpcBehavior();

    const { useStore } = await import('../index');
    useStore.setState({
      connectionState: 'disconnected',
      connectionError: undefined,
      config: null,
      status: null,
      airtime: null,
      neighbors: [],
      routes: [],
      messages: [],
      conversations: new Map(),
    } as any);
  });

  it('pulls status and peer locations on reconnect, not just a subset', async () => {
    const { connect } = await import('../actions');

    await connect('wifi', { url: 'ws://127.0.0.1/ws' });
    expect(capturedOnReconnect).toBeTypeOf('function');

    // Ignore the initial connect's RPCs; only the reconnect load is under test.
    rpcMock.mockClear();

    await capturedOnReconnect!();

    const reconnectMethods = rpcMock.mock.calls.map(([method]) => method);
    // The two the drifted handler used to skip.
    expect(reconnectMethods).toContain('bramble.getStatus');
    expect(reconnectMethods).toContain('bramble.getPeerLocations');
    // And the slices it did already refresh, to prove reconnect is the full load.
    expect(reconnectMethods).toContain('bramble.getNeighbors');
    expect(reconnectMethods).toContain('bramble.getRoutes');
    expect(reconnectMethods).toContain('bramble.getAirtime');
    expect(reconnectMethods).toContain('bramble.getMessages');
  });
});
