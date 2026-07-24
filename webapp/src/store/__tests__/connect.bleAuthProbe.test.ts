import { beforeEach, describe, expect, it, vi } from 'vitest';
import { useStore } from '../index';
import { connect } from '../actions';

// A BLE or WiFi connection must FAIL CLOSED when the node requires auth but no
// (or a blank) token was given. verifyBrambleNode only proves the endpoint is
// a Bramble node via the allowlisted getVersion, which an auth-required node
// answers even unauthenticated. Without the auth probe, a blank-token connect
// looked Connected and then rejected every real RPC as Unauthorized.

const rpcMock = vi.fn<(method: string, params?: Record<string, unknown>, timeoutMs?: number) => Promise<any>>();

vi.mock('../../transport', () => {
  class MockBrambleClient {
    subscribe = vi.fn(() => () => {});
    clearSubscriptions = vi.fn();
    disconnect = vi.fn(async () => {});
    rpc = vi.fn((m: string, p?: Record<string, unknown>, t?: number) => rpcMock(m, p, t));
    constructor(_t: unknown) {}
  }
  return {
    createTransport: vi.fn(() => ({
      connect: vi.fn(async () => {}),
      disconnect: vi.fn(async () => {}),
      connected: true,
      onNotification: vi.fn(),
      sendRPC: vi.fn(),
    })),
    BrambleClient: MockBrambleClient,
  };
});

vi.mock('../messageDb', () => ({
  messageDb: {
    open: vi.fn(async () => {}), getMessages: vi.fn(async () => []),
    saveMessages: vi.fn(async () => {}), saveMessage: vi.fn(async () => {}),
    updateMessageStatus: vi.fn(async () => {}),
  },
}));
vi.mock('../deliveryEventStore', () => ({
  deliveryEventStore: {
    open: vi.fn(async () => {}), pruneOldEvents: vi.fn(async () => {}),
    listByMessage: vi.fn(async () => []), listByPacketId: vi.fn(async () => []),
    upsertDeliveryEvents: vi.fn(async () => {}), upsertDeliveryEvent: vi.fn(async () => {}),
  },
}));

// getStatus behavior is per-test; everything else answers benignly.
function baseRpc(getStatus: () => Promise<any>): void {
  rpcMock.mockImplementation(async (method: string) => {
    switch (method) {
      case 'bramble.ping': return { ok: true };
      case 'bramble.getVersion': return { supportsDeliveryEventSync: false };
      case 'bramble.getConfig': return { identity: { address: 0xdeadbeef } };
      case 'bramble.getStatus': return getStatus();
      case 'bramble.getNeighbors': return { neighbors: [] };
      case 'bramble.getRoutes': return { routes: [] };
      case 'bramble.getMessages': return { messages: [] };
      default: return {};
    }
  });
}

beforeEach(() => {
  localStorage.clear();
  sessionStorage.clear();
  vi.clearAllMocks();
  useStore.setState({
    connectionState: 'disconnected', connectionError: undefined,
    transport: null, config: null, devices: [],
  } as any);
});

describe('BLE/WiFi auth probe fails closed', () => {
  it('rejects a BLE connect when the node requires auth (getStatus Unauthorized)', async () => {
    baseRpc(async () => { throw new Error('Unauthorized'); });
    await connect('ble', { token: undefined });
    expect(useStore.getState().connectionState).not.toBe('connected');
    expect(useStore.getState().connectionError ?? '').toMatch(/auth token|authentication required/i);
  });

  it('rejects a WiFi connect the same way on an auth-required node', async () => {
    baseRpc(async () => { throw new Error('-1005 Unauthorized'); });
    await connect('wifi', { url: 'ws://192.0.2.1/ws', ip: '192.0.2.1' });
    expect(useStore.getState().connectionState).not.toBe('connected');
    expect(useStore.getState().connectionError ?? '').toMatch(/auth token|authentication required/i);
  });

  it('connects when getStatus succeeds (correct token or auth-disabled node)', async () => {
    baseRpc(async () => ({ address: 'DEADBEEF', hardware: 'heltec_v4' }));
    await connect('ble', { token: 'good-token' });
    expect(useStore.getState().connectionState).toBe('connected');
  });

  it('does not block the connect on a non-auth getStatus error', async () => {
    baseRpc(async () => { throw new Error('RPC timeout: bramble.getStatus'); });
    await connect('ble', { token: 'good-token' });
    expect(useStore.getState().connectionState).toBe('connected');
  });
});
