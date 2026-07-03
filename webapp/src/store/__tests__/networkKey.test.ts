import { beforeEach, describe, expect, it, vi } from 'vitest';

// The network-key actions guard on the module-level `client`, exactly like
// addChannel. There is no separate client module to mock (see
// actions.serial-init.test.ts): the client is created inside connect() from
// a mocked transport, so we drive a real connect() to populate it, then
// exercise the RPC calls the same way the sibling store tests do.
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

describe('network key actions', () => {
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

  it('setNetworkKey posts the hex key and returns ok', async () => {
    const { connect, setNetworkKey } = await import('../actions');
    await connect('serial');

    rpcMock.mockResolvedValueOnce({ ok: true });
    const ok = await setNetworkKey('ab'.repeat(32));
    expect(ok).toBe(true);
    expect(rpcMock).toHaveBeenCalledWith('bramble.setNetworkKey', { key: 'ab'.repeat(32) }, undefined);
  });

  it('getNetworkKeyStatus returns provisioned + fingerprint', async () => {
    const { connect, getNetworkKeyStatus } = await import('../actions');
    await connect('serial');

    rpcMock.mockResolvedValueOnce({ provisioned: true, fingerprint: 'deadbeef' });
    const s = await getNetworkKeyStatus();
    expect(s).toEqual({ provisioned: true, fingerprint: 'deadbeef' });
    expect(rpcMock).toHaveBeenCalledWith('bramble.getNetworkKeyStatus', undefined, undefined);
  });

  it('rejects when not connected', async () => {
    const { setNetworkKey, getNetworkKeyStatus } = await import('../actions');
    await expect(setNetworkKey('ab'.repeat(32))).rejects.toThrow('Not connected');
    await expect(getNetworkKeyStatus()).rejects.toThrow('Not connected');
  });
});
