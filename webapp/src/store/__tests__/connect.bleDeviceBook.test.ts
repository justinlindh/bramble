import { beforeEach, describe, expect, it, vi } from 'vitest';
import { useStore } from '../index';
import { connect } from '../actions';
import { listDevices, getDeviceToken } from '../../lib/deviceBook';

// A BLE connect with Remember must save the node to the device book with its
// auth token, so a returning user does not retype the token (WiFi parity).

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
      connect: vi.fn(async () => {}), disconnect: vi.fn(async () => {}),
      connected: true, onNotification: vi.fn(), sendRPC: vi.fn(),
    })),
    BrambleClient: MockBrambleClient,
  };
});
vi.mock('../messageDb', () => ({
  messageDb: { open: vi.fn(async () => {}), getMessages: vi.fn(async () => []), saveMessages: vi.fn(async () => {}), saveMessage: vi.fn(async () => {}), updateMessageStatus: vi.fn(async () => {}) },
}));
vi.mock('../deliveryEventStore', () => ({
  deliveryEventStore: { open: vi.fn(async () => {}), pruneOldEvents: vi.fn(async () => {}), listByMessage: vi.fn(async () => []), listByPacketId: vi.fn(async () => []), upsertDeliveryEvents: vi.fn(async () => {}), upsertDeliveryEvent: vi.fn(async () => {}) },
}));

beforeEach(() => {
  localStorage.clear();
  sessionStorage.clear();
  vi.clearAllMocks();
  rpcMock.mockImplementation(async (method: string) => {
    switch (method) {
      case 'bramble.ping': return { ok: true };
      case 'bramble.getVersion': return { supportsDeliveryEventSync: false };
      case 'bramble.getConfig': return { identity: { address: 0xdeadbeef } };
      case 'bramble.getStatus': return { address: 'DEADBEEF' };
      case 'bramble.getNeighbors': return { neighbors: [] };
      case 'bramble.getRoutes': return { routes: [] };
      case 'bramble.getMessages': return { messages: [] };
      default: return {};
    }
  });
  useStore.setState({ connectionState: 'disconnected', connectionError: undefined, transport: null, config: null, devices: [] } as any);
});

describe('BLE device book', () => {
  it('saves the node with a remembered token after a BLE connect', async () => {
    await connect('ble', { token: 'ble-secret', remember: true, name: 'Node A' });
    expect(useStore.getState().connectionState).toBe('connected');
    const saved = listDevices().find(d => d.transport === 'ble');
    expect(saved?.address).toBe('DEADBEEF');
    expect(saved?.name).toBe('Node A');
    expect(getDeviceToken('DEADBEEF')).toBe('ble-secret');
    // remembered -> persisted to localStorage, not just the session
    expect(localStorage.getItem('bramble.deviceToken.DEADBEEF')).toBe('ble-secret');
  });

  it('keeps the BLE token session-only when Remember is off', async () => {
    await connect('ble', { token: 'ble-secret', remember: false });
    const saved = listDevices().find(d => d.transport === 'ble');
    expect(saved?.transport).toBe('ble');
    expect(getDeviceToken('DEADBEEF')).toBe('ble-secret');
    expect(localStorage.getItem('bramble.deviceToken.DEADBEEF')).toBeNull();
  });
});
