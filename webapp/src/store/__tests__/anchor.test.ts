import { beforeEach, describe, expect, it, vi } from 'vitest';

// The anchor actions guard on the module-level `client`, exactly like the
// network-key actions (see networkKey.test.ts): the client is created inside
// connect() from a mocked transport, so we drive a real connect() to populate
// it, then exercise the RPC calls the same way the sibling store tests do.
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

describe('anchor actions', () => {
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
      anchorStatus: null,
    } as any);
  });

  it('setAnchor posts the public key only', async () => {
    const { connect, setAnchor } = await import('../actions');
    await connect('serial');

    rpcMock.mockResolvedValueOnce({ ok: true });
    const ok = await setAnchor('cd'.repeat(32));
    expect(ok).toBe(true);
    expect(rpcMock).toHaveBeenCalledWith('bramble.setAnchor', { anchor_pubkey: 'cd'.repeat(32) }, undefined);
  });

  it('getIdentity returns address + pubkey_hash + ed25519_pub', async () => {
    const { connect, getIdentity } = await import('../actions');
    await connect('serial');

    rpcMock.mockResolvedValueOnce({ address: '00001234', pubkey_hash: 'abcd', ed25519_pub: 'cd'.repeat(32) });
    const id = await getIdentity();
    expect(id).toEqual({ address: '00001234', pubkey_hash: 'abcd', ed25519_pub: 'cd'.repeat(32) });
    expect(rpcMock).toHaveBeenCalledWith('bramble.getIdentity', undefined, undefined);
  });

  it('setEndorsement posts not_after + endorsement_sig', async () => {
    const { connect, setEndorsement } = await import('../actions');
    await connect('serial');

    rpcMock.mockResolvedValueOnce({ ok: true });
    const ok = await setEndorsement('ffffffffffffffff', 'ef'.repeat(64));
    expect(ok).toBe(true);
    expect(rpcMock).toHaveBeenCalledWith(
      'bramble.setEndorsement',
      { not_after: 'ffffffffffffffff', endorsement_sig: 'ef'.repeat(64) },
      undefined,
    );
  });

  it('loadAnchorStatus pushes anchor status into the store', async () => {
    const { connect, loadAnchorStatus } = await import('../actions');
    const { useStore } = await import('../index');
    await connect('serial');

    rpcMock.mockResolvedValueOnce({ anchored: false, endorsed: false });
    await loadAnchorStatus();
    expect(useStore.getState().anchorStatus).toEqual({ anchored: false, endorsed: false });
    expect(rpcMock).toHaveBeenCalledWith('bramble.getAnchorStatus', undefined, undefined);
  });

  it('rejects when not connected', async () => {
    const { setAnchor, getIdentity, setEndorsement } = await import('../actions');
    await expect(setAnchor('cd'.repeat(32))).rejects.toThrow('Not connected');
    await expect(getIdentity()).rejects.toThrow('Not connected');
    await expect(setEndorsement('ffffffffffffffff', 'ef'.repeat(64))).rejects.toThrow('Not connected');
  });
});
