import { beforeEach, describe, expect, it, vi } from 'vitest';

const rpcMock = vi.fn<(
  method: string,
  params?: Record<string, unknown>,
  timeoutMs?: number,
) => Promise<any>>();
const transportConnectMock = vi.fn(async () => {});
const transportDisconnectMock = vi.fn(async () => {});

vi.mock('../../transport', () => {
  class MockBrambleClient {
    subscribe = vi.fn(() => () => {});
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

describe('connect serial init readiness gate', () => {
  beforeEach(async () => {
    vi.resetModules();
    vi.clearAllMocks();
    transportConnectMock.mockResolvedValue(undefined);
    transportDisconnectMock.mockResolvedValue(undefined);
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

  it('continues with best-effort init when serial readiness probe fails', async () => {
    rpcMock.mockImplementation(async (method: string) => {
      if (method === 'bramble.ping') throw new Error('timeout waiting for rpc');
      if (method === 'bramble.getConfig') return { identity: { address: 0x1234 } };
      return {};
    });

    const { connect } = await import('../actions');
    const { useStore } = await import('../index');

    await connect('serial');

    expect(rpcMock).toHaveBeenCalledWith('bramble.ping', undefined, expect.any(Number));
    const calledMethods = rpcMock.mock.calls.map(([method]) => method);
    expect(calledMethods).toContain('bramble.getConfig');
    expect(useStore.getState().connectionState).toBe('connected');
  });

  it('runs init after serial readiness succeeds and starts with config load', async () => {
    const { connect } = await import('../actions');

    await connect('serial');

    const calledMethods = rpcMock.mock.calls.map(([method]) => method);
    expect(calledMethods[0]).toBe('bramble.ping');
    expect(calledMethods).toContain('bramble.getConfig');
    expect(calledMethods.indexOf('bramble.getConfig')).toBeLessThan(calledMethods.indexOf('bramble.getStatus'));
  });

  it('reuses last-node address when config address is unavailable', async () => {
    localStorage.setItem('bramble:last-node-addr', 'DEADBEEF');
    rpcMock.mockImplementation(async (method: string) => {
      if (method === 'bramble.getConfig') throw new Error('config timeout');
      if (method === 'bramble.ping') return { ok: true };
      return {};
    });

    const { connect } = await import('../actions');
    const { messageDb } = await import('../messageDb');

    await connect('serial');

    expect(messageDb.open).toHaveBeenCalledWith('DEADBEEF');
  });

  it('verifies a network endpoint with ping before init (issue #91)', async () => {
    const { connect } = await import('../actions');
    const { useStore } = await import('../index');

    await connect('wifi', { url: 'ws://127.0.0.1/ws' });

    // Network transports now verify the endpoint with an allowlisted ping
    // before declaring Connected (issue #91), then proceed to the init RPCs.
    const calledMethods = rpcMock.mock.calls.map(([method]) => method);
    expect(calledMethods[0]).toBe('bramble.ping');
    expect(calledMethods).toContain('bramble.getConfig');
    expect(useStore.getState().connectionState).toBe('connected');
  });

  it('does not report Connected when a network endpoint never answers ping (issue #91)', async () => {
    rpcMock.mockImplementation(async (method: string) => {
      if (method === 'bramble.ping') throw new Error('timeout waiting for rpc');
      if (method === 'bramble.getStatus') throw new Error('timeout waiting for rpc');
      return {};
    });

    const { connect } = await import('../actions');
    const { useStore } = await import('../index');

    await connect('wifi', { url: 'ws://127.0.0.1/ws' });

    const calledMethods = rpcMock.mock.calls.map(([method]) => method);
    expect(calledMethods).not.toContain('bramble.getConfig'); // init never started
    expect(useStore.getState().connectionState).toBe('disconnected');
    expect(useStore.getState().connectionError).toContain('did not respond as a Bramble node');
  });

  it('maps auth transport failures to user-friendly error', async () => {
    transportConnectMock.mockRejectedValueOnce(new Error('WebSocket close 1008 unauthorized'));

    const { connect } = await import('../actions');
    const { useStore } = await import('../index');

    await connect('wifi', { url: 'ws://127.0.0.1/ws' });

    expect(useStore.getState().connectionState).toBe('disconnected');
    expect(useStore.getState().connectionError).toContain('Authentication required');
  });
});
